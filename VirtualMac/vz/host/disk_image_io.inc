// Disk.img write-back.
//
// With a Cached disk attachment every guest write lands in the iPad page
// cache, and the next guest barrier makes the VMM's F_BARRIERFSYNC write the
// whole backlog synchronously while the single block queue waits: about
// 64 ms + 0.5 ms per MiB, so a few hundred MiB stall the guest for 0.3 s.
// Write-back caps that backlog by starting a background fsync whenever
// DISK_WRITEBACK_THRESHOLD of writes have accumulated, so a guest barrier
// only has to wait for the remainder.
//
// The client opens Disk.img and hands the VMM a descriptor, so the descriptor
// is recognised by F_GETPATH the first time a write reaches it.

#define DISK_WRITEBACK_THRESHOLD (20ULL << 20)
#define DISK_IMAGE_MAX_FD 4096

static int gDiskImageFD = -1;
// Per descriptor: 0 = unchecked, 1 = not Disk.img.
static uint8_t gDiskImageCheckedFDs[DISK_IMAGE_MAX_FD];
static uint64_t gDiskWritebackPending;
static int gDiskWritebackInFlight;
static dispatch_queue_t gDiskWritebackQueue;

static int disk_image_fd(int fd) {
    int known = __atomic_load_n(&gDiskImageFD, __ATOMIC_ACQUIRE);
    if (known >= 0)
        return fd == known;
    if (fd < 0 || fd >= DISK_IMAGE_MAX_FD || gDiskImageCheckedFDs[fd])
        return 0;
    char path[MAXPATHLEN];
    if (fcntl(fd, F_GETPATH, path) == 0) {
        const char *slash = strrchr(path, '/');
        if (strcmp(slash ? slash + 1 : path, "Disk.img") == 0) {
            static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
            pthread_mutex_lock(&lock);
            if (gDiskImageFD < 0) {
                gDiskWritebackQueue = dispatch_queue_create(
                    "virtualmac.disk-writeback",
                    dispatch_queue_attr_make_with_qos_class(
                        DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
                logf_("[vmmhook] Disk.img fd=%d writeback=%llu MiB", fd,
                      DISK_WRITEBACK_THRESHOLD >> 20);
                __atomic_store_n(&gDiskImageFD, fd, __ATOMIC_RELEASE);
            }
            pthread_mutex_unlock(&lock);
            return fd == gDiskImageFD;
        }
    }
    gDiskImageCheckedFDs[fd] = 1;
    return 0;
}

// Runs on the serial write-back queue, so at most one is in flight.
static void disk_writeback_run(void *context) {
    // Called from this image, so it reaches the real fsync rather than any
    // interposed one.
    fsync((int)(intptr_t)context);
    __atomic_store_n(&gDiskWritebackInFlight, 0, __ATOMIC_RELEASE);
}

static void disk_image_record_write(int fd, ssize_t result) {
    if (result <= 0)
        return;
    uint64_t pending = __atomic_add_fetch(&gDiskWritebackPending, result,
                                          __ATOMIC_RELAXED);
    int idle = 0;
    if (pending >= DISK_WRITEBACK_THRESHOLD &&
        __atomic_compare_exchange_n(&gDiskWritebackInFlight, &idle, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        __atomic_store_n(&gDiskWritebackPending, 0, __ATOMIC_RELAXED);
        dispatch_async_f(gDiskWritebackQueue, (void *)(intptr_t)fd,
                         disk_writeback_run);
    }
}

static ssize_t disk_image_pwrite(int fd, const void *buffer, size_t length,
                                 off_t offset) {
    ssize_t result = pwrite(fd, buffer, length, offset);
    if (disk_image_fd(fd))
        disk_image_record_write(fd, result);
    return result;
}

static ssize_t disk_image_pwritev(int fd, const struct iovec *vectors,
                                  int count, off_t offset) {
    ssize_t result = pwritev(fd, vectors, count, offset);
    if (disk_image_fd(fd))
        disk_image_record_write(fd, result);
    return result;
}

__attribute__((used)) static struct {
    const void *replacement;
    const void *replacee;
} _ip_disk_image_io[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void *)&disk_image_pwrite, (const void *)&pwrite},
    {(const void *)&disk_image_pwritev, (const void *)&pwritev},
};
