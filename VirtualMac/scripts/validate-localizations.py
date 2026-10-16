#!/usr/bin/env python3
"""Validate the checked-in, human-reviewed iPadOS 16 language resources."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
RESOURCE_ROOT = ROOT / "resources" / "Localizations"
EXPECTED = {
    "cs", "da", "de", "el", "en", "es", "es_419", "es_US", "fi", "fr",
    "fr_CA", "he", "hi", "hr", "hu", "id", "it", "ja", "ko", "ms",
    "nl", "no", "pl", "pt", "pt_PT", "ro", "ru", "sk", "sl", "sv",
    "th", "tr", "uk", "vi", "zh_CN", "zh_HK", "zh_TW",
}
SOURCE_PATTERN = re.compile(r'(?:VZL|VML)\(@"((?:[^"\\]|\\.)*)"\)')
STRINGS_PATTERN = re.compile(r'^"((?:[^"\\]|\\.)*)"\s*=\s*"((?:[^"\\]|\\.)*)";', re.M)
FORMAT_PATTERN = re.compile(
    r'%(?:%|(?:\d+\$)?[-+ #0\']*(?:\d+|\*)?(?:\.(?:\d+|\*))?'
    r'(?:hh|h|ll|l|j|z|t|L)?[diuoxXfFeEgGaAcsp@])'
)
RAW_UI_PATTERNS = (
    re.compile(r'(?:actionWithTitle|alertControllerWithTitle|initWithTitle):@"'),
    re.compile(r'menuWithTitle:@"(?=[^"])'),
    re.compile(r'\.(?:title|text|placeholder|accessibilityLabel)\s*=\s*@"'),
    re.compile(r'(?:message|headerText|footerText):@"'),
    re.compile(r'setTitle:@"'),
)
TECHNICAL_IDENTICAL_VALUES = {
    "%@ CPU · %@ GB RAM · %llu GB · %@%@",
    "4K Retina",
    "5K Retina",
    "6K Retina",
    "Build %@ · %@ · %@",
    "Ethernet (%@)",
    "Fullscreen Mirroring",
    "MAC Address",
    "Mac Keyboard",
    "Mac Trackpad",
    "Magic Trackpad",
    "NAT: Share %@",
    "Start %@",
    "USB Keyboard",
    "USB Mouse",
    "Virtual Mac",
    "VPN",
    "Wi-Fi",
}


def fail(message: str) -> None:
    raise SystemExit(f"localization audit failed: {message}")


source_keys = set()
sources = list((ROOT / "vz" / "host").glob("*.m"))
sources += list((ROOT / "vz" / "guest").glob("*.m"))
for source in sources:
    contents = source.read_text()
    source_keys.update(SOURCE_PATTERN.findall(contents))
    for pattern in RAW_UI_PATTERNS:
        match = pattern.search(contents)
        if match:
            line = contents.count("\n", 0, match.start()) + 1
            fail(f"raw user-facing string in {source.name}:{line}; wrap it in VZL()")

actual = {path.name.removesuffix(".lproj") for path in RESOURCE_ROOT.glob("*.lproj")}
if actual != EXPECTED:
    fail(f"language list mismatch; missing={sorted(EXPECTED-actual)}, extra={sorted(actual-EXPECTED)}")

for locale in sorted(EXPECTED):
    path = RESOURCE_ROOT / f"{locale}.lproj" / "Localizable.strings"
    ordered_entries = STRINGS_PATTERN.findall(path.read_text())
    entries = dict(ordered_entries)
    ordered_keys = [key for key, _ in ordered_entries]
    expected_order = sorted(source_keys, key=str.casefold)
    if ordered_keys != expected_order:
        fail(f"{locale} keys are not in one alphabetical sequence")
    missing = source_keys - entries.keys()
    if missing:
        fail(f"{locale} is missing keys: {sorted(missing)}")
    for key in source_keys:
        if sorted(FORMAT_PATTERN.findall(key)) != sorted(FORMAT_PATTERN.findall(entries[key])):
            fail(f"{locale} changed format placeholders for {key!r}")
        if (locale != "en" and entries[key] == key and " " in key and
                key not in TECHNICAL_IDENTICAL_VALUES):
            fail(f"{locale} retains an untranslated phrase: {key!r}")
    info_path = RESOURCE_ROOT / f"{locale}.lproj" / "InfoPlist.strings"
    if not info_path.is_file():
        fail(f"{locale} is missing InfoPlist.strings")
    info_entries = dict(STRINGS_PATTERN.findall(info_path.read_text()))
    if set(info_entries) != {"CFBundleDisplayName", "CFBundleName"}:
        fail(f"{locale} has invalid InfoPlist.strings keys")
    if not info_entries["CFBundleDisplayName"] or (
            info_entries["CFBundleDisplayName"] != info_entries["CFBundleName"]):
        fail(f"{locale} has inconsistent localized app names")

print(f"localization audit passed: {len(EXPECTED)} language variants, {len(source_keys)} UI strings")
