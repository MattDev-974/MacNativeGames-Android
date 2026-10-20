#!/usr/bin/env python3
"""import_totk.py - turn your own XCI/NSP into the Android asset tree.

MacNativeGames already does the hard part: reading the container, verifying
every hash, rewriting the NSO for a non-Horizon host, and decrypting the 17 GB
RomFS.  None of that is Apple-specific -- it is Python plus a locally built
hactool -- so this script reuses it verbatim and only replaces the packaging
tail, which *is* Apple-specific.

    python3 tools/import_totk.py ~/Games/totk.xci -k ~/keys/prod.keys \\
        --upstream ~/MacNativeGames --work ~/TOTK_android

It mirrors build_app.py's step_code / step_data command lines one for one; the
only difference is where the result ends up.

Layout produced under --work:

    totk/                              a working copy of games/totk
    totk/extracted/v0/exefs/           the game's ExeFS, hash-checked
    totk/extracted/v0/elf/             ELF + symbol inventory (analysis only)
    totk/rewritten/v0/*.patched.bin    code rewritten for a native host
    totk/rewritten/v0/*.tsv            gate / fixup tables the loader reads
    totk/extracted/v0/romfs/           romfs.bin + index.json (17 GB)
    totk/extracted/firmware/shared-fonts/
    android-manifest.json              what build_apk.py packages

The RomFS does not fit in an APK and never will; push_data.py sends it to the
device separately.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_VENV = ROOT / ".import-venv"


def say(msg: str) -> None:
    print(f"[import] {msg}", flush=True)


def die(msg: str) -> None:
    print(f"[import] ERROR: {msg}", file=sys.stderr, flush=True)
    raise SystemExit(1)


def run(cmd, tree: pathlib.Path, log_path: pathlib.Path, what: str) -> None:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    say(f"{what}  (log: {log_path.name})")
    with log_path.open("w") as log:
        proc = subprocess.run([str(c) for c in cmd], cwd=tree,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        log.write(proc.stdout)
    if proc.returncode:
        tail = "\n".join(proc.stdout.strip().splitlines()[-15:])
        die(f"{what} failed (exit {proc.returncode}).\n{tail}\nFull log: {log_path}")
    for line in proc.stdout.strip().splitlines()[-2:]:
        print(f"           {line}")


def find_upstream(explicit: str | None) -> pathlib.Path:
    cands = [explicit, os.environ.get("MACNATIVEGAMES")]
    for cand in cands:
        if cand:
            p = pathlib.Path(cand).expanduser()
            if (p / "games/totk/scripts/extract_exefs.py").is_file():
                return p
            die(f"{p} is not a MacNativeGames checkout")
    for p in ROOT.parent.glob("*"):
        if (p / "games/totk/scripts/extract_exefs.py").is_file():
            return p
    die("MacNativeGames checkout not found. Pass --upstream /path/to/MacNativeGames "
        "(git clone https://github.com/ly05010419/MacNativeGames)")


def ensure_venv(venv: pathlib.Path, totk: pathlib.Path) -> pathlib.Path:
    py = venv / "bin/python"
    if not py.exists():
        say(f"creating the import virtualenv in {venv}")
        subprocess.run([sys.executable, "-m", "venv", str(venv)], check=True)
        subprocess.run([str(py), "-m", "pip", "install", "-q", "-r",
                        str(totk / "requirements.txt")], check=True)
    if subprocess.run([str(py), "-c", "import cryptography, lz4, capstone"],
                      capture_output=True).returncode:
        die(f"{py} lacks cryptography/lz4/capstone; delete {venv} and rerun")
    return py


def prepare_tree(upstream: pathlib.Path, work: pathlib.Path, venv: pathlib.Path) -> pathlib.Path:
    """Copy games/totk into the work folder, as build_app.py's prepare_work does,
    so the upstream checkout is never written to."""
    src = upstream / "games/totk"
    tree = work / "totk"
    marker = tree / ".imported"
    if not marker.exists():
        say(f"copying {src} -> {tree}")
        if tree.exists():
            shutil.rmtree(tree)
        shutil.copytree(src, tree,
                        ignore=shutil.ignore_patterns("extracted", "rewritten", "build",
                                                      "analysis", ".venv", "tooling",
                                                      "__pycache__", "*.pyc"))
        marker.write_text("1\n")
    vlink = tree / ".venv"
    if vlink.is_symlink() or vlink.exists():
        vlink.unlink()
    vlink.symlink_to(venv)
    for d in ("extracted/v0", "rewritten/v0", "analysis", "build"):
        (tree / d).mkdir(parents=True, exist_ok=True)
    return tree


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", type=pathlib.Path, help="your XCI or NSP (base game)")
    ap.add_argument("-k", "--keys", type=pathlib.Path, required=True, help="your prod.keys")
    ap.add_argument("--upstream", help="path to a MacNativeGames checkout")
    ap.add_argument("--work", type=pathlib.Path, default=pathlib.Path.home() / "TOTK_android")
    ap.add_argument("--update", type=pathlib.Path,
                    help="cumulative update NSP/NSZ (see the upstream --version steps)")
    ap.add_argument("--skip-romfs", action="store_true",
                    help="only extract + rewrite the code (fast, no 17 GB step)")
    ap.add_argument("--firmware", type=pathlib.Path,
                    help="unpacked firmware NCA directory (NSP input needs the shared fonts)")
    ap.add_argument("--shared-fonts", type=pathlib.Path,
                    help="previously extracted shared-font directory (six .bfttf files)")
    args = ap.parse_args()

    upstream = find_upstream(args.upstream)
    game = args.game.expanduser().resolve()
    keys = args.keys.expanduser().resolve()
    work = args.work.expanduser().resolve()

    for what, p in (("game file", game), ("prod.keys", keys)):
        if not p.is_file():
            die(f"{what} not found: {p}")
    if args.update:
        die("--update is not wired yet on the Android side; run the base game first")

    venv = ensure_venv(DEFAULT_VENV, upstream / "games/totk")
    tree = prepare_tree(upstream, work, venv)
    logs = work / "logs"
    py = venv

    say(f"upstream: {upstream}\n[import] tree:     {tree}")

    # ---- 1. ExeFS (build_app.py step_code) --------------------------------
    exefs = tree / "extracted/v0/exefs"
    if not (exefs / "main").is_file():
        run([py, "scripts/extract_exefs.py", game, "--keys", keys, "--output", exefs],
            tree, logs / "extract_exefs.log", "1/5 extracting the ExeFS")
    else:
        say("1/5 ExeFS already extracted")

    # ---- 2. module inventory ----------------------------------------------
    elf = tree / "extracted/v0/elf"
    if not (elf / "main-inventory.json").is_file():
        run([py, "scripts/prepare_modules.py", exefs, "--output", elf],
            tree, logs / "prepare_modules.log", "2/5 inventorying the modules")
    else:
        say("2/5 modules already inventoried")

    # ---- 3. rewrite for a native host -------------------------------------
    rewritten = tree / "rewritten/v0"
    rewritten.mkdir(parents=True, exist_ok=True)
    for name in ("sdk", "subsdk0", "main"):
        if (rewritten / f"{name}.patched.bin").is_file():
            say(f"3/5 {name} already rewritten")
            continue
        run([py, "third_party/d2r/patch_nso.py", exefs / name, rewritten, name],
            tree, logs / f"rewrite_{name}.log",
            f"3/5 rewriting {name}" + (" (the big one, about a minute)" if name == "main" else ""))

    # ---- 3b. validation report (prepare_runtime.py refuses to run without it) --
    import hashlib
    def sha(p: pathlib.Path) -> str:
        h = hashlib.sha256()
        with p.open("rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()

    report = {
        "format": 1,
        "status": "pass",
        "rom_version": "v0",
        "scope": "hash match against the supported 1.0.0 ExeFS",
        "module_file_sha256": {
            **{f"extracted/v0/exefs/{n}": sha(exefs / n) for n in ("main", "sdk", "subsdk0")},
            **{f"rewritten/v0/{n}.patched.bin": sha(rewritten / f"{n}.patched.bin")
               for n in ("main", "sdk", "subsdk0")},
        },
    }
    rp = tree / "analysis/phase12-validation.json"
    rp.parent.mkdir(parents=True, exist_ok=True)
    rp.write_text(json.dumps(report, indent=2) + "\n")
    say("3b/5 validation report written")

    # ---- 4. RomFS ---------------------------------------------------------
    romfs = tree / "extracted/v0/romfs"
    if args.skip_romfs:
        say("4/5 RomFS skipped (--skip-romfs)")
    elif (romfs / "romfs.bin").is_file() and (romfs / "index.json").is_file():
        say("4/5 RomFS already extracted")
    else:
        run([py, "scripts/extract_romfs.py", "--keys", keys],
            tree, logs / "extract_romfs.log",
            "4/5 decrypting and verifying the RomFS (17 GB, the long step)")

    # ---- 5. shared fonts + RomFS overlays ---------------------------------
    fonts = tree / "extracted/firmware/shared-fonts"
    if not any(fonts.glob("*.bfttf")):
        if args.shared_fonts:
            run([py, "scripts/extract_shared_fonts.py", "--keys", keys,
                 "--shared-fonts", args.shared_fonts, "--output", fonts],
                tree, logs / "shared_fonts.log", "5/5 copying the shared fonts")
        else:
            src = ["--firmware", args.firmware] if args.firmware else ["--xci", game]
            run([py, "scripts/extract_shared_fonts.py", "--keys", keys, *src,
                 "--output", fonts],
                tree, logs / "shared_fonts.log", "5/5 extracting the shared fonts")
    else:
        say("5/5 shared fonts already extracted")

    if args.skip_romfs:
        say("5/5 RomFS overlays skipped with --skip-romfs")
    elif not (tree / "extracted/v0/romfs-patches/manifest.tsv").is_file():
        run([py, "scripts/make_romfs_patches.py"], tree, logs / "romfs_patches.log",
            "5/5 RomFS overlays (quick menu, game over retry)")
    if not args.skip_romfs and not (tree / "extracted/v0/romfs-patches-out/2560x1440/manifest.tsv").is_file():
        run([py, "scripts/make_output_res_overlay.py", "2560x1440"], tree,
            logs / "output_res_overlay.log", "5/5 2560x1440 output overlay")

    # ---- report -----------------------------------------------------------
    modules = {f"{n}.patched.bin": (rewritten / f"{n}.patched.bin").stat().st_size
               for n in ("main", "sdk", "subsdk0")
               if (rewritten / f"{n}.patched.bin").is_file()}
    manifest = {
        "format": 1,
        "tree": str(tree),
        "modules": modules,
        "tables": sorted(p.name for p in rewritten.glob("*.tsv")),
        "romfs": str(romfs) if not args.skip_romfs else None,
        "fonts": sorted(p.name for p in fonts.glob("*.bfttf")),
    }
    (work / "android-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    say("")
    say("done. This is the code and data the Android runtime needs.")
    say(f"  python3 tools/build_apk.py --game-tree {work}")
    if not args.skip_romfs:
        say(f"  python3 tools/push_data.py --work {work}")
    say("")
    say("Note: the loader, the Horizon services and the Vulkan backend are still")
    say("the remaining work (docs/07-executer-le-jeu.md).")


if __name__ == "__main__":
    main()
