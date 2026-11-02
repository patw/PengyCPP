#!/usr/bin/env python3
"""A flat, greppable memory store — append, ranked search, list, forget.

One JSON object per line in a text file. No service, no database, no
embeddings, no API key. It exists because "write down what you learned" is the
one habit nearly every agent needs and almost none start with.

``search`` is BM25 over the note text, subject and tags, with a little stemming,
so a question phrased in your own words finds the note: ask "why does https keep
failing on a revoked certificate" and the note about truststore still comes back
first. Matching *any* word is enough to be found; matching more of them ranks
higher; an exact phrase lifts a note further.

    ./memory.py add --text "Pengy's UI tests need QT_QPA_PLATFORM=offscreen"
    ./memory.py search --q "why do the gui tests need a display"
    ./memory.py --json list --limit 5

Store location, in order: ``--store``, ``$PENGY_MEMORY_FILE``, then
``memory.jsonl`` next to this script.
"""
from __future__ import annotations

import argparse
import contextlib
import json
import math
import os
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

DEFAULT_FILENAME = "memory.jsonl"

# BM25 constants. k1 saturates term frequency, b normalizes by note length.
# The values are the usual defaults and are not tuned to any particular corpus.
K1 = 1.2
B = 0.75

# How much a hit in the subject or a tag is worth relative to the note text.
# Both are deliberate classification, so they should outrank an incidental
# mention in prose.
FIELD_WEIGHTS = {"text": 1.0, "subject": 1.5, "tags": 1.5}

# Words that carry no retrieval signal. Dropped from queries AND from the index
# so a sentence-shaped query — "how do the tests run" — is searched as
# "tests run" instead of requiring the notes to contain the word "how".
STOPWORDS = frozenset("""
a about above after again against all am an and any are aren't as at be
because been before being below between both but by can can't cannot could
couldn't did didn't do does doesn't doing don't down during each few for from
further had hadn't has hasn't have haven't having he her here hers herself him
himself his how i i'm if in into is isn't it it's its itself just let's me
more most mustn't my myself no nor not of off on once only or other ought our
ours ourselves out over own same shan't she should shouldn't so some such than
that that's the their theirs them themselves then there there's these they
this those through to too under until up very was wasn't we were weren't what
what's when where which while who whom why will with won't would wouldn't you
your yours yourself yourselves
""".split())

# A token is a run of letters/digits, optionally joined by _ . : / - so
# identifiers and paths survive as one term ("QT_QPA_PLATFORM", "10.0.23.6",
# "client_max_body_size"). The parts are indexed too, so "max body size" finds
# the identifier anyway.
TOKEN_RE = re.compile(r"[a-z0-9]+(?:[_.:/-][a-z0-9]+)*")
SPLIT_RE = re.compile(r"[_.:/-]")


def store_path(override: str | None = None) -> Path:
    if override:
        return Path(override).expanduser()
    env = os.environ.get("PENGY_MEMORY_FILE")
    if env:
        return Path(env).expanduser()
    return Path(__file__).resolve().parent / DEFAULT_FILENAME


# ── reading and writing ─────────────────────────────────────────────────────

def load_lines(path: Path) -> list[tuple[dict | None, str]]:
    """Return every line as ``(record_or_None, raw_text)``.

    A line that does not parse keeps its raw text so that rewriting the store
    (forget, supersede) preserves it verbatim instead of silently dropping a
    hand-edit.
    """
    if not path.is_file():
        return []
    out: list[tuple[dict | None, str]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        try:
            record = json.loads(stripped)
        except json.JSONDecodeError:
            out.append((None, stripped))
            continue
        if isinstance(record, dict) and "id" in record:
            out.append((record, stripped))
        else:
            out.append((None, stripped))
    return out


def load(path: Path) -> list[dict]:
    return [record for record, _ in load_lines(path) if record is not None]


@contextlib.contextmanager
def locked(path: Path):
    """Hold an exclusive lock on ``<store>.lock`` for a read-modify-write.

    Without this, two notes written at the same instant both compute
    ``max(id)+1`` from the same snapshot and land on the same id — and then a
    later ``forget --id N`` takes out both. Agents call tools in parallel, so
    this is the normal path, not an edge case. Searches take no lock: they read
    a snapshot and a torn final line is skipped by the parser.
    """
    lock_path = path.with_suffix(path.suffix + ".lock")
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    handle = open(lock_path, "a+", encoding="utf-8")
    try:
        try:
            import fcntl
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        except (ImportError, OSError):  # non-POSIX: best effort, no lock
            pass
        yield
    finally:
        try:
            import fcntl
            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        except (ImportError, OSError):
            pass
        handle.close()


def append_line(path: Path, record: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(record, ensure_ascii=False) + "\n")
        fh.flush()
        os.fsync(fh.fileno())


def rewrite(path: Path, lines: list[tuple[dict | None, str]]) -> None:
    """Replace the store atomically, keeping unparseable lines as they were."""
    text = "".join(
        (json.dumps(record, ensure_ascii=False) if record is not None else raw) + "\n"
        for record, raw in lines
    )
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w", encoding="utf-8") as fh:
        fh.write(text)
        fh.flush()
        os.fsync(fh.fileno())
    tmp.replace(path)


def next_id(records: list[dict]) -> int:
    return max((int(r["id"]) for r in records), default=0) + 1


def _now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


# ── text processing ─────────────────────────────────────────────────────────

def terms(text: str) -> list[str]:
    """Tokens for indexing/searching: normalized, split, stopworded, stemmed."""
    out: list[str] = []
    for token in TOKEN_RE.findall(str(text).lower()):
        candidates = [token]
        if SPLIT_RE.search(token):
            candidates.extend(part for part in SPLIT_RE.split(token) if part)
        for candidate in candidates:
            if candidate in STOPWORDS:
                continue
            out.append(stem(candidate))
    return out


def stem(word: str) -> str:
    """A crude suffix stripper — enough to match test/tests, run/runs, deploy/deployed.

    Deliberately not a full Porter stemmer: it only has to make the plural and
    past-tense forms of ordinary English meet each other, and identifiers
    (anything with a digit or underscore) are left alone.
    """
    if len(word) < 4 or not word.isalpha():
        return word
    if word.endswith("ies") and len(word) > 4:
        return word[:-3] + "y"
    for suffix in ("ing", "edly", "ed", "es", "s"):
        if word.endswith(suffix) and len(word) - len(suffix) >= 3:
            if suffix == "s" and word.endswith("ss"):
                break
            return word[: -len(suffix)]
    return word


def query_terms(q: str) -> list[str]:
    """Unique query terms, order preserved, so 'the the' is one term."""
    seen, out = set(), []
    for term in terms(q):
        if term not in seen:
            seen.add(term)
            out.append(term)
    return out


# ── ranking ─────────────────────────────────────────────────────────────────

def rank(records: list[dict], query: str) -> list[tuple[dict, float]]:
    """Score notes against *query* with BM25, best first.

    Term frequency is accumulated across the weighted fields, document length is
    the same weighted sum, and idf is computed over the store as it stands.
    Notes that match more of the query are lifted by a coverage multiplier, and
    an exact phrase gets one more nudge — so a verbatim quotation wins over a
    scattering of the same words.
    """
    wanted = query_terms(query)
    if not wanted or not records:
        return []

    docs = []
    for record in records:
        fields = {
            "text": terms(record.get("text", "")),
            "subject": terms(record.get("subject", "")),
            "tags": terms(" ".join(record.get("tags") or [])),
        }
        tf: dict[str, float] = {}
        length = 0.0
        for name, tokens in fields.items():
            weight = FIELD_WEIGHTS[name]
            length += weight * len(tokens)
            for token in tokens:
                tf[token] = tf.get(token, 0.0) + weight
        # Tokenized once, reused for the phrase check below — tokenizing the
        # whole store twice per query doubles the cost of a search.
        haystack = " ".join(fields["text"] + fields["subject"] + fields["tags"])
        docs.append((record, tf, length, haystack))

    total = len(docs)
    avgdl = sum(length for _, _, length, _ in docs) / total or 1.0
    df: dict[str, int] = {}
    for _, tf, _, _ in docs:
        for token in tf:
            df[token] = df.get(token, 0) + 1

    phrase = " ".join(wanted)
    scored: list[tuple[dict, float]] = []
    for record, tf, length, haystack in docs:
        matched, score = 0, 0.0
        for term in wanted:
            freq = tf.get(term)
            if not freq:
                continue
            matched += 1
            idf = math.log(1 + (total - df[term] + 0.5) / (df[term] + 0.5))
            score += idf * (freq * (K1 + 1)) / (
                freq + K1 * (1 - B + B * length / avgdl))
        if not matched:
            continue
        if phrase in haystack:
            score *= 1.25
        scored.append((record, score * (1 + 0.5 * matched / len(wanted))))

    # Score first; among equals, the newer note wins — a correction should beat
    # the note it corrects. Two stable passes: sort by recency, then by score,
    # and the recency order survives every tie. Rounding keeps float noise from
    # reordering notes that a user would call equally relevant.
    scored.sort(key=lambda pair: (str(pair[0].get("ts", "")), pair[0]["id"]), reverse=True)
    scored.sort(key=lambda pair: -round(pair[1], 9))
    return scored


# ── output ──────────────────────────────────────────────────────────────────

def _line(record: dict) -> str:
    tags = f"  [{', '.join(record['tags'])}]" if record.get("tags") else ""
    subject = f"{record['subject']}: " if record.get("subject") else ""
    retired = record.get("superseded_by")
    note = f"  (superseded by #{retired})" if retired else ""
    return f"#{record['id']}  {record['ts'][:10]}  {subject}{record['text']}{tags}{note}"


def _print(records: list[dict], as_json: bool, scores: dict[int, float] | None = None) -> None:
    if as_json:
        out = []
        for record in records:
            enriched = dict(record)
            if scores and record["id"] in scores:
                enriched["score"] = round(scores[record["id"]], 4)
            out.append(enriched)
        print(json.dumps(out, indent=2, ensure_ascii=False))
        return
    if not records:
        print("(nothing found)")
        return
    for record in records:
        print(_line(record))


# ── commands ────────────────────────────────────────────────────────────────

def cmd_add(args) -> int:
    path = store_path(args.store)
    text = args.text.strip()
    if not text:
        print("error: --text is empty", file=sys.stderr)
        return 2

    with locked(path):
        lines = load_lines(path)
        records = [record for record, _ in lines if record is not None]

        # The same sentence stored twice is a retrieval liability, not a second
        # opinion: both copies come back, and neither is newer in any useful
        # sense. Say so instead of quietly appending.
        squashed = " ".join(text.lower().split())
        for existing in records:
            if " ".join(str(existing.get("text", "")).lower().split()) == squashed:
                if not args.force:
                    message = {
                        "stored": None,
                        "duplicate_of": existing["id"],
                        "store": str(path),
                    }
                    if args.json:
                        print(json.dumps(message, ensure_ascii=False))
                    else:
                        print(f"Already stored as #{existing['id']} — nothing added "
                              f"(use --force to keep a second copy).")
                    return 0

        record = {
            "id": next_id(records),
            "ts": _now(),
            "text": text,
            "subject": args.subject.strip(),
            "tags": [t.strip() for t in (args.tags or "").split(",") if t.strip()],
        }
        if args.supersedes is not None:
            record["supersedes"] = args.supersedes

        append_line(path, record)

        retired = None
        if args.supersedes is not None:
            target = args.supersedes
            # Re-read: the note we just appended has to survive this rewrite.
            lines = load_lines(path)
            touched = False
            for index, (entry, _raw) in enumerate(lines):
                if entry is not None and int(entry["id"]) == target:
                    entry["superseded_by"] = record["id"]
                    lines[index] = (entry, "")
                    touched = True
            if touched:
                rewrite(path, lines)
                retired = target
            else:
                print(f"warning: no note #{target} to supersede", file=sys.stderr)

    if args.json:
        print(json.dumps({"stored": record, "supersedes": retired, "store": str(path)},
                         ensure_ascii=False))
    else:
        print(f"Stored note #{record['id']} in {path}")
        if retired is not None:
            print(f"  (note #{retired} is now marked superseded)")
    return 0


def cmd_search(args) -> int:
    path = store_path(args.store)
    if not query_terms(args.q):
        print("error: --q has no searchable words", file=sys.stderr)
        return 2
    records = [r for r in load(path) if args.all or not r.get("superseded_by")]
    hits = rank(records, args.q)[: args.limit]
    _print([record for record, _ in hits], args.json,
           scores={record["id"]: score for record, score in hits})
    return 0 if hits else 1


def cmd_list(args) -> int:
    path = store_path(args.store)
    records = [r for r in load(path) if args.all or not r.get("superseded_by")]
    if args.subject:
        wanted = args.subject.lower()
        records = [r for r in records if wanted in str(r.get("subject", "")).lower()]
    if args.tag:
        wanted = args.tag.lower()
        records = [r for r in records
                   if wanted in [t.lower() for t in (r.get("tags") or [])]]
    records = sorted(records, key=lambda r: (str(r.get("ts", "")), int(r["id"])),
                     reverse=True)
    _print(records[: args.limit], args.json)
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
        "superseded": sum(1 for r in records if r.get("superseded_by")),
        "bytes": path.stat().st_size if path.is_file() else 0,
        "oldest": min((r["ts"] for r in records), default=None),
        "newest": max((r["ts"] for r in records), default=None),
        "subjects": subjects,
    }
    if args.json:
        print(json.dumps(stats, indent=2, ensure_ascii=False))
    else:
        print(f"store:   {stats['store']}")
        print(f"notes:   {stats['notes']}  ({stats['bytes']} bytes)"
              + (f", {stats['superseded']} superseded" if stats["superseded"] else ""))
        if stats["oldest"]:
            print(f"range:   {stats['oldest'][:10]} .. {stats['newest'][:10]}")
        for key, count in sorted(subjects.items(), key=lambda kv: -kv[1]):
            print(f"  {count:4d}  {key}")
    return 0


def cmd_forget(args) -> int:
    path = store_path(args.store)
    with locked(path):
        lines = load_lines(path)
        matching = [record for record, _ in lines
                    if record is not None and int(record["id"]) == args.id]
        if not matching:
            print(f"error: no note with id {args.id}", file=sys.stderr)
            return 1
        # Delete the record the user is looking at (newest first, matching the
        # order `list` and `search` print), not merely the first line with that
        # id — a store written by an older copy of this script can hold two.
        victim = max((index for index, (record, _) in enumerate(lines)
                      if record is not None and int(record["id"]) == args.id),
                     key=lambda index: (str(lines[index][0].get("ts", "")), index))
        if not args.yes:
            print(f"refusing to delete note #{args.id} without --yes "
                  f"(text: {lines[victim][0]['text']})", file=sys.stderr)
            return 2

        backup = path.with_suffix(path.suffix + ".bak")
        if path.is_file():
            backup.write_bytes(path.read_bytes())

        kept = [pair for index, pair in enumerate(lines) if index != victim]
        rewrite(path, kept)
        remaining = len([r for r, _ in kept if r is not None])
        if len(matching) > 1:
            print(f"warning: id {args.id} appeared {len(matching)} times; "
                  f"{len(matching) - 1} other note(s) still carry it",
                  file=sys.stderr)

    if args.json:
        print(json.dumps({"deleted": args.id, "remaining": remaining,
                          "backup": str(backup)}))
    else:
        print(f"Deleted note #{args.id}; {remaining} left (backup: {backup.name}).")
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
    add.add_argument("--supersedes", type=int, default=None, metavar="ID",
                     help="Mark an older note as replaced by this one")
    add.add_argument("--force", action="store_true",
                     help="Store even if an identical note already exists")
    add.set_defaults(func=cmd_add)

    search = sub.add_parser("search", parents=[common],
                            help="Ranked search; any word can match, more words rank higher")
    search.add_argument("--q", required=True, help="What you're looking for, in your own words")
    search.add_argument("--limit", type=int, default=10, help="Max results (default 10)")
    search.add_argument("--all", action="store_true",
                        help="Include notes that have been superseded")
    search.set_defaults(func=cmd_search)

    listing = sub.add_parser("list", parents=[common], help="Newest notes first")
    listing.add_argument("--subject", default="", help="Only this subject")
    listing.add_argument("--tag", default="", help="Only notes with this tag")
    listing.add_argument("--limit", type=int, default=20, help="Max results (default 20)")
    listing.add_argument("--all", action="store_true",
                         help="Include notes that have been superseded")
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
