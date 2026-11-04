#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modern_pvg_memory.h"

// The struct encoding in macOS 27's -[PGMemoryMapDescriptor addRange:].
typedef struct {
    uint64_t physicalAddress;
    uint64_t physicalLength;
    void *virtualAddress;
} VZPGGuestPhysicalRange;

static pthread_mutex_t rangeLock = PTHREAD_MUTEX_INITIALIZER;
static VZPGGuestPhysicalRange ranges[256];
static size_t rangeCount;
static bool rangeOverflow;
static id pendingResets[2];
static size_t pendingResetCount, remappedSinceReset;

static void VZModernApplyPendingResets(void);

bool VZModernPVGEnabled(void) {
    const char *version = getenv("VZ_PVG_BACKEND_VERSION");
    return version && strcmp(version, "27") == 0;
}

void VZModernRecordGuestMapping(void *address, uint64_t physical,
                               size_t length, uint64_t flags) {
    if (!VZModernPVGEnabled() || !address || !length ||
        physical > UINT64_MAX - length || !(flags & 1)) return;
    pthread_mutex_lock(&rangeLock);
    bool replaced = false;
    for (size_t i = 0; i < rangeCount; ++i) {
        if (ranges[i].physicalAddress == physical &&
            ranges[i].physicalLength == length) {
            ranges[i].virtualAddress = address;
            replaced = true;
            remappedSinceReset++;
            break;
        }
    }
    if (!replaced) {
        if (rangeCount < sizeof(ranges) / sizeof(ranges[0]))
            ranges[rangeCount++] = (VZPGGuestPhysicalRange){physical, length, address};
        else rangeOverflow = true;
    }
    pthread_mutex_unlock(&rangeLock);
    fprintf(stderr, "[ModernPVG] RAM mapping physical=0x%llx length=0x%zx host=%p\n",
        (unsigned long long)physical, length, address);
    VZModernApplyPendingResets();
}

static id VZModernCopyMemoryMapDescriptor(void) {
    Class mapClass = objc_getClass("PGMemoryMapDescriptor");
    SEL addRange = sel_registerName("addRange:");
    if (!mapClass || ![mapClass instancesRespondToSelector:addRange]) return nil;
    id map = [mapClass new];
    pthread_mutex_lock(&rangeLock);
    BOOL valid = !rangeOverflow && rangeCount != 0;
    if (valid) {
        for (size_t i = 0; i < rangeCount; ++i)
            ((void (*)(id, SEL, VZPGGuestPhysicalRange))objc_msgSend)(map, addRange, ranges[i]);
    }
    size_t count = rangeCount;
    pthread_mutex_unlock(&rangeLock);
    if (!valid) {
        [map release];
        fprintf(stderr, "[ModernPVG] no complete recorded guest memory map (count=%zu)\n", count);
        return nil;
    }
    fprintf(stderr, "[ModernPVG] descriptor includes %zu real RAM ranges\n", count);
    return map;
}

// A guest reboot gives guest RAM new backing pages at the same addresses, then
// registers it with Hypervisor again. The Ventura VMM only calls -reset, which
// keeps a map of the old pages, so a new GPU task reads stale guest page tables.
// Do what the macOS 27 VMM does: rebuild each device's map from the new pages.
static IMP originalDeviceReset, originalSurfaceReset;
static __thread bool resettingWithMap;

static void VZModernApplyPendingResets(void) {
    id devices[2];
    size_t count = 0;
    pthread_mutex_lock(&rangeLock);
    if (remappedSinceReset >= rangeCount) {
        for (; count < pendingResetCount; ++count) devices[count] = pendingResets[count];
        if (count) pendingResetCount = 0, remappedSinceReset = 0;
    }
    pthread_mutex_unlock(&rangeLock);
    for (size_t i = 0; i < count; ++i) {
        id map = VZModernCopyMemoryMapDescriptor();
        if (!map) continue;
        resettingWithMap = true;
        bool result = ((bool (*)(id, SEL, id))objc_msgSend)(devices[i],
            sel_registerName("resetWithMemoryMap:"), map);
        resettingWithMap = false;
        [map release];
        fprintf(stderr, "[ModernPVG] %s resetWithMemoryMap after remap result=%d\n",
            object_getClassName(devices[i]), result);
    }
}

static void VZModernNoteDeviceReset(id device) {
    pthread_mutex_lock(&rangeLock);
    bool known = false;
    for (size_t i = 0; i < pendingResetCount; ++i) known |= pendingResets[i] == device;
    if (!known && pendingResetCount < sizeof(pendingResets) / sizeof(pendingResets[0]))
        pendingResets[pendingResetCount++] = device;
    remappedSinceReset = 0;
    pthread_mutex_unlock(&rangeLock);
    fprintf(stderr, "[ModernPVG] %s reset; waiting for guest RAM\n", object_getClassName(device));
}

id VZModernNewDeviceWithDescriptor(id descriptor) {
    Class deviceClass = objc_getClass("_PGDevice");
    SEL setMap = sel_registerName("setMemoryMapDescriptor:");
    SEL initDevice = sel_registerName("initWithDescriptor:");
    if (!deviceClass || ![descriptor respondsToSelector:setMap] ||
        ![deviceClass instancesRespondToSelector:initDevice]) {
        fprintf(stderr, "[ModernPVG] required descriptor API unavailable\n");
        return nil;
    }
    id map = VZModernCopyMemoryMapDescriptor();
    if (!map) return nil;
    ((void (*)(id, SEL, id))objc_msgSend)(descriptor, setMap, map);
    [map release];
    fprintf(stderr, "[ModernPVG] creating native _PGDevice\n");
    // macOS 27's legacy exported factory returns nil. Use its measured native
    // initializer after translating real Hypervisor mappings to the descriptor.
    // Successful construction does not prove GPU-task service compatibility.
    id result = ((id (*)(id, SEL, id))objc_msgSend)([deviceClass alloc], initDevice, descriptor);
    fprintf(stderr, "[ModernPVG] device=%p\n", result);
    return result;
}

static char legacySurfaceMapKey, legacySurfaceUnmapKey;
static id (*originalSurfaceInitializer)(id, SEL, id);

static void VZModernSetSurfaceMap(id descriptor, SEL selector, id block) {
    (void)selector;
    objc_setAssociatedObject(descriptor, &legacySurfaceMapKey, block,
                             OBJC_ASSOCIATION_COPY_NONATOMIC);
}

static void VZModernSetSurfaceUnmap(id descriptor, SEL selector, id block) {
    (void)selector;
    objc_setAssociatedObject(descriptor, &legacySurfaceUnmapKey, block,
                             OBJC_ASSOCIATION_COPY_NONATOMIC);
}

static id VZModernGetSurfaceMap(id descriptor, SEL selector) {
    (void)selector;
    return objc_getAssociatedObject(descriptor, &legacySurfaceMapKey);
}

static id VZModernGetSurfaceUnmap(id descriptor, SEL selector) {
    (void)selector;
    return objc_getAssociatedObject(descriptor, &legacySurfaceUnmapKey);
}

static id VZModernInitSurfaceWithDescriptor(id object, SEL selector,
                                           id descriptor) {
    // Ventura supplies map/unmap blocks. macOS 27 consumes a PGMemoryMap of
    // actual guest physical ranges instead. Translate at the initialization
    // boundary, after Hypervisor has installed the complete VM RAM map.
    id map = VZModernCopyMemoryMapDescriptor();
    if (!map) {
        [object release];
        return nil;
    }
    ((void (*)(id, SEL, id))objc_msgSend)(descriptor,
        sel_registerName("setMemoryMapDescriptor:"), map);
    [map release];
    id result = originalSurfaceInitializer(object, selector, descriptor);
    fprintf(stderr, "[ModernPVG] IOSurface device=%p with real RAM map\n", result);
    return result;
}

static void VZModernDeviceReset(id device, SEL selector) {
    ((void (*)(id, SEL))originalDeviceReset)(device, selector);
    if (!resettingWithMap) VZModernNoteDeviceReset(device);
}

static void VZModernSurfaceReset(id device, SEL selector) {
    ((void (*)(id, SEL))originalSurfaceReset)(device, selector);
    if (!resettingWithMap) VZModernNoteDeviceReset(device);
}

static void VZModernInstallResetBridge(void) {
    SEL reset = sel_registerName("reset");
    Class deviceClass = objc_getClass("_PGDevice");
    Class surfaceClass = objc_getClass("PGIOSurfaceHostDevice");
    SEL resetMap = sel_registerName("resetWithMemoryMap:");
    if (!originalDeviceReset && [deviceClass instancesRespondToSelector:resetMap])
        originalDeviceReset = method_setImplementation(
            class_getInstanceMethod(deviceClass, reset), (IMP)VZModernDeviceReset);
    if (!originalSurfaceReset && [surfaceClass instancesRespondToSelector:resetMap])
        originalSurfaceReset = method_setImplementation(
            class_getInstanceMethod(surfaceClass, reset), (IMP)VZModernSurfaceReset);
    fprintf(stderr, "[ModernPVG] reset bridge device=%d surface=%d\n",
        originalDeviceReset != NULL, originalSurfaceReset != NULL);
}

bool VZModernInstallIOSurfaceDescriptorBridge(void) {
    if (!VZModernPVGEnabled()) return false;
    VZModernInstallResetBridge();
    Class descriptorClass = objc_getClass("PGIOSurfaceHostDeviceDescriptor");
    Class deviceClass = objc_getClass("PGIOSurfaceHostDevice");
    SEL initSelector = sel_registerName("initWithDescriptor:");
    Method initializer = class_getInstanceMethod(deviceClass, initSelector);
    if (!descriptorClass || !initializer ||
        ![descriptorClass instancesRespondToSelector:
            sel_registerName("setMemoryMapDescriptor:")]) return false;
    if (originalSurfaceInitializer) return true;
    // Exact setter/getter encodings measured in Ventura's descriptor. Copies
    // preserve the VMM block ownership contract; the modern initializer uses
    // the recorded real RAM map and never pretends a mapping succeeded.
    class_addMethod(descriptorClass, sel_registerName("setMapMemory:"),
                    (IMP)VZModernSetSurfaceMap, "v24@0:8@?16");
    class_addMethod(descriptorClass, sel_registerName("setUnmapMemory:"),
                    (IMP)VZModernSetSurfaceUnmap, "v24@0:8@?16");
    class_addMethod(descriptorClass, sel_registerName("mapMemory"),
                    (IMP)VZModernGetSurfaceMap, "@?16@0:8");
    class_addMethod(descriptorClass, sel_registerName("unmapMemory"),
                    (IMP)VZModernGetSurfaceUnmap, "@?16@0:8");
    originalSurfaceInitializer = (id (*)(id, SEL, id))method_setImplementation(
        initializer, (IMP)VZModernInitSurfaceWithDescriptor);
    fprintf(stderr, "[ModernPVG] installed IOSurface descriptor RAM bridge\n");
    return true;
}
