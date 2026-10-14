// macOS 27 moved user GPU work to a per-connection XPC executable. iPadOS 16
// does not launch framework XPC services. Preserve the real NSXPC protocol and
// Apple's task implementation, using the same anonymous-endpoint rendezvous
// already exercised by the iPad VMM. No GPU commands or replies are emulated.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "modern_pvg_task.h"
#ifdef VZ_MODERN_TASK_SERVER_COMPAT
#include "native_pvg_cache_path.h"
#include "native_pvg_cache_identity.h"
#endif
#include "modern_pvg_fault_observation.h"
#ifdef VZ_MODERN_TASK_SERVER_COMPAT
#include "modern_pvg_texture_error_observation.h"
#include "modern_pvg_event.h"
#include "modern_pvg_heap.h"
#include "modern_pvg_linear_texture.h"
#include "modern_pvg_texture_preflight.h"
#include "modern_pvg_shader_cache.h"
#include "modern_pvg_hang_diagnostics.h"
#include "modern_pvg_address_guard.h"
#include "modern_pvg_shader_audit.h"
#endif

typedef xpc_object_t VZXPCObject;
extern int sandbox_init_with_parameters(const char *, uint64_t,
                                        const char *const *, char **);

static const char *taskName = "com.apple.gpusw.ParavirtualizedGraphicsGPUTask";
static const char *taskBinary = "/var/root/VirtualMac2/payload/Frameworks/"
    "ParavirtualizedGraphics.framework/Versions/A/XPCServices/"
    "com.apple.gpusw.ParavirtualizedGraphicsGPUTask.xpc/Contents/MacOS/"
    "com.apple.gpusw.ParavirtualizedGraphicsGPUTask";
// Measured in this iPad's libxpc; also used by the working VMM endpoint path.
static const size_t endpointPortOffset = 0x18;
static id taskListener;
static Method serviceListenerMethod;
static IMP originalServiceListener;
static void (*originalResume)(id, SEL);
static id (*originalConnectionInit)(id, SEL, NSString *);
static dispatch_source_t parentMonitor;

// BEGIN isolated child lifecycle candidate
// Lifecycle only for the positive PID returned by this call's posix_spawn.
// During endpoint rendezvous the caller is the sole waitpid owner. On handoff,
// only the PROC_EXIT observer (or its allocation-failure fallback) may reap.
// Process exit is not GPU command completion; no stamp/event/reply is changed.
#include <errno.h>
#include <sys/wait.h>
#include <signal.h>

@interface VZModernGPUTaskChild : NSObject {
@public
    pid_t pid;
    NSString *directory;
    dispatch_source_t observer;
    BOOL started, handedOff, transportConnected, rendezvousFailed;
    BOOL exitObserved, reaped, waitUnavailable, reapScheduled, finished;
    BOOL terminationAttempted;
    int exitStatus, lastWaitError;
}
- (id)initWithDirectory:(const char *)path;
- (void)observeSpawnedPID:(pid_t)spawnedPID;
- (BOOL)pollStartup;
- (void)failRendezvous;
- (void)handoffConnected:(BOOL)connected;
- (void)reap;
- (void)finish;
@end

@implementation VZModernGPUTaskChild
- (id)initWithDirectory:(const char *)path {
    if ((self = [super init])) directory = [[NSString alloc] initWithUTF8String:path];
    return self;
}
- (void)observeSpawnedPID:(pid_t)spawnedPID {
    // Called exactly once after successful posix_spawn, before any child wait.
    // A waitable original child reserves its PID even if it exits immediately.
    if (spawnedPID <= 0 || started) return;
    pid = spawnedPID;
    started = YES;
#ifdef VZ_TASK_CHILD_TEST_NO_OBSERVER
    if (VZ_TASK_CHILD_TEST_NO_OBSERVER) return;
#endif
    observer = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)pid,
        DISPATCH_PROC_EXIT, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (!observer) return;
    dispatch_source_set_event_handler(observer, ^{
        @autoreleasepool {
            BOOL queue = NO;
            @synchronized (self) {
                if (!finished) {
                    exitObserved = YES;
                    queue = handedOff && !reapScheduled;
                    if (queue) reapScheduled = YES;
                }
            }
            // The caller still owns waitpid until handedOff becomes true.
            if (queue) [self reap];
        }
    });
    dispatch_resume(observer);
}
- (pid_t)pollLocked {
    // The synchronous startup caller or the unique asynchronous reaper holds
    // this lock. Reaped/ECHILD is terminal: never look up that numeric PID again.
    if (!started || reaped || waitUnavailable || finished) return -1;
    int status = 0;
    pid_t rc;
    unsigned interrupted = 0;
    do { rc = waitpid(pid, &status, WNOHANG); }
    while (rc < 0 && errno == EINTR && ++interrupted < 3);
    lastWaitError = rc < 0 ? errno : 0;
    if (rc == pid) {
        reaped = YES;
        exitObserved = YES;
        exitStatus = status;
    } else if (rc < 0 && lastWaitError == ECHILD) {
        waitUnavailable = YES;
    }
    return rc;
}
- (BOOL)pollStartup {
    BOOL terminal;
    @synchronized (self) {
        if (!started || handedOff) return NO;
        if (!finished) [self pollLocked];
        terminal = reaped || waitUnavailable;
    }
    if (terminal) [self finish];
    return terminal;
}
- (void)failRendezvous {
    @synchronized (self) {
        if (!started || finished || handedOff) return;
        rendezvousFailed = YES;
        pid_t rc = [self pollLocked];
        // Signal only a presently waitable, unreaped child still owned by this
        // startup call. No signal follows reaped/ECHILD or an unknown error.
        // Before handoff this is the sole wait owner, so an exiting child still
        // reserves the PID until we reap it. There is no delayed numeric kill.
        if (rc == 0 && !exitObserved) {
            terminationAttempted = YES;
            int result = kill(pid, SIGTERM);
            int error = result < 0 ? errno : 0;
            fprintf(stderr, "[ModernGPUTask] child rendezvous-stop pid=%d rc=%d errno=%d directory=%s\n",
                pid, result, error, directory.UTF8String);
        }
    }
    [self handoffConnected:NO];
}
- (void)handoffConnected:(BOOL)connected {
    BOOL queue = NO, terminal = NO;
    @synchronized (self) {
        if (!started || finished || handedOff) return;
        transportConnected = connected;
        handedOff = YES;
        terminal = reaped || waitUnavailable;
        queue = !terminal && !reapScheduled && (exitObserved || !observer);
        if (queue) reapScheduled = YES;
    }
    if (terminal) [self finish];
    else if (queue) dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool { [self reap]; }
    });
}
- (void)reap {
    BOOL terminal;
    @synchronized (self) {
        // Only one chain is scheduled; startup must have transferred ownership.
        if (!handedOff || !reapScheduled || finished) return;
        [self pollLocked];
        terminal = reaped || waitUnavailable;
    }
    if (terminal) { [self finish]; return; }
    // PROC_EXIT can precede the final waitable state. Allocation failure also
    // uses this nonblocking chain. It retains self and never sends any signal.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50000000),
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            @autoreleasepool { [self reap]; }
        });
}
- (void)finish {
    dispatch_source_t source;
    BOOL didReap, unavailable, observed, connected, failed;
    int status, error;
    @synchronized (self) {
        if (finished || (!reaped && !waitUnavailable)) return;
        finished = YES;
        source = observer;
        observer = NULL;
        didReap = reaped; unavailable = waitUnavailable; observed = exitObserved;
        connected = transportConnected; failed = rendezvousFailed;
        status = exitStatus; error = lastWaitError;
    }
    if (source) {
        dispatch_source_cancel(source);
        // The handler retains self; self owning the source until dealloc would
        // be a cycle. Drop this ownership after cancellation, outside the lock.
        dispatch_release(source);
    }
    if (didReap)
        fprintf(stderr, "[ModernGPUTask] child reaped pid=%d rawstatus=%d exit=%d signal=%d connected=%d rendezvousFailed=%d directory=%s\n",
            pid, status, WIFEXITED(status) ? WEXITSTATUS(status) : -1,
            WIFSIGNALED(status) ? WTERMSIG(status) : 0, connected, failed, directory.UTF8String);
    else if (unavailable)
        fprintf(stderr, "[ModernGPUTask] child wait-unavailable pid=%d errno=%d exitObserved=%d connected=%d rendezvousFailed=%d directory=%s\n",
            pid, error, observed, connected, failed, directory.UTF8String);
    // Keep task.log, endpoint and captured libraries for post-exit diagnosis.
}
- (void)dealloc {
    if (observer) dispatch_release(observer);
    [directory release];
    [super dealloc];
}
@end
// END isolated child lifecycle candidate

static BOOL ModernVersion(void) {
    const char *version = getenv("VZ_PVG_BACKEND_VERSION");
    return version && !strcmp(version, "27");
}

static BOOL ServerRole(void) {
    const char *role = getenv("VZ_PVG_TASK_ROLE");
    return role && !strcmp(role, "server");
}

static id AnonymousServiceListener(id cls, SEL selector) {
    (void)selector;
    taskListener = [((id (*)(id, SEL))objc_msgSend)(cls,
        sel_registerName("anonymousListener")) retain];
    // Redirect Apple's main once. Native -resume asks +serviceListener to
    // distinguish the launchd service from an anonymous listener. Restore its
    // real factory before that identity check, and keep our endpoint stable.
    method_setImplementation(serviceListenerMethod, originalServiceListener);
    fprintf(stderr, "[ModernGPUTask] created anonymous listener=%p\n", taskListener);
    return taskListener;
}

static void TaskListenerResume(id listener, SEL selector) {
    fprintf(stderr, "[ModernGPUTask] resume listener=%p expected=%p\n", listener, taskListener);
    (void)VZModernInstallFaultObservation();
    originalResume(listener, selector);
    if (listener != taskListener) return;
    id endpoint = ((id (*)(id, SEL))objc_msgSend)(listener,
        sel_registerName("endpoint"));
    VZXPCObject object = ((VZXPCObject (*)(id, SEL))objc_msgSend)(endpoint,
        sel_registerName("_endpoint"));
    const char *path = getenv("VZ_PVG_TASK_ENDPOINT_FILE");
    if (!object || xpc_get_type(object) != XPC_TYPE_ENDPOINT || !path)
        _exit(70);
    mach_port_t port = *(mach_port_t *)((char *)object + endpointPortOffset);
    FILE *file = fopen(path, "w");
    if (!file || !MACH_PORT_VALID(port)) _exit(70);
    fprintf(file, "%x\n", port);
    fclose(file);
    fprintf(stderr, "[ModernGPUTask] listener ready pid=%d port=0x%x\n", getpid(), port);
    pid_t parent = getppid();
    parentMonitor = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, parent,
        DISPATCH_PROC_EXIT, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (parentMonitor) {
        dispatch_source_set_event_handler(parentMonitor, ^{ _exit(0); });
        dispatch_resume(parentMonitor);
    }
    // serviceListener's resume normally enters xpc_main and never returns.
    dispatch_main();
}

static id TaskConnectionInit(id object, SEL selector, NSString *name) {
    if (!ModernVersion() || ![name isEqualToString:@"com.apple.gpusw.ParavirtualizedGraphicsGPUTask"])
        return originalConnectionInit(object, selector, name);
    (void)VZModernInstallFaultObservation();
    char directory[] = "/tmp/virtualmac27-task-XXXXXX";
    if (!mkdtemp(directory)) return originalConnectionInit(object, selector, name);
    NSString *endpointPath = [NSString stringWithFormat:@"%s/endpoint", directory];
    NSString *logPath = [NSString stringWithFormat:@"%s/task.log", directory];
    NSMutableDictionary *environment = [[[NSProcessInfo.processInfo environment] mutableCopy] autorelease];
    [environment removeObjectForKey:@"DYLD_INSERT_LIBRARIES"];
    environment[@"VZ_PVG_TASK_ROLE"] = @"server";
    environment[@"VZ_PVG_TASK_ENDPOINT_FILE"] = endpointPath;
    environment[@"VZ_PVG_BACKEND_VERSION"] = @"27";
    char **envp = calloc(environment.count + 1, sizeof(char *));
    if (!envp) return originalConnectionInit(object, selector, name);
    NSUInteger index = 0;
    for (NSString *key in environment)
        envp[index++] = strdup([[NSString stringWithFormat:@"%@=%@", key, environment[key]] UTF8String]);
    VZModernGPUTaskChild *child = [[VZModernGPUTaskChild alloc] initWithDirectory:directory];
    if (!child) {
        for (NSUInteger i = 0; envp[i]; ++i) free(envp[i]);
        free(envp);
        return originalConnectionInit(object, selector, name);
    }
    char *argv[] = {(char *)taskBinary, NULL};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, logPath.fileSystemRepresentation,
        O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    pid_t pid = 0;
    int result = posix_spawn(&pid, taskBinary, &actions, NULL, argv, envp);
    posix_spawn_file_actions_destroy(&actions);
    for (NSUInteger i = 0; envp[i]; ++i) free(envp[i]);
    free(envp);
    fprintf(stderr, "[ModernGPUTask] spawn rc=%d pid=%d directory=%s\n", result, pid, directory);
    if (result) {
        [child release];
        return originalConnectionInit(object, selector, name);
    }
    [child observeSpawnedPID:pid];
    mach_port_t childTask = MACH_PORT_NULL;
    kern_return_t kr = task_for_pid(mach_task_self(), pid, &childTask);
    fprintf(stderr, "[ModernGPUTask] task_for_pid kr=%d task=0x%x\n", kr, childTask);
    mach_port_t childPort = MACH_PORT_NULL;
    for (unsigned i = 0; kr == KERN_SUCCESS && i < 100 && !childPort; ++i) {
        FILE *file = fopen(endpointPath.fileSystemRepresentation, "r");
        if (file) { if (fscanf(file, "%x", &childPort) != 1) childPort = 0; fclose(file); }
        if (!childPort) {
            if ([child pollStartup]) break;
            usleep(50000);
        }
    }
    mach_port_t sendRight = MACH_PORT_NULL;
    mach_msg_type_name_t acquiredType = 0;
    if (kr == KERN_SUCCESS && childPort)
        kr = mach_port_extract_right(childTask, childPort, MACH_MSG_TYPE_COPY_SEND,
                                     &sendRight, &acquiredType);
    else kr = KERN_FAILURE;
    if (MACH_PORT_VALID(childTask)) mach_port_deallocate(mach_task_self(), childTask);
    if (kr != KERN_SUCCESS || !MACH_PORT_VALID(sendRight)) {
        fprintf(stderr, "[ModernGPUTask] rendezvous failed kr=%d childPort=0x%x\n", kr, childPort);
        [child failRendezvous];
        [child release];
        return originalConnectionInit(object, selector, name);
    }
    xpc_connection_t shape = xpc_connection_create(NULL,
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    xpc_connection_set_event_handler(shape, ^(VZXPCObject event) { (void)event; });
    xpc_connection_resume(shape);
    VZXPCObject xpcEndpoint = xpc_endpoint_create(shape);
    mach_port_t shapePort = *(mach_port_t *)((char *)xpcEndpoint + endpointPortOffset);
    *(mach_port_t *)((char *)xpcEndpoint + endpointPortOffset) = sendRight;
    mach_port_deallocate(mach_task_self(), shapePort);
    id endpoint = [objc_getClass("NSXPCListenerEndpoint") new];
    ((void (*)(id, SEL, VZXPCObject))objc_msgSend)(endpoint,
        sel_registerName("_setEndpoint:"), xpcEndpoint);
    id connection = ((id (*)(id, SEL, id))objc_msgSend)(object,
        sel_registerName("initWithListenerEndpoint:"), endpoint);
    [endpoint release];
    xpc_release(xpcEndpoint);
    xpc_connection_cancel(shape);
    xpc_release(shape);
    fprintf(stderr, "[ModernGPUTask] connected pid=%d connection=%p\n", pid, connection);
    [child handoffConnected:connection != nil];
    [child release];
    return connection;
}

bool VZModernInstallTaskTransport(void) {
    if (!ModernVersion()) return false;
    Class endpointClass = objc_getClass("NSXPCListenerEndpoint");
    if (!class_getInstanceMethod(endpointClass, sel_registerName("_endpoint")) ||
        !class_getInstanceMethod(endpointClass, sel_registerName("_setEndpoint:"))) return false;
    if (ServerRole()) {
        Class cls = objc_getClass("NSXPCListener");
        // Foundation may specialize its XPC entry points in +initialize.
        // Complete initialization before replacing those entry points.
        ((id (*)(id, SEL))objc_msgSend)(cls, sel_registerName("class"));
        Method service = class_getClassMethod(cls, sel_registerName("serviceListener"));
        Method resume = class_getInstanceMethod(cls, sel_registerName("resume"));
        if (!service || !resume || originalResume) return false;
        serviceListenerMethod = service;
        originalServiceListener = method_setImplementation(service, (IMP)AnonymousServiceListener);
        originalResume = (void (*)(id, SEL))method_setImplementation(resume, (IMP)TaskListenerResume);
    } else {
        Class cls = objc_getClass("NSXPCConnection");
        Method init = class_getInstanceMethod(cls, sel_registerName("initWithServiceName:"));
        if (!init || originalConnectionInit) return false;
        originalConnectionInit = (id (*)(id, SEL, NSString *))method_setImplementation(init, (IMP)TaskConnectionInit);
    }
    fprintf(stderr, "[ModernGPUTask] transport installed role=%s\n", ServerRole() ? "server" : "client");
    return true;
}

#ifdef VZ_MODERN_TASK_SERVER_COMPAT
static id (*nativeComputeDescriptor)(id, SEL, id, NSUInteger, id *, NSError **);
static id (*nativeComputeFunction)(id, SEL, id, NSError **);
static id (*nativeRenderDescriptor)(id, SEL, id, NSUInteger, id *, NSError **);
static id (*nativeRenderSimple)(id, SEL, id, NSError **);
static id (*nativeResourceBufferTrap)(id, SEL);
static unsigned diagnosticCount;

static void LogPipelineError(id descriptor, NSError *error) {
    if (__sync_fetch_and_add(&diagnosticCount, 1) >= 8) return;
    NSString *label = [descriptor respondsToSelector:sel_registerName("label")]
        ? ((id (*)(id, SEL))objc_msgSend)(descriptor, sel_registerName("label")) : nil;
    id vertex = [descriptor respondsToSelector:sel_registerName("vertexFunction")]
        ? ((id (*)(id, SEL))objc_msgSend)(descriptor, sel_registerName("vertexFunction")) : nil;
    id fragment = [descriptor respondsToSelector:sel_registerName("fragmentFunction")]
        ? ((id (*)(id, SEL))objc_msgSend)(descriptor, sel_registerName("fragmentFunction")) : nil;
    id compute = [descriptor respondsToSelector:sel_registerName("computeFunction")]
        ? ((id (*)(id, SEL))objc_msgSend)(descriptor, sel_registerName("computeFunction")) : nil;
    fprintf(stderr, "[ModernGPUTask] native pipeline error class=%s label=%s "
        "vertex=%s fragment=%s compute=%s error=%s\n", object_getClassName(descriptor),
        label.UTF8String ?: "none", [[vertex name] UTF8String] ?: "none",
        [[fragment name] UTF8String] ?: "none", [[compute name] UTF8String] ?: "none",
        error.description.UTF8String ?: "none");
}

static id ComputeDescriptor(id device, SEL selector, id descriptor,
                            NSUInteger options, id *reflection, NSError **error) {
    NSError *local = nil;
    NSError **output = error ?: &local;
    id result = nativeComputeDescriptor(device, selector, descriptor, options, reflection, output);
    if (!result) LogPipelineError(descriptor, *output);
    else VZModernMarkAddressPipeline(result, descriptor);
    return result;
}

static id ComputeFunction(id device, SEL selector, id function, NSError **error) {
    NSError *local = nil;
    NSError **output = error ?: &local;
    id result = nativeComputeFunction(device, selector, function, output);
    if (!result) LogPipelineError(function, *output);
    else VZModernMarkAddressPipeline(result, function);
    return result;
}

// Keep native failures visible when a desktop vertex/fragment shader fails.
// The existing compiler still chooses the pipeline and returns its own error.
static id RenderDescriptor(id device, SEL selector, id descriptor,
                           NSUInteger options, id *reflection, NSError **error) {
    NSError *local = nil;
    NSError **output = error ?: &local;
    id result = nativeRenderDescriptor(device, selector, descriptor, options, reflection, output);
    if (!result) LogPipelineError(descriptor, *output);
    else VZModernMarkAddressPipeline(result, descriptor);
    return result;
}

static id RenderSimple(id device, SEL selector, id descriptor, NSError **error) {
    NSError *local = nil;
    NSError **output = error ?: &local;
    id result = nativeRenderSimple(device, selector, descriptor, output);
    if (!result) LogPipelineError(descriptor, *output);
    else VZModernMarkAddressPipeline(result, descriptor);
    return result;
}

static id ResourceBufferTrap(id resource, SEL selector) {
    if (__sync_fetch_and_add(&diagnosticCount, 1) < 8)
        fprintf(stderr, "[ModernGPUTask] abstract buffer lookup class=%s stack=%s\n",
            object_getClassName(resource), [[NSThread callStackSymbols].description UTF8String]);
    return nativeResourceBufferTrap(resource, selector);
}

static void InstallTaskDiagnostics(id device) {
    Method descriptor = class_getInstanceMethod([device class],
        sel_registerName("newComputePipelineStateWithDescriptor:options:reflection:error:"));
    if (descriptor) nativeComputeDescriptor = (void *)method_setImplementation(descriptor, (IMP)ComputeDescriptor);
    Method function = class_getInstanceMethod([device class],
        sel_registerName("newComputePipelineStateWithFunction:error:"));
    if (function) nativeComputeFunction = (void *)method_setImplementation(function, (IMP)ComputeFunction);
    Method render = class_getInstanceMethod([device class],
        sel_registerName("newRenderPipelineStateWithDescriptor:options:reflection:error:"));
    if (render) nativeRenderDescriptor = (void *)method_setImplementation(render, (IMP)RenderDescriptor);
    Method renderSimple = class_getInstanceMethod([device class],
        sel_registerName("newRenderPipelineStateWithDescriptor:error:"));
    if (renderSimple) nativeRenderSimple = (void *)method_setImplementation(renderSimple, (IMP)RenderSimple);
    Method resource = class_getInstanceMethod(objc_getClass("PGResource"), sel_registerName("getBuffer"));
    if (resource) nativeResourceBufferTrap = (void *)method_setImplementation(resource, (IMP)ResourceBufferTrap);
}

// The Apple executable applies its macOS sandbox profile at launch. This
// separately signed iPad process has the same no-sandbox entitlement as the
// VMM; only its exact macOS-only profile is skipped.
static int TaskSandbox(const char *profile, uint64_t flags,
                       const char *const *parameters, char **error) {
    if (ServerRole() && ModernVersion() && profile && !strcmp(profile, taskName)) {
        fprintf(stderr, "[ModernGPUTask] using signed iPad process sandbox policy\n");
        if (error) *error = NULL;
        return 0;
    }
    static int (*native)(const char *, uint64_t, const char *const *, char **);
    if (!native) native = dlsym(RTLD_NEXT, "sandbox_init_with_parameters");
    return native ? native(profile, flags, parameters, error) : -1;
}
__attribute__((used)) static struct {const void *replacement; const void *original;}
    sandboxInterpose __attribute__((section("__DATA,__interpose"))) =
    {(const void *)TaskSandbox, (const void *)sandbox_init_with_parameters};

static size_t TaskConfstr(int name, char *buffer, size_t capacity) {
    // 0x10002 is _CS_DARWIN_USER_CACHE_DIR, not the temporary directory.
    // Native persistent GPU cache follows runtime UID + pinned GPU identity.
    // The parent provisions this app-private tree; this function never creates,
    // clears or changes its permissions. Per-child endpoint/log/TMPDIR stay as-is.
    int savedErrno = errno;
    char path[VZ_NATIVE_PVG_PATH_CAPACITY];
    enum VZNativePVGCacheSelection selection = VZNativePVGSelectCachePath(
        ServerRole() && ModernVersion(), name, VZ_NATIVE_PVG_CACHE_BASE,
        VZ_NATIVE_PVG_CACHE_SCOPE, geteuid(), 501,
        getenv("VZ_PVG_TASK_ENDPOINT_FILE"), path, sizeof(path));
    if (selection != VZNativePVGCacheNotMapped) {
        size_t length = VZNativePVGCacheConfstr(path, buffer, capacity);
        static unsigned reported;
        if (__sync_fetch_and_add(&reported, 1) == 0)
            fprintf(stderr, "[ModernGPUTask] native user cache selection=%s uid=%u scope=%s path=%s\n",
                selection == VZNativePVGCacheStable ? "stable" : "task-fallback",
                (unsigned)geteuid(), VZ_NATIVE_PVG_CACHE_SCOPE, path);
        errno = savedErrno;
        return length;
    }
    // Preserve native name, buffer, capacity, result and errno on every other
    // backend/role/setting, including _CS_DARWIN_USER_TEMP_DIR and _CS_PATH.
    errno = savedErrno;
    static size_t (*native)(int, char *, size_t);
    if (!native) native = dlsym(RTLD_NEXT, "confstr");
    return native ? native(name, buffer, capacity) : 0;
}
__attribute__((used)) static struct {const void *replacement; const void *original;}
    confstrInterpose __attribute__((section("__DATA,__interpose"))) =
    {(const void *)TaskConfstr, (const void *)confstr};

// This macOS API selects the actual iPad device by its measured registry ID.
id MTLCopyDeviceForRegistryID(uint64_t registryID) {
    id device = MTLCreateSystemDefaultDevice();
    SEL selector = sel_registerName("registryID");
    if (!device || ![device respondsToSelector:selector] ||
        ((uint64_t (*)(id, SEL))objc_msgSend)(device, selector) != registryID) return nil;
    return [device retain];
}

// Exhaustively measured against macOS 27's native private function for all
// 16-bit pixel-format values. This means RGB-only, rather than all RGBA formats.
bool isRGBPixelFormat(uint64_t format) {
    return (format >= 45 && format <= 48) || (format >= 95 && format <= 100) ||
           (format >= 120 && format <= 122);
}

__attribute__((constructor)) static void InstallTaskServerTransport(void) {
    @autoreleasepool {
        VZModernInstallTaskTransport();
        if (ServerRole() && ModernVersion()) {
            (void)VZModernInstallTextureErrorObservation();
            id device = MTLCreateSystemDefaultDevice();
            if (VZModernInstallScheduledEventCompatibility(device))
                fprintf(stderr, "[ModernGPUTask] installed native scheduled-handler event backport\n");
            VZModernInstallLinearTextureCompatibility(device);
            (void)VZModernInstallTexturePreflightCallsiteBridge(device);
            VZModernInstallHeapCompatibility();
            VZModernInstallAddressGuard(device);
            VZModernInstallShaderCompatibilityCache(device);
            VZModernInstallShaderAudit(device);
            InstallTaskDiagnostics(device);
            VZModernInstallHangDiagnostics(device);
        }
    }
}
#endif
