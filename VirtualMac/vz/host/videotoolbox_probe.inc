// Diagnostic probe for the paravirtualized VideoToolbox device.
//
// The VMM runs two VideoToolbox images: the Ventura one bundled in the payload,
// which implements the guest-facing host session, and iPadOS's own. Report
// what each can decode in hardware inside this process, and count the host
// session traffic the guest actually generates. Nothing is changed: session
// calls are forwarded with their original arguments.
//
// Enabled only while /var/root/VirtualMac2/diagnostics/videotoolbox-probe exists.
#include <CoreFoundation/CoreFoundation.h>

static const char vtProbeFlag[] = "/var/root/VirtualMac2/diagnostics/videotoolbox-probe";
static const char vtBundledSuffix[] = "/payload/Frameworks/VideoToolbox.framework/VideoToolbox";
static const char vtSystemPath[] = "/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox";

typedef int32_t (*VTProbeFormatCreate)(CFAllocatorRef, size_t, const uint8_t *const *,
                                       const size_t *, int, void **);
typedef int32_t (*VTProbeSessionCreate)(CFAllocatorRef, void *, CFDictionaryRef,
                                        CFDictionaryRef, const void *, void **);
typedef int32_t (*VTProbeCopyProperty)(void *, CFStringRef, CFAllocatorRef, void *);
typedef void (*VTProbeInvalidate)(void *);
typedef Boolean (*VTProbeHardwareSupported)(uint32_t);

static void vt_probe_image(const char *label, const char *path, void *format) {
    void *image = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    if (!image) { logf_("[VTProbe] %s image not loaded path=%s", label, path); return; }
    VTProbeHardwareSupported supported = dlsym(image, "VTIsHardwareDecodeSupported");
    VTProbeSessionCreate create = dlsym(image, "VTDecompressionSessionCreate");
    VTProbeCopyProperty copy = dlsym(image, "VTSessionCopyProperty");
    VTProbeInvalidate invalidate = dlsym(image, "VTDecompressionSessionInvalidate");
    logf_("[VTProbe] %s path=%s hostSessionCreate=%p hardwareSupported h264=%d hevc=%d",
          label, path, dlsym(image, "VTParavirtualizationHostSessionCreate"),
          supported ? supported('avc1') : -1, supported ? supported('hvc1') : -1);
    if (!create || !copy || !invalidate || !format) return;
    for (int require = 0; require < 2; ++require) {
        CFStringRef key = require ? CFSTR("RequireHardwareAcceleratedVideoDecoder")
                                  : CFSTR("EnableHardwareAcceleratedVideoDecoder");
        const void *keys[] = {key}, *values[] = {kCFBooleanTrue};
        CFDictionaryRef specification = CFDictionaryCreate(NULL, keys, values, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        void *session = NULL;
        int32_t status = create(NULL, format, specification, NULL, NULL, &session);
        CFBooleanRef hardware = NULL;
        CFStringRef decoder = NULL;
        if (!status && session) {
            copy(session, CFSTR("UsingHardwareAcceleratedVideoDecoder"), NULL, &hardware);
            copy(session, CFSTR("DecoderID"), NULL, &decoder);
        }
        char name[128] = "?";
        if (decoder) CFStringGetCString(decoder, name, sizeof(name), kCFStringEncodingUTF8);
        logf_("[VTProbe] %s h264 session hardware=%s status=%d usingHardware=%s decoder=%s",
              label, require ? "required" : "allowed", status,
              hardware ? (CFBooleanGetValue(hardware) ? "yes" : "no") : "unknown", name);
        if (hardware) CFRelease(hardware);
        if (decoder) CFRelease(decoder);
        if (session) { invalidate(session); CFRelease(session); }
        CFRelease(specification);
    }
}

static void vt_probe_run(void) {
    static const uint8_t sps[] = {0x67,0x42,0xC0,0x1F,0xDA,0x01,0x40,0x16,0xE8,0x06,0xD0,0xA1,0x35};
    static const uint8_t pps[] = {0x68,0xCE,0x06,0xE2};
    const uint8_t *sets[] = {sps, pps};
    size_t sizes[] = {sizeof(sps), sizeof(pps)};
    VTProbeFormatCreate formatCreate =
        dlsym(RTLD_DEFAULT, "CMVideoFormatDescriptionCreateFromH264ParameterSets");
    void *format = NULL;
    int32_t status = formatCreate ? formatCreate(NULL, 2, sets, sizes, 4, &format) : -1;
    logf_("[VTProbe] format status=%d decoderPluginDirectory=%d", status,
          access("/System/Library/Video/Plug-Ins", F_OK) == 0);
    const char *bundled = NULL;
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const char *name = _dyld_get_image_name(i);
        size_t length = name ? strlen(name) : 0;
        if (length >= sizeof(vtBundledSuffix) - 1 &&
            !strcmp(name + length - (sizeof(vtBundledSuffix) - 1), vtBundledSuffix)) bundled = name;
    }
    if (bundled) vt_probe_image("bundled-macOS13", bundled, format);
    else logf_("[VTProbe] bundled VideoToolbox image not found");
    vt_probe_image("system-iPadOS", vtSystemPath, format);
    if (format) CFRelease(format);
}

// Host session entry points take pointer and integer arguments only, so the
// eight argument registers carry every argument and can be forwarded as is.
typedef uint64_t (*VTProbeForward)(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t);
// Weak: this library also loads where no VideoToolbox exports the host session,
// and an unresolved strong reference would stop that process from launching.
extern uint64_t VTParavirtualizationHostSessionCreate(uint64_t, uint64_t, uint64_t, uint64_t,
    uint64_t, uint64_t, uint64_t, uint64_t) __attribute__((weak_import));
extern uint64_t VTParavirtualizationHostSessionDeliverMessageFromGuest(
    uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
    __attribute__((weak_import));
static BOOL vt_probe_enabled;
static unsigned vt_probe_creates, vt_probe_messages;

static uint64_t vt_probe_session_create(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                                        uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    uint64_t result = VTParavirtualizationHostSessionCreate(a, b, c, d, e, f, g, h);
    if (vt_probe_enabled)
        logf_("[VTProbe] host session create #%u -> %d", __sync_add_and_fetch(&vt_probe_creates, 1),
              (int32_t)result);
    return result;
}
static uint64_t vt_probe_deliver(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                                 uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    uint64_t result = VTParavirtualizationHostSessionDeliverMessageFromGuest(a, b, c, d, e, f, g, h);
    if (vt_probe_enabled) {
        unsigned count = __sync_add_and_fetch(&vt_probe_messages, 1);
        if (count <= 32 || !(count & (count - 1)))
            logf_("[VTProbe] guest message #%u session=%p -> %d", count, (void *)a, (int32_t)result);
    }
    return result;
}
__attribute__((used)) static struct { const void *replacement, *original; }
_ip_vt_probe[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void *)vt_probe_session_create, (const void *)VTParavirtualizationHostSessionCreate},
    {(const void *)vt_probe_deliver,
     (const void *)VTParavirtualizationHostSessionDeliverMessageFromGuest},
};

static void videotoolbox_probe_install(void) {
    vt_probe_enabled = access(vtProbeFlag, F_OK) == 0;
    if (!vt_probe_enabled) return;
    // Let Virtualization finish creating its devices before probing.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC),
                   dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ vt_probe_run(); });
}
