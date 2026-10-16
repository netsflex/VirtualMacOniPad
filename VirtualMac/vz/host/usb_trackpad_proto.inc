// Experimental, opt-in: present the framework's USB touch screen to the guest
// as a USB Magic Trackpad 2, and turn the touches the app sends it (see
// VZUSBTrackpadRelay.m) into Magic Trackpad "compact V7" frames so macOS
// drives its native trackpad gestures. Installed only in the one reviewed VMM
// build whose layout is described below; the device exists only when the app
// chose the Magic Trackpad pointing device.
//
// VzCore::Hardware::UsbTouchScreen in VMM UUID BAC451C3-70AB-3259-B77E-91F5673D15E1:
//   vtable address point 0x1001b2cc8; slot n at +8n, signed IA with the slot
//   address blended with a per-slot constant.
//   slot 18 report descriptor  -> {pointer, length}
//   slot 19 SET_REPORT output  -> bool(this, report ID, transfer)
//   slot 20 GET_REPORT feature -> bool(this, report ID, transfer)
//   slot 21 SET_REPORT feature -> bool(this, report ID, transfer)
//   slot 22 multi-touch event   -> void(this, vector<Touch>)
// A transfer holds a vector of {buffer, length} at +0x10, the current buffer
// index at +0x28, the offset in it at +0x30, the bytes moved at +0x34 and the
// setup packet (request, wValue, wIndex, wLength) at +0x38.
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <math.h>
#include <os/lock.h>
#include <ptrauth.h>
#include <uuid/uuid.h>


static const uint8_t usbTrackpadVMMUUID[16] = {
    0xBA, 0xC4, 0x51, 0xC3, 0x70, 0xAB, 0x32, 0x59,
    0xB7, 0x7E, 0x91, 0xF5, 0x67, 0x3D, 0x15, 0xE1};

enum {
    kTouchVTableSlot17 = 0x1001b2d50,
    kTouchInterfaceSlot2 = 0x1001b2da0,  // multi-touch interface vtable, third entry
    kTouchDescriptor = 0x1001c4428,
    kTouchDescriptorLength = 0xbf,
};

typedef struct { uint8_t *data; uint64_t length; } VZUsbBuffer;
typedef struct {
    uint8_t unknown0[0x10];
    VZUsbBuffer *begin, *end;
    uint8_t unknown20[0x8];
    uint64_t index;
    uint32_t offset, total;
    uint16_t request, value, windex, wlength;
} VZUsbTransfer;

typedef struct { const uint8_t *data; size_t length; } VZDescriptorSpan;

// Generic Desktop mouse with report ID 2: three buttons, relative X/Y, then
// vendor bytes sized for the Magic Trackpad 2 USB frame (12-byte header, nine
// bytes per finger, up to 15 fingers).
static const uint8_t usbTrackpadDescriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02,
    0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0,
    0x06, 0x00, 0xff, 0x09, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00,
    0x75, 0x08, 0x95, 0x8f, 0x81, 0x02,
    0xc0,
    // Vendor collection declaring the feature reports the multitouch driver
    // reads; the HID layer sizes each GET_REPORT from these declarations.
    0x06, 0x00, 0xff, 0x09, 0x0e, 0xa1, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08,
    0x85, 0x01, 0x09, 0x01, 0x95, 0x04, 0xb1, 0x02,
    0x85, 0x02, 0x09, 0x02, 0x95, 0x01, 0xb1, 0x02,
    0x85, 0xdb, 0x09, 0xdb, 0x95, 0x49, 0xb1, 0x02,
    0x85, 0xd1, 0x09, 0xd1, 0x95, 0x01, 0xb1, 0x02,
    0x85, 0xd3, 0x09, 0xd3, 0x95, 0x0c, 0xb1, 0x02,
    0x85, 0xd0, 0x09, 0xd0, 0x95, 0x0f, 0xb1, 0x02,
    0x85, 0xa1, 0x09, 0xa1, 0x95, 0x06, 0xb1, 0x02,
    0x85, 0xd9, 0x09, 0xd9, 0x95, 0x10, 0xb1, 0x02,
    0x85, 0x7f, 0x09, 0x7f, 0x95, 0x04, 0xb1, 0x02,
    // Interrupt input report 0x31: the touch frame the guest reads.
    0x85, 0x31, 0x09, 0x31, 0x95, 0xff, 0x81, 0x02,
    0xc0,
};

// Feature reports read from a Mac's built-in trackpad, used as a first guess
// for what the multitouch driver queries during initialization.
static const struct { uint8_t id; uint8_t length; uint8_t bytes[80]; } usbTrackpadFeatures[] = {
    // AppleMultitouchHIDEventDriverV2 reads report 0 as the device's
    // "initialized state" when it starts. Try reporting it initialized.
    {0x00, 2, {0x00, 0x01}},
    {0xd0, 16, {0xd0, 0x02, 0x01, 0x00, 0x10, 0x01, 0x00, 0x18, 0x00, 0x02, 0x10, 0x02, 0x01, 0x0b, 0x02, 0x00}},
    {0xd1, 2, {0xd1, 0x6d}},
    {0xd3, 13, {0xd3, 0x01, 0x12, 0x18, 0x07, 0x95, 0x00, 0x10, 0x18, 0x00, 0x10, 0x00, 0x00}},
    {0xa1, 7, {0xa1, 0x00, 0x00, 0x04, 0x00, 0x00, 0x02}},
    {0x7f, 5, {0x7f, 0x00, 0x00, 0x00, 0x00}},
    {0xd9, 17, {0xd9, 0xc0, 0x30, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0xf8, 0xe8, 0x5d, 0xff, 0x10, 0x19, 0xc3, 0x1c}},
    {0xdb, 74, {0xdb, 0x01, 0x02, 0x00, 0xd1, 0x6d, 0x0d, 0x00, 0xd3, 0x01, 0x12, 0x18, 0x07, 0x95, 0x00, 0x10,
                0x18, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00, 0xd0, 0x02, 0x01, 0x00, 0x10, 0x01, 0x00, 0x18, 0x00,
                0x02, 0x10, 0x02, 0x01, 0x0b, 0x02, 0x00, 0x07, 0x00, 0xa1, 0x00, 0x00, 0x04, 0x00, 0x00, 0x02,
                0x11, 0x00, 0xd9, 0xc0, 0x30, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0xf8, 0xe8, 0x5d, 0xff, 0x10,
                0x19, 0xc3, 0x1c, 0x05, 0x00, 0x7f, 0x00, 0x00, 0x00, 0x00}},
};

static uintptr_t usbTrackpadSlide;
static void *volatile usbTrackpadDevice;

static void usb_trackpad_log_bytes(const char *what, uint8_t id, const VZUsbTransfer *t,
                                   const uint8_t *bytes, size_t length) {
    char hex[3 * 64 + 1] = "";
    for (size_t i = 0; i < length && i < 64; i++)
        snprintf(hex + 3 * i, 4, "%02x ", bytes[i]);
    logf_("[USBTrackpad] %s id=0x%02x request=0x%04x value=0x%04x index=0x%04x length=%u data(%zu)=%s",
          what, id, t->request, t->value, t->windex, t->wlength, length, hex);
}

// Append bytes for the host, as the base class's own GET_PROTOCOL path does.
static size_t usb_trackpad_write(VZUsbTransfer *t, const uint8_t *bytes, size_t length) {
    size_t written = 0;
    while (written < length) {
        size_t count = (size_t)(t->end - t->begin);
        if (t->index >= count) break;
        VZUsbBuffer *buffer = t->begin + t->index;
        if (buffer->length <= t->offset) break;
        size_t room = buffer->length - t->offset, step = length - written;
        if (step > room) step = room;
        memcpy(buffer->data + t->offset, bytes + written, step);
        written += step;
        t->total += (uint32_t)step;
        t->offset += (uint32_t)step;
        if (t->offset == buffer->length) { t->offset = 0; t->index++; }
    }
    return written;
}

// Consume the bytes the host sent with a SET_REPORT.
static size_t usb_trackpad_read(VZUsbTransfer *t, uint8_t *bytes, size_t capacity) {
    size_t read = 0;
    while (read < capacity) {
        size_t count = (size_t)(t->end - t->begin);
        if (t->index >= count) break;
        VZUsbBuffer *buffer = t->begin + t->index;
        if (buffer->length <= t->offset) break;
        size_t available = buffer->length - t->offset, step = capacity - read;
        if (step > available) step = available;
        memcpy(bytes + read, buffer->data + t->offset, step);
        read += step;
        t->total += (uint32_t)step;
        t->offset += (uint32_t)step;
        if (t->offset == buffer->length) { t->offset = 0; t->index++; }
    }
    return read;
}

// Replace the touch screen's product ID wherever its serialized descriptors
// keep it: in the object or in a heap block the object points to.
static void usb_trackpad_patch_product(void *device) {
    static const uint8_t old_id[4] = {0xac, 0x05, 0x08, 0x81};
    static const uint8_t new_id[4] = {0xac, 0x05, 0x65, 0x02};
    unsigned patched = 0;
    uint8_t *object = device;
    size_t objectSize = malloc_size(object);
    if (!objectSize) objectSize = 0x200;
    for (size_t i = 0; i + 4 <= objectSize; i++)
        if (!memcmp(object + i, old_id, 4)) {
            memcpy(object + i, new_id, 4);
            logf_("[USBTrackpad] product ID patched in object at +0x%zx", i);
            patched++;
        }
    for (size_t i = 0; i + 8 <= objectSize; i += 8) {
        uint8_t *block = *(uint8_t **)(object + i);
        size_t size = block ? malloc_size(block) : 0;
        for (size_t j = 0; size && j + 4 <= size; j++)
            if (!memcmp(block + j, old_id, 4)) {
                memcpy(block + j, new_id, 4);
                logf_("[USBTrackpad] product ID patched in block at object+0x%zx offset 0x%zx", i, j);
                patched++;
            }
    }
    logf_("[USBTrackpad] product ID patch finished: %u site(s), object size %zu", patched, objectSize);
}

static VZDescriptorSpan usb_trackpad_descriptor(void *self) {
    if (!__atomic_exchange_n(&usbTrackpadDevice, self, __ATOMIC_ACQ_REL)) {
        logf_("[USBTrackpad] report descriptor requested during setup; device=%p", self);
        // The descriptors are serialized after this call returns; patch them
        // once construction has finished, well before the guest enumerates.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2),
                       dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
            usb_trackpad_patch_product(self);
        });
    } else {
        logf_("[USBTrackpad] report descriptor requested by the guest");
    }
    return (VZDescriptorSpan){usbTrackpadDescriptor, sizeof(usbTrackpadDescriptor)};
}

static bool usb_trackpad_set_output(void *self, uint8_t id, VZUsbTransfer *t) {
    (void)self;
    uint8_t bytes[256];
    size_t length = usb_trackpad_read(t, bytes, t->wlength < sizeof(bytes) ? t->wlength : sizeof(bytes));
    usb_trackpad_log_bytes("SET_REPORT output", id, t, bytes, length);
    return true;
}

// The multitouch driver reads a device property in three steps: it writes
// feature report 1 = {1, property}, reads feature report 1 for the length of
// that property's report, then reads the property's own feature report.
// A Mac's built-in trackpad answers report 1 with {1, ff ff ff ff} when no
// property is selected. The length counts the report ID byte.
static volatile int usbTrackpadSelectedProperty = -1;
static volatile int gUsbTrackpadEnabled;

static int usb_trackpad_feature_index(uint8_t id) {
    for (size_t i = 0; i < sizeof(usbTrackpadFeatures) / sizeof(*usbTrackpadFeatures); i++)
        if (usbTrackpadFeatures[i].id == id) return (int)i;
    return -1;
}

static bool usb_trackpad_get_feature(void *self, uint8_t id, VZUsbTransfer *t) {
    (void)self;
    if (id == 0x01) {
        int selected = usbTrackpadSelectedProperty;
        int index = selected >= 0 ? usb_trackpad_feature_index((uint8_t)selected) : -1;
        // AppleMultitouchHIDEventDriverV2::getMultitouchReportInfo expects
        // {1, echoed report ID, report type (0), length (16-bit LE)} and
        // ignores the reply when the ID is not echoed.
        uint8_t reply[5] = {0x01, 0xff, 0xff, 0xff, 0xff};
        if (index >= 0) {
            uint16_t length = usbTrackpadFeatures[index].length;
            reply[1] = (uint8_t)selected;
            reply[2] = 0x00;
            reply[3] = length & 0xff;
            reply[4] = length >> 8;
        }
        size_t count = t->wlength < sizeof(reply) ? t->wlength : sizeof(reply);
        usb_trackpad_write(t, reply, count);
        usb_trackpad_log_bytes("GET_REPORT feature (property length)", id, t, reply, count);
        return true;
    }
    for (size_t i = 0; i < sizeof(usbTrackpadFeatures) / sizeof(*usbTrackpadFeatures); i++)
        if (usbTrackpadFeatures[i].id == id) {
            size_t length = usbTrackpadFeatures[i].length;
            if (length > t->wlength) length = t->wlength;
            usb_trackpad_write(t, usbTrackpadFeatures[i].bytes, length);
            usb_trackpad_log_bytes("GET_REPORT feature (answered)", id, t, usbTrackpadFeatures[i].bytes, length);
            return true;
        }
    // Unknown so far: answer zeros so the driver's initialization goes on
    // and shows which report it asks for next.
    static const uint8_t zeros[256];
    uint8_t reply[256];
    size_t length = t->wlength < sizeof(reply) ? t->wlength : sizeof(reply);
    memcpy(reply, zeros, length);
    if (length && id) reply[0] = id;
    usb_trackpad_write(t, reply, length);
    usb_trackpad_log_bytes("GET_REPORT feature (unknown, answered with zeros)", id, t, reply, length);
    return true;
}

static bool usb_trackpad_set_feature(void *self, uint8_t id, VZUsbTransfer *t) {
    (void)self;
    uint8_t bytes[256];
    size_t length = usb_trackpad_read(t, bytes, t->wlength < sizeof(bytes) ? t->wlength : sizeof(bytes));
    usb_trackpad_log_bytes("SET_REPORT feature", id, t, bytes, length);
    if (id == 0x01 && length >= 2) {
        usbTrackpadSelectedProperty = bytes[1];
        if (usb_trackpad_feature_index(bytes[1]) < 0)
            logf_("[USBTrackpad] property 0x%02x selected but not known", bytes[1]);
    }
    // The multitouch driver writes report 2 = {02, 01} to turn the device on.
    if (id == 0x02 && length >= 2) {
        gUsbTrackpadEnabled = bytes[1];
        logf_("[USBTrackpad] multitouch mode %s", bytes[1] ? "enabled" : "disabled");
    }
    return true;
}

// ---- Input frames (report 0x31, Magic Trackpad 2 "compact V7" format) ----
// Each contact is nine bytes; the frame is a four-byte header plus one contact
// per touching finger. Positions are the device's signed 13-bit units.
static const char usbTrackpadSelfTestFlag[] =
    "/var/mobile/Media/VirtualMac/Diagnostics/usb-trackpad-selftest";

typedef struct { int32_t xraw, yraw; uint8_t phase, finger, type; } VZTrackpadContact;

static void usb_trackpad_pack_contact(uint8_t *b, const VZTrackpadContact *c) {
    uint32_t x = (uint32_t)c->xraw & 0x1fff, y = (uint32_t)c->yraw & 0x1fff;
    b[0] = x & 0xff;
    b[1] = ((x >> 8) & 0x1f) | ((y & 0x7) << 5);
    b[2] = (y >> 3) & 0xff;
    // Bits 5-7 carry the MT path stage (4 = touching), bits 2-4 the finger.
    b[3] = ((y >> 11) & 0x3) | ((c->type & 0x7) << 2) | ((c->phase & 0x7) << 5);
    b[4] = 0x64;  // touch major (encoded), fixed for now
    b[5] = 0x64;  // touch minor
    b[6] = 0x20;  // size
    b[7] = 0x50;  // pressure/density
    b[8] = (c->finger & 0x0f) | (4 << 5);  // contact id, orientation 4 = upright
}

static size_t usb_trackpad_build_frame(uint8_t *frame, const VZTrackpadContact *contacts,
                                       size_t count, uint32_t timestamp) {
    frame[0] = 0x31;
    frame[1] = (uint8_t)((timestamp & 0x1f) << 3);
    frame[2] = (timestamp >> 5) & 0xff;
    frame[3] = (timestamp >> 13) & 0xff;
    for (size_t i = 0; i < count; i++)
        usb_trackpad_pack_contact(frame + 4 + i * 9, &contacts[i]);
    return 4 + count * 9;
}

// Completing a pending interrupt IN transfer, as UsbTouchScreen's own event
// path does: take the oldest pending transfer (a shared_ptr), fill it, then
// call the USB bus interface's completion (vtable +0x30) and drop the ref.
enum { kTakePendingTransfer = 0x1000d42d0 };
typedef struct { VZUsbTransfer *transfer; void *control; } VZPendingTransfer;
extern void vz_shared_release_weak(void *control)
    __asm__("__ZNSt3__119__shared_weak_count14__release_weakEv");

static void usb_trackpad_release(void *control) {
    if (!control) return;
    if (__atomic_fetch_sub((long *)((uint8_t *)control + 8), 1, __ATOMIC_ACQ_REL) != 0) return;
    void *vtable = ptrauth_auth_data(*(void **)control, ptrauth_key_asda,
                                     ptrauth_blend_discriminator(control, 0x634F));
    void **slot = (void **)vtable + 2;
    void (*zero)(void *) = ptrauth_auth_and_resign(*slot, ptrauth_key_asia,
        ptrauth_blend_discriminator(slot, 0x4444), ptrauth_key_asia, 0);
    zero(control);
    vz_shared_release_weak(control);
}

static bool usb_trackpad_complete_pending(void *device, const uint8_t *frame, size_t length) {
    void (*take)(VZPendingTransfer *, void *) = ptrauth_sign_unauthenticated(
        (void *)(usbTrackpadSlide + kTakePendingTransfer), ptrauth_key_asia, 0);
    VZPendingTransfer pending = {0};
    take(&pending, device);
    if (!pending.transfer) return false;
    usb_trackpad_write(pending.transfer, frame, length);
    void *bus = *(void **)((uint8_t *)device + 0x10);
    void *vtable = ptrauth_auth_data(*(void **)bus, ptrauth_key_asda,
                                     ptrauth_blend_discriminator(bus, 0xF806));
    void **slot = (void **)((uint8_t *)vtable + 0x30);
    void (*complete)(void *, void *, void *, int) = ptrauth_auth_and_resign(*slot,
        ptrauth_key_asia, ptrauth_blend_discriminator(slot, 0xDA26), ptrauth_key_asia, 0);
    complete(bus, device, pending.transfer, 0);
    usb_trackpad_release(pending.control);
    return true;
}

// Self-test: while multitouch mode is on, deliver a one-finger circle every
// 8 ms through the pending interrupt transfer.
static uint32_t gUsbTrackpadTimestamp;

static size_t usb_trackpad_selftest_frame(uint8_t *frame) {
    VZTrackpadContact contact;
    double angle = (double)gUsbTrackpadTimestamp * 0.00625;
    contact.xraw = (int32_t)(1800.0 * cos(angle));
    contact.yraw = (int32_t)(-580.0 + 1400.0 * sin(angle));
    contact.phase = 4;   // touching
    contact.finger = 1;
    contact.type = 2;
    // The header timestamp counts milliseconds.
    uint32_t timestamp = gUsbTrackpadTimestamp;
    gUsbTrackpadTimestamp += 8;
    return usb_trackpad_build_frame(frame, &contact, 1, timestamp);
}

static void usb_trackpad_start_selftest(void) {
    static dispatch_source_t timer;
    if (timer || access(usbTrackpadSelfTestFlag, F_OK)) return;
    timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
        dispatch_queue_create("usb-trackpad-selftest", DISPATCH_QUEUE_SERIAL));
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                              8 * NSEC_PER_MSEC, NSEC_PER_MSEC);
    dispatch_source_set_event_handler(timer, ^{
        void *device = usbTrackpadDevice;
        if (!device || !gUsbTrackpadEnabled) return;
        uint8_t frame[4 + 16 * 9];
        size_t length = usb_trackpad_selftest_frame(frame);
        static uint64_t sent, missed;
        if (usb_trackpad_complete_pending(device, frame, length)) sent++; else missed++;
        if (((sent + missed) % 500) == 1)
            logf_("[USBTrackpad] selftest frames sent=%llu no-pending=%llu",
                  (unsigned long long)sent, (unsigned long long)missed);
    });
    dispatch_resume(timer);
    logf_("[USBTrackpad] selftest timer started");
}

// ---- Touch events from the app (UsbTouchScreen slot 22) ----
// The app sends _VZMultiTouchEvent through the framework; the VMM hands the
// touches to slot 22 as a std::vector of VzCore MultiTouchEvent::Touch
// (40 bytes). Touches with phase below 3 are down. Index 63 is not a finger: the app uses it to carry the
// trackpad's physical click.
typedef struct {
    uint8_t index; uint8_t pad[3]; int32_t phase;
    double x, y;
    uint32_t swipeAim; uint32_t pad2;
    double timestamp;
} VZTouchRecord;
_Static_assert(sizeof(VZTouchRecord) == 40, "VzCore touch record is 40 bytes");
typedef struct { VZTouchRecord *begin, *end, *capacity; } VZTouchVector;
enum { kClickTouchIndex = 63, kFrameQueueLength = 32, kMaxFrameLength = 4 + 15 * 9 };

// A queued frame. Motion frames (same fingers and button as the previous
// frame, all touching) replace a motion frame still waiting in the queue.
typedef struct { uint8_t bytes[kMaxFrameLength]; uint8_t length; bool motion; uint64_t key; } VZTrackpadFrame;
static VZTrackpadFrame usbTrackpadQueue[kFrameQueueLength];
static unsigned usbTrackpadQueueHead, usbTrackpadQueueCount;
static os_unfair_lock usbTrackpadQueueLock = OS_UNFAIR_LOCK_INIT;
static uint64_t usbTrackpadDownMask;  // contact slots down in the last frame
static VZTrackpadContact usbTrackpadLastContacts[15];
static uint8_t usbTrackpadFingerType[15];
static bool usbTrackpadEdgeEntry[15];

static void usb_trackpad_enqueue(const uint8_t *frame, size_t length, bool motion, uint64_t key) {
    os_unfair_lock_lock(&usbTrackpadQueueLock);
    if (motion && usbTrackpadQueueCount) {
        VZTrackpadFrame *tail = &usbTrackpadQueue[(usbTrackpadQueueHead + usbTrackpadQueueCount - 1) % kFrameQueueLength];
        if (tail->motion && tail->key == key) {
            memcpy(tail->bytes, frame, length);
            tail->length = (uint8_t)length;
            os_unfair_lock_unlock(&usbTrackpadQueueLock);
            return;
        }
    }
    if (usbTrackpadQueueCount == kFrameQueueLength) {  // drop the oldest
        usbTrackpadQueueHead = (usbTrackpadQueueHead + 1) % kFrameQueueLength;
        usbTrackpadQueueCount--;
    }
    VZTrackpadFrame *slot = &usbTrackpadQueue[(usbTrackpadQueueHead + usbTrackpadQueueCount) % kFrameQueueLength];
    memcpy(slot->bytes, frame, length);
    slot->length = (uint8_t)length;
    slot->motion = motion;
    slot->key = key;
    usbTrackpadQueueCount++;
    os_unfair_lock_unlock(&usbTrackpadQueueLock);
}

static uint32_t usb_trackpad_milliseconds(void) {
    static uint64_t start;
    static mach_timebase_info_data_t timebase;
    uint64_t now = mach_absolute_time();
    if (!start) { mach_timebase_info(&timebase); start = now; }
    return (uint32_t)((now - start) * timebase.numer / timebase.denom / 1000000);
}

// The guest derives finger velocity from the header timestamp, so it must be
// the real time of the report, and a real trackpad reports at most every
// 8 ms: frames delivered back to back (queued while the guest was busy)
// would otherwise show a burst of motion in 1 ms and fling a scroll. Deliver
// at most one frame per interval; motion arriving meanwhile replaces the
// queued frame.
enum { kReportIntervalMs = 8 };
static uint32_t usbTrackpadLastDelivery;  // accessed atomically

static void usb_trackpad_stamp(uint8_t *frame, uint32_t now) {
    frame[1] = (uint8_t)((frame[1] & 0x07) | ((now & 0x1f) << 3));
    frame[2] = (now >> 5) & 0xff;
    frame[3] = (now >> 13) & 0xff;
}

// Claim the next report slot. On failure *wait holds the milliseconds left.
static bool usb_trackpad_claim_slot(uint32_t *stamp, uint32_t *previous, uint32_t *wait) {
    uint32_t now = usb_trackpad_milliseconds();
    uint32_t last = __atomic_load_n(&usbTrackpadLastDelivery, __ATOMIC_ACQUIRE);
    if (last && now - last < kReportIntervalMs) {
        *wait = kReportIntervalMs - (now - last);
        return false;
    }
    if (now <= last) now = last + 1;
    if (!__atomic_compare_exchange_n(&usbTrackpadLastDelivery, &last, now, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        *wait = 1;
        return false;
    }
    *stamp = now;
    *previous = last;
    return true;
}

// Give a claimed slot back when its frame could not be delivered.
static void usb_trackpad_release_slot(uint32_t stamp, uint32_t previous) {
    __atomic_compare_exchange_n(&usbTrackpadLastDelivery, &stamp, previous, false,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static bool usb_trackpad_queue_empty(void) {
    os_unfair_lock_lock(&usbTrackpadQueueLock);
    bool empty = usbTrackpadQueueCount == 0;
    os_unfair_lock_unlock(&usbTrackpadQueueLock);
    return empty;
}

static bool usb_trackpad_dequeue(VZTrackpadFrame *frame) {
    os_unfair_lock_lock(&usbTrackpadQueueLock);
    bool found = usbTrackpadQueueCount > 0;
    if (found) {
        *frame = usbTrackpadQueue[usbTrackpadQueueHead];
        usbTrackpadQueueHead = (usbTrackpadQueueHead + 1) % kFrameQueueLength;
        usbTrackpadQueueCount--;
    }
    os_unfair_lock_unlock(&usbTrackpadQueueLock);
    return found;
}

static void usb_trackpad_put_back(const VZTrackpadFrame *frame) {
    os_unfair_lock_lock(&usbTrackpadQueueLock);
    usbTrackpadQueueHead = (usbTrackpadQueueHead + kFrameQueueLength - 1) % kFrameQueueLength;
    usbTrackpadQueue[usbTrackpadQueueHead] = *frame;
    usbTrackpadQueueCount++;
    os_unfair_lock_unlock(&usbTrackpadQueueLock);
}

static void usb_trackpad_flush(void *device);

static void usb_trackpad_schedule_flush(void *device, uint32_t wait) {
    static int scheduled;
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ queue = dispatch_queue_create("usb-trackpad-pacing", DISPATCH_QUEUE_SERIAL); });
    if (__atomic_exchange_n(&scheduled, 1, __ATOMIC_ACQ_REL)) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)wait * NSEC_PER_MSEC), queue, ^{
        __atomic_store_n(&scheduled, 0, __ATOMIC_RELEASE);
        usb_trackpad_flush(device);
    });
}

// Deliver queued frames, one per report interval, while the guest has a
// transfer pending; without one, slot 17 delivers on the guest's next poll.
static void usb_trackpad_flush(void *device) {
    VZTrackpadFrame frame;
    uint32_t stamp, previous, wait;
    while (!usb_trackpad_queue_empty()) {
        if (!usb_trackpad_claim_slot(&stamp, &previous, &wait)) {
            usb_trackpad_schedule_flush(device, wait);
            return;
        }
        if (!usb_trackpad_dequeue(&frame)) {
            usb_trackpad_release_slot(stamp, previous);
            return;
        }
        usb_trackpad_stamp(frame.bytes, stamp);
        if (!usb_trackpad_complete_pending(device, frame.bytes, frame.length)) {
            usb_trackpad_put_back(&frame);
            usb_trackpad_release_slot(stamp, previous);
            return;
        }
    }
}

// Normalized iPad position (origin top left) to the device's units, from the
// mapping measured with MultitouchSupport in the guest: x' = (2x + 5978) / 12480
// and y' = (2y + 5246) / 7680 with y' growing upward.
static const double kEdgeOverscan = 0.04;

static int32_t usb_trackpad_clamp(double value) {
    return value > 4095 ? 4095 : value < -4096 ? -4096 : (int32_t)lround(value);
}

static void usb_trackpad_touch_events(void *self, const VZTouchVector *touches) {
    __atomic_store_n(&usbTrackpadDevice, self, __ATOMIC_RELEASE);
    VZTrackpadContact contacts[15];
    size_t count = 0;
    uint64_t downMask = 0;
    bool click = false;
    for (const VZTouchRecord *t = touches->begin; t && t != touches->end; t++) {
        bool down = t->phase >= 0 && t->phase < 3;
        if (t->index == kClickTouchIndex) { click = down; continue; }
        if (!down || count == 15) continue;
        unsigned slot = t->index % 15;
        if (downMask & (1ULL << slot)) continue;
        downMask |= 1ULL << slot;
        // Bits 2-4 name the finger: 1 thumb, 2 index, 3 middle, 4 ring,
        // 5 little, 7 a contact entering from an edge. A real trackpad marks
        // a finger that lands at the border as 7 until it moves inward, and
        // macOS's edge swipes (Notification Center) depend on it. macOS also
        // leaves a resting thumb out of multi-finger clicks, so hand out
        // index to little first. A contact keeps its finger once assigned.
        double ex = fmin(t->x, 1 - t->x), ey = fmin(t->y, 1 - t->y);
        bool landed = !(usbTrackpadDownMask & (1ULL << slot));
        if (landed) usbTrackpadEdgeEntry[slot] = fmin(ex, ey) < 0.03;
        else if (usbTrackpadEdgeEntry[slot] && fmin(ex, ey) >= 0.05) usbTrackpadEdgeEntry[slot] = false;
        if (usbTrackpadEdgeEntry[slot]) {
            usbTrackpadFingerType[slot] = 7;
        } else if (landed || usbTrackpadFingerType[slot] == 7) {
            static const uint8_t order[] = {2, 3, 4, 5, 1};
            uint8_t used = 0;
            for (unsigned other = 0; other < 15; other++)
                if (((usbTrackpadDownMask | downMask) & (1ULL << other)) && other != slot)
                    used |= 1 << usbTrackpadFingerType[other];
            uint8_t type = order[4];
            for (unsigned i = 0; i < sizeof(order); i++)
                if (!(used & (1 << order[i]))) { type = order[i]; break; }
            usbTrackpadFingerType[slot] = type;
        }
        VZTrackpadContact *c = &contacts[count++];
        // A real trackpad senses a finger sliding in from an edge just
        // outside its surface, and macOS's edge swipes (Notification Center)
        // look for that. Overscan so the iPad's outer strips land there.
        double x = fmin(1, fmax(0, t->x)) * (1 + 2 * kEdgeOverscan) - kEdgeOverscan;
        double y = fmin(1, fmax(0, t->y)) * (1 + 2 * kEdgeOverscan) - kEdgeOverscan;
        c->xraw = usb_trackpad_clamp((x * 12480 - 5978) / 2);
        c->yraw = usb_trackpad_clamp(((1 - y) * 7680 - 5246) / 2);
        c->phase = landed ? 3 : 4;  // make touch, then touching
        c->finger = (uint8_t)(slot + 1);
        c->type = usbTrackpadFingerType[slot];
        usbTrackpadLastContacts[slot] = *c;
    }
    // Fingers that left since the last frame are sent once as "break touch".
    for (unsigned slot = 0; slot < 15 && count < 15; slot++) {
        if ((usbTrackpadDownMask & (1ULL << slot)) && !(downMask & (1ULL << slot))) {
            contacts[count] = usbTrackpadLastContacts[slot];
            contacts[count++].phase = 5;
        }
    }
    uint64_t previousMask = usbTrackpadDownMask;
    usbTrackpadDownMask = downMask;
    static bool lastClick;
    bool previousClick = lastClick;
    if (!count && !previousMask && click == lastClick) return;
    lastClick = click;
    uint8_t frame[kMaxFrameLength];
    size_t length = usb_trackpad_build_frame(frame, contacts, count, 0);
    if (click) frame[1] |= 1;
    bool motion = downMask && downMask == previousMask && click == previousClick;
    usb_trackpad_enqueue(frame, length, motion, downMask | ((uint64_t)click << 63));
    if (!downMask && count) {  // end the touch sequence with an empty frame
        length = usb_trackpad_build_frame(frame, NULL, 0, 0);
        if (click) frame[1] |= 1;
        usb_trackpad_enqueue(frame, length, false, 0);
    }
    static uint64_t events;
    if ((events++ % 200) == 0)
        logf_("[USBTrackpad] touch event #%llu contacts=%zu click=%d first phase=%d x=%.3f y=%.3f",
              (unsigned long long)events, count, click,
              touches->begin != touches->end ? touches->begin->phase : -1,
              touches->begin != touches->end ? touches->begin->x : 0,
              touches->begin != touches->end ? touches->begin->y : 0);
    if (gUsbTrackpadEnabled) usb_trackpad_flush(self);
}

// The multi-touch interface sits 0xc8 bytes into UsbTouchScreen.
static void usb_trackpad_touch_events_interface(void *interface, const VZTouchVector *touches) {
    usb_trackpad_touch_events((uint8_t *)interface - 0xc8, touches);
}

// Slot 17 is called when the guest polls the interrupt IN endpoint. Return a
// queued frame, or false to leave the transfer pending until one arrives.
static bool usb_trackpad_fetch_report(void *self, VZUsbTransfer *t) {
    (void)self;
    static uint64_t calls;
    if ((calls++ % 2000) == 0)
        logf_("[USBTrackpad] fetch_report call #%llu enabled=%d", (unsigned long long)calls, gUsbTrackpadEnabled);
    VZTrackpadFrame frame;
    uint32_t stamp, previous, wait;
    if (!gUsbTrackpadEnabled || usb_trackpad_queue_empty()) return false;
    if (!usb_trackpad_claim_slot(&stamp, &previous, &wait)) {
        usb_trackpad_schedule_flush(self, wait);  // completes this transfer
        return false;
    }
    if (!usb_trackpad_dequeue(&frame)) {
        usb_trackpad_release_slot(stamp, previous);
        return false;
    }
    usb_trackpad_stamp(frame.bytes, stamp);
    usb_trackpad_write(t, frame.bytes, frame.length);
    return true;
}

// Slot 14 handles every control request. It answers HID class requests and
// passes the rest to the generic USB device code, which traps on vendor
// requests. Answer vendor requests here and pass everything else through.
// It returns success as 1 << 32 with no error code in the low half.
typedef uint64_t (*VZControlHandler)(void *self, VZUsbTransfer **transfer);
static VZControlHandler usbTrackpadOriginalControl;

static uint64_t usb_trackpad_control(void *self, VZUsbTransfer **holder) {
    VZUsbTransfer *t = *holder;
    uint8_t requestType = t->request & 0xff;
    if ((requestType & 0x60) != 0x40)
        return usbTrackpadOriginalControl(self, holder);
    if (requestType & 0x80) {
        static const uint8_t zeros[256];
        size_t length = t->wlength < sizeof(zeros) ? t->wlength : sizeof(zeros);
        usb_trackpad_write(t, zeros, length);
        usb_trackpad_log_bytes("vendor IN (answered with zeros)", t->request >> 8, t, zeros, length);
    } else {
        uint8_t bytes[256];
        size_t length = usb_trackpad_read(t, bytes, t->wlength < sizeof(bytes) ? t->wlength : sizeof(bytes));
        usb_trackpad_log_bytes("vendor OUT", t->request >> 8, t, bytes, length);
    }
    return 1ULL << 32;
}

// Find the VMM executable among the loaded images and check its UUID.
static bool usb_trackpad_layout_matches(void) {
    static const char suffix[] = "/MacOS/com.apple.Virtualization.VirtualMachine";
    for (uint32_t image = 0; image < _dyld_image_count(); image++) {
        const char *name = _dyld_get_image_name(image);
        size_t length = name ? strlen(name) : 0;
        if (length < sizeof(suffix) - 1 || strcmp(name + length - (sizeof(suffix) - 1), suffix))
            continue;
        const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(image);
        const struct load_command *command = (const struct load_command *)(header + 1);
        for (uint32_t i = 0; i < header->ncmds; i++) {
            if (command->cmd == LC_UUID) {
                const uint8_t *uuid = ((const struct uuid_command *)command)->uuid;
                uuid_string_t text;
                uuid_unparse_upper(uuid, text);
                logf_("[USBTrackpad] VMM image %u %s UUID %s", image, name, text);
                if (memcmp(uuid, usbTrackpadVMMUUID, 16)) return false;
                usbTrackpadSlide = (uintptr_t)_dyld_get_image_vmaddr_slide(image);
                return true;
            }
            command = (const struct load_command *)((const uint8_t *)command + command->cmdsize);
        }
    }
    logf_("[USBTrackpad] VMM image not found among %u images", _dyld_image_count());
    return false;
}

static void usb_trackpad_install_slot(unsigned slot, uint16_t diversifier, void *function) {
    void **entry = (void **)(usbTrackpadSlide + kTouchVTableSlot17 + ((intptr_t)slot - 17) * 8);
    void *plain = ptrauth_strip(function, ptrauth_key_asia);
    *entry = ptrauth_sign_unauthenticated(plain, ptrauth_key_asia,
                                          ptrauth_blend_discriminator(entry, diversifier));
}

static void usb_trackpad_proto_install(void) {
    if (!usb_trackpad_layout_matches()) {
        logf_("[USBTrackpad] not installed: unexpected VMM build");
        return;
    }
    const uint8_t *descriptor = (const uint8_t *)(usbTrackpadSlide + kTouchDescriptor);
    if (descriptor[0] != 0x05 || descriptor[1] != 0x0d || descriptor[kTouchDescriptorLength - 1] != 0xc0) {
        logf_("[USBTrackpad] not installed: touch screen descriptor not found");
        return;
    }
    vm_address_t page = trunc_page(usbTrackpadSlide + kTouchVTableSlot17);
    vm_address_t first = usbTrackpadSlide + kTouchVTableSlot17 - 3 * 8;
    page = trunc_page(first);
    vm_size_t size = round_page(usbTrackpadSlide + kTouchInterfaceSlot2 + 8) - page;
    kern_return_t kr = vm_protect(mach_task_self(), page, size, false,
                                  VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) {
        logf_("[USBTrackpad] not installed: vm_protect=%d", kr);
        return;
    }
    void **controlEntry = (void **)(usbTrackpadSlide + kTouchVTableSlot17 - 3 * 8);
    usbTrackpadOriginalControl = (VZControlHandler)ptrauth_auth_and_resign(
        *controlEntry, ptrauth_key_asia, ptrauth_blend_discriminator(controlEntry, 0x4E2B),
        ptrauth_key_asia, 0);
    usb_trackpad_install_slot(14, 0x4E2B, (void *)usb_trackpad_control);
    usb_trackpad_install_slot(17, 0xCB8F, (void *)usb_trackpad_fetch_report);
    usb_trackpad_install_slot(18, 0xEF70, (void *)usb_trackpad_descriptor);
    usb_trackpad_install_slot(19, 0x630C, (void *)usb_trackpad_set_output);
    usb_trackpad_install_slot(20, 0x998B, (void *)usb_trackpad_get_feature);
    usb_trackpad_install_slot(21, 0xD7DA, (void *)usb_trackpad_set_feature);
    usb_trackpad_install_slot(22, 0x8B9A, (void *)usb_trackpad_touch_events);
    void **interfaceEntry = (void **)(usbTrackpadSlide + kTouchInterfaceSlot2);
    *interfaceEntry = ptrauth_sign_unauthenticated(
        ptrauth_strip((void *)usb_trackpad_touch_events_interface, ptrauth_key_asia),
        ptrauth_key_asia, ptrauth_blend_discriminator(interfaceEntry, 0x14DB));
    vm_protect(mach_task_self(), page, size, false, VM_PROT_READ);
    usb_trackpad_start_selftest();
    logf_("[USBTrackpad] installed on UsbTouchScreen slots 14, 17-22 and the touch interface (slide=0x%lx)", (unsigned long)usbTrackpadSlide);
}
