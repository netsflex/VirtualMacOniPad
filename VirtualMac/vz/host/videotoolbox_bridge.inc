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
// On by default; creating
// /var/root/VirtualMac2/diagnostics/videotoolbox-bridge-disabled turns it off
// from the next VM start, leaving the guest with its software decoders.

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

static const char vtBridgeDisabledFlag[] = "/var/root/VirtualMac2/diagnostics/videotoolbox-bridge-disabled";


// CMBaseObject and VTVideoDecoder class layouts (CoreMedia/VideoToolbox SPI).
// On arm64e every member is signed with its own address-diversified
// discriminator. The values are the ones the bundled framework's ten built-in
// decoder classes carry in their fixups and that its call sites authenticate
// against. Members no built-in class sets stay NULL. The tile members come from
// the guest-side paravirtualized decoder's class, the one built-in class that
// sets all three; VTTileDecompressionSession only calls them in a class of
// version 3 or later and otherwise fails with -12782.
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
typedef struct { int32_t x, y; } VTBridgePoint;
typedef struct { int32_t width, height; } VTBridgeSize;
typedef struct {
    uintptr_t version;
    int32_t (*VT_BRIDGE_CODE(0x7BE9) startSession)(void *decoder, void *session, void *format);
    int32_t (*VT_BRIDGE_CODE(0xAF83) decodeFrame)(void *decoder, void *frame, void *sampleBuffer,
                                                  uint32_t flags, uint32_t *infoFlagsOut);
    int32_t (*VT_BRIDGE_CODE(0x50EB) copySupportedPropertyDictionary)(void *decoder,
                                                                      CFDictionaryRef *out);
    void *setProperties, *copySerializableProperties, *canAcceptFormatDescription;
    int32_t (*VT_BRIDGE_CODE(0xBFC3) finishDelayedFrames)(void *decoder);
    // Version 3. The tile is cropped out of the decoded sample at cropOffset
    // with cropSize and written into the canvas at canvasOffset.
    int32_t (*VT_BRIDGE_CODE(0x9605) startTileSession)(void *decoder, void *tileSession, void *format);
    int32_t (*VT_BRIDGE_CODE(0xBA86) decodeTile)(void *decoder, int64_t tile, void *sampleBuffer,
                                                 VTBridgePoint cropOffset, VTBridgeSize cropSize,
                                                 void *canvas, VTBridgePoint canvasOffset,
                                                 uint32_t flags, uint32_t *infoFlagsOut);
    int32_t (*VT_BRIDGE_CODE(0xECDB) finishDelayedTiles)(void *decoder);
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
    // When the system session was created with a copy of format instead, that
    // copy. Samples are moved onto it to decode.
    void *sessionFormat;
    BOOL invalidating, failed;
    unsigned index, frames, emitFailures, copyFailures, formatChanges;
    // A decoder started as a tile session instead (HEIC). hostTileSession is
    // the bundled framework's tile decoder session.
    void *systemTileSession, *hostTileSession;
    unsigned tiles, tileErrors;
} VTBridgeDecoder;
typedef void (*VTBridgeTileOutput)(void *refcon, void *tile, VTBridgePoint cropOffset, VTBridgeSize cropSize,
                                   void *canvas, VTBridgePoint canvasOffset, int32_t status,
                                   uint32_t infoFlags);

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
    // Tile decoding, all NULL when either framework lacks a part of it.
    int32_t (*emitTile)(void *tileSession, int64_t tile, int32_t status, uint32_t infoFlags, void *canvas);
    void (*cleanUpAfterTile)(void *tileSession);
    int32_t (*setTileRequirements)(void *tileSession, CFDictionaryRef canvasAttributes,
                                   CFDictionaryRef requirements);
    int32_t (*tileSessionCreate)(CFAllocatorRef, void *format, CFDictionaryRef, VTBridgeTileOutput,
                                 void *refcon, void **);
    int32_t (*tileSessionDecode)(void *, void *sampleBuffer, VTBridgePoint, VTBridgeSize, void *canvas,
                                 VTBridgePoint, uint32_t flags, void *tile, uint32_t *);
    int32_t (*tileSessionFinish)(void *);
    void (*tileSessionInvalidate)(void *);
    int32_t (*tileSessionCopyProperty)(void *, CFStringRef, CFAllocatorRef, void *);
    CFStringRef canvasAttributesKey, tileRequirementsKey;
} vtBridge;
static unsigned vtBridgeSessions, vtBridgeErrors, vtBridgeTileSessions;

// Copy the decoded planes into a buffer from the host session's pool: handing
// back the decoder's own buffer reaches the guest as an empty image.
static void *vt_bridge_copy(VTBridgeDecoder *decoder, void *source) {
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
    if (status && __sync_add_and_fetch(&vtBridgeErrors, 1) <= 16)
        logf_("[VTBridge] session #%u system decoder returned status=%d", decoder->index, status);

    // Invalidation drains the system session. The guest is tearing the decoder
    // down and no longer wants these images; report them as dropped instead of
    // sending full frame messages into its shutdown.
    if (__atomic_load_n(&decoder->invalidating, __ATOMIC_ACQUIRE)) {
        vtBridge.emitFrame(decoder->hostSession, frame, status ? status : -12903, infoFlags, NULL);
        return;
    }
    // After the guest stopped acknowledging frames, each further image would
    // wait out the host session's one second reply timeout. Report an error.
    if (decoder->failed) {
        vtBridge.emitFrame(decoder->hostSession, frame, status ? status : -12911, infoFlags, NULL);
        return;
    }
    // Images the player decodes but does not display arrive without a buffer
    // and are passed on as they are.
    void *copied = imageBuffer && !status ? vt_bridge_copy(decoder, imageBuffer) : NULL;
    int32_t result = vtBridge.emitFrame(decoder->hostSession, frame, status, infoFlags,
                                        copied ? copied : imageBuffer);
    ++decoder->frames;
    if (result) {
        if (++decoder->emitFailures >= 2) decoder->failed = YES;
        logf_("[VTBridge] session #%u emit frame %u -> %d%s", decoder->index, decoder->frames, result,
              decoder->failed ? "; guest is not acknowledging, bridge stops for this session" : "");
    } else decoder->emitFailures = 0;
    if (copied) CFRelease(copied);
    // In a paravirtualized session the destination buffers belong to the
    // guest, which allocates one for every VTDecoderSessionCreatePixelBuffer.
    // The session tells the guest which ones the host has let go of only from
    // here, so a decoder that never calls this keeps every buffer of the
    // session alive in the guest until teardown, and then sends one release
    // batch large enough to overflow the guest's receive buffer.
    if (vtBridge.cleanUpAfterDecode) vtBridge.cleanUpAfterDecode(decoder->hostSession);
}

// The first pixel format the host session's client asked for, or 0.
static int32_t vt_bridge_requested_format(void *hostSession) {
    CFStringRef *formatKey = dlsym(RTLD_DEFAULT, "kCVPixelBufferPixelFormatTypeKey");
    CFDictionaryRef attributes = vtBridge.destinationAttributes && formatKey ?
        vtBridge.destinationAttributes(hostSession) : NULL;
    CFTypeRef value = attributes && CFGetTypeID(attributes) == CFDictionaryGetTypeID() ?
        CFDictionaryGetValue(attributes, *formatKey) : NULL;
    if (value && CFGetTypeID(value) == CFArrayGetTypeID())
        value = CFArrayGetCount(value) ? CFArrayGetValueAtIndex(value, 0) : NULL;
    int32_t format = 0;
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID() ||
        !CFNumberGetValue(value, kCFNumberSInt32Type, &format)) return 0;
    return format;
}

// Destination attributes for the system session: the given pixel format, or
// when it is 0 the one matching the stream, as the guest's software decoder
// would produce it.
static CFDictionaryRef vt_bridge_pixel_attributes(void *formatDescription, int32_t requested) {
    CFStringRef *formatKey = dlsym(RTLD_DEFAULT, "kCVPixelBufferPixelFormatTypeKey");
    CFStringRef *surfaceKey = dlsym(RTLD_DEFAULT, "kCVPixelBufferIOSurfacePropertiesKey");
    if (!formatKey || !surfaceKey) return NULL;
    // Keep 10-bit content 10-bit, full range content full range and
    // transparent content transparent; a narrower format makes VideoToolbox
    // convert, and the conversion rounds.
    int32_t format = requested, depth = 8;
    CFTypeRef (*extension)(void *, CFStringRef) = dlsym(RTLD_DEFAULT, "CMFormatDescriptionGetExtension");
    CFTypeRef bits = extension ? extension(formatDescription, CFSTR("BitsPerComponent")) : NULL;
    CFTypeRef alpha = extension ? extension(formatDescription, CFSTR("ContainsAlphaChannel")) : NULL;
    CFTypeRef full = extension ? extension(formatDescription, CFSTR("FullRangeVideo")) : NULL;
    BOOL fullRange = full == kCFBooleanTrue;
    if (!format) {
        if (bits && CFGetTypeID(bits) == CFNumberGetTypeID() &&
            CFNumberGetValue(bits, kCFNumberSInt32Type, &depth) && depth == 10)
            format = fullRange ? 'xf20' : 'x420';
        else if (alpha == kCFBooleanTrue) format = 'v0a8';
        else format = fullRange ? '420f' : '420v';
    }
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

// Whether an hvcC carries parameter sets for a layer above the base layer.
static Boolean vt_bridge_hvcc_has_upper_layer(CFDataRef hvcc) {
    const uint8_t *bytes = CFDataGetBytePtr(hvcc);
    CFIndex length = CFDataGetLength(hvcc), at = 23;
    if (length < 23) return false;
    for (unsigned array = 0, arrays = bytes[22]; array < arrays && at + 3 <= length; ++array) {
        unsigned units = bytes[at + 1] << 8 | bytes[at + 2];
        at += 3;
        for (unsigned unit = 0; unit < units && at + 2 <= length; ++unit) {
            CFIndex size = bytes[at] << 8 | bytes[at + 1];
            if (size >= 2 && at + 2 + size <= length &&
                ((bytes[at + 2] & 1) << 5 | bytes[at + 3] >> 3)) return true;
            at += 2 + size;
        }
    }
    return false;
}

// The bundled framework decodes HEVC with alpha in its own muxed alpha
// decoder, which splits the stream into a base layer and an alpha layer and
// opens a session of this decoder for each, with an hvcC holding only that
// layer's parameter sets but the original alpha extensions. iPadOS sends a
// description marked as containing alpha to its own alpha decoder, which then
// refuses it for the missing alpha layer. Return a copy without the alpha
// marking for such a single-layer description, NULL for any other.
static void *vt_bridge_single_layer_format(void *format) {
    CFDictionaryRef (*extensionsOf)(void *) = dlsym(RTLD_DEFAULT, "CMFormatDescriptionGetExtensions");
    uint32_t (*subType)(void *) = dlsym(RTLD_DEFAULT, "CMFormatDescriptionGetMediaSubType");
    uint64_t (*dimensions)(void *) = dlsym(RTLD_DEFAULT, "CMVideoFormatDescriptionGetDimensions");
    int32_t (*create)(CFAllocatorRef, uint32_t, int32_t, int32_t, CFDictionaryRef, void **) =
        dlsym(RTLD_DEFAULT, "CMVideoFormatDescriptionCreate");
    CFDictionaryRef extensions = extensionsOf ? extensionsOf(format) : NULL;
    if (!extensions || !subType || !dimensions || !create || subType(format) != 'hvc1') return NULL;
    CFDictionaryRef atoms = CFDictionaryGetValue(extensions, CFSTR("SampleDescriptionExtensionAtoms"));
    CFDataRef hvcc = atoms && CFGetTypeID(atoms) == CFDictionaryGetTypeID() ?
        CFDictionaryGetValue(atoms, CFSTR("hvcC")) : NULL;
    if (!hvcc || CFGetTypeID(hvcc) != CFDataGetTypeID() || vt_bridge_hvcc_has_upper_layer(hvcc) ||
        (CFDictionaryGetValue(extensions, CFSTR("ContainsAlphaChannel")) != kCFBooleanTrue &&
         !CFDictionaryContainsKey(atoms, CFSTR("almo"))))
        return NULL;
    CFMutableDictionaryRef cleaned = CFDictionaryCreateMutableCopy(NULL, 0, extensions);
    CFMutableDictionaryRef cleanedAtoms = CFDictionaryCreateMutableCopy(NULL, 0, atoms);
    CFDictionaryRemoveValue(cleanedAtoms, CFSTR("almo"));
    CFDictionarySetValue(cleaned, CFSTR("SampleDescriptionExtensionAtoms"), cleanedAtoms);
    CFRelease(cleanedAtoms);
    // The verbatim sample description repeats the alpha marking.
    static const CFStringRef dropped[] = {CFSTR("ContainsAlphaChannel"), CFSTR("AlphaChannelMode"),
                                          CFSTR("VerbatimSampleDescription"), CFSTR("VerbatimISOSampleEntry")};
    for (unsigned i = 0; i < sizeof(dropped) / sizeof(dropped[0]); ++i)
        CFDictionaryRemoveValue(cleaned, dropped[i]);
    void *result = NULL;
    uint64_t size = dimensions(format);
    if (create(NULL, 'hvc1', (int32_t)(uint32_t)size, (int32_t)(size >> 32), cleaned, &result))
        result = NULL;
    CFRelease(cleaned);
    return result;
}

static int32_t vt_bridge_create_session(VTBridgeDecoder *decoder, void *format,
                                        CFDictionaryRef attributes, void **created) {
    const void *keys[] = {CFSTR("RequireHardwareAcceleratedVideoDecoder")};
    const void *values[] = {kCFBooleanTrue};
    CFDictionaryRef specification = CFDictionaryCreate(NULL, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    VTBridgeOutputRecord record = {vt_bridge_output, decoder};
    int32_t status = vtBridge.sessionCreate(NULL, format, specification, attributes, &record, created);
    CFRelease(specification);
    return status;
}

// Create the system session for format and make it the decoder's current one.
static int32_t vt_bridge_open(VTBridgeDecoder *decoder, void *format) {
    void *sessionFormat = vt_bridge_single_layer_format(format);
    // A layer of an alpha stream goes to the bundled muxed alpha decoder,
    // which asks for the pixel format it combines the layers in. Decoding to
    // any other makes the host session convert each image, and the alpha
    // values come out off by one. Fall back to our own choice if the system
    // decoder cannot produce the requested format.
    int32_t requested = sessionFormat ? vt_bridge_requested_format(decoder->hostSession) : 0;
    CFDictionaryRef attributes = vt_bridge_pixel_attributes(sessionFormat ? sessionFormat : format, requested);
    void *created = NULL;
    int32_t status = vt_bridge_create_session(decoder, sessionFormat ? sessionFormat : format,
                                              attributes, &created);
    if (status && requested) {
        if (attributes) CFRelease(attributes);
        attributes = vt_bridge_pixel_attributes(sessionFormat, 0);
        int32_t retried = vt_bridge_create_session(decoder, sessionFormat, attributes, &created);
        logf_("[VTBridge] session #%u requested %c%c%c%c refused (%d); default format -> %d", decoder->index,
              requested >> 24, requested >> 16, requested >> 8, requested, status, retried);
        status = retried;
    }
    if (sessionFormat)
        logf_("[VTBridge] session #%u single layer of an alpha stream requested=%c%c%c%c -> %d",
              decoder->index, requested ? requested >> 24 : '-', requested ? requested >> 16 : '-',
              requested ? requested >> 8 : '-', requested ? requested : '-', status);
    if (!status && created) {
        decoder->systemSession = created;
        CFRetain(format);
        if (decoder->format) CFRelease(decoder->format);
        decoder->format = format;
        if (decoder->sessionFormat) CFRelease(decoder->sessionFormat);
        decoder->sessionFormat = sessionFormat;
        sessionFormat = NULL;
        if (attributes && vtBridge.setPixelBufferAttributes)
            vtBridge.setPixelBufferAttributes(decoder->hostSession, attributes);
    }
    if (sessionFormat) CFRelease(sessionFormat);
    if (attributes) CFRelease(attributes);
    return status;
}

// The same sample on the format the system session was created with.
static void *vt_bridge_rewrap(void *sampleBuffer, void *format) {
    static void *(*dataOf)(void *);
    static long (*samplesOf)(void *);
    static int32_t (*timingOf)(void *, long, void *, long *), (*sizesOf)(void *, long, size_t *, long *);
    static int32_t (*create)(CFAllocatorRef, void *, void *, long, long, const void *, long,
                             const size_t *, void **);
    static CFArrayRef (*attachmentsOf)(void *, Boolean);
    static void (*propagate)(void *, void *);
    if (!create) {
        dataOf = dlsym(RTLD_DEFAULT, "CMSampleBufferGetDataBuffer");
        samplesOf = dlsym(RTLD_DEFAULT, "CMSampleBufferGetNumSamples");
        timingOf = dlsym(RTLD_DEFAULT, "CMSampleBufferGetSampleTimingInfoArray");
        sizesOf = dlsym(RTLD_DEFAULT, "CMSampleBufferGetSampleSizeArray");
        attachmentsOf = dlsym(RTLD_DEFAULT, "CMSampleBufferGetSampleAttachmentsArray");
        propagate = dlsym(RTLD_DEFAULT, "CMPropagateAttachments");
        create = dlsym(RTLD_DEFAULT, "CMSampleBufferCreateReady");
    }
    if (!dataOf || !samplesOf || !timingOf || !sizesOf || !attachmentsOf || !propagate || !create)
        return NULL;
    long timings = 0, sizes = 0;
    if (timingOf(sampleBuffer, 0, NULL, &timings)) timings = 0;
    if (sizesOf(sampleBuffer, 0, NULL, &sizes)) sizes = 0;
    VTBridgeTime *timing = timings ? calloc((size_t)timings * 3, sizeof(VTBridgeTime)) : NULL;
    size_t *size = sizes ? calloc((size_t)sizes, sizeof(size_t)) : NULL;
    void *result = NULL;
    if ((!timings || (timing && !timingOf(sampleBuffer, timings, timing, &timings))) &&
        (!sizes || (size && !sizesOf(sampleBuffer, sizes, size, &sizes))) &&
        !create(NULL, dataOf(sampleBuffer), format, samplesOf(sampleBuffer), timings, timing, sizes,
                size, &result) && result) {
        propagate(sampleBuffer, result);
        CFArrayRef from = attachmentsOf(sampleBuffer, false), to = attachmentsOf(result, true);
        for (CFIndex i = 0; from && to && i < CFArrayGetCount(from) && i < CFArrayGetCount(to); ++i) {
            CFDictionaryRef source = CFArrayGetValueAtIndex(from, i);
            CFMutableDictionaryRef target = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(to, i);
            CFIndex count = CFDictionaryGetCount(source);
            const void **keys = calloc((size_t)count * 2 + 1, sizeof(void *));
            CFDictionaryGetKeysAndValues(source, keys, keys + count);
            for (CFIndex key = 0; key < count; ++key) CFDictionarySetValue(target, keys[key], keys[count + key]);
            free(keys);
        }
    }
    free(timing);
    free(size);
    return result;
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
    // A session made for a layer of an alpha stream judges the new format the
    // way it was created: without the alpha marking.
    void *candidate = decoder->sessionFormat ? vt_bridge_single_layer_format(format) : NULL;
    if (vtBridge.sessionCanAccept && (!decoder->sessionFormat || candidate) &&
        vtBridge.sessionCanAccept(decoder->systemSession, candidate ? candidate : format)) {
        if (change <= 4)
            logf_("[VTBridge] session #%u format change %u accepted by the system session",
                  decoder->index, change);
        CFRetain(format);
        if (decoder->format) CFRelease(decoder->format);
        decoder->format = format;
        if (candidate) {
            CFRelease(decoder->sessionFormat);
            decoder->sessionFormat = candidate;
        }
        return 0;
    }
    if (candidate) CFRelease(candidate);
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
    if (status || change <= 4)
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
    decoder->index = __sync_add_and_fetch(&vtBridgeSessions, 1);
    int32_t status = vt_bridge_open(decoder, format);
    uint32_t (*subType)(void *) = dlsym(RTLD_DEFAULT, "CMFormatDescriptionGetMediaSubType");
    uint64_t (*dimensions)(void *) = dlsym(RTLD_DEFAULT, "CMVideoFormatDescriptionGetDimensions");
    uint32_t codec = subType ? subType(format) : 0;
    uint64_t size = dimensions ? dimensions(format) : 0;
    logf_("[VTBridge] session #%u start %c%c%c%c %ux%u status=%d", decoder->index,
          codec >> 24, codec >> 16, codec >> 8, codec, (uint32_t)size, (uint32_t)(size >> 32), status);
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
    if (!decoder->sessionFormat)
        return vtBridge.sessionDecode(decoder->systemSession, sampleBuffer, flags, frame, infoFlagsOut);
    void *rewrapped = vt_bridge_rewrap(sampleBuffer, decoder->sessionFormat);
    if (!rewrapped) return -12911;
    int32_t status = vtBridge.sessionDecode(decoder->systemSession, rewrapped, flags, frame, infoFlagsOut);
    CFRelease(rewrapped);
    return status;
}

static int32_t vt_bridge_finish(void *object) {
    VTBridgeDecoder *decoder = vtBridge.derivedStorage(object);
    if (!decoder || !decoder->systemSession) return 0;
    int32_t status = vtBridge.sessionFinish(decoder->systemSession);
    int32_t waited = vtBridge.sessionWait(decoder->systemSession);
    return status ? status : waited;
}

// HEIC images decode through tile sessions: each sample is one tile of the
// image, which the decoder writes into a canvas buffer the guest allocated
// with the attributes the decoder asked for. Forward them to a hardware tile
// session of the system framework, which writes into the same canvas.
static void vt_bridge_tile_output(void *refcon, void *tile, VTBridgePoint cropOffset, VTBridgeSize cropSize,
                                  void *canvas, VTBridgePoint canvasOffset, int32_t status,
                                  uint32_t infoFlags) {
    (void)cropOffset; (void)cropSize; (void)canvas; (void)canvasOffset;
    VTBridgeDecoder *decoder = refcon;
    if (status && ++decoder->tileErrors <= 4)
        logf_("[VTBridge] tile session #%u system decoder returned status=%d", decoder->index, status);
    ++decoder->tiles;
    // NULL leaves the canvas the guest passed in with the tile.
    int32_t result = vtBridge.emitTile(decoder->hostTileSession, (int64_t)(intptr_t)tile, status,
                                       infoFlags, NULL);
    if (result && ++decoder->tileErrors <= 4)
        logf_("[VTBridge] tile session #%u emit tile %u -> %d", decoder->index, decoder->tiles, result);
    // As with frames, the guest frees the canvas it allocated for a tile only
    // once the host session reports letting go of it, which it does only from
    // here. Without this every tile's canvas stays wired in the guest.
    vtBridge.cleanUpAfterTile(decoder->hostTileSession);
}

static int32_t vt_bridge_start_tiles(void *object, void *tileSession, void *format) {
    VTBridgeDecoder *decoder = vtBridge.derivedStorage(object);
    if (!decoder) return -12902;
    if (!vtBridge.tileSessionCreate) return -12910;
    decoder->hostTileSession = tileSession;
    decoder->index = __sync_add_and_fetch(&vtBridgeSessions, 1);
    const void *keys[] = {CFSTR("RequireHardwareAcceleratedVideoDecoder")};
    const void *values[] = {kCFBooleanTrue};
    CFDictionaryRef specification = CFDictionaryCreate(NULL, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    void *created = NULL;
    int32_t status = vtBridge.tileSessionCreate(NULL, format, specification, vt_bridge_tile_output,
                                                decoder, &created);
    CFRelease(specification);
    // The guest sizes and aligns tiles and allocates the canvas by what the
    // decoder requires; pass on what the system decoder requires.
    CFDictionaryRef canvasAttributes = NULL, requirements = NULL;
    int32_t requirementsStatus = -12782;
    if (!status && created) {
        decoder->systemTileSession = created;
        vtBridge.tileSessionCopyProperty(created, vtBridge.canvasAttributesKey, NULL, &canvasAttributes);
        vtBridge.tileSessionCopyProperty(created, vtBridge.tileRequirementsKey, NULL, &requirements);
        requirementsStatus = vtBridge.setTileRequirements(tileSession, canvasAttributes, requirements);
    }
    uint32_t (*subType)(void *) = dlsym(RTLD_DEFAULT, "CMFormatDescriptionGetMediaSubType");
    uint64_t (*dimensions)(void *) = dlsym(RTLD_DEFAULT, "CMVideoFormatDescriptionGetDimensions");
    uint32_t codec = subType ? subType(format) : 0;
    uint64_t size = dimensions ? dimensions(format) : 0;
    // Images open one tile session each; log the first ones in full.
    unsigned count = __sync_add_and_fetch(&vtBridgeTileSessions, 1);
    if (status || count <= 4) {
        logf_("[VTBridge] tile session #%u start %c%c%c%c %ux%u status=%d requirements=%d", decoder->index,
              codec >> 24, codec >> 16, codec >> 8, codec, (uint32_t)size, (uint32_t)(size >> 32), status,
              requirementsStatus);
        if (count == 1 && requirements) {
            CFStringRef text = CFCopyDescription(requirements);
            char buffer[512] = "";
            if (text) CFStringGetCString(text, buffer, sizeof(buffer), kCFStringEncodingUTF8);
            // One log line: fold the description's newlines and indentation.
            char *to = buffer;
            for (const char *from = buffer; *from; ++from)
                if (!isspace((unsigned char)*from) || (to > buffer && to[-1] != ' ')) *to++ = isspace((unsigned char)*from) ? ' ' : *from;
            *to = 0;
            logf_("[VTBridge] tile requirements %s", buffer);
            if (text) CFRelease(text);
        }
    }
    if (canvasAttributes) CFRelease(canvasAttributes);
    if (requirements) CFRelease(requirements);
    return status;
}

static int32_t vt_bridge_decode_tile(void *object, int64_t tile, void *sampleBuffer, VTBridgePoint cropOffset,
                                     VTBridgeSize cropSize, void *canvas, VTBridgePoint canvasOffset,
                                     uint32_t flags, uint32_t *infoFlagsOut) {
    VTBridgeDecoder *decoder = vtBridge.derivedStorage(object);
    if (!decoder || !decoder->systemTileSession) return -12902;
    // The tile number travels as the system session's per-tile refcon and
    // comes back in vt_bridge_tile_output, which emits it exactly once.
    int32_t status = vtBridge.tileSessionDecode(decoder->systemTileSession, sampleBuffer, cropOffset, cropSize,
                                                canvas, canvasOffset, flags, (void *)(intptr_t)tile,
                                                infoFlagsOut);
    if (status && ++decoder->tileErrors <= 4)
        logf_("[VTBridge] tile session #%u decode tile %lld crop %d,%d %dx%d at %d,%d -> %d", decoder->index,
              (long long)tile, cropOffset.x, cropOffset.y, cropSize.width, cropSize.height,
              canvasOffset.x, canvasOffset.y, status);
    return status;
}

static int32_t vt_bridge_finish_tiles(void *object) {
    VTBridgeDecoder *decoder = vtBridge.derivedStorage(object);
    if (!decoder || !decoder->systemTileSession) return 0;
    return vtBridge.tileSessionFinish(decoder->systemTileSession);
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
    if (decoder && decoder->systemTileSession) {
        if (decoder->tileErrors || vtBridgeTileSessions <= 4)
            logf_("[VTBridge] tile session #%u invalidate tiles=%u errors=%u", decoder->index,
                  decoder->tiles, decoder->tileErrors);
        vtBridge.tileSessionInvalidate(decoder->systemTileSession);
        CFRelease(decoder->systemTileSession);
        decoder->systemTileSession = NULL;
    }
    if (decoder && decoder->format) {
        CFRelease(decoder->format);
        decoder->format = NULL;
    }
    if (decoder && decoder->sessionFormat) {
        CFRelease(decoder->sessionFormat);
        decoder->sessionFormat = NULL;
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
    3, vt_bridge_start, vt_bridge_decode, vt_bridge_supported_properties, NULL, NULL, NULL,
    vt_bridge_finish, vt_bridge_start_tiles, vt_bridge_decode_tile, vt_bridge_finish_tiles,
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
    if (!access(vtBridgeDisabledFlag, F_OK)) {
        logf_("[VTBridge] disabled by %s", vtBridgeDisabledFlag);
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
    // Tile sessions (SPI in both frameworks) have the same signatures in both.
    CFStringRef *canvasKey = dlsym(system, "kVTDecompressionPropertyKey_CanvasPixelBufferAttributes");
    CFStringRef *requirementsKey = dlsym(system, "kVTDecompressionPropertyKey_TileDecoderRequirements");
    void *tileParts[] = {
        dlsym(bundled, "VTTileDecoderSessionEmitDecodedTile"),
        dlsym(bundled, "VTTileDecoderSessionSetTileDecodeRequirements"),
        dlsym(system, "VTTileDecompressionSessionCreate"),
        dlsym(system, "VTTileDecompressionSessionDecodeTile"),
        dlsym(system, "VTTileDecompressionSessionFinishDelayedTiles"),
        dlsym(system, "VTTileDecompressionSessionInvalidate"),
        dlsym(system, "VTTileDecompressionSessionCopyProperty"),
        dlsym(bundled, "VTTileDecoderSessionCleanUpAfterDecode"),
    };
    BOOL tiles = canvasKey && *canvasKey && requirementsKey && *requirementsKey;
    for (unsigned i = 0; i < sizeof(tileParts) / sizeof(tileParts[0]); ++i) tiles = tiles && tileParts[i];
    if (tiles && tileParts[2] != dlsym(bundled, "VTTileDecompressionSessionCreate")) {
        vtBridge.emitTile = tileParts[0];
        vtBridge.setTileRequirements = tileParts[1];
        vtBridge.tileSessionCreate = tileParts[2];
        vtBridge.tileSessionDecode = tileParts[3];
        vtBridge.tileSessionFinish = tileParts[4];
        vtBridge.tileSessionInvalidate = tileParts[5];
        vtBridge.tileSessionCopyProperty = tileParts[6];
        vtBridge.cleanUpAfterTile = tileParts[7];
        vtBridge.canvasAttributesKey = *canvasKey;
        vtBridge.tileRequirementsKey = *requirementsKey;
    } else logf_("[VTBridge] tile decoding not bridged: a symbol is missing or not distinct");
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
