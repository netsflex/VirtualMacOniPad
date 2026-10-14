#!/bin/bash
# Build vz-air-normalize, the fallback shader converter that the GPU task runs
# when Apple's helper rejects an input. Deploy it as root:wheel 0755 next to
# metal-library-convert in /var/root/VirtualMac2/compiler27 and add its CDHash
# to the jailbreak trust cache.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTPUT="${1:?Usage: build-air-normalize.sh output-directory}"
mkdir -p "$OUTPUT"
BINARY="$OUTPUT/vz-air-normalize"
ENTITLEMENTS="$OUTPUT/vz-air-normalize.entitlements"
# The same entitlements as metal-library-convert: it loads the extracted
# macOS 27 compiler libraries, which are trusted only by CDHash.
cat >"$ENTITLEMENTS" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "https://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>com.apple.private.amfi.can-execute-cdhash</key><true/>
<key>com.apple.private.security.no-sandbox</key><true/>
<key>get-task-allow</key><true/>
<key>platform-application</key><true/>
</dict></plist>
PLIST
xcrun --sdk iphoneos clang++ -std=c++17 -O2 -Wall -Wextra -arch arm64e \
    -miphoneos-version-min=16.1 "$REPO_ROOT/vz/host/air_normalize.cpp" -o "$BINARY"
codesign --force --sign - --timestamp=none --entitlements "$ENTITLEMENTS" "$BINARY"
codesign --verify --strict "$BINARY"
codesign -dv --verbose=4 "$BINARY" 2>&1 | grep -E '^(Identifier|CDHash)='
