// Optional app-owned positive converter cache. Included after the unchanged
// original ResultValidForInput. No Metal device/object/work or fake completion.
#include <objc/runtime.h>

static const uint64_t persistentReserve = 2500ULL * 1000 * 1000;
static const size_t persistentLibraryLimit = 2 * 1024 * 1024;
static const size_t persistentRecordLimit = 16 * 1024;
static const size_t persistentMetadataLimit = 16 * 1024;
static const uint64_t persistentDiskLimit = 1024ULL * 1024 * 1024;
static const unsigned persistentEntryLimit = 8192;
// Publication runs on a utility queue outside worker.lock. A full scan of the
// profile, linear in the entry count, happens only when the usage tally is
// missing or the cache is full, so its budget still grows with the limit.
static const uint64_t persistentPublishBudget = 1000000000ULL;
// A hit refreshes its entry's time at most this often; eviction removes the
// entries unused for longest until the cache is back to 90% of either limit.
static const time_t persistentTouchInterval = 3600;
static const char persistentUsageName[] = "usage";
static char persistentProducerKey;

// Fixture overrides exist only in the independent local CPU harness. They are
// never used by the production build and do not attest device/root identity.
#ifdef VZ_PVG_PERSISTENT_TESTING
static NSString *persistentFixtureCompiler;
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
static NSString *PersistentIdentityHash(void);
static NSString *PersistentCompilerDirectory(void) {
#ifdef VZ_PVG_PERSISTENT_TESTING
    return persistentFixtureCompiler;
#else
    return [NSString stringWithUTF8String:codeDirectory];
#endif
}
static NSString *PersistentParent(void) {
#ifdef VZ_PVG_PERSISTENT_TESTING
    return persistentFixtureParent;
#else
    return @"/var/root/VirtualMac2/shader-cache/converter-v2";
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
    return getuid() == 501 && geteuid() == 501 && role && !strcmp(role,"server") &&
        backend && !strcmp(backend,"27");
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
static const struct { const char *name,*sha,*cd; uint64_t size; } persistentFiles[] = {
    {"VM27CompilerCompat.dylib","7e60029cb6a27f99dde2ba444403359618de9e7cc31276b72bb8fff4af5e2cb9","26b3426f4ae76bb4900dbe2d2724a4c2dd1123a6",51312},
    {"VM27GPUCompiler.dylib","5bf8d5ad9cc1f478e3557fb6c663a5735e183e28b097de8b6c56ff4aae511bfa","4ee95465f485ae6a5ed4fe7a9c08f49ba42e3cb7",259856},
    {"VM27GPUCompilerImpl.dylib","cb1d702bb4a3c094b8237a020d6b437a092f9272cb325166e8dcce02a4e3ec85","5755dff78889d2b199113b3fc9d242d12b185d2a",16949248},
    {"VM27LLVM.dylib","2140410973ad9d048625f739626398b5972e16a033ce051d016bd2e4cfbf27bb","f9eade69cce3728d83c63f4e668651e185f136fd",106874736},
    {"VM27llvm-flatbuffers.dylib","4589ef19a9037f519580c9fb61b9d51c22a23b2b05b9305428e25e5528d640dd","8a3977a8530372c340c812ad192d39f62d0a7846",415088},
    {"VM27llvm-lmdb.dylib","2e63f8f5506fc76c8091e2b2af2242492dd041221d095770a0603708aba647ae","a4a30fe433a4b6c34d480cd86c1186d8b6875d70",116288},
    {"metal-library-convert","3493a884fe631b3b7fbf990699acd026b92d323405f23e39ba0529d494060708","cd7b9238679d0435293166fefd93774529a4e370",70432}
};
static const char *const persistentProfileSHA = "6ec7684553365a759feb68f2820dd5fcc333d62ca2ccdae9daa7863d48672e59";
// The profile directory is named after the compiled compiler set, so entries
// produced by a different compiler release are never looked up. Nothing here
// is captured per device: inode, timestamps and st_dev change across installs
// and reboots, which silently disabled the cache everywhere but one iPad.
static NSString *PersistentIdentityHash(void) {
#ifdef VZ_PVG_PERSISTENT_TESTING
    return persistentFixtureHash;
#else
    static NSString *identity;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSMutableString *text=[NSMutableString stringWithFormat:@"converter-v2\n%s\n",persistentProfileSHA];
        for (unsigned i=0;i<7;++i)
            [text appendFormat:@"%s %s %s %llu\n",persistentFiles[i].name,persistentFiles[i].sha,persistentFiles[i].cd,
                (unsigned long long)persistentFiles[i].size];
        NSData *bytes=[text dataUsingEncoding:NSUTF8StringEncoding];
        identity=[Hash(bytes.bytes,bytes.length) retain];
    });
    return identity;
#endif
}
static BOOL PersistentVerifyIdentity(uint64_t deadline) {
    if (!PersistentEnabled() || !HashString(PersistentIdentityHash()) || !Before(deadline)) return NO;
    int compilerDir=PersistentDirectory(PersistentCompilerDirectory(),PersistentRootUID(),0755);
    if (compilerDir<0) return NO;
    BOOL valid=YES;
    for (unsigned i=0;i<7 && valid;++i) {
        int fd=openat(compilerDir,persistentFiles[i].name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        struct stat value;
        valid=fd>=0 && !fstat(fd,&value) && S_ISREG(value.st_mode) &&
            value.st_uid==PersistentRootUID() && value.st_gid==PersistentRootGID() &&
            (value.st_mode&07777)==0755 && value.st_nlink==1 &&
            (uint64_t)value.st_size==persistentFiles[i].size && Before(deadline);
        if (fd>=0) close(fd);
    }
    close(compilerDir);
    return valid && Before(deadline);
}
static int PersistentOpen(uint64_t deadline) {
    if (!PersistentVerifyIdentity(deadline)) return -1;
    // shader-cache is the package's mobile-owned data directory; create the
    // converter parent and this compiler set's profile there on first use.
    if (mkdir(PersistentParent().fileSystemRepresentation,0700) && errno!=EEXIST) return -1;
    int parent=PersistentDirectory(PersistentParent(),501,0700);
    if (parent<0) return -1;
    int fd=-1;
    @try {
        const char *leaf=PersistentIdentityHash().UTF8String;
        if (!mkdirat(parent,leaf,0700) || errno==EEXIST)
            fd=openat(parent,leaf,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    } @finally { close(parent); }
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
        ![record[@"compilerSetSHA256"] isEqual:PersistentIdentityHash()] || ![record[@"inputSHA256"] isEqual:hash] ||
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
    // The entry directory's modification time is its last use.
    struct timespec now;
    if (!clock_gettime(CLOCK_REALTIME,&now) && now.tv_sec-directory.st_mtimespec.tv_sec>persistentTouchInterval)
        utimensat(parent,hash.UTF8String,NULL,AT_SYMLINK_NOFOLLOW);
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
                fprintf(stderr,"[ModernShaderPersistent] input=%s outcome=owned-data-hit stage=%s bytes=%zu elapsedNanoseconds=%llu compilerSet=%s\n",
                    hash.UTF8String,cap==50000000ULL?"pre-lock":"post-lock",result.length,
                    (unsigned long long)(end-begin),PersistentIdentityHash().UTF8String);
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
static unsigned PersistentEntryLimit(void) {
    static unsigned limit;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        limit=persistentEntryLimit;
        // Diagnostics only: a smaller limit, to exercise eviction.
        FILE *file=fopen("/var/root/VirtualMac2/diagnostics/shader-cache-entry-limit","r");
        unsigned value=0;
        if (file) { if (fscanf(file,"%u",&value)==1 && value>=4 && value<limit) limit=value; fclose(file); }
    });
    return limit;
}
typedef struct { char name[65]; uint64_t bytes; struct timespec used; } PersistentEntry;
static int PersistentEntryOlder(const void *a,const void *b) {
    const struct timespec *x=&((const PersistentEntry *)a)->used,*y=&((const PersistentEntry *)b)->used;
    return x->tv_sec!=y->tv_sec ? (x->tv_sec<y->tv_sec?-1:1) : (x->tv_nsec<y->tv_nsec?-1:x->tv_nsec>y->tv_nsec);
}
// Every entry of the profile with its size and last use. Anything that is not
// a well-formed entry, other than the lock, the tally and unfinished
// publications, fails the scan, which leaves the cache untouched.
static BOOL PersistentScan(int directory,uint64_t deadline,PersistentEntry **entriesOut,unsigned *countOut,uint64_t *totalOut) {
    // A dup shares its readdir offset. Open "." so every scan starts at the
    // beginning even when this FD has already been inspected.
    int dupfd=openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if (dupfd<0) return NO;
    DIR *stream=fdopendir(dupfd);if (!stream) { close(dupfd);return NO; }
    unsigned count=0,capacity=0;uint64_t total=0;BOOL valid=YES;struct dirent *entry;
    PersistentEntry *entries=NULL;
    @try {
    while (Before(deadline)) {
        errno=0;entry=readdir(stream);if (!entry) { if (errno) valid=NO;break; }
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..") || !strcmp(entry->d_name,"publisher.lock") ||
            !strcmp(entry->d_name,persistentUsageName) || !strncmp(entry->d_name,".pending-",9) ||
            !strncmp(entry->d_name,".usage-",7)) continue;
        NSString *name=[NSString stringWithUTF8String:entry->d_name];
        if (!HashString(name) || count>=2*persistentEntryLimit) { valid=NO;break; }
        int child=openat(directory,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        struct stat value,folder;
        if (child<0 || fstat(child,&folder) || !S_ISDIR(folder.st_mode) || folder.st_uid!=501 || (folder.st_mode&07777)!=0700) valid=NO;
        uint64_t bytes=0;
        static const char *const files[]={"output.metallib","helper.json","record.json"};
        for (unsigned i=0;child>=0 && valid && i<3;++i) {
            if (fstatat(child,files[i],&value,AT_SYMLINK_NOFOLLOW) || !S_ISREG(value.st_mode) || value.st_uid!=501 || value.st_nlink!=1 || (value.st_mode&07777)!=0600 || value.st_size<=0 || (uint64_t)value.st_size>persistentLibraryLimit) valid=NO;
            else bytes+=(uint64_t)value.st_size;
        }
        if (child>=0) close(child);if (!valid) break;
        if (count==capacity) {
            capacity=capacity?capacity*2:1024;
            PersistentEntry *grown=realloc(entries,capacity*sizeof(*entries));
            if (!grown) { valid=NO;break; }
            entries=grown;
        }
        strlcpy(entries[count].name,entry->d_name,sizeof(entries[count].name));
        entries[count].bytes=bytes;
        entries[count].used=folder.st_mtimespec;
        ++count;total+=bytes;
    }
    valid=valid && Before(deadline);
    } @finally { closedir(stream); }
    if (!valid) { free(entries);return NO; }
    *entriesOut=entries;*countOut=count;*totalOut=total;
    return YES;
}
// The tally of entries and bytes, kept beside the entries. Only a publisher
// holding publisher.lock reads or replaces it.
static BOOL PersistentReadUsage(int directory,unsigned *count,uint64_t *total) {
    int fd=openat(directory,persistentUsageName,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if (fd<0) return NO;
    char text[64]={0};
    struct stat value;
    ssize_t n=(!fstat(fd,&value) && S_ISREG(value.st_mode) && value.st_uid==501) ? read(fd,text,sizeof(text)-1) : -1;
    close(fd);
    unsigned long long entries=0,bytes=0;
    if (n<=0 || sscanf(text,"%llu %llu",&entries,&bytes)!=2 || entries>2*persistentEntryLimit) return NO;
    *count=(unsigned)entries;*total=bytes;
    return YES;
}
static void PersistentWriteUsage(int directory,unsigned count,uint64_t total) {
    char text[64];
    int length=snprintf(text,sizeof(text),"%u %llu\n",count,(unsigned long long)total);
    char temporary[32];
    snprintf(temporary,sizeof(temporary),".usage-%d",getpid());
    int fd=openat(directory,temporary,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) return;
    BOOL written=write(fd,text,(size_t)length)==length;
    if (close(fd) || !written || renameat(directory,temporary,directory,persistentUsageName)) {
        unlinkat(directory,temporary,0);
        unlinkat(directory,persistentUsageName,0);
    }
}
static void PersistentRemoveEntry(int directory,const char *name) {
    int child=openat(directory,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (child>=0) {
        unlinkat(child,"output.metallib",0);unlinkat(child,"helper.json",0);unlinkat(child,"record.json",0);
        close(child);
    }
    unlinkat(directory,name,AT_REMOVEDIR);
}
// Make room for one more entry of the given size. The tally answers in the
// common case; a full scan rebuilds it when it is missing and, when the cache
// is full, removes the entries unused for longest first.
static BOOL PersistentCapacity(int directory,size_t additional,uint64_t deadline,unsigned *countOut,uint64_t *totalOut) {
    unsigned limit=PersistentEntryLimit(),count=0;uint64_t total=0;
    if (additional>persistentDiskLimit) return NO;
    if (PersistentReadUsage(directory,&count,&total) && count<limit-1 && additional<=persistentDiskLimit-total) {
        *countOut=count;*totalOut=total;
        return YES;
    }
    PersistentEntry *entries=NULL;
    if (!PersistentScan(directory,deadline,&entries,&count,&total)) return NO;
    unsigned removed=0;uint64_t freed=0;
    if (count>=limit-1 || additional>persistentDiskLimit-total) {
        uint64_t byteTarget=persistentDiskLimit/10*9;
        unsigned countTarget=limit/10*9;
        qsort(entries,count,sizeof(*entries),PersistentEntryOlder);
        for (unsigned i=0;i<count && Before(deadline) &&
             (count-removed>=countTarget || total-freed+additional>byteTarget);++i) {
            PersistentRemoveEntry(directory,entries[i].name);
            ++removed;freed+=entries[i].bytes;
        }
        count-=removed;total-=freed;
    }
    free(entries);
    PersistentWriteUsage(directory,count,total);
    static unsigned logged;
    if (__sync_fetch_and_add(&logged,1)<16)
        fprintf(stderr,"[ModernShaderPersistent] usage rebuilt entries=%u bytes=%llu evicted=%u freed=%llu limit=%u\n",
            count,(unsigned long long)total,removed,(unsigned long long)freed,limit);
    if (count>=limit-1 || additional>persistentDiskLimit-total || !Before(deadline)) return NO;
    *countOut=count;*totalOut=total;
    return YES;
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
    NSDictionary *record=@{@"formatVersion":@1,@"compilerSetSHA256":PersistentIdentityHash(),@"inputSHA256":hash,@"inputBytes":@(size),
        @"inputFunctionType":@(type),@"inputFunctionBytesHex":FunctionHex(function),@"outputSHA256":outputHash,
        @"outputBytes":@(data.length),@"metadataSHA256":metadataHash,@"originalJobLeaf":leaf};
    NSData *recordBytes=[NSJSONSerialization dataWithJSONObject:record options:0 error:NULL];
    unsigned usageCount=0;uint64_t usageTotal=0;
    size_t entryBytes=recordBytes ? data.length+metadata.length+recordBytes.length : 0;
    if (!recordBytes || recordBytes.length>persistentRecordLimit || !Before(deadline) ||
        !PersistentCapacity(parent,entryBytes,deadline,&usageCount,&usageTotal)) return;
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
            PersistentWriteUsage(parent,usageCount+1,usageTotal+entryBytes);
            // RAM hits share this NSData. Once it has been published, their
            // native library loads need no repeated identity checks or writes.
            if (objc_getAssociatedObject(data,&persistentProducerKey)==proof)
                objc_setAssociatedObject(data,&persistentProducerKey,nil,OBJC_ASSOCIATION_RETAIN);
            static unsigned published;
            if (__sync_fetch_and_add(&published,1)<16)
                fprintf(stderr,"[ModernShaderPersistent] input=%s output=%s outcome=published bytes=%zu compilerSet=%s\n",
                    hash.UTF8String,outputHash.UTF8String,data.length,PersistentIdentityHash().UTF8String);
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
