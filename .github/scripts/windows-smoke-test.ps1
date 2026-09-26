# Smoke-test a built or installed pengy.exe on the Windows runner: catches a
# missing Qt/MSVC DLL, a broken WinMain entry point, or a console-subsystem exe.
param([Parameter(Mandatory)] [string] $Exe)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
Write-Host "Smoke testing $Exe"

# PE header: Subsystem 2 = Windows GUI (no console), 3 = console.
$bytes = [IO.File]::ReadAllBytes($Exe)
$pe = [BitConverter]::ToInt32($bytes, 0x3C)
$subsystem = [BitConverter]::ToUInt16($bytes, $pe + 0x5C)
Write-Host "PE subsystem: $subsystem"
if ($subsystem -ne 2) { throw "pengy.exe is not a GUI-subsystem exe (subsystem $subsystem)" }

# --version exits before any window is created, but the process still has to
# load every linked DLL to get there.
$out = Join-Path $env:RUNNER_TEMP 'pengy-version.txt'
$p = Start-Process -FilePath $Exe -ArgumentList '--version' -Wait -NoNewWindow -PassThru `
    -RedirectStandardOutput $out
$version = Get-Content $out -Raw
Write-Host "--version: $version"
if ($p.ExitCode -ne 0 -or $version -notmatch 'Pengy v') { throw "--version failed (exit $($p.ExitCode))" }

# Full launch against a throwaway config dir: the Qt platform plugin and the
# rest of windeployqt's output must load for it to stay up.
$cfg = Join-Path $env:RUNNER_TEMP 'pengy-smoke-config'
New-Item -ItemType Directory -Force $cfg | Out-Null
$p = Start-Process -FilePath $Exe -ArgumentList '--config-dir', "`"$cfg`"" -PassThru
Start-Sleep -Seconds 8
if ($p.HasExited) { throw "pengy.exe exited early with code $($p.ExitCode)" }
Stop-Process -Id $p.Id -Force
$p.WaitForExit()  # release the exe's file lock before anything uninstalls it
Write-Host "GUI stayed up for 8s"
