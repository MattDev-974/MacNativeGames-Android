#!/usr/bin/env python3
"""push_data.py - send the game's RomFS to the device.

The Mac build puts the 17 GB RomFS inside the app bundle.  An APK cannot carry
it (the format caps at 4 GB, the Play limit is far lower), so the data lives in
the app's external files directory and is pushed once, over adb:

    /sdcard/Android/data/org.macnativegames.totk/files/romfs/

That path needs no runtime permission: it belongs to the app, and the app reads
it with a plain open().  Push is resumable -- adb's sync protocol skips files
whose size and mtime already match, so re-running after an interruption picks
up where it stopped.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import sys

PKG = "org.macnativegames.totk"
DEST = f"/sdcard/Android/data/{PKG}/files"


def say(msg: str) -> None:
    print(f"[push] {msg}", flush=True)


def die(msg: str) -> None:
    print(f"[push] ERROR: {msg}", file=sys.stderr, flush=True)
    raise SystemExit(1)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", type=pathlib.Path, default=pathlib.Path.home() / "TOTK_android")
    ap.add_argument("--romfs", type=pathlib.Path, help="override the RomFS directory")
    ap.add_argument("--dry-run", action="store_true", help="check everything, copy nothing")
    args = ap.parse_args()

    work = args.work.expanduser().resolve()
    manifest_path = work / "android-manifest.json"
    if not manifest_path.is_file():
        die(f"{manifest_path} missing: run tools/import_totk.py first")
    manifest = json.loads(manifest_path.read_text())

    romfs = args.romfs or pathlib.Path(manifest.get("romfs") or "")
    if not romfs or not romfs.is_dir():
        die("no RomFS in this import (built with --skip-romfs?)")
    if not (romfs / "index.json").is_file():
        die(f"{romfs}/index.json missing: the RomFS was not extracted by extract_romfs.py")

    size = sum(p.stat().st_size for p in romfs.rglob("*") if p.is_file())
    say(f"RomFS: {romfs}  ({size / 1e9:.2f} GB)")

    if not shutil.which("adb"):
        die("adb not found")
    state = subprocess.run(["adb", "get-state"], capture_output=True, text=True)
    if state.returncode:
        die("no device: check 'adb devices' and the USB debugging prompt")

    free = subprocess.run(["adb", "shell", f"df -k {DEST} 2>/dev/null | tail -1"],
                          capture_output=True, text=True).stdout.split()
    if len(free) >= 4:
        try:
            avail = int(free[3]) * 1024
            say(f"free on device: {avail / 1e9:.1f} GB")
            if avail < size:
                die(f"not enough space: need {size / 1e9:.1f} GB, "
                    f"{avail / 1e9:.1f} GB free")
        except ValueError:
            pass

    if args.dry_run:
        say("dry run: nothing copied")
        return

    subprocess.run(["adb", "shell", "mkdir", "-p", f"{DEST}/romfs"], check=True)
    say("pushing (resumable; interrupt and rerun at any time)")
    rc = subprocess.run(["adb", "push", str(romfs) + "/.", f"{DEST}/romfs/"]).returncode
    if rc:
        die("adb push failed")

    say("verifying")
    on_device = subprocess.run(
        ["adb", "shell", f"du -sb {DEST}/romfs 2>/dev/null | cut -f1"],
        capture_output=True, text=True).stdout.strip()
    try:
        got = int(on_device)
        say(f"device reports {got / 1e9:.2f} GB (local {size / 1e9:.2f} GB)")
        if abs(got - size) > size * 0.01:
            say("WARNING: sizes differ by more than 1%; rerun the push")
    except ValueError:
        say("could not read the size back; check manually")

    say("done. the runtime reads " + f"{DEST}/romfs")


if __name__ == "__main__":
    main()
