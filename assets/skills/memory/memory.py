#!/usr/bin/env python3
"""A flat, greppable memory store — append, search, list, forget.

One JSON object per line in a text file. No service, no database, no
embeddings, no API key. It exists because "write down what you learned" is the
one habit nearly every agent needs and almost none start with.

    ./memory.py add --text "Pengy's tests run with QT_QPA_PLATFORM=offscreen"
    ./memory.py search --q "tests offscreen"
    ./memory.py --json list --limit 5

Store location, in order: ``--store``, ``$PENGY_MEMORY_FILE``, then
``memory.jsonl`` next to this script.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

DEFAULT_FILENAME = "memory.jsonl"


def store_path(override: str | None = None) -> Path:
    if override:
        return Path(override).expanduser()
    env = os.environ.get("PENGY_MEMORY_FILE")
    if env:
        return Path(env).expanduser()
    return Path(__file__).resolve().parent / DEFAULT_FILENAME


def load(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    records = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue  # a hand-edit that broke one line must not lose the rest
        if isinstance(record, dict) and "id" in record:
            records.append(record)
    return records


def append(path: Path, record: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(record, ensure_ascii=False) + "\n")


def next_id(records: list[dict]) -> int:
    return max((int(r["id"]) for r in records), default=0) + 1


def _now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def _print(records: list[dict], as_json: bool) -> None:
    if as_json:
        print(json.dumps(records, indent=2, ensure_ascii=False))
        return
    if not records:
        print("(nothing found)")
        return
    for r in records:
        tags = f"  [{', '.join(r['tags'])}]" if r.get("tags") else ""
        subject = f"{r['subject']}: " if r.get("subject") else ""
        print(f"#{r['id']}  {r['ts'][:10]}  {subject}{r['text']}{tags}")


def cmd_add(args) -> int:
    path = store_path(args.store)
    text = args.text.strip()
    if not text:
        print("error: --text is empty", file=sys.stderr)
        return 2
    records = load(path)
    record = {
        "id": next_id(records),
        "ts": _now(),
        "text": text,
        "subject": args.subject.strip(),
        "tags": [t.strip() for t in (args.tags or "").split(",") if t.strip()],
    }
    append(path, record)
    if args.json:
        print(json.dumps({"stored": record, "store": str(path)}, ensure_ascii=False))
    else:
        print(f"Stored note #{record['id']} in {path}")
    return 0


def cmd_search(args) -> int:
    path = store_path(args.store)
    terms = [t for t in (args.q or "").lower().split() if t]
    if not terms:
        print("error: --q is empty", file=sys.stderr)
        return 2
    hits = []
    for record in load(path):
        haystack = " ".join([
            str(record.get("text", "")),
            str(record.get("subject", "")),
            " ".join(record.get("tags", []) or []),
        ]).lower()
        if all(term in haystack for term in terms):
            hits.append(record)
    hits.reverse()  # newest first — recent facts usually win
    _print(hits[:args.limit], args.json)
    return 0 if hits else 1


def cmd_list(args) -> int:
    path = store_path(args.store)
    records = load(path)
    if args.subject:
        wanted = args.subject.lower()
        records = [r for r in records if wanted in str(r.get("subject", "")).lower()]
    if args.tag:
        wanted = args.tag.lower()
        records = [r for r in records
                   if wanted in [t.lower() for t in (r.get("tags") or [])]]
    records.reverse()
    _print(records[:args.limit], args.json)
    return 0


def cmd_stats(args) -> int:
    path = store_path(args.store)
    records = load(path)
    subjects: dict[str, int] = {}
    for record in records:
        key = record.get("subject") or "(no subject)"
        subjects[key] = subjects.get(key, 0) + 1
    stats = {
        "store": str(path),
        "exists": path.is_file(),
        "notes": len(records),
        "bytes": path.stat().st_size if path.is_file() else 0,
        "oldest": records[0]["ts"] if records else None,
        "newest": records[-1]["ts"] if records else None,
        "subjects": subjects,
    }
    if args.json:
        print(json.dumps(stats, indent=2, ensure_ascii=False))
    else:
        print(f"store:   {stats['store']}")
        print(f"notes:   {stats['notes']}  ({stats['bytes']} bytes)")
        if stats["oldest"]:
            print(f"range:   {stats['oldest'][:10]} .. {stats['newest'][:10]}")
        for key, count in sorted(subjects.items(), key=lambda kv: -kv[1]):
            print(f"  {count:4d}  {key}")
    return 0


def cmd_forget(args) -> int:
    path = store_path(args.store)
    records = load(path)
    keep = [r for r in records if int(r["id"]) != args.id]
    if len(keep) == len(records):
        print(f"error: no note with id {args.id}", file=sys.stderr)
        return 1
    if not args.yes:
        print(f"refusing to delete note #{args.id} without --yes "
              f"(text: {next(r['text'] for r in records if int(r['id']) == args.id)})",
              file=sys.stderr)
        return 2
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in keep),
                   encoding="utf-8")
    tmp.replace(path)
    if args.json:
        print(json.dumps({"deleted": args.id, "remaining": len(keep)}))
    else:
        print(f"Deleted note #{args.id}; {len(keep)} left.")
    return 0


def build_parser() -> argparse.ArgumentParser:
    # --json is accepted before OR after the subcommand: the parent gives the
    # flag, and each subparser carries a SUPPRESS'd copy so `search --json`
    # resolves to the same attribute instead of erroring.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--json", action="store_true", default=argparse.SUPPRESS,
                        help="Print machine-readable JSON")
    common.add_argument("--store", default=argparse.SUPPRESS,
                        help=f"Path to the store (default: {DEFAULT_FILENAME} beside this script)")

    parser = argparse.ArgumentParser(
        description="Flat, greppable agent memory: one JSON object per line.")
    parser.add_argument("--json", action="store_true", help="Print machine-readable JSON")
    parser.add_argument("--store", default=None, help="Path to the store")
    sub = parser.add_subparsers(dest="command", required=True)

    add = sub.add_parser("add", parents=[common], help="Store a note")
    add.add_argument("--text", required=True, help="The fact, decision or caveat")
    add.add_argument("--subject", default="", help="Optional grouping, e.g. a project or host")
    add.add_argument("--tags", default="", help="Comma-separated tags")
    add.set_defaults(func=cmd_add)

    search = sub.add_parser("search", parents=[common], help="Find notes (all terms must match)")
    search.add_argument("--q", required=True, help="Words to look for")
    search.add_argument("--limit", type=int, default=10, help="Max results (default 10)")
    search.set_defaults(func=cmd_search)

    listing = sub.add_parser("list", parents=[common], help="Newest notes first")
    listing.add_argument("--subject", default="", help="Only this subject")
    listing.add_argument("--tag", default="", help="Only notes with this tag")
    listing.add_argument("--limit", type=int, default=20, help="Max results (default 20)")
    listing.set_defaults(func=cmd_list)

    stats = sub.add_parser("stats", parents=[common], help="Counts, size and subjects")
    stats.set_defaults(func=cmd_stats)

    forget = sub.add_parser("forget", parents=[common], help="Delete one note (needs --yes)")
    forget.add_argument("--id", type=int, required=True, help="Note id from list/search")
    forget.add_argument("--yes", action="store_true", help="Confirm the deletion")
    forget.set_defaults(func=cmd_forget)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if not hasattr(args, "json"):
        args.json = False
    if not hasattr(args, "store") or args.store is None:
        args.store = None
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
