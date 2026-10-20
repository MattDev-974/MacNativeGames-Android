#!/usr/bin/env python3
"""check_inputs.py - is this XCI/NSP + prod.keys pair usable, before a 17 GB import?

Reads nothing but the container header and the key names: it never decrypts,
never prints a key value, and needs no network.

    python3 tools/check_inputs.py ~/import/totk.xci -k ~/import/prod.keys
"""
from __future__ import annotations

import argparse
import pathlib
import struct
import sys

# The base game this port supports.  Anything else stops here rather than after
# an hour of extraction.
TOTK_BASE_TITLE_ID = 0x0100F2C0115B6000

# Minimum key set for a base game: the master keys derive the key area, and the
# ticket path needs a titlekek.  Values are never read out, only presence.
REQUIRED_KEY_PREFIXES = ("master_key_", "titlekek_")


def say(msg: str) -> None:
    print(msg, flush=True)


def fail(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr, flush=True)
    raise SystemExit(1)


def sniff_container(path: pathlib.Path) -> str:
    """XCI: 'HEAD' at 0x100 inside the HFS0 game-card header.  NSP: 'PFS0' at 0."""
    size = path.stat().st_size
    with path.open("rb") as f:
        head = f.read(0x200)
        f.seek(0)
        magic0 = f.read(4)
    if magic0 == b"PFS0":
        return "nsp"
    if len(head) >= 0x104 and head[0x100:0x104] == b"HEAD":
        return "xci"
    fail(f"{path.name}: neither an XCI ('HEAD' at 0x100) nor an NSP ('PFS0' at 0). "
         f"First bytes: {magic0!r}, size {size} bytes. "
         "A .nsz/.xcz must be decompressed first.")


def nsp_title_id(path: pathlib.Path) -> int | None:
    """The PFS0 file table names the Program NCA, whose name starts with the
    title id in hex.  Cheap, unencrypted, and enough to catch the wrong game."""
    with path.open("rb") as f:
        head = f.read(0x10)                      # magic, count, stringTableSize, reserved
        if head[:4] != b"PFS0":
            return None
        count, string_table_size = struct.unpack_from("<II", head, 4)
        if not count or count > 4096:
            return None
        table = f.read(count * 0x18)             # entry: u64 off, u64 size, u32 name, u32 pad
        names = f.read(string_table_size)
    for i in range(count):
        _off, _size, nameoff, _pad = struct.unpack_from("<QQII", table, i * 0x18)
        if nameoff >= len(names):
            continue
        end = names.find(b"\0", nameoff)
        name = names[nameoff:end if end >= 0 else nameoff + 64].decode("ascii", "replace")
        if name.endswith(".nca"):
            try:
                return int(name.split(".")[0][:16], 16)
            except ValueError:
                continue
    return None


def _read_at(f, offset: int, length: int) -> bytes:
    f.seek(offset)
    data = f.read(length)
    if len(data) != length:
        raise ValueError("truncated container")
    return data


def hfs0_dir(f, offset: int, bound: int) -> dict[str, tuple[int, int]] | None:
    """One HFS0 partition table.  Entry stride is 0x40; the entry's data offset
    is relative to the end of the table, not to the table's start."""
    if offset < 0 or bound < 16:
        return None
    hdr = _read_at(f, offset, 16)
    if hdr[:4] != b"HFS0":
        return None
    count, strsize = struct.unpack_from("<II", hdr, 4)
    if count > 4096 or strsize > 1 << 20:
        return None
    size = 16 + count * 64 + strsize
    if size > bound:
        return None
    raw = _read_at(f, offset, size)
    strings = raw[16 + count * 64:]
    out: dict[str, tuple[int, int]] = {}
    for i in range(count):
        off, length, nameoff = struct.unpack_from("<QQI", raw, 16 + i * 64)
        if nameoff >= len(strings):
            continue
        end = strings.find(b"\0", nameoff)
        if end < 0:
            continue
        name = strings[nameoff:end].decode("utf-8", "replace")
        out[name] = (offset + size + off, length)
    return out


def xci_title_id(path: pathlib.Path) -> int | None:
    """The cartridge header carries no title id.  It is in the Program NCA's
    *filename*, inside the secure partition, which stays plaintext even though
    the NCA contents are encrypted.  Root HFS0 offset lives at 0x130."""
    total = path.stat().st_size
    with path.open("rb") as f:
        if _read_at(f, 0x100, 4) != b"HEAD":
            return None
        root = struct.unpack("<Q", _read_at(f, 0x130, 8))[0]
        if not 0 < root < total:
            return None
        parts = hfs0_dir(f, root, total - root)
        if not parts:
            return None
        for part in ("secure", "normal", "update"):
            entry = parts.get(part)
            if entry is None:
                continue
            table = hfs0_dir(f, entry[0], entry[1])
            if not table:
                continue
            for name in sorted(table):
                if not name.endswith(".nca"):
                    continue
                try:
                    return int(name.split(".")[0][:16], 16)
                except ValueError:
                    continue
    return None


def read_keys(path: pathlib.Path) -> dict[str, int]:
    keys: dict[str, int] = {}
    for lineno, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if "=" not in line:
            fail(f"{path.name}:{lineno}: expected name = hex")
        name, _, value = line.partition("=")
        name, value = name.strip(), value.strip().replace(" ", "")
        if not value or any(c not in "0123456789abcdefABCDEF" for c in value):
            fail(f"{path.name}:{lineno}: {name} is not hexadecimal")
        if len(value) % 2:
            fail(f"{path.name}:{lineno}: {name} has an odd number of hex digits")
        keys[name] = len(value) // 2
    return keys


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", type=pathlib.Path, help="the XCI or NSP file")
    ap.add_argument("-k", "--keys", type=pathlib.Path, required=True, help="prod.keys")
    args = ap.parse_args()

    game = args.game.expanduser().resolve()
    keys_path = args.keys.expanduser().resolve()
    for what, p in (("game file", game), ("prod.keys", keys_path)):
        if not p.is_file():
            fail(f"{what} not found: {p}")

    say(f"game      {game}")
    say(f"          {game.stat().st_size / 1e9:.2f} GB")

    kind = sniff_container(game)
    say(f"format    {kind.upper()}")

    tid = nsp_title_id(game) if kind == "nsp" else xci_title_id(game)
    if tid is None:
        say("title id  could not be read from the header (not fatal: the import "
            "verifies hashes itself)")
    else:
        say(f"title id  {tid:016X}")
        if tid != TOTK_BASE_TITLE_ID:
            say("")
            say(f"WARNING: this port targets the base game {TOTK_BASE_TITLE_ID:016X}")
            say("         (Tears of the Kingdom 1.0.0). The import will stop when")
            say("         the ExeFS hashes do not match, so nothing is wasted --")
            say("         but check you picked the base game and not an update.")

    keys = read_keys(keys_path)
    say(f"keys      {len(keys)} entries, none displayed")
    missing = [p for p in REQUIRED_KEY_PREFIXES
               if not any(k.startswith(p) for k in keys)]
    if missing:
        fail("prod.keys lacks " + ", ".join(missing) + " "
             "(an incomplete dump: re-dump with Lockpick_RCM)")
    if "master_key_00" not in keys:
        fail("prod.keys has no master_key_00; this is not a full prod.keys")

    sizes = {keys[k] for k in keys if k.startswith("master_key_")}
    if sizes - {16}:
        say(f"WARNING: unexpected master key length(s) {sorted(sizes)}; expected 16 bytes")

    say("")
    say("OK. Nothing was decrypted. Next:")
    say(f"  python3 tools/import_totk.py {game} -k {keys_path} "
        f"--upstream {(pathlib.Path(__file__).resolve().parent.parent.parent / 'mng')}")


if __name__ == "__main__":
    main()
