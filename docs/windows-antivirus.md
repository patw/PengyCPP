# Windows Defender, SmartScreen, and antivirus false positives

PengyCPP is an **agent harness**: it hands an LLM a set of tools that run PowerShell,
write and replace files, download URLs, and walk your filesystem. Windows' built-in
protection is built to be suspicious of exactly that behaviour. When it decides to
act, it rarely asks first — it **force-closes the process** and **quarantines the
executable**.

If a session vanished mid-task, PengyCPP is gone from the Start menu, or a reinstall
"works" for a few seconds and then the app is missing again, this page is for you.

> **Reinstalling on its own will not fix it.** Defender deletes the same bytes
> again as soon as the installer writes them. Exclude the program folder
> **first**, then restore or reinstall.

This is not a code defect and it is not something PengyCPP can work around: the only
supported fix is a Windows security exclusion. It is also a **false positive** —
PengyCPP hides nothing, installs no background service, and talks only to the LLM
endpoint you configure. All three editions (Python Pengy,
PengyR, PengyCPP) present the same tool surface and therefore attract the same
verdict — the paths differ, the procedure does not.

---

## What the symptom looks like

| What you see | What actually happened |
|---|---|
| The window closed with no error, no crash dialog, no traceback | The process was terminated by Defender, not by PengyCPP |
| The Start Menu entry is gone, or does nothing when clicked | Its target `pengy.exe` no longer exists, so Windows dropped or broke the shortcut |
| Reinstall appears to succeed, then the app dies again on launch or on the first tool call | The freshly written executable was quarantined again |
| The app runs but stops as soon as it runs a command or edits files | Behaviour monitoring (not a file signature) blocked it |
| Nothing in PengyCPP's own logs | It was killed from outside; there is no in-app record to find |

Nothing in your chats, settings or tasks is lost. The installer never touches
`%APPDATA%\pengy\`.

---

## Why it happens

- **The tool surface is the threat model.** `run_powershell`, `run_bash`,
  `write_file`, `apply_changes`, `download_file` and `directory_tree` compose into
  a program that spawns shells, rewrites files in bulk and reads the disk. That is
  a description of a dropper, and heuristics cannot read intent.
- **The binaries are unsigned and have no download reputation.** A per-user
  install into `%LocalAppData%\Programs\...` is also the shape of commodity
  malware delivery.
- **The verdicts are probabilistic.** Machine-learning and behaviour detections
  (names ending in `!ml`, or beginning `Behavior:`) are not deterministic; the same
  build can be clean on one machine and quarantined on another, and a rebuild with
  no behaviour change can flip the verdict. That is why one user is hit and the
  next is not — and why "it worked yesterday" is normal.
- **Notification may be off.** With notifications suppressed or the toast missed,
  the first evidence is an app that simply disappears.

---

## Step 1 — Confirm it was Defender

Before changing anything, check the record:

**Windows Security UI**

1. Open **Windows Security** → **Virus & threat protection**.
2. Click **Protection history**.
3. Look for a recent item naming `pengy.exe`, the install folder, or the portable
   copy you ran. Expand it — the detection name and the action taken
   (*Quarantined*, *Removed*, *Blocked*) are shown there.

**Event log** — *Event Viewer → Applications and Services Logs → Microsoft →
Windows → Windows Defender → Operational*. Detections are **1116**; the action
taken is **1117**.

**PowerShell**

```powershell
# Recent detections, newest first
Get-MpThreatDetection |
  Sort-Object InitialDetectionTime -Descending |
  Select-Object -First 10 InitialDetectionTime, ThreatID, Resources, ActionSuccess

# What each ThreatID was
Get-MpThreat | Select-Object ThreatID, ThreatName, SeverityID, Resources
```

**Check the file is really gone**

```powershell
Test-Path "$env:LOCALAPPDATA\Programs\PengyCPP\pengy.exe"
```

`Get-ChildItem "$env:LOCALAPPDATA\Programs\PengyCPP"` showing the folder but not
`pengy.exe` is the classic picture.

If Protection history is empty **and** `pengy.exe` is still there, this is not
Defender: look in *Event Viewer → Windows Logs → Application* for an application
error, and check any third-party antivirus you run (it has its own console and
its own exclusions list — the same steps apply there).

---

## Step 2 — Exclude the program folder (do this first)

Excluding the folder **before** restoring stops Defender from eating the file
again the moment it reappears.

**Windows Security UI**

1. **Windows Security** → **Virus & threat protection** → **Manage settings**.
2. Scroll to **Exclusions** → **Add or remove exclusions** → **Add an exclusion**.
   (Windows will ask for elevation.)
3. Add **Folder** → `%LOCALAPPDATA%\Programs\PengyCPP`
   (%LOCALAPPDATA% is `C:\Users\<you>\AppData\Local`).
4. Add **Process** → `pengy.exe` as a belt-and-braces second entry.

**The same thing in PowerShell, from an elevated prompt**

```powershell
Add-MpPreference -ExclusionPath "$env:LOCALAPPDATA\Programs\PengyCPP"
Add-MpPreference -ExclusionProcess "pengy.exe"

# Verify
Get-MpPreference | Select-Object -ExpandProperty ExclusionPath
Get-MpPreference | Select-Object -ExpandProperty ExclusionProcess
```

To undo it later:

```powershell
Remove-MpPreference -ExclusionPath "$env:LOCALAPPDATA\Programs\PengyCPP"
Remove-MpPreference -ExclusionProcess "pengy.exe"
```

> **Exclude narrowly.** A folder exclusion is recursive and applies to everything
> inside it, so keep it to the program folder that holds `pengy.exe` and the Qt
> DLLs. Never exclude your profile (`C:\Users\<you>`), your whole drive, or
> `%APPDATA%` — that blinds Defender to everything else you do. If you ran the
> portable ZIP, exclude the folder you extracted it into.

---

## Step 3 — Restore the file, or reinstall

With the exclusion in place, either restore what was quarantined or reinstall —
both work; restoring is faster.

**Restore from Protection history**

**Windows Security** → **Virus & threat protection** → **Protection history** →
expand the detection → **Actions** → **Restore**. If the card offers
**Allow on device**, take it — that also clears the standing verdict for that file.

**Restore from the command line** (elevated)

```powershell
& "$env:ProgramFiles\Windows Defender\MpCmdRun.exe" -Restore -Name "<ThreatName>" -All
```

Use the `ThreatName` reported by `Get-MpThreat` in Step 1 — not the ThreatID.
`-All` restores every quarantined copy of that threat; drop it to restore only
the most recent one. Note that `MpCmdRun.exe` lives under
`C:\ProgramData\Microsoft\Windows Defender\Platform\<version>\` on some builds.

**Or reinstall**

Run the MSI again (installs are per-user, no admin prompt), or re-extract the
portable ZIP. This recreates both `pengy.exe` and the Start Menu shortcut. It is
the only way back if the quarantined copy cannot be restored.

**If the Start Menu entry is still missing** after `pengy.exe` is back, reinstall
once more — the shortcut is an MSI component, and repairing or reinstalling
recreates it. PengyCPP's shortcut points at
`%LOCALAPPDATA%\Programs\PengyCPP\pengy.exe`.

---

## Step 4 — If the installer or ZIP was blocked instead

A browser download carries Mark-of-the-Web, and that is a separate check from
the on-disk scan.

- **"Windows protected your PC"** → **More info** → **Run anyway**.
- **"This file came from another computer and might be blocked"** → right-click
  the `.msi`/`.zip` → **Properties** → tick **Unblock** → **OK**. Or:
  ```powershell
  Unblock-File .\Pengy-Windows-<version>.msi
  Unblock-File .\Pengy-Windows-<version>.zip
  ```
- **Extract the ZIP before running.** Launching `pengy.exe` from inside the
  Explorer ZIP preview runs it from a temp folder, where a detection deletes the
  copy you cannot see and leaves the ZIP looking fine.
- **Windows 11 Smart App Control** blocks unsigned, low-reputation executables
  outright. Check *Windows Security → App & browser control → Smart App Control*.
  Turning it off is a one-way change (it cannot be re-enabled without reinstalling
  Windows), so treat that as a last resort and prefer the exclusions above.

---

## Step 5 — If it comes back, or you are on a managed machine

- **Re-check the exclusion list.** A Defender "restore defaults", a Windows
  feature update, or a third-party antivirus taking over as the primary engine can
  clear or bypass exclusions. `Get-MpPreference` shows what is actually in force.
- **The portable build and the MSI are different paths.** If you use both, exclude
  both folders, or you will fix one and lose the other.
- **Work or school machine?** Exclusions are usually controlled by policy and the
  Security page will be greyed out. Ask IT to allow the executable — by install
  path or by file hash — or to add it to the antivirus's allow-list. Defender for
  Endpoint customers should submit it as an **indicator allow** in the portal.
- **Report the false positive.** Submitting the file to Microsoft
  (https://www.microsoft.com/en-us/wdsi/filesubmission, "Software developer" →
  false positive) is what eventually clears the verdict for everyone, and is worth
  doing even after your own machine is working.

---

## Reducing exposure instead of (or as well as) excluding

PengyCPP's tools are genuinely powerful, and an exclusion means Defender is no
longer watching that one folder. Two habits cost little and shrink the blast
radius:

- **Keep tool confirmation at `All` or `Safe`**, not `None`, at least until you
  trust the model you are talking to. `Safe` auto-approves read-only tools and
  asks before anything that writes or executes.
- **Treat skills and prompts as code.** A skill is a markdown file plus optional
  scripts that the agent will run; a pasted "do this for me" instruction is the
  same thing by another route. Review before you install, and do not run a prompt
  from a source you would not run a script from.

For experiments you do not trust, use a VM or **Windows Sandbox** rather than
exclusions on your daily machine.

---

## What PengyCPP will never ask you to do

PengyCPP does not disable Defender, edit security policy, or install certificates.
If a message, page or agent claiming to be Pengy asks you to **turn off real-time
protection**, that is not us, and it is not the fix — exclusions are. The app also
never needs to run elevated: the MSI is per-user and `run_powershell` reports steps
that require admin back to you instead of attempting them.
