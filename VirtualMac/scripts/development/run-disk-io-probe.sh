#!/bin/bash
# Run the guest disk I/O probe once under the current host disk policy.
#
# 1. Quit Virtual Mac and pick a policy, changing one axis per comparison
#    (the default with no override is cached + fsync):
#      set-disk-policy-override.sh automatic full    # previous default
#      set-disk-policy-override.sh automatic fsync   # synchronization only
#      set-disk-policy-override.sh cached full       # caching only
# 2. Boot the guest, let it settle, then run this script.
# 3. Restore the default policy afterwards: set-disk-policy-override.sh clear
#
# VZ_IPAD_SSH is the iPad root SSH target and VZ_GUEST_SSH the guest's
# user@address as seen from the iPad (the guest is reached through the iPad
# with ssh -J). Both use key authentication.
#
# Compare flush-* and smallfiles between full and fsync runs, and the pass2
# read rows and write-rand between automatic and cached runs.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../lib/common.sh
source "$SCRIPT_DIR/../lib/common.sh"

need_command xcrun
: "${VZ_IPAD_SSH:?set VZ_IPAD_SSH to the iPad root SSH target}"
: "${VZ_GUEST_SSH:?set VZ_GUEST_SSH to the guest user@address}"
FILE_MIB="${DISK_PROBE_FILE_MIB:-1024}"
SECONDS_PER_TEST="${DISK_PROBE_SECONDS:-15}"
TESTS="${DISK_PROBE_TESTS:-all}"
GUEST_DIR="${DISK_PROBE_GUEST_DIR:-/tmp/disk-io-probe-data}"

OUT="$VZ_BUILD_ROOT/development/disk-io-probe"
RESULT_DIR="$VZ_BUILD_ROOT/diagnostics/disk-io-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$(dirname "$OUT")" "$RESULT_DIR"

xcrun --sdk macosx clang -O2 -Wall -arch arm64 -mmacosx-version-min=12.0 \
    "$SCRIPT_DIR/disk-io-probe.c" -o "$OUT"

GUEST_SSH_ARGS=(-J "$VZ_IPAD_SSH" -o ServerAliveInterval=5
    -o ServerAliveCountMax=3)

# Record which policy the host actually applied for this boot.
ssh "$VZ_IPAD_SSH" "grep -h 'disk policy' /tmp/VirtualMac.log | tail -1" \
    | tee "$RESULT_DIR/host-policy.txt" || true
if [[ ! -s "$RESULT_DIR/host-policy.txt" ]]; then
    echo "warning: no disk policy line in /tmp/VirtualMac.log;" \
        "the installed app may predate the override" |
        tee "$RESULT_DIR/host-policy.txt"
fi

scp -q "${GUEST_SSH_ARGS[@]}" "$OUT" "$VZ_GUEST_SSH:/tmp/disk-io-probe"
ssh "${GUEST_SSH_ARGS[@]}" "$VZ_GUEST_SSH" \
    "mkdir -p '$GUEST_DIR' && chmod 755 /tmp/disk-io-probe &&
     /tmp/disk-io-probe '$GUEST_DIR' '$FILE_MIB' '$SECONDS_PER_TEST' '$TESTS'" \
    | tee "$RESULT_DIR/guest-probe.log"

# Host memory pressure matters for cached runs; keep a snapshot next to them.
ssh "$VZ_IPAD_SSH" "memory_pressure 2>/dev/null | tail -3 ||
    vm_stat 2>/dev/null | head -12" >"$RESULT_DIR/host-memory.txt" || true

echo "disk I/O probe evidence: $RESULT_DIR"
