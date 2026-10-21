#!/bin/bash
# Set or clear the developer Disk.img caching/synchronization policy.
#
#   set-disk-policy-override.sh <automatic|uncached|cached> <full|fsync|none>
#   set-disk-policy-override.sh clear        # back to the default, cached + fsync
#
# VZ_IPAD_SSH names the iPad's root SSH target (for example root@<address>);
# key authentication is expected. The app keeps Settings.plist in memory and
# rewrites it on every settings change, so quit Virtual Mac first; the policy
# takes effect at its next VM boot. "none" skips host flushes entirely and can
# corrupt the guest file system if the VMM dies; use it only for diagnosis.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../lib/common.sh
source "$SCRIPT_DIR/../lib/common.sh"

need_command plutil
: "${VZ_IPAD_SSH:?set VZ_IPAD_SSH to the iPad root SSH target}"

SETTINGS="/var/mobile/Media/VirtualMac/Settings.plist"
CACHING_KEY=DebugDiskCachingMode
SYNCHRONIZATION_KEY=DebugDiskSynchronizationMode

if [[ "${1:-}" == clear && $# == 1 ]]; then
    MODE=clear
elif [[ $# == 2 && "$1" =~ ^(automatic|uncached|cached)$ &&
        "$2" =~ ^(full|fsync|none)$ ]]; then
    MODE=set
else
    die "usage: $0 <automatic|uncached|cached> <full|fsync|none> | clear"
fi

if ssh "$VZ_IPAD_SSH" "killall -0 VirtualMac 2>/dev/null"; then
    die "quit Virtual Mac on the iPad first; it would overwrite Settings.plist"
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
LOCAL="$WORK/Settings.plist"
if ! scp -q "$VZ_IPAD_SSH:$SETTINGS" "$LOCAL" 2>/dev/null; then
    plutil -create xml1 "$LOCAL"
fi

plutil -remove "$CACHING_KEY" "$LOCAL" 2>/dev/null || true
plutil -remove "$SYNCHRONIZATION_KEY" "$LOCAL" 2>/dev/null || true
if [[ "$MODE" == set ]]; then
    plutil -insert "$CACHING_KEY" -string "$1" "$LOCAL"
    plutil -insert "$SYNCHRONIZATION_KEY" -string "$2" "$LOCAL"
fi

ssh "$VZ_IPAD_SSH" "mkdir -p '$(dirname "$SETTINGS")'"
scp -q "$LOCAL" "$VZ_IPAD_SSH:$SETTINGS"
ssh "$VZ_IPAD_SSH" "chown mobile:mobile '$SETTINGS'"

if [[ "$MODE" == set ]]; then
    echo "disk policy override: caching=$1 synchronization=$2"
else
    echo "disk policy override cleared (default cached + fsync)"
fi
