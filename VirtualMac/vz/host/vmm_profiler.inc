// On-demand on-CPU profile of the VMM process.
//
// Creating /var/root/VirtualMac2/diagnostics/vmm-profile-now starts a ten
// second profile; the file is removed when it starts. Every few milliseconds
// each running thread is suspended just long enough to copy its frame-pointer
// chain into a preallocated buffer; symbols are resolved only after sampling
// has finished, so no suspended thread can be holding a lock the sampler
// needs. The result goes to vmmhook.log: the busiest threads with the frames
// most of their samples pass through, then whole-process totals. Until the
// file appears, the only cost is checking for it every two seconds.
#include <mach/mach.h>

enum { vmmProfileDepth = 24, vmmProfileCapacity = 60000 };
typedef struct { uint32_t thread; uint16_t index; uint8_t depth; uintptr_t pc[vmmProfileDepth]; } VMMProfileSample;
static int vmm_profile_running;

static uintptr_t vmm_profile_strip(uintptr_t pc) {
#if __has_feature(ptrauth_calls)
    return (uintptr_t)ptrauth_strip((void *)pc, ptrauth_key_return_address);
#else
    return pc;
#endif
}

static int vmm_profile_compare(const void *a, const void *b) {
    const uint64_t *x = a, *y = b;
    return x[1] < y[1] ? 1 : x[1] > y[1] ? -1 : 0;
}

// Name a code address as symbol, or as image+offset where the image is stripped.
static void vmm_profile_name(uintptr_t pc, char *out, size_t size) {
    Dl_info information = {0};
    if (!dladdr((void *)pc, &information)) { snprintf(out, size, "0x%lx", (unsigned long)pc); return; }
    const char *image = information.dli_fname ? strrchr(information.dli_fname, '/') : NULL;
    image = image ? image + 1 : "?";
    if (information.dli_sname && strcmp(information.dli_sname, "<redacted>"))
        snprintf(out, size, "%s %s", image, information.dli_sname);
    else
        snprintf(out, size, "%s+0x%lx", image, (unsigned long)(pc - (uintptr_t)information.dli_fbase));
}

// For the busiest threads, list the frames that appear in most of their
// samples, by exact return address, so stripped code can be found in a
// disassembly.
static void vmm_profile_report_threads(VMMProfileSample *samples, unsigned count) {
    enum { threadSlots = 128, frameSlots = 2048 };
    static uint32_t threadCounts[threadSlots];
    memset(threadCounts, 0, sizeof(threadCounts));
    for (unsigned i = 0; i < count; ++i)
        if (samples[i].index < threadSlots) ++threadCounts[samples[i].index];
    for (int rank = 0; rank < 4; ++rank) {
        unsigned best = 0;
        for (unsigned t = 1; t < threadSlots; ++t) if (threadCounts[t] > threadCounts[best]) best = t;
        if (!best || !threadCounts[best]) break;
        unsigned total = threadCounts[best];
        threadCounts[best] = 0;
        char name[64] = "";
        static uint64_t frames[frameSlots][2];
        memset(frames, 0, sizeof(frames));
        for (unsigned i = 0; i < count; ++i) {
            if (samples[i].index != best) continue;
            if (!name[0]) {
                pthread_t pthread = pthread_from_mach_thread_np(samples[i].thread);
                if (pthread) pthread_getname_np(pthread, name, sizeof(name));
            }
            for (unsigned d = 0; d < samples[i].depth; ++d) {
                uintptr_t key = samples[i].pc[d];
                for (unsigned probe = 0; probe < frameSlots; ++probe) {
                    unsigned slot = (unsigned)((key >> 2) * 2654435761u + probe) % frameSlots;
                    if (frames[slot][0] && frames[slot][0] != key) continue;
                    frames[slot][0] = key;
                    ++frames[slot][1];
                    break;
                }
            }
        }
        qsort(frames, frameSlots, sizeof(frames[0]), vmm_profile_compare);
        logf_("[VMMProfile] thread %u \"%s\" %u samples", best, name, total);
        for (unsigned i = 0; i < 24 && frames[i][1]; ++i) {
            char text[256];
            vmm_profile_name((uintptr_t)frames[i][0], text, sizeof(text));
            logf_("[VMMProfile]   %5.1f%% %s", 100.0 * frames[i][1] / total, text);
        }
    }
}

static void vmm_profile_report(VMMProfileSample *samples, unsigned count, unsigned ticks) {
    // Count, per function start address, the samples it is the leaf of and the
    // samples it appears in at all (once per sample).
    enum { slots = 4096 };
    static uint64_t leaf[slots][2], inclusive[slots][2];
    memset(leaf, 0, sizeof(leaf));
    memset(inclusive, 0, sizeof(inclusive));
    for (unsigned i = 0; i < count; ++i) {
        uintptr_t seen[vmmProfileDepth];
        unsigned seenCount = 0;
        for (unsigned d = 0; d < samples[i].depth; ++d) {
            Dl_info information = {0};
            uintptr_t pc = samples[i].pc[d], key = pc;
            if (dladdr((void *)(d ? pc - 4 : pc), &information) && information.dli_saddr)
                key = (uintptr_t)information.dli_saddr;
            BOOL duplicate = NO;
            for (unsigned k = 0; k < seenCount; ++k) if (seen[k] == key) duplicate = YES;
            if (duplicate) continue;
            seen[seenCount++] = key;
            for (int table = d ? 1 : 0; table < 2; ++table) {
                uint64_t (*bucket)[2] = table ? inclusive : leaf;
                for (unsigned probe = 0; probe < slots; ++probe) {
                    unsigned slot = (unsigned)((key >> 2) * 2654435761u + probe) % slots;
                    if (bucket[slot][0] && bucket[slot][0] != key) continue;
                    bucket[slot][0] = key;
                    ++bucket[slot][1];
                    break;
                }
            }
        }
    }
    vmm_profile_report_threads(samples, count);
    qsort(leaf, slots, sizeof(leaf[0]), vmm_profile_compare);
    qsort(inclusive, slots, sizeof(inclusive[0]), vmm_profile_compare);
    logf_("[VMMProfile] %u on-CPU thread samples over %u ticks", count, ticks);
    for (int table = 0; table < 2; ++table) {
        uint64_t (*bucket)[2] = table ? inclusive : leaf;
        for (unsigned i = 0; i < 40 && bucket[i][1]; ++i) {
            Dl_info information = {0};
            dladdr((void *)bucket[i][0], &information);
            const char *image = information.dli_fname ? strrchr(information.dli_fname, '/') : NULL;
            logf_("[VMMProfile] %s %6llu %5.1f%% %s %s", table ? "incl" : "self",
                  (unsigned long long)bucket[i][1], 100.0 * bucket[i][1] / (count ? count : 1),
                  image ? image + 1 : "?", information.dli_sname ? information.dli_sname : "?");
        }
    }
}

static void vmm_profile_run(double seconds) {
    VMMProfileSample *samples = calloc(vmmProfileCapacity, sizeof(VMMProfileSample));
    if (!samples) { __atomic_store_n(&vmm_profile_running, 0, __ATOMIC_RELEASE); return; }
    unsigned count = 0, ticks = 0;
    thread_act_t self = mach_thread_self();
    uint64_t end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + (uint64_t)(seconds * 1e9);
    while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < end && count < vmmProfileCapacity) {
        ++ticks;
        thread_act_array_t threads = NULL;
        mach_msg_type_number_t threadCount = 0;
        if (task_threads(mach_task_self(), &threads, &threadCount) != KERN_SUCCESS) break;
        for (mach_msg_type_number_t t = 0; t < threadCount; ++t) {
            thread_act_t thread = threads[t];
            if (thread == self || count >= vmmProfileCapacity) goto next;
            struct thread_basic_info basic;
            mach_msg_type_number_t basicCount = THREAD_BASIC_INFO_COUNT;
            if (thread_info(thread, THREAD_BASIC_INFO, (thread_info_t)&basic, &basicCount) ||
                basic.run_state != TH_STATE_RUNNING) goto next;
            pthread_t pthread = pthread_from_mach_thread_np(thread);
            uintptr_t top = pthread ? (uintptr_t)pthread_get_stackaddr_np(pthread) : 0;
            uintptr_t bottom = pthread ? top - pthread_get_stacksize_np(pthread) : 0;
            if (thread_suspend(thread) != KERN_SUCCESS) goto next;
            arm_thread_state64_t state;
            mach_msg_type_number_t stateCount = ARM_THREAD_STATE64_COUNT;
            if (thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&state, &stateCount) ==
                KERN_SUCCESS) {
                VMMProfileSample *sample = &samples[count++];
                sample->thread = thread;
                sample->index = (uint16_t)(t + 1);
                sample->pc[0] = vmm_profile_strip(arm_thread_state64_get_pc(state));
                sample->depth = 1;
                uintptr_t lr = vmm_profile_strip(arm_thread_state64_get_lr(state));
                if (lr && lr != sample->pc[0]) sample->pc[sample->depth++] = lr;
                uintptr_t fp = arm_thread_state64_get_fp(state);
                while (sample->depth < vmmProfileDepth && top && fp >= bottom && fp + 16 <= top &&
                       !(fp & 7)) {
                    uintptr_t next = ((uintptr_t *)fp)[0], pc = vmm_profile_strip(((uintptr_t *)fp)[1]);
                    if (!pc) break;
                    if (sample->depth != 2 || pc != sample->pc[1]) sample->pc[sample->depth++] = pc;
                    if (next <= fp) break;
                    fp = next;
                }
            }
            thread_resume(thread);
        next:
            mach_port_deallocate(mach_task_self(), thread);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)threads, threadCount * sizeof(thread_act_t));
        usleep(2000);
    }
    mach_port_deallocate(mach_task_self(), self);
    vmm_profile_report(samples, count, ticks);
    free(samples);
    __atomic_store_n(&vmm_profile_running, 0, __ATOMIC_RELEASE);
}

// Profile the VMM for ten seconds whenever this file appears; it is removed
// when the profile starts.
static const char vmmProfileNowFlag[] = "/var/root/VirtualMac2/diagnostics/vmm-profile-now";
static void vmm_profile_watch(void) {
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                              2 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
    dispatch_source_set_event_handler(timer, ^{
        if (access(vmmProfileNowFlag, F_OK) || unlink(vmmProfileNowFlag) ||
            __atomic_exchange_n(&vmm_profile_running, 1, __ATOMIC_ACQ_REL)) return;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ vmm_profile_run(10.0); });
    });
    dispatch_resume(timer);
}

static void vmm_profiler_install(void) { vmm_profile_watch(); }
