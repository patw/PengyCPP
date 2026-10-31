# User Profile — interview the human, then keep the notes

Who am I talking to? This skill is the answer to that question, and the reason
it is worth five minutes on the first session: everything else gets better when
the agent knows the person — the timezone behind "tonight", the machine behind
"my laptop", the tone they actually want.

The profile lives in `profile.md`, next to this file. Plain markdown, theirs to
edit, no database.

## When to use this skill

- First run, or `profile.md` still contains `<!-- TODO -->` markers.
- The user says "update my profile", "remember this about me", "interview me".
- Before a task that depends on preferences (scheduling, tone, hardware,
  credentials layouts) and the profile does not answer it.

## The contract

**Interview, do not fill in guesses.** Never invent a name, a city, a job, a
preference, or a machine. An empty field is honest; a wrong one is worse than
nothing and will be believed forever.

1. Read `profile.md`. Keep every answer that is already there; you are filling
   gaps, not starting over.
2. Ask **one question at a time**, conversationally. Batch nothing — a wall of
   eight questions gets abandoned.
3. Say why a question is worth answering ("so I can stop asking what timezone
   you're in") and offer a skip. `/skip` or "dunno" moves on immediately.
4. Prefer closed questions with an easy default ("Linux desktop — is that
   right?") over open ones.
5. Write the file after **each** answer, not at the end: the session may be
   interrupted, and the user should see progress. Remove the `<!-- TODO -->`
   markers you resolved, keep the rest.
6. Finish by updating the `Last updated` line with today's date and showing the
   user what changed in one short sentence. Never paste the whole file back at
   them.

Ask the sections in this order — it moves from least sensitive to most:

| Order | Section | Why it is asked |
|-------|---------|-----------------|
| 1 | Name / handle | What to call them |
| 2 | Location + timezone | "tonight", "tomorrow", weather, scheduling |
| 3 | What they do | Framing, examples, reference points |
| 4 | Tech setup | OS, editor, languages, machines on the network |
| 5 | Preferences | Length, tone, formality, how much to ask before acting |
| 6 | Current projects | What to offer help with unprompted |

Optional, only if they volunteer it: interests, hardware, preferred long-form
output format.

## Rules

- **This file is not a transcript.** A page is plenty. "Prefers terse answers,
  no preamble" belongs here; a story about a bad meeting does not.
- **Never send it anywhere.** No uploads, no pasting into a chat completion
  unless the content is relevant to the task at hand.
- **It may be synced.** On a fleet with Syncthing it travels to every machine;
  if the user wants it local-only, say so before writing sensitive detail.
- **They own it.** If they edit it by hand, their edit wins, silently. Re-read
  before assuming.
- If the user would rather not be interviewed at all, write one line —
  `Prefers not to profile.` — and never bring it up again.
