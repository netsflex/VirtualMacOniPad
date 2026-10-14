#!/bin/bash
# Build only the VirtualMac2 VMM-side compatibility library. The compile and
# link line is the one build-virtualmac2.sh uses, so a hook change can be
# rebuilt and deployed without rebuilding the runtime, GPU task or app.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTPUT="${1:?Usage: build-modern-vmm-compat.sh output-directory}"
mkdir -p "$OUTPUT"
LIBRARY="$OUTPUT/LaunchServicesCompat.dylib"
# Match the validated v14 VMM-side library (MRC, iOS14.5, no AVFAudio link).
xcrun --sdk iphoneos clang -arch arm64e \
    -dynamiclib -fno-objc-arc -fblocks -Wl,-undefined,dynamic_lookup -Wl,-no_adhoc_codesign \
    -miphoneos-version-min=14.5 -framework CoreFoundation -framework CoreServices \
    -framework Foundation -framework IOKit -framework Metal \
    -Wl,-reexport_framework,CoreServices \
    -install_name @rpath/LaunchServicesCompat.dylib \
    "$REPO_ROOT/vz/host/lsshim.m" "$REPO_ROOT/vz/host/vmmhook.m" \
    "$REPO_ROOT/vz/host/pvg_trace.m" "$REPO_ROOT/vz/host/modern_pvg_memory.m" \
    "$REPO_ROOT/vz/host/modern_pvg_task.m" \
    "$REPO_ROOT/vz/host/modern_pvg_fault_observation.m" \
    "$REPO_ROOT/vz/host/modern_pvg_guest_linear_alignment.m" \
    "$REPO_ROOT/vz/host/modern_pvg_guest_texture_buffer_alignment.m" \
    -o "$LIBRARY"
# The task transport and alignment scope must be real local definitions.
nm -gU "$LIBRARY" | awk '
    BEGIN {
        required["_VZModernInstallTaskTransport"]=1
        required["_VZModernInstallFaultObservation"]=1
        required["_VZModernInstallGuestLinearAlignmentAdvertisement"]=1
        required["_VZModernInstallGuestTextureBufferAlignmentAdvertisement"]=1
    }
    { if ($NF in required) delete required[$NF] }
    END {
        for (symbol in required) {
            print "Missing required local definition: " symbol > "/dev/stderr"
            missing=1
        }
        exit missing
    }'
codesign --force --sign - --timestamp=none "$LIBRARY"
codesign --verify --strict "$LIBRARY"
dyld_info -validate_only "$LIBRARY"
codesign -dv --verbose=4 "$LIBRARY" 2>&1 | grep -E "^(Identifier|CDHash)="
