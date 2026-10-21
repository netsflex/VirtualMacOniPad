// Disk.img write-back and developer synchronization trace.
//
// With a Cached disk attachment every guest write lands in the iPad page
// cache, and the next guest barrier makes the VMM's F_BARRIERFSYNC write the
// whole backlog synchronously while the single block queue waits: about
// 64 ms + 0.5 ms per MiB, so a few hundred MiB stall the guest for 0.3 s.
// Write-back caps that backlog by starting a background fsync whenever
// DISK_WRITEBACK_DEFAULT_MB of writes have accumulated, so a guest barrier
// only has to wait for the remainder. /tmp/vz-disk-writeback-mb, read once as
// the VMM first writes Disk.img, overrides the threshold in MiB; 0 disables it.
//
// When /tmp/vz-trace-disk-sync exists, every write, fsync, F_FULLFSYNC and
// F_BARRIERFSYNC on Disk.img is also timed. Syncs slower than 20 ms are
// logged with the bytes written since the previous sync, and a one-second
// summary records counts and time spent, so guest-side latency spikes can be
// lined up against host writeback. Output: /tmp/vz-disk-sync.log.
//
// The client opens Disk.img and hands the VMM a descriptor, so the descriptor
// is recognised by F_GETPATH the first time a write or sync reaches it.

#define DISK_WRITEBACK_CONFIG "/tmp/vz-disk-writeback-mb"
#define DISK_WRITEBACK_DEFAULT_MB 32
#define DISK_SYNC_TRACE_MARKER "/tmp/vz-trace-disk-sync"
#define DISK_SYNC_TRACE_LOG "/tmp/vz-disk-sync.log"
#define DISK_SYNC_SLOW_NS 20000000ULL
#define DISK_IMAGE_MAX_FD 4096

static int gDiskImageFD = -1;
// Per descriptor: 0 = unchecked, 1 = not Disk.img.
static uint8_t gDiskImageCheckedFDs[DISK_IMAGE_MAX_FD];
static int gDiskSyncTraceEnabled;
static uint64_t gDiskWritebackThreshold;
static uint64_t gDiskWritebackPending;
static int gDiskWritebackInFlight;
static dispatch_queue_t gDiskWritebackQueue;

static uint64_t gDiskSyncWrittenSinceSync;
static uint64_t gDiskSyncWrites, gDiskSyncWriteBytes, gDiskSyncWriteNs;
static uint64_t gDiskSyncBarriers, gDiskSyncBarrierNs;
static uint64_t gDiskSyncFlushes, gDiskSyncFlushNs;
static uint64_t gDiskSyncWritebacks, gDiskSyncWritebackNs;
static uint64_t gDiskSyncWorstNs;
static mach_timebase_info_data_t gDiskSyncTimebase;

static uint64_t disk_sync_now_ns(void) {
    return mach_absolute_time() * gDiskSyncTimebase.numer /
        gDiskSyncTimebase.denom;
}

static void disk_sync_log(const char *fmt, ...) {
    FILE *f = fopen(DISK_SYNC_TRACE_LOG, "a");
    if (!f)
        return;
    struct timeval now;
    gettimeofday(&now, NULL);
    struct tm local;
    localtime_r(&now.tv_sec, &local);
    fprintf(f, "%02d:%02d:%02d.%03d ", local.tm_hour, local.tm_min,
            local.tm_sec, (int)(now.tv_usec / 1000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static uint64_t disk_sync_take(uint64_t *counter) {
    return __atomic_exchange_n(counter, 0, __ATOMIC_RELAXED);
}

static void disk_sync_summary(void *unused) {
    (void)unused;
    uint64_t writes = disk_sync_take(&gDiskSyncWrites);
    uint64_t bytes = disk_sync_take(&gDiskSyncWriteBytes);
    uint64_t writeNs = disk_sync_take(&gDiskSyncWriteNs);
    uint64_t barriers = disk_sync_take(&gDiskSyncBarriers);
    uint64_t barrierNs = disk_sync_take(&gDiskSyncBarrierNs);
    uint64_t flushes = disk_sync_take(&gDiskSyncFlushes);
    uint64_t flushNs = disk_sync_take(&gDiskSyncFlushNs);
    uint64_t writebacks = disk_sync_take(&gDiskSyncWritebacks);
    uint64_t writebackNs = disk_sync_take(&gDiskSyncWritebackNs);
    uint64_t worst = disk_sync_take(&gDiskSyncWorstNs);
    if (writes || barriers || flushes || writebacks)
        disk_sync_log("SUMMARY writes=%llu MiB=%.1f write_ms=%.1f "
                      "barriers=%llu barrier_ms=%.1f flushes=%llu "
                      "flush_ms=%.1f writebacks=%llu writeback_ms=%.1f "
                      "worst_sync_ms=%.1f",
                      writes, bytes / 1048576.0, writeNs / 1e6, barriers,
                      barrierNs / 1e6, flushes, flushNs / 1e6, writebacks,
                      writebackNs / 1e6, worst / 1e6);
}

static void disk_image_start(int fd, const char *path) {
    mach_timebase_info(&gDiskSyncTimebase);
    uint64_t megabytes = DISK_WRITEBACK_DEFAULT_MB;
    FILE *config = fopen(DISK_WRITEBACK_CONFIG, "r");
    if (config) {
        unsigned long long value;
        if (fscanf(config, "%llu", &value) == 1)
            megabytes = value;
        fclose(config);
    }
    gDiskWritebackThreshold = megabytes << 20;
    if (gDiskWritebackThreshold)
        gDiskWritebackQueue = dispatch_queue_create(
            "virtualmac.disk-writeback",
            dispatch_queue_attr_make_with_qos_class(
                DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    gDiskSyncTraceEnabled = access(DISK_SYNC_TRACE_MARKER, F_OK) == 0;
    logf_("[vmmhook] Disk.img fd=%d writeback=%llu MiB trace=%d", fd,
          megabytes, gDiskSyncTraceEnabled);
    if (!gDiskSyncTraceEnabled)
        return;
    disk_sync_log("TRACE fd=%d writeback_MiB=%llu path=%s", fd, megabytes,
                  path);
    dispatch_source_t timer = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(timer,
                              dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
                              NSEC_PER_SEC, NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler_f(timer, disk_sync_summary);
    dispatch_resume(timer);
}

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
                disk_image_start(fd, path);
                __atomic_store_n(&gDiskImageFD, fd, __ATOMIC_RELEASE);
            }
            pthread_mutex_unlock(&lock);
            return fd == gDiskImageFD;
        }
    }
    gDiskImageCheckedFDs[fd] = 1;
    return 0;
}

static void disk_sync_note_worst(uint64_t elapsed) {
    uint64_t worst = __atomic_load_n(&gDiskSyncWorstNs, __ATOMIC_RELAXED);
    while (elapsed > worst &&
           !__atomic_compare_exchange_n(&gDiskSyncWorstNs, &worst, elapsed,
                                        1, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
}

static void disk_sync_record_sync(const char *kind, uint64_t *count,
                                  uint64_t *total, uint64_t start,
                                  int result) {
    uint64_t elapsed = disk_sync_now_ns() - start;
    uint64_t backlog = disk_sync_take(&gDiskSyncWrittenSinceSync);
    __atomic_add_fetch(count, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(total, elapsed, __ATOMIC_RELAXED);
    disk_sync_note_worst(elapsed);
    if (elapsed >= DISK_SYNC_SLOW_NS)
        disk_sync_log("SLOW %s ms=%.1f written_since_last_sync_MiB=%.1f "
                      "result=%d", kind, elapsed / 1e6, backlog / 1048576.0,
                      result);
}

// Runs on the serial write-back queue, so at most one is in flight.
static void disk_writeback_run(void *context) {
    int fd = (int)(intptr_t)context;
    uint64_t start = gDiskSyncTraceEnabled ? disk_sync_now_ns() : 0;
    // Called from this image, so it reaches the real fsync, not the
    // interposed one, and is not counted as a guest flush.
    int result = fsync(fd);
    if (gDiskSyncTraceEnabled) {
        uint64_t elapsed = disk_sync_now_ns() - start;
        uint64_t backlog = disk_sync_take(&gDiskSyncWrittenSinceSync);
        __atomic_add_fetch(&gDiskSyncWritebacks, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&gDiskSyncWritebackNs, elapsed, __ATOMIC_RELAXED);
        if (elapsed >= DISK_SYNC_SLOW_NS)
            disk_sync_log("SLOW writeback ms=%.1f "
                          "written_since_last_sync_MiB=%.1f result=%d",
                          elapsed / 1e6, backlog / 1048576.0, result);
    }
    __atomic_store_n(&gDiskWritebackInFlight, 0, __ATOMIC_RELEASE);
}

static void disk_image_record_write(int fd, ssize_t result, uint64_t start) {
    if (result > 0 && gDiskWritebackThreshold) {
        uint64_t pending = __atomic_add_fetch(&gDiskWritebackPending, result,
                                              __ATOMIC_RELAXED);
        int idle = 0;
        if (pending >= gDiskWritebackThreshold &&
            __atomic_compare_exchange_n(&gDiskWritebackInFlight, &idle, 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            __atomic_store_n(&gDiskWritebackPending, 0, __ATOMIC_RELAXED);
            dispatch_async_f(gDiskWritebackQueue, (void *)(intptr_t)fd,
                             disk_writeback_run);
        }
    }
    if (!gDiskSyncTraceEnabled)
        return;
    __atomic_add_fetch(&gDiskSyncWrites, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&gDiskSyncWriteNs, disk_sync_now_ns() - start,
                       __ATOMIC_RELAXED);
    if (result > 0) {
        __atomic_add_fetch(&gDiskSyncWriteBytes, result, __ATOMIC_RELAXED);
        __atomic_add_fetch(&gDiskSyncWrittenSinceSync, result,
                           __ATOMIC_RELAXED);
    }
}

static ssize_t disk_image_pwrite(int fd, const void *buffer, size_t length,
                                 off_t offset) {
    if (!disk_image_fd(fd))
        return pwrite(fd, buffer, length, offset);
    uint64_t start = gDiskSyncTraceEnabled ? disk_sync_now_ns() : 0;
    ssize_t result = pwrite(fd, buffer, length, offset);
    disk_image_record_write(fd, result, start);
    return result;
}

static ssize_t disk_image_pwritev(int fd, const struct iovec *vectors,
                                  int count, off_t offset) {
    if (!disk_image_fd(fd))
        return pwritev(fd, vectors, count, offset);
    uint64_t start = gDiskSyncTraceEnabled ? disk_sync_now_ns() : 0;
    ssize_t result = pwritev(fd, vectors, count, offset);
    disk_image_record_write(fd, result, start);
    return result;
}

static int disk_image_fsync(int fd) {
    if (!disk_image_fd(fd) || !gDiskSyncTraceEnabled)
        return fsync(fd);
    uint64_t start = disk_sync_now_ns();
    int result = fsync(fd);
    disk_sync_record_sync("fsync", &gDiskSyncFlushes, &gDiskSyncFlushNs,
                          start, result);
    return result;
}

static int disk_image_fcntl(int fd, int command, ...) {
    va_list ap;
    va_start(ap, command);
    void *argument = va_arg(ap, void *);
    va_end(ap);
    if ((command != F_FULLFSYNC && command != F_BARRIERFSYNC) ||
        !disk_image_fd(fd) || !gDiskSyncTraceEnabled)
        return fcntl(fd, command, argument);
    uint64_t start = disk_sync_now_ns();
    int result = fcntl(fd, command, argument);
    if (command == F_FULLFSYNC)
        disk_sync_record_sync("F_FULLFSYNC", &gDiskSyncFlushes,
                              &gDiskSyncFlushNs, start, result);
    else
        disk_sync_record_sync("F_BARRIERFSYNC", &gDiskSyncBarriers,
                              &gDiskSyncBarrierNs, start, result);
    return result;
}

__attribute__((used)) static struct {
    const void *replacement;
    const void *replacee;
} _ip_disk_image_io[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void *)&disk_image_pwrite, (const void *)&pwrite},
    {(const void *)&disk_image_pwritev, (const void *)&pwritev},
    {(const void *)&disk_image_fsync, (const void *)&fsync},
    {(const void *)&disk_image_fcntl, (const void *)&fcntl},
};
