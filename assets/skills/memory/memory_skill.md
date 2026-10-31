# Memory — write it down, look it up

A flat text file of one-line facts, plus `memory.py` to add and find them. That
is the whole skill: no database, no embeddings, no service, no API key, nothing
to start. The value is the **habit**, not the tool — search before asking, and
record what you'd otherwise have to rediscover.

`memory.jsonl`, next to this file, is the store. One JSON object per line, so
`grep` works and a corrupt line costs one note, not the file.

## When to use it

- **Before asking** the user something they may already have told you — a
  preference, a hostname, which machine runs what, which test command works.
- **After learning** something durable: a decision and its reason, a fix, a
  caveat, an environment quirk, where a file lives.
- **When starting a task** that seems familiar: `search` the topic first. Past
  you may have left the answer.

Do not use it as a transcript. If you would not want the note read back in six
months, it does not belong in the store.

## Commands

```
python3 memory.py add    --text "…" [--subject NAME] [--tags a,b]
python3 memory.py search --q "words" [--limit 10]
python3 memory.py list   [--subject NAME] [--tag TAG] [--limit 20]
python3 memory.py stats
python3 memory.py forget --id N --yes
```

(`./memory.py` also works — installing the starter skills restores the execute
bit, which a wheel would otherwise drop. `python3` is the portable form.)

| Flag | Meaning |
|------|---------|
| `--json` | Machine-readable output. Accepted **before or after** the subcommand (`memory.py --json list` and `memory.py list --json` both work). |
| `--store PATH` | Use another store file. Also `$PENGY_MEMORY_FILE`. |
| `--yes` | Required to make `forget` actually delete. |

`search` matches **all** words, case-insensitively, across text, subject and
tags, newest first. It exits 1 when nothing matches, so `search && act` works.

## What a good note looks like

One fact per note, specific enough to act on, and the reason attached:

```bash
python3 memory.py add --subject pengy --tags tests \
  --text "UI tests need QT_QPA_PLATFORM=offscreen and env -u XDG_DATA_HOME; conftest refuses a real XDG_DATA_HOME"
```

- **Specific beats tidy.** A path, a command, a version, a `file:line`.
- **Say why**, not just what. "Uses truststore because certifi kept a revoked
  root" is a note; "uses truststore" is trivia.
- **Date lives in `ts`**, so don't repeat it in the text.
- **One note per fact.** Three related facts are three notes — they get found
  independently.

## Corrections, not edits

Never rewrite an existing note in place. Add a new one that says what changed:

```bash
python3 memory.py add --subject pengy --text "Supersedes #12: the offscreen flag is only needed for GUI tests, not the CLI suite"
```

The old note keeps its history (and its mistakes), the new one wins by being
newer, and `search` shows both. `forget --id N --yes` is for notes that are
simply wrong or private — not for disagreeing with them.

## What never goes in

- **Secrets.** No keys, tokens, passwords, cookies — not even "temporarily".
  Record *where* a secret lives, never its value.
- **Other people's personal data** that the user has no reason to keep.
- **Transcripts.** "User said hi, then I said hi" is not memory.
- **Guesses.** A note that turns out false is worse than a gap: it will be
  believed later.

If this skills folder is synced between machines, your memory is too — which is
usually the point, but say so before storing anything host-specific. Point
`--store` somewhere local to keep it off the sync.

## Housekeeping

`stats` once in a while: notes, size, subjects. A few hundred notes is nothing
(they are a few hundred KB). If the store grows past a few thousand, the
upgrade path is a real index — see below — not a bigger file.

## Growing up: from a file to a shared memory bus

The habits above transfer unchanged to a server-backed memory; only the store
moves. If the user has one — the fleet behind this skill uses **BotTalk**
(`mem.catbee.ca`, a REST memory board with semantic search, tags and a web UI) —
then:

- The client side is small: an HTTP wrapper plus a key (`BOTTALK_API_KEY`).
- **The server side is not**: BotTalk is a separate process with a database,
  embedding model and web front end. It is an operator deployment, not
  something a skills folder can install. If the user wants one, that is a
  project, not a button.
- Ask before sending anything to a remote service. A local file is private by
  default; a shared bus is not.

Until then, the file is enough, and it never fails to start.
