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
#include <crt_externs.h>

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

// The VMM copies every host-to-guest VideoToolbox message into a buffer the
// guest posted and asserts "Buffer too small" when it does not fit. Keep the
// largest size seen for each message type on that path, which is the only
// place the VMM executable itself asks a block buffer for its length, so an
// oversized message can be identified from the log after the VMM stops on it.
extern size_t CMBlockBufferGetDataLength(void *buffer) __attribute__((weak_import));
static struct { uint64_t type; size_t largest; unsigned count; } vtProbeMessages[32];
static pthread_mutex_t vtProbeMessageLock = PTHREAD_MUTEX_INITIALIZER;
static size_t vt_probe_block_length(void *buffer) {
    size_t length = CMBlockBufferGetDataLength(buffer);
    if (!vt_probe_enabled) return length;
    void *caller = __builtin_return_address(0);
#if __has_feature(ptrauth_calls)
    caller = ptrauth_strip(caller, ptrauth_key_return_address);
#endif
    Dl_info information = {0};
    if (!dladdr(caller, &information) || information.dli_fbase != (void *)_NSGetMachExecuteHeader())
        return length;
    int32_t (*copyBytes)(void *, size_t, size_t, void *) =
        dlsym(RTLD_DEFAULT, "CMBlockBufferCopyDataBytes");
    uint8_t head[12] = {0};
    if (!copyBytes || length < sizeof(head) || copyBytes(buffer, 0, sizeof(head), head)) return length;
    uint64_t type;
    memcpy(&type, head + 4, sizeof(type));
    pthread_mutex_lock(&vtProbeMessageLock);
    for (unsigned i = 0; i < sizeof(vtProbeMessages) / sizeof(vtProbeMessages[0]); ++i) {
        if (vtProbeMessages[i].count && vtProbeMessages[i].type != type) continue;
        BOOL grew = length > vtProbeMessages[i].largest;
        vtProbeMessages[i].type = type;
        ++vtProbeMessages[i].count;
        if (grew) {
            vtProbeMessages[i].largest = length;
            logf_("[VTProbe] host-to-guest message type=%.8s largest=%zu count=%u", head + 4,
                  length, vtProbeMessages[i].count);
        }
        break;
    }
    pthread_mutex_unlock(&vtProbeMessageLock);
    return length;
}
__attribute__((used)) static struct { const void *replacement, *original; }
_ip_vt_probe_block[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void *)vt_probe_block_length, (const void *)CMBlockBufferGetDataLength},
};

// On-CPU profile of the whole VMM, taken while the guest decodes a large
// stream through the hardware bridge. Every few milliseconds each running
// thread is suspended just long enough to copy its frame-pointer chain into a
// preallocated buffer; symbols are resolved only after sampling has finished,
// so no suspended thread can be holding a lock the sampler needs.
//
// Runs once per large session while
// /var/root/VirtualMac2/diagnostics/videotoolbox-profile exists.
static const char vtProfileFlag[] = "/var/root/VirtualMac2/diagnostics/videotoolbox-profile";
enum { vtProfileDepth = 24, vtProfileCapacity = 60000 };
typedef struct { uint32_t thread; uint16_t index; uint8_t depth; uintptr_t pc[vtProfileDepth]; } VTProfileSample;
static int vt_profile_running;

static uintptr_t vt_profile_strip(uintptr_t pc) {
#if __has_feature(ptrauth_calls)
    return (uintptr_t)ptrauth_strip((void *)pc, ptrauth_key_return_address);
#else
    return pc;
#endif
}

static int vt_profile_compare(const void *a, const void *b) {
    const uint64_t *x = a, *y = b;
    return x[1] < y[1] ? 1 : x[1] > y[1] ? -1 : 0;
}

// Name a code address as symbol, or as image+offset where the image is stripped.
static void vt_profile_name(uintptr_t pc, char *out, size_t size) {
    Dl_info information = {0};
    if (!dladdr((void *)pc, &information)) { snprintf(out, size, "0x%lx", (unsigned long)pc); return; }
    const char *image = information.dli_fname ? strrchr(information.dli_fname, '/') : NULL;
    image = image ? image + 1 : "?";
    if (information.dli_sname && strcmp(information.dli_sname, "<redacted>"))
        snprintf(out, size, "%s %s", image, information.dli_sname);
    else
        snprintf(out, size, "%s+0x%lx", image, (unsigned long)(pc - (uintptr_t)information.dli_fbase));
}

// For the busiest threads, list the frames that appear in most of their
// samples, by exact return address, so stripped code can be found in a
// disassembly.
static void vt_profile_report_threads(VTProfileSample *samples, unsigned count) {
    enum { threadSlots = 128, frameSlots = 2048 };
    static uint32_t threadCounts[threadSlots];
    memset(threadCounts, 0, sizeof(threadCounts));
    for (unsigned i = 0; i < count; ++i)
        if (samples[i].index < threadSlots) ++threadCounts[samples[i].index];
    for (int rank = 0; rank < 4; ++rank) {
        unsigned best = 0;
        for (unsigned t = 1; t < threadSlots; ++t) if (threadCounts[t] > threadCounts[best]) best = t;
        if (!best || !threadCounts[best]) break;
        unsigned total = threadCounts[best];
        threadCounts[best] = 0;
        char name[64] = "";
        static uint64_t frames[frameSlots][2];
        memset(frames, 0, sizeof(frames));
        for (unsigned i = 0; i < count; ++i) {
            if (samples[i].index != best) continue;
            if (!name[0]) {
                pthread_t pthread = pthread_from_mach_thread_np(samples[i].thread);
                if (pthread) pthread_getname_np(pthread, name, sizeof(name));
            }
            for (unsigned d = 0; d < samples[i].depth; ++d) {
                uintptr_t key = samples[i].pc[d];
                for (unsigned probe = 0; probe < frameSlots; ++probe) {
                    unsigned slot = (unsigned)((key >> 2) * 2654435761u + probe) % frameSlots;
                    if (frames[slot][0] && frames[slot][0] != key) continue;
                    frames[slot][0] = key;
                    ++frames[slot][1];
                    break;
                }
            }
        }
        qsort(frames, frameSlots, sizeof(frames[0]), vt_profile_compare);
        logf_("[VTProfile] thread %u \"%s\" %u samples", best, name, total);
        for (unsigned i = 0; i < 24 && frames[i][1]; ++i) {
            char text[256];
            vt_profile_name((uintptr_t)frames[i][0], text, sizeof(text));
            logf_("[VTProfile]   %5.1f%% %s", 100.0 * frames[i][1] / total, text);
        }
    }
}

static void vt_profile_report(VTProfileSample *samples, unsigned count, unsigned ticks) {
    // Count, per function start address, the samples it is the leaf of and the
    // samples it appears in at all (once per sample).
    enum { slots = 4096 };
    static uint64_t leaf[slots][2], inclusive[slots][2];
    memset(leaf, 0, sizeof(leaf));
    memset(inclusive, 0, sizeof(inclusive));
    for (unsigned i = 0; i < count; ++i) {
        uintptr_t seen[vtProfileDepth];
        unsigned seenCount = 0;
        for (unsigned d = 0; d < samples[i].depth; ++d) {
            Dl_info information = {0};
            uintptr_t pc = samples[i].pc[d], key = pc;
            if (dladdr((void *)(d ? pc - 4 : pc), &information) && information.dli_saddr)
                key = (uintptr_t)information.dli_saddr;
            BOOL duplicate = NO;
            for (unsigned k = 0; k < seenCount; ++k) if (seen[k] == key) duplicate = YES;
            if (duplicate) continue;
            seen[seenCount++] = key;
            for (int table = d ? 1 : 0; table < 2; ++table) {
                uint64_t (*bucket)[2] = table ? inclusive : leaf;
                for (unsigned probe = 0; probe < slots; ++probe) {
                    unsigned slot = (unsigned)((key >> 2) * 2654435761u + probe) % slots;
                    if (bucket[slot][0] && bucket[slot][0] != key) continue;
                    bucket[slot][0] = key;
                    ++bucket[slot][1];
                    break;
                }
            }
        }
    }
    vt_profile_report_threads(samples, count);
    qsort(leaf, slots, sizeof(leaf[0]), vt_profile_compare);
    qsort(inclusive, slots, sizeof(inclusive[0]), vt_profile_compare);
    logf_("[VTProfile] %u on-CPU thread samples over %u ticks", count, ticks);
    for (int table = 0; table < 2; ++table) {
        uint64_t (*bucket)[2] = table ? inclusive : leaf;
        for (unsigned i = 0; i < 40 && bucket[i][1]; ++i) {
            Dl_info information = {0};
            dladdr((void *)bucket[i][0], &information);
            const char *image = information.dli_fname ? strrchr(information.dli_fname, '/') : NULL;
            logf_("[VTProfile] %s %6llu %5.1f%% %s %s", table ? "incl" : "self",
                  (unsigned long long)bucket[i][1], 100.0 * bucket[i][1] / (count ? count : 1),
                  image ? image + 1 : "?", information.dli_sname ? information.dli_sname : "?");
        }
    }
}

static void vt_profile_run(double seconds) {
    VTProfileSample *samples = calloc(vtProfileCapacity, sizeof(VTProfileSample));
    if (!samples) { __atomic_store_n(&vt_profile_running, 0, __ATOMIC_RELEASE); return; }
    unsigned count = 0, ticks = 0;
    thread_act_t self = mach_thread_self();
    uint64_t end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + (uint64_t)(seconds * 1e9);
    while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < end && count < vtProfileCapacity) {
        ++ticks;
        thread_act_array_t threads = NULL;
        mach_msg_type_number_t threadCount = 0;
        if (task_threads(mach_task_self(), &threads, &threadCount) != KERN_SUCCESS) break;
        for (mach_msg_type_number_t t = 0; t < threadCount; ++t) {
            thread_act_t thread = threads[t];
            if (thread == self || count >= vtProfileCapacity) goto next;
            struct thread_basic_info basic;
            mach_msg_type_number_t basicCount = THREAD_BASIC_INFO_COUNT;
            if (thread_info(thread, THREAD_BASIC_INFO, (thread_info_t)&basic, &basicCount) ||
                basic.run_state != TH_STATE_RUNNING) goto next;
            pthread_t pthread = pthread_from_mach_thread_np(thread);
            uintptr_t top = pthread ? (uintptr_t)pthread_get_stackaddr_np(pthread) : 0;
            uintptr_t bottom = pthread ? top - pthread_get_stacksize_np(pthread) : 0;
            if (thread_suspend(thread) != KERN_SUCCESS) goto next;
            arm_thread_state64_t state;
            mach_msg_type_number_t stateCount = ARM_THREAD_STATE64_COUNT;
            if (thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&state, &stateCount) ==
                KERN_SUCCESS) {
                VTProfileSample *sample = &samples[count++];
                sample->thread = thread;
                sample->index = (uint16_t)(t + 1);
                sample->pc[0] = vt_profile_strip(arm_thread_state64_get_pc(state));
                sample->depth = 1;
                uintptr_t lr = vt_profile_strip(arm_thread_state64_get_lr(state));
                if (lr && lr != sample->pc[0]) sample->pc[sample->depth++] = lr;
                uintptr_t fp = arm_thread_state64_get_fp(state);
                while (sample->depth < vtProfileDepth && top && fp >= bottom && fp + 16 <= top &&
                       !(fp & 7)) {
                    uintptr_t next = ((uintptr_t *)fp)[0], pc = vt_profile_strip(((uintptr_t *)fp)[1]);
                    if (!pc) break;
                    if (sample->depth != 2 || pc != sample->pc[1]) sample->pc[sample->depth++] = pc;
                    if (next <= fp) break;
                    fp = next;
                }
            }
            thread_resume(thread);
        next:
            mach_port_deallocate(mach_task_self(), thread);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)threads, threadCount * sizeof(thread_act_t));
        usleep(2000);
    }
    mach_port_deallocate(mach_task_self(), self);
    vt_profile_report(samples, count, ticks);
    free(samples);
    __atomic_store_n(&vt_profile_running, 0, __ATOMIC_RELEASE);
}

// Called when the bridge starts a session for a stream of the given size.
static void vt_profile_session_started(uint64_t pixels) {
    if (pixels <= 1920ULL * 1088 || access(vtProfileFlag, F_OK) ||
        __atomic_exchange_n(&vt_profile_running, 1, __ATOMIC_ACQ_REL)) return;
    // Skip the first second, in which the pools are still being filled.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                   dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ vt_profile_run(4.0); });
}

// Profile the VMM for ten seconds whenever this file appears; it is removed
// when the profile starts.
static const char vtProfileNowFlag[] = "/var/root/VirtualMac2/diagnostics/vmm-profile-now";
static void vt_profile_watch(void) {
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                              2 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
    dispatch_source_set_event_handler(timer, ^{
        if (access(vtProfileNowFlag, F_OK) || unlink(vtProfileNowFlag) ||
            __atomic_exchange_n(&vt_profile_running, 1, __ATOMIC_ACQ_REL)) return;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ vt_profile_run(10.0); });
    });
    dispatch_resume(timer);
}

static void videotoolbox_probe_install(void) {
    vt_probe_enabled = access(vtProbeFlag, F_OK) == 0;
    if (!vt_probe_enabled) return;
    vt_profile_watch();
    // Let Virtualization finish creating its devices before probing.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC),
                   dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ vt_probe_run(); });
}
