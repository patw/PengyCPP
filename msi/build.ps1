# Build the PengyCPP MSI from the windeployqt'd app folder.
#
#   msi\build.ps1 -AppDir Pengy-Windows -Version 1.9.3 -Out Pengy-Windows-v1.9.3.msi
#
# Needs the WiX v5 CLI and its UI extension (v5 is pinned on purpose: v6
# changed the licence terms for the binaries):
#   dotnet tool install --global wix --version 5.0.2
#   wix extension add -g WixToolset.UI.wixext/5.0.2
param(
    [Parameter(Mandatory)] [string] $AppDir,
    # MSI versions are major.minor.build only (CMakeLists.txt's project VERSION).
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $Out
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$appDir = (Resolve-Path $AppDir).Path

# WixUI_Minimal shows the license and only takes RTF, so render LICENSE
# (plain ASCII) as the simplest RTF that displays it.
$license = Get-Content (Join-Path $repo 'LICENSE') -Raw
$license = $license.Replace('\', '\\').Replace('{', '\{').Replace('}', '\}')
$license = $license -replace "\r?\n", "\par`r`n"
$rtf = Join-Path ([IO.Path]::GetTempPath()) 'pengycpp-license.rtf'
"{\rtf1\ansi\deff0{\fonttbl{\f0\fswiss Segoe UI;}}\f0\fs18 $license}" |
    Set-Content -Encoding Ascii $rtf

wix build (Join-Path $PSScriptRoot 'pengy.wxs') -arch x64 -ext WixToolset.UI.wixext `
    -bindpath "app=$appDir" -d "Version=$Version" -d "LicenseRtf=$rtf" -o $Out
if ($LASTEXITCODE) { throw "wix build failed ($LASTEXITCODE)" }

# ICE38/ICE64/ICE91 flag components under the user profile that are keyed on
# files rather than HKCU values. That is inherent to harvesting the whole
# windeployqt folder into %LocalAppData%\Programs, which is the point of a
# per-user install; CI's install/uninstall round trip checks the real effect.
wix msi validate -sice ICE38 -sice ICE64 -sice ICE91 $Out
if ($LASTEXITCODE) { throw "MSI validation failed ($LASTEXITCODE)" }
