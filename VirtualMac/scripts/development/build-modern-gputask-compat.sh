#!/bin/bash
# Build the transport for the matching macOS 27 GPU task. Dependencies must be
# the same verified runtime and Metal shims being deployed to VirtualMac2.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUNTIME="${1:?Usage: build-modern-gputask-compat.sh runtime-dylib metal-dylib output-directory}"
METAL="${2:?Missing Metal compatibility library}"
OUTPUT="${3:?Missing output directory}"
test -f "$RUNTIME"
test -f "$METAL"
mkdir -p "$OUTPUT"
LIBRARY="$OUTPUT/ModernGPUTaskCompat.dylib"
xcrun --sdk iphoneos clang -arch arm64e \
    -dynamiclib -fno-objc-arc -fblocks -Wl,-undefined,dynamic_lookup -Wl,-no_adhoc_codesign \
    -miphoneos-version-min=16.1 \
    -DVZ_MODERN_TASK_SERVER_COMPAT -framework Foundation -framework Metal \
    -Wl,-reexport_library,"$RUNTIME" -Wl,-reexport_library,"$METAL" \
    -Wl,-rpath,/var/root/VirtualMac2/payload/Frameworks \
    -install_name @rpath/ModernGPUTaskCompat.dylib \
    "$REPO_ROOT/vz/host/modern_pvg_task.m" \
    "$REPO_ROOT/vz/host/modern_pvg_event.m" \
    "$REPO_ROOT/vz/host/modern_pvg_heap.m" \
    "$REPO_ROOT/vz/host/modern_pvg_linear_texture.m" \
    "$REPO_ROOT/vz/host/modern_pvg_shader_cache.m" \
    "$REPO_ROOT/vz/host/modern_pvg_shader_worker.m" \
    "$REPO_ROOT/vz/host/modern_pvg_shader_audit.m" \
    "$REPO_ROOT/vz/host/modern_pvg_fault_observation.m" \
    "$REPO_ROOT/vz/host/modern_pvg_texture_error_observation.m" \
    "$REPO_ROOT/vz/host/modern_pvg_texture_preflight.m" \
    "$REPO_ROOT/vz/host/modern_pvg_hang_diagnostics.m" \
    "$REPO_ROOT/vz/host/modern_pvg_address_guard.m" -o "$LIBRARY"
# Project compatibility entries must be defined here; dynamic lookup cannot supply them.
nm -gU "$LIBRARY" | awk '
    BEGIN {
        required["_VZModernInstallTaskTransport"]=1
        required["_VZModernInstallFaultObservation"]=1
        required["_VZModernInstallTextureErrorObservation"]=1
        required["_VZModernInstallTexturePreflightCallsiteBridge"]=1
        required["_VZModernInstallScheduledEventCompatibility"]=1
        required["_VZModernInstallHeapCompatibility"]=1
        required["_VZModernInstallLinearTextureCompatibility"]=1
        required["_VZModernInstallShaderCompatibilityCache"]=1
        required["_VZModernInstallShaderAudit"]=1
        required["_VZModernConvertUnknownShader"]=1
        required["_VZModernRecordSuccessfulNativeLibrary"]=1
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
codesign -dv --verbose=4 "$LIBRARY"
