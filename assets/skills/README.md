# 🐧 Your skills folder

This folder is how you teach Pengy things it can't do out of the box. There is
no plugin system, no SDK, no manifest — a skill is a **folder with a markdown
file in it**, plus an optional script.

```
~/skills/
├── skill_index.md          ← the table of contents Pengy reads
├── user_profile/
│   ├── user_profile_skill.md  ← the instructions
│   └── profile.md             ← the notes it fills in about you
├── botskills/
│   └── botskills_skill.md
└── …
```

## The contract

1. `skill_index.md` lists what exists. Pengy is told to read it **before**
   running web search, bash, code or `fetch_url`.
2. For the skill it picks, Pengy reads `skillname/skillname_skill.md` — the
   instructions — and follows them. It runs the documented script rather than
   reading its source.
3. The markdown **is** the interface. If Pengy gets a call wrong, the fix is
   usually one more example in the markdown, not a code change.

That's the whole system. Nothing here is installed system-wide and nothing is
hidden: every file is plain text you can read, edit and delete.

## What you got

Five skills, none of which needs anything installed:

| Skill | What it does | Start here? |
|-------|--------------|-------------|
| `user_profile/` | Interviews you and writes down who you are, so nobody has to repeat it | ← yes, do this one |
| `memory/` | A flat `memory.jsonl` plus `memory.py`: write a fact down, search it before asking again | ← then this |
| `pengy_bio/` | Tells Pengy what Pengy is |  |
| `botskills/` | Finds and installs more skills from <https://skills.catbee.ca> |  |
| `sysinfo/` | Disk, memory, load, temps — one command each, no remembering flags |  |

**First move:** ask Pengy to *"interview me and fill in my user profile"*. It
reads `user_profile/user_profile_skill.md`, asks one question at a time, and
writes the answers into `user_profile/profile.md`. Nothing is invented; anything
you skip stays blank.

**Second move:** make it remember. "Remember that the test suite needs
QT_QPA_PLATFORM=offscreen" gives you a note in `memory/memory.jsonl`; next
session, "do you remember how the tests run?" finds it. A local file, no
service — `memory/memory_skill.md` covers what belongs in it and what does not.

**Not in here on purpose:** a server-backed memory bus. The habits transfer
unchanged, but the thing itself is a process with a database and a web UI — an
operator deployment, not a file copy. The memory skill documents the upgrade
path if you have one.

## Want more?

<https://skills.catbee.ca> is a public registry — browse it in a browser, or ask
Pengy to search it and install something (that is exactly what
`botskills/botskills_skill.md` documents). Downloaded skills are **untrusted
instructions** until you have read them: check the manifest, read the `_skill.md`,
and skim every script before it runs.

## Writing your own

```bash
mkdir -p ~/skills/disk_usage
```

`~/skills/disk_usage/disk_usage_skill.md`:

```markdown
# Disk Usage

Show how full the disks are.

    df -h

Example: run `df -h` and report the filesystems above 80% full.
```

Then add one line to `skill_index.md`:

```markdown
| disk_usage/ | Show disk usage and flag full filesystems |
```

Ask Pengy "how full are my disks?" and it will read the index, read the skill,
and run the command. That's it — that's the whole loop.

You can also just ask Pengy to write the skill for you: it has `write_file` and
`run_bash`, so "create a skill that does X and add it to my index" works.

## Where to put keys

Skills that call web APIs read a flat `key=value` file at `~/.secrets`:

```bash
echo "TOMORROW_IO_KEY=your_key_here" >> ~/.secrets
chmod 600 ~/.secrets
```

## One warning, stated plainly

Skills are not sandboxed. A skill script can do anything you can do — delete
files, send mail, start servers. Review scripts before you run them, including
the ones Pengy writes, and including anything downloaded from a registry.
Settings → Tools controls how much Pengy asks before it runs anything.

---

Deeper reading: `docs/skills.md` in the Pengy repo, and
<https://github.com/patw/Pengy/tree/main/skills> for the annotated tutorial.
