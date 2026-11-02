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
                         [--supersedes ID] [--force]
python3 memory.py search --q "…" [--limit 10] [--all]
python3 memory.py list   [--subject NAME] [--tag TAG] [--limit 20] [--all]
python3 memory.py stats
python3 memory.py forget --id N --yes
```

(`./memory.py` also works — installing the starter skills restores the execute
bit, which a wheel would otherwise drop. `python3` is the portable form.)

| Flag | Meaning |
|------|---------|
| `--json` | Machine-readable output. Accepted **before or after** the subcommand (`memory.py --json list` and `memory.py list --json` both work). `search` adds a `score`. |
| `--store PATH` | Use another store file. Also `$PENGY_MEMORY_FILE`. |
| `--all` | Include notes that have been superseded. They are hidden by default. |
| `--yes` | Required to make `forget` actually delete. |

## Searching: ask in your own words

`search` ranks notes with BM25 over the text, the subject and the tags. **Any
word can match, more matching words rank higher, and an exact phrase lifts a
note further** — so you do not have to remember the note's wording:

```
$ python3 memory.py search --q "why do the gui tests need a display"
#4  2026-10-31  pengy: UI tests need QT_QPA_PLATFORM=offscreen; xvfb is not needed
#2  2026-10-31  pengy: UI tests are broken under xvfb; use a real display for now
```

Plurals and tenses meet each other (`test`/`tests`, `deploy`/`deployed`),
common words like *the* and *how* are ignored, and identifiers stay searchable
both whole and in parts — `client_max_body_size` is found by `max body size`.
Subjects and tags count for more than a mention buried in a sentence.

Exit codes are for shell chains: **0** when something matched, **1** when
nothing did, **2** for a malformed query. So `search --q … && act` still means
"only act if memory had something".

A miss means *this store* has nothing — not that the fact is unknown. Say
"nothing in my notes" rather than guessing, and write the answer down once you
have it.

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

Adding a note whose text already exists (ignoring case and spacing) stores
nothing and tells you which id it is; `--force` keeps a second copy anyway.

## Corrections: supersede, don't rewrite

Never edit an existing note in place. Add a new one and point it at the old:

```bash
python3 memory.py add --subject pengy --tags tests \
  --text "UI tests need the offscreen flag; xvfb is no longer used" \
  --supersedes 4
```

The old note keeps its history (and its mistakes), the new one wins, and the
retired note stops appearing in `search`/`list` — it is still there for `--all`,
labelled `(superseded by #5)`. That matters because a stale note is worse than
no note: it gets believed. `stats` counts them under `superseded`.

`forget --id N --yes` is for notes that are simply wrong or private — not for
disagreeing with them. It deletes one note, writes `memory.jsonl.bak` first,
and leaves the rest of the file (including any hand-broken line) untouched.

## Parallel use is safe

`add` and `forget` take an exclusive lock, so two agents writing at the same
moment cannot mint the same id or overwrite each other's notes. Notes are
appended and fsynced individually, never rewritten in place, except by
`forget` and `--supersedes` (which rewrite the whole file, atomically).

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

`stats` once in a while: notes, size, subjects, how many are superseded. A few
hundred notes is nothing (they are a few hundred KB, and a search is instant).
At about five thousand notes a search still takes roughly a tenth of a second,
because every query re-reads and re-tokenises the file. That is the point to
stop adding and move on, not to keep going.

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

What the upgrade buys is not "search works" — it is a memory **several machines
and several agents write to at once**, with an author on every note, a
machine-readable lifecycle (`status`, `superseded_by`), dedupe before posting,
tag browsing, usage analytics and a web UI a human can read. What it costs is a
service to run, a 400 MB embedding model, a database and its backups.

A rough guide to when the file stops being enough:

| Symptom | The file's limit |
|---|---|
| Two agents on two machines "remember" different things | nothing is shared without a synced folder |
| "Who wrote this note, and is it still true?" | no author, no status beyond `--supersedes` |
| A question uses words no note contains ("why does my laptop keep dying") | lexical search, no semantics |
| You want to see what memory has been *useless* | no access tracking |

Until then, the file is enough, and it never fails to start.
