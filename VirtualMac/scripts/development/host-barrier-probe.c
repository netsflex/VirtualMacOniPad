// iPad host-side sync latency probe, run without any VM.
//
// Repeats a 4 KiB random overwrite followed by F_BARRIERFSYNC (or fsync /
// F_FULLFSYNC) on a scratch file, and prints every operation slower than the
// stall threshold with its wall-clock end time. Comparing the stall times with
// the VMM's Disk.img trace shows whether slow guest barriers come from the VM
// or from periodic iPadOS file system work.
//
//   host-barrier-probe <file> [seconds=120] [barrier|fsync|full]
//                      [cached|uncached] [pause-us=0] [full-every-ms=0]
//
// pause-us sleeps between operations to lower the write rate; full-every-ms
// adds an F_FULLFSYNC at that interval on the same descriptor.

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define BLOCK_SIZE 4096
#define FILE_SIZE (64ULL << 20)
#define STALL_NS 50000000ULL

static mach_timebase_info_data_t gTimebase;

static uint64_t NowNanoseconds(void) {
    return mach_absolute_time() * gTimebase.numer / gTimebase.denom;
}

static void PrintWallClock(const char *label, double milliseconds) {
    struct timeval now;
    gettimeofday(&now, NULL);
    struct tm local;
    localtime_r(&now.tv_sec, &local);
    printf("%s end=%02d:%02d:%02d.%03d ms=%.1f\n", label, local.tm_hour,
           local.tm_min, local.tm_sec, (int)(now.tv_usec / 1000),
           milliseconds);
    fflush(stdout);
}

int main(int argc, const char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: host-barrier-probe <file> [seconds=120] "
                "[barrier|fsync|full] [cached|uncached] [pause-us=0] "
                "[full-every-ms=0]\n");
        return 64;
    }
    mach_timebase_info(&gTimebase);
    double seconds = argc > 2 ? strtod(argv[2], NULL) : 120;
    const char *kind = argc > 3 ? argv[3] : "barrier";
    int uncached = argc > 4 && strcmp(argv[4], "uncached") == 0;
    useconds_t pause = argc > 5 ? (useconds_t)strtoul(argv[5], NULL, 10) : 0;
    uint64_t fullEvery = (argc > 6 ? strtoull(argv[6], NULL, 10) : 0) * 1000000;
    uint64_t nextFull = 0;

    int fd = open(argv[1], O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }
    if (uncached && fcntl(fd, F_NOCACHE, 1) != 0) {
        perror("F_NOCACHE");
        return 1;
    }
    char buffer[BLOCK_SIZE];
    memset(buffer, 0x6b, sizeof(buffer));
    for (uint64_t offset = 0; offset < FILE_SIZE; offset += BLOCK_SIZE)
        if (pwrite(fd, buffer, BLOCK_SIZE, (off_t)offset) != BLOCK_SIZE) {
            perror("prepare");
            return 1;
        }
    fcntl(fd, F_FULLFSYNC);

    uint64_t state = 0x2545f4914f6cdd1dULL, operations = 0, slow = 0;
    uint64_t worst = 0, total = 0;
    uint64_t start = NowNanoseconds(), deadline = start + seconds * 1e9;
    uint64_t fulls = 0, fullWorst = 0;
    nextFull = start + fullEvery;
    PrintWallClock("START", 0);
    while (NowNanoseconds() < deadline) {
        if (fullEvery && NowNanoseconds() >= nextFull) {
            uint64_t fullStart = NowNanoseconds();
            fcntl(fd, F_FULLFSYNC);
            uint64_t fullElapsed = NowNanoseconds() - fullStart;
            if (fullElapsed > fullWorst)
                fullWorst = fullElapsed;
            fulls++;
            nextFull += fullEvery;
        }
        if (pause)
            usleep(pause);
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        off_t offset = (off_t)(state % (FILE_SIZE / BLOCK_SIZE)) * BLOCK_SIZE;
        uint64_t before = NowNanoseconds();
        if (pwrite(fd, buffer, BLOCK_SIZE, offset) != BLOCK_SIZE) {
            perror("write");
            return 1;
        }
        int result = strcmp(kind, "fsync") == 0 ? fsync(fd)
            : strcmp(kind, "full") == 0 ? fcntl(fd, F_FULLFSYNC)
            : fcntl(fd, F_BARRIERFSYNC);
        if (result != 0) {
            perror(kind);
            return 1;
        }
        uint64_t elapsed = NowNanoseconds() - before;
        total += elapsed;
        operations++;
        if (elapsed > worst)
            worst = elapsed;
        if (elapsed >= STALL_NS) {
            slow++;
            PrintWallClock("STALL", elapsed / 1e6);
        }
    }
    printf("RESULT kind=%s mode=%s pause_us=%u ops=%" PRIu64 " mean_us=%.0f "
           "max_ms=%.1f stalls=%" PRIu64 " extra_full=%" PRIu64
           " extra_full_max_ms=%.1f\n", kind,
           uncached ? "uncached" : "cached", (unsigned)pause, operations,
           total / 1e3 / operations, worst / 1e6, slow, fulls,
           fullWorst / 1e6);
    close(fd);
    unlink(argv[1]);
    return 0;
}
