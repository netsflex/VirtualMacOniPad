// BTAudioHALPlugin reconnect limiter (nfzerox/VirtualMacOniPad#71).
//
// With guest audio enabled, CoreAudio loads the system Bluetooth HAL plugin
// into the VMM. bluetoothd rejects the VMM's signing identity, and on customer
// builds the plugin's XPC error handler reconnects at once: its check of
// os_variant_has_internal_content("com.apple.bluetooth") skips the 30 second
// "bluetoothd is rejecting connection" delay that internal builds take. The
// result is millions of connection attempts a minute, loading bluetoothd,
// launchd and logd.
//
// Redirect that one conditional branch to the plugin's own delayed path, in
// this process's private copy of the page. The plugin keeps retrying, so
// Bluetooth behaves as before if bluetoothd ever accepts the connection.
// The site is located by instruction signature rather than a per-build
// offset; anything but exactly one complete match leaves the plugin untouched.
#include <libkern/OSCacheControl.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach/mach.h>

static int btaudio_retry_done;

static BOOL btaudio_is_bl(uint32_t word) { return (word & 0xfc000000u) == 0x94000000u; }

// Returns the word index of the reconnect branch and its delayed target.
static BOOL btaudio_find_site(const uint32_t *code, size_t count,
                              size_t *site, size_t *delayed) {
    unsigned matches = 0;
    for (size_t i = 3; i + 4 < count; ++i) {
        // mov x0, x19; bl <reset>; bl <internal build?>; cbz w0, <reconnect now>
        // nop; ldr x8, =_xpc_error_connection_interrupted; cmp x20, x8
        if (code[i - 3] != 0xaa1303e0u || !btaudio_is_bl(code[i - 2]) ||
            !btaudio_is_bl(code[i - 1]) || (code[i] & 0xff00001fu) != 0x34000000u ||
            code[i + 1] != 0xd503201fu || (code[i + 2] & 0xff00001fu) != 0x58000008u ||
            code[i + 3] != 0xeb08029fu) continue;
        // The branch taken on customer builds: mov x0, x19; bl <connect>; b <out>
        size_t now = i + ((code[i] >> 5) & 0x7ffffu);
        if ((code[i] & 0x00800000u) || now + 2 >= count || code[now] != 0xaa1303e0u ||
            !btaudio_is_bl(code[now + 1]) || (code[now + 2] & 0xfc000000u) != 0x14000000u)
            continue;
        // The delayed path loads x20 = 30,000,000,000 ns and passes it on:
        // mov x0, #0; mov x1, x20; bl _dispatch_time
        size_t target = 0;
        unsigned targets = 0;
        for (size_t j = i + 4; j + 2 < count && j < i + 256; ++j) {
            if (code[j] != 0xd2958014u || code[j + 1] != 0xf2bf8474u ||
                code[j + 2] != 0xf2c000d4u) continue;
            for (size_t k = j + 3; k + 2 < count && k < j + 40; ++k)
                if (code[k] == 0xd2800000u && code[k + 1] == 0xaa1403e1u &&
                    btaudio_is_bl(code[k + 2])) { target = j; ++targets; break; }
        }
        if (targets != 1) continue;
        *site = i;
        *delayed = target;
        ++matches;
    }
    return matches == 1;
}

static void btaudio_retry_image_added(const struct mach_header *header, intptr_t slide) {
    (void)slide;
    static const char suffix[] = "/BTAudioHALPlugin.driver/BTAudioHALPlugin";
    Dl_info information;
    if (!dladdr(header, &information) || !information.dli_fname) return;
    size_t length = strlen(information.dli_fname);
    if (length < sizeof(suffix) - 1 ||
        strcmp(information.dli_fname + length - (sizeof(suffix) - 1), suffix) ||
        header->magic != MH_MAGIC_64 || header->filetype != MH_BUNDLE) return;
    if (__atomic_exchange_n(&btaudio_retry_done, 1, __ATOMIC_ACQ_REL)) return;

    unsigned long size = 0;
    uint32_t *code = (uint32_t *)getsectiondata(
        (const struct mach_header_64 *)header, "__TEXT", "__text", &size);
    size_t site = 0, delayed = 0;
    if (!code || ((uintptr_t)code & 3) ||
        !btaudio_find_site(code, size / sizeof(uint32_t), &site, &delayed)) {
        logf_("[BTAudioRetry] plugin not recognized; reconnect path left unchanged");
        return;
    }
    uint32_t original = code[site];
    uint32_t replacement = 0x14000000u | (uint32_t)((delayed - site) & 0x03ffffffu);
    vm_address_t address = (vm_address_t)&code[site];
    vm_address_t page = address & ~((vm_address_t)vm_page_size - 1);
    // Keep execute permission throughout; COPY makes the page private to the VMM.
    kern_return_t result = vm_protect(mach_task_self(), page, vm_page_size, false,
        VM_PROT_READ | VM_PROT_EXECUTE | VM_PROT_WRITE | VM_PROT_COPY);
    if (result == KERN_SUCCESS) {
        result = vm_write(mach_task_self(), address,
                          (vm_offset_t)(uintptr_t)&replacement, sizeof(replacement));
        sys_icache_invalidate((void *)address, sizeof(replacement));
        kern_return_t restored = vm_protect(mach_task_self(), page, vm_page_size, false,
                                            VM_PROT_READ | VM_PROT_EXECUTE);
        if (restored != KERN_SUCCESS)
            logf_("[BTAudioRetry] code page left writable kr=%d", restored);
    }
    logf_("[BTAudioRetry] site=+0x%lx 0x%08x -> 0x%08x delayed=+0x%lx kr=%d applied=%d",
          (unsigned long)((uintptr_t)address - (uintptr_t)header), original, replacement,
          (unsigned long)((uintptr_t)&code[delayed] - (uintptr_t)header), result,
          code[site] == replacement);
}

static void btaudio_retry_install(void) {
    const char *mode = getenv("VMMHOOK_BTAUDIO_RETRY");
    if (mode && !strcmp(mode, "0")) return;
    // Also runs for images that are already loaded.
    _dyld_register_func_for_add_image(btaudio_retry_image_added);
}
