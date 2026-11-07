$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$temp = Join-Path ([IO.Path]::GetTempPath()) ("scifishell-updater-test-" + [guid]::NewGuid())
New-Item -ItemType Directory $temp | Out-Null
try {
    $script = Get-Content (Join-Path $root 'installer\update.ps1') -Raw
    if (-not $script.Contains('Add-Type -AssemblyName System.Windows.Forms') -or
        -not $script.Contains('[Windows.Forms.MessageBox]::Show') -or
        -not $script.Contains('try {')) { throw 'Updater test hooks no longer match the script' }
    $stub = @'
Add-Type -TypeDefinition @"
using System;
using System.IO;
public static class TestMessages {
    public static string Show(string message, string title, string buttons, string icon) {
        File.AppendAllText(Environment.GetEnvironmentVariable("SCIFI_TEST_DIALOGS"), title + ": " + message + "\n");
        return "No";
    }
}
"@
function Invoke-RestMethod {
    param($Uri, $Headers)
    if ($env:SCIFI_TEST_MODE -eq 'failure') { throw 'Simulated network failure' }
    $assets = @()
    if ($env:SCIFI_TEST_MODE -ne 'missing') {
        $assets = @(@{ name = 'SciFiShell-Setup-0.9.19.exe'; browser_download_url = 'https://example.invalid/installer.exe' })
    }
    return @{ tag_name = 'v0.9.19'; assets = $assets }
}
function Invoke-WebRequest { throw 'Unexpected download after declining update' }
function Start-Process { throw 'Unexpected installer launch after declining update' }
'@
    $script = $script.Replace('Add-Type -AssemblyName System.Windows.Forms', $stub).
        Replace('[Windows.Forms.MessageBox]::Show', '[TestMessages]::Show')
    $fixture = Join-Path $temp 'update.ps1'
    Set-Content -Path $fixture -Value $script -Encoding UTF8
    $env:SCIFI_TEST_DIALOGS = Join-Path $temp 'dialogs.txt'
    $cases = @(
        @{ version = '0.9.18'; mode = 'normal'; interactive = $false; exit = 0; contains = 'SciFi Shell 0.9.19 is available (you have 0.9.18)' },
        @{ version = '0.9.19'; mode = 'normal'; interactive = $true; exit = 0; contains = 'SciFi Shell 0.9.19 is up to date.' },
        @{ version = '0.9.20'; mode = 'normal'; interactive = $false; exit = 0; contains = '' },
        @{ version = '0.9.18'; mode = 'missing'; interactive = $true; exit = 1; contains = 'Release 0.9.19 has no installer attached.' },
        @{ version = '0.9.18'; mode = 'failure'; interactive = $true; exit = 1; contains = 'Simulated network failure' }
    )
    foreach ($case in $cases) {
        Set-Content -Path (Join-Path $temp 'version.txt') -Value $case.version -NoNewline
        if (Test-Path $env:SCIFI_TEST_DIALOGS) { Remove-Item $env:SCIFI_TEST_DIALOGS }
        $env:SCIFI_TEST_MODE = $case.mode
        $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $fixture)
        if ($case.interactive) { $args += '-Interactive' }
        & powershell.exe @args
        if ($LASTEXITCODE -ne $case.exit) { throw "Unexpected exit for $($case.version) / $($case.mode): $LASTEXITCODE" }
        $dialog = if (Test-Path $env:SCIFI_TEST_DIALOGS) { Get-Content $env:SCIFI_TEST_DIALOGS -Raw } else { '' }
        if ($case.contains -eq '' -and $dialog -ne '') { throw "Unexpected dialog: $dialog" }
        if ($case.contains -ne '' -and -not $dialog.Contains($case.contains)) { throw "Missing dialog: $($case.contains); got $dialog" }
        Write-Host "PASS $($case.version) / $($case.mode)"
    }
} finally {
    Remove-Item Env:SCIFI_TEST_MODE, Env:SCIFI_TEST_DIALOGS -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $temp -Recurse -Force
}
