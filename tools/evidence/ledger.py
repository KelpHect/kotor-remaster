#!/usr/bin/env python3
"""SQLite evidence/coverage ledger for the KotOR interoperability program.

Stores artifact hashes, function coverage, action-table status, resource-type
counts, and cited claims. Local only; the database lives under artifacts/.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sqlite3
import sys
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "inventory"))
from kotor_types import label  # noqa: E402

DEFAULT_DB = "artifacts/evidence.db"
DEFAULT_INVENTORY = "artifacts/inventory.json"
DEFAULT_ACTIONS = "artifacts/action_table.json"
DEFAULT_FUNCTIONS = "artifacts/ghidra/functions-full.jsonl"


def now() -> str:
    return datetime.now(timezone.utc).isoformat()


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


SCHEMA = """
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS artifacts (
    name TEXT PRIMARY KEY,
    kind TEXT NOT NULL,
    path TEXT,
    sha256 TEXT,
    size INTEGER,
    mtime TEXT,
    notes TEXT
);
CREATE TABLE IF NOT EXISTS functions (
    entry TEXT PRIMARY KEY,
    name TEXT,
    size INTEGER,
    instructions INTEGER,
    pseudocode_sha256 TEXT,
    status TEXT NOT NULL DEFAULT 'decompiled',
    notes TEXT
);
CREATE TABLE IF NOT EXISTS actions (
    id INTEGER PRIMARY KEY,
    name TEXT,
    args TEXT,
    handler TEXT,
    registered_by TEXT,
    status TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS resource_types (
    source TEXT NOT NULL,
    type INTEGER NOT NULL,
    label TEXT,
    count INTEGER,
    bytes INTEGER,
    PRIMARY KEY (source, type)
);
CREATE TABLE IF NOT EXISTS claims (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    claim TEXT NOT NULL,
    status TEXT NOT NULL,
    confidence TEXT NOT NULL,
    evidence TEXT,
    created TEXT NOT NULL
);
CREATE VIEW IF NOT EXISTS coverage AS
SELECT
    (SELECT COUNT(*) FROM functions) AS functions_total,
    (SELECT COUNT(*) FROM functions WHERE pseudocode_sha256 IS NOT NULL) AS functions_decompiled,
    (SELECT COUNT(*) FROM actions) AS actions_total,
    (SELECT COUNT(*) FROM actions WHERE handler IS NOT NULL) AS actions_mapped,
    (SELECT COUNT(*) FROM actions WHERE handler IS NULL) AS actions_unmapped,
    (SELECT COUNT(*) FROM claims) AS claims_total;
"""


def add_artifact(conn: sqlite3.Connection, name: str, kind: str, path: str | None,
                 sha256: str | None, size: int | None, mtime: str | None, notes: str | None = None) -> None:
    conn.execute(
        "INSERT OR REPLACE INTO artifacts(name,kind,path,sha256,size,mtime,notes) VALUES (?,?,?,?,?,?,?)",
        (name, kind, path, sha256, size, mtime, notes),
    )


def import_inventory(conn: sqlite3.Connection, path: str) -> None:
    with open(path, encoding="utf-8") as f:
        inv = json.load(f)
    for row in inv.get("manifest", []):
        add_artifact(conn, row["name"], "game", os.path.join(inv["game_dir"], row["name"]),
                     row["sha256"], row["size"], row["mtime_utc"])
    add_artifact(conn, "inventory.json", "evidence", os.path.abspath(path), sha256_file(path),
                 os.path.getsize(path), now(), "archive inventory")
    for source, counts, bytes_map in (
        ("base_bif", inv.get("resources", {}).get("by_type", {}), inv.get("resources", {}).get("by_type_bytes", {})),
        ("modules", inv.get("modules", {}).get("by_type", {}), {}),
        ("global_rims", inv.get("global_rims", {}).get("by_type", {}), {}),
    ):
        for rtype, count in counts.items():
            rtype = int(rtype)
            conn.execute("INSERT OR REPLACE INTO resource_types(source,type,label,count,bytes) VALUES (?,?,?,?,?)",
                         (source, rtype, label(rtype), count, bytes_map.get(str(rtype))))
    for erf in inv.get("erf_archives", []):
        for rtype, count in erf.get("by_type", {}).items():
            rtype = int(rtype)
            src = f"erf:{erf['file']}"
            conn.execute("INSERT OR REPLACE INTO resource_types(source,type,label,count,bytes) VALUES (?,?,?,?,?)",
                         (src, rtype, label(rtype), count, None))


def import_actions(conn: sqlite3.Connection, path: str) -> None:
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    for a in data["actions"]:
        conn.execute(
            "INSERT OR REPLACE INTO actions(id,name,args,handler,registered_by,status) VALUES (?,?,?,?,?,?)",
            (a["id"], a["name"], a.get("args"), a.get("handler"), a.get("registered_by"),
             "mapped" if a.get("handler") else "unassigned"),
        )
    add_artifact(conn, "action_table.json", "evidence", os.path.abspath(path), sha256_file(path),
                 os.path.getsize(path), now(), f"initializer {data.get('initializer')}")


def import_functions(conn: sqlite3.Connection, path: str, status: str | None = None,
                     artifact_name: str | None = None) -> int:
    n = 0
    with open(path, encoding="utf-8") as f:
        for line in f:
            if not line.strip():
                continue
            row = json.loads(line)
            state = status or ("decompiled" if row.get("pseudocode_sha256") else "failed")
            conn.execute(
                "INSERT OR REPLACE INTO functions(entry,name,size,instructions,pseudocode_sha256,status) VALUES (?,?,?,?,?,?)",
                (row["entry"], row.get("name"), row.get("size"), row.get("instructions"),
                 row.get("pseudocode_sha256"), state),
            )
            n += 1
    add_artifact(conn, artifact_name or os.path.basename(path), "evidence", os.path.abspath(path),
                 sha256_file(path), os.path.getsize(path), now(), f"{n} functions ({status or 'auto'})")
    return n


def seed_claims(conn: sqlite3.Connection, exe_sha: str | None) -> None:
    claims = [
        ("Artifact of record is swkotor.exe SHA-256 " + (exe_sha or "unknown") + "; the prior Ghidra program "
         "(34e6d971…, .bind, entry 0x86d2ed) was a different build and is discarded.", "verified", "observed",
         "docs/recon/01_ARTIFACTS.md; artifacts/ghidra/analyze.log"),
        ("Full decompilation coverage: 8,616/8,616 functions, 0 errors.", "verified", "observed",
         "artifacts/ghidra/export-full.log; functions-full.jsonl"),
        ("NWScript action table has 772 slots; 757 direct handlers; names from shipped nwscript.nss.", "verified",
         "observed", "artifacts/action_table.json; initializer 0x0054c960 + 0x005cd2c0"),
        ("Resource type 3 is TGA; 2016/2052/2053 are WOK/DWK/PWK; TPC is 3007.", "verified", "observed",
         "docs/recon/01_ARTIFACTS.md; payload signatures"),
        ("VFS precedence is five passes (directories, patch ERFs, RIMs, tagged-2 ERFs, BIFs); measure texture-pack selection.",
         "documented", "derived", "docs/recon/02_TECHNICAL_NOTES.md"),
    ]
    for claim, status, confidence, evidence in claims:
        exists = conn.execute("SELECT 1 FROM claims WHERE claim=?", (claim,)).fetchone()
        if not exists:
            conn.execute("INSERT INTO claims(claim,status,confidence,evidence,created) VALUES (?,?,?,?,?)",
                         (claim, status, confidence, evidence, now()))


def cmd_init(args: argparse.Namespace) -> int:
    os.makedirs(os.path.dirname(os.path.abspath(args.db)), exist_ok=True)
    conn = sqlite3.connect(args.db)
    conn.executescript(SCHEMA)
    import_inventory(conn, args.inventory)
    import_actions(conn, args.actions)
    import_functions(conn, args.functions)
    exe_sha = conn.execute("SELECT sha256 FROM artifacts WHERE name='swkotor.exe'").fetchone()
    seed_claims(conn, exe_sha[0] if exe_sha else None)
    conn.commit()
    print_status(conn)
    conn.close()
    return 0


def print_status(conn: sqlite3.Connection) -> None:
    cov = conn.execute("SELECT * FROM coverage").fetchone()
    cols = [d[0] for d in conn.execute("SELECT * FROM coverage").description]
    print("coverage:", dict(zip(cols, cov)))
    print("actions unassigned:", [r[0] for r in conn.execute(
        "SELECT id FROM actions WHERE status='unassigned' ORDER BY id")])
    print("resource types:", conn.execute("SELECT COUNT(*) FROM resource_types").fetchone()[0],
          "rows across", conn.execute("SELECT COUNT(DISTINCT source) FROM resource_types").fetchone()[0], "sources")
    print("claims:", conn.execute("SELECT COUNT(*) FROM claims").fetchone()[0])


def cmd_status(args: argparse.Namespace) -> int:
    conn = sqlite3.connect(args.db)
    print_status(conn)
    conn.close()
    return 0


def cmd_add_functions(args: argparse.Namespace) -> int:
    conn = sqlite3.connect(args.db)
    n = import_functions(conn, args.path, args.status, args.artifact_name)
    conn.commit()
    print(f"imported {n} functions as {args.status}")
    print_status(conn)
    conn.close()
    return 0


def cmd_claim(args: argparse.Namespace) -> int:
    conn = sqlite3.connect(args.db)
    conn.execute("INSERT INTO claims(claim,status,confidence,evidence,created) VALUES (?,?,?,?,?)",
                 (args.claim, args.status, args.confidence, args.evidence, now()))
    conn.commit()
    print_status(conn)
    conn.close()
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--db", default=DEFAULT_DB)
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("init", help="create/refresh the ledger")
    p.add_argument("--inventory", default=DEFAULT_INVENTORY)
    p.add_argument("--actions", default=DEFAULT_ACTIONS)
    p.add_argument("--functions", default=DEFAULT_FUNCTIONS)
    p.set_defaults(func=cmd_init)

    p = sub.add_parser("status", help="print coverage")
    p.set_defaults(func=cmd_status)

    p = sub.add_parser("add-functions", help="import an additional function JSONL")
    p.add_argument("--path", required=True)
    p.add_argument("--status", default="handler-defined")
    p.add_argument("--artifact-name", default=None)
    p.set_defaults(func=cmd_add_functions)

    p = sub.add_parser("claim", help="add a cited claim")
    p.add_argument("claim")
    p.add_argument("--status", default="open")
    p.add_argument("--confidence", default="observed", choices=["observed", "derived", "inferred"])
    p.add_argument("--evidence", default="")
    p.set_defaults(func=cmd_claim)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
