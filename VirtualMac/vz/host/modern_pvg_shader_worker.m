// App-private CPU conversion client. No Metal device, GPU object or submission.
// The signed helper owns the genuine Apple downgrade/verifier/writer ABI; this
// client checks its complete, hash-bound result before returning library data.
#import "modern_pvg_shader_worker.h"
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *const codeDirectory = "/var/root/VirtualMac2/compiler27";
static const char *const workerPath = "/var/root/VirtualMac2/compiler27/metal-library-convert";
static const char *const normalizerPath = "/var/root/VirtualMac2/compiler27/vz-air-normalize";
static const char *const compilerPath = "/var/root/VirtualMac2/compiler27/VM27GPUCompiler.dylib";
static const char *const workDirectory = "/var/root/VirtualMac2/compiler27/work";
static const size_t libraryLimit = 16 * 1024 * 1024;
static const size_t metadataLimit = 64 * 1024;
static const uint64_t cacheLimit = 32 * 1024 * 1024;
static const uint64_t diskLimit = 64 * 1024 * 1024;
static const uint64_t logLimit = 256 * 1024;
static const uint64_t callNanoseconds = 3000ULL * 1000 * 1000;
static const uint64_t terminationReserve = 150ULL * 1000 * 1000;

static uint64_t Now(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) return 0;
    return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
}

static BOOL Before(uint64_t deadline) {
    uint64_t now = Now();
    return now && now < deadline;
}

static void Pause(uint64_t deadline) {
    uint64_t now = Now();
    if (!now || now >= deadline) return;
    uint64_t duration = deadline - now;
    if (duration > 5000000ULL) duration = 5000000ULL;
    struct timespec value = {0, (long)duration};
    // EINTR simply returns to a loop which rechecks the absolute deadline.
    nanosleep(&value, NULL);
}

static BOOL HashString(id value) {
    if (![value isKindOfClass:[NSString class]] || [value length] != 64) return NO;
    for (NSUInteger i = 0; i < 64; ++i) {
        unichar c = [value characterAtIndex:i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return NO;
    }
    return YES;
}

static NSString *Hash(const void *bytes, size_t size) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    char text[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    CC_SHA256(bytes, (CC_LONG)size, digest);
    for (unsigned i = 0; i < sizeof(digest); ++i)
        snprintf(text + i * 2, 3, "%02x", digest[i]);
    return [NSString stringWithUTF8String:text];
}

static BOOL Integer(id object, uint64_t *number) {
    if (![object isKindOfClass:[NSNumber class]] ||
        CFGetTypeID((CFTypeRef)object) != CFNumberGetTypeID() ||
        CFNumberIsFloatType((CFNumberRef)object) || [object longLongValue] < 0)
        return NO;
    *number = [object unsignedLongLongValue];
    return YES;
}

static BOOL StrictBoolean(id object, BOOL expected) {
    return [object isKindOfClass:[NSNumber class]] &&
        CFGetTypeID((CFTypeRef)object) == CFBooleanGetTypeID() &&
        [object boolValue] == expected;
}

static BOOL Version(id value, uint32_t parts[3]) {
    if (![value isKindOfClass:[NSArray class]] || [value count] != 3) return NO;
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t number;
        if (!Integer(value[i], &number) || number > UINT32_MAX) return NO;
        parts[i] = (uint32_t)number;
    }
    return YES;
}

// Apple's downgrade pass maps a macOS library to macosx13.0 and a Mac Catalyst
// library to the matching ios16.0-macabi; it never changes the platform.
static BOOL SupportedTarget(id target) {
    return [target isEqual:@"air64-apple-macosx13.0.0"] ||
        [target isEqual:@"air64-apple-ios16.0.0-macabi"];
}

static BOOL SupportedVersions(NSDictionary *record) {
    uint32_t air[3], metal[3];
    return Version(record[@"afterAIR"], air) && air[0] == 2 && air[1] == 5 && !air[2] &&
        Version(record[@"afterMetal"], metal) && !metal[2] &&
        ((metal[0] == 1 && metal[1] <= 2) || (metal[0] == 2 && metal[1] <= 4) ||
         (metal[0] == 3 && !metal[1]));
}

// Adapt the project's measured single-function MTLB v2 reader. The worker
// performs full AIR parsing; these independent bounds also bind NAME/TYPE.
typedef struct { const unsigned char *name; size_t nameSize; uint8_t type; } Function;
static uint16_t U16(const unsigned char *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t U32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t U64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static BOOL Range(uint64_t at, uint64_t size, size_t total) {
    return at <= total && size <= total - at;
}

static BOOL ReadFunction(const void *pointer, size_t size, Function *function) {
    const unsigned char *data = pointer;
    memset(function, 0, sizeof(*function));
    if (!data || size < 88 || size > libraryLimit || memcmp(data, "MTLB", 4) ||
        U16(data + 6) != 2 || U64(data + 16) != size) return NO;
    uint64_t offsets[4] = {U64(data + 24), U64(data + 40), U64(data + 56), U64(data + 72)};
    uint64_t lengths[4] = {U64(data + 32), U64(data + 48), U64(data + 64), U64(data + 80)};
    if (lengths[0] < 8 || lengths[0] > size || !Range(offsets[0], 4, size)) return NO;
    lengths[0] += 4;
    for (unsigned i = 0; i < 4; ++i) {
        if (offsets[i] < 88 || !Range(offsets[i], lengths[i], size)) return NO;
        for (unsigned j = 0; j < i; ++j)
            if (lengths[i] && lengths[j] && offsets[i] < offsets[j] + lengths[j] &&
                offsets[j] < offsets[i] + lengths[i]) return NO;
    }
    if (lengths[3] < 4 || U32(data + offsets[0]) != 1 ||
        U32(data + offsets[0] + 4) != lengths[0] - 4) return NO;
    size_t cursor = (size_t)offsets[0] + 8, end = (size_t)(offsets[0] + lengths[0]);
    unsigned char tags[64][4]; unsigned count = 0;
    const unsigned char *airOffsets = NULL, *airLength = NULL;
    BOOL ended = NO, haveType = NO;
    while (end - cursor >= 4) {
        const unsigned char *tag = data + cursor; cursor += 4;
        if (!memcmp(tag, "ENDT", 4)) { ended = YES; break; }
        if (end - cursor < 2 || count == 64) return NO;
        for (unsigned i = 0; i < count; ++i) if (!memcmp(tag, tags[i], 4)) return NO;
        memcpy(tags[count++], tag, 4);
        uint16_t length = U16(data + cursor); cursor += 2;
        if (length > end - cursor) return NO;
        if (!memcmp(tag, "NAME", 4)) {
            if (!length || data[cursor + length - 1]) return NO;
            size_t nameSize = length;
            while (nameSize && !data[cursor + nameSize - 1]) --nameSize;
            if (!nameSize || nameSize > 1024 || memchr(data + cursor, 0, nameSize)) return NO;
            function->name = data + cursor; function->nameSize = nameSize;
        } else if (!memcmp(tag, "TYPE", 4)) {
            if (length != 1 || data[cursor] > 3) return NO;
            function->type = data[cursor]; haveType = YES;
        } else if (!memcmp(tag, "OFFT", 4)) {
            if (length != 24) return NO;
            airOffsets = data + cursor;
        } else if (!memcmp(tag, "MDSZ", 4)) {
            if (length != 8) return NO;
            airLength = data + cursor;
        }
        cursor += length;
    }
    if (!ended || cursor != end || !function->name || !haveType || !airOffsets) return NO;
    uint64_t airAt = U64(airOffsets + 16);
    if (airAt > lengths[3]) return NO;
    uint64_t airSize = airLength ? U64(airLength) : lengths[3] - airAt;
    if (airSize < 4 || airSize > lengths[3] - airAt) return NO;
    const unsigned char *air = data + offsets[3] + airAt;
    return !memcmp(air, "\xde\xc0\x17\x0b", 4) || !memcmp(air, "BC\xc0\xde", 4);
}

static NSString *FunctionHex(Function function) {
    char text[2049];
    for (size_t i = 0; i < function.nameSize; ++i)
        snprintf(text + i * 2, 3, "%02x", function.name[i]);
    return [NSString stringWithUTF8String:text];
}

static BOOL SameStat(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
        a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
        a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
        a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
        a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
}

static NSData *ReadLimited(int directory, const char *name, size_t limit, uint64_t deadline) {
    if (!Before(deadline)) return nil;
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    struct stat before, after;
    if (fd < 0) return nil;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_uid != geteuid() ||
        before.st_nlink != 1 || (before.st_mode & 0777) != 0600 ||
        before.st_size <= 0 || (uint64_t)before.st_size > limit) { close(fd); return nil; }
    size_t size = (size_t)before.st_size, done = 0;
    unsigned char *bytes = malloc(size);
    if (!bytes) { close(fd); return nil; }
    BOOL valid = YES;
    while (done < size) {
        if (!Before(deadline)) { valid = NO; break; }
        size_t chunk = size - done; if (chunk > 64 * 1024) chunk = 64 * 1024;
        ssize_t count = read(fd, bytes + done, chunk);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { valid = NO; break; }
        done += (size_t)count;
    }
    unsigned char extra;
    ssize_t tail = valid ? read(fd, &extra, 1) : -1;
    valid = valid && tail == 0 && !fstat(fd, &after) && SameStat(&before, &after) && Before(deadline);
    close(fd);
    if (!valid) { free(bytes); return nil; }
    return [[[NSData alloc] initWithBytesNoCopy:bytes length:size freeWhenDone:YES] autorelease];
}

static BOOL ResultValid(NSData *data, NSData *metadata, Function inputFunction,
                        NSString *inputHash, size_t inputSize,
                        NSString *inputPath, NSString *outputPath) {
    Function output;
    if (!ReadFunction(data.bytes, data.length, &output) ||
        output.nameSize != inputFunction.nameSize || output.type != inputFunction.type ||
        memcmp(output.name, inputFunction.name, output.nameSize)) return NO;
    id record = [NSJSONSerialization JSONObjectWithData:metadata options:0 error:NULL];
    if (![record isKindOfClass:[NSDictionary class]]) return NO;
    uint64_t schema, inputBytes, outputBytes, type;
    if (!Integer(record[@"schema"], &schema) || schema != 1 ||
        !Integer(record[@"inputBytes"], &inputBytes) || inputBytes != inputSize ||
        !Integer(record[@"outputBytes"], &outputBytes) || outputBytes != data.length ||
        !Integer(record[@"functionType"], &type) || type > 3 || type != output.type ||
        ![record[@"inputSHA256"] isEqual:inputHash] || !HashString(record[@"outputSHA256"]) ||
        ![record[@"outputSHA256"] isEqual:Hash(data.bytes, data.length)] ||
        ![record[@"compiler"] isEqual:[NSString stringWithUTF8String:compilerPath]] ||
        ![record[@"input"] isEqual:inputPath] || ![record[@"output"] isEqual:outputPath] ||
        !SupportedTarget(record[@"afterTarget"]) || !SupportedVersions(record) ||
        ![record[@"function"] isKindOfClass:[NSString class]] || ![record[@"function"] length] ||
        ![record[@"functionBytesHex"] isEqual:FunctionHex(output)]) return NO;
    // The helper's JSON writer encodes each original NAME byte as Latin-1.
    NSString *name = [[[NSString alloc] initWithBytes:output.name length:output.nameSize
                                          encoding:NSISOLatin1StringEncoding] autorelease];
    if (!name || ![record[@"function"] isEqual:name]) return NO;
    for (NSString *key in @[@"appleDowngradeReturnedTrue", @"appleAIRVerifierAfter",
                            @"appleLLVMVerifierBeforeAndAfter", @"writerBitcodeDoubleVerified",
                            @"functionNameAndTypePreserved"])
        if (!StrictBoolean(record[key], YES)) return NO;
    return StrictBoolean(record[@"metadataManuallyChanged"], NO) && StrictBoolean(record[@"gpuWorkSubmitted"], NO);
}

// The signed helper has a separate, honest normalization protocol for this
// single measured private entry. Other private AIR functions stay rejected.
static BOOL PrivateVertexFunction(Function function) {
    static const unsigned char name[] = "air.vertexFetchFunction";
    return function.name && function.type == 0 &&
        function.nameSize == sizeof(name) - 1 &&
        !memcmp(function.name, name, sizeof(name) - 1);
}

static BOOL PrivateAIRFunction(Function function) {
    return function.name && function.nameSize >= 4 &&
        !memcmp(function.name, "air.", 4);
}

// A stage-in pipeline of any kind gets an Apple-generated vertex fetch entry.
static BOOL FetchFunction(Function function) {
    static const unsigned char name[] = "air.vertexFetchFunction";
    return function.name && function.nameSize == sizeof(name) - 1 &&
        !memcmp(function.name, name, sizeof(name) - 1);
}

static BOOL ResultValidPrivateVertex(NSData *data, NSData *metadata,
                                     Function inputFunction, NSString *inputHash,
                                     size_t inputSize, NSString *inputPath,
                                     NSString *outputPath) {
    if (!PrivateVertexFunction(inputFunction) || !HashString(inputHash)) return NO;
    Function output;
    NSString *expectedName = [@"vz_pvg_vertex_" stringByAppendingString:inputHash];
    const char *expectedBytes = expectedName.UTF8String;
    size_t expectedSize = [expectedName lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    if (!expectedBytes || !ReadFunction(data.bytes, data.length, &output) ||
        output.type != 0 || output.nameSize != expectedSize ||
        memcmp(output.name, expectedBytes, expectedSize)) return NO;
    id record = [NSJSONSerialization JSONObjectWithData:metadata options:0 error:NULL];
    if (![record isKindOfClass:[NSDictionary class]]) return NO;
    uint64_t schema, inputBytes, outputBytes, inputType, outputType;
    if (!Integer(record[@"schema"], &schema) || schema != 2 ||
        ![record[@"conversionMode"] isEqual:@"private-vertex-loader-normalization"] ||
        !Integer(record[@"inputBytes"], &inputBytes) || inputBytes != inputSize ||
        !Integer(record[@"outputBytes"], &outputBytes) || outputBytes != data.length ||
        !Integer(record[@"inputFunctionType"], &inputType) || inputType != 0 ||
        !Integer(record[@"functionType"], &outputType) || outputType != 0 ||
        ![record[@"inputFunction"] isEqual:@"air.vertexFetchFunction"] ||
        ![record[@"inputFunctionBytesHex"] isEqual:FunctionHex(inputFunction)] ||
        ![record[@"function"] isEqual:expectedName] ||
        ![record[@"functionBytesHex"] isEqual:FunctionHex(output)] ||
        ![record[@"inputSHA256"] isEqual:inputHash] || !HashString(record[@"outputSHA256"]) ||
        ![record[@"outputSHA256"] isEqual:Hash(data.bytes, data.length)] ||
        ![record[@"compiler"] isEqual:[NSString stringWithUTF8String:compilerPath]] ||
        ![record[@"input"] isEqual:inputPath] || ![record[@"output"] isEqual:outputPath] ||
        !SupportedTarget(record[@"afterTarget"]) ||
        !SupportedVersions(record)) return NO;
    for (NSString *key in @[@"appleDowngradeReturnedTrue", @"appleAIRVerifierAfter",
                            @"appleLLVMVerifierBeforeAndAfter", @"writerBitcodeDoubleVerified",
                            @"typePreserved", @"metadataManuallyChanged",
                            @"metadataNormalizationPerformed",
                            @"normalizationInstructionTextIdentical",
                            @"normalizationNumericVersionsUnchanged",
                            @"normalizationBindingsPreserved",
                            @"normalizedStrictAIRVerified", @"normalizedLLVMVerified",
                            @"normalizationEntryAndReferencesRenamed"])
        if (!StrictBoolean(record[key], YES)) return NO;
    return StrictBoolean(record[@"functionNameAndTypePreserved"], NO) &&
        StrictBoolean(record[@"namePreserved"], NO) &&
        StrictBoolean(record[@"gpuWorkSubmitted"], NO);
}

// vz-air-normalize repairs a measured shape in the module text, then runs the
// same Apple pass, verifiers and writer. Its evidence names what it changed.
static const uint64_t normalizerVersion = 1;
static BOOL ResultValidNormalized(NSData *data, NSData *metadata, Function inputFunction,
                                  NSString *inputHash, size_t inputSize,
                                  NSString *inputPath, NSString *outputPath) {
    if (PrivateAIRFunction(inputFunction) && !FetchFunction(inputFunction)) return NO;
    NSData *expected = FetchFunction(inputFunction)
        ? [[@"vz_pvg_vertex_" stringByAppendingString:inputHash] dataUsingEncoding:NSUTF8StringEncoding]
        : [NSData dataWithBytes:inputFunction.name length:inputFunction.nameSize];
    Function output;
    if (!HashString(inputHash) || !expected || !ReadFunction(data.bytes, data.length, &output) ||
        output.type != inputFunction.type || output.nameSize != expected.length ||
        memcmp(output.name, expected.bytes, expected.length)) return NO;
    id record = [NSJSONSerialization JSONObjectWithData:metadata options:0 error:NULL];
    if (![record isKindOfClass:[NSDictionary class]]) return NO;
    uint64_t schema, version, inputBytes, outputBytes, type, converts, hidden;
    if (!Integer(record[@"schema"], &schema) || schema != 3 ||
        ![record[@"conversionMode"] isEqual:@"air-normalization"] ||
        !Integer(record[@"normalizerVersion"], &version) || version != normalizerVersion ||
        !Integer(record[@"inputBytes"], &inputBytes) || inputBytes != inputSize ||
        !Integer(record[@"outputBytes"], &outputBytes) || outputBytes != data.length ||
        !Integer(record[@"functionType"], &type) || type != output.type ||
        ![record[@"inputSHA256"] isEqual:inputHash] ||
        ![record[@"outputSHA256"] isEqual:Hash(data.bytes, data.length)] ||
        ![record[@"compiler"] isEqual:[NSString stringWithUTF8String:compilerPath]] ||
        ![record[@"input"] isEqual:inputPath] || ![record[@"output"] isEqual:outputPath] ||
        ![record[@"inputFunctionBytesHex"] isEqual:FunctionHex(inputFunction)] ||
        ![record[@"functionBytesHex"] isEqual:FunctionHex(output)] ||
        !Integer(record[@"convertCallsRewritten"], &converts) ||
        !Integer(record[@"hiddenBuffersNormalized"], &hidden) || (!converts && !hidden) ||
        !StrictBoolean(record[@"entryRenamed"], FetchFunction(inputFunction)) ||
        !SupportedTarget(record[@"afterTarget"]) || !SupportedVersions(record)) return NO;
    for (NSString *key in @[@"appleDowngradeReturnedTrue", @"appleAIRVerifierAfter", @"appleLLVMVerifierAfter"])
        if (!StrictBoolean(record[key], YES)) return NO;
    return StrictBoolean(record[@"gpuWorkSubmitted"], NO);
}

static BOOL ResultValidForInput(NSData *data, NSData *metadata, Function inputFunction,
                               NSString *inputHash, size_t inputSize,
                               NSString *inputPath, NSString *outputPath) {
    id record = metadata ? [NSJSONSerialization JSONObjectWithData:metadata options:0 error:NULL] : nil;
    uint64_t schema;
    if ([record isKindOfClass:[NSDictionary class]] && Integer(record[@"schema"], &schema) && schema == 3)
        return ResultValidNormalized(data, metadata, inputFunction, inputHash, inputSize,
                                     inputPath, outputPath);
    if (PrivateVertexFunction(inputFunction))
        return ResultValidPrivateVertex(data, metadata, inputFunction, inputHash,
                                        inputSize, inputPath, outputPath);
    if (PrivateAIRFunction(inputFunction)) return NO;
    return ResultValid(data, metadata, inputFunction, inputHash, inputSize,
                       inputPath, outputPath);
}

#include "modern_pvg_shader_persistent.inc"

static BOOL TrustedFile(int directory, const char *name) {
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    struct stat value;
    BOOL valid = fd >= 0 && !fstat(fd, &value) && S_ISREG(value.st_mode) &&
        value.st_uid == 0 && value.st_size > 0 && value.st_nlink == 1 &&
        (value.st_mode & 07777) == 0755;
    if (fd >= 0) close(fd);
    return valid;
}

static int OpenWork(void) {
    int code = open(codeDirectory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat value;
    if (code < 0) return -1;
    if (fstat(code, &value) || !S_ISDIR(value.st_mode) || value.st_uid != 0 ||
        (value.st_mode & 07777) != 0755 || !TrustedFile(code, "metal-library-convert") ||
        !TrustedFile(code, "VM27GPUCompiler.dylib")) { close(code); return -1; }
    int work = openat(code, "work", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(code);
    if (work < 0) return -1;
    if (fstat(work, &value) || !S_ISDIR(value.st_mode) || value.st_uid != 501 ||
        value.st_gid != 501 || (value.st_mode & 07777) != 0700) { close(work); return -1; }
    return work;
}

#include "fair_lease.inc"
static int Coordinate(int work, uint64_t deadline, FairLease *lease) {
    BOOL created = YES;
    int fd = openat(work, "worker.lock", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 && errno == EEXIST) {
        created = NO;
        fd = openat(work, "worker.lock", O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    }
    struct stat value;
    if (fd < 0) return -1;
    // A root diagnostic caller may create the coordination file; the actual
    // mobile GPU tasks must still be able to open that same fixed file later.
    if (created && geteuid() == 0 && fchown(fd, 501, 501)) { close(fd); return -1; }
    if (fstat(fd, &value) || !S_ISREG(value.st_mode) || value.st_uid != 501 ||
        value.st_gid != 501 || value.st_nlink != 1 || (value.st_mode & 07777) != 0600) {
        close(fd); return -1;
    }
    if (!FairCreate(work, deadline, lease)) { close(fd); return -1; }
    while (Before(deadline)) {
        int turn = FairHasTurn(lease, deadline);
        if (turn < 0) break;
        if (!turn) { Pause(deadline); continue; }
        if (!flock(fd, LOCK_EX | LOCK_NB)) return fd;
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) break;
        Pause(deadline);
    }
    close(fd); return -1;
}

static DIR *DirectoryStream(int directory) {
    int fd = openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return NULL;
    DIR *stream = fdopendir(fd);
    if (!stream) close(fd);
    return stream;
}

static BOOL RoomForJob(int work, uint64_t deadline) {
    DIR *stream = DirectoryStream(work);
    if (!stream) return NO;
    unsigned count = 0, visited = 0;
    BOOL valid = YES;
    struct dirent *entry;
    while ((entry = readdir(stream))) {
        if (!Before(deadline) || ++visited > 64) { valid = NO; break; }
        if (!strncmp(entry->d_name, "job-", 4) && ++count >= 4) break;
    }
    closedir(stream);
    // Never reclaim an older directory by a PID embedded in its name.
    return valid && count < 4;
}

static BOOL WorkerTemporaryName(const char *name) {
    static const char prefix[]=".metallib-convert-";
    size_t prefixSize=sizeof(prefix)-1;
    if(strlen(name)!=prefixSize+16+4||memcmp(name,prefix,prefixSize)||strcmp(name+prefixSize+16,".tmp"))return NO;
    for(unsigned i=0;i<16;i++) {
        char c=name[prefixSize+i];
        if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return NO;
    }
    return YES;
}
static BOOL DiskWithinBounds(int directory) {
    DIR *stream=DirectoryStream(directory);
    if(!stream)return NO;
    struct dirent *entry;unsigned count=0,visited=0;uint64_t total=0;
    struct {dev_t device;ino_t inode;} seen[16];BOOL valid=YES;
    while(1) {
        errno=0;entry=readdir(stream);
        if(!entry){if(errno){valid=NO;}break;}
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        if(++visited>32){valid=NO;break;}
#ifdef VZ_DISK_SCAN_REPRO
        VZ_DISK_SCAN_REPRO(directory,entry->d_name);
#endif
        struct stat value;errno=0;
        if(fstatat(directory,entry->d_name,&value,AT_SYMLINK_NOFOLLOW)) {
            int error=errno;
            // Only the genuine helper's atomic temporary filename can vanish
            // between readdir and stat as renameat publishes its final file.
            if(error==ENOENT&&WorkerTemporaryName(entry->d_name))continue;
            valid=NO;break;
        }
        if(!S_ISREG(value.st_mode)||value.st_uid!=geteuid()||value.st_nlink!=1||
           (value.st_mode&0777)!=0600||value.st_size<0||(uint64_t)value.st_size>libraryLimit||
           (!strcmp(entry->d_name,"worker.log")&&(uint64_t)value.st_size>logLimit)) {
            valid=NO;break;
        }
        BOOL duplicate=NO;
        for(unsigned i=0;i<count;i++)if(seen[i].device==value.st_dev&&seen[i].inode==value.st_ino)duplicate=YES;
        // A renamed inode can appear once under the temporary name and once
        // under the published name in a live directory enumeration.
        if(duplicate)continue;
        if(count==16||(uint64_t)value.st_size>diskLimit-total) {
            valid=NO;break;
        }
        seen[count].device=value.st_dev;seen[count].inode=value.st_ino;++count;
        total+=(uint64_t)value.st_size;
    }
    closedir(stream);return valid;
}

static void RemoveJob(int directory, int work, const char *name) {
    struct stat opened, linked;
    if (directory < 0 || work < 0 || fstat(directory, &opened) ||
        fstatat(work, name, &linked, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(linked.st_mode) ||
        opened.st_dev != linked.st_dev || opened.st_ino != linked.st_ino ||
        opened.st_uid != geteuid()) return;
    DIR *stream = DirectoryStream(directory);
    if (!stream) return;
    struct dirent *entry; unsigned count = 0;
    while ((entry = readdir(stream))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (++count > 32) break;
        // unlinkat never follows a symlink. Unexpected subdirectories remain
        // quarantined and count towards the four-leftover-job bound.
        unlinkat(directory, entry->d_name, 0);
    }
    closedir(stream);
    if (!fstatat(work, name, &linked, AT_SYMLINK_NOFOLLOW) &&
        opened.st_dev == linked.st_dev && opened.st_ino == linked.st_ino)
        unlinkat(work, name, AT_REMOVEDIR);
}

@interface VZModernShaderJob : NSObject {
@public
    int directory, work, coordination;
    FairLease fair;
    pid_t pid;
    char name[NAME_MAX + 1];
    dispatch_source_t observer;
    BOOL exitObserved, detached, finished, reaped, waitUnavailable, reapScheduled;
}
- (void)finish;
- (void)detach;
- (void)reap;
@end

@implementation VZModernShaderJob
- (id)init {
    if ((self = [super init])) { directory = work = coordination = -1; FairInit(&fair); }
    return self;
}
- (void)finish {
    int d, w, c; dispatch_source_t source;
    @synchronized (self) {
        if (finished) return;
        finished = YES;
        d = directory; w = work; c = coordination; source = observer;
        observer = NULL;
        directory = work = coordination = -1;
    }
    if (source) {
        dispatch_source_cancel(source);
        // Drop the job's reference now: the source handler retains this job,
        // so waiting for -dealloc to release the source would form a cycle.
        dispatch_release(source);
    }
    RemoveJob(d, w, name);
    if (d >= 0) close(d);
    if (c >= 0) close(c);
    if (w >= 0) close(w);
    FairRelease(&fair);
}
- (void)detach {
    BOOL queue;
    @synchronized (self) {
        detached = YES;
        queue = !reapScheduled && (exitObserved || !observer);
        if (queue) reapScheduled = YES;
    }
    // Only this owner may waitpid for the spawn from now on. The observer is
    // tied to the original process; allocation failure uses the same one-owner
    // nonblocking reaper instead. Neither path ever sends a delayed signal.
    if (queue) dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool { [self reap]; }
    });
}
- (void)reap {
    BOOL unavailable, exited;
    @synchronized (self) {
        if (finished) return;
        unavailable = reaped || waitUnavailable;
        exited = exitObserved || reaped || (waitUnavailable && !observer);
    }
    if (unavailable) {
        // Once reaped/ECHILD, no future numeric PID lookup can target a new
        // child with a reused number. Directory cleanup still needs exit proof.
        if (exited) [self finish];
        return;
    }
    int status = 0;
    pid_t rc = waitpid(pid, &status, WNOHANG);
    if (rc == pid) {
        @synchronized (self) { reaped = YES; exitObserved = YES; }
        [self finish]; return;
    }
    if (rc < 0 && errno == ECHILD) {
        @synchronized (self) { waitUnavailable = YES; if (!observer) exitObserved = YES; }
        [self finish]; return;
    }
    // A PROC_EXIT event may precede the child's final waitable state. No
    // utility thread blocks, and exactly one future reap remains scheduled.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5000000),
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            @autoreleasepool { [self reap]; }
        });
}
- (void)dealloc {
    if (observer) dispatch_release(observer);
    // Normally -finish already closed these. An incomplete pre-spawn setup
    // can still reach -dealloc; never remove a running child's directory here.
    if (directory >= 0) close(directory);
    if (coordination >= 0) close(coordination);
    if (work >= 0) close(work);
    FairRelease(&fair);
    [super dealloc];
}
@end

static void ObserveExit(VZModernShaderJob *job) {
    job->observer = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)job->pid,
        DISPATCH_PROC_EXIT, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (!job->observer) return;
    dispatch_source_set_event_handler(job->observer, ^{
        @autoreleasepool {
            BOOL reap;
            @synchronized (job) {
                job->exitObserved = YES;
                reap = job->detached && !job->reapScheduled;
                if (reap) job->reapScheduled = YES;
            }
            if (reap) [job reap];
        }
    });
    dispatch_resume(job->observer);
}

static BOOL WriteInput(int directory, const void *bytes, size_t size, uint64_t deadline) {
    int fd = openat(directory, "input.metallib", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return NO;
    const unsigned char *input = bytes; size_t done = 0;
    while (done < size && Before(deadline)) {
        size_t chunk = size - done; if (chunk > 64 * 1024) chunk = 64 * 1024;
        ssize_t count = write(fd, input + done, chunk);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        done += (size_t)count;
    }
    struct stat value;
    BOOL valid = done == size && !fstat(fd, &value) && S_ISREG(value.st_mode) &&
        value.st_uid == geteuid() && value.st_nlink == 1 && (uint64_t)value.st_size == size;
    if (close(fd)) valid = NO;
    return valid && Before(deadline);
}

static int Spawn(VZModernShaderJob *job, const char *helper, const char *input,
                 const char *output, const char *evidence, const char *log) {
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_t actions;
    int rc = posix_spawnattr_init(&attributes);
    if (rc) return rc;
    rc = posix_spawn_file_actions_init(&actions);
    if (rc) { posix_spawnattr_destroy(&attributes); return rc; }
    sigset_t mask, defaults;
    sigemptyset(&mask); sigfillset(&defaults);
    short flags = POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#define ACTION(call) do { if (!rc) rc = (call); } while (0)
    ACTION(posix_spawnattr_setflags(&attributes, flags));
    ACTION(posix_spawnattr_setsigmask(&attributes, &mask));
    ACTION(posix_spawnattr_setsigdefault(&attributes, &defaults));
    // Only this new, nonsensitive lock is inherited beyond stdio. It survives
    // a GPUtask exit until the CPU-only child itself exits; all parent GPU/XPC
    // file descriptors are excluded by CLOEXEC_DEFAULT.
    ACTION(posix_spawn_file_actions_adddup2(&actions, job->coordination, 3));
    ACTION(posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0));
    ACTION(posix_spawn_file_actions_addopen(&actions, 1, log,
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
    ACTION(posix_spawn_file_actions_adddup2(&actions, 1, 2));
    char *argv[] = {(char *)helper, (char *)compilerPath, (char *)input,
                    (char *)output, (char *)evidence, NULL};
    char *environment[] = {"LANG=C", "LC_ALL=C", "TMPDIR=/var/root/VirtualMac2/compiler27/work", NULL};
    if (!rc) rc = posix_spawn(&job->pid, helper, &actions, &attributes, argv, environment);
#undef ACTION
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    return rc;
}

static BOOL Wait(VZModernShaderJob *job, uint64_t runningDeadline, uint64_t finalDeadline) {
    int status = 0;
    while (Before(runningDeadline)) {
        pid_t rc = waitpid(job->pid, &status, WNOHANG);
        if (rc == job->pid) {
            job->reaped = YES;
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (rc < 0 && errno != EINTR) {
            if (errno == ECHILD) job->waitUnavailable = YES;
            [job detach]; return NO;
        }
        if (!DiskWithinBounds(job->directory)) break;
        Pause(runningDeadline);
    }
    // This call alone owns waitpid for this spawn. An already-reaped child or
    // ECHILD never leads to a signal to the old PID. No delayed PID-based kill.
    pid_t rc;
    unsigned interrupted = 0;
    do { rc = waitpid(job->pid, &status, WNOHANG); }
    while (rc < 0 && errno == EINTR && ++interrupted < 3 && Before(finalDeadline));
    if (rc == job->pid) { job->reaped = YES; return NO; }
    if (rc < 0 && errno != EINTR) {
        if (errno == ECHILD) job->waitUnavailable = YES;
        [job detach]; return NO;
    }
    BOOL observed;
    @synchronized (job) { observed = job->exitObserved; }
    if (!observed) kill(job->pid, SIGKILL);
    uint64_t reapDeadline = Now() + 100000000ULL;
    if (reapDeadline > finalDeadline) reapDeadline = finalDeadline;
    while (Before(reapDeadline)) {
        rc = waitpid(job->pid, &status, WNOHANG);
        if (rc == job->pid) { job->reaped = YES; return NO; }
        if (rc < 0 && errno != EINTR) {
            if (errno == ECHILD) job->waitUnavailable = YES;
            [job detach]; return NO;
        }
        Pause(reapDeadline);
    }
    [job detach];
    return NO;
}

// Rejected conversions fall back to the unconverted library without a trace,
// and the job directory is removed. Record why, and keep a bounded number of
// rejected inputs with the helper's log so the conversion can be replayed.
static NSString *const rejectedDirectory = @"/var/root/VirtualMac2/shader-cache/rejected";
static const unsigned rejectedLimit = 64;
static const size_t rejectedInputLimit = 4 * 1024 * 1024;
static __thread const char *rejectReason;
static void Reject(const char *reason) { if (!rejectReason) rejectReason = reason; }

static int RejectedEntry(NSString *hash, BOOL create) {
    if (create && mkdir(rejectedDirectory.fileSystemRepresentation, 0700) && errno != EEXIST) return -1;
    int parent = open(rejectedDirectory.fileSystemRepresentation,
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0) return -1;
    int entry = openat(parent, hash.UTF8String, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (entry < 0 && create) {
        unsigned count = 0;
        DIR *stream = DirectoryStream(parent);
        if (stream) {
            while (readdir(stream)) ++count;
            closedir(stream);
        }
        // "." and ".." are counted; a full or unreadable directory keeps nothing new.
        if (stream && count < rejectedLimit + 2 && !mkdirat(parent, hash.UTF8String, 0700))
            entry = openat(parent, hash.UTF8String, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    close(parent);
    return entry;
}

static void RejectedWrite(int entry, const char *name, const void *bytes, size_t size) {
    int fd = openat(entry, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = write(fd, (const unsigned char *)bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        offset += (size_t)count;
    }
    close(fd);
}

// Copy the helper's own files out of a failed job before the job is removed.
static void KeepRejectedJob(int directory, NSString *hash, const char *prefix) {
    uint64_t now = Now();
    int entry = now ? RejectedEntry(hash, YES) : -1;
    if (entry < 0) return;
    static const char *const names[] = {"worker.log", "evidence.conversion.json", "output.metallib"};
    for (unsigned i = 0; i < 3; ++i) {
        NSData *data = ReadLimited(directory, names[i], rejectedInputLimit, now + 250000000ULL);
        char name[64];
        snprintf(name, sizeof(name), "%s%s", prefix, names[i]);
        if (data) RejectedWrite(entry, name, data.bytes, data.length);
    }
    close(entry);
}

static void ReportRejected(const void *bytes, size_t size, NSString *hash, uint64_t start) {
    Function function;
    BOOL readable = ReadFunction(bytes, size, &function);
    const char *reason = rejectReason ?: "unknown";
    BOOL kept = NO;
    if (size <= rejectedInputLimit) {
        int entry = RejectedEntry(hash, YES);
        if (entry >= 0) {
            RejectedWrite(entry, "input.metallib", bytes, size);
            RejectedWrite(entry, "reason.txt", reason, strlen(reason));
            close(entry);
            kept = YES;
        }
    }
    static unsigned logged;
    if (__sync_fetch_and_add(&logged, 1) >= 64) return;
    uint64_t now = Now();
    fprintf(stderr, "[ModernShaderWorker] rejected input=%s function=%.*s type=%d bytes=%zu "
            "reason=%s elapsedMs=%llu kept=%d\n", hash.UTF8String,
            readable ? (int)function.nameSize : 1, readable ? (const char *)function.name : "?",
            readable ? function.type : -1, size, reason,
            (unsigned long long)(now > start ? (now - start) / 1000000ULL : 0), kept);
}

static NSData *ConvertWith(const char *helper, const char *keepPrefix, const void *bytes,
                           size_t size, NSString *hash, Function function, uint64_t deadline) {
    uint64_t runningDeadline = deadline - terminationReserve;
    NSData *persistent = PersistentTryRead(hash, size, function, runningDeadline, 50000000ULL);
    if (persistent) return persistent;
    VZModernShaderJob *job = [[VZModernShaderJob alloc] init];
    NSData *result = nil;
    @try {
    job->work = OpenWork();
    if (job->work < 0) { Reject("work-directory-unavailable"); return nil; }
    job->coordination = Coordinate(job->work, runningDeadline, &job->fair);
    if (job->coordination < 0) { Reject("timed-out-waiting-for-worker-turn"); return nil; }
    if (!RoomForJob(job->work, runningDeadline)) { Reject("work-directory-full"); return nil; }
    if (!Before(runningDeadline)) { Reject("timed-out-before-spawn"); return nil; }
    persistent = PersistentTryRead(hash, size, function, runningDeadline, 20000000ULL);
    if (persistent) return persistent;
    char directory[PATH_MAX];
    int length = snprintf(directory, sizeof(directory), "%s/job-XXXXXX", workDirectory);
    if (length < 0 || (size_t)length >= sizeof(directory) || !mkdtemp(directory)) {
        Reject("job-directory-create-failed");
        return nil;
    }
    strlcpy(job->name, strrchr(directory, '/') + 1, sizeof(job->name));
    job->directory = openat(job->work, job->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat value;
    if (job->directory < 0 || fstat(job->directory, &value) || value.st_uid != geteuid() ||
        !S_ISDIR(value.st_mode) || (value.st_mode & 07777) != 0700 ||
        !WriteInput(job->directory, bytes, size, runningDeadline)) {
        Reject("job-input-write-failed");
        return nil;
    }
    NSString *path = [NSString stringWithUTF8String:directory];
    NSString *input = [path stringByAppendingPathComponent:@"input.metallib"];
    NSString *output = [path stringByAppendingPathComponent:@"output.metallib"];
    NSString *evidence = [path stringByAppendingPathComponent:@"evidence"];
    NSString *log = [path stringByAppendingPathComponent:@"worker.log"];
    int rc = Before(runningDeadline) ? Spawn(job, helper, input.fileSystemRepresentation,
        output.fileSystemRepresentation, evidence.fileSystemRepresentation, log.fileSystemRepresentation) : ETIMEDOUT;
    if (rc) { Reject("helper-spawn-failed"); return nil; }
    ObserveExit(job);
    BOOL exited = Wait(job, runningDeadline, deadline);
    if (!exited) Reject(Before(runningDeadline) ? "helper-exited-with-failure" : "helper-timed-out");
    else if (job->detached || !Before(deadline)) Reject("timed-out-after-helper");
    else if (!DiskWithinBounds(job->directory)) Reject("job-directory-out-of-bounds");
    if (exited && !job->detached && Before(deadline) && DiskWithinBounds(job->directory)) {
        NSData *data = ReadLimited(job->directory, "output.metallib", libraryLimit, deadline);
        NSData *metadata = ReadLimited(job->directory, "evidence.conversion.json", metadataLimit, deadline);
        if (!data || !metadata) Reject("helper-output-missing");
        else if (!ResultValidForInput(data, metadata, function, hash, size, input, output)) Reject("helper-output-failed-validation");
        else if (!Before(deadline)) Reject("timed-out-during-validation");
        else {
            result = data;
            PersistentAssociateProducer(data, metadata, hash, size, function, path);
        }
    }
    if (!result && !job->detached) KeepRejectedJob(job->directory, hash, keepPrefix);
    return result;
    } @finally {
        if (job->pid > 0 && !job->reaped && !job->detached)
            Wait(job, Now(), deadline);
        if (!job->detached) [job finish];
        [job release];
    }
}

@interface VZModernShaderFlight : NSObject {
@public BOOL complete; NSData *result;
}
@end
@implementation VZModernShaderFlight
- (void)dealloc { [result release]; [super dealloc]; }
@end

static pthread_once_t initializeState = PTHREAD_ONCE_INIT;
static pthread_mutex_t stateLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stateChanged = PTHREAD_COND_INITIALIZER;
static NSMutableDictionary *results, *flights;
static NSMutableArray *insertionOrder;
static uint64_t resultBytes;

static void Initialize(void) {
    results = [[NSMutableDictionary alloc] init];
    flights = [[NSMutableDictionary alloc] init];
    insertionOrder = [[NSMutableArray alloc] init];
}

static void Publish(NSString *key, VZModernShaderFlight *flight, NSData *data) {
    pthread_mutex_lock(&stateLock);
    if (data) {
        while (results.count >= 128 || data.length > cacheLimit - resultBytes) {
            NSString *oldest = insertionOrder[0];
            resultBytes -= [results[oldest] length];
            [results removeObjectForKey:oldest];
            [insertionOrder removeObjectAtIndex:0];
        }
        results[key] = data;
        [insertionOrder addObject:key];
        resultBytes += data.length;
        flight->result = [data retain];
    }
    flight->complete = YES;
    [flights removeObjectForKey:key];
    pthread_cond_broadcast(&stateChanged);
    pthread_mutex_unlock(&stateLock);
}

// Apple's helper converts almost everything as delivered. A few inputs fail its
// pass or its private-entry rules; the normalizer repairs only those shapes.
// The first reason is kept for the report if both fail.
static NSData *Convert(const void *bytes, size_t size, NSString *hash,
                       Function function, uint64_t deadline) {
    NSData *data = nil;
    if (!FetchFunction(function) || PrivateVertexFunction(function))
        data = ConvertWith(workerPath, "", bytes, size, hash, function, deadline);
    else
        Reject("private-fetch-entry-needs-normalization");
    if (data || !Before(deadline)) return data;
    return ConvertWith(normalizerPath, "normalizer-", bytes, size, hash, function, deadline);
}

NSData *VZModernConvertUnknownShader(const void *bytes, size_t size, NSString *inputHash) {
    uint64_t start = Now();
    if (!start || !bytes || size < 88 || size > libraryLimit || !HashString(inputHash)) return nil;
    uint64_t deadline = start + callNanoseconds;
    NSString *key = [[inputHash copy] autorelease];
    Function function;
    rejectReason = NULL;
    if (!ReadFunction(bytes, size, &function)) {
        Reject("not-a-single-function-library");
        ReportRejected(bytes, size, key, start);
        return nil;
    }
    if (PrivateAIRFunction(function) && !FetchFunction(function)) {
        Reject("unsupported-private-air-function");
        ReportRejected(bytes, size, key, start);
        return nil;
    }
    if (![Hash(bytes, size) isEqual:key] || !Before(deadline)) return nil;
    pthread_once(&initializeState, Initialize);
    pthread_mutex_lock(&stateLock);
    NSData *cached = [results[key] retain];
    if (cached) { pthread_mutex_unlock(&stateLock); return [cached autorelease]; }
    VZModernShaderFlight *flight = [flights[key] retain];
    if (flight) {
        while (!flight->complete && Before(deadline)) {
            uint64_t now = Now();
            if (!now || now >= deadline) break;
            uint64_t remaining = deadline - now;
            if (remaining > 5000000ULL) remaining = 5000000ULL;
            struct timespec relative = {0, (long)remaining};
            pthread_cond_timedwait_relative_np(&stateChanged, &stateLock, &relative);
        }
        NSData *data = flight->complete && Before(deadline) ? [flight->result retain] : nil;
        if (!data) Reject(flight->complete ? "shared-conversion-was-rejected" : "timed-out-waiting-for-shared-conversion");
        pthread_mutex_unlock(&stateLock);
        if (!data) ReportRejected(bytes, size, key, start);
        [flight release]; return [data autorelease];
    }
    if (flights.count >= 128) { pthread_mutex_unlock(&stateLock); return nil; }
    flight = [[VZModernShaderFlight alloc] init];
    flights[key] = flight;
    pthread_mutex_unlock(&stateLock);
    NSData *data = nil;
    @try { if (Before(deadline)) data = Convert(bytes, size, key, function, deadline); }
    @catch (NSException *exception) {
        // No conversion exception is promoted into a fabricated Metal error.
        fprintf(stderr, "[ModernShaderWorker] client exception=%s\n", exception.name.UTF8String);
    }
    Publish(key, flight, data);
    [flight release];
    if (!data) ReportRejected(bytes, size, key, start);
    return data;
}

// Address-use analysis. Only libraries that declare argument buffers with
// indirect arguments can hold a guest address, so others are not analyzed.
static NSString *const addressDirectory = @"/var/root/VirtualMac2/shader-cache/address-use";
static const uint64_t addressNanoseconds = 2000ULL * 1000 * 1000;
static NSMutableDictionary *addressResults;
static pthread_mutex_t addressLock = PTHREAD_MUTEX_INITIALIZER;

static int AddressDirectory(void) {
    if (mkdir(addressDirectory.fileSystemRepresentation, 0700) && errno != EEXIST) return -1;
    int fd = open(addressDirectory.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat value;
    if (fd >= 0 && (fstat(fd, &value) || value.st_uid != geteuid() || (value.st_mode & 0777) != 0700)) {
        close(fd);
        return -1;
    }
    return fd;
}

// 0 or 1 from an earlier analysis, -1 when unknown.
static int StoredAddressUse(int directory, NSString *hash) {
    NSData *data = ReadLimited(directory, hash.UTF8String, 4, Now() + 50000000ULL);
    if (data.length == 2 && ((const char *)data.bytes)[1] == '\n') {
        char value = ((const char *)data.bytes)[0];
        if (value == '0' || value == '1') return value - '0';
    }
    return -1;
}

static void StoreAddressUse(int directory, NSString *hash, BOOL uses) {
    char name[96];
    snprintf(name, sizeof(name), ".%s.%d", hash.UTF8String, getpid());
    int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    BOOL written = write(fd, uses ? "1\n" : "0\n", 2) == 2;
    if (close(fd) || !written || renameat(directory, name, directory, hash.UTF8String)) unlinkat(directory, name, 0);
}

// Runs vz-air-normalize --address-use on the library; -1 when it cannot say.
static int AnalyzeAddressUse(int directory, NSString *hash, NSData *library) {
    char input[96], log[96];
    snprintf(input, sizeof(input), ".%s.%d.metallib", hash.UTF8String, getpid());
    snprintf(log, sizeof(log), ".%s.%d.log", hash.UTF8String, getpid());
    int result = -1;
    int fd = openat(directory, input, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    BOOL written = write(fd, library.bytes, library.length) == (ssize_t)library.length;
    if (close(fd) || !written) { unlinkat(directory, input, 0); return -1; }
    NSString *inputPath = [addressDirectory stringByAppendingPathComponent:@(input)];
    NSString *logPath = [addressDirectory stringByAppendingPathComponent:@(log)];
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, logPath.fileSystemRepresentation,
                                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    char *argv[] = {(char *)normalizerPath, "--address-use", (char *)compilerPath,
                    (char *)inputPath.fileSystemRepresentation, NULL};
    char *environment[] = {"LANG=C", "LC_ALL=C", "TMPDIR=/var/root/VirtualMac2/shader-cache/address-use", NULL};
    pid_t pid = -1;
    int rc = posix_spawn(&pid, normalizerPath, &actions, &attributes, argv, environment);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (!rc) {
        uint64_t deadline = Now() + addressNanoseconds;
        int status = 0;
        pid_t waited = 0;
        while (!(waited = waitpid(pid, &status, WNOHANG)) && Before(deadline)) Pause(deadline);
        if (!waited) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
        else if (waited == pid && WIFEXITED(status) && !WEXITSTATUS(status)) {
            NSData *output = ReadLimited(directory, log, 4096, Now() + 50000000ULL);
            NSString *text = output ? [[[NSString alloc] initWithData:output encoding:NSUTF8StringEncoding] autorelease] : nil;
            if ([text containsString:@" address-use=1 "]) result = 1;
            else if ([text containsString:@" address-use=0 "]) result = 0;
        }
    }
    unlinkat(directory, input, 0);
    unlinkat(directory, log, 0);
    return result;
}

bool VZModernShaderUsesAddresses(NSData *library) {
    static const char marker[] = "air.indirect_argument";
    if (library.length < 88 || library.length > libraryLimit ||
        !memmem(library.bytes, library.length, marker, sizeof(marker) - 1)) return false;
    NSString *hash = Hash(library.bytes, library.length);
    pthread_mutex_lock(&addressLock);
    if (!addressResults) addressResults = [[NSMutableDictionary alloc] init];
    NSNumber *known = [[addressResults[hash] retain] autorelease];
    pthread_mutex_unlock(&addressLock);
    if (known) return known.boolValue;
    int directory = AddressDirectory();
    if (directory < 0) return false;
    int uses = StoredAddressUse(directory, hash);
    if (uses < 0) {
        uses = AnalyzeAddressUse(directory, hash, library);
        if (uses >= 0) StoreAddressUse(directory, hash, uses);
    }
    close(directory);
    if (uses < 0) return false;
    pthread_mutex_lock(&addressLock);
    addressResults[hash] = @(uses == 1);
    pthread_mutex_unlock(&addressLock);
    return uses == 1;
}
