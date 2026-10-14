// Hardware video encoding for the paravirtualized VideoToolbox device.
//
// The counterpart of the decoder bridge in videotoolbox_bridge.inc: register
// H.264 and HEVC encoders with the bundled Ventura VideoToolbox, which serves
// the guest's encode sessions, and forward every frame to a hardware
// compression session of iPadOS's own VideoToolbox. The encoder plug-in class
// layout below was read from the bundled framework's ten built-in encoders
// and the call sites that use them.
//
// On by default; creating
// /var/root/VirtualMac2/diagnostics/videotoolbox-encoder-bridge-disabled turns
// it off from the next VM start, leaving the guest with its software encoders.
static const char vtEncBridgeDisabledFlag[] =
    "/var/root/VirtualMac2/diagnostics/videotoolbox-encoder-bridge-disabled";

typedef struct {
    uintptr_t version;
    // The codec type is not passed again; the factory was told it.
    int32_t (*VT_BRIDGE_CODE(0x6411) startSession)(void *encoder, void *session, uint64_t dimensions,
                                                   void *reserved);
    int32_t (*VT_BRIDGE_CODE(0x89F7) encodeFrame)(void *encoder, void *frame, void *imageBuffer,
                                                  VTBridgeTime presentation, VTBridgeTime duration,
                                                  CFDictionaryRef frameProperties, uint32_t *infoFlagsOut);
    int32_t (*VT_BRIDGE_CODE(0x434E) completeFrames)(void *encoder, VTBridgeTime until);
    int32_t (*VT_BRIDGE_CODE(0x78FE) copySupportedPropertyDictionary)(void *encoder, CFDictionaryRef *out);
    void *slot5, *slot6;
    int32_t (*VT_BRIDGE_CODE(0x3929) prepareToEncodeFrames)(void *encoder);
    void *slots8to20[13];
} VTEncBridgeEncoderClass;
typedef struct {
    struct {
        const void *reserved;
        const VTBridgeBaseClass *VT_BRIDGE_DATA(0xB911) baseClass;
    } base;
    const VTEncBridgeEncoderClass *VT_BRIDGE_DATA(0x076B) encoderClass;
} VTEncBridgeVTable;

typedef struct {
    void *systemSession, *hostSession;
    uint32_t codec;
    int32_t width, height;
    unsigned index, frames, errors;
    BOOL invalidating;
} VTEncBridgeEncoder;

static struct {
    // Bundled framework.
    int32_t (*registerEncoder)(uint32_t, CFDictionaryRef, int32_t (*)(uint32_t, CFAllocatorRef, void **));
    void *(*encoderClassID)(void);
    int32_t (*emitFrame)(void *session, void *frame, int32_t status, uint32_t infoFlags, void *sampleBuffer);
    int32_t (*setPixelBufferAttributes)(void *session, CFDictionaryRef);
    // CoreMedia.
    int32_t (*derivedObjectCreate)(CFAllocatorRef, const void *vtable, void *classID, void **);
    void *(*derivedStorage)(void *);
    // System framework.
    int32_t (*sessionCreate)(CFAllocatorRef, int32_t, int32_t, uint32_t, CFDictionaryRef, CFDictionaryRef,
                             CFAllocatorRef, void (*)(void *, void *, int32_t, uint32_t, void *), void *, void **);
    int32_t (*sessionEncode)(void *, void *imageBuffer, VTBridgeTime, VTBridgeTime, CFDictionaryRef, void *,
                             uint32_t *);
    int32_t (*sessionComplete)(void *, VTBridgeTime);
    int32_t (*sessionPrepare)(void *);
    void (*sessionInvalidate)(void *);
    int32_t (*setProperty)(void *, CFStringRef, CFTypeRef);
    int32_t (*copyProperty)(void *, CFStringRef, CFAllocatorRef, void *);
    int32_t (*copySupported)(void *, CFDictionaryRef *);
} vtEnc;
static unsigned vtEncSessions;

// Encoded output goes back to the guest under the frame token it came with.
static void vt_enc_output(void *refcon, void *frame, int32_t status, uint32_t infoFlags, void *sampleBuffer) {
    VTEncBridgeEncoder *encoder = refcon;
    if (status && ++encoder->errors <= 8)
        logf_("[VTEncBridge] session #%u system encoder returned status=%d", encoder->index, status);
    if (__atomic_load_n(&encoder->invalidating, __ATOMIC_ACQUIRE)) {
        vtEnc.emitFrame(encoder->hostSession, frame, status ? status : -12903, infoFlags | 2, NULL);
        return;
    }
    ++encoder->frames;
    int32_t result = vtEnc.emitFrame(encoder->hostSession, frame, status, infoFlags, sampleBuffer);
    if (result && ++encoder->errors <= 8)
        logf_("[VTEncBridge] session #%u emit frame %u -> %d", encoder->index, encoder->frames, result);
}

static int32_t vt_enc_start(void *object, void *session, uint64_t dimensions, void *reserved) {
    (void)reserved;
    VTEncBridgeEncoder *encoder = vtEnc.derivedStorage(object);
    if (!encoder) return -12902;
    uint32_t codec = encoder->codec;
    encoder->hostSession = session;
    encoder->width = (int32_t)(uint32_t)dimensions;
    encoder->height = (int32_t)(dimensions >> 32);
    encoder->index = __sync_add_and_fetch(&vtEncSessions, 1);
    const void *keys[] = {CFSTR("RequireHardwareAcceleratedVideoEncoder")};
    const void *values[] = {kCFBooleanTrue};
    CFDictionaryRef specification = CFDictionaryCreate(NULL, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int32_t status = vtEnc.sessionCreate(NULL, encoder->width, encoder->height, codec, specification, NULL,
                                         NULL, vt_enc_output, encoder, &encoder->systemSession);
    CFRelease(specification);
    // Tell the guest-facing session which images the hardware encoder takes.
    CFDictionaryRef attributes = NULL;
    if (!status && vtEnc.copyProperty && vtEnc.setPixelBufferAttributes &&
        !vtEnc.copyProperty(encoder->systemSession, CFSTR("VideoEncoderPixelBufferAttributes"), NULL,
                            &attributes) && attributes) {
        vtEnc.setPixelBufferAttributes(session, attributes);
        CFRelease(attributes);
    }
    logf_("[VTEncBridge] session #%u start %c%c%c%c %dx%d status=%d", encoder->index, codec >> 24,
          codec >> 16, codec >> 8, codec, encoder->width, encoder->height, status);
    return status;
}

static int32_t vt_enc_encode(void *object, void *frame, void *imageBuffer, VTBridgeTime presentation,
                             VTBridgeTime duration, CFDictionaryRef frameProperties, uint32_t *infoFlagsOut) {
    VTEncBridgeEncoder *encoder = vtEnc.derivedStorage(object);
    if (!encoder || !encoder->systemSession) return -12902;
    // The frame token travels as the system session's source frame refcon
    // and comes back in vt_enc_output, which emits it exactly once.
    return vtEnc.sessionEncode(encoder->systemSession, imageBuffer, presentation, duration,
                               frameProperties, frame, infoFlagsOut);
}

static int32_t vt_enc_complete(void *object, VTBridgeTime until) {
    VTEncBridgeEncoder *encoder = vtEnc.derivedStorage(object);
    if (!encoder || !encoder->systemSession) return 0;
    return vtEnc.sessionComplete(encoder->systemSession, until);
}

static int32_t vt_enc_prepare(void *object) {
    VTEncBridgeEncoder *encoder = vtEnc.derivedStorage(object);
    if (!encoder || !encoder->systemSession || !vtEnc.sessionPrepare) return 0;
    return vtEnc.sessionPrepare(encoder->systemSession);
}

// Properties are the system session's: rate control, profile, key frame
// interval and the rest are applied by the hardware encoder itself.
static int32_t vt_enc_supported_properties(void *object, CFDictionaryRef *out) {
    VTEncBridgeEncoder *encoder = vtEnc.derivedStorage(object);
    if (!out) return -12902;
    *out = NULL;
    if (encoder && encoder->systemSession && vtEnc.copySupported &&
        !vtEnc.copySupported(encoder->systemSession, out) && *out)
        return 0;
    *out = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
    return *out ? 0 : -12904;
}

static int32_t vt_enc_set_property(void *object, CFStringRef key, CFTypeRef value) {
    VTEncBridgeEncoder *encoder = vtEnc.derivedStorage(object);
    if (!encoder || !encoder->systemSession) return -12900;
    return vtEnc.setProperty(encoder->systemSession, key, value);
}

static int32_t vt_enc_copy_property(void *object, CFStringRef key, CFAllocatorRef allocator, void *out) {
    VTEncBridgeEncoder *encoder = vtEnc.derivedStorage(object);
    if (!encoder || !encoder->systemSession) return -12900;
    return vtEnc.copyProperty(encoder->systemSession, key, allocator, out);
}

static int32_t vt_enc_invalidate(void *object) {
    VTEncBridgeEncoder *encoder = vtEnc.derivedStorage(object);
    if (encoder && encoder->systemSession) {
        logf_("[VTEncBridge] session #%u invalidate frames=%u errors=%u", encoder->index, encoder->frames,
              encoder->errors);
        __atomic_store_n(&encoder->invalidating, YES, __ATOMIC_RELEASE);
        // Blocks until pending output callbacks have run.
        vtEnc.sessionInvalidate(encoder->systemSession);
        CFRelease(encoder->systemSession);
        encoder->systemSession = NULL;
    }
    return 0;
}
static void vt_enc_finalize(void *object) { vt_enc_invalidate(object); }
static CFStringRef vt_enc_description(void *object) {
    (void)object;
    return CFSTR("VirtualMac iPadOS hardware encoder bridge");
}

static const VTBridgeBaseClass vtEncBridgeBaseClass = {
    1, sizeof(VTEncBridgeEncoder), NULL, vt_enc_invalidate, vt_enc_finalize,
    vt_enc_description, vt_enc_copy_property, vt_enc_set_property, NULL, NULL,
};
static const VTEncBridgeEncoderClass vtEncBridgeEncoderClass = {
    1, vt_enc_start, vt_enc_encode, vt_enc_complete, vt_enc_supported_properties, NULL, NULL,
    vt_enc_prepare, {NULL},
};
static const VTEncBridgeVTable vtEncBridgeVTable = {{NULL, &vtEncBridgeBaseClass}, &vtEncBridgeEncoderClass};

static int32_t vt_enc_create(uint32_t codec, CFAllocatorRef allocator, void **encoderOut) {
    if (!encoderOut) return -12902;
    void *encoder = NULL;
    int32_t status = vtEnc.derivedObjectCreate(allocator, &vtEncBridgeVTable, vtEnc.encoderClassID(), &encoder);
    if (status && encoder) { CFRelease(encoder); encoder = NULL; }
    VTEncBridgeEncoder *storage = encoder ? vtEnc.derivedStorage(encoder) : NULL;
    if (storage) storage->codec = codec;
    *encoderOut = encoder;
    return encoder ? status : -12904;
}

static void videotoolbox_encoder_bridge_install(void) {
    if (!access(vtEncBridgeDisabledFlag, F_OK)) {
        logf_("[VTEncBridge] disabled by %s", vtEncBridgeDisabledFlag);
        return;
    }
    const char *bundledPath = NULL;
    static const char suffix[] = "/payload/Frameworks/VideoToolbox.framework/VideoToolbox";
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const char *name = _dyld_get_image_name(i);
        size_t length = name ? strlen(name) : 0;
        if (length >= sizeof(suffix) - 1 && !strcmp(name + length - (sizeof(suffix) - 1), suffix))
            bundledPath = name;
    }
    void *bundled = bundledPath ? dlopen(bundledPath, RTLD_NOW | RTLD_NOLOAD) : NULL;
    void *system = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW);
    if (!bundled || !system) {
        logf_("[VTEncBridge] not installed: bundled=%p system=%p", bundled, system);
        return;
    }
    vtEnc.registerEncoder = dlsym(bundled, "VTRegisterVideoEncoderWithInfo");
    vtEnc.encoderClassID = dlsym(bundled, "VTVideoEncoderGetClassID");
    vtEnc.emitFrame = dlsym(bundled, "VTEncoderSessionEmitEncodedFrame");
    vtEnc.setPixelBufferAttributes = dlsym(bundled, "VTEncoderSessionSetPixelBufferAttributes");
    vtEnc.derivedObjectCreate = dlsym(RTLD_DEFAULT, "CMDerivedObjectCreate");
    vtEnc.derivedStorage = dlsym(RTLD_DEFAULT, "CMBaseObjectGetDerivedStorage");
    vtEnc.sessionCreate = dlsym(system, "VTCompressionSessionCreate");
    vtEnc.sessionEncode = dlsym(system, "VTCompressionSessionEncodeFrame");
    vtEnc.sessionComplete = dlsym(system, "VTCompressionSessionCompleteFrames");
    vtEnc.sessionPrepare = dlsym(system, "VTCompressionSessionPrepareToEncodeFrames");
    vtEnc.sessionInvalidate = dlsym(system, "VTCompressionSessionInvalidate");
    vtEnc.setProperty = dlsym(system, "VTSessionSetProperty");
    vtEnc.copyProperty = dlsym(system, "VTSessionCopyProperty");
    vtEnc.copySupported = dlsym(system, "VTSessionCopySupportedPropertyDictionary");
    CFStringRef *hardwareKey = dlsym(bundled, "kVTVideoEncoder_IsHardwareAccelerated");
    CFStringRef *awareKey = dlsym(bundled, "kVTVideoEncoder_IsParavirtualizationAware");
    CFStringRef *ratingKey = dlsym(bundled, "kVTVideoEncoder_Rating");
    if (!vtEnc.registerEncoder || !vtEnc.encoderClassID || !vtEnc.emitFrame || !vtEnc.derivedObjectCreate ||
        !vtEnc.derivedStorage || !vtEnc.sessionCreate || !vtEnc.sessionEncode || !vtEnc.sessionComplete ||
        !vtEnc.sessionInvalidate || !vtEnc.setProperty || !vtEnc.copyProperty || !hardwareKey ||
        !*hardwareKey || !awareKey || !*awareKey || !ratingKey || !*ratingKey ||
        (void *)vtEnc.sessionCreate == dlsym(bundled, "VTCompressionSessionCreate")) {
        logf_("[VTEncBridge] not installed: a required symbol is missing or not distinct");
        return;
    }
    static const uint32_t codecs[] = {'avc1', 'hvc1'};
    int32_t rating = 1000000;
    CFNumberRef ratingValue = CFNumberCreate(NULL, kCFNumberSInt32Type, &rating);
    for (unsigned i = 0; i < sizeof(codecs) / sizeof(codecs[0]); ++i) {
        const void *keys[] = {*hardwareKey, *awareKey, *ratingKey};
        const void *values[] = {kCFBooleanTrue, kCFBooleanTrue, ratingValue};
        CFDictionaryRef information = CFDictionaryCreate(NULL, keys, values, 3,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        int32_t status = vtEnc.registerEncoder(codecs[i], information, vt_enc_create);
        CFRelease(information);
        logf_("[VTEncBridge] registered %c%c%c%c hardware encoder bridge status=%d", codecs[i] >> 24,
              codecs[i] >> 16, codecs[i] >> 8, codecs[i], status);
    }
    CFRelease(ratingValue);
}
