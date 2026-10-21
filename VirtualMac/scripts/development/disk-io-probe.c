// Guest-side Disk.img I/O probe for comparing host disk attachment policies.
//
// The host can vary two things independently (DebugDiskCachingMode and
// DebugDiskSynchronizationMode in Settings.plist), so every test isolates
// one of them:
//
//   flush-*      4 KiB random overwrites, each followed by a guest flush.
//                Measures the host synchronization mode: a guest F_FULLFSYNC
//                becomes a virtio flush, which the host turns into
//                F_FULLFSYNC (full) or F_BARRIERFSYNC (fsync) on Disk.img.
//   smallfiles   create, write, F_FULLFSYNC and close many small files, the
//                shape of installers, git and SQLite-heavy applications.
//   read-*       reads with guest F_NOCACHE, so the guest's own buffer cache
//                never answers and every byte crosses virtio. Pass 2 re-reads
//                the same range: only a Cached host attachment can serve it
//                from iPad memory, so pass 2 versus pass 1 isolates the host
//                caching mode.
//   write-rand   4 KiB random writes with guest F_NOCACHE and no flush, which
//                a Cached host can absorb in its page cache.
//
// Results are printed as one RESULT line per test for easy diffing.

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define BLOCK_SIZE 4096
#define SEQUENTIAL_SIZE (1 << 20)
#define MAX_SAMPLES 200000

static mach_timebase_info_data_t gTimebase;
static uint64_t *gSamples;
static size_t gSampleCount;

static uint64_t NowNanoseconds(void) {
    return mach_absolute_time() * gTimebase.numer / gTimebase.denom;
}

static void Die(const char *what) {
    fprintf(stderr, "disk-io-probe: %s: %s\n", what, strerror(errno));
    exit(1);
}

static uint64_t NextRandom(uint64_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

// Operations at least this slow are printed with their wall-clock end time so
// they can be matched against host-side traces.
#define STALL_REPORT_NS 50000000ULL

static void RecordSample(uint64_t nanoseconds) {
    if (gSampleCount < MAX_SAMPLES)
        gSamples[gSampleCount++] = nanoseconds;
    if (nanoseconds >= STALL_REPORT_NS) {
        struct timeval now;
        gettimeofday(&now, NULL);
        struct tm local;
        localtime_r(&now.tv_sec, &local);
        printf("STALL end=%02d:%02d:%02d.%03d ms=%.1f\n", local.tm_hour,
               local.tm_min, local.tm_sec, (int)(now.tv_usec / 1000),
               nanoseconds / 1e6);
    }
}

static int CompareSamples(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : a > b;
}

static double Percentile(double fraction) {
    if (gSampleCount == 0)
        return 0;
    size_t index = (size_t)(fraction * (double)(gSampleCount - 1));
    return gSamples[index] / 1000.0;
}

// Print one RESULT line and reset the latency samples for the next test.
static void Report(const char *test, uint64_t operations, uint64_t bytes,
                   uint64_t elapsed) {
    qsort(gSamples, gSampleCount, sizeof(gSamples[0]), CompareSamples);
    double seconds = elapsed / 1e9;
    printf("RESULT test=%s ops=%" PRIu64 " seconds=%.2f ops_per_s=%.1f "
           "MiB_per_s=%.1f p50_us=%.0f p99_us=%.0f max_us=%.0f\n",
           test, operations, seconds, operations / seconds,
           bytes / 1048576.0 / seconds, Percentile(0.50), Percentile(0.99),
           Percentile(1.0));
    fflush(stdout);
    gSampleCount = 0;
}

static int OpenUncached(const char *path, int flags) {
    int fd = open(path, flags, 0644);
    if (fd < 0)
        Die(path);
    if (fcntl(fd, F_NOCACHE, 1) != 0)
        Die("F_NOCACHE");
    return fd;
}

// Lay out the data file once with real (non-zero, incompressible) blocks so
// later reads cannot be satisfied from sparse holes.
static void PrepareDataFile(const char *path, uint64_t size) {
    struct stat info;
    if (stat(path, &info) == 0 && (uint64_t)info.st_size == size)
        return;
    int fd = OpenUncached(path, O_RDWR | O_CREAT | O_TRUNC);
    char *buffer = malloc(SEQUENTIAL_SIZE);
    uint64_t state = 0x9e3779b97f4a7c15ULL;
    for (uint64_t offset = 0; offset < size; offset += SEQUENTIAL_SIZE) {
        for (size_t word = 0; word < SEQUENTIAL_SIZE / 8; word++)
            ((uint64_t *)buffer)[word] = NextRandom(&state);
        if (pwrite(fd, buffer, SEQUENTIAL_SIZE, (off_t)offset) !=
            SEQUENTIAL_SIZE)
            Die("prepare write");
    }
    if (fcntl(fd, F_FULLFSYNC) != 0)
        Die("prepare F_FULLFSYNC");
    close(fd);
    free(buffer);
}

static int FlushFile(int fd, const char *kind) {
    if (strcmp(kind, "full") == 0)
        return fcntl(fd, F_FULLFSYNC);
    if (strcmp(kind, "barrier") == 0)
        return fcntl(fd, F_BARRIERFSYNC);
    return fsync(fd);
}

static void TestFlush(const char *path, uint64_t size, double duration,
                      const char *kind) {
    int fd = OpenUncached(path, O_RDWR);
    char buffer[BLOCK_SIZE];
    memset(buffer, 0x5a, sizeof(buffer));
    uint64_t blocks = size / BLOCK_SIZE, state = 0x1234567ULL, operations = 0;
    uint64_t start = NowNanoseconds(), deadline = start + duration * 1e9;
    uint64_t now = start;
    while (now < deadline) {
        off_t offset = (off_t)(NextRandom(&state) % blocks) * BLOCK_SIZE;
        uint64_t before = NowNanoseconds();
        if (pwrite(fd, buffer, BLOCK_SIZE, offset) != BLOCK_SIZE)
            Die("flush write");
        if (FlushFile(fd, kind) != 0)
            Die("flush");
        now = NowNanoseconds();
        RecordSample(now - before);
        operations++;
    }
    close(fd);
    char name[32];
    snprintf(name, sizeof(name), "flush-%s", kind);
    Report(name, operations, operations * BLOCK_SIZE, now - start);
}

static void TestSmallFiles(const char *directory, double duration) {
    char folder[1024], path[1100];
    snprintf(folder, sizeof(folder), "%s/smallfiles", directory);
    mkdir(folder, 0755);
    char buffer[16384];
    memset(buffer, 0xa5, sizeof(buffer));
    uint64_t operations = 0, start = NowNanoseconds();
    uint64_t deadline = start + duration * 1e9, now = start;
    while (now < deadline) {
        snprintf(path, sizeof(path), "%s/f%06" PRIu64, folder, operations);
        uint64_t before = NowNanoseconds();
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            Die(path);
        size_t length = 2048 + (operations % 7) * 2048;
        if (write(fd, buffer, length) != (ssize_t)length)
            Die("smallfile write");
        if (fcntl(fd, F_FULLFSYNC) != 0)
            Die("smallfile F_FULLFSYNC");
        close(fd);
        now = NowNanoseconds();
        RecordSample(now - before);
        operations++;
    }
    for (uint64_t index = 0; index < operations; index++) {
        snprintf(path, sizeof(path), "%s/f%06" PRIu64, folder, index);
        unlink(path);
    }
    rmdir(folder);
    Report("smallfiles", operations, 0, now - start);
}

// One timed pass of either 4 KiB random reads (a fixed seed, so pass 2 reads
// exactly the blocks pass 1 read) or 1 MiB sequential reads over [0, size).
static void ReadPass(const char *path, uint64_t size, int random, int pass) {
    int fd = OpenUncached(path, O_RDONLY);
    size_t length = random ? BLOCK_SIZE : SEQUENTIAL_SIZE;
    char *buffer = malloc(length);
    uint64_t operations = random ? MIN(size / BLOCK_SIZE / 4, MAX_SAMPLES)
                                 : size / SEQUENTIAL_SIZE;
    uint64_t blocks = size / BLOCK_SIZE, state = 0xfeedfaceULL;
    uint64_t start = NowNanoseconds(), now = start;
    for (uint64_t index = 0; index < operations; index++) {
        off_t offset = random
            ? (off_t)(NextRandom(&state) % blocks) * BLOCK_SIZE
            : (off_t)(index * SEQUENTIAL_SIZE);
        uint64_t before = NowNanoseconds();
        if (pread(fd, buffer, length, offset) != (ssize_t)length)
            Die("read");
        now = NowNanoseconds();
        RecordSample(now - before);
    }
    close(fd);
    free(buffer);
    char name[32];
    snprintf(name, sizeof(name), "read-%s-pass%d", random ? "rand" : "seq",
             pass);
    Report(name, operations, operations * length, now - start);
}

static void TestWriteRandom(const char *path, uint64_t size,
                            double duration) {
    int fd = OpenUncached(path, O_RDWR);
    char buffer[BLOCK_SIZE];
    memset(buffer, 0x3c, sizeof(buffer));
    uint64_t blocks = size / BLOCK_SIZE, state = 0xabcdefULL, operations = 0;
    uint64_t start = NowNanoseconds(), deadline = start + duration * 1e9;
    uint64_t now = start;
    while (now < deadline) {
        off_t offset = (off_t)(NextRandom(&state) % blocks) * BLOCK_SIZE;
        uint64_t before = NowNanoseconds();
        if (pwrite(fd, buffer, BLOCK_SIZE, offset) != BLOCK_SIZE)
            Die("random write");
        now = NowNanoseconds();
        RecordSample(now - before);
        operations++;
    }
    // Include the cost of making the burst durable, as a real workload would.
    uint64_t before = NowNanoseconds();
    if (fcntl(fd, F_FULLFSYNC) != 0)
        Die("random write F_FULLFSYNC");
    now = NowNanoseconds();
    close(fd);
    printf("NOTE write-rand final F_FULLFSYNC took %.1f ms\n",
           (now - before) / 1e6);
    Report("write-rand", operations, operations * BLOCK_SIZE, now - start);
}

static int Wants(const char *tests, const char *name) {
    if (strcmp(tests, "all") == 0)
        return 1;
    size_t length = strlen(name);
    for (const char *cursor = tests; (cursor = strstr(cursor, name));
         cursor += length) {
        int starts = cursor == tests || cursor[-1] == ',';
        int ends = cursor[length] == '\0' || cursor[length] == ',';
        if (starts && ends)
            return 1;
    }
    return 0;
}

int main(int argc, const char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: disk-io-probe <directory> [file-MiB=1024] "
                "[seconds-per-test=15] [tests=all]\n"
                "tests: flush,smallfiles,read,write (comma separated)\n");
        return 64;
    }
    mach_timebase_info(&gTimebase);
    gSamples = calloc(MAX_SAMPLES, sizeof(gSamples[0]));
    const char *directory = argv[1];
    uint64_t size = (argc > 2 ? strtoull(argv[2], NULL, 10) : 1024) << 20;
    double duration = argc > 3 ? strtod(argv[3], NULL) : 15;
    const char *tests = argc > 4 ? argv[4] : "all";
    if (size < SEQUENTIAL_SIZE || duration <= 0) {
        fprintf(stderr, "disk-io-probe: invalid size or duration\n");
        return 64;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/disk-io-probe.dat", directory);
    printf("CONFIG directory=%s file_MiB=%" PRIu64 " seconds=%.0f tests=%s\n",
           directory, size >> 20, duration, tests);
    fflush(stdout);
    PrepareDataFile(path, size);

    if (Wants(tests, "flush")) {
        TestFlush(path, size, duration, "full");
        TestFlush(path, size, duration, "barrier");
        TestFlush(path, size, duration, "fsync");
    }
    if (Wants(tests, "smallfiles"))
        TestSmallFiles(directory, duration);
    if (Wants(tests, "read")) {
        ReadPass(path, size, 1, 1);
        ReadPass(path, size, 1, 2);
        ReadPass(path, size, 0, 1);
        ReadPass(path, size, 0, 2);
    }
    if (Wants(tests, "write"))
        TestWriteRandom(path, size, duration);
    return 0;
}
