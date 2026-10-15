#!/bin/sh

# On-device launcher shared by the UIKit app and SSH automation. It prewarms
# DeviceSupport's usbmuxd before VZMacOSInstaller reaches the RestoreOS USB
# handoff, then starts the real Apple installation stack in the background.

set -eu
# System applications do not inherit the interactive jailbreak shell PATH.
# Keep every utility used below resolvable when this script is exec'd by the
# setuid install-launcher from UIKit.
jb_prefix=/var/jb
test -x /var/jb/usr/bin/launchctl || jb_prefix=
launchctl="$jb_prefix/usr/bin/launchctl"
PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin:/usr/sbin:/sbin
export PATH
trap 'status=$?; if [ "$status" -ne 0 ] && { [ -z "${log:-}" ] || ! grep -q "INSTALL_FAILED" "$log" 2>/dev/null; }; then echo "INSTALL_FAILED launcher status=$status"; fi' EXIT

if [ "$#" -ne 7 ]; then
    echo "usage: start-install.sh IPSW STAGING_BUNDLE FINAL_BUNDLE LOG CPU MEMORY_BYTES DISK_BYTES" >&2
    exit 2
fi

# The launcher executes this script at <runtime>/install/start-install.sh.
remote=$(cd "$(dirname "$0")/.." && pwd -P)
host_version=$(sw_vers -productVersion)
ipsw=$1
staging=$2
final=$3
log=$4
cpu=$5
memory=$6
disk=$7
usbmuxd="$remote/payload/Installation.xpc/Contents/Frameworks/MobileDevice.framework/Versions/A/Resources/usbmuxd"
third_party_usbmux_jobs=/tmp/virtualmac-third-party-usbmuxd.jobs

test -f "$ipsw"
test ! -e "$staging"
test ! -e "$final"
echo "INSTALL_PREPARE_BEGIN ipsw=$ipsw"
case "$staging" in
    /var/mobile/Media/VirtualMac/Installations/*.bundle.installing) ;;
    *) echo "staging bundle must be in Virtual Mac installation storage" >&2; exit 2 ;;
esac
test "${final%/*}" = /var/mobile/Media/VirtualMac || {
    echo "final bundle must be directly in /var/mobile/Media/VirtualMac" >&2
    exit 2
}

# Procursus/libimobiledevice can install its own launchd-managed usbmuxd. It
# races the matching DeviceSupport daemon below for /var/run/usbmuxd and forces
# the virtual restore device back into DFU (MobileRestore error 4014). Unload
# only jailbreak-provided jobs whose plist explicitly runs usbmuxd. A monitor
# restores each original job after this installer process exits.
: >"$third_party_usbmux_jobs"
for plist in "$jb_prefix"/Library/LaunchDaemons/*.plist; do
    test -f "$plist" || continue
    grep -q '/usbmuxd\|>usbmuxd<' "$plist" 2>/dev/null || continue
    # iPadOS 16.0/16.1 do not ship /usr/bin/plutil, and a fresh Dopamine
    # bootstrap does not necessarily provide another copy. These jailbreak
    # launchd plists are XML (the grep above intentionally only matches XML),
    # so read the Label without introducing a package dependency. The old
    # plutil call returned an empty label on 16.1, leaving a KeepAlive
    # libimobiledevice usbmuxd job active throughout restore.
    label=$(sed -n '/<key>Label<\/key>/{
        n
        s/.*<string>\([^<]*\)<\/string>.*/\1/p
        q
    }' "$plist" 2>/dev/null || true)
    test -n "$label" || continue
    # Dopamine rootless LaunchDaemons normally live in user/501. Query it
    # first because launchctl also accepts system/<label> as an alias while
    # warning that the service belongs to the user domain; recording that
    # alias would later restore the job into the wrong domain.
    for domain in user/501 system; do
        if "$launchctl" print "$domain/$label" >/dev/null 2>&1; then
            bootout_status=0
            "$launchctl" bootout "$domain/$label" \
                >/dev/null 2>&1 || bootout_status=$?
            # iPadOS 14's launchctl can return an error after it has already
            # removed a rootful system job. This is reproducible with
            # Procursus org.libimobiledevice.usbmuxd: the command is nonzero,
            # but a subsequent print reports that the service no longer
            # exists. Accept only that proven 14.x state; keep the established
            # strict result handling on iPadOS 15 and 16.
            bootout_succeeded=0
            if test "$bootout_status" -eq 0; then
                bootout_succeeded=1
            elif test "$host_version" != "${host_version#14.}" &&
                    ! "$launchctl" print "$domain/$label" \
                        >/dev/null 2>&1; then
                bootout_succeeded=1
                echo "INSTALL_USB_CONFLICT_BOOTOUT_STATUS_IGNORED host=$host_version status=$bootout_status domain=$domain label=$label"
            fi
            if test "$bootout_succeeded" -eq 1; then
                printf '%s\t%s\t%s\n' "$domain" "$label" "$plist" \
                    >>"$third_party_usbmux_jobs"
                echo "INSTALL_USB_CONFLICT_DISABLED domain=$domain label=$label"
            else
                echo "INSTALL_FAILED could not disable conflicting usbmuxd job $domain/$label"
                exit 1
            fi
            break
        fi
    done
done
installer_pid=$$
if test -s "$third_party_usbmux_jobs"; then
    (
        while kill -0 "$installer_pid" 2>/dev/null; do sleep 1; done
        while IFS="$(printf '\t')" read -r domain label plist; do
            "$launchctl" bootstrap "$domain" "$plist" \
                >/dev/null 2>&1 || true
            "$launchctl" kickstart "$domain/$label" \
                >/dev/null 2>&1 || true
        done <"$third_party_usbmux_jobs"
        rm -f "$third_party_usbmux_jobs"
    ) >/tmp/virtualmac-usbmuxd-restore.log 2>&1 &
fi
case "$final" in
    *.bundle) ;;
    *) echo "final bundle must have a .bundle suffix" >&2; exit 2 ;;
esac

# Darwin truncates the process name used by killall, so the Installation.xpc
# executable's full bundle name is never matched reliably. Kill only helpers
# whose complete command path belongs to this payload; otherwise an orphaned
# listener can retain global usbmux/endpoint state across restore attempts.
ps -axo pid=,command= | while read -r stale_pid stale_command; do
    case "$stale_command" in
        "$remote/payload/Installation.xpc/Contents/MacOS/com.apple.Virtualization.Installation")
            kill "$stale_pid" 2>/dev/null || true
            ;;
    esac
done
sleep 0.1
ps -axo pid=,command= | while read -r stale_pid stale_command; do
    case "$stale_command" in
        "$remote/payload/Installation.xpc/Contents/MacOS/com.apple.Virtualization.Installation")
            kill -9 "$stale_pid" 2>/dev/null || true
            ;;
    esac
done
killall install-macos 2>/dev/null || true
killall usbmuxd 2>/dev/null || true
if test -f /tmp/vz-usbmuxd-launch.pid; then
    kill "$(cat /tmp/vz-usbmuxd-launch.pid)" 2>/dev/null || true
fi
rm -f /var/run/usbmuxd /tmp/vzusbmuxd /tmp/vz-usbmuxd-enable
rm -f /tmp/installation_ep.txt /tmp/vmm_ep.txt \
    /tmp/installation.stderr.log /tmp/installationhook.log \
    /tmp/installation-usb.log /tmp/restore-vmm.stderr.log \
    /tmp/vmm.stderr.log /tmp/vmmhook.log \
    /tmp/vzxpchook.log /tmp/vz-usbmuxd.log

DYLD_INSERT_LIBRARIES="$remote/payload/Installation.xpc/Contents/Frameworks/InstallationCompat.dylib" \
INSTALL_USB_POLL_US=250000 \
INSTALL_USB_ENABLE_FILE=/tmp/vz-usbmuxd-enable \
    "$usbmuxd" -debug 5 >/tmp/vz-usbmuxd.log 2>&1 &
echo $! >/tmp/vz-usbmuxd.pid

# DeviceSupport's private usbmuxd is needed only for the restore transport.
# The shell is replaced by install-macos below, so a small watcher owns helper
# cleanup after that process exits. Without this, successful restores leave a
# polling usbmuxd consuming CPU until the next restore or reboot.
restore_process_pid=$$
(
    while kill -0 "$restore_process_pid" 2>/dev/null; do sleep 1; done
    helper_pid=$(cat /tmp/vz-usbmuxd.pid 2>/dev/null || true)
    if test -n "$helper_pid"; then
        kill "$helper_pid" 2>/dev/null || true
        attempt=0
        while kill -0 "$helper_pid" 2>/dev/null; do
            attempt=$((attempt + 1))
            test "$attempt" -lt 20 || {
                kill -9 "$helper_pid" 2>/dev/null || true
                break
            }
            sleep 0.1
        done
    fi
    if test -L /var/run/usbmuxd &&
            test "$(readlink /var/run/usbmuxd 2>/dev/null || true)" = /tmp/vzusbmuxd; then
        rm -f /var/run/usbmuxd
    fi
    rm -f /tmp/vzusbmuxd /tmp/vz-usbmuxd.pid /tmp/vz-usbmuxd-enable
) >/tmp/vz-usbmuxd-cleanup.log 2>&1 &

attempt=0
while ! test -S /tmp/vzusbmuxd; do
    attempt=$((attempt + 1))
    test "$attempt" -lt 30 || {
        echo "usbmuxd did not create /tmp/vzusbmuxd" >&2
        exit 1
    }
    sleep 0.1
done

# Coordinate the fake RestoreOS device with the matching bundled usbmuxd.
(
    while ! grep -q 'fake USB device descriptor: .* ac 05 ac 12' \
        /tmp/vmmhook.log 2>/dev/null; do
        kill -0 "$installer_pid" 2>/dev/null || exit 0
        sleep 0.01
    done
    while ! grep -q 'RestoreOS USBMux handshake cached generation=' \
        /tmp/vmmhook.log 2>/dev/null; do
        kill -0 "$installer_pid" 2>/dev/null || exit 0
        sleep 0.01
    done
    touch /tmp/vz-usbmuxd-enable
    ln -s /tmp/vzusbmuxd /var/run/usbmuxd
) >/tmp/vz-usbmuxd-launch.log 2>&1 &
echo $! >/tmp/vz-usbmuxd-launch.pid

# iPadOS cannot create Ventura's kernel-backed IOUSBHostControllerInterface.
# The userspace controller bridge carries the genuine virtual DFU endpoint
# traffic to MobileDevice and is required for unattended on-device restores.
export VZ_INSTALL_CPU_COUNT="$cpu"
export VZ_INSTALL_MEMORY_SIZE="$memory"
export VZ_INSTALL_STORAGE_SIZE="$disk"
export VZ_VMM_STDERR_LOG=/tmp/restore-vmm.stderr.log
# RestoreOS must never inherit the normal-boot guest kernel policy from the
# UIKit process. The app deliberately keeps that setting in its environment
# for a running OpenGL-enabled VM, and install-launcher inherits the app's
# environment. Without clearing it here, a later restore in the same app
# process can run the SIP/CSR worker against the temporary RestoreOS kernel.
unset VZ_GUEST_RUNTIME_POLICY
# The setuid restore host must create its own rendezvous names. Inheriting the
# UIKit app's mobile-owned names lets the root VMM replace them and can prevent
# every later VM from starting. The host hook generates root-scoped paths.
unset VZ_VMM_ENDPOINT_FILE VZ_INSTALLATION_ENDPOINT_FILE
export VMMHOOK_TRACE_USB="${VMMHOOK_TRACE_USB:-0}"
export VMMHOOK_TRACE_IOKIT="${VMMHOOK_TRACE_IOKIT:-1}"
export VMMHOOK_TRACE_XPC_LIMIT="${VMMHOOK_TRACE_XPC_LIMIT:-200}"
export VZ_DEBUG_LOGGING=1
export VMMHOOK_FAKE_USB="${VMMHOOK_FAKE_USB:-1}"
export INSTALL_USB_DEBUG_DELAY_MS="${INSTALL_USB_DEBUG_DELAY_MS:-}"
export INSTALL_USB_TRACE_TRANSFERS="${INSTALL_USB_TRACE_TRANSFERS:-0}"

# Keep install-macos as the validated setuid launcher's main process. The
# successful visible restore proved that a separate launchd coalition is not
# required; exec preserves errors and termination directly in the UI log.
echo $$ >/tmp/install-macos.pid
echo "INSTALL_LAUNCHED pid=$$ log=$log"
exec "$remote/install/install-macos" "$ipsw" "$staging" "$final" >>"$log" 2>&1
