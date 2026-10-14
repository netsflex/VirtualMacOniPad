// Hardware video decoding for the paravirtualized VideoToolbox device.
//
// The bundled Ventura VideoToolbox implements the guest-facing host session
// but finds no decoders on iPadOS: it scans /System/Library/Video/Plug-Ins,
// which does not exist. iPadOS's own VideoToolbox decodes in hardware but has
// no host session. Register a decoder with the bundled framework, through the
// plug-in interface WebKit's VP9 decoder also uses, that forwards every frame
// to a hardware session of the system framework. Both frameworks share one
// CoreMedia, so sample buffers and pixel buffers pass through unchanged.
//
// Experimental: enabled only while
// /var/root/VirtualMac2/diagnostics/videotoolbox-bridge exists.

// Ventura's decoder selection asks CoreMedia whether a registered item needs
// Rosetta. iPadOS's CoreMedia has neither entry point, so the bundled
// framework's weak flat-namespace references are NULL and the first registered
// decoder it selects jumps there. Nothing on iPadOS runs under Rosetta.
__attribute__((visibility("default"))) Boolean FigRegistryItemRequiresRosetta(void *item) {
    (void)item;
    return false;
}
__attribute__((visibility("default"))) Boolean FigRegistryItemRequiresMissingRosetta(void *item) {
    (void)item;
    return false;
}

static const char vtBridgeFlag[] = "/var/root/VirtualMac2/diagnostics/videotoolbox-bridge";
// Decoded planes are copied into a buffer from the host session's own pool:
// handing back the decoder's buffer reaches the guest as an empty image. This
// flag selects that direct hand-back again, to investigate why.
static const char vtBridgeDirectFlag[] = "/var/root/VirtualMac2/diagnostics/videotoolbox-bridge-direct";


// CMBaseObject and VTVideoDecoder class layouts (CoreMedia/VideoToolbox SPI).
// On arm64e every member is signed with its own address-diversified
// discriminator. The values are the ones the bundled framework's ten built-in
// decoder classes carry in their fixups and that its call sites authenticate
// against. Members no built-in class sets stay NULL.
#if __has_feature(ptrauth_calls)
#define VT_BRIDGE_CODE(discriminator) __ptrauth(ptrauth_key_asia, 1, discriminator)
#define VT_BRIDGE_DATA(discriminator) __ptrauth(ptrauth_key_asda, 1, discriminator)
#else
#define VT_BRIDGE_CODE(discriminator)
#define VT_BRIDGE_DATA(discriminator)
#endif
typedef struct {
    uintptr_t version;
    size_t derivedStorageSize;
    void *equal;
    int32_t (*VT_BRIDGE_CODE(0x3AF8) invalidate)(void *);
    void (*VT_BRIDGE_CODE(0x5680) finalize)(void *);
    CFStringRef (*VT_BRIDGE_CODE(0xF18E) copyDebugDescription)(void *);
    int32_t (*VT_BRIDGE_CODE(0x9725) copyProperty)(void *, CFStringRef, CFAllocatorRef, void *);
    int32_t (*VT_BRIDGE_CODE(0x2724) setProperty)(void *, CFStringRef, CFTypeRef);
    void *notificationBarrier;
    const void *protocolTable;
} VTBridgeBaseClass;
typedef struct {
    uintptr_t version;
    int32_t (*VT_BRIDGE_CODE(0x7BE9) startSession)(void *decoder, void *session, void *format);
    int32_t (*VT_BRIDGE_CODE(0xAF83) decodeFrame)(void *decoder, void *frame, void *sampleBuffer,
                                                  uint32_t flags, uint32_t *infoFlagsOut);
    int32_t (*VT_BRIDGE_CODE(0x50EB) copySupportedPropertyDictionary)(void *decoder,
                                                                      CFDictionaryRef *out);
    void *setProperties, *copySerializableProperties, *canAcceptFormatDescription;
    int32_t (*VT_BRIDGE_CODE(0xBFC3) finishDelayedFrames)(void *decoder);
    void *startTileSession, *decodeTile, *finishDelayedTiles;
} VTBridgeDecoderClass;
typedef struct {
    struct {
        const void *reserved;
        const VTBridgeBaseClass *VT_BRIDGE_DATA(0xB911) baseClass;
    } base;
    const VTBridgeDecoderClass *VT_BRIDGE_DATA(0x2356) decoderClass;
} VTBridgeVTable;
typedef struct { int64_t value; int32_t timescale; uint32_t flags; int64_t epoch; } VTBridgeTime;
typedef struct {
    void (*callback)(void *refcon, void *frame, int32_t status, uint32_t infoFlags,
                     void *imageBuffer, VTBridgeTime presentation, VTBridgeTime duration);
    void *refcon;
} VTBridgeOutputRecord;

// Per-decoder storage. hostSession is the bundled framework's decoder session.
typedef struct {
    void *systemSession, *hostSession;
    // The format the system session was created for or last accepted.
    void *format;
    BOOL copy, invalidating, failed;
    unsigned index, frames, emitFailures, copyFailures, formatChanges;
    uint64_t emitNanoseconds, slowestEmit, copyNanoseconds;
} VTBridgeDecoder;

static struct {
    // Bundled framework.
    int32_t (*registerDecoder)(uint32_t, CFDictionaryRef, int32_t (*)(uint32_t, CFAllocatorRef, void **));
    void *(*decoderClassID)(void);
    int32_t (*emitFrame)(void *session, void *frame, int32_t status, uint32_t infoFlags, void *image);
    int32_t (*setPixelBufferAttributes)(void *session, CFDictionaryRef);
    CFDictionaryRef (*destinationAttributes)(void *session);
    int32_t (*createPixelBuffer)(void *session, void *unused, void **pixelBufferOut);
    void (*cleanUpAfterDecode)(void *session);
    // CoreMedia.
    int32_t (*derivedObjectCreate)(CFAllocatorRef, const void *vtable, void *classID, void **);
    void *(*derivedStorage)(void *);
    // System framework.
    int32_t (*sessionCreate)(CFAllocatorRef, void *format, CFDictionaryRef, CFDictionaryRef,
                             const VTBridgeOutputRecord *, void **);
    int32_t (*sessionDecode)(void *, void *sampleBuffer, uint32_t flags, void *frame, uint32_t *);
    int32_t (*sessionFinish)(void *);
    int32_t (*sessionWait)(void *);
    void (*sessionInvalidate)(void *);
    Boolean (*sessionCanAccept)(void *, void *format);
} vtBridge;
static unsigned vtBridgeSessions, vtBridgeFrames, vtBridgeErrors;

static void vt_bridge_describe(const char *label, CFTypeRef object) {
    CFStringRef description = object ? CFCopyDescription(object) : NULL;
    char text[700] = "(null)";
    if (description) {
        CFStringGetCString(description, text, sizeof(text), kCFStringEncodingUTF8);
        CFRelease(description);
    }
    for (char *c = text; *c; ++c) if (*c == '\n') *c = ' ';
    logf_("[VTBridge] %s %s", label, text);
}

// Copy the decoded planes into a buffer from the host session's pool.
static void *vt_bridge_copy(VTBridgeDecoder *decoder, void *source, unsigned count) {
    static int32_t (*lock)(void *, uint64_t), (*unlock)(void *, uint64_t);
    static size_t (*planes)(void *), (*rowBytes)(void *, size_t), (*rows)(void *, size_t);
    static void *(*base)(void *, size_t);
    static uint32_t (*formatOf)(void *);
    if (!lock) {
        lock = dlsym(RTLD_DEFAULT, "CVPixelBufferLockBaseAddress");
        unlock = dlsym(RTLD_DEFAULT, "CVPixelBufferUnlockBaseAddress");
        planes = dlsym(RTLD_DEFAULT, "CVPixelBufferGetPlaneCount");
        rowBytes = dlsym(RTLD_DEFAULT, "CVPixelBufferGetBytesPerRowOfPlane");
        rows = dlsym(RTLD_DEFAULT, "CVPixelBufferGetHeightOfPlane");
        base = dlsym(RTLD_DEFAULT, "CVPixelBufferGetBaseAddressOfPlane");
        formatOf = dlsym(RTLD_DEFAULT, "CVPixelBufferGetPixelFormatType");
    }
    if (!lock || !unlock || !planes || !rowBytes || !rows || !base || !formatOf ||
        !vtBridge.createPixelBuffer) return NULL;
    void *target = NULL;
    int32_t status = vtBridge.createPixelBuffer(decoder->hostSession, NULL, &target);
    (void)count;
    if (status || !target) {
        if (++decoder->copyFailures <= 4)
            logf_("[VTBridge] session #%u host pool buffer status=%d", decoder->index, status);
        return NULL;
    }
    size_t planeCount = planes(source);
    if (formatOf(source) != formatOf(target) || planeCount != planes(target) || !planeCount) {
        if (++decoder->copyFailures <= 4)
            logf_("[VTBridge] session #%u host pool buffer format 0x%x planes=%zu differs from decoded 0x%x planes=%zu",
                  decoder->index, formatOf(target), planes(target), formatOf(source), planeCount);
        CFRelease(target);
        return NULL;
    }
    lock(source, 1);
    lock(target, 0);
    for (size_t plane = 0; plane < planeCount; ++plane) {
        size_t sourceRow = rowBytes(source, plane), targetRow = rowBytes(target, plane);
        size_t width = sourceRow < targetRow ? sourceRow : targetRow;
        size_t height = rows(source, plane) < rows(target, plane) ? rows(source, plane) : rows(target, plane);
        const uint8_t *from = base(source, plane);
        uint8_t *to = base(target, plane);
        for (size_t row = 0; from && to && row < height; ++row)
            memcpy(to + row * targetRow, from + row * sourceRow, width);
    }
    unlock(target, 0);
    unlock(source, 1);
    return target;
}

static void vt_bridge_output(void *refcon, void *frame, int32_t status, uint32_t infoFlags,
                             void *imageBuffer, VTBridgeTime presentation, VTBridgeTime duration) {
    (void)presentation; (void)duration;
    VTBridgeDecoder *decoder = refcon;
    unsigned count = __sync_add_and_fetch(&vtBridgeFrames, 1);
    if (status && __sync_add_and_fetch(&vtBridgeErrors, 1) <= 16)
        logf_("[VTBridge] system decoder returned status=%d frame #%u", status, count);

    // Invalidation drains the system session. The guest is tearing the decoder
    // down and no longer wants these images; report them as dropped instead of
    // sending full frame messages into its shutdown.
    if (__atomic_load_n(&decoder->invalidating, __ATOMIC_ACQUIRE)) {
        vtBridge.emitFrame(decoder->hostSession, frame, status ? status : -12903, infoFlags, NULL);
        return;
    }
    void *emitted = imageBuffer, *copied = NULL;
    // After the guest stopped acknowledging frames, each further image would
    // wait out the host session's one second reply timeout. Report an error.
    if (decoder->failed) {
        vtBridge.emitFrame(decoder->hostSession, frame, status ? status : -12911, infoFlags, NULL);
        return;
    }
    if (decoder->frames < 3)
        logf_("[VTBridge] session #%u output %u status=%d infoFlags=0x%x image=%p", decoder->index,
              decoder->frames + 1, status, infoFlags, imageBuffer);
    uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (decoder->copy && imageBuffer && !status) {
        copied = vt_bridge_copy(decoder, imageBuffer, count);
        if (copied) emitted = copied;
    }
    uint64_t copiedAt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    int32_t result = vtBridge.emitFrame(decoder->hostSession, frame, status, infoFlags, emitted);
    uint64_t spent = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - copiedAt;
    unsigned frames = ++decoder->frames;
    decoder->copyNanoseconds += copiedAt - began;
    decoder->emitNanoseconds += spent;
    if (spent > decoder->slowestEmit) decoder->slowestEmit = spent;
    if (result) {
        if (++decoder->emitFailures >= 2) decoder->failed = YES;
        logf_("[VTBridge] session #%u emit frame %u copy=%d -> %d after %llu ms%s", decoder->index,
              frames, copied != NULL, result, (unsigned long long)(spent / 1000000),
              decoder->failed ? "; guest is not acknowledging, bridge stops for this session" : "");
    } else decoder->emitFailures = 0;
    if (frames <= 2 || frames % 30 == 0)
        logf_("[VTBridge] session #%u frames=%u copy avg=%llu us emit avg=%llu us slowest=%llu ms",
              decoder->index, frames,
              (unsigned long long)(decoder->copyNanoseconds / frames / 1000),
              (unsigned long long)(decoder->emitNanoseconds / frames / 1000),
              (unsigned long long)(decoder->slowestEmit / 1000000));
    if (copied) CFRelease(copied);
    // In a paravirtualized session the destination buffers belong to the
    // guest, which allocates one for every VTDecoderSessionCreatePixelBuffer.
    // The session tells the guest which ones the host has let go of only from
    // here, so a decoder that never calls this keeps every buffer of the
    // session alive in the guest until teardown, and then sends one release
    // batch large enough to overflow the guest's receive buffer.
    if (vtBridge.cleanUpAfterDecode) vtBridge.cleanUpAfterDecode(decoder->hostSession);
}

static CFDictionaryRef vt_bridge_pixel_attributes(void *formatDescription) {
    CFStringRef *formatKey = dlsym(RTLD_DEFAULT, "kCVPixelBufferPixelFormatTypeKey");
    CFStringRef *surfaceKey = dlsym(RTLD_DEFAULT, "kCVPixelBufferIOSurfacePropertiesKey");
    if (!formatKey || !surfaceKey) return NULL;
    // Keep 10-bit content 10-bit; everything else decodes to 8-bit 4:2:0.
    int32_t format = '420v', depth = 8;
    CFTypeRef (*extension)(void *, CFStringRef) = dlsym(RTLD_DEFAULT, "CMFormatDescriptionGetExtension");
    CFTypeRef bits = extension ? extension(formatDescription, CFSTR("BitsPerComponent")) : NULL;
    if (bits && CFGetTypeID(bits) == CFNumberGetTypeID() &&
        CFNumberGetValue(bits, kCFNumberSInt32Type, &depth) && depth == 10) format = 'x420';
    CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt32Type, &format);
    // The decoded surface reaches the guest through another process. iPadOS
    // only lets that process look the surface up when it is registered
    // globally, as vmm_IOSurfaceCreate arranges for PVG's own surfaces. The
    // decoder's pool does not go through that hook.
    const void *surfaceKeys[] = {CFSTR("IOSurfaceIsGlobal")}, *surfaceValues[] = {kCFBooleanTrue};
    CFDictionaryRef surface = CFDictionaryCreate(NULL, surfaceKeys, surfaceValues, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *keys[] = {*formatKey, *surfaceKey}, *values[] = {number, surface};
    CFDictionaryRef attributes = CFDictionaryCreate(NULL, keys, values, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(number);
    CFRelease(surface);
    return attributes;
}

// Create the system session for format and make it the decoder's current one.
static int32_t vt_bridge_open(VTBridgeDecoder *decoder, void *format) {
    const void *keys[] = {CFSTR("RequireHardwareAcceleratedVideoDecoder")};
    const void *values[] = {kCFBooleanTrue};
    CFDictionaryRef specification = CFDictionaryCreate(NULL, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef attributes = vt_bridge_pixel_attributes(format);
    VTBridgeOutputRecord record = {vt_bridge_output, decoder};
    void *created = NULL;
    int32_t status = vtBridge.sessionCreate(NULL, format, specification, attributes, &record, &created);
    if (!status && created) {
        decoder->systemSession = created;
        CFRetain(format);
        if (decoder->format) CFRelease(decoder->format);
        decoder->format = format;
        if (attributes && vtBridge.setPixelBufferAttributes)
            vtBridge.setPixelBufferAttributes(decoder->hostSession, attributes);
    }
    if (attributes) CFRelease(attributes);
    CFRelease(specification);
    return status;
}

// Streams may carry a new format description on any sample, as Safari's media
// source playback does for HEVC from the first frame. A session refuses a
// format it cannot accept with kVTFormatDescriptionChangeNotSupportedErr,
// which the guest only sees as a broken decoder. Move to a session for the new
// format once the current one has delivered everything it holds.
static int32_t vt_bridge_follow_format(VTBridgeDecoder *decoder, void *sampleBuffer) {
    static void *(*formatOf)(void *);
    static Boolean (*equal)(void *, void *);
    if (!formatOf) {
        formatOf = dlsym(RTLD_DEFAULT, "CMSampleBufferGetFormatDescription");
        equal = dlsym(RTLD_DEFAULT, "CMFormatDescriptionEqual");
    }
    void *format = formatOf ? formatOf(sampleBuffer) : NULL;
    if (!format || format == decoder->format || !equal ||
        (decoder->format && equal(format, decoder->format))) return 0;
    unsigned change = ++decoder->formatChanges;
    if (vtBridge.sessionCanAccept && vtBridge.sessionCanAccept(decoder->systemSession, format)) {
        if (change <= 4)
            logf_("[VTBridge] session #%u format change %u accepted by the system session",
                  decoder->index, change);
        CFRetain(format);
        if (decoder->format) CFRelease(decoder->format);
        decoder->format = format;
        return 0;
    }
    if (change <= 2) {
        vt_bridge_describe("format before change", decoder->format);
        vt_bridge_describe("format after change", format);
    }
    void *previous = decoder->systemSession;
    vtBridge.sessionFinish(previous);
    vtBridge.sessionWait(previous);
    decoder->systemSession = NULL;
    int32_t status = vt_bridge_open(decoder, format);
    if (status) decoder->systemSession = previous;
    else {
        vtBridge.sessionInvalidate(previous);
        CFRelease(previous);
    }
    logf_("[VTBridge] session #%u format change %u needs a new system session -> %d",
          decoder->index, change, status);
    return status;
}

static int32_t vt_bridge_start(void *object, void *session, void *format) {
    VTBridgeDecoder *decoder = vtBridge.derivedStorage(object);
    if (!decoder) return -12902;
    decoder->hostSession = session;
    uint64_t (*dimensionsOf)(void *) = dlsym(RTLD_DEFAULT, "CMVideoFormatDescriptionGetDimensions");
    uint64_t packed = dimensionsOf ? dimensionsOf(format) : 0;
    uint64_t pixels = (uint64_t)(uint32_t)packed * (uint32_t)(packed >> 32);
    // A refused start lets VideoToolbox fall back to the guest's software decoder.
    if (!pixels) return -12910;
    decoder->copy = access(vtBridgeDirectFlag, F_OK) != 0;
    if (vtBridge.destinationAttributes)
        vt_bridge_describe("host session destination attributes",
                           vtBridge.destinationAttributes(session));
    int32_t status = vt_bridge_open(decoder, format);
    decoder->index = __sync_add_and_fetch(&vtBridgeSessions, 1);
    if (!status) vt_profile_session_started(pixels);
    uint32_t (*subType)(void *) = dlsym(RTLD_DEFAULT, "CMFormatDescriptionGetMediaSubType");
    uint64_t (*dimensions)(void *) = dlsym(RTLD_DEFAULT, "CMVideoFormatDescriptionGetDimensions");
    uint32_t codec = subType ? subType(format) : 0;
    uint64_t size = dimensions ? dimensions(format) : 0;
    logf_("[VTBridge] session #%u start %c%c%c%c %ux%u status=%d copy=%d", decoder->index,
          codec >> 24, codec >> 16, codec >> 8, codec, (uint32_t)size, (uint32_t)(size >> 32),
          status, decoder->copy);
    return status;
}

static int32_t vt_bridge_decode(void *object, void *frame, void *sampleBuffer, uint32_t flags,
                                uint32_t *infoFlagsOut) {
    VTBridgeDecoder *decoder = vtBridge.derivedStorage(object);
    if (!decoder || !decoder->systemSession) return -12902;
    if (decoder->failed) return -12911;
    int32_t changed = vt_bridge_follow_format(decoder, sampleBuffer);
    if (changed) return changed;
    // The frame token travels as the system session's per-frame refcon and
    // comes back in vt_bridge_output, which emits it exactly once.
    return vtBridge.sessionDecode(decoder->systemSession, sampleBuffer, flags, frame, infoFlagsOut);
}

static int32_t vt_bridge_finish(void *object) {
    VTBridgeDecoder *decoder = vtBridge.derivedStorage(object);
    if (!decoder || !decoder->systemSession) return 0;
    int32_t status = vtBridge.sessionFinish(decoder->systemSession);
    int32_t waited = vtBridge.sessionWait(decoder->systemSession);
    logf_("[VTBridge] finish delayed frames system=%p status=%d wait=%d frames=%u",
          decoder->systemSession, status, waited, vtBridgeFrames);
    return status ? status : waited;
}

// All ten built-in decoders implement this; this one has no settable properties.
static int32_t vt_bridge_supported_properties(void *object, CFDictionaryRef *out) {
    (void)object;
    if (!out) return -12902;
    *out = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
    return *out ? 0 : -12904;
}

static int32_t vt_bridge_invalidate(void *object) {
    VTBridgeDecoder *decoder = vtBridge.derivedStorage(object);
    if (decoder && decoder->systemSession) {
        logf_("[VTBridge] session #%u invalidate frames=%u failed=%d", decoder->index,
              decoder->frames, decoder->failed);
        __atomic_store_n(&decoder->invalidating, YES, __ATOMIC_RELEASE);
        // Blocks until pending output callbacks have run.
        vtBridge.sessionInvalidate(decoder->systemSession);
        CFRelease(decoder->systemSession);
        decoder->systemSession = NULL;
    }
    if (decoder && decoder->format) {
        CFRelease(decoder->format);
        decoder->format = NULL;
    }
    return 0;
}
static void vt_bridge_finalize(void *object) { vt_bridge_invalidate(object); }
static CFStringRef vt_bridge_description(void *object) {
    (void)object;
    return CFSTR("VirtualMac iPadOS hardware decoder bridge");
}

static const VTBridgeBaseClass vtBridgeBaseClass = {
    1, sizeof(VTBridgeDecoder), NULL, vt_bridge_invalidate, vt_bridge_finalize,
    vt_bridge_description, NULL, NULL, NULL, NULL,
};
static const VTBridgeDecoderClass vtBridgeDecoderClass = {
    1, vt_bridge_start, vt_bridge_decode, vt_bridge_supported_properties, NULL, NULL, NULL,
    vt_bridge_finish, NULL, NULL, NULL,
};
static const VTBridgeVTable vtBridgeVTable = {{NULL, &vtBridgeBaseClass}, &vtBridgeDecoderClass};

static int32_t vt_bridge_create(uint32_t codec, CFAllocatorRef allocator, void **decoderOut) {
    (void)codec;
    if (!decoderOut) return -12902;
    void *decoder = NULL;
    int32_t status = vtBridge.derivedObjectCreate(allocator, &vtBridgeVTable,
                                                  vtBridge.decoderClassID(), &decoder);
    if (status && decoder) { CFRelease(decoder); decoder = NULL; }
    *decoderOut = decoder;
    return decoder ? status : -12904;
}

static void videotoolbox_bridge_install(void) {
    if (access(vtBridgeFlag, F_OK)) return;
    const char *bundledPath = NULL;
    static const char suffix[] = "/payload/Frameworks/VideoToolbox.framework/VideoToolbox";
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const char *name = _dyld_get_image_name(i);
        size_t length = name ? strlen(name) : 0;
        if (length >= sizeof(suffix) - 1 && !strcmp(name + length - (sizeof(suffix) - 1), suffix))
            bundledPath = name;
    }
    void *bundled = bundledPath ? dlopen(bundledPath, RTLD_NOW | RTLD_NOLOAD) : NULL;
    void *system = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox",
                          RTLD_NOW);
    if (!bundled || !system) {
        logf_("[VTBridge] not installed: bundled=%p system=%p", bundled, system);
        return;
    }
    vtBridge.registerDecoder = dlsym(bundled, "VTRegisterVideoDecoderWithInfo");
    vtBridge.decoderClassID = dlsym(bundled, "VTVideoDecoderGetClassID");
    vtBridge.emitFrame = dlsym(bundled, "VTDecoderSessionEmitDecodedFrame");
    vtBridge.setPixelBufferAttributes = dlsym(bundled, "VTDecoderSessionSetPixelBufferAttributes");
    vtBridge.destinationAttributes = dlsym(bundled, "VTDecoderSessionGetDestinationPixelBufferAttributes");
    vtBridge.createPixelBuffer = dlsym(bundled, "VTDecoderSessionCreatePixelBuffer");
    vtBridge.cleanUpAfterDecode = dlsym(bundled, "VTDecoderSessionCleanUpAfterDecode");
    vtBridge.derivedObjectCreate = dlsym(RTLD_DEFAULT, "CMDerivedObjectCreate");
    vtBridge.derivedStorage = dlsym(RTLD_DEFAULT, "CMBaseObjectGetDerivedStorage");
    vtBridge.sessionCreate = dlsym(system, "VTDecompressionSessionCreate");
    vtBridge.sessionDecode = dlsym(system, "VTDecompressionSessionDecodeFrame");
    vtBridge.sessionFinish = dlsym(system, "VTDecompressionSessionFinishDelayedFrames");
    vtBridge.sessionWait = dlsym(system, "VTDecompressionSessionWaitForAsynchronousFrames");
    vtBridge.sessionInvalidate = dlsym(system, "VTDecompressionSessionInvalidate");
    vtBridge.sessionCanAccept = dlsym(system, "VTDecompressionSessionCanAcceptFormatDescription");
    // The host session offers the guest only decoders whose registration says
    // hardware accelerated and paravirtualization aware, and not a wrapper.
    CFStringRef *hardwareKey = dlsym(bundled, "kVTVideoDecoder_IsHardwareAccelerated");
    CFStringRef *awareKey = dlsym(bundled, "kVTVideoDecoder_IsParavirtualizationAware");
    // It also forwards two things the guest uses to choose a decoder. A
    // registration without a rating gets 0, below the guest's software decoder,
    // so an application that merely allows hardware never picks this one. The
    // capabilities are read from the IORegistry property named here; iPadOS's
    // AppleAVD publishes it under the same name as macOS.
    CFStringRef *ratingKey = dlsym(bundled, "kVTVideoDecoderInfo_Rating");
    CFStringRef *capabilitiesKey = dlsym(bundled, "kVTVideoDecoderInfo_IORegistryCapabilitiesKey");
    if (!vtBridge.registerDecoder || !vtBridge.decoderClassID || !vtBridge.emitFrame ||
        !vtBridge.derivedObjectCreate || !vtBridge.derivedStorage || !vtBridge.sessionCreate ||
        !vtBridge.sessionDecode || !vtBridge.sessionFinish || !vtBridge.sessionWait ||
        !vtBridge.sessionInvalidate || !hardwareKey || !*hardwareKey || !awareKey || !*awareKey ||
        !ratingKey || !*ratingKey || !capabilitiesKey || !*capabilitiesKey ||
        (void *)vtBridge.sessionCreate == dlsym(bundled, "VTDecompressionSessionCreate")) {
        logf_("[VTBridge] not installed: a required symbol is missing or not distinct");
        return;
    }
    // The codecs iPadOS 16 decodes in hardware on this class of device. VP9 is
    // reported only to a process entitled to select alternate video decoders,
    // and, as with macOS's own VP9 decoder, has no capabilities property in
    // the IORegistry. There is no AV1 decoder.
    static const struct { uint32_t codec; CFStringRef capabilities; } codecs[] = {
        {'avc1', CFSTR("IOAVDH264DecodeCapabilities")},
        {'hvc1', CFSTR("IOAVDHEVCDecodeCapabilities")},
        {'vp09', NULL},
    };
    Boolean (*supported)(uint32_t) = dlsym(system, "VTIsHardwareDecodeSupported");
    int32_t rating = 1000000;
    CFNumberRef ratingValue = CFNumberCreate(NULL, kCFNumberSInt32Type, &rating);
    for (unsigned i = 0; i < sizeof(codecs) / sizeof(codecs[0]); ++i) {
        uint32_t codec = codecs[i].codec;
        if (!supported || !supported(codec)) {
            logf_("[VTBridge] %c%c%c%c not registered: no system hardware decoder",
                  codec >> 24, codec >> 16, codec >> 8, codec);
            continue;
        }
        const void *keys[] = {*hardwareKey, *awareKey, *ratingKey, *capabilitiesKey};
        const void *values[] = {kCFBooleanTrue, kCFBooleanTrue, ratingValue, codecs[i].capabilities};
        CFDictionaryRef information = CFDictionaryCreate(NULL, keys, values,
            codecs[i].capabilities ? 4 : 3,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        int32_t status = vtBridge.registerDecoder(codec, information, vt_bridge_create);
        CFRelease(information);
        logf_("[VTBridge] registered %c%c%c%c hardware bridge status=%d",
              codec >> 24, codec >> 16, codec >> 8, codec, status);
    }
    CFRelease(ratingValue);
}
