#!/usr/bin/env python3
"""check_provenance.py - the repository must never contain game data or keys.

MacNativeGames' rule, and this port's: the tree holds only code and
documentation.  The game, its keys and everything derived from them are
produced by the *player's* build, from the player's own files, and stay on the
player's machine.

This script fails if anything that could be game-derived is found:

    python3 tools/check_provenance.py          # scan the repository
    python3 tools/check_provenance.py --all    # also scan build/ and out/

Exit status 1 means the tree must not be published as is.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Names that only ever come from a game dump, a key dump, or a build of one.
DENY_NAMES = {
    "prod.keys", "title.keys", "console.keys", "dev.keys",
    "runtime_tables.h", "romfs.bin", "main.npdm",
}
DENY_SUFFIX = {
    ".xci", ".nsp", ".nsz", ".xcz", ".nca", ".ncz", ".tik", ".cert",
    ".bfttf", ".patched.bin", ".sites.tsv", ".gates.tsv", ".fixups.tsv",
    ".fixup.tsv", ".cuts.tsv", ".obs.tsv", ".params.tsv",
}
# Container / executable magics that can only come from the game.
MAGICS = (b"HEAD", b"PFS0", b"NSO0", b"IVFC")

# A line that looks like a pasted key dump.
KEY_LINE = re.compile(rb"^\s*(master_key|titlekek|key_area_key|ticket|"
                      rb"header_key|xci_header_key)\w*\s*=\s*[0-9a-fA-F]{16,}\s*$",
                      re.MULTILINE)

SKIP_DIRS = {".git", ".import-venv", "__pycache__", "gen"}


def scan(path: pathlib.Path, big: int) -> list[str]:
    problems: list[str] = []
    for p in sorted(path.rglob("*")):
        if any(part in SKIP_DIRS for part in p.parts):
            continue
        if not p.is_file() or p.is_symlink():
            continue
        rel = p.relative_to(ROOT)
        if p.name in DENY_NAMES or p.suffix in DENY_SUFFIX:
            problems.append(f"{rel}: game-derived file name")
            continue
        size = p.stat().st_size
        if size > big:
            problems.append(f"{rel}: {size/1e6:.1f} MB binary (too large for source)")
            continue
        try:
            head = p.open("rb").read(8)
        except OSError:
            continue
        if head[:4] in MAGICS:
            problems.append(f"{rel}: starts with {head[:4]!r} (game container)")
            continue
        if size < 1 << 20:
            try:
                if KEY_LINE.search(p.read_bytes()):
                    problems.append(f"{rel}: looks like a key dump")
            except OSError:
                pass
    return problems


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--all", action="store_true",
                    help="also scan build/ and out/ (they hold local artifacts)")
    ap.add_argument("--max-mb", type=int, default=8,
                    help="largest file accepted as source (default 8 MB)")
    args = ap.parse_args()

    problems: list[str] = []
    for sub in ("app", "docs", "tools", "tests"):
        d = ROOT / sub
        if d.is_dir():
            problems += scan(d, args.max_mb * 1024 * 1024)
    if args.all:
        for sub in ("build", "out"):
            d = ROOT / sub
            if d.is_dir():
                problems += scan(d, args.max_mb * 1024 * 1024)

    if problems:
        print("REPOSITORY IS NOT CLEAN:", file=sys.stderr)
        for line in sorted(set(problems)):
            print("  " + line, file=sys.stderr)
        print("\nEvery one of these must be produced by the player's build, "
              "not committed.", file=sys.stderr)
        raise SystemExit(1)

    print(f"clean: no game data, no keys, no game-derived file under {ROOT.name}")
    print("  (the game itself is imported by the player with "
          "tools/import_totk.py, into a folder outside the repository)")


if __name__ == "__main__":
    main()
