// Diagnostic candidate, separate from the frozen VirtualMac2 production source.
// Exact method ABIs were read from the matching Mac27 PVG/task ObjC metadata.
// Every replacement calls its saved original IMP with the original arguments.
// A reply is wrapped only if the *native Blocks runtime* reports the exact
// audited callback ABI; otherwise the original block is forwarded untouched.
// Callbacks are never invented, suppressed, reordered or converted to success.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <Block.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "modern_pvg_fault_observation.h"

#ifndef VZ_FAULT_LOG_DIRECTORY
#define VZ_FAULT_LOG_DIRECTORY "/var/root/VirtualMac2/diagnostics/pvg-fault-observation"
#endif

// All quotas are per process and per event kind; request traffic cannot spend
// the fault/interrupt/quiesce quotas. Each record is <= 768 bytes.
typedef enum {
    ObsInstall, ObsExec, ObsExecReply, ObsExecReturn, ObsInfo, ObsInfoReply,
    ObsClientExec, ObsClientReply, ObsToken, ObsFault, ObsStamp, ObsFIFOLife,
    ObsFaultEnqueue, ObsInterrupt, ObsFaultRead, ObsDeviceLife, ObsCrash,
    ObsExecReplyPriority, ObsInfoReplyPriority, ObsClientReplyPriority, ObsTokenPriority,
    ObsKindCount
} ObsKind;
static const char *const kindNames[ObsKindCount] = {
    "install", "server-exec", "server-exec-reply", "server-exec-return",
    "server-info", "server-info-reply", "client-exec", "client-completion",
    "native-token-complete", "fifo-fault", "native-stamp-signal", "fifo-lifecycle",
    "native-fault-enqueue", "native-interrupt", "native-fault-read",
    "device-lifecycle", "native-remote-crashed", "server-exec-reply-priority",
    "server-info-reply-priority", "client-completion-priority", "native-token-complete-priority"
};
static const unsigned quotas[ObsKindCount] = {
    64, 256, 256, 256, 128, 128, 256, 256, 256, 768, 128, 256,
    512, 512, 128, 128, 64, 256, 128, 256, 256
};
static atomic_uint_fast64_t kindCounts[ObsKindCount], recordSequence, requests;
static atomic_flag installing = ATOMIC_FLAG_INIT;
static int journalFD = -1;
static const char *role = "client";
static bool configured;
static __thread bool logging;
static const char *(*nativeBlockSignature)(void *);

static uint64_t MonotonicNS(void) {
    int savedErrno = errno;
    struct timespec ts = {0, 0};
    uint64_t result = 0;
    if (!clock_gettime(CLOCK_MONOTONIC, &ts) && ts.tv_sec >= 0 &&
        (uint64_t)ts.tv_sec < UINT64_MAX / UINT64_C(1000000000))
        result = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    errno = savedErrno;
    return result;
}

static bool ReplyTiming(uint64_t start, uint64_t *elapsed) {
    uint64_t now = MonotonicNS();
    bool known = start && now && now >= start;
    *elapsed = known ? now - start : 0;
    return known;
}

static bool PriorityResult(uint32_t result, bool elapsedKnown, uint64_t elapsed) {
    // Native primary-reply control flow treats 1 and 4 differently from other
    // results. 4 is remoteCrashed, not GPU success; retain it as priority too.
    // No result is changed, interpreted as a completion, or fed back natively.
    return result != 1 || (elapsedKnown && elapsed >= UINT64_C(1000000000));
}

static bool Enabled(void) {
    int savedErrno = errno;
    const char *version = getenv("VZ_PVG_BACKEND_VERSION");
    const char *enabled = getenv("VZ_PVG_FAULT_OBSERVE");
    bool result = false;
    if (version && !strcmp(version, "27")) {
        if (enabled) result = !strcmp(enabled, "1");
        else {
            // Frontend launched through uiopen cannot receive a new env var.
            // Root prepares this app-private mobile501 flag while VM is off.
            // Opening with NONBLOCK makes even a rejected FIFO nonblocking.
            int dir = open(VZ_FAULT_LOG_DIRECTORY, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat d;
            if (dir >= 0 && !fstat(dir, &d) && S_ISDIR(d.st_mode) &&
                d.st_uid == geteuid() && (d.st_mode & 0777) == 0700) {
                int fd = openat(dir, "enabled", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
                struct stat before, after;
                if (fd >= 0 && !fstat(fd, &before) && S_ISREG(before.st_mode) &&
                    before.st_uid == geteuid() && (before.st_mode & 0777) == 0600 &&
                    before.st_nlink == 1 && before.st_size == 2) {
                    char bytes[3];
                    ssize_t n = read(fd, bytes, sizeof(bytes));
                    result = n == 2 && bytes[0] == '1' && bytes[1] == '\n' &&
                        !fstat(fd, &after) && S_ISREG(after.st_mode) &&
                        after.st_dev == before.st_dev && after.st_ino == before.st_ino &&
                        after.st_uid == geteuid() && (after.st_mode & 0777) == 0600 &&
                        after.st_nlink == 1 && after.st_size == 2;
                }
                if (fd >= 0) close(fd);
            }
            if (dir >= 0) close(dir);
        }
    }
    errno = savedErrno;
    return result;
}

static void Journal(ObsKind kind, const char *format, ...) {
    int savedErrno = errno;
    if (journalFD < 0 || logging || kind >= ObsKindCount) return;
    uint64_t n = atomic_fetch_add_explicit(&kindCounts[kind], 1, memory_order_relaxed);
    if (n > quotas[kind]) { errno = savedErrno; return; }
    logging = true;
    struct timespec ts = {0, 0};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t tid = 0;
    (void)pthread_threadid_np(NULL, &tid);
    uint64_t seq = atomic_fetch_add_explicit(&recordSequence, 1, memory_order_relaxed) + 1;
    char line[768];
    int prefix = snprintf(line, sizeof(line),
        "[PVGFaultObserve] seq=%" PRIu64 " mono=%lld.%09ld pid=%d tid=%" PRIu64
        " role=%s event=%s n=%" PRIu64 " ", seq, (long long)ts.tv_sec,
        ts.tv_nsec, getpid(), tid, role, kindNames[kind], n + 1);
    if (prefix > 0 && (size_t)prefix < sizeof(line) - 2) {
        size_t used = (size_t)prefix;
        if (n == quotas[kind]) {
            int count = snprintf(line + used, sizeof(line) - used, "quota-exhausted=%u", quotas[kind]);
            if (count > 0) used += (size_t)count < sizeof(line) - used ? (size_t)count : sizeof(line) - used - 1;
        } else {
            va_list args;
            va_start(args, format);
            int count = vsnprintf(line + used, sizeof(line) - used, format, args);
            va_end(args);
            if (count > 0) used += (size_t)count < sizeof(line) - used ? (size_t)count : sizeof(line) - used - 1;
        }
        if (used > sizeof(line) - 2) used = sizeof(line) - 2;
        line[used++] = '\n';
        // One bounded write to a regular file. No pipe, retry loop, lock,
        // dispatch, wait, file flush or original GPU call inside the logger.
        (void)write(journalFD, line, used);
    }
    logging = false;
    errno = savedErrno;
}

static bool OpenJournal(void) {
    if (journalFD >= 0) return true;
    int dir = open(VZ_FAULT_LOG_DIRECTORY, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir >= 0) {
        struct stat st;
        if (!fstat(dir, &st) && S_ISDIR(st.st_mode) && st.st_uid == geteuid() &&
            (st.st_mode & 0777) == 0700) {
            struct timespec ts = {0, 0};
            (void)clock_gettime(CLOCK_MONOTONIC, &ts);
            char name[128];
            int n = snprintf(name, sizeof(name), "pvgfault-%s-%d-%lld-%09ld.log",
                role, getpid(), (long long)ts.tv_sec, ts.tv_nsec);
            if (n > 0 && (size_t)n < sizeof(name)) {
                int fd = openat(dir, name, O_WRONLY | O_APPEND | O_CREAT | O_EXCL |
                    O_NOFOLLOW | O_CLOEXEC, 0600);
                struct stat file;
                if (fd >= 0 && !fstat(fd, &file) && S_ISREG(file.st_mode) &&
                    file.st_uid == geteuid() && file.st_nlink == 1 &&
                    (file.st_mode & 0777) == 0600) journalFD = fd;
                else if (fd >= 0) close(fd);
            }
        }
        close(dir);
    }
    if (journalFD < 0) {
        // GPUtask fd2 is its app-created task.log. A tty/socket/pipe/symlink or
        // other owner is not a diagnostic sink, so observation stays off.
        struct stat st;
        if (!fstat(STDERR_FILENO, &st) && S_ISREG(st.st_mode) &&
            st.st_uid == geteuid() && st.st_nlink == 1 && (st.st_mode & 0022) == 0)
            journalFD = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
    }
    return journalFD >= 0;
}

// Compare the types, not frame-offset spelling, of the audited 64-bit ABI.
// Class annotations on object types do not change the ABI; @? is a block and
// must not compare equal to an ordinary object or a pointer/integer.
static bool SameType(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && strchr("rnNoORV", *a)) ++a;
    while (*b && strchr("rnNoORV", *b)) ++b;
    if (*a == '@' && *b == '@') return (a[1] == '?') == (b[1] == '?');
    return !strcmp(a, b);
}

static bool SignatureMatches(const char *actual, const char *expected) {
    bool matches = false;
    int savedErrno = errno;
    @try {
        if (actual && expected && strnlen(actual, 512) < 512) {
            NSMethodSignature *a = [NSMethodSignature signatureWithObjCTypes:actual];
            NSMethodSignature *b = [NSMethodSignature signatureWithObjCTypes:expected];
            matches = a && b && a.numberOfArguments == b.numberOfArguments &&
                SameType(a.methodReturnType, b.methodReturnType);
            for (NSUInteger i = 0; matches && i < a.numberOfArguments; ++i)
                matches = SameType([a getArgumentTypeAtIndex:i], [b getArgumentTypeAtIndex:i]);
        }
    } @catch (id exception) { (void)exception; matches = false; }
    errno = savedErrno;
    return matches;
}

static bool ReplyMatches(id block, const char *expected, char signature[128]) {
    signature[0] = 0;
    if (!block || !nativeBlockSignature) return false;
    int savedErrno = errno;
    // Use libsystem_blocks' own descriptor and pointer-auth handling rather
    // than interpreting a future/compact/authenticated descriptor ourselves.
    const char *actual = nativeBlockSignature((void *)block);
    if (actual && strnlen(actual, 512) < 512) {
        (void)snprintf(signature, 128, "%s", actual);
        bool valid = SignatureMatches(actual, expected);
        errno = savedErrno;
        return valid;
    }
    errno = savedErrno;
    return false;
}

static uint32_t TaskID(id object) {
    Class cls = object_getClass(object);
    Ivar ivar = class_getInstanceVariable(cls, "_taskID");
    ptrdiff_t offset = ivar ? ivar_getOffset(ivar) : -1;
    if (offset < 0 || (uintptr_t)offset % sizeof(uint32_t) ||
        (size_t)offset + sizeof(uint32_t) > class_getInstanceSize(cls) ||
        !SameType(ivar_getTypeEncoding(ivar), "I")) return UINT32_MAX;
    return __atomic_load_n((uint32_t *)((char *)(void *)object + offset), __ATOMIC_RELAXED);
}

typedef struct { uint32_t index, offset; } FIFOContext;
static Class rootFIFOClass, childFIFOClass;
static Ivar fifoOffsetIvar, childIndexIvar, deviceFaultMaskIvar;
static bool IsKindOfClass(Class actual, Class expected) {
    if (!expected) return false;
    for (Class c = actual; c; c = class_getSuperclass(c)) if (c == expected) return true;
    return false;
}

static bool IvarReadable(Class cls, Ivar ivar, size_t width, const char *type) {
    if (!cls || !ivar) return false;
    ptrdiff_t offset = ivar_getOffset(ivar);
    return offset >= 0 && !((uintptr_t)offset % width) &&
        (size_t)offset + width <= class_getInstanceSize(cls) &&
        (!type || SameType(ivar_getTypeEncoding(ivar), type));
}

static FIFOContext ContextForFIFO(id fifo) {
    FIFOContext result = {UINT32_MAX, UINT32_MAX};
    Class cls = fifo ? object_getClass(fifo) : Nil;
    bool root = IsKindOfClass(cls, rootFIFOClass), child = IsKindOfClass(cls, childFIFOClass);
    if (!root && !child) return result;
    if (root) result.index = 0;
    if (child && IvarReadable(cls, childIndexIvar, 4, "I"))
        result.index = __atomic_load_n((uint32_t *)((char *)(void *)fifo +
            ivar_getOffset(childIndexIvar)), __ATOMIC_RELAXED);
    if (IvarReadable(cls, fifoOffsetIvar, 4, "I"))
        result.offset = __atomic_load_n((uint32_t *)((char *)(void *)fifo +
            ivar_getOffset(fifoOffsetIvar)), __ATOMIC_RELAXED);
    return result;
}

static bool FaultMask(id device, uint64_t *mask) {
    // This is a readonly snapshot of the exact audited atomic<uint64_t> ivar,
    // not an MMIO read/ack or a call that can clear the native fault mask.
    const char *type = "{atomic<unsigned long long>=\"__a_\"{__cxx_atomic_impl<unsigned long long, std::__cxx_atomic_base_impl<unsigned long long>>=\"__a_value\"AQ}}";
    Class cls = device ? object_getClass(device) : Nil;
    if (!IvarReadable(cls, deviceFaultMaskIvar, 8, type) ||
        ivar_getOffset(deviceFaultMaskIvar) != 1200) return false;
    *mask = __atomic_load_n((uint64_t *)((char *)(void *)device + 1200), __ATOMIC_ACQUIRE);
    return true;
}

typedef void (^ExecReply)(uint32_t, id, uint32_t);
typedef void (^InfoReply)(uint32_t);
typedef void (^ClientReply)(uint32_t, id);
// Store as IMP and perform an explicit typed cast at each invocation. Writing
// an IMP through an aliased differently-typed function-pointer slot would skip
// the compiler's arm64e cast/authentication convention and violate C aliasing.
static IMP originalExec, originalInfo, originalClientExec, originalTokenComplete;
static IMP originalFaultOffset, originalFaultStamp, originalStamp;
static IMP originalFIFOQuiesce, originalFIFOResume, originalFaultEnqueue;
static IMP originalInterrupt, originalReadFault, originalPause, originalReset;
static IMP originalCrash, originalResetMap;

// The pointer alone cannot be read after the fact. Record what the native
// error says; nothing is retained and the error is forwarded unchanged.
static void JournalNativeError(const char *source, uint64_t reference, id error) {
    if (!error) return;
    @try {
        if (![error isKindOfClass:[NSError class]]) {
            Journal(ObsFault, "source=%s reference=%" PRIu64 " nativeErrorClass=%s", source, reference,
                    object_getClassName(error));
            return;
        }
        NSString *info = [[(NSError *)error userInfo] description];
        if (info.length > 420) info = [info substringToIndex:420];
        info = [info stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
        Journal(ObsFault, "source=%s reference=%" PRIu64 " errorDomain=%s errorCode=%ld userInfo=%s",
                source, reference, [(NSError *)error domain].UTF8String ?: "?",
                (long)[(NSError *)error code], info.UTF8String ?: "?");
    } @catch (id exception) { (void)exception; }
}

static void ObserveExec(id object, SEL sel, id data, uint32_t commands, uint32_t resources,
                        uint32_t index, uint64_t fifoValue, uint64_t token, id reply) {
    int entryErrno = errno;
    uint64_t start = MonotonicNS();
    uint64_t request = atomic_fetch_add_explicit(&requests, 1, memory_order_relaxed) + 1;
    uint32_t task = TaskID(object);
    char signature[128];
    bool wrap = ReplyMatches(reply, "v28@?0I8@12I20", signature);
    uintptr_t address = (uintptr_t)(void *)object;
    Journal(ObsExec, "request=%" PRIu64 " object=%p task=%u data=%p commands=%u resources=%u"
        " fifoIndex=%u fifoValue=%" PRIu64 " token=%" PRIu64 " reply=%p wrapped=%u signature=%s",
        request, object, task, data, commands, resources, index, fifoValue, token,
        reply, wrap, signature[0] ? signature : "unavailable");
    ExecReply observer = nil;
    if (wrap) {
        ExecReply origReply = (ExecReply)reply;
        @try {
            observer = Block_copy(^(uint32_t result, id backingIDs, uint32_t count) {
                uint64_t elapsed = 0;
                bool timed = ReplyTiming(start, &elapsed);
                Journal(PriorityResult(result, timed, elapsed) ? ObsExecReplyPriority : ObsExecReply,
                    "request=%" PRIu64 " object=%p task=%u fifoIndex=%u"
                    " fifoValue=%" PRIu64 " token=%" PRIu64 " status=%u backingIDs=%p count=%u"
                    " elapsedKnown=%u elapsedNS=%" PRIu64,
                    request, (void *)address, task, index, fifoValue, token, result, backingIDs, count, timed, elapsed);
                origReply(result, backingIDs, count);
            });
        } @catch (id exception) {
            (void)exception;
            Journal(ObsInstall, "request=%" PRIu64 " block-copy-failed=1 original-reply-forwarded=1", request);
        }
    }
    errno = entryErrno;
    @try {
        ((void (*)(id, SEL, id, uint32_t, uint32_t, uint32_t, uint64_t, uint64_t, id))originalExec)(
            object, sel, data, commands, resources, index, fifoValue, token,
            observer ? (id)observer : reply);
        Journal(ObsExecReturn, "request=%" PRIu64 " object=%p task=%u original-returned=1"
            " gpu-completion-inferred=0", request, object, task);
    } @finally {
        int savedErrno = errno;
        if (observer) Block_release(observer);
        errno = savedErrno;
    }
}

static void ObserveInfo(id object, SEL sel, id data, uint32_t commands, uint32_t resources,
                        uint32_t index, uint64_t fifoValue, id reply) {
    int entryErrno = errno;
    uint64_t start = MonotonicNS();
    uint64_t request = atomic_fetch_add_explicit(&requests, 1, memory_order_relaxed) + 1;
    uint32_t task = TaskID(object);
    char signature[128];
    bool wrap = ReplyMatches(reply, "v12@?0I8", signature);
    Journal(ObsInfo, "request=%" PRIu64 " object=%p task=%u data=%p commands=%u resources=%u"
        " fifoIndex=%u fifoValue=%" PRIu64 " reply=%p wrapped=%u signature=%s", request,
        object, task, data, commands, resources, index, fifoValue, reply, wrap,
        signature[0] ? signature : "unavailable");
    uintptr_t address = (uintptr_t)(void *)object;
    InfoReply observer = nil;
    if (wrap) {
        InfoReply origReply = (InfoReply)reply;
        @try {
            observer = Block_copy(^(uint32_t result) {
                uint64_t elapsed = 0;
                bool timed = ReplyTiming(start, &elapsed);
                Journal(PriorityResult(result, timed, elapsed) ? ObsInfoReplyPriority : ObsInfoReply,
                    "request=%" PRIu64 " object=%p task=%u fifoIndex=%u"
                    " fifoValue=%" PRIu64 " status=%u elapsedKnown=%u elapsedNS=%" PRIu64,
                    request, (void *)address, task, index, fifoValue, result, timed, elapsed);
                origReply(result);
            });
        } @catch (id exception) {
            (void)exception;
            Journal(ObsInstall, "request=%" PRIu64 " block-copy-failed=1 original-reply-forwarded=1", request);
        }
    }
    errno = entryErrno;
    @try { ((void (*)(id, SEL, id, uint32_t, uint32_t, uint32_t, uint64_t, id))originalInfo)(
        object, sel, data, commands, resources, index, fifoValue,
        observer ? (id)observer : reply); }
    @finally {
        int savedErrno = errno;
        if (observer) Block_release(observer);
        errno = savedErrno;
    }
}

static void ObserveClientExec(id object, SEL sel, uint32_t commands, id data,
    uint32_t index, id event, uint64_t *value, uint32_t resources, id reply) {
    int entryErrno = errno;
    uint64_t start = MonotonicNS();
    uint64_t request = atomic_fetch_add_explicit(&requests, 1, memory_order_relaxed) + 1;
    uint32_t task = TaskID(object);
    char signature[128];
    bool wrap = ReplyMatches(reply, "v20@?0I8@12", signature);
    Journal(ObsClientExec, "request=%" PRIu64 " object=%p task=%u commands=%u data=%p"
        " fifoIndex=%u fifoEvent=%p fifoValuePointer=%p resources=%u reply=%p wrapped=%u signature=%s",
        request, object, task, commands, data, index, event, value, resources,
        reply, wrap, signature[0] ? signature : "unavailable");
    uintptr_t address = (uintptr_t)(void *)object;
    ClientReply observer = nil;
    if (wrap) {
        ClientReply origReply = (ClientReply)reply;
        @try {
            observer = Block_copy(^(uint32_t result, id error) {
                uint64_t elapsed = 0;
                bool timed = ReplyTiming(start, &elapsed);
                Journal(PriorityResult(result, timed, elapsed) ? ObsClientReplyPriority : ObsClientReply,
                    "request=%" PRIu64 " object=%p task=%u fifoIndex=%u"
                    " status=%u nativeError=%p elapsedKnown=%u elapsedNS=%" PRIu64,
                    request, (void *)address, task, index, result, error, timed, elapsed);
                if (result != 1) JournalNativeError("client-completion", request, error);
                origReply(result, error);
            });
        } @catch (id exception) {
            (void)exception;
            Journal(ObsInstall, "request=%" PRIu64 " block-copy-failed=1 original-reply-forwarded=1", request);
        }
    }
    // The original owns and may update *value. The observer does not dereference
    // or write it; server-exec records the actual value crossing the wire.
    errno = entryErrno;
    @try { ((void (*)(id, SEL, uint32_t, id, uint32_t, id, uint64_t *, uint32_t, id))originalClientExec)(
        object, sel, commands, data, index, event, value,
        resources, observer ? (id)observer : reply); }
    @finally {
        int savedErrno = errno;
        if (observer) Block_release(observer);
        errno = savedErrno;
    }
}

static void ObserveToken(id object, SEL sel, uint64_t token, uint32_t result, id error) {
    Journal(result == 1 ? ObsToken : ObsTokenPriority, "delegate=%p token=%" PRIu64 " status=%u nativeError=%p",
        object, token, result, error);
    if (result != 1) JournalNativeError("native-token-complete", token, error);
    ((void (*)(id, SEL, uint64_t, uint32_t, id))originalTokenComplete)(object, sel, token, result, error);
}

static void ObserveFaultOffset(id fifo, SEL sel, uint32_t offset, uint32_t stamp) {
    int savedErrno = errno;
    FIFOContext c = ContextForFIFO(fifo);
    Journal(ObsFault, "fifo=%p fifoIndex=%u call=faultAtOffset currentOffset=%u faultOffset=%u stamp=%u",
        fifo, c.index, c.offset, offset, stamp);
    errno = savedErrno;
    ((void (*)(id, SEL, uint32_t, uint32_t))originalFaultOffset)(fifo, sel, offset, stamp);
}
static void ObserveFaultStamp(id fifo, SEL sel, uint32_t stamp) {
    int savedErrno = errno;
    FIFOContext c = ContextForFIFO(fifo);
    Journal(ObsFault, "fifo=%p fifoIndex=%u call=faultAtStamp currentOffset=%u stamp=%u",
        fifo, c.index, c.offset, stamp);
    errno = savedErrno;
    ((void (*)(id, SEL, uint32_t))originalFaultStamp)(fifo, sel, stamp);
}
static void ObserveStamp(id fifo, SEL sel, uint32_t stamp) {
    int savedErrno = errno;
    FIFOContext c = ContextForFIFO(fifo);
    Journal(ObsStamp, "fifo=%p fifoIndex=%u currentOffset=%u stamp=%u phase=original-entry original-only=1",
        fifo, c.index, c.offset, stamp);
    errno = savedErrno;
    ((void (*)(id, SEL, uint32_t))originalStamp)(fifo, sel, stamp);
}
static void ObserveQuiesce(id fifo, SEL sel) {
    int savedErrno = errno;
    FIFOContext c = ContextForFIFO(fifo);
    Journal(ObsFIFOLife, "fifo=%p fifoIndex=%u currentOffset=%u call=quiesce phase=entry",
        fifo, c.index, c.offset);
    errno = savedErrno;
    ((void (*)(id, SEL))originalFIFOQuiesce)(fifo, sel);
    Journal(ObsFIFOLife, "fifo=%p fifoIndex=%u call=quiesce phase=original-return", fifo, c.index);
}
static void ObserveResume(id fifo, SEL sel) {
    int savedErrno = errno;
    FIFOContext c = ContextForFIFO(fifo);
    Journal(ObsFIFOLife, "fifo=%p fifoIndex=%u currentOffset=%u call=resume phase=entry",
        fifo, c.index, c.offset);
    errno = savedErrno;
    ((void (*)(id, SEL))originalFIFOResume)(fifo, sel);
}
static void ObserveFaultEnqueue(id device, SEL sel, id fifo) {
    int savedErrno = errno;
    FIFOContext c = ContextForFIFO(fifo);
    Journal(ObsFaultEnqueue, "device=%p fifo=%p fifoIndex=%u currentOffset=%u phase=original-entry",
        device, fifo, c.index, c.offset);
    errno = savedErrno;
    ((void (*)(id, SEL, id))originalFaultEnqueue)(device, sel, fifo);
}
static void ObserveInterrupt(id device, SEL sel) {
    int savedErrno = errno;
    uint64_t mask = 0;
    bool known = FaultMask(device, &mask);
    Journal(ObsInterrupt, "device=%p faultMaskKnown=%u faultMask=0x%" PRIx64
        " phase=original-entry mask-readonly=1", device, known, mask);
    errno = savedErrno;
    ((void (*)(id, SEL))originalInterrupt)(device, sel);
}
static uint32_t ObserveReadFault(id device, SEL sel) {
    uint32_t result = ((uint32_t (*)(id, SEL))originalReadFault)(device, sel);
    Journal(ObsFaultRead, "device=%p originalResult=%u", device, result);
    return result;
}
static void ObservePause(id device, SEL sel) {
    Journal(ObsDeviceLife, "device=%p call=pause phase=entry", device);
    ((void (*)(id, SEL))originalPause)(device, sel);
    Journal(ObsDeviceLife, "device=%p call=pause phase=original-return", device);
}
static void ObserveReset(id device, SEL sel) {
    Journal(ObsDeviceLife, "device=%p call=reset phase=entry", device);
    ((void (*)(id, SEL))originalReset)(device, sel);
    Journal(ObsDeviceLife, "device=%p call=reset phase=original-return", device);
}
static bool ObserveResetMap(id device, SEL sel, id map) {
    Journal(ObsDeviceLife, "device=%p call=resetWithMemoryMap map=%p phase=entry", device, map);
    bool result = ((bool (*)(id, SEL, id))originalResetMap)(device, sel, map);
    Journal(ObsDeviceLife, "device=%p call=resetWithMemoryMap phase=original-return result=%u", device, result);
    return result;
}
static void ObserveCrash(id delegate, SEL sel) {
    Journal(ObsCrash, "delegate=%p original-notification=1", delegate);
    ((void (*)(id, SEL))originalCrash)(delegate, sel);
}

typedef struct { const char *className, *selector, *types; IMP replacement; IMP *original; } Hook;
#define H(c,s,t,fn,orig) {c,s,t,(IMP)fn,&orig}
static const Hook serverHooks[] = {
    H("ParavirtualizedGraphicsGPUTask", "execIndirect3WithData:cmdBufferCount:resourceCount:stampIndex:fifoEventValueIn:gpuCompletion:reply:",
      "v60@0:8@16I24I28I32Q36Q44@?52", ObserveExec, originalExec),
    H("ParavirtualizedGraphicsGPUTask", "infoIndirectWithData:cmdBufferCount:resourceCount:stampIndex:fifoEventValueIn:reply:",
      "v52@0:8@16I24I28I32Q36@?44", ObserveInfo, originalInfo)
};
static const Hook clientHooks[] = {
    H("PGRemoteTask", "doExecIndirectWithCmdBufCount:commandData:stampIndex:fifoEvent:fifoEventValue:resourceCount:completionHandler:",
      "v60@0:8I16@20I28@32^Q40I48@?52", ObserveClientExec, originalClientExec),
    H("PGRemoteTaskDeviceDelegate", "completeAsyncTokenID:success:error:",
      "v36@0:8Q16I24@28", ObserveToken, originalTokenComplete),
    H("PGRemoteTaskDeviceDelegate", "remoteCrashed", "v16@0:8", ObserveCrash, originalCrash),
    H("PGFIFO", "faultAtOffset:stampValue:", "v24@0:8I16I20", ObserveFaultOffset, originalFaultOffset),
    H("PGFIFO", "faultAtStampValue:", "v20@0:8I16", ObserveFaultStamp, originalFaultStamp),
    H("PGFIFO", "signalStampValue:", "v20@0:8I16", ObserveStamp, originalStamp),
    H("PGFIFO", "quiesce", "v16@0:8", ObserveQuiesce, originalFIFOQuiesce),
    H("PGFIFO", "resume", "v16@0:8", ObserveResume, originalFIFOResume),
    H("_PGDevice", "signalFaultInFIFO:", "v24@0:8@16", ObserveFaultEnqueue, originalFaultEnqueue),
    H("_PGDevice", "interrupt", "v16@0:8", ObserveInterrupt, originalInterrupt),
    H("_PGDevice", "readInterruptFault", "I16@0:8", ObserveReadFault, originalReadFault),
    H("_PGDevice", "pause", "v16@0:8", ObservePause, originalPause),
    H("_PGDevice", "reset", "v16@0:8", ObserveReset, originalReset),
    H("_PGDevice", "resetWithMemoryMap:", "B24@0:8@16", ObserveResetMap, originalResetMap)
};
#undef H

static Method OwnMethod(Class cls, SEL selector) {
    unsigned count = 0;
    Method *methods = cls ? class_copyMethodList(cls, &count) : NULL;
    Method found = NULL;
    for (unsigned i = 0; i < count; ++i)
        if (sel_isEqual(method_getName(methods[i]), selector)) { found = methods[i]; break; }
    free(methods);
    return found;
}

bool VZModernInstallFaultObservation(void) {
    if (!Enabled() || atomic_flag_test_and_set_explicit(&installing, memory_order_acquire)) return false;
    int savedErrno = errno;
    bool installed = false;
    @try {
        const char *requestedRole = getenv("VZ_PVG_TASK_ROLE");
        bool server = requestedRole && !strcmp(requestedRole, "server");
        if (!configured) role = server ? "server" : "client";
        if (configured && strcmp(role, server ? "server" : "client")) {
            Journal(ObsInstall, "later-role-change=1 ignored=1");
        } else if (OpenJournal()) {
            if (!configured) {
                nativeBlockSignature = (const char *(*)(void *))dlsym(RTLD_DEFAULT, "_Block_signature");
                rootFIFOClass = objc_getClass("PGRootFIFO");
                childFIFOClass = objc_getClass("PGChildFIFO");
                fifoOffsetIvar = class_getInstanceVariable(objc_getClass("PGFIFO"), "_currentCommandOffset");
                childIndexIvar = class_getInstanceVariable(childFIFOClass, "_stampIndex");
                deviceFaultMaskIvar = class_getInstanceVariable(objc_getClass("_PGDevice"), "_interruptFaultMask");
                // Immutable before the first method is replaced. Repeated
                // NSXPC connections never rewrite metadata being read by a
                // concurrent native callback or interrupt hook.
                configured = true;
            }
            const Hook *hooks = server ? serverHooks : clientHooks;
            size_t count = server ? sizeof(serverHooks)/sizeof(serverHooks[0]) : sizeof(clientHooks)/sizeof(clientHooks[0]);
            for (size_t i = 0; i < count; ++i) {
                const Hook *h = &hooks[i];
                Class cls = objc_getClass(h->className);
                SEL sel = sel_registerName(h->selector);
                // Never mutate an inherited method owned by a different class.
                Method method = OwnMethod(cls, sel);
                const char *encoding = method ? method_getTypeEncoding(method) : NULL;
                if (!method || !SignatureMatches(encoding, h->types)) {
                    Journal(ObsInstall, "class=%s selector=%s installed=0 actual=%s expected=%s",
                        h->className, h->selector, encoding ? encoding : "unavailable", h->types);
                    continue;
                }
                if (method_getImplementation(method) == h->replacement) { installed = true; continue; }
                if (*h->original) {
                    Journal(ObsInstall, "class=%s selector=%s installed=0 later-IMP-change=1",
                        h->className, h->selector);
                    continue;
                }
                // Publish the saved original before publishing the replacement.
                // No lock/flag remains held while any original method executes.
                *h->original = method_getImplementation(method);
                (void)method_setImplementation(method, h->replacement);
                installed = true;
                Journal(ObsInstall, "class=%s selector=%s installed=1 actual=%s savedOriginal=%p"
                    " nativeBlockSignature=%u", h->className, h->selector, encoding,
                    (void *)*h->original, nativeBlockSignature != NULL);
            }
        }
    } @catch (id exception) {
        (void)exception;
        // Observation is optional. Already published hooks keep their original
        // IMPs and remain pure forwarders; no native call is cancelled here.
        Journal(ObsInstall, "installer-exception=1 observation-only=1");
    } @finally {
        atomic_flag_clear_explicit(&installing, memory_order_release);
        errno = savedErrno;
    }
    return installed;
}
