# Windows only: build SciFiShell with SCIFI_TEST_SYNTHETIC_CITY=ON in a separate
# build directory, then pass that executable and an existing artifact directory.
# This exercises a simulated focus gate on an inactive desktop, not physical input.
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [switch]$AlphaOnly
)

$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'This isolated-desktop integration test requires Windows.' }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class GameplayTestDesktop {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct Startup { public int cb; public string reserved,desktop,title; public int x,y,xSize,ySize,xChars,yChars,fill,flags; public short show,reserved2; public IntPtr reservedPtr,stdin,stdout,stderr; }
    [StructLayout(LayoutKind.Sequential)]
    struct Process { public IntPtr process,thread; public int pid,tid; }
    [StructLayout(LayoutKind.Sequential)]
    struct Security { public int length; public IntPtr descriptor; public int inherit; }
    [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
    static extern IntPtr CreateDesktop(string name,IntPtr device,IntPtr mode,int flags,uint access,IntPtr security);
    [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
    static extern bool CreateProcess(string app,StringBuilder cmd,IntPtr pa,IntPtr ta,bool inherit,uint flags,IntPtr env,string cwd,ref Startup si,out Process pi);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr handle,uint ms);
    [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr handle,out uint code);
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr handle,uint code);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
    static extern IntPtr CreateFile(string path,uint access,uint share,ref Security security,uint creation,uint flags,IntPtr template);
    public static int Run(string exe,string log,string cwd,uint timeout) {
        string name = "SciFiGameplayTest_" + Guid.NewGuid().ToString("N");
        IntPtr desktop = CreateDesktop(name,IntPtr.Zero,IntPtr.Zero,0,0x01ff,IntPtr.Zero);
        if (desktop == IntPtr.Zero) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        try {
            Security security = new Security { length = Marshal.SizeOf(typeof(Security)), inherit = 1 };
            IntPtr output = CreateFile(log,0x40000000,1,ref security,2,0x80,IntPtr.Zero);
            if (output == new IntPtr(-1)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            try {
                Startup si = new Startup(); si.cb = Marshal.SizeOf(typeof(Startup)); si.desktop = name;
                si.flags = 0x100; si.stdout = output; si.stderr = output;
                Process pi;
                if (!CreateProcess(exe,new StringBuilder("\"" + exe + "\""),IntPtr.Zero,IntPtr.Zero,true,0x08000000,IntPtr.Zero,cwd,ref si,out pi))
                    throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                try {
                    if (WaitForSingleObject(pi.process,timeout) != 0) {
                        TerminateProcess(pi.process,99);
                        WaitForSingleObject(pi.process,5000);
                        return 99;
                    }
                    uint code;
                    if (!GetExitCodeProcess(pi.process,out code)) throw new System.ComponentModel.Win32Exception();
                    return (int)code;
                } finally { CloseHandle(pi.thread); CloseHandle(pi.process); }
            } finally { CloseHandle(output); }
        } finally { CloseDesktop(desktop); }
    }
}
'@

$exe = (Resolve-Path -LiteralPath $Executable).Path
$exeText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($exe))
if (!$exeText.Contains('Synthetic city integration test build; not for normal use')) {
    throw 'Refusing to launch an executable without the synthetic-city test marker.'
}
$root = (Resolve-Path -LiteralPath $OutputDirectory).Path
$run = Join-Path $root ('gameplay-control-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
$fixtures = Join-Path $PSScriptRoot 'fixtures'

function New-FixtureZip([string]$source, [string]$zip) {
    if (Test-Path -LiteralPath $source -PathType Leaf) {
        Copy-Item -LiteralPath $source -Destination $zip
    } else {
        $archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            $source = (Resolve-Path -LiteralPath $source).Path.TrimEnd('\')
            Get-ChildItem -LiteralPath $source -File -Recurse | ForEach-Object {
                $entry = $_.FullName.Substring($source.Length + 1).Replace('\', '/')
                if ($entry -eq 'plugin.json' -or $entry -eq 'scripts/main.lua' -or
                    $entry -match '^assets/[a-z0-9_-]+\.(png|glb|wav|ogg)$') {
                    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $entry) | Out-Null
                }
            }
        } finally { $archive.Dispose() }
    }
}

function Run-Scenario([string]$name, [string]$source, [string[]]$commands, [string[]]$additionalSources = @()) {
    $dir = Join-Path $run $name
    New-Item -ItemType Directory -Path $dir | Out-Null
    $profile = Join-Path $dir 'profile'
    New-Item -ItemType Directory -Path $profile | Out-Null
    $zip = Join-Path $dir 'fixture.zip'
    New-FixtureZip $source $zip
    $number = 2
    foreach ($extra in $additionalSources) {
        $extraZip = Join-Path $dir ("fixture$number.zip")
        New-FixtureZip $extra $extraZip
        $token = "{ZIP$number}"
        $commands = $commands | ForEach-Object { $_.Replace($token, $extraZip) }
        $number++
    }
    $script = Join-Path $dir 'control.txt'
    $commands = $commands | ForEach-Object { $_.Replace('{ZIP}', $zip) }
    Set-Content -LiteralPath $script -Value $commands -Encoding ASCII
    $log = Join-Path $dir 'app.log'
    $saved = @{}
    Get-ChildItem Env: | Where-Object {
        $_.Name -like 'SCIFI_*' -or $_.Name -in @('LOCALAPPDATA', 'APPDATA', 'USERPROFILE', 'HOME', 'SDL_AUDIODRIVER')
    } | ForEach-Object { $saved[$_.Name] = $_.Value }
    try {
        $env:LOCALAPPDATA = $profile
        $env:APPDATA = $profile
        $env:USERPROFILE = $profile
        $env:HOME = $profile
        $env:SDL_AUDIODRIVER = 'dummy'
        Get-ChildItem Env: | Where-Object { $_.Name -like 'SCIFI_*' } |
            ForEach-Object { Remove-Item -LiteralPath ("Env:\" + $_.Name) }
        $env:SCIFI_NO_UPDATE = '1'
        $env:SCIFI_SCRIPT = $script
        if ($AlphaOnly) { $env:SCIFI_TEST_HEADLESS = '1' }
        $timeout = if ($AlphaOnly) { 30000 } else { 60000 }
        $code = [GameplayTestDesktop]::Run($exe, $log, (Split-Path -Parent $exe), $timeout)
    } finally {
        Get-ChildItem Env: | Where-Object {
            $_.Name -like 'SCIFI_*' -or $_.Name -in @('LOCALAPPDATA', 'APPDATA', 'USERPROFILE', 'HOME', 'SDL_AUDIODRIVER')
        } | ForEach-Object { Remove-Item -LiteralPath ("Env:\" + $_.Name) }
        foreach ($key in $saved.Keys) { Set-Item -LiteralPath ("Env:\" + $key) -Value $saved[$key] }
    }
    if ($code -ne 0) { throw "$name exited $code; see $log" }
    if (!(Select-String -Quiet -LiteralPath $log -Pattern 'Synthetic city integration test build')) {
        throw "Not a synthetic-city test executable; see $log"
    }
    $control = Join-Path $profile 'SciFiShell\control.log'
    if (!(Test-Path -LiteralPath $control)) { throw "$name produced no control log; see $log" }
    return @(Get-Content -LiteralPath $control)
}

if ($AlphaOnly) {
    $sender = Join-Path $run 'alpha-sender'
    Copy-Item -LiteralPath (Join-Path $fixtures 'alpha-sender') -Destination $sender -Recurse
    $assets = Join-Path $sender 'assets'
    New-Item -ItemType Directory -Path $assets | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\examples\plugins\audio-demo\assets\tone.ogg') -Destination $assets
    $wav = [System.IO.BinaryWriter]::new([System.IO.File]::Create((Join-Path $assets 'tone.wav')))
    try {
        $wav.Write([System.Text.Encoding]::ASCII.GetBytes('RIFF'))
        $wav.Write([uint32](36 + 3200))
        $wav.Write([System.Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
        $wav.Write([uint32]16); $wav.Write([uint16]1); $wav.Write([uint16]1)
        $wav.Write([uint32]16000); $wav.Write([uint32]32000)
        $wav.Write([uint16]2); $wav.Write([uint16]16)
        $wav.Write([System.Text.Encoding]::ASCII.GetBytes('data'))
        $wav.Write([uint32]3200)
        for ($i = 0; $i -lt 1600; $i++) {
            if (($i % 40) -lt 20) { $wav.Write([int16]4000) } else { $wav.Write([int16]-4000) }
        }
    } finally { $wav.Dispose() }
    $alpha = Run-Scenario 'alpha' $sender @(
        'wait 2', 'waitcity', 'plugininstall {ZIP2}',
        'pluginclick ALPHA RECEIVER', 'pluginclick ALPHA RECEIVER', 'settingsclose',
        'wait 1', 'testgamehud com.example.alpha-receiver',
        'plugininstall {ZIP}', 'pluginclick ALPHA SENDER', 'pluginclick ALPHA SENDER',
        'settingsclose', 'wait 3', 'testgamehud com.example.alpha-receiver',
        'testsamples com.example.alpha-sender', 'gameplaystatus com.example.alpha-sender',
        'pluginclick DISABLE ALPHA SENDER', 'wait 0.5',
        'testsamples com.example.alpha-sender', 'gameplaystatus com.example.alpha-sender',
        'pluginerror', 'quit'
    ) @((Join-Path $fixtures 'alpha-receiver'))
    $joined = $alpha -join "`n"
    if ($joined -notmatch 'test hud WAITING' -or
        $joined -notmatch 'test hud MESSAGE RECEIVED' -or
        $joined -notmatch 'test samples com\.example\.alpha-sender=2' -or
        $joined -notmatch 'test samples com\.example\.alpha-sender=0' -or
        $joined -notmatch 'gameplay com\.example\.alpha-sender running=1' -or
        $joined -notmatch 'gameplay com\.example\.alpha-sender running=0') {
        throw "Alpha host routing/audio scenario failed; see $run\alpha\profile\SciFiShell\control.log"
    }
    Write-Host "Alpha host routing and packaged audio passed: $run"
    return
}

$common = @('wait 2', 'waitcity', 'cam 0 1.7 110 0 0', 'fly off', 'wait 1',
            'plugininstall {ZIP}')
$success = Run-Scenario 'success' (Join-Path $fixtures 'gameplay-control') ($common + @(
    'pluginclick CONTROL FIXTURE', 'pluginnotice', 'pluginclick CONTROL FIXTURE',
    'pluginnotice', 'settingsclose', 'wait 2', 'testclearance com.example.control-fixture', 'testfocus 1', 'testcontrol',
    'testkey g', 'wait 0.6', 'testcontrol', 'testkey escape', 'wait 0.4',
    'testcontrol', 'testkey g', 'wait 0.5', 'testcontrol', 'testfocus 0', 'wait 0.4',
    'testcontrol', 'testfocus 1', 'testkey g', 'wait 0.4', 'testcontrol',
    'pluginclick DISABLE CONTROL FIXTURE', 'wait 0.4', 'testcontrol',
    'gameplaystatus com.example.control-fixture', 'pluginnotice', 'quit'))
$joined = $success -join "`n"
$states = @($success | Where-Object { $_ -like 'test control owner=*' })
if ($states.Count -ne 7 -or
    !(($states[0] -match 'owner=0') -and ($states[1] -match 'owner=1') -and
      ($states[2] -match 'owner=0') -and ($states[3] -match 'owner=1') -and
      ($states[4] -match 'owner=0') -and ($states[5] -match 'owner=1') -and
      ($states[6] -match 'owner=0')) -or
    $joined -notmatch 'ALLOW SCENE, CITY COLLISION, PLAYER/CAMERA CONTROL' -or
    $joined -notmatch 'plugin notice PLUGIN ENABLED' -or
    $joined -notmatch 'plugin notice PLUGIN DISABLED' -or
    $joined -notmatch 'gameplay com.example.control-fixture running=0') {
    throw "Control scenario failed; see $run\success\profile\SciFiShell\control.log"
}
if ($states[1] -notmatch 'pos=([0-9.]+),') { throw 'Missing controlled camera position' }
$position = [double]::Parse($Matches[1], [System.Globalization.CultureInfo]::InvariantCulture)
if ($position -le 0.2) { throw "Controlled camera did not move: $position" }
$clearance = @($success | Where-Object { $_ -like 'test clearance host=*' })
if ($clearance.Count -ne 1 -or $clearance[0] -notmatch '^test clearance host=([0-9.]+) script=([0-9.]+)$') {
    throw "Missing track clearance result; see $run\success\profile\SciFiShell\control.log"
}
$hostHeight = [double]::Parse($Matches[1], [System.Globalization.CultureInfo]::InvariantCulture)
$scriptHeight = [double]::Parse($Matches[2], [System.Globalization.CultureInfo]::InvariantCulture)
if ($hostHeight -le 0 -or $hostHeight -ne $scriptHeight) {
    throw "Script and host track clearance differ: $($clearance[0])"
}

$failure = Run-Scenario 'failure' (Join-Path $fixtures 'gameplay-control-failure') ($common + @(
    'pluginclick CONTROL FAILURE', 'pluginnotice', 'pluginclick CONTROL FAILURE',
    'pluginnotice', 'settingsclose', 'wait 2', 'testfocus 1', 'testkey g',
    'wait 0.6', 'testcontrol', 'gameplaystatus com.example.control-failure',
    'pluginerror', 'quit'))
$joined = $failure -join "`n"
if ($joined -notmatch 'test control owner=0' -or
    $joined -notmatch 'gameplay com.example.control-failure running=0' -or
    $joined -notmatch "plugin error com.example.control-failure: .*missing_callback") {
    throw "Callback failure scenario failed; see $run\failure\profile\SciFiShell\control.log"
}
$exampleOutput = Join-Path $run 'examples'
& python (Join-Path $PSScriptRoot '..\examples\plugins\build-gameplay-examples.py') $exampleOutput | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Failed to build gameplay example packages' }
$car = Run-Scenario 'car' (Join-Path $exampleOutput 'scripted-car-track.zip') ($common + @(
    'pluginclick SCRIPTED CAR TRACK', 'pluginnotice', 'pluginclick SCRIPTED CAR TRACK',
    'pluginnotice', 'settingsclose', 'wait 2', 'testfocus 1',
    'gameplaystatus com.example.car-track', 'testgotooffer com.example.car-track',
    'testaudio com.example.car-track', 'testkey g', 'wait 0.6', 'testcontrol',
    'testaudio com.example.car-track', 'testkey escape', 'wait 0.4',
    'testcontrol', 'testaudio com.example.car-track',
    'testgotooffer com.example.car-track', 'testkey g', 'wait 0.4',
    'testaudio com.example.car-track', 'pluginclick DISABLE SCRIPTED CAR TRACK', 'pluginnotice',
    'wait 0.4', 'testaudio com.example.car-track', 'testcontrol',
    'gameplaystatus com.example.car-track', 'pluginerror', 'quit'))
$joined = $car -join "`n"
$audio = @($car | Where-Object { $_ -like 'test audio engine=*' })
if ($audio.Count -ne 5 -or $audio[0] -ne 'test audio engine=0' -or
    $audio[1] -ne 'test audio engine=1' -or $audio[2] -ne 'test audio engine=0' -or
    $audio[3] -ne 'test audio engine=1' -or $audio[4] -ne 'test audio engine=0' -or
    $joined -notmatch 'test offer reached' -or
    $joined -notmatch 'gameplay com.example.car-track running=1 boxes=36 models=1' -or
    $joined -notmatch 'gameplay com.example.car-track running=0' -or
    $joined -notmatch 'test control owner=1' -or $joined -notmatch 'test control owner=0' -or
    $joined -notmatch 'SOUND EFFECTS' -or
    $joined -notmatch 'plugin notice PLUGIN ENABLED' -or $joined -notmatch 'plugin notice PLUGIN DISABLED') {
    throw "Packaged car scenario failed; see $run\car\profile\SciFiShell\control.log"
}
$parks = Run-Scenario 'parks' (Join-Path $exampleOutput 'scripted-parks-toys.zip') ($common + @(
    'pluginclick SCRIPTED PARKS AND TOYS', 'pluginnotice', 'pluginclick SCRIPTED PARKS AND TOYS',
    'pluginnotice', 'settingsclose', 'wait 3', 'gameplaystatus com.example.parks-toys',
    'testfocus 1', 'testcontrol', 'testclick down', 'wait 0.3', 'testgamehud com.example.parks-toys', 'testwindow',
    'testclick up', 'wait 0.3', 'testclick down', 'wait 0.3', 'testgamehud com.example.parks-toys',
    'testclick up', 'wait 6', 'testgamehud com.example.parks-toys',
    'pluginclick DISABLE SCRIPTED PARKS AND TOYS', 'pluginnotice',
    'gameplaystatus com.example.parks-toys', 'pluginclick SCRIPTED PARKS AND TOYS',
    'pluginnotice', 'settingsclose', 'wait 2', 'gameplaystatus com.example.parks-toys',
    'pluginerror', 'quit'))
$joined = $parks -join "`n"
$running = @($parks | Where-Object { $_ -match '^gameplay com\.example\.parks-toys running=1 boxes=([0-9]+) models=([0-9]+)' })
if ($running.Count -ne 2 -or $joined -notmatch 'plugin notice PLUGIN ENABLED' -or
    $joined -notmatch 'plugin notice PLUGIN DISABLED' -or
    $joined -notmatch 'gameplay com.example.parks-toys running=0 boxes=0 models=0' -or
    $joined -notmatch 'CITY LAYOUT' -or $joined -notmatch 'PLAYER PUSH' -or
    $joined -notmatch 'test hud CLICK TO THROW hotdog' -or $joined -notmatch 'test hud SCORE ' -or
    $joined -notmatch 'test window focus=0') {
    throw "Packaged parks and toys scenario failed; see $run\parks\profile\SciFiShell\control.log"
}
foreach ($line in $running) {
    if ($line -notmatch 'boxes=([0-9]+) models=([0-9]+)' -or
        [int]$Matches[1] -lt 2 -or [int]$Matches[2] -lt 1) {
        throw "Packaged parks and toys did not render its stands and duck: $line"
    }
}
$farZip = Join-Path $run 'far.zip'
$archive = [System.IO.Compression.ZipFile]::Open($farZip, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $farSource = Join-Path $fixtures 'gameplay-click-far'
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, (Join-Path $farSource 'plugin.json'), 'plugin.json') | Out-Null
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, (Join-Path $farSource 'scripts\main.lua'), 'scripts/main.lua') | Out-Null
} finally { $archive.Dispose() }
$clicks = Run-Scenario 'clicks' (Join-Path $fixtures 'gameplay-click-near') ($common + @(
    ('plugininstall ' + $farZip),
    'pluginclick NEAR TARGET', 'pluginclick NEAR TARGET',
    'pluginclick FAR TARGET', 'pluginclick FAR TARGET',
    'settingsclose', 'wait 2', 'testfocus 1', 'testwindow',
    'testclick down', 'wait 0.3', 'testgamehud com.example.z-near',
    'testgamehud com.example.a-far', 'testwindow',
    'testclick down', 'wait 0.3', 'testgamehud com.example.z-near', 'testwindow',
    'testfocus 0', 'wait 0.3', 'testgamehud com.example.z-near',
    'testfocus 1', 'testclick down', 'wait 0.3', 'testgamehud com.example.z-near',
    'testclick up', 'wait 0.3', 'testgamehud com.example.z-near',
    'cam 14 1.7 110 0 0', 'testclick down', 'wait 0.3', 'testwindow', 'testclick up',
    'pluginerror', 'quit'))
$joined = $clicks -join "`n"
$windows = @($clicks | Where-Object { $_ -like 'test window focus=*' })
if ($joined -notmatch 'test hud NEAR PRESS 1' -or
    ([regex]::Matches($joined, 'test hud NEAR PRESS 1')).Count -ne 2 -or
    $joined -notmatch 'test hud FAR READY' -or $joined -match 'test hud FAR PRESS' -or
    $joined -notmatch 'test hud NEAR RELEASE 1' -or
    $joined -notmatch 'test hud NEAR PRESS 2' -or $joined -notmatch 'test hud NEAR RELEASE 2' -or
    $windows.Count -ne 4 -or $windows[0] -ne 'test window focus=0' -or
    $windows[1] -ne 'test window focus=0' -or $windows[2] -ne 'test window focus=0' -or
    $windows[3] -ne 'test window focus=1') {
    throw "Click routing scenario failed; see $run\clicks\profile\SciFiShell\control.log"
}
$clickFailure = Run-Scenario 'click-failure' (Join-Path $fixtures 'gameplay-click-failure') ($common + @(
    'pluginclick CLICK FAILURE', 'pluginclick CLICK FAILURE',
    'settingsclose', 'wait 2', 'testfocus 1',
    'testclick down', 'wait 0.3', 'gameplaystatus com.example.click-failure', 'pluginerror', 'testwindow',
    'testclick down', 'wait 0.3', 'testwindow', 'testclick up', 'wait 0.3',
    'testclick down', 'wait 0.3', 'testwindow', 'testclick up', 'quit'))
$joined = $clickFailure -join "`n"
$windows = @($clickFailure | Where-Object { $_ -like 'test window focus=*' })
if ($joined -notmatch 'gameplay com.example.click-failure running=0 boxes=0 models=0' -or
    $joined -notmatch 'plugin error com.example.click-failure: .*missing_callback' -or
    $windows.Count -ne 3 -or $windows[0] -ne 'test window focus=0' -or
    $windows[1] -ne 'test window focus=0' -or $windows[2] -ne 'test window focus=1') {
    throw "Click failure cleanup scenario failed; see $run\click-failure\profile\SciFiShell\control.log"
}
Write-Output "Synthetic control integration passed. Artifacts: $run"
