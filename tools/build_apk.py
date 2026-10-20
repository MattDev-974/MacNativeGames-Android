#!/usr/bin/env python3
"""build_apk.py - assemble the Android port with the NDK and the SDK build-tools.

The macOS project drives one Xcode build from build_app.py; this does the same
job with cmake + aapt2 + apksigner, and needs nothing else installed:

    python3 tools/build_apk.py                # debug-sign, arm64-v8a
    python3 tools/build_apk.py --release -o out/TOTK.apk

Environment (auto-detected when unset):
    ANDROID_NDK       Android NDK r26+ root
    ANDROID_HOME      SDK root (build-tools + platforms)
    JAVA_HOME         JDK (apksigner only)
"""
from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
CPP = ROOT / "app" / "cpp"
BUILD = ROOT / "build"
OUT = ROOT / "out"


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def find_ndk(explicit: str | None) -> pathlib.Path:
    for cand in (explicit, os.environ.get("ANDROID_NDK"),
                 os.environ.get("ANDROID_NDK_HOME"),
                 os.environ.get("ANDROID_NDK_ROOT")):
        if cand and (pathlib.Path(cand) / "build/cmake/android.toolchain.cmake").exists():
            return pathlib.Path(cand)
    for base in (ROOT / "tools", pathlib.Path.home() / "Android" / "Sdk", pathlib.Path("/opt")):
        if not base.exists():
            continue
        for d in sorted(base.glob("android-ndk-*"), reverse=True):
            if (d / "build/cmake/android.toolchain.cmake").exists():
                return d
    sys.exit("Android NDK not found: set ANDROID_NDK")


def find_sdk(explicit: str | None) -> pathlib.Path:
    for cand in (explicit, os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT")):
        if cand and (pathlib.Path(cand) / "platforms").exists():
            return pathlib.Path(cand)
    for base in (ROOT / "tools", pathlib.Path.home() / "Android" / "Sdk"):
        for cand in (base / "android-sdk", base / "Sdk"):
            if (cand / "platforms").exists():
                return cand
    sys.exit("Android SDK not found: set ANDROID_HOME")


def newest(path: pathlib.Path) -> pathlib.Path:
    """Newest versioned subdirectory, or `path` itself if it is already one
    (a hand-unpacked build-tools/platform tree has no version level)."""
    if (path / "aapt2").exists() or (path / "android.jar").exists():
        return path
    items = sorted((p for p in path.iterdir() if p.is_dir()),
                   key=lambda p: p.name, reverse=True)
    if not items:
        sys.exit(f"nothing under {path}")
    return items[0]


def shader_header(glslang: str | None) -> None:
    """Compile GLSL to SPIR-V and refresh the embedded header."""
    vert, frag = CPP / "shaders/tri.vert", CPP / "shaders/tri.frag"
    spv_v, spv_f = vert.with_suffix(".vert.spv"), frag.with_suffix(".frag.spv")
    if glslang:
        run([glslang, "-V", "--target-env", "vulkan1.1", "-o", spv_v, vert])
        run([glslang, "-V", "--target-env", "vulkan1.1", "-o", spv_f, frag])
    elif not spv_v.exists() or not spv_f.exists():
        sys.exit("need glslangValidator on PATH (or prebuilt .spv files)")
    run([sys.executable, ROOT / "tools/embed_spirv.py",
         CPP / "shaders_spv.h", f"tri_vert_spv={spv_v}", f"tri_frag_spv={spv_f}"])


def build_native(ndk: pathlib.Path, abi: str, api: int, build_type: str,
                 game_tables: pathlib.Path | None = None) -> pathlib.Path:
    gen = BUILD / abi
    run([shutil.which("cmake") or "cmake",
         "-S", CPP, "-B", gen, "-G", "Ninja",
         f"-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake",
         f"-DANDROID_ABI={abi}",
         f"-DANDROID_PLATFORM=android-{api}",
         f"-DCMAKE_BUILD_TYPE={build_type}",
         f"-DANDROID_NDK={ndk}"]
        + ([f"-DMNG_GAME_TABLES={game_tables}"] if game_tables else []))
    run([shutil.which("cmake") or "cmake", "--build", gen, "-j", str(os.cpu_count() or 4)])
    so = gen / "libtotk.so"
    if not so.exists():
        sys.exit(f"expected {so}")
    return so


def game_modules(tree: pathlib.Path | None) -> list[tuple[pathlib.Path, str]]:
    """The rewritten game modules, packaged under lib/<abi>/ so Android extracts
    them into nativeLibraryDir -- a read-only, executable location.  Code loaded
    from the app's data directory is refused by SELinux (execmod)."""
    if not tree:
        return []
    import json
    manifest = tree / "android-manifest.json"
    if not manifest.is_file():
        sys.exit(f"{manifest} missing: run tools/import_totk.py first")
    data = json.loads(manifest.read_text())
    base = pathlib.Path(data["tree"]) / "rewritten/v0"
    out = []
    for name in ("main", "sdk", "subsdk0"):
        src = base / f"{name}.patched.bin"
        if src.is_file():
            out.append((src, f"libgame_{name}.so"))
    for tsv in sorted(base.glob("*.tsv")):
        out.append((tsv, f"assets/nvn/{tsv.name}"))
    return out


def build_apk(sdk: pathlib.Path, so: pathlib.Path, abi: str, api: int,
              release: bool, output: pathlib.Path,
              extra: list[tuple[pathlib.Path, str]] | None = None) -> None:
    bt = newest(sdk / "build-tools")
    aapt2 = bt / "aapt2"
    zipalign = bt / "zipalign"
    apksigner = bt / "apksigner"
    android_jar = newest(sdk / "platforms") / "android.jar"

    BUILD.mkdir(exist_ok=True)
    res_zip = BUILD / "res.zip"
    unsigned = BUILD / "base-unsigned.apk"
    aligned = BUILD / "base-aligned.apk"

    run([aapt2, "compile", "--dir", ROOT / "app/res", "-o", res_zip])
    run([aapt2, "link",
         "-o", unsigned,
         "-I", android_jar,
         "--manifest", ROOT / "app/AndroidManifest.xml",
         "--min-sdk-version", str(api),
         "--target-sdk-version", "34",
         "--version-code", "1",
         "--version-name", "0.1-scaffold",
         res_zip])

    # Put the .so in the APK, stored uncompressed so the loader can mmap it and
    # so Android 15's 16 KB pages are honoured without a second copy.
    with zipfile.ZipFile(unsigned, "a", zipfile.ZIP_DEFLATED) as z:
        z.write(so, f"lib/{abi}/libtotk.so", compress_type=zipfile.ZIP_STORED)
        for src, arcname in (extra or []):
            if arcname.endswith(".so"):
                z.write(src, f"lib/{abi}/{arcname}", compress_type=zipfile.ZIP_STORED)
            else:
                z.write(src, arcname)

    run([zipalign, "-p", "-f", "4", unsigned, aligned])

    keystore = ROOT / "debug.keystore"
    if not keystore.exists():
        keytool = pathlib.Path(os.environ.get("JAVA_HOME", "")) / "bin/keytool"
        keytool = keytool if keytool.exists() else pathlib.Path(shutil.which("keytool") or "keytool")
        run([keytool, "-genkeypair", "-keystore", keystore,
             "-alias", "androiddebugkey", "-storepass", "android",
             "-keypass", "android", "-keyalg", "RSA", "-keysize", "2048",
             "-validity", "10000", "-dname", "CN=Android Debug,O=Android,C=US"])

    output.parent.mkdir(parents=True, exist_ok=True)
    run([apksigner, "sign",
         "--ks", keystore, "--ks-pass", "pass:android",
         "--key-pass", "pass:android",
         "--min-sdk-version", str(api),
         "--out", output, aligned])

    print(f"\nbuilt {output} ({output.stat().st_size/1e6:.1f} MB)")
    print(f"install: adb install -r {output}")
    print("runtime log: adb logcat -s mnge-vk mnge-nvn mnge-main")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--abi", default="arm64-v8a",
                    help="arm64-v8a (default) or x86_64 for an emulator")
    ap.add_argument("--api", type=int, default=26, help="minSdk (default 26)")
    ap.add_argument("--release", action="store_true", help="optimised build")
    ap.add_argument("--ndk")
    ap.add_argument("--sdk")
    ap.add_argument("-o", "--output", default=None)
    ap.add_argument("--game-tree", type=pathlib.Path,
                    help="work folder from tools/import_totk.py; packages the game modules")
    args = ap.parse_args()

    ndk = find_ndk(args.ndk)
    sdk = find_sdk(args.sdk)
    print(f"NDK {ndk}\nSDK {sdk}")

    shader_header(shutil.which("glslangValidator"))
    tables = None
    if args.game_tree:
        tree = args.game_tree.expanduser().resolve()
        gen = tree / "totk/build/runtime_tables.h"
        if not gen.is_file():
            sys.exit(f"{gen} missing: run scripts/prepare_runtime.py in {tree / 'totk'}")
        tables = gen.parent
    so = build_native(ndk, args.abi, args.api, "Release" if args.release else "Debug", tables)
    out = pathlib.Path(args.output) if args.output else OUT / "TOTK-debug.apk"
    extra = game_modules(args.game_tree.expanduser().resolve() if args.game_tree else None)
    if extra:
        print(f"packaging {len(extra)} game file(s) from {args.game_tree}")
    build_apk(sdk, so, args.abi, args.api, args.release, out, extra)


if __name__ == "__main__":
    main()
