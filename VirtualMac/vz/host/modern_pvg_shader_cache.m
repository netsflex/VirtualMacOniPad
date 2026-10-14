// Application-only cache of genuine Apple AIR transformations. The original
// and transformed complete libraries are pinned by SHA256. Unknown libraries,
// rejected transformations, and every subsequent pipeline/GPU error retain
// the native Metal path. This does not change GPU capability declarations.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <CommonCrypto/CommonDigest.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "modern_pvg_shader_cache.h"
#include "modern_pvg_shader_worker.h"
#include "modern_pvg_address_guard.h"

static NSString *const cacheDirectory = @"/var/root/VirtualMac2/shader-cache/air25";
static NSDictionary *cacheEntries;
static id (*nativeLibraryData)(id, SEL, dispatch_data_t, NSError **);
static unsigned diagnosticCount;
static unsigned dynamicDiagnosticCount;

// Bound the read before allocating; cache data must be an ordinary file.
static NSData *ReadCacheFile(NSString *path, size_t limit) {
    int fd = open(path.fileSystemRepresentation,
                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return nil;
    struct stat information;
    if (fstat(fd, &information) || !S_ISREG(information.st_mode) ||
        information.st_size <= 0 || (uint64_t)information.st_size > limit) {
        close(fd);
        return nil;
    }
    NSMutableData *data = [NSMutableData dataWithLength:(NSUInteger)information.st_size];
    size_t offset = 0;
    while (offset < data.length) {
        ssize_t count = read(fd, (unsigned char *)data.mutableBytes + offset,
                             data.length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); return nil; }
        offset += (size_t)count;
    }
    close(fd);
    return [[data copy] autorelease];
}

static BOOL IsHash(NSString *value) {
    if (![value isKindOfClass:[NSString class]] || value.length != 64) return NO;
    for (unsigned i = 0; i < 64; ++i) {
        unichar c = [value characterAtIndex:i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return NO;
    }
    return YES;
}

static NSString *HashBytes(const void *bytes, size_t size) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    char hash[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    CC_SHA256(bytes, (CC_LONG)size, digest);
    for (unsigned i = 0; i < sizeof(digest); ++i)
        snprintf(hash + i * 2, 3, "%02x", digest[i]);
    return [NSString stringWithUTF8String:hash];
}

static BOOL ValidEntry(NSString *inputHash, NSDictionary *entry) {
    if (!IsHash(inputHash) || ![entry isKindOfClass:[NSDictionary class]] ||
        !IsHash(entry[@"outputSHA256"])) return NO;
    NSString *filename = [inputHash stringByAppendingString:@".metallib"];
    if (![entry[@"filename"] isEqual:filename] ||
        ![entry[@"airVersion"] isEqual:@[@2, @5, @0]]) return NO;
    NSArray *language = entry[@"languageVersion"];
    if (![language isKindOfClass:[NSArray class]] || language.count != 3)
        return NO;
    for (id part in language)
        if (![part isKindOfClass:[NSNumber class]]) return NO;
    unsigned major = [language[0] unsignedIntValue];
    unsigned minor = [language[1] unsignedIntValue];
    return [language[2] unsignedIntValue] == 0 &&
        ((major == 1 && minor <= 2) || (major == 2 && minor <= 4) ||
         (major == 3 && minor == 0));
}

static id CachedLibraryData(id device, SEL selector, dispatch_data_t input,
                            NSError **error) {
    if (!input || dispatch_data_get_size(input) > 16 * 1024 * 1024)
        return nativeLibraryData(device, selector, input, error);
    const void *bytes = NULL;
    size_t size = 0;
    dispatch_data_t mapped = dispatch_data_create_map(input, &bytes, &size);
    if (!mapped || !bytes || size > 16 * 1024 * 1024) {
        if (mapped) dispatch_release(mapped);
        return nativeLibraryData(device, selector, input, error);
    }
    NSString *inputHash = HashBytes(bytes, size);
    NSDictionary *entry = cacheEntries[inputHash];
    NSData *converted = nil;
    NSString *outputHash = nil;
    BOOL dynamic = entry == nil;
    if (entry) {
        NSString *path = [cacheDirectory stringByAppendingPathComponent:entry[@"filename"]];
        converted = ReadCacheFile(path, 16 * 1024 * 1024);
        outputHash = entry[@"outputSHA256"];
        if (!converted || ![HashBytes(converted.bytes, converted.length) isEqual:outputHash]) {
            if (__sync_fetch_and_add(&diagnosticCount, 1) < 24)
                fprintf(stderr, "[ModernShaderCache] integrity failure input=%s\n",
                        inputHash.UTF8String);
            dispatch_release(mapped);
            return nativeLibraryData(device, selector, input, error);
        }
    } else {
        // Keep the mapped input alive until the isolated worker has copied it.
        converted = VZModernConvertUnknownShader(bytes, size, inputHash);
        if (converted) outputHash = HashBytes(converted.bytes, converted.length);
    }
    dispatch_release(mapped);
    if (!converted) return nativeLibraryData(device, selector, input, error);
    dispatch_data_t data = dispatch_data_create(converted.bytes, converted.length,
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ (void)converted; });
    NSError *failure = nil;
    id library = nativeLibraryData(device, selector, data, &failure);
    dispatch_release(data);
    unsigned logged = dynamic ? __sync_fetch_and_add(&dynamicDiagnosticCount, 1) :
                               __sync_fetch_and_add(&diagnosticCount, 1);
    if (logged < (dynamic ? 64 : 24))
        fprintf(stderr,
            "[ModernShaderCache] input=%s output=%s dynamic=%d nativeLibrary=%p error=%s\n",
            inputHash.UTF8String, outputHash.UTF8String, dynamic, library,
            failure.description.UTF8String ?: "none");
    if (!library) return nativeLibraryData(device, selector, input, error);
    if (VZModernShaderUsesAddresses(converted)) VZModernMarkAddressLibrary(library);
    if (dynamic && !failure)
        VZModernRecordSuccessfulNativeLibrary(converted, inputHash);
    if (error) *error = nil;
    return library;
}

bool VZModernInstallShaderCompatibilityCache(id device) {
    const char *role = getenv("VZ_PVG_TASK_ROLE");
    const char *version = getenv("VZ_PVG_BACKEND_VERSION");
    if (!device || !role || strcmp(role, "server") || !version ||
        strcmp(version, "27") || nativeLibraryData) return false;
    NSString *manifestPath = [cacheDirectory stringByAppendingPathComponent:@"manifest.json"];
    errno = 0;
    NSData *manifestData = ReadCacheFile(manifestPath, 1024 * 1024);
    int readErrno = errno;
    if (!manifestData) {
        fprintf(stderr,
            "[ModernShaderCache] manifest read failed path=%s euid=%u errno=%d(%s)\n",
            manifestPath.UTF8String, (unsigned)geteuid(), readErrno,
            strerror(readErrno));
        return false;
    }
    if (manifestData.length > 1024 * 1024) {
        fprintf(stderr,
            "[ModernShaderCache] manifest too large path=%s bytes=%lu euid=%u\n",
            manifestPath.UTF8String, (unsigned long)manifestData.length,
            (unsigned)geteuid());
        return false;
    }
    id manifest = [NSJSONSerialization JSONObjectWithData:manifestData options:0 error:NULL];
    if (![manifest isKindOfClass:[NSDictionary class]] ||
        ![manifest[@"formatVersion"] isEqual:@1] ||
        ![manifest[@"entries"] isKindOfClass:[NSDictionary class]]) return false;
    NSMutableDictionary *valid = [NSMutableDictionary dictionary];
    for (id hash in manifest[@"entries"]) {
        id entry = manifest[@"entries"][hash];
        if (ValidEntry(hash, entry)) valid[hash] = entry;
    }
    if (!valid.count) return false;
    SEL selector = sel_registerName("newLibraryWithData:error:");
    Class cls = object_getClass(device);
    Method method = class_getInstanceMethod(cls, selector);
    if (!method) return false;
    IMP original = method_getImplementation(method);
    // Publish the saved IMP and immutable cache before making the hook visible.
    cacheEntries = [valid copy];
    nativeLibraryData = (void *)original;
    if (!class_addMethod(cls, selector, (IMP)CachedLibraryData,
                        method_getTypeEncoding(method)))
        method_setImplementation(class_getInstanceMethod(cls, selector),
                                 (IMP)CachedLibraryData);
    fprintf(stderr, "[ModernShaderCache] installed %lu genuine AIR2.5 conversions\n",
            (unsigned long)cacheEntries.count);
    return true;
}
