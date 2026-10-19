#!/usr/bin/env python3
"""Package Virtual Mac 2 so that it no longer needs release 1.2.3 installed.

The package takes over everything 1.2.3 supplied to Virtual Mac 2: the
installation helpers, the keyboard tweak and the rootless networking daemons.
It conflicts with and replaces com.mac.virtual, so dpkg removes 1.2.3 when
this package is installed. Only rootless iPadOS 16 is supported, so only the
iPadOS 16 variants of the shared components are included.

Inputs are local snapshots:
  --v2-tree       a copy of the installed Virtual Mac 2 files, rooted at /
  --release-123   the unpacked data tree of the 1.2.3 package, rooted at /
  --binaries      freshly built VirtualMac (unsigned), virtualmac-diagnostics,
                  install-launcher, install-macos and VZHostCompat.dylib
                  (signed), with install-launcher and install-macos built with
                  -DVZ_RUNTIME_ROOT='"/var/root/VirtualMac2"', and optionally a
                  rebuilt VZKeyboardPassthrough.dylib and .plist
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
RUNTIME = "/var/root/VirtualMac2"
APP_PATH = "/var/jb/Applications/VirtualMac2.app"
MACHO_MAGICS = {b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe",
                b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}
COMPILER_FILES = ("VM27CompilerCompat.dylib", "VM27GPUCompiler.dylib",
                  "VM27GPUCompilerImpl.dylib", "VM27LLVM.dylib",
                  "VM27llvm-flatbuffers.dylib", "VM27llvm-lmdb.dylib",
                  "metal-library-convert", "vz-air-normalize",
                  "compiler-profile.json")
# Shared files taken unchanged from 1.2.3, at the same installed paths. The
# networking daemons resolve their libraries through @loader_path/../lib.
SHARED_FILES = (
    "var/jb/Library/LaunchDaemons/com.apple.NetworkSharing.plist",
    "var/jb/Library/LaunchDaemons/com.apple.bootpd.plist",
    "var/jb/basebin/LaunchDaemons/com.apple.NetworkSharing.plist",
    "var/jb/usr/lib/AuthorizationCompat.dylib",
    "var/jb/usr/lib/NetworkMemoryPolicy.dylib",
    "var/jb/usr/lib/OpenDirectoryCompat.dylib",
    "var/jb/usr/lib/libmrc.dylib",
    "var/jb/usr/libexec/bootpd",
    "var/jb/usr/sbin/rtadvd",
    "var/root/VirtualMac/bootstrap-common/usr/lib/TweakInject/VZKeyboardPassthrough.dylib",
    "var/root/VirtualMac/bootstrap-common/usr/lib/TweakInject/VZKeyboardPassthrough.plist",
)
# Directories this package may have to create once 1.2.3 is removed. Their
# parents belong to the jailbreak bootstrap and are not package data.
OWNED_SYSTEM_DIRECTORIES = ("var/jb/Library/LaunchDaemons", "var/jb/basebin/LaunchDaemons")


def run(*arguments, **kwargs):
    return subprocess.run(arguments, check=True, **kwargs)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def is_macho(path):
    with path.open("rb") as handle:
        return handle.read(4) in MACHO_MAGICS


def cdhash(path):
    details = run("codesign", "-d", "--verbose=4", str(path), capture_output=True, text=True)
    match = re.search(r"^CDHash=([0-9a-f]{40})$", details.stdout + details.stderr, re.M)
    if not match:
        raise ValueError(f"Missing code-directory hash: {path}")
    return match.group(1)


def deb_writer():
    spec = importlib.util.spec_from_file_location("virtualmac2_deb", HERE / "build-virtualmac2-deb.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def copy_file(source, destination, mode=None):
    if source.is_symlink() or not source.is_file():
        raise ValueError(f"Expected a regular input file: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(mode if mode is not None else (0o755 if is_macho(destination) else 0o644))


def copy_tree(source, destination):
    if source.is_symlink() or not source.is_dir():
        raise ValueError(f"Expected a regular input directory: {source}")
    shutil.copytree(source, destination, symlinks=True, copy_function=shutil.copyfile)
    for path in sorted(destination.rglob("*"), reverse=True):
        if path.name == ".DS_Store" or path.name.startswith("._"):
            shutil.rmtree(path) if path.is_dir() and not path.is_symlink() else path.unlink()
    for path in [destination, *destination.rglob("*")]:
        if path.is_symlink():
            target = os.readlink(path)
            if target.startswith("/") or not path.resolve().is_relative_to(destination.resolve()):
                raise ValueError(f"Link leaves its copied subtree: {path} -> {target}")
        elif path.is_dir():
            path.chmod(0o755)
        elif path.is_file():
            path.chmod(0o755 if is_macho(path) else 0o644)
        else:
            raise ValueError(f"Unsupported package input node: {path}")


def stage_app(source, binary, destination, version, build):
    copy_tree(source, destination)
    shutil.rmtree(destination / "_CodeSignature")
    copy_file(binary, destination / "VirtualMac", 0o755)
    info_path = destination / "Info.plist"
    info = plistlib.loads(info_path.read_bytes())
    if info.get("CFBundleIdentifier") != "com.mac.virtual.v2" or info.get("VirtualMacRuntimeRoot") != RUNTIME:
        raise ValueError("Input app is not the Virtual Mac 2 bundle")
    upstream = info["CFBundleShortVersionString"]
    info.update(CFBundleShortVersionString=version, CFBundleVersion=build,
                CFBundleDisplayName=info["CFBundleDisplayName"].replace(upstream, version))
    info_path.write_bytes(plistlib.dumps(info))
    # Localized names carry the version too, for example "虚拟Mac 2.0.0beta1".
    for strings in destination.glob("*.lproj/InfoPlist.strings"):
        raw = run("plutil", "-convert", "xml1", "-o", "-", str(strings), capture_output=True).stdout
        localized = plistlib.loads(raw)
        for key, value in localized.items():
            if isinstance(value, str):
                localized[key] = value.replace(upstream, version)
        strings.write_bytes(plistlib.dumps(localized, fmt=plistlib.FMT_BINARY))
    # Keep the installed app's entitlements, including platform-application, and
    # add the HID client rights the Magic Trackpad relay needs to monitor events.
    entitlements = destination.parent / "app-entitlements.plist"
    granted = plistlib.loads(run("codesign", "-d", "--entitlements", ":-", str(source),
                                 capture_output=True).stdout)
    fork = plistlib.loads((REPO / "vz/host/VirtualMac.entitlements").read_bytes())
    granted.update({key: value for key, value in fork.items()
                    if key.startswith("com.apple.private.hid.client.")})
    entitlements.write_bytes(plistlib.dumps(granted))
    run("codesign", "--force", "--sign", "-", "--timestamp=none", "--identifier",
        "com.mac.virtual.v2", "--entitlements", str(entitlements), str(destination))
    run("codesign", "--verify", "--deep", "--strict", str(destination))
    signed = run("codesign", "-d", "--entitlements", ":-", str(destination), capture_output=True).stdout
    if plistlib.loads(signed) != plistlib.loads(entitlements.read_bytes()):
        raise ValueError("Signing changed the app entitlements")
    entitlements.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--v2-tree", type=Path, required=True)
    parser.add_argument("--release-123", type=Path, required=True)
    parser.add_argument("--binaries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--release-version", default="2.2.0beta1")
    parser.add_argument("--build-number", default="1004")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9][A-Za-z0-9.+-]*", args.release_version) or not re.fullmatch(r"[0-9]+", args.build_number):
        parser.error("Expected a safe release version and numeric build number")
    args.output.mkdir(parents=True, exist_ok=True)
    deb = args.output.resolve() / f"VirtualMac2_{args.release_version}_{args.build_number}_standalone.deb"
    if deb.exists():
        raise ValueError("Refusing to overwrite a release package")
    writer = deb_writer()
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "1791417600"))
    v2_runtime = args.v2_tree / RUNTIME[1:]
    with tempfile.TemporaryDirectory(prefix="virtualmac2-standalone-", dir="/private/tmp") as temporary:
        temporary = Path(temporary)
        stage = temporary / "stage"
        runtime, app = stage / RUNTIME[1:], stage / APP_PATH[1:]
        runtime.mkdir(parents=True)
        app.parent.mkdir(parents=True)
        stage_app(args.v2_tree / APP_PATH[1:], args.binaries / "VirtualMac", app,
                  args.release_version, args.build_number)
        for name in ("payload", "testing"):
            copy_tree(v2_runtime / name, runtime / name)
        (runtime / "shader-cache").mkdir()
        copy_tree(v2_runtime / "shader-cache/air25", runtime / "shader-cache/air25")

        # The compiler libraries come from the reviewed 2.0.0beta1 package; the
        # converter helpers are the locally patched builds now in service.
        compiler = runtime / "package-source/compiler27"
        for name in COMPILER_FILES:
            origin = v2_runtime / "compiler27" / name
            if not origin.is_file():
                origin = v2_runtime / "package-source/compiler27" / name
            copy_file(origin, compiler / name)

        install = runtime / "install"
        for name in ("install-launcher", "install-macos", "VZHostCompat.dylib"):
            copy_file(args.binaries / name, install / name, 0o755)
        copy_file(REPO / "vz/install/start-install.sh", install / "start-install.sh", 0o755)
        if b"/var/root/VirtualMac2/install/start-install.sh" not in (install / "install-launcher").read_bytes():
            raise ValueError("install-launcher was not built for the Virtual Mac 2 runtime root")
        copy_file(args.binaries / "virtualmac-diagnostics", stage / "var/jb/usr/bin/virtualmac-diagnostics", 0o755)
        for relative in SHARED_FILES:
            destination = relative.replace("var/root/VirtualMac/", RUNTIME[1:] + "/")
            # A rebuilt keyboard tweak in --binaries replaces 1.2.3's copy.
            origin = args.binaries / Path(relative).name
            if "/TweakInject/" not in relative or not origin.is_file():
                origin = args.release_123 / relative
            copy_file(origin, stage / destination)
        # iPadOS 16 uses the .ipados16 network sharing daemon.
        copy_file(args.release_123 / "var/jb/usr/libexec/InternetSharing.ipados16",
                  stage / "var/jb/usr/libexec/InternetSharing", 0o755)
        for relative in OWNED_SYSTEM_DIRECTORIES:
            (stage / relative).mkdir(parents=True, exist_ok=True)

        files = sorted(path for path in stage.rglob("*") if path.is_file() and not path.is_symlink())
        records, trust = [], []
        for path in files:
            installed = "/" + str(path.relative_to(stage))
            if compiler in path.parents:
                installed = RUNTIME + "/compiler27/" + path.name
            row = dict(path=installed, sha256=sha(path), bytes=path.stat().st_size)
            if is_macho(path):
                row["cdhash"] = cdhash(path)
                trust.append(row["cdhash"] + "\t" + installed)
            records.append(row)
        release = runtime / "release"
        release.mkdir()
        manifest = dict(formatVersion=1, package="com.mac.virtual.v2", version=args.release_version,
                        build=args.build_number, standalone=True,
                        replaces="com.mac.virtual (all versions)", files=records)
        (release / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        (release / "trustcache.txt").write_text("\n".join(sorted(set(trust))) + "\n")
        (release / "files.sha256").write_text("".join(f"{row['sha256']}  {row['path']}\n" for row in records))
        (runtime / "package-source/compiler27.sha256").write_text(
            "".join(f"{sha(compiler / name)}  {name}\n" for name in COMPILER_FILES))

        maintainer = temporary / "DEBIAN"
        shutil.copytree(REPO / "packaging/virtualmac2-standalone/DEBIAN", maintainer)
        size = sum(path.stat().st_size for path in stage.rglob("*") if path.is_file() and not path.is_symlink())
        control = (maintainer / "control").read_text()
        control = control.replace("@VERSION@", args.build_number).replace("@RELEASE@", args.release_version)
        control = control.replace("@INSTALLED_SIZE@", str((size + 1023) // 1024))
        (maintainer / "control").write_text(control)
        (maintainer / "control").chmod(0o644)
        for name in ("preinst", "postinst", "prerm", "postrm"):
            (maintainer / name).chmod(0o755)
            run("/bin/sh", "-n", str(maintainer / name))

        # Only package-owned nodes go into the archive: never metadata for
        # /var, /var/root, /var/jb or the bootstrap's own directories.
        owned_roots = [runtime, app, *(stage / relative for relative in OWNED_SYSTEM_DIRECTORIES)]
        data_paths = set(owned_roots)
        for root in owned_roots:
            data_paths.update(root.rglob("*"))
        data_paths.update(path for path in files)
        data_paths = sorted(data_paths, key=lambda path: str(path.relative_to(stage)))
        control_tar = writer.tar_bytes(sorted(maintainer.iterdir()), maintainer, epoch)
        data_tar = writer.tar_bytes(data_paths, stage, epoch)
        writer.write_deb(deb, control_tar, data_tar, epoch)
        (args.output / "release-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (args.output / "SHA256SUMS").write_text(sha(deb) + "  " + deb.name + "\n")
    print(json.dumps(dict(package=str(deb), sha256=sha(deb), files=len(records),
                          signedCodeFiles=len(trust), build=args.build_number)))


if __name__ == "__main__":
    main()
