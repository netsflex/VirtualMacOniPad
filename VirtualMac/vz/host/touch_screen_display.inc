// Native Touch: bind the VMM's Apple Virtual Platform touch screen
// to the guest's display. WindowServer drops a digitizer unless its service's
// displayUUID names a display, so both ends need a UUID known before the guest boots.
// The app derives one per virtual machine and passes it in VZ_TOUCH_DISPLAY_UUID.
//
// Touch screen end: the AVP service's registry properties come from a dictionary
// the VMM builds, and the guest's HID bridge publishes them.
//
// Display end: the guest's display driver takes IOMFBUUID from the shared state page
// at +0xa74 and generates a random one per boot when that is empty. The PVG framework
// this VMM uses predates the field, so it is filled in here. The page's host mapping
// is at +0x28 of _PGDisplayNub, and the object that holds the mapping at +0x20.
enum {
    kNubMappingOffset = 0x20, kNubStateOffset = 0x28,
    kStateDisplayUUIDOffset = 0xa74, kStateDisplayUUIDSize = 0x27,
};

static const char *touch_display_uuid(void) {
    const char *uuid = getenv("VZ_TOUCH_DISPLAY_UUID");
    return uuid && strlen(uuid) == 36 ? uuid : NULL;
}

static CFDictionaryRef touch_screen_dictionary_create(CFAllocatorRef allocator, const void **keys,
    const void **values, CFIndex count, const CFDictionaryKeyCallBacks *keyCallBacks,
    const CFDictionaryValueCallBacks *valueCallBacks) {
    enum { kMaxEntries = 16 };
    const char *uuid = count < kMaxEntries ? touch_display_uuid() : NULL;
    bool isTouchScreen = false;
    for (CFIndex i = 0; uuid && values && i < count && !isTouchScreen; i++)
        isTouchScreen = values[i] && CFGetTypeID(values[i]) == CFStringGetTypeID() &&
            CFEqual(values[i], CFSTR("Virtual Touch Screen"));
    if (!isTouchScreen)
        return CFDictionaryCreate(allocator, keys, values, count, keyCallBacks, valueCallBacks);
    const void *allKeys[kMaxEntries + 1], *allValues[kMaxEntries + 1];
    memcpy(allKeys, keys, count * sizeof *keys);
    memcpy(allValues, values, count * sizeof *values);
    CFStringRef value = CFStringCreateWithCString(NULL, uuid, kCFStringEncodingUTF8);
    allKeys[count] = CFSTR("displayUUID");
    allValues[count] = value;
    CFDictionaryRef result = CFDictionaryCreate(allocator, allKeys, allValues, count + 1, keyCallBacks, valueCallBacks);
    CFRelease(value);
    return result;
}
__attribute__((used)) static struct { const void *replacement, *replacee; }
_ip_touch_screen_dictionary __attribute__((section("__DATA,__interpose"))) = {
    (const void *)touch_screen_dictionary_create, (const void *)CFDictionaryCreate,
};

static id (*gOriginalNubInit)(id, SEL, id, NSUInteger, uint32_t);

// The field is cleared right after the nub is created, and the guest reads it within
// a couple of milliseconds of that. Neither end signals the host in between, so watch
// the field closely until it has been cleared, then more loosely in case it happens
// again while the guest's display comes up.
static void keep_display_uuid(char *state, id mapping, const char *uuid) {
    enum { kSpinLimitMs = 300, kSpinAfterClearMs = 5, kWatchLimitMs = 5000, kWatchIntervalUs = 2000 };
    uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint64_t spinUntil = kSpinLimitMs;
    unsigned cleared = 0;
    for (;;) {
        uint64_t ms = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1000000;
        if (ms > kWatchLimitMs) break;
        if (*(volatile char *)(state + kStateDisplayUUIDOffset) == 0) {
            strlcpy(state + kStateDisplayUUIDOffset, uuid, kStateDisplayUUIDSize);
            if (!cleared++) spinUntil = ms + kSpinAfterClearMs;
        }
        if (ms > spinUntil) usleep(kWatchIntervalUs);
    }
    logf_("[TouchScreen] display UUID field was cleared %u times", cleared);
    CFRelease(mapping);
}

static id touch_display_nub_init(id self, SEL selector, id device, NSUInteger port, uint32_t page) {
    id result = gOriginalNubInit(self, selector, device, port, page);
    const char *uuid = touch_display_uuid();
    if (!result || !uuid) return result;
    id mapping = *(id *)((uint8_t *)result + kNubMappingOffset);
    char *state = *(char **)((uint8_t *)result + kNubStateOffset);
    // Check the layout this relies on against the object itself.
    if (!mapping || !state || ![mapping respondsToSelector:sel_registerName("virtualAddress")] ||
        ((void *(*)(id, SEL))objc_msgSend)(mapping, sel_registerName("virtualAddress")) != state) {
        logf_("[TouchScreen] not setting the display UUID: unexpected _PGDisplayNub layout");
        return result;
    }
    strlcpy(state + kStateDisplayUUIDOffset, uuid, kStateDisplayUUIDSize);
    // Holding the mapping keeps the page mapped however soon the display goes away.
    CFRetain(mapping);
    char *uuidCopy = strdup(uuid);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        keep_display_uuid(state, mapping, uuidCopy);
        free(uuidCopy);
    });
    return result;
}

static void touch_display_install(void) {
    if (!touch_display_uuid()) return;
    Class nub = objc_getClass("_PGDisplayNub");
    Method method = nub ? class_getInstanceMethod(nub, sel_registerName("initWithDevice:port:sharedStatePage:")) : NULL;
    if (!method || strcmp(method_getTypeEncoding(method), "@36@0:8@16Q24I32") != 0) {
        logf_("[TouchScreen] not setting the display UUID: unexpected _PGDisplayNub");
        return;
    }
    gOriginalNubInit = (id (*)(id, SEL, id, NSUInteger, uint32_t))method_setImplementation(method, (IMP)touch_display_nub_init);
    logf_("[TouchScreen] display UUID %s", touch_display_uuid());
}
