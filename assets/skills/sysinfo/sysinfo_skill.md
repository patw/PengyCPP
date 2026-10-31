# Sysinfo — the machine, in one command

Zero dependencies, read-only, works on any Linux box. These are the commands
worth having in a skill precisely because nobody remembers the flags at the
moment they need them.

Report only what was asked; do not dump every command's output at once.

## Disk

```bash
df -h -x tmpfs -x devtmpfs            # usage per filesystem
du -sh ~/Downloads                    # how big is that folder
du -xh --max-depth=1 ~ | sort -h | tail -10   # what is eating home
```

Flag anything at or above **80% used**, and name the biggest consumer.

## Memory, load, uptime

```bash
free -h
uptime
ps -eo pid,comm,%cpu,%mem --sort=-%mem | head -10
```

`free -h`'s "available" column is the one that matters — "free" being small on
Linux is normal (page cache).

## Temperature and fans

```bash
sensors                     # needs lm-sensors; absent on minimal installs
nvidia-smi --query-gpu=name,temperature.gpu,memory.used,memory.total --format=csv
```

Skip silently if the command is not installed; say so rather than guessing.

## System identity

```bash
hostnamectl                 # OS, kernel, hostname, virtualization
uname -srm
cat /etc/os-release
```

## Running services

```bash
systemctl --user --failed
systemctl --failed          # system units, may need sudo to be complete
```

## Rules

- **Read-only.** Nothing here changes state; if the user asks for a fix
  (kill the process, clear the cache), that is a different, confirmed action.
- Prefer one command that answers the question over a wall of raw output.
- Quote the number, not the screenshot: "`/` is at 84% (12G free of 78G)".
