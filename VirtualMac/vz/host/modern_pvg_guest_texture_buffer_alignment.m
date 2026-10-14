#import "modern_pvg_guest_texture_buffer_alignment.h"
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <mach/mach_vm.h>
#else
extern kern_return_t mach_vm_read_overwrite(vm_map_read_t, mach_vm_address_t,
    mach_vm_size_t, mach_vm_address_t, mach_vm_size_t *);
#endif
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif

typedef NSUInteger (*NativeAlignmentGetter)(id, SEL);
typedef NSUInteger (*NativeFormatGetter)(id, SEL, NSUInteger);
typedef void (*NativeDeviceInfoGetter)(id, SEL, uint32_t, uint32_t, uint32_t);
typedef id (*NativeMetalDeviceGetter)(id, SEL);

typedef struct {
    Class deviceClass;
    NativeAlignmentGetter originalMinimum;
    NativeFormatGetter originalPublicBuffer;
    bool usable;
} M2Binding;
typedef struct TextureBufferScope {
    id device;
    const M2Binding *binding;
    NSUInteger requirement;
    uint32_t maxKey, capacity;
    struct TextureBufferScope *previous;
} TextureBufferScope;

static pthread_mutex_t installationLock = PTHREAD_MUTEX_INITIALIZER;
static M2Binding m2Binding;
static bool bindingPublished;
static Class registeredPGDevice;
static NativeDeviceInfoGetter originalDeviceInfo;
static NativeMetalDeviceGetter originalMetalDevice;
static uintptr_t verifiedPVGBase;
static __thread TextureBufferScope *currentScope;
static unsigned startupCount, scopeCount, getterCount;

static const char vmmVarPath[] = "/var/root/VirtualMac2/payload/VirtualMachine.xpc/Contents/MacOS/com.apple.Virtualization.VirtualMachine";
static const char vmmPrivatePath[] = "/private/var/root/VirtualMac2/payload/VirtualMachine.xpc/Contents/MacOS/com.apple.Virtualization.VirtualMachine";
static const char pvgVarPath[] = "/var/root/VirtualMac2/payload/Frameworks/ParavirtualizedGraphics.framework/Versions/A/ParavirtualizedGraphics";
static const char pvgPrivatePath[] = "/private/var/root/VirtualMac2/payload/Frameworks/ParavirtualizedGraphics.framework/Versions/A/ParavirtualizedGraphics";
static const char pvgVarRootPath[] = "/var/root/VirtualMac2/payload/Frameworks/ParavirtualizedGraphics.framework/ParavirtualizedGraphics";
static const char pvgPrivateRootPath[] = "/private/var/root/VirtualMac2/payload/Frameworks/ParavirtualizedGraphics.framework/ParavirtualizedGraphics";
// Device classes on which the native values were measured: the device reports
// a 16 byte linear alignment while every probed color format needs 64 bytes
// for a texture buffer. M2 was measured by the original author, M1 on an
// iPad13,5 running 16.3.1. Each class is bound to its own driver image.
static const struct { const char *className, *driverPath; } measuredDevices[] = {
    {"AGXG14GDevice", "/System/Library/Extensions/AGXMetalG14G.bundle/AGXMetalG14G"},
    {"AGXG13GDevice", "/System/Library/Extensions/AGXMetal13_3.bundle/AGXMetal13_3"},
};
static const char *MeasuredDriverPath(Class cls) {
    const char *name = cls ? class_getName(cls) : NULL;
    for (unsigned n = 0; name && n < sizeof(measuredDevices) / sizeof(measuredDevices[0]); ++n)
        if (!strcmp(name, measuredDevices[n].className)) return measuredDevices[n].driverPath;
    return NULL;
}

// Actual deployed PVG SHA6e1d9945180e8dcf57474b7ecd7bc42a49379e4101bb8db089a4433eae97c92d.
// getDeviceInfo helper key40: BL e2d0 -> selector stub62d00, return PC e2d4.
// Key13 calls a different selector at da44/LRda48 and is never modified here.
static const unsigned char expectedPVGUUID[16] = {
    0x9e,0x6a,0x66,0x7a,0xa7,0x75,0x32,0xde,0x8d,0xd5,0x68,0x72,0xef,0x6a,0x53,0xd9,
};
static const unsigned char expectedVMMUUID[16] = {
    0xba,0xc4,0x51,0xc3,0x70,0xab,0x32,0x59,0xb7,0x7e,0x91,0xf5,0x67,0x3d,0x15,0xe1,
};
static const unsigned char expectedWire40Code[] = {
    0xbf,0xa6,0x00,0x71,0x43,0x01,0x00,0x54,0x7f,0x03,0x14,0x6b,0x02,0x01,0x00,0x54,
    0xe0,0x03,0x16,0xaa,0x8c,0x52,0x01,0x94,0xe8,0x03,0x1b,0x2a,0x7b,0x07,0x00,0x11,
    0x89,0x0f,0x08,0x8b,0x08,0x05,0x80,0x52,0x28,0x01,0x00,0x29,
};
static const unsigned char expectedMinimumStub[] = {
    0x41,0x00,0x00,0x90,0x21,0x0c,0x47,0xf9,0x50,0x00,0x00,0xd0,0x10,0xa2,0x14,0x91,
    0x11,0x02,0x40,0xf9,0x30,0x0a,0x1f,0xd7,0x1f,0x20,0x03,0xd5,0x1f,0x20,0x03,0xd5,
};
static const unsigned char expectedGetInfoEntry[] = {
    0x7f,0x23,0x03,0xd5,0xf6,0x57,0xbd,0xa9,0xf4,0x4f,0x01,0xa9,0xfd,0x7b,0x02,0xa9,
    0xfd,0x83,0x00,0x91,0xf5,0x03,0x03,0xaa,0xf3,0x03,0x02,0xaa,0xf4,0x03,0x00,0xaa,
};

static bool SafeRead(uintptr_t address, void *destination, size_t size) {
    if (!address || !destination || !size || size > 32768 || address > UINTPTR_MAX-size)
        return false;
    mach_vm_size_t copied = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size,
        (mach_vm_address_t)(uintptr_t)destination, &copied) == KERN_SUCCESS && copied == size;
}
static bool SafeString(const char *pointer, char output[PATH_MAX]) {
    if (!pointer || !vm_page_size || vm_page_size > (1U<<20)) return false;
    uintptr_t begin = (uintptr_t)pointer;
    for (size_t copied=0; copied<PATH_MAX;) {
        if (begin > UINTPTR_MAX-copied) return false;
        uintptr_t at = begin+copied;
        size_t size = (size_t)vm_page_size-at%vm_page_size;
        if (size>128) size=128;
        if (size>PATH_MAX-copied) size=PATH_MAX-copied;
        if (!SafeRead(at, output+copied, size)) return false;
        for (size_t n=0;n<size;++n) if (!output[copied+n]) return copied+n>0;
        copied+=size;
    }
    return false;
}
static bool Contains(uint64_t start, uint64_t length, uint64_t address, uint64_t size) {
    return size && length && address>=start && address-start<=length && size<=length-(address-start);
}
static bool SameRegularFiles(const char *a, const char *b) {
    struct stat first={0}, second={0};
    return !stat(a,&first) && !stat(b,&second) && S_ISREG(first.st_mode) &&
        S_ISREG(second.st_mode) && first.st_dev==second.st_dev && first.st_ino==second.st_ino;
}
static void *UnsignedIMP(IMP imp) {
#if __has_feature(ptrauth_calls)
    return ptrauth_strip(imp,ptrauth_key_function_pointer);
#else
    return (void *)imp;
#endif
}
static bool NativeImage(IMP imp, const char *expected) {
    Dl_info info={0};char path[PATH_MAX];
    return imp && dladdr(UnsignedIMP(imp),&info) && info.dli_fbase &&
        SafeString(info.dli_fname,path) && !strcmp(path,expected);
}

static bool ReadCommands(uintptr_t base, uint32_t kind,
                         struct mach_header_64 *header, unsigned char **commands) {
    *commands=NULL;
    if (!SafeRead(base,header,sizeof(*header)) || header->magic!=MH_MAGIC_64 ||
        header->cputype!=CPU_TYPE_ARM64 || header->filetype!=kind ||
        !header->ncmds || header->ncmds>128 || !header->sizeofcmds ||
        header->sizeofcmds>32768 || base>UINTPTR_MAX-sizeof(*header)-header->sizeofcmds)
        return false;
    *commands=malloc(header->sizeofcmds);
    if (!*commands) return false;
    if (!SafeRead(base+sizeof(*header),*commands,header->sizeofcmds)) {
        free(*commands);*commands=NULL;return false;
    }
    return true;
}
static bool NextCommand(const struct mach_header_64 *header,
                         const unsigned char *commands, size_t offset,
                         struct load_command *command) {
    if (offset>header->sizeofcmds || sizeof(*command)>header->sizeofcmds-offset) return false;
    memcpy(command,commands+offset,sizeof(*command));
    return command->cmdsize>=sizeof(*command) && !((command->cmdsize)%8) &&
        command->cmdsize<=header->sizeofcmds-offset;
}
static bool VerifiedVMMHeader(uintptr_t base) {
    struct mach_header_64 header;unsigned char *commands=NULL;
    if (!ReadCommands(base,MH_EXECUTE,&header,&commands)) return false;
    bool okay=true,found=false;size_t offset=0;
    for (unsigned n=0;okay&&n<header.ncmds;++n) {
        struct load_command command;
        if (!NextCommand(&header,commands,offset,&command)) {okay=false;break;}
        if (command.cmd==LC_UUID) {
            if (found || command.cmdsize!=sizeof(struct uuid_command)) {okay=false;break;}
            found=!memcmp(commands+offset+8,expectedVMMUUID,16);
            if (!found) {okay=false;break;}
        }
        offset+=command.cmdsize;
    }
    okay=okay&&found&&offset==header.sizeofcmds;free(commands);return okay;
}
static bool ActualVMM27(void) {
    const char *version=getenv("VZ_PVG_BACKEND_VERSION"),*role=getenv("VZ_PVG_TASK_ROLE");
    if (!version||strcmp(version,"27")||(role&&strcmp(role,"client")) ||
        getuid()!=501||geteuid()!=501||sizeof(NSUInteger)!=8) return false;
    void *header=dlsym(RTLD_MAIN_ONLY,"_mh_execute_header");
    Dl_info info={0};char path[PATH_MAX];
    if (!header||!dladdr(header,&info)||info.dli_fbase!=header||
        !SafeString(info.dli_fname,path)||
        (strcmp(path,vmmVarPath)&&strcmp(path,vmmPrivatePath)) ||
        !SameRegularFiles(vmmVarPath,vmmPrivatePath)) return false;
    return VerifiedVMMHeader((uintptr_t)header);
}

static bool VerifyPVGGeometryAndBytes(uintptr_t base) {
    struct mach_header_64 header;unsigned char *commands=NULL;
    if (!ReadCommands(base,MH_DYLIB,&header,&commands)) return false;
    bool okay=true,uuid=false,text=false,code=false,stub=false,selrefs=false;
    size_t offset=0;
    for (unsigned n=0;okay&&n<header.ncmds;++n) {
        struct load_command command;
        if (!NextCommand(&header,commands,offset,&command)) {okay=false;break;}
        if (command.cmd==LC_UUID) {
            if (uuid||command.cmdsize!=sizeof(struct uuid_command)) {okay=false;break;}
            uuid=!memcmp(commands+offset+8,expectedPVGUUID,16);
            if (!uuid) {okay=false;break;}
        } else if (command.cmd==LC_SEGMENT_64) {
            struct segment_command_64 segment;
            if (command.cmdsize<sizeof(segment)) {okay=false;break;}
            memcpy(&segment,commands+offset,sizeof(segment));
            if (segment.nsects>128||sizeof(segment)+segment.nsects*sizeof(struct section_64)!=command.cmdsize) {okay=false;break;}
            bool isText=!memcmp(segment.segname,"__TEXT\0\0\0\0\0\0\0\0\0\0",16);
            bool isConst=!memcmp(segment.segname,"__DATA_CONST\0\0\0\0",16);
            if (isText) {
                if (text||segment.vmaddr!=0x100000000ULL||segment.fileoff||
                    segment.vmsize!=0x68000||segment.filesize!=0x68000||
                    (segment.initprot&(VM_PROT_READ|VM_PROT_EXECUTE))!=(VM_PROT_READ|VM_PROT_EXECUTE)||
                    (segment.initprot&VM_PROT_WRITE)) {okay=false;break;}
                text=true;
            }
            for (unsigned k=0;okay&&k<segment.nsects;++k) {
                struct section_64 section;
                memcpy(&section,commands+offset+sizeof(segment)+k*sizeof(section),sizeof(section));
                bool sectionInside=section.size?
                    Contains(segment.vmaddr,segment.vmsize,section.addr,section.size):
                    section.addr>=segment.vmaddr&&section.addr-segment.vmaddr<=segment.vmsize;
                if (memcmp(section.segname,segment.segname,16)||!sectionInside) {okay=false;break;}
                if (isText&&!memcmp(section.sectname,"__text\0\0\0\0\0\0\0\0\0\0",16)) {
                    if (code||!Contains(section.addr,section.size,0x10000e2bcULL,sizeof(expectedWire40Code))||
                        !Contains(section.addr,section.size,0x100024650ULL,sizeof(expectedGetInfoEntry))) {okay=false;break;}
                    code=true;
                }
                if (isText&&!memcmp(section.sectname,"__auth_stubs\0\0\0\0",16)) {
                    if (stub||!Contains(section.addr,section.size,0x100062d00ULL,sizeof(expectedMinimumStub))) {okay=false;break;}
                    stub=true;
                }
                if (isConst&&!memcmp(section.sectname,"__objc_selrefs\0\0",16)) {
                    if (selrefs||!Contains(section.addr,section.size,0x10006ae18ULL,sizeof(SEL))) {okay=false;break;}
                    selrefs=true;
                }
            }
        }
        offset+=command.cmdsize;
    }
    okay=okay&&uuid&&text&&code&&stub&&selrefs&&offset==header.sizeofcmds;
    free(commands);
    unsigned char actualCall[sizeof(expectedWire40Code)],actualStub[sizeof(expectedMinimumStub)],actualEntry[sizeof(expectedGetInfoEntry)];
    SEL actualSelector=NULL;
    return okay&&base<=UINTPTR_MAX-0x6ae18-sizeof(SEL)&&
        SafeRead(base+0xe2bc,actualCall,sizeof(actualCall))&&!memcmp(actualCall,expectedWire40Code,sizeof(actualCall))&&
        SafeRead(base+0x62d00,actualStub,sizeof(actualStub))&&!memcmp(actualStub,expectedMinimumStub,sizeof(actualStub))&&
        SafeRead(base+0x24650,actualEntry,sizeof(actualEntry))&&!memcmp(actualEntry,expectedGetInfoEntry,sizeof(actualEntry))&&
        SafeRead(base+0x6ae18,&actualSelector,sizeof(actualSelector))&&
        actualSelector==sel_registerName("deviceLinearTextureAlignmentBytes");
}
static bool PVGMethodImage(IMP implementation, uintptr_t *base) {
    Dl_info info={0};char path[PATH_MAX];
    if (!implementation||!dladdr(UnsignedIMP(implementation),&info)||!info.dli_fbase||
        !SafeString(info.dli_fname,path)||
        (strcmp(path,pvgVarPath)&&strcmp(path,pvgPrivatePath)&&strcmp(path,pvgVarRootPath)&&strcmp(path,pvgPrivateRootPath))||
        !SameRegularFiles(pvgVarPath,pvgPrivatePath)||!SameRegularFiles(path,pvgVarPath)) return false;
    *base=(uintptr_t)info.dli_fbase;return true;
}

static bool FullABI(Method method, const char *result, const char *const *args,
                      unsigned count, const char *encoding) {
    if (!method||!method_getImplementation(method)||method_getNumberOfArguments(method)!=count||
        !method_getTypeEncoding(method)||strcmp(method_getTypeEncoding(method),encoding)) return false;
    char *type=method_copyReturnType(method);bool okay=type&&!strcmp(type,result);free(type);
    for (unsigned n=0;okay&&n<count;++n) {
        type=method_copyArgumentType(method,n);okay=type&&!strcmp(type,args[n]);free(type);
    }
    return okay;
}
static bool AlignmentValid(NSUInteger value) {return value&&value<=65536&&!(value&(value-1));}
static Method LocalMethod(Class cls, SEL sel) {
    unsigned count=0;Method *methods=class_copyMethodList(cls,&count),found=NULL;
    for (unsigned n=0;n<count;++n) if (method_getName(methods[n])==sel) {found=methods[n];break;}
    free(methods);return found;
}
static bool ReplaceActualMethod(Class cls, SEL sel, IMP original, IMP replacement, const char *encoding) {
    if (class_addMethod(cls,sel,replacement,encoding)) return true;
    Method method=LocalMethod(cls,sel);
    if (!method||method_getImplementation(method)!=original) return false;
    IMP previous=method_setImplementation(method,replacement);
    if (previous==original) return true;
    if (method_getImplementation(method)==replacement) method_setImplementation(method,previous);
    return false;
}
static unsigned ObservationSlot(unsigned *counter, unsigned limit) {
    unsigned before=__atomic_load_n(counter,__ATOMIC_RELAXED);
    while (before<limit) if (__atomic_compare_exchange_n(counter,&before,before+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) return before+1;
    return 0;
}
static uint64_t Now(void) {
    struct timespec t={0};if (clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0) return 0;
    return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
}
static const M2Binding *BindingForDevice(id device) {
    if (!__atomic_load_n(&bindingPublished,__ATOMIC_ACQUIRE)) return NULL;
    // A genuine subclass can inherit this hook. Keep its native forwarding
    // record, but the replacement decision below still requires the exact M2
    // class. Inherited/outside-scope callers must not become exceptions.
    for (Class cls=object_getClass(device);cls;cls=class_getSuperclass(cls))
        if (cls==m2Binding.deviceClass) return &m2Binding;
    return NULL;
}
static NSUInteger SelectMinimum(id device, SEL selector, uintptr_t caller) {
    const M2Binding *binding=BindingForDevice(device);
    if (!binding) @throw [NSException exceptionWithName:NSInternalInconsistencyException
        reason:@"Missing native texture-buffer forwarding record" userInfo:nil];
    // The native getter is called exactly once, even outside the one wire key.
    NSUInteger nativeValue=binding->originalMinimum(device,selector);
    int savedErrno=errno;
    TextureBufferScope *scope=currentScope;
    bool same=scope&&scope->device==device&&scope->binding==binding&&
        object_getClass(device)==binding->deviceClass;
    bool exact=verifiedPVGBase&&caller==verifiedPVGBase+0xe2d4&&
        selector==sel_registerName("deviceLinearTextureAlignmentBytes");
    bool active=same&&exact&&__atomic_load_n(&binding->usable,__ATOMIC_ACQUIRE)&&
        nativeValue==16&&scope->requirement==64&&scope->maxKey>=41&&scope->capacity;
    NSUInteger returned=active?scope->requirement:nativeValue;
    if (same) {
        unsigned slot=ObservationSlot(&getterCount,32);
        if (slot) {
            uint64_t tid=0;bool known=!pthread_threadid_np(NULL,&tid);
            fprintf(stderr,"[ModernGuestTextureBufferAlignment] getter slot=%u wire40Callsite=%u sameDevice=1 native=%llu returned=%llu replacement=%u originalSingleForward=1 maxKey=%u capacity=%u mono=%llu threadKnown=%u threadID=%llu\n",
                slot,exact,(unsigned long long)nativeValue,(unsigned long long)returned,active,
                scope->maxKey,scope->capacity,(unsigned long long)Now(),known,(unsigned long long)tid);
        }
    }
    errno=savedErrno;return returned;
}
__attribute__((noinline)) static NSUInteger ScopedDeviceMinimum(id device, SEL selector) {
    // Capture this wrapper's incoming LR before another C helper changes it.
    void *caller=__builtin_return_address(0);
#if __has_feature(ptrauth_calls)
    caller=ptrauth_strip(caller,ptrauth_key_return_address);
#endif
    return SelectMinimum(device,selector,(uintptr_t)caller);
}

static const M2Binding *PrepareM2Binding(id device) {
    Class cls=object_getClass(device);
    const char *nativeAGXPath=MeasuredDriverPath(cls);
    if (!nativeAGXPath) return NULL;
    pthread_mutex_lock(&installationLock);
    if (__atomic_load_n(&bindingPublished,__ATOMIC_ACQUIRE)) {
        const M2Binding *result=cls==m2Binding.deviceClass&&__atomic_load_n(&m2Binding.usable,__ATOMIC_ACQUIRE)?&m2Binding:NULL;
        pthread_mutex_unlock(&installationLock);return result;
    }
    SEL minimum=sel_registerName("deviceLinearTextureAlignmentBytes");
    SEL publicBuffer=sel_registerName("minimumTextureBufferAlignmentForPixelFormat:");
    const char *minimumArgs[]={"@",":"},*formatArgs[]={"@",":","Q"};
    Method minimumMethod=class_getInstanceMethod(cls,minimum),publicMethod=class_getInstanceMethod(cls,publicBuffer);
    bool abi=FullABI(minimumMethod,"Q",minimumArgs,2,"Q16@0:8")&&
        FullABI(publicMethod,"Q",formatArgs,3,"Q24@0:8Q16");
    if (!abi||!NativeImage(method_getImplementation(minimumMethod),nativeAGXPath)||
        !NativeImage(method_getImplementation(publicMethod),nativeAGXPath)) {
        pthread_mutex_unlock(&installationLock);return NULL;
    }
    m2Binding=(M2Binding){cls,(NativeAlignmentGetter)method_getImplementation(minimumMethod),
        (NativeFormatGetter)method_getImplementation(publicMethod),false};
    __atomic_store_n(&bindingPublished,true,__ATOMIC_RELEASE);
    bool installed=ReplaceActualMethod(cls,minimum,(IMP)m2Binding.originalMinimum,(IMP)ScopedDeviceMinimum,method_getTypeEncoding(minimumMethod));
    __atomic_store_n(&m2Binding.usable,installed,__ATOMIC_RELEASE);
    pthread_mutex_unlock(&installationLock);return installed?&m2Binding:NULL;
}
static const NSUInteger safeColorFormats[]={
    MTLPixelFormatR8Unorm,MTLPixelFormatRG8Unorm,MTLPixelFormatRGBA8Unorm,
    MTLPixelFormatRGBA8Unorm_sRGB,MTLPixelFormatBGRA8Unorm,MTLPixelFormatBGRA8Unorm_sRGB,
    MTLPixelFormatRGBA16Unorm,MTLPixelFormatRGBA16Float,MTLPixelFormatRGBA32Float,
};
static bool MeasureBufferRequirement(id device,const M2Binding *binding,NSUInteger *requirement) {
    if (binding->originalMinimum(device,sel_registerName("deviceLinearTextureAlignmentBytes"))!=16) return false;
    NSUInteger maximum=1;SEL selector=sel_registerName("minimumTextureBufferAlignmentForPixelFormat:");
    for (unsigned n=0;n<sizeof(safeColorFormats)/sizeof(safeColorFormats[0]);++n) {
        NSUInteger value=binding->originalPublicBuffer(device,selector,safeColorFormats[n]);
        if (!AlignmentValid(value)) return false;
        if (value>maximum) maximum=value;
    }
    // Limit this policy to the native public requirement observed on the measured devices.
    if (maximum!=64) return false;
    *requirement=maximum;return true;
}
// The scoped policy below is bound to the one device class it was measured
// on. Report what any other class answers so its values can be compared.
static void ReportUnboundDevice(id device) {
    static unsigned reported;
    if (!device||__sync_fetch_and_add(&reported,1)) return;
    int savedErrno=errno;
    @try {
        SEL minimum=sel_registerName("deviceLinearTextureAlignmentBytes");
        SEL publicBuffer=sel_registerName("minimumTextureBufferAlignmentForPixelFormat:");
        SEL publicLinear=sel_registerName("minimumLinearTextureAlignmentForPixelFormat:");
        char values[384];size_t used=0;values[0]=0;
        for (unsigned n=0;n<sizeof(safeColorFormats)/sizeof(safeColorFormats[0]);++n) {
            long long buffer=[device respondsToSelector:publicBuffer]?(long long)((NativeFormatGetter)objc_msgSend)(device,publicBuffer,safeColorFormats[n]):-1;
            long long linear=[device respondsToSelector:publicLinear]?(long long)((NativeFormatGetter)objc_msgSend)(device,publicLinear,safeColorFormats[n]):-1;
            int count=snprintf(values+used,sizeof(values)-used," %lu:%lld/%lld",(unsigned long)safeColorFormats[n],buffer,linear);
            if (count<0||(size_t)count>=sizeof(values)-used) break;
            used+=(size_t)count;
        }
        fprintf(stderr,"[ModernGuestTextureBufferAlignment] unbound device=%s deviceLinear=%lld format:textureBuffer/linear=%s\n",
            object_getClassName(device),
            [device respondsToSelector:minimum]?(long long)((NativeAlignmentGetter)objc_msgSend)(device,minimum):-1,values);
    } @catch (NSException *exception) {(void)exception;}
    errno=savedErrno;
}
static void ScopedDeviceInfo(id pgDevice,SEL selector,uint32_t maxKey,uint32_t capacity,uint32_t guestDst) {
    int entryErrno=errno;
    id device=nil;const M2Binding *binding=NULL;NSUInteger requirement=0;bool prepared=false;
    TextureBufferScope *previous=currentScope;currentScope=NULL;
    @try {
        // Do not query a device for packets which cannot contain key40 anyway.
        if (maxKey>=41&&capacity) {
            device=[originalMetalDevice(pgDevice,sel_registerName("mtlDevice")) retain];
            binding=PrepareM2Binding(device);
            if (binding) prepared=MeasureBufferRequirement(device,binding,&requirement);
            else ReportUnboundDevice(device);
        }
    } @catch (NSException *exception) {(void)exception;prepared=false;}
    @finally {currentScope=previous;errno=entryErrno;}
    TextureBufferScope scope={device,binding,requirement,maxKey,capacity,previous};
    @try {
        currentScope=prepared?&scope:NULL;
        if (prepared&&ObservationSlot(&scopeCount,8)) {
            int savedErrno=errno;
            fprintf(stderr,"[ModernGuestTextureBufferAlignment] scope nativeDeviceLinear=16 publicTextureBufferRequirement=%llu measuredFormats=9 maxKey=%u capacity=%u key13NativeForward=1\n",
                (unsigned long long)requirement,maxKey,capacity);
            errno=savedErrno;
        }
        // guestDst is a 32-bit guest VA. Never read or write it in this module.
        originalDeviceInfo(pgDevice,selector,maxKey,capacity,guestDst);
    } @finally {
        int originalErrno=errno;currentScope=previous;[device release];errno=originalErrno;
    }
}

static bool PGMethods(Class cls,Method *info,Method *metal) {
    if (!cls||strcmp(class_getName(cls),"_PGDevice")||sizeof(uint32_t)!=4||sizeof(NSUInteger)!=8) return false;
    *info=class_getInstanceMethod(cls,sel_registerName("getDeviceInfo:length:dst:"));
    *metal=class_getInstanceMethod(cls,sel_registerName("mtlDevice"));
    const char *infoArgs[]={"@",":","I","I","I"},*metalArgs[]={"@",":"};
    return FullABI(*info,"v",infoArgs,5,"v28@0:8I16I20I24")&&FullABI(*metal,"@",metalArgs,2,"@16@0:8");
}
static bool InstallScopeMethods(Class cls,uintptr_t base) {
    Method info=NULL,metal=NULL;if (!base||!PGMethods(cls,&info,&metal)) return false;
    pthread_mutex_lock(&installationLock);
    if (registeredPGDevice) {
        bool same=registeredPGDevice==cls&&verifiedPVGBase==base&&method_getImplementation(info)==(IMP)ScopedDeviceInfo;
        pthread_mutex_unlock(&installationLock);return same;
    }
    originalDeviceInfo=(NativeDeviceInfoGetter)method_getImplementation(info);
    originalMetalDevice=(NativeMetalDeviceGetter)method_getImplementation(metal);
    verifiedPVGBase=base;
    bool installed=ReplaceActualMethod(cls,sel_registerName("getDeviceInfo:length:dst:"),
        (IMP)originalDeviceInfo,(IMP)ScopedDeviceInfo,method_getTypeEncoding(info));
    if (installed) registeredPGDevice=cls;
    pthread_mutex_unlock(&installationLock);return installed;
}
bool VZModernInstallGuestTextureBufferAlignmentAdvertisement(Class pgDeviceClass) {
    int savedErrno=errno;bool identity=ActualVMM27(),decoded=false,installed=false;
    Method info=NULL,metal=NULL;uintptr_t base=0,metalBase=0;
    if (identity&&PGMethods(pgDeviceClass,&info,&metal)) {
        decoded=PVGMethodImage(method_getImplementation(info),&base)&&
            (uintptr_t)UnsignedIMP(method_getImplementation(info))==base+0x24650&&
            PVGMethodImage(method_getImplementation(metal),&metalBase)&&metalBase==base&&VerifyPVGGeometryAndBytes(base);
        if (decoded) installed=InstallScopeMethods(pgDeviceClass,base);
    }
    const char *version=getenv("VZ_PVG_BACKEND_VERSION");
    if (version&&!strcmp(version,"27")&&ObservationSlot(&startupCount,1))
        fprintf(stderr,"[ModernGuestTextureBufferAlignment] startup identity=%u pvgCallsiteDecoded=%u installed=%u policy=wire40-native-public-only key13NativeForward=1\n",identity,decoded,installed);
    errno=savedErrno;return installed;
}
