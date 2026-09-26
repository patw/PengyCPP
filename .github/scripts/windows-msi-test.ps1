# Install -> upgrade -> uninstall round trip for the PengyCPP MSI on the Windows
# runner. $NextMsi is the same app built with a bumped version, so the upgrade
# step exercises <MajorUpgrade> the way a real release-to-release update does.
param(
    [Parameter(Mandatory)] [string] $Msi,
    [Parameter(Mandatory)] [string] $NextMsi
)
$ErrorActionPreference = 'Stop'
$upgradeCode = '{DAAB9CF7-E498-4411-95F3-7F144BE04162}'  # must match msi\pengy.wxs
$installDir = Join-Path $env:LOCALAPPDATA 'Programs\PengyCPP'
$exe = Join-Path $installDir 'pengy.exe'
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'PengyCPP.lnk'
$installer = New-Object -ComObject WindowsInstaller.Installer

function Invoke-Msiexec([string] $Action, [string] $Package, [string] $Name) {
    $log = Join-Path $env:RUNNER_TEMP "msi-$Name.log"
    $pkg = (Resolve-Path $Package).Path
    $p = Start-Process msiexec.exe -ArgumentList $Action, "`"$pkg`"", '/qn', '/l*v', "`"$log`"" -Wait -PassThru
    if ($p.ExitCode -notin 0, 3010) {
        Get-Content $log -Tail 80
        throw "msiexec $Action $Package failed (exit $($p.ExitCode))"
    }
}

# WindowsInstaller.Installer has no type info PowerShell can bind to, so its
# (parameterized) properties are read through InvokeMember.
function Get-ComProperty($Object, [string] $Name, [object[]] $Arguments) {
    $Object.GetType().InvokeMember($Name, [Reflection.BindingFlags]::GetProperty, $null, $Object, $Arguments)
}

# Installed products sharing PengyCPP's UpgradeCode, with their versions.
# PowerShell enumerates the returned StringList itself, so @() of the call is
# the list of product codes (empty when nothing is installed).
function Get-Installed {
    $codes = @(Get-ComProperty $installer 'RelatedProducts' @($upgradeCode))
    $found = @()
    foreach ($code in $codes) {
        $found += [pscustomobject]@{
            Code    = $code
            Version = Get-ComProperty $installer 'ProductInfo' @($code, 'VersionString')
        }
    }
    , $found
}

Write-Host '== Install'
Invoke-Msiexec '/i' $Msi 'install'
if (-not (Test-Path $exe)) { throw "pengy.exe not installed at $exe" }
if (-not (Test-Path $shortcut)) { throw "Start menu shortcut missing: $shortcut" }
$installed = Get-Installed
$installed | Format-Table | Out-String | Write-Host
if ($installed.Count -ne 1) { throw "expected 1 installed PengyCPP, found $($installed.Count)" }
$firstVersion = $installed[0].Version
$fileCount = (Get-ChildItem $installDir -Recurse -File).Count
Write-Host "Installed $fileCount files to $installDir"
& (Join-Path $PSScriptRoot 'windows-smoke-test.ps1') -Exe $exe

Write-Host '== Upgrade'
Invoke-Msiexec '/i' $NextMsi 'upgrade'
$installed = Get-Installed
$installed | Format-Table | Out-String | Write-Host
if ($installed.Count -ne 1) { throw "upgrade left $($installed.Count) PengyCPP entries installed" }
if ($installed[0].Version -eq $firstVersion) { throw "upgrade did not change the installed version ($firstVersion)" }
if (-not (Test-Path $exe)) { throw "pengy.exe missing after upgrade" }

Write-Host '== Uninstall'
Invoke-Msiexec '/x' $NextMsi 'uninstall'
$installed = Get-Installed
if ($installed.Count -ne 0) { throw "uninstall left $($installed.Count) PengyCPP entries installed" }
if (Test-Path $shortcut) { throw "Start menu shortcut left behind" }
if (Test-Path $installDir) {
    Get-ChildItem $installDir -Recurse | Select-Object -ExpandProperty FullName
    throw "install folder left behind: $installDir"
}
Write-Host 'Install, upgrade and uninstall all clean'
