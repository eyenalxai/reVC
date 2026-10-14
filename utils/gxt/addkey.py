#!/usr/bin/env python3
"""Insert a key into a Vice City .gxt text table.

The game loads gamefiles/TEXT/*.gxt (see src/text/Text.cpp).  The shipped
tables were built by gxt.exe from the .txt sources in this directory, but
gxt.exe needs Wine, so this script patches a .gxt directly.  It adds one key
to the main menu text table; add the same key to the .txt sources as well so
a future gxt.exe rebuild keeps it.

Verified file layout (all shipped tables match):

  TABL chunk    "TABL" + u32 size.  size/12 entries of
                (char name[8], u32 absolute file offset).  Entry "MAIN"
                points at the main TKEY chunk; the other entries point at
                the mission text sections.
  TKEY chunk    "TKEY" + u32 size.  size/12 entries of
                (u32 offset, char key[8]).  The table is sorted by key:
                CKeyArray::BinarySearch strcmp()s through it.  offset is
                relative to the first byte of the main TDAT data.
  TDAT chunk    "TDAT" + u32 size, then UTF-16LE strings, each terminated
                by a NUL character.
  mission       char name[8] + TKEY chunk + TDAT chunk.  Sections sit back
  sections      to back after the main TDAT; a section start may be padded
                with up to 2 zero bytes for alignment.

The new TKEY entry goes at its sorted position.  Because the entry sits
before the TDAT data, all stored (relative) string offsets stay valid; the
string is appended at the end of the main TDAT data and both chunk sizes are
updated.  TABL holds absolute offsets, so the mission entries shift by the
number of inserted bytes.

Usage:
    python3 utils/gxt/addkey.py KEY TEXT FILE.gxt [FILE.gxt ...]

Example:
    python3 utils/gxt/addkey.py FEC_DRF "DRIFT HANDLING" \
        gamefiles/TEXT/american.gxt gamefiles/TEXT/french.gxt
"""

import argparse
import os
import struct
import sys
import tempfile

TABL, TKEY, TDAT = b"TABL", b"TKEY", b"TDAT"
KEY_BYTES = 8
ENTRY_SIZE = 12  # u32 offset + char key[8]


class GxtError(Exception):
    pass


def u32(d, off):
    return struct.unpack_from("<I", d, off)[0]


def strcmp(a, b):
    """Compare NUL-terminated byte strings like C strcmp with signed chars."""
    for x, y in zip(a, b):
        if x != y:
            return (x - 256 if x > 127 else x) - (y - 256 if y > 127 else y)
    return len(a) - len(b)


def parse(d):
    """Validate the chunk/section layout and return the fixed offsets."""
    if len(d) < 8 or d[:4] != TABL:
        raise GxtError("not a VC GXT file: no TABL chunk at offset 0")
    tabl_size = u32(d, 4)
    if tabl_size % ENTRY_SIZE:
        raise GxtError("TABL size %d is not a multiple of %d" % (tabl_size, ENTRY_SIZE))
    num_tables = tabl_size // ENTRY_SIZE
    tkey_off = 8 + tabl_size
    if d[tkey_off:tkey_off + 4] != TKEY:
        raise GxtError("no TKEY chunk after TABL")
    tkey_size = u32(d, tkey_off + 4)
    if tkey_size % ENTRY_SIZE:
        raise GxtError("TKEY size %d is not a multiple of %d" % (tkey_size, ENTRY_SIZE))
    tdat_off = tkey_off + 8 + tkey_size
    if d[tdat_off:tdat_off + 4] != TDAT:
        raise GxtError("no TDAT chunk after TKEY")
    tdat_size = u32(d, tdat_off + 4)
    info = {
        "tabl_size": tabl_size,
        "num_tables": num_tables,
        "tkey_off": tkey_off,
        "tkey_size": tkey_size,
        "tkey_data": tkey_off + 8,
        "tdat_off": tdat_off,
        "tdat_size": tdat_size,
        "tdat_data": tdat_off + 8,
        "tdat_end": tdat_off + 8 + tdat_size,
        "missions": [],  # (TABL index, name, absolute offset)
    }
    prev_end = None
    for i in range(num_tables):
        e = 8 + i * ENTRY_SIZE
        name = d[e:e + 8]
        off = u32(d, e + 8)
        if i == 0:
            if name[:4] != b"MAIN" or off != tkey_off:
                raise GxtError("TABL[0] is not MAIN -> main TKEY")
            continue
        if off + 20 > len(d) or d[off:off + 8] != name or d[off + 8:off + 12] != TKEY:
            raise GxtError("TABL[%d] does not point at a mission TKEY chunk" % i)
        ms = u32(d, off + 12)
        off2 = off + 16 + ms
        if d[off2:off2 + 4] != TDAT:
            raise GxtError("mission section %d has no TDAT chunk" % i)
        end = off2 + 8 + u32(d, off2 + 4)
        if end > len(d):
            raise GxtError("mission section %d runs past EOF" % i)
        if prev_end is None:
            pad = d[info["tdat_end"]:off]
            if pad not in (b"", b"\0", b"\0\0"):
                raise GxtError("padding before mission section %d is not zero" % i)
        elif off - prev_end not in (0, 2):
            raise GxtError("mission section %d starts at odd gap %d" % (i, off - prev_end))
        elif any(d[prev_end:off]):
            raise GxtError("mission section %d gap is not zero padding" % i)
        info["missions"].append((i, name.split(b"\0")[0], off))
        prev_end = end
    if prev_end != len(d):
        raise GxtError("last mission section ends at %d, file has %d bytes" % (prev_end, len(d)))
    return info


def entries(d, info):
    """Return the main TKEY entries as a list of (key bytes, relative offset)."""
    n = info["tkey_size"] // ENTRY_SIZE
    out = []
    for i in range(n):
        b = info["tkey_data"] + i * ENTRY_SIZE
        off = u32(d, b)
        key = d[b + 4:b + 12].split(b"\0")[0]
        out.append((key, off))
    return out


def string_at(d, info, rel):
    """Decode the UTF-16LE string at a TDAT-relative offset."""
    if rel % 2 or rel + 2 > info["tdat_size"]:
        raise GxtError("string offset %d is out of bounds" % rel)
    base = info["tdat_data"] + rel
    end = info["tdat_data"] + info["tdat_size"]
    i = base
    while i + 2 <= end:
        if d[i] == 0 and d[i + 1] == 0:
            return d[base:i].decode("utf-16le")
        i += 2
    raise GxtError("string at offset %d has no NUL terminator" % rel)


def patch(d, info, key, text):
    """Return (new contents, insert index, previous key, next key, TDAT offset)."""
    kb = key.encode("ascii")
    if not kb or len(kb) >= KEY_BYTES:
        raise GxtError("key must be 1..%d ASCII bytes (key[8] needs the NUL)" % (KEY_BYTES - 1))
    old = entries(d, info)
    if any(k == kb for k, _ in old):
        raise GxtError("key %s is already in the table" % key)

    idx = 0
    while idx < len(old) and strcmp(old[idx][0], kb) < 0:
        idx += 1
    prev = old[idx - 1][0] if idx else None
    nxt = old[idx][0] if idx < len(old) else None

    if info["tdat_size"] % 2:
        raise GxtError("TDAT data size is not u16 aligned")
    str_off = info["tdat_size"]  # append at the end of the main TDAT data
    strb = text.encode("utf-16le") + b"\x00\x00"
    entry = struct.pack("<I", str_off) + kb.ljust(KEY_BYTES, b"\0")
    pos = info["tkey_data"] + idx * ENTRY_SIZE
    tdat_end = info["tdat_end"]

    for _, _, off in info["missions"]:
        if pos <= off < tdat_end:
            raise GxtError("TABL offset %d points inside the main text chunks" % off)

    shifted = ENTRY_SIZE + len(strb)
    new = bytearray(d[:pos] + entry + d[pos:tdat_end] + strb + d[tdat_end:])
    # the TKEY size field sits before the insertion point, the TDAT size
    # field after it, so only the latter moves by ENTRY_SIZE
    struct.pack_into("<I", new, info["tkey_off"] + 4, info["tkey_size"] + ENTRY_SIZE)
    struct.pack_into("<I", new, info["tdat_off"] + ENTRY_SIZE + 4, info["tdat_size"] + len(strb))
    for i, _, off in info["missions"]:
        struct.pack_into("<I", new, 8 + i * ENTRY_SIZE + 8, off + shifted)
    return bytes(new), idx, prev, nxt, str_off


def verify(old_d, new_d, key, text):
    """Re-check the patched table against the format and the original bytes."""
    kb = key.encode("ascii")
    strb = text.encode("utf-16le") + b"\x00\x00"
    oi = parse(old_d)
    ni = parse(new_d)
    oe = entries(old_d, oi)
    ne = entries(new_d, ni)

    if len(ne) != len(oe) + 1:
        raise GxtError("expected %d entries, found %d" % (len(oe) + 1, len(ne)))
    idx = next((i for i, (k, _) in enumerate(ne) if k == kb), None)
    if idx is None:
        raise GxtError("key %s is not in the new table" % key)
    for i in range(1, len(ne)):
        if strcmp(ne[i - 1][0], ne[i][0]) >= 0:
            raise GxtError("table is not sorted at index %d" % i)
    if ne[:idx] != oe[:idx] or ne[idx + 1:] != oe[idx:]:
        raise GxtError("existing entries changed")

    # every offset is in bounds and decodes as a NUL-terminated UTF-16LE string
    for k, off in ne:
        if off % 2 or off + 2 > ni["tdat_size"]:
            raise GxtError("key %r offset %d out of bounds" % (k, off))
        try:
            string_at(new_d, ni, off)
        except GxtError as e:
            raise GxtError("key %r: %s" % (k, e))
        except UnicodeDecodeError:
            raise GxtError("key %r offset %d is not valid UTF-16LE" % (k, off))
    if string_at(new_d, ni, ne[idx][1]) != text:
        raise GxtError("%s does not resolve to %r" % (key, text))

    # sizes and file bodies are consistent with the original
    if ni["tkey_size"] != oi["tkey_size"] + ENTRY_SIZE:
        raise GxtError("TKEY size is wrong")
    if ni["tdat_size"] != oi["tdat_size"] + len(strb):
        raise GxtError("TDAT size is wrong")
    if len(new_d) != len(old_d) + ENTRY_SIZE + len(strb):
        raise GxtError("file size is wrong")
    if new_d[ni["tdat_data"]:ni["tdat_data"] + oi["tdat_size"]] != \
            old_d[oi["tdat_data"]:oi["tdat_data"] + oi["tdat_size"]]:
        raise GxtError("main text data changed")
    if new_d[ni["tkey_data"]:ni["tkey_data"] + idx * ENTRY_SIZE] != \
            old_d[oi["tkey_data"]:oi["tkey_data"] + idx * ENTRY_SIZE]:
        raise GxtError("text before the inserted key changed")
    if new_d[ni["tkey_data"] + (idx + 1) * ENTRY_SIZE:ni["tdat_off"]] != \
            old_d[oi["tkey_data"] + idx * ENTRY_SIZE:oi["tdat_off"]]:
        raise GxtError("text after the inserted key changed")
    if new_d[ni["tdat_end"]:] != old_d[oi["tdat_end"]:]:
        raise GxtError("mission sections changed")
    shift = ENTRY_SIZE + len(strb)
    if [m[1] for m in ni["missions"]] != [m[1] for m in oi["missions"]] or \
            [m[2] for m in ni["missions"]] != [m[2] + shift for m in oi["missions"]]:
        raise GxtError("mission section offsets are wrong")
    return idx


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("key", help="GXT key, up to 7 ASCII bytes")
    ap.add_argument("text", help="string the key resolves to")
    ap.add_argument("files", nargs="+", help=".gxt files to patch in place")
    ap.add_argument("--dry-run", action="store_true", help="verify only, do not write")
    args = ap.parse_args(argv)

    failures = 0
    for path in args.files:
        name = os.path.basename(path)
        try:
            with open(path, "rb") as f:
                old_d = f.read()
            info = parse(old_d)
            num_old = len(entries(old_d, info))
            new_d, idx, prev, nxt, str_off = patch(old_d, info, args.key, args.text)
            verify(old_d, new_d, args.key, args.text)
            if not args.dry_run:
                mode = os.stat(path).st_mode & 0o7777
                fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path) or ".")
                try:
                    with os.fdopen(fd, "wb") as f:
                        f.write(new_d)
                    os.chmod(tmp, mode)
                    os.replace(tmp, path)
                except BaseException:
                    os.unlink(tmp)
                    raise
                with open(path, "rb") as f:
                    if f.read() != new_d:
                        raise GxtError("written file does not match verified buffer")
            print("%s OK" % name)
            print("  keys %d -> %d, %s inserted at index %d (%s < %s < %s), TDAT offset %d, %d string bytes"
                  % (num_old, num_old + 1, args.key, idx,
                     prev.decode("latin-1") if prev else "None", args.key,
                     nxt.decode("latin-1") if nxt else "None", str_off, len(args.text.encode("utf-16le")) + 2))
            print("  TKEY %d -> %d, TDAT %d -> %d, file %d -> %d bytes"
                  % (info["tkey_size"], info["tkey_size"] + ENTRY_SIZE, info["tdat_size"],
                     info["tdat_size"] + len(args.text.encode("utf-16le")) + 2, len(old_d), len(new_d)))
            print("  checks: table sorted, %d/%d offsets in bounds and NUL-terminated, "
                  "TABL %d/%d mission sections intact, %s = %r"
                  % (len(entries(new_d, parse(new_d))), len(entries(new_d, parse(new_d))),
                     len(info["missions"]), len(info["missions"]), args.key, args.text))
        except (GxtError, OSError, UnicodeDecodeError) as e:
            failures += 1
            print("%s FAILED: %s" % (name, e), file=sys.stderr)
    if failures:
        print("%d of %d files failed" % (failures, len(args.files)), file=sys.stderr)
        return 1
    print("%d/%d files patched" % (len(args.files), len(args.files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
