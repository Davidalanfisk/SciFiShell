<#
 Builds the installer from build\Release and optionally publishes it as a private GitHub release.
   .\installer\build-installer.ps1 -Version 0.2.0            # installer\Output\SciFiShell-Setup-0.2.0.exe
   .\installer\build-installer.ps1 -Version 0.2.0 -Publish   # ...and gh release create v0.2.0 (the updater picks it up)
#>
param(
    [Parameter(Mandatory)][string]$Version,
    [switch]$Publish
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$rel = Join-Path $root 'build\Release'
$stage = Join-Path $PSScriptRoot 'stage'
if (-not (Test-Path (Join-Path $rel 'SciFiShell.exe'))) { throw "Build first: cmake --build build --config Release --target SciFiShell" }

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item $stage -ItemType Directory | Out-Null
robocopy $rel $stage /E /XF *.lib *.pdb *.log *.exp *.ilk /NFL /NDL /NJH /NJS /NP | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'update.ps1') $stage
Set-Content (Join-Path $stage 'version.txt') $Version -NoNewline

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup not found: winget install -e --id JRSoftware.InnoSetup" }
& $iscc "/DAppVersion=$Version" (Join-Path $PSScriptRoot 'SciFiShell.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed" }
$out = Join-Path $PSScriptRoot "Output\SciFiShell-Setup-$Version.exe"
Write-Host "Installer: $out"

if ($Publish) {
    gh release create "v$Version" $out --title "SciFi Shell $Version" --generate-notes
}
