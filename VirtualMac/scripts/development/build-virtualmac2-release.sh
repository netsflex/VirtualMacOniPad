#!/bin/bash
# Build the standalone Virtual Mac 2 package from this repository and the two
# published upstream packages:
#   VirtualMac_1.2.3.deb            (networking daemons and their libraries)
#   VirtualMac_2.0.0beta1_1001.deb  (Apple frameworks, GPU task, compiler,
#                                    shader cache and app resources)
# Every file this fork changes or adds is compiled here from source; all other
# files are taken unchanged from the upstream packages. Requires Xcode and ldid.
set -euo pipefail
if [[ $# -lt 3 ]]; then
    echo "Usage: $0 <VirtualMac_1.2.3.deb> <VirtualMac_2.0.0beta1_1001.deb> <output-directory> [--build-number N]" >&2
    exit 2
fi
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RELEASE_123="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
RELEASE_V2="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
mkdir -p "$3"
OUTPUT="$(cd "$3" && pwd)"
shift 3
package_options=()
[[ $# -eq 0 ]] || package_options=("$@")

RELEASE_123_SHA256=435ce1dc76b9e18b1547c77b84e2cf33ffe40a16be63e366f181d14709a41aa0
RELEASE_V2_SHA256=853d6a92ead26f0e8cf001d0302f9211db5a676d5260ff4a45e18309e8800ed0
[[ "$(shasum -a 256 "$RELEASE_123" | cut -d ' ' -f1)" == "$RELEASE_123_SHA256" ]] || {
    echo "Not the published VirtualMac_1.2.3.deb" >&2; exit 1; }
[[ "$(shasum -a 256 "$RELEASE_V2" | cut -d ' ' -f1)" == "$RELEASE_V2_SHA256" ]] || {
    echo "Not the published VirtualMac_2.0.0beta1_1001.deb" >&2; exit 1; }

WORK="$(mktemp -d /private/tmp/virtualmac2-release.XXXXXX)"
trap 'rm -rf -- "$WORK"' EXIT
unpack() {
    mkdir -p "$2"
    python3 -I - "$1" "$WORK/members" <<'PY'
import os, sys
data = open(sys.argv[1], "rb").read()
if data[:8] != b"!<arch>\n":
    raise SystemExit("not a Debian archive")
os.makedirs(sys.argv[2], exist_ok=True)
offset = 8
while offset < len(data):
    header = data[offset:offset + 60]
    name, size = header[:16].decode().strip().rstrip("/"), int(header[48:58])
    offset += 60
    if name.startswith("data.tar"):
        open(os.path.join(sys.argv[2], "data.tar"), "wb").write(data[offset:offset + size])
    offset += size + (size & 1)
PY
    tar -xf "$WORK/members/data.tar" -C "$2"
    rm -rf "$WORK/members"
}
unpack "$RELEASE_123" "$WORK/release-123"
unpack "$RELEASE_V2" "$WORK/v2"
V2_RUNTIME="$WORK/v2/var/root/VirtualMac2"
V2_APP="$WORK/v2/var/jb/Applications/VirtualMac2.app"

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
HOST="$REPO_ROOT/vz/host"
INSTALL="$REPO_ROOT/vz/install"
HOST_ENTS="$HOST/VirtualMac.entitlements"
BIN="$WORK/binaries"
mkdir -p "$BIN"

# Application and diagnostics, as scripts/build-ipad-app.sh compiles them.
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.1 -isysroot "$SDK" -fblocks \
    -framework AVFAudio -framework CoreImage -framework Foundation \
    -framework GameController -framework Metal -framework Security -framework UIKit \
    -framework UniformTypeIdentifiers -framework QuartzCore -framework IOKit \
    -Wl,-export_dynamic -Wl,-undefined,dynamic_lookup \
    "$HOST/NSViewShim.m" "$HOST/VZAppSettings.m" "$HOST/VZDiagnostics.m" \
    "$HOST/VZFailureDetailsViewController.m" "$HOST/VZRestoreCatalog.m" "$HOST/VZSupport.m" \
    "$HOST/VZGuestTools.m" "$HOST/VZGuestRuntimePolicy.m" "$HOST/VZNewVMViewController.m" \
    "$HOST/VZProgressViewController.m" "$HOST/VZSettingsViewController.m" \
    "$HOST/VZTrackpadScrollBridge.m" "$HOST/VZVMLibraryViewController.m" \
    "$HOST/VZUSBTrackpadRelay.m" "$HOST/VirtualMacApp.m" \
    -o "$BIN/VirtualMac"
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.1 -isysroot "$SDK" -fblocks \
    -framework Foundation -framework UIKit \
    "$HOST/VZAppSettings.m" "$HOST/VZDiagnostics.m" "$HOST/virtualmac_diagnostics_main.m" \
    -o "$BIN/virtualmac-diagnostics"
ldid -S "$BIN/virtualmac-diagnostics"

# Installation helpers, as scripts/build-ipad-installation.sh compiles them,
# but locating their scripts below the Virtual Mac 2 runtime root.
xcrun --sdk iphoneos clang -arch arm64e -miphoneos-version-min=16.1 -isysroot "$SDK" \
    -dynamiclib -fblocks -Wl,-undefined,dynamic_lookup \
    -install_name "@rpath/VZHostCompat.dylib" "$HOST/vzxpchook.m" -o "$BIN/VZHostCompat.dylib"
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.1 -isysroot "$SDK" -fblocks \
    -framework Foundation -framework Metal -framework UIKit -Wl,-export_dynamic \
    -DVZ_RUNTIME_ROOT='"/var/root/VirtualMac2"' \
    "$HOST/NSViewShim.m" "$INSTALL/install_macos.m" -o "$BIN/install-macos"
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.1 -isysroot "$SDK" \
    -DVZ_RUNTIME_ROOT='"/var/root/VirtualMac2"' "$INSTALL/install_launcher.c" -o "$BIN/install-launcher"
for binary in VZHostCompat.dylib install-macos install-launcher; do
    ldid -S"$HOST_ENTS" "$BIN/$binary"
done

# SpringBoard keyboard passthrough, as scripts/build-springboard-tweak.sh builds it.
xcrun --sdk iphoneos clang -arch arm64e -miphoneos-version-min=14.5 -isysroot "$SDK" \
    -dynamiclib -fblocks -framework Foundation -Wl,-undefined,dynamic_lookup \
    -install_name /var/jb/Library/MobileSubstrate/DynamicLibraries/VZKeyboardPassthrough.dylib \
    "$REPO_ROOT/vz/tweak/VZKeyboardPassthrough.m" -o "$BIN/VZKeyboardPassthrough.dylib"
ldid -S "$BIN/VZKeyboardPassthrough.dylib"
cp "$REPO_ROOT/vz/tweak/VZKeyboardPassthrough.plist" "$BIN/"

# Runtime libraries this fork changes, built as build-virtualmac2.sh does.
FRAMEWORKS="$V2_RUNTIME/payload/Frameworks"
xcrun --sdk iphoneos clang -arch arm64e \
    -dynamiclib -fno-objc-arc -fblocks -Wl,-undefined,dynamic_lookup -Wl,-no_adhoc_codesign \
    -miphoneos-version-min=14.5 -framework CoreFoundation -framework CoreServices \
    -framework Foundation -framework IOKit -framework Metal \
    -Wl,-reexport_framework,CoreServices -install_name @rpath/LaunchServicesCompat.dylib \
    "$HOST/lsshim.m" "$HOST/vmmhook.m" "$HOST/pvg_trace.m" "$HOST/modern_pvg_memory.m" \
    "$HOST/modern_pvg_task.m" "$HOST/modern_pvg_fault_observation.m" \
    "$HOST/modern_pvg_guest_linear_alignment.m" \
    "$HOST/modern_pvg_guest_texture_buffer_alignment.m" \
    -o "$FRAMEWORKS/LaunchServicesCompat.dylib"
codesign --force --sign - --timestamp=none "$FRAMEWORKS/LaunchServicesCompat.dylib"
bash "$SCRIPT_DIR/build-modern-gputask-compat.sh" "$FRAMEWORKS/ModernRuntimeCompat.dylib" \
    "$FRAMEWORKS/MetalCompat.dylib" "$WORK/gputask" >/dev/null
cp "$WORK/gputask/ModernGPUTaskCompat.dylib" "$FRAMEWORKS/ModernGPUTaskCompat.dylib"

# The VMM gains the entitlements that expose the iPad's hardware video decoders.
VMM="$V2_RUNTIME/payload/VirtualMachine.xpc/Contents/MacOS/com.apple.Virtualization.VirtualMachine"
ldid -S"$REPO_ROOT/vz/patches/vmm.ents.xml" "$VMM"

# Shader converter helpers: the patched Apple helper and the AIR normalizer.
COMPILER="$V2_RUNTIME/compiler27"
mkdir -p "$COMPILER"
ORIGINAL_CONVERTER="$V2_RUNTIME/package-source/compiler27/metal-library-convert"
python3 "$SCRIPT_DIR/patch-metal-library-convert.py" "$ORIGINAL_CONVERTER" "$COMPILER/metal-library-convert" >/dev/null
codesign -d --entitlements :- "$ORIGINAL_CONVERTER" >"$WORK/converter.entitlements" 2>/dev/null
IDENTIFIER="$(codesign -dv "$ORIGINAL_CONVERTER" 2>&1 | sed -n 's/^Identifier=//p')"
codesign --force --sign - --timestamp=none --identifier "$IDENTIFIER" \
    --entitlements "$WORK/converter.entitlements" "$COMPILER/metal-library-convert"
bash "$SCRIPT_DIR/build-air-normalize.sh" "$WORK/normalizer" >/dev/null
cp "$WORK/normalizer/vz-air-normalize" "$COMPILER/vz-air-normalize"

python3 "$SCRIPT_DIR/build-virtualmac2-standalone-deb.py" --v2-tree "$WORK/v2" \
    --release-123 "$WORK/release-123" --binaries "$BIN" --output "$OUTPUT" \
    ${package_options[@]+"${package_options[@]}"}
