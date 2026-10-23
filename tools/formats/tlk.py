#!/usr/bin/env python3
"""KotOR 1 TLK V3.0 reader.

Header: 'TLK ' + 'V3.0' + language u32 + entryCount u32 + textOffset u32.
Entry (40 bytes): flags u32, sound resref[16], volume variance u32, pitch variance u32,
text offset u32, text length u32, sound length f32. Text offsets are relative to textOffset.
Read-only; --dump writes game strings only under artifacts/ (never commit).
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from collections import Counter
from datetime import datetime, timezone
from typing import Iterator

ENTRY = struct.Struct("<I16sIIIIf")
HEADER = struct.Struct("<4s4sIII")


def read_header(path: str) -> dict:
    with open(path, "rb") as f:
        head = f.read(HEADER.size)
    sig, ver, language, count, text_off = HEADER.unpack(head)
    if sig != b"TLK " or ver != b"V3.0":
        raise ValueError(f"{path}: expected TLK V3.0, got {sig!r} {ver!r}")
    size = os.path.getsize(path)
    if 20 + count * ENTRY.size > size:
        raise ValueError(f"{path}: entry table exceeds file")
    if text_off > size:
        raise ValueError(f"{path}: text offset outside file")
    return {"path": path, "size": size, "language": language, "count": count, "text_offset": text_off}


def iter_entries(path: str, header: dict) -> Iterator[dict]:
    with open(path, "rb") as f:
        f.seek(HEADER.size)
        table = f.read(header["count"] * ENTRY.size)
        f.seek(header["text_offset"])
        blob = f.read()
    for i in range(header["count"]):
        flags, sound, vol, pitch, off, length, sound_len = ENTRY.unpack_from(table, i * ENTRY.size)
        if off + length > len(blob):
            raise ValueError(f"entry {i}: text [{off},{off+length}) exceeds blob {len(blob)}")
        raw = blob[off:off + length]
        yield {
            "strref": i,
            "flags": flags,
            "sound": sound.split(b"\x00", 1)[0].decode("latin-1", "replace"),
            "volume_variance": vol,
            "pitch_variance": pitch,
            "text": raw.decode("cp1252", "replace"),
            "sound_length": sound_len,
        }


def summarize(path: str, dump: str | None = None) -> dict:
    header = read_header(path)
    total_chars = 0
    total_bytes = 0
    empty = 0
    with_sound = 0
    longest = (0, 0)
    flags = Counter()
    dumped = 0
    out = open(dump, "w", encoding="utf-8") if dump else None
    try:
        for e in iter_entries(path, header):
            total_chars += len(e["text"])
            total_bytes += len(e["text"].encode("cp1252", "replace"))
            empty += (e["text"] == "")
            with_sound += (e["sound"] != "")
            flags[e["flags"]] += 1
            if len(e["text"]) > longest[0]:
                longest = (len(e["text"]), e["strref"])
            if out:
                out.write(json.dumps(e, ensure_ascii=False) + "\n")
                dumped += 1
    finally:
        if out:
            out.close()
    return {
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "file": os.path.abspath(path),
        "size": header["size"],
        "language": header["language"],
        "entries": header["count"],
        "text_offset": header["text_offset"],
        "total_text_chars": total_chars,
        "total_text_bytes_cp1252": total_bytes,
        "empty_strings": empty,
        "entries_with_sound": with_sound,
        "longest_text": {"chars": longest[0], "strref": longest[1]},
        "flag_histogram": dict(sorted(flags.items())),
        "dump_path": os.path.abspath(dump) if dump else None,
        "dumped": dumped,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("path", help="dialog.tlk")
    ap.add_argument("--dump", help="write per-StrRef JSONL under artifacts/")
    ap.add_argument("--out", help="write summary JSON")
    ap.add_argument("--check", action="store_true", help="exit 2 on any structural error")
    args = ap.parse_args()
    try:
        summary = summarize(args.path, args.dump)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2 if args.check else 1
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2)
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
