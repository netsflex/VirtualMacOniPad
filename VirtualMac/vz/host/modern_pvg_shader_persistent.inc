// Optional app-owned positive converter cache. Included after the unchanged
// original ResultValidForInput. No Metal device/object/work or fake completion.
#include <objc/runtime.h>
#include "persistent_cache_identity.h"

static const uint64_t persistentReserve = 2500ULL * 1000 * 1000;
static const size_t persistentLibraryLimit = 2 * 1024 * 1024;
static const size_t persistentRecordLimit = 16 * 1024;
static const size_t persistentMetadataLimit = 16 * 1024;
static const uint64_t persistentDiskLimit = 1024ULL * 1024 * 1024;
static const unsigned persistentEntryLimit = 8192;
// Publication runs on a utility queue outside worker.lock; the quota scan is
// linear in the entry count, so its budget grows with the entry limit.
static const uint64_t persistentPublishBudget = 1000000000ULL;
static char persistentProducerKey;

// Fixture overrides exist only in the independent local CPU harness. They are
// never used by the production build and do not attest device/root identity.
#ifdef VZ_PVG_PERSISTENT_TESTING
static NSString *persistentFixtureCompiler, *persistentFixtureReceiptDirectory;
static NSString *persistentFixtureParent, *persistentFixtureWork, *persistentFixtureHash;
#endif
static uid_t PersistentRootUID(void) {
#ifdef VZ_PVG_PERSISTENT_TESTING
    return getuid();
#else
    return 0;
#endif
}
static gid_t PersistentRootGID(void) {
#ifdef VZ_PVG_PERSISTENT_TESTING
    return getgid();
#else
    return 0;
#endif
}
static NSString *PersistentReceiptHash(void) {
#ifdef VZ_PVG_PERSISTENT_TESTING
    return persistentFixtureHash;
#else
    return @VZ_PERSISTENT_RECEIPT_SHA256;
#endif
}
static NSString *PersistentCompilerDirectory(void) {
#ifdef VZ_PVG_PERSISTENT_TESTING
    return persistentFixtureCompiler;
#else
    return [NSString stringWithUTF8String:codeDirectory];
#endif
}
static NSString *PersistentReceiptDirectory(void) {
#ifdef VZ_PVG_PERSISTENT_TESTING
    return persistentFixtureReceiptDirectory;
#else
    return PersistentCompilerDirectory();
#endif
}
static NSString *PersistentParent(void) {
#ifdef VZ_PVG_PERSISTENT_TESTING
    return persistentFixtureParent;
#else
    return @"/var/root/VirtualMac2/converter-cache-v1";
#endif
}
static NSString *PersistentWork(void) {
#ifdef VZ_PVG_PERSISTENT_TESTING
    return persistentFixtureWork;
#else
    return [NSString stringWithUTF8String:workDirectory];
#endif
}
static BOOL PersistentEnabled(void) {
    const char *role = getenv("VZ_PVG_TASK_ROLE"), *backend = getenv("VZ_PVG_BACKEND_VERSION");
    NSString *hash = PersistentReceiptHash();
    return getuid() == 501 && geteuid() == 501 && role && !strcmp(role,"server") &&
        backend && !strcmp(backend,"27") && HashString(hash) &&
        ![hash isEqual:@"0000000000000000000000000000000000000000000000000000000000000000"];
}
static uint64_t PersistentOptionalDeadline(uint64_t runningDeadline, uint64_t cap) {
    uint64_t now = Now();
    if (!now || !cap || runningDeadline <= persistentReserve ||
        now >= runningDeadline - persistentReserve) return 0;
    uint64_t head = runningDeadline - persistentReserve - now;
    if (cap > head) cap = head;
    return cap <= UINT64_MAX - now ? now + cap : 0;
}
static int PersistentDirectory(NSString *path, uid_t owner, mode_t mode) {
    if (!path) return -1;
    int fd = open(path.fileSystemRepresentation,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    struct stat value;
    if (fd < 0) return -1;
    if (fstat(fd,&value) || !S_ISDIR(value.st_mode) || value.st_uid != owner ||
        (value.st_mode & 07777) != mode) { close(fd); return -1; }
    return fd;
}
static NSData *PersistentReceiptBytes(int directory, uint64_t deadline) {
    if (!Before(deadline)) return nil;
    int fd = openat(directory,"persistent-cache-identity-v1.json",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    struct stat before,after;
    if (fd < 0) return nil;
    if (fstat(fd,&before) || !S_ISREG(before.st_mode) || before.st_uid != PersistentRootUID() || before.st_gid != PersistentRootGID() ||
        (before.st_mode & 07777) != 0444 || before.st_nlink != 1 || before.st_size <= 0 ||
        before.st_size > 65536) { close(fd); return nil; }
    size_t length=(size_t)before.st_size,done=0;unsigned char *bytes=malloc(length);
    if (!bytes) { close(fd); return nil; }
    while (done<length && Before(deadline)) {
        ssize_t count=read(fd,bytes+done,length-done);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) break;
        done+=(size_t)count;
    }
    unsigned char extra;ssize_t tail=done==length?read(fd,&extra,1):-1;
    BOOL valid=tail==0 && !fstat(fd,&after) && SameStat(&before,&after) && Before(deadline);
    close(fd);
    if (!valid) { free(bytes);return nil; }
    // This small receipt copy permits unambiguous cleanup even if NSData
    // allocation throws; its original read buffer always has one owner.
    @try { return [NSData dataWithBytes:bytes length:length]; }
    @finally { free(bytes); }
}
static BOOL PersistentStatNumber(NSDictionary *row, NSString *key, uint64_t expected) {
    uint64_t value;return Integer(row[key],&value) && value==expected;
}
static BOOL PersistentStatMatches(NSDictionary *row, struct stat value, uint64_t size) {
    return [row isKindOfClass:[NSDictionary class]] && S_ISREG(value.st_mode) &&
        value.st_uid==PersistentRootUID() && value.st_gid==PersistentRootGID() &&
        (value.st_mode&07777)==0755 && value.st_nlink==1 && (uint64_t)value.st_size==size &&
        PersistentStatNumber(row,@"dev",(uint64_t)value.st_dev) &&
        PersistentStatNumber(row,@"ino",(uint64_t)value.st_ino) &&
        PersistentStatNumber(row,@"size",(uint64_t)value.st_size) &&
        PersistentStatNumber(row,@"uid",value.st_uid) && PersistentStatNumber(row,@"gid",value.st_gid) &&
        PersistentStatNumber(row,@"nlink",value.st_nlink) && PersistentStatNumber(row,@"mode",value.st_mode) &&
        PersistentStatNumber(row,@"flags",value.st_flags) && PersistentStatNumber(row,@"gen",value.st_gen) &&
        PersistentStatNumber(row,@"rdev",(uint64_t)value.st_rdev) &&
        PersistentStatNumber(row,@"blocks",(uint64_t)value.st_blocks) &&
        PersistentStatNumber(row,@"blksize",(uint64_t)value.st_blksize) &&
        PersistentStatNumber(row,@"mtimeSec",(uint64_t)value.st_mtimespec.tv_sec) &&
        PersistentStatNumber(row,@"mtimeNsec",(uint64_t)value.st_mtimespec.tv_nsec) &&
        PersistentStatNumber(row,@"ctimeSec",(uint64_t)value.st_ctimespec.tv_sec) &&
        PersistentStatNumber(row,@"ctimeNsec",(uint64_t)value.st_ctimespec.tv_nsec) &&
        PersistentStatNumber(row,@"birthSec",(uint64_t)value.st_birthtimespec.tv_sec) &&
        PersistentStatNumber(row,@"birthNsec",(uint64_t)value.st_birthtimespec.tv_nsec);
}
static const struct { const char *name,*sha,*cd; uint64_t size; } persistentFiles[] = {
    {"VM27CompilerCompat.dylib","7e60029cb6a27f99dde2ba444403359618de9e7cc31276b72bb8fff4af5e2cb9","26b3426f4ae76bb4900dbe2d2724a4c2dd1123a6",51312},
    {"VM27GPUCompiler.dylib","5bf8d5ad9cc1f478e3557fb6c663a5735e183e28b097de8b6c56ff4aae511bfa","4ee95465f485ae6a5ed4fe7a9c08f49ba42e3cb7",259856},
    {"VM27GPUCompilerImpl.dylib","cb1d702bb4a3c094b8237a020d6b437a092f9272cb325166e8dcce02a4e3ec85","5755dff78889d2b199113b3fc9d242d12b185d2a",16949248},
    {"VM27LLVM.dylib","2140410973ad9d048625f739626398b5972e16a033ce051d016bd2e4cfbf27bb","f9eade69cce3728d83c63f4e668651e185f136fd",106874736},
    {"VM27llvm-flatbuffers.dylib","4589ef19a9037f519580c9fb61b9d51c22a23b2b05b9305428e25e5528d640dd","8a3977a8530372c340c812ad192d39f62d0a7846",415088},
    {"VM27llvm-lmdb.dylib","2e63f8f5506fc76c8091e2b2af2242492dd041221d095770a0603708aba647ae","a4a30fe433a4b6c34d480cd86c1186d8b6875d70",116288},
    {"metal-library-convert","92521f90f368cb7eb9c0d3b5abf3c3cf788f478954a3372651124e8db3a94311","6af8d4c000c4d42303adc01fb32869e4c4a15b54",70448}
};
static BOOL PersistentVerifyIdentity(uint64_t deadline) {
    if (!PersistentEnabled() || !Before(deadline)) return NO;
    int receiptDir=PersistentDirectory(PersistentReceiptDirectory(),PersistentRootUID(),0755);
    if (receiptDir<0) return NO;
    NSData *bytes=nil;
    @try { bytes=PersistentReceiptBytes(receiptDir,deadline); }
    @finally { close(receiptDir); }
    if (!bytes || !Before(deadline) || ![Hash(bytes.bytes,bytes.length) isEqual:PersistentReceiptHash()] || !Before(deadline)) return NO;
    NSDictionary *receipt=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL];
    uint64_t format;
    if (!Before(deadline) || ![receipt isKindOfClass:[NSDictionary class]] ||
        !Integer(receipt[@"formatVersion"],&format) || format!=1 ||
        ![receipt[@"compilerProfileSHA256"] isEqual:@"6ec7684553365a759feb68f2820dd5fcc333d62ca2ccdae9daa7863d48672e59"] ||
        ![receipt[@"compilerProfileFileSHA256"] isEqual:@"cd3846395ae7586c757a0e3de34b1770e97aa33aea529e01e8461710a2894601"] ||
        !StrictBoolean(receipt[@"deviceSystemModified"],NO) ||
        ![receipt[@"files"] isKindOfClass:[NSArray class]] || [receipt[@"files"] count]!=7) return NO;
    int compilerDir=PersistentDirectory(PersistentCompilerDirectory(),PersistentRootUID(),0755);
    if (compilerDir<0) return NO;
    BOOL valid=YES;
    @try {
    for (unsigned i=0;i<7 && valid;++i) {
        NSDictionary *row=receipt[@"files"][i];uint64_t size;
        if (!Before(deadline) || ![row isKindOfClass:[NSDictionary class]] ||
            ![row[@"filename"] isEqual:[NSString stringWithUTF8String:persistentFiles[i].name]] ||
            ![row[@"sha256"] isEqual:[NSString stringWithUTF8String:persistentFiles[i].sha]] ||
            ![row[@"cdhash"] isEqual:[NSString stringWithUTF8String:persistentFiles[i].cd]] ||
            !Integer(row[@"size"],&size) || size!=persistentFiles[i].size) { valid=NO;break; }
        int fd=openat(compilerDir,persistentFiles[i].name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        struct stat value;
        @try { valid=fd>=0 && !fstat(fd,&value) && PersistentStatMatches(row[@"stat"],value,size) && Before(deadline); }
        @finally { if (fd>=0) close(fd); }
    }
    return valid && Before(deadline);
    } @finally { close(compilerDir); }
}
static int PersistentOpen(uint64_t deadline) {
    if (!PersistentVerifyIdentity(deadline)) return -1;
    int parent=PersistentDirectory(PersistentParent(),PersistentRootUID(),0755);
    if (parent<0) return -1;
    int fd=-1;
    @try { fd=openat(parent,PersistentReceiptHash().UTF8String,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK); }
    @finally { close(parent); }
    struct stat value;
    if (fd<0) return -1;
    if (fstat(fd,&value) || !S_ISDIR(value.st_mode) || value.st_uid!=501 ||
        (value.st_mode&07777)!=0700 || !Before(deadline)) { close(fd);return -1; }
    return fd;
}
static BOOL PersistentJobLeaf(NSString *leaf) {
    if (![leaf isKindOfClass:[NSString class]] || leaf.length!=10 || ![leaf hasPrefix:@"job-"]) return NO;
    for (unsigned i=4;i<10;++i) {
        unichar c=[leaf characterAtIndex:i];
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'))) return NO;
    }
    return YES;
}
static BOOL PersistentPaths(NSString *leaf, NSString **input, NSString **output) {
    if (!PersistentJobLeaf(leaf)) return NO;
    NSString *base=[PersistentWork() stringByAppendingPathComponent:leaf];
    *input=[base stringByAppendingPathComponent:@"input.metallib"];
    *output=[base stringByAppendingPathComponent:@"output.metallib"];
    return *input && *output;
}
static NSData *PersistentRead(int parent, NSString *hash, size_t size, Function function, uint64_t deadline) {
    if (!Before(deadline)) return nil;
    int fd=openat(parent,hash.UTF8String,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    struct stat directory;
    if (fd<0) return nil;
    if (fstat(fd,&directory) || !S_ISDIR(directory.st_mode) || directory.st_uid!=501 || (directory.st_mode&07777)!=0700) { close(fd);return nil; }
    NSData *descriptor=nil,*metadata=nil,*data=nil;
    @try {
        descriptor=ReadLimited(fd,"record.json",persistentRecordLimit,deadline);
        metadata=Before(deadline)?ReadLimited(fd,"helper.json",persistentMetadataLimit,deadline):nil;
        data=Before(deadline)?ReadLimited(fd,"output.metallib",persistentLibraryLimit,deadline):nil;
    } @finally { close(fd); }
    if (!descriptor || !metadata || !data || !Before(deadline)) return nil;
    NSDictionary *record=[NSJSONSerialization JSONObjectWithData:descriptor options:0 error:NULL];
    uint64_t format,inputBytes,outputBytes,type;
    if (!Before(deadline) || ![record isKindOfClass:[NSDictionary class]] || record.count!=10 ||
        !Integer(record[@"formatVersion"],&format) || format!=1 ||
        ![record[@"receiptSHA256"] isEqual:PersistentReceiptHash()] || ![record[@"inputSHA256"] isEqual:hash] ||
        !Integer(record[@"inputBytes"],&inputBytes) || inputBytes!=size ||
        !Integer(record[@"outputBytes"],&outputBytes) || outputBytes!=data.length ||
        !Integer(record[@"inputFunctionType"],&type) || type!=function.type ||
        ![record[@"inputFunctionBytesHex"] isEqual:FunctionHex(function)] ||
        !HashString(record[@"outputSHA256"]) || !HashString(record[@"metadataSHA256"])) return nil;
    NSString *input=nil,*output=nil;
    if (!PersistentPaths(record[@"originalJobLeaf"],&input,&output) || !Before(deadline)) return nil;
    if (![Hash(data.bytes,data.length) isEqual:record[@"outputSHA256"]] || !Before(deadline)) return nil;
    if (![Hash(metadata.bytes,metadata.length) isEqual:record[@"metadataSHA256"]] || !Before(deadline)) return nil;
    // Pass only our fixed derived work paths to the unchanged schema validators.
    if (!ResultValidForInput(data,metadata,function,hash,size,input,output) || !Before(deadline)) return nil;
    return data; // Owned immutable NSData with malloc/freeWhenDone; never mmap.
}
static NSData *PersistentTryRead(NSString *hash,size_t size,Function function,uint64_t runningDeadline,uint64_t cap) {
    uint64_t begin=Now();
    uint64_t optional=PersistentOptionalDeadline(runningDeadline,cap);
    if (!optional) return nil;
    int directory=-1;
    @try {
        directory=PersistentOpen(optional);if (directory<0) return nil;
        NSData *result=PersistentRead(directory,hash,size,function,optional);
        if (result) {
            static unsigned logged;
            uint64_t end=Now();
            if (begin && end>=begin && __sync_fetch_and_add(&logged,1)<32)
                fprintf(stderr,"[ModernShaderPersistent] input=%s outcome=owned-data-hit stage=%s bytes=%zu elapsedNanoseconds=%llu receipt=%s\n",
                    hash.UTF8String,cap==50000000ULL?"pre-lock":"post-lock",result.length,
                    (unsigned long long)(end-begin),PersistentReceiptHash().UTF8String);
        }
        return result;
    } @catch (NSException *exception) { (void)exception;return nil; }
    @finally { if (directory>=0) close(directory); }
}
static void PersistentAssociateProducer(NSData *data,NSData *metadata,NSString *hash,size_t size,Function function,NSString *jobPath) {
    @try {
        if (!PersistentEnabled() || !data || !metadata || data.length>persistentLibraryLimit || metadata.length>persistentMetadataLimit) return;
        NSString *leaf=jobPath.lastPathComponent,*input=nil,*output=nil;
        if (!PersistentPaths(leaf,&input,&output) || ![jobPath isEqual:[input stringByDeletingLastPathComponent]]) return;
        NSDictionary *proof=@{@"hash":hash,@"inputBytes":@(size),@"name":[NSData dataWithBytes:function.name length:function.nameSize],
            @"type":@(function.type),@"leaf":leaf,@"metadata":metadata};
        // An atomic retained/autoreleased getter protects another RAM-hit
        // caller while a successful background publisher clears this proof.
        objc_setAssociatedObject(data,&persistentProducerKey,proof,OBJC_ASSOCIATION_RETAIN);
    } @catch (NSException *exception) { (void)exception; }
}
static BOOL PersistentCapacity(int directory,size_t additional,uint64_t deadline) {
    // A dup shares its readdir offset. Open "." so every quota scan starts at
    // the beginning even when this FD has already been inspected.
    int dupfd=openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if (dupfd<0) return NO;
    DIR *stream=fdopendir(dupfd);if (!stream) { close(dupfd);return NO; }
    unsigned count=0;uint64_t total=0;BOOL valid=YES;struct dirent *entry;
    @try {
    while (Before(deadline)) {
        errno=0;entry=readdir(stream);if (!entry) { if (errno) valid=NO;break; }
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..") || !strcmp(entry->d_name,"publisher.lock")) continue;
        NSString *name=[NSString stringWithUTF8String:entry->d_name];
        if (!HashString(name) || ++count>=persistentEntryLimit) { valid=NO;break; }
        int child=openat(directory,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        struct stat value;
        if (child<0 || fstat(child,&value) || !S_ISDIR(value.st_mode) || value.st_uid!=501 || (value.st_mode&07777)!=0700) valid=NO;
        static const char *const files[]={"output.metallib","helper.json","record.json"};
        for (unsigned i=0;child>=0 && valid && i<3;++i) {
            if (fstatat(child,files[i],&value,AT_SYMLINK_NOFOLLOW) || !S_ISREG(value.st_mode) || value.st_uid!=501 || value.st_nlink!=1 || (value.st_mode&07777)!=0600 || value.st_size<=0 || (uint64_t)value.st_size>persistentLibraryLimit) valid=NO;
            else if ((uint64_t)value.st_size>persistentDiskLimit-total) valid=NO;
            else total+=(uint64_t)value.st_size;
        }
        if (child>=0) close(child);if (!valid) break;
    }
    return valid && Before(deadline) && additional<=persistentDiskLimit-total;
    } @finally { closedir(stream); }
}
static BOOL PersistentWrite(int directory,const char *name,NSData *data,uint64_t deadline) {
    if (!Before(deadline)) return NO;
    int fd=openat(directory,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) return NO;
    size_t offset=0;BOOL valid=YES,complete=NO;
    @try {
    while (offset<data.length) {
        if (!Before(deadline)) { valid=NO;break; }
        size_t chunk=data.length-offset;if (chunk>65536) chunk=65536;
        ssize_t n=write(fd,(const unsigned char *)data.bytes+offset,chunk);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { valid=NO;break; }offset+=(size_t)n;
    }
    complete=valid && Before(deadline);
    } @finally {
        if (close(fd)) complete=NO;
        if (!complete) unlinkat(directory,name,0);
    }
    return complete;
}
static void PersistentPublish(int parent,NSData *data,NSDictionary *proof,uint64_t deadline) {
    NSString *hash=proof[@"hash"],*leaf=proof[@"leaf"],*input=nil,*output=nil;
    NSData *metadata=proof[@"metadata"],*name=proof[@"name"];
    uint64_t size,type;
    if (!Before(deadline) || !HashString(hash) || !PersistentPaths(leaf,&input,&output) ||
        !Integer(proof[@"inputBytes"],&size) || !Integer(proof[@"type"],&type) || type>3 ||
        ![name isKindOfClass:[NSData class]] || !name.length || name.length>512 ||
        ![metadata isKindOfClass:[NSData class]] || metadata.length>persistentMetadataLimit) return;
    Function function={name.bytes,name.length,(uint8_t)type};
    if (!ResultValidForInput(data,metadata,function,hash,(size_t)size,input,output) || !Before(deadline)) return;
    NSString *outputHash=Hash(data.bytes,data.length);if (!Before(deadline)) return;
    NSString *metadataHash=Hash(metadata.bytes,metadata.length);if (!Before(deadline)) return;
    NSDictionary *record=@{@"formatVersion":@1,@"receiptSHA256":PersistentReceiptHash(),@"inputSHA256":hash,@"inputBytes":@(size),
        @"inputFunctionType":@(type),@"inputFunctionBytesHex":FunctionHex(function),@"outputSHA256":outputHash,
        @"outputBytes":@(data.length),@"metadataSHA256":metadataHash,@"originalJobLeaf":leaf};
    NSData *recordBytes=[NSJSONSerialization dataWithJSONObject:record options:0 error:NULL];
    if (!recordBytes || recordBytes.length>persistentRecordLimit || !Before(deadline) ||
        !PersistentCapacity(parent,data.length+metadata.length+recordBytes.length,deadline)) return;
    NSString *temporary=[@".pending-" stringByAppendingString:NSUUID.UUID.UUIDString];
    if (mkdirat(parent,temporary.UTF8String,0700)) return;
    int child=openat(parent,temporary.UTF8String,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    BOOL committed=NO;
    @try {
        if (child<0 || !PersistentWrite(child,"output.metallib",data,deadline) ||
            !PersistentWrite(child,"helper.json",metadata,deadline) || !PersistentWrite(child,"record.json",recordBytes,deadline)) return;
        // child is the entry FD, not a profile parent: verify the same bytes directly.
        NSData *stored=ReadLimited(child,"output.metallib",persistentLibraryLimit,deadline);
        NSData *storedMetadata=Before(deadline)?ReadLimited(child,"helper.json",persistentMetadataLimit,deadline):nil;
        NSData *storedRecord=Before(deadline)?ReadLimited(child,"record.json",persistentRecordLimit,deadline):nil;
        if (!stored || !storedMetadata || !storedRecord || ![stored isEqual:data] ||
            ![storedMetadata isEqual:metadata] || ![storedRecord isEqual:recordBytes] ||
            !Before(deadline) || ![Hash(stored.bytes,stored.length) isEqual:record[@"outputSHA256"]] || !Before(deadline) ||
            !ResultValidForInput(stored,storedMetadata,function,hash,(size_t)size,input,output) || !Before(deadline)) return;
        // Never replace a prior nonempty accepted entry; global writer lock prevents a competing publisher.
        struct stat prior;
        if (!fstatat(parent,hash.UTF8String,&prior,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT) return;
        committed=renameat(parent,temporary.UTF8String,parent,hash.UTF8String)==0;
        if (committed) {
            // RAM hits share this NSData. Once it has been published, their
            // native library loads need no repeated receipt checks or writes.
            if (objc_getAssociatedObject(data,&persistentProducerKey)==proof)
                objc_setAssociatedObject(data,&persistentProducerKey,nil,OBJC_ASSOCIATION_RETAIN);
            static unsigned published;
            if (__sync_fetch_and_add(&published,1)<16)
                fprintf(stderr,"[ModernShaderPersistent] input=%s output=%s outcome=published bytes=%zu receipt=%s\n",
                    hash.UTF8String,outputHash.UTF8String,data.length,PersistentReceiptHash().UTF8String);
        }
    } @finally {
        if (child>=0) {
            if (!committed) { unlinkat(child,"output.metallib",0);unlinkat(child,"helper.json",0);unlinkat(child,"record.json",0); }
            close(child);
        }
        if (!committed) unlinkat(parent,temporary.UTF8String,AT_REMOVEDIR);
    }
}
void VZModernRecordSuccessfulNativeLibrary(NSData *data,NSString *hash) {
    // Only the caller after a genuinely nonnil/error-free native library load invokes this.
    // All publication IO is outside worker.lock and native pipeline completion.
    int parent=-1,lock=-1;BOOL retained=NO,queued=NO;NSDictionary *proof=nil;
    @try {
        proof=objc_getAssociatedObject(data,&persistentProducerKey);
        if (!proof || ![proof[@"hash"] isEqual:hash] || data.length>persistentLibraryLimit) return;
        uint64_t begin=Now();if (!begin || begin>UINT64_MAX-50000000ULL) return;
        parent=PersistentOpen(begin+50000000ULL);if (parent<0) return;
        lock=openat(parent,"publisher.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
        struct stat value;
        if (lock<0 || fstat(lock,&value) || !S_ISREG(value.st_mode) || value.st_uid!=501 || value.st_nlink!=1 ||
            (value.st_mode&07777)!=0600 || flock(lock,LOCK_EX|LOCK_NB)) return;
        struct stat existing;
        if (!fstatat(parent,hash.UTF8String,&existing,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT) return;
        // The sole global lock also bounds retained producer data across every Task to one2MiB item.
        [data retain];[proof retain];retained=YES;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
            @autoreleasepool {
                @try { uint64_t now=Now();if (now && now<=UINT64_MAX-persistentPublishBudget) PersistentPublish(parent,data,proof,now+persistentPublishBudget); }
                @catch (NSException *exception) { (void)exception; }
                @finally { close(lock);close(parent);[proof release];[data release]; }
            }
        });
        queued=YES;
    } @catch (NSException *exception) { (void)exception; }
    @finally {
        if (!queued) { if (lock>=0) close(lock);if (parent>=0) close(parent);if (retained) { [proof release];[data release]; } }
    }
}
