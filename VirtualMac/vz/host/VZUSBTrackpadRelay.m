// Experimental "Magic Trackpad" pointing device: relay the iPad trackpad's
// fingers to the guest's emulated USB Magic Trackpad.
//
// iPadOS delivers trackpad pointer events with a digitizer child that holds
// one child per finger. A HID monitor client reads those fingers and sends
// them to the framework's USB touch screen as _VZMultiTouchEvent; the VMM hook
// (usb_trackpad_proto.inc) turns them into Magic Trackpad frames. The
// trackpad's click travels as a touch with index 63. The touch screen and
// mice keep using the Mac trackpad device; the monitor also notes when a
// mouse moves so the app can tell which device its UIKit events came from.
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <objc/message.h>
#include <stdio.h>

typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef void (*IOHIDEventSystemClientEventCallback)(void *, void *,
    IOHIDServiceClientRef, IOHIDEventRef);
extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(
    CFAllocatorRef, int, CFDictionaryRef);
extern void IOHIDEventSystemClientRegisterEventCallback(
    IOHIDEventSystemClientRef, IOHIDEventSystemClientEventCallback, void *, void *);
extern void IOHIDEventSystemClientUnregisterEventCallback(
    IOHIDEventSystemClientRef, IOHIDEventSystemClientEventCallback, void *, void *);
extern void IOHIDEventSystemClientScheduleWithRunLoop(
    IOHIDEventSystemClientRef, CFRunLoopRef, CFStringRef);
extern void IOHIDEventSystemClientUnscheduleWithRunLoop(
    IOHIDEventSystemClientRef, CFRunLoopRef, CFStringRef);
extern uint32_t IOHIDEventGetType(IOHIDEventRef);
extern uint64_t IOHIDEventGetSenderID(IOHIDEventRef);
extern CFArrayRef IOHIDEventGetChildren(IOHIDEventRef);
extern CFIndex IOHIDEventGetIntegerValue(IOHIDEventRef, uint32_t);
extern double IOHIDEventGetFloatValue(IOHIDEventRef, uint32_t);

enum { kMonitorClient = 1, kScrollEvent = 6, kDigitizerEvent = 11, kPointerEvent = 17 };
#define FIELD(type, index) (((uint32_t)(type) << 16) | (index))
enum { kButtonMask = 3, kIdentity = 6, kRange = 8, kTouch = 9 };
enum { kClickTouchIndex = 63, kMovedPhase = 1 };

static const char kRelayLog[] =
    "/var/mobile/Media/VirtualMac/Diagnostics/usb-trackpad-relay.log";

static IOHIDEventSystemClientRef gClient;
static id gTouchDevice;
static uint64_t gTrackpadSender;
static uint64_t gEventCount;
static CFTimeInterval gLastTrackpadTime, gLastMouseTime;
static void (*gMouseMoved)(double dx, double dy);
static void (*gMouseButtons)(long buttons);
static long gMouseButtonState;

static void relayLog(const char *format, ...) {
    FILE *log = fopen(kRelayLog, "a");
    if (!log) return;
    va_list arguments;
    va_start(arguments, format);
    vfprintf(log, format, arguments);
    va_end(arguments);
    fputc('\n', log);
    fclose(log);
}

static IOHIDEventRef digitizerChild(IOHIDEventRef event) {
    CFArrayRef children = IOHIDEventGetChildren(event);
    for (CFIndex i = 0; children && i < CFArrayGetCount(children); i++) {
        IOHIDEventRef child = (IOHIDEventRef)CFArrayGetValueAtIndex(children, i);
        if (IOHIDEventGetType(child) == kDigitizerEvent) return child;
    }
    return NULL;
}

static id makeTouch(uint8_t index, double x, double y) {
    return ((id(*)(id, SEL, uint8_t, NSInteger, CGPoint, NSInteger, double))objc_msgSend)(
        ((id(*)(id, SEL))objc_msgSend)(objc_getClass("_VZTouch"), sel_registerName("alloc")),
        sel_registerName("initWithIndex:phase:location:swipeAim:timestamp:"),
        index, (NSInteger)kMovedPhase, CGPointMake(x, y), (NSInteger)0, CACurrentMediaTime());
}

// The fingers last seen on the trackpad, re-sent when only the button changes.
typedef struct { uint8_t index; double x, y; } RelayFinger;
static RelayFinger gFingers[16];
static unsigned gFingerCount;
static CFIndex gButtons;
static uint64_t gFingerGeneration;
static uint64_t gDownMask;  // finger identities held down by the last event
static double gLastX[64], gLastY[64];

static void sendTouches(void) {
    NSMutableSet *touches = [NSMutableSet set];
    for (unsigned i = 0; i < gFingerCount; i++) {
        id touch = makeTouch(gFingers[i].index, gFingers[i].x, gFingers[i].y);
        if (touch) { [touches addObject:touch]; [touch release]; }
    }
    // iPadOS reports a two-finger click as the secondary button. The Magic
    // Trackpad has one switch; macOS picks the click type from the fingers.
    if (gButtons & 7) {
        id touch = makeTouch(kClickTouchIndex, 0, 0);
        if (touch) { [touches addObject:touch]; [touch release]; }
    }
    id multiTouch = ((id(*)(id, SEL, id))objc_msgSend)(
        ((id(*)(id, SEL))objc_msgSend)(objc_getClass("_VZMultiTouchEvent"), sel_registerName("alloc")),
        sel_registerName("initWithTouches:"), touches);
    if (!multiTouch) return;
    ((void(*)(id, SEL, id))objc_msgSend)(gTouchDevice,
        sel_registerName("sendMultiTouchEvents:"), @[multiTouch]);
    [multiTouch release];
}

// iPadOS interleaves pointer events without finger data while fingers stay
// down, so those never mean "lifted". Lift the fingers only when the trackpad
// reports them untouched, or when finger data stops arriving.
static void scheduleLiftTimeout(void) {
    uint64_t generation = ++gFingerGeneration;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        if (generation != gFingerGeneration || !gTouchDevice || !gFingerCount) return;
        gFingerCount = 0;
        gDownMask = 0;
        sendTouches();
    });
}

static void relayCallback(void *target, void *refcon, IOHIDServiceClientRef service,
                          IOHIDEventRef event) {
    (void)target; (void)refcon; (void)service;
    uint32_t type = IOHIDEventGetType(event);
    if (!gTouchDevice || (type != kPointerEvent && type != kScrollEvent)) return;
    uint64_t sender = IOHIDEventGetSenderID(event);
    IOHIDEventRef hand = type == kPointerEvent ? digitizerChild(event) : NULL;
    // Pointer and scroll events from other senders are a mouse.
    if (!hand && sender != gTrackpadSender) {
        gLastMouseTime = CACurrentMediaTime();
        // The iPad pointer is locked, so UIKit no longer reports where a mouse
        // is or reliably delivers its clicks, and a mouse need not be a
        // GCMouse. Hand its relative motion and buttons to the app.
        if (type == kPointerEvent && gMouseMoved) {
            double dx = IOHIDEventGetFloatValue(event, FIELD(kPointerEvent, 0));
            double dy = IOHIDEventGetFloatValue(event, FIELD(kPointerEvent, 1));
            if (dx || dy) gMouseMoved(dx, dy);
            long buttons = (long)(IOHIDEventGetIntegerValue(event,
                FIELD(kPointerEvent, kButtonMask)) & 7);
            if (buttons != gMouseButtonState && gMouseButtons) {
                gMouseButtonState = buttons;
                gMouseButtons(buttons);
            }
        }
        return;
    }
    gLastTrackpadTime = CACurrentMediaTime();
    if (type != kPointerEvent) return;
    CFIndex buttons = IOHIDEventGetIntegerValue(event, FIELD(kPointerEvent, kButtonMask));
    if (!hand) {
        if (!(buttons & 7) == !(gButtons & 7)) return;
        gButtons = buttons;
        sendTouches();
        return;
    }
    gTrackpadSender = sender;
    buttons |= IOHIDEventGetIntegerValue(hand, FIELD(kDigitizerEvent, kButtonMask));
    gButtons = buttons;
    gFingerCount = 0;
    uint64_t wasDown = gDownMask, nowDown = 0;
    CFArrayRef children = IOHIDEventGetChildren(hand);
    for (CFIndex i = 0; children && i < CFArrayGetCount(children) && gFingerCount < 16; i++) {
        IOHIDEventRef finger = (IOHIDEventRef)CFArrayGetValueAtIndex(children, i);
        if (IOHIDEventGetType(finger) != kDigitizerEvent) continue;
        // The touch flag of a finger already down can drop for a frame or
        // two while it slides; keep it down while the finger stays in range.
        uint8_t index = (uint8_t)(IOHIDEventGetIntegerValue(finger,
            FIELD(kDigitizerEvent, kIdentity)) % kClickTouchIndex);
        BOOL touching = IOHIDEventGetIntegerValue(finger, FIELD(kDigitizerEvent, kTouch)) != 0;
        BOOL inRange = IOHIDEventGetIntegerValue(finger, FIELD(kDigitizerEvent, kRange)) != 0;
        if (!touching && !(inRange && (wasDown & (1ULL << index)))) continue;
        nowDown |= 1ULL << index;
        RelayFinger *f = &gFingers[gFingerCount++];
        f->index = index;
        // A finger held down only by being in range is lifting; its position
        // drifts as the contact shrinks, which would fling a scroll. Keep the
        // last position it had while touching.
        if (touching) {
            gLastX[index] = IOHIDEventGetFloatValue(finger, FIELD(kDigitizerEvent, 0));
            gLastY[index] = IOHIDEventGetFloatValue(finger, FIELD(kDigitizerEvent, 1));
        }
        f->x = gLastX[index];
        f->y = gLastY[index];
    }
    gDownMask = nowDown;
    sendTouches();
    if (gFingerCount) scheduleLiftTimeout();
    if (gEventCount++ % 500 == 0)
        relayLog("event #%llu sender=0x%llx fingers=%u buttons=0x%lx",
                 (unsigned long long)gEventCount, (unsigned long long)sender,
                 gFingerCount, (long)buttons);
}

// Ask the root view controller to re-evaluate prefersPointerLocked, which
// depends on whether the relay runs.
static void updatePointerLock(void) {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        for (UIWindow *window in ((UIWindowScene *)scene).windows) {
            UIViewController *root = window.rootViewController;
            if (![root respondsToSelector:@selector(logPointerLockState:)]) continue;
            [root setNeedsUpdateOfPrefersPointerLocked];
            [root performSelector:@selector(logPointerLockState:) withObject:nil];
        }
    }
}

// The app moves its absolute pointer by a mouse's relative motion and
// presses its buttons.
void VZUSBTrackpadRelaySetMouseHandler(void (*moved)(double dx, double dy),
                                       void (*buttons)(long buttons)) {
    gMouseMoved = moved;
    gMouseButtons = buttons;
}

void VZUSBTrackpadRelayStart(id virtualMachine) {
    if (gClient) return;
    id devices = [virtualMachine respondsToSelector:sel_registerName("_multiTouchDevices")]
        ? ((id(*)(id, SEL))objc_msgSend)(virtualMachine, sel_registerName("_multiTouchDevices"))
        : nil;
    gTouchDevice = [[devices firstObject] retain];
    if (!gTouchDevice) return;  // another pointing device was chosen
    gClient = IOHIDEventSystemClientCreateWithType(NULL, kMonitorClient, NULL);
    relayLog("start device=%s client=%p",
             [[gTouchDevice description] UTF8String] ?: "(none)", gClient);
    if (!gClient) { [gTouchDevice release]; gTouchDevice = nil; return; }
    IOHIDEventSystemClientRegisterEventCallback(gClient, relayCallback, NULL, NULL);
    IOHIDEventSystemClientScheduleWithRunLoop(gClient, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    dispatch_async(dispatch_get_main_queue(), ^{ updatePointerLock(); });
}

void VZUSBTrackpadRelayStop(void) {
    if (!gClient) return;
    IOHIDEventSystemClientUnregisterEventCallback(gClient, relayCallback, NULL, NULL);
    IOHIDEventSystemClientUnscheduleWithRunLoop(gClient, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    CFRelease(gClient);
    gClient = NULL;
    [gTouchDevice release];
    gTouchDevice = nil;
    gTrackpadSender = 0;
    gFingerCount = 0;
    gDownMask = 0;
    gButtons = 0;
    gLastTrackpadTime = gLastMouseTime = 0;
    gMouseButtonState = 0;
    gFingerGeneration++;
    dispatch_async(dispatch_get_main_queue(), ^{ updatePointerLock(); });
    relayLog("stop after %llu events", (unsigned long long)gEventCount);
}

BOOL VZUSBTrackpadRelayActive(void) {
    return gClient != NULL;
}

// When the iPad trackpad and a mouse last produced events, for the app to
// decide which device its own UIKit pointer, scroll and gesture events came
// from: the emulated trackpad already gives macOS the trackpad's, so those
// would arrive twice.
CFTimeInterval VZUSBTrackpadRelayLastTrackpadTime(void) {
    return gLastTrackpadTime;
}

CFTimeInterval VZUSBTrackpadRelayLastMouseTime(void) {
    return gLastMouseTime;
}

void VZUSBTrackpadRelayNote(NSString *message) {
    relayLog("%s", message.UTF8String);
}
