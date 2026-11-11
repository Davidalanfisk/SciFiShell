# Windows only: build SciFiShell with SCIFI_TEST_SYNTHETIC_CITY=ON in a separate
# build directory, then pass that executable and an existing artifact directory.
# This exercises a simulated focus gate on an inactive desktop, not physical input.
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [switch]$AlphaOnly,
    [switch]$MotionOnly,
    [switch]$FlightInputsOnly,
    [switch]$MouseInputsOnly,
    [switch]$ControlOnly,
    [string]$TrackFlyZip
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
if ($MouseInputsOnly) {
    $source = Join-Path $run 'mouse-input-source'
    Copy-Item -LiteralPath (Join-Path $fixtures 'gameplay-control') -Destination $source -Recurse
    $manifestPath = Join-Path $source 'plugin.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifest.permissions += @('hud')
    Set-Content -LiteralPath $manifestPath -Value ($manifest | ConvertTo-Json -Depth 4) -Encoding ASCII
    $script = @'
local active, yaw, pitch, count, sumYaw, sumPitch = 0, 0, 0, 0, 0, 0
return {
    on_tick = function()
        if not world_available() then return end
        local x,y,z,dx,dy,dz = view_pose()
        control_offer(x,y,z)
        if active == 1 then
            control_pose(x,y,z,math.atan(-dx,-dz)-yaw,math.asin(dy)-pitch)
        end
        yaw, pitch = 0, 0
        hud_set("A"..math.floor(active).." N"..count.." Y"..math.floor(sumYaw*1000+.5)..
                " P"..math.floor(sumPitch*1000+.5))
    end,
    on_input = function(k,v)
        if k == "control" then
            active, yaw, pitch, count, sumYaw, sumPitch = v, 0, 0, 0, 0, 0
        elseif k == "mouse_yaw" or k == "mouse_pitch" then
            if active ~= 1 then unexpected_unowned_mouse() end
            count = count + 1
            if k == "mouse_yaw" then yaw, sumYaw = yaw+v, sumYaw+v
            else pitch, sumPitch = pitch+v, sumPitch+v end
        end
    end
}
'@
    Set-Content -LiteralPath (Join-Path $source 'scripts\main.lua') -Value $script -Encoding ASCII
    $log = Run-Scenario 'mouse-inputs' $source ($common + @(
        'pluginclick CONTROL FIXTURE','pluginclick CONTROL FIXTURE','settingsclose','wait 1',
        'testfocus 1','wait 0.2','testkey g','testmouse 100 -100','wait 0.2',
        'testcontrol','testgamehud com.example.control-fixture',
        'testmouse 4 -2','wait 0.1','testmouse 4 -2','wait 0.1',
        'testcontrol','testgamehud com.example.control-fixture',
        'wait 0.2','testgamehud com.example.control-fixture',
        'testfocus 0','testmouse 100 -100','wait 0.2','testgamehud com.example.control-fixture',
        'testfocus 1','wait 0.2','testkey g','testmouse 100 -100','wait 0.2',
        'testgamehud com.example.control-fixture',
        'testmouse 4 0','wait 0.1','testcontrol','testgamehud com.example.control-fixture',
        'testkey escape','testmouse 100 -100','wait 0.1','testcontrol',
        'testkey g','wait 0.2','testmouse 0 2','wait 0.1',
        'testgamehud com.example.control-fixture',
        'gameplaystatus com.example.control-fixture','pluginerror','quit'
    ))
    $joined = $log -join "`n"
    $states = @($log | Where-Object { $_ -like 'test control owner=*' })
    $hud = @($log | Where-Object { $_ -like 'test hud *' })
    $expectedHud = @('A1 N0 Y0 P0','A1 N4 Y20 P-10','A1 N4 Y20 P-10','A0 N0 Y0 P0',
                     'A1 N0 Y0 P0','A1 N1 Y10 P0','A1 N1 Y0 P5')
    if ($hud.Count -ne $expectedHud.Count -or $states.Count -ne 4) {
        throw "Missing mouse input states: $run"
    }
    for ($i=0; $i -lt $expectedHud.Count; $i++) {
        if ($hud[$i] -ne ('test hud '+$expectedHud[$i])) {
            throw "Unexpected mouse event delivery: $($hud[$i]); see $run"
        }
    }
    $angles = @($states | ForEach-Object {
        if ($_ -notmatch 'yaw=([-0-9.]+) pitch=([-0-9.]+)') { throw "Missing pose: $_" }
        @{ Yaw=[double]::Parse($Matches[1], [cultureinfo]::InvariantCulture);
           Pitch=[double]::Parse($Matches[2], [cultureinfo]::InvariantCulture) }
    })
    if ([math]::Abs(($angles[1].Yaw-$angles[0].Yaw)+.02) -gt .0001 -or
        [math]::Abs(($angles[1].Pitch-$angles[0].Pitch)-.01) -gt .0001 -or
        [math]::Abs($angles[3].Yaw-$angles[2].Yaw) -gt .0001 -or
        [math]::Abs($angles[3].Pitch-$angles[2].Pitch) -gt .0001 -or
        $states[3] -notmatch 'owner=0' -or
        $joined -notmatch 'gameplay com.example.control-fixture running=1' -or
        $joined -match 'unexpected_unowned_mouse|pose exceeds') {
        throw "Mouse pose ownership, native double rotation or release flush failed: $run"
    }
    if ($TrackFlyZip) {
        $track = Run-Scenario 'trackfly-mouse' $TrackFlyZip ($common + @(
            'pluginclick TRACKFLY 2','pluginclick TRACKFLY 2','settingsclose','wait 6',
            'testfocus 1','testgotooffer com.davidalanfisk.trackfly2','wait 0.2',
            'testkey g','wait 0.3','testcontrol','testgamehud com.davidalanfisk.trackfly2',
            'testmouse 4 2','wait 0.1','testmouse 4 2','wait 0.1',
            'testcontrol','testgamehud com.davidalanfisk.trackfly2',
            'testboost 1','testpad connected 1','testpad rt 1','wait 0.5',
            'testcontrol','testgamehud com.davidalanfisk.trackfly2',
            'testfocus 0','wait 0.2','testcontrol','testgamehud com.davidalanfisk.trackfly2',
            'gameplaystatus com.davidalanfisk.trackfly2','pluginerror',
            'pluginclick DISABLE TRACKFLY 2','wait 0.2',
            'gameplaystatus com.davidalanfisk.trackfly2','quit'
        ))
        $joined = $track -join "`n"
        $states = @($track | Where-Object { $_ -like 'test control owner=*' })
        if ($states.Count -ne 4 -or $states[0] -notmatch 'owner=1' -or
            $states[1] -notmatch 'owner=1' -or $states[2] -notmatch 'owner=1' -or
            $states[3] -notmatch 'owner=0' -or
            $joined -notmatch 'test hud [0-9]+u/s' -or
            $joined -notmatch 'gameplay com.davidalanfisk.trackfly2 running=1' -or
            $joined -notmatch 'gameplay com.davidalanfisk.trackfly2 running=0 boxes=0 models=0' -or
            $joined -match 'pose exceeds|limit exceeded|test offer unavailable') {
            throw "TrackFly mouse/speed HUD integration failed: $run\trackfly-mouse"
        }
        if ($states[0] -notmatch 'yaw=([-0-9.]+) pitch=([-0-9.]+)') { throw 'Missing TrackFly pose' }
        $yaw = [double]::Parse($Matches[1], [cultureinfo]::InvariantCulture)
        $pitch = [double]::Parse($Matches[2], [cultureinfo]::InvariantCulture)
        if ($states[1] -notmatch 'yaw=([-0-9.]+) pitch=([-0-9.]+)' -or
            [double]::Parse($Matches[1], [cultureinfo]::InvariantCulture) -ge ($yaw-.001) -or
            [double]::Parse($Matches[2], [cultureinfo]::InvariantCulture) -ge ($pitch-.001)) {
            throw "TrackFly did not consume right/down mouse steering: $run\trackfly-mouse"
        }
    }
    Write-Output "Relative mouse frames, ownership, focus/reacquisition, pose routing and package integration passed: $run"
    return
}
if ($FlightInputsOnly) {
    $source = Join-Path $run 'flight-input-source'
    Copy-Item -LiteralPath (Join-Path $fixtures 'gameplay-control') -Destination $source -Recurse
    $manifestPath = Join-Path $source 'plugin.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifest.permissions += @('controller','hud')
    Set-Content -LiteralPath $manifestPath -Value ($manifest | ConvertTo-Json -Depth 4) -Encoding ASCII
    $script = @'
local active, boost, lx, rt, rb = 0, 0, 0, 0, 0
return {
    on_tick = function()
        if not world_available() then return end
        local x,y,z = view_pose()
        control_offer(x,y,z)
        hud_set("A"..math.floor(active).." B"..math.floor(boost).." L"..math.floor(lx*10)..
                " T"..math.floor(rt*10).." R"..math.floor(rb))
    end,
    on_input = function(k,v)
        if k == "boost" then boost = v
        elseif k == "gamepad_left_x" then lx = v
        elseif k == "right_trigger" then rt = v
        elseif k == "pad_rb" then rb = v
        elseif k == "control" then
            if v == 0 and (boost ~= 0 or lx ~= 0 or rt ~= 0 or rb ~= 0) then missing_release_cleanup() end
            active, boost, lx, rt, rb = v, 0, 0, 0, 0
        end
    end
}
'@
    Set-Content -LiteralPath (Join-Path $source 'scripts\main.lua') -Value $script -Encoding ASCII
    $activate = @('testpad start 0','wait 0.1','testpad start 1','wait 0.2',
                  'testpad start 0','wait 0.1')
    $neutral = @('testboost 0','testpad lx 0','testpad rt 0','testpad rb 0',
                 'testpad x 0','testpad y 0','testpad b 0','testpad back 0','testpad start 0','wait 0.2')
    $commands = $common + @(
        'pluginclick CONTROL FIXTURE','pluginclick CONTROL FIXTURE','settingsclose','wait 1',
        'testpad connected 1','wait 0.1','testpad start 1','wait 0.2','testcontrol',
        'testfocus 1','wait 0.2','testcontrol',
        'testboost 1','testpad lx 0.5','testpad rt 1'
    ) + $activate + @(
        'testcontrol','testgamehud com.example.control-fixture',
        'testpad x 1','testpad y 1','testpad rb 1','wait 0.2',
        'testcontrol','testgamehud com.example.control-fixture',
        'testboost 0','wait 0.1','testgamehud com.example.control-fixture',
        'testpad b 1','wait 0.2','testcontrol'
    ) + $neutral + $activate + @(
        'testcontrol','testboost 1','testpad rt 1','wait 0.1',
        'testfocus 0','wait 0.2','testcontrol','testgamehud com.example.control-fixture',
        'testfocus 1','wait 0.2','testcontrol'
    ) + $neutral + $activate + @(
        'testcontrol','testpad start 1','wait 0.2','testcontrol'
    ) + $neutral + $activate + @(
        'testcontrol','testpad back 1','wait 0.2','testcontrol'
    ) + $neutral + $activate + @(
        'testcontrol','testpad connected 0','wait 0.2','testcontrol',
        'testpad connected 1','wait 0.2','testpad x 1','wait 0.2','testcontrol',
        'gameplaystatus com.example.control-fixture','pluginerror','quit'
    )
    $log = Run-Scenario 'flight-inputs' $source $commands
    $states = @($log | Where-Object { $_ -like 'test control owner=*' })
    $expected = @(0,0,1,1,0,1,0,0,1,0,1,0,1,0,0)
    if ($states.Count -ne $expected.Count) { throw "Missing flight input states: $run" }
    for ($i=0; $i -lt $expected.Count; $i++) {
        if ($states[$i] -notmatch ("owner="+$expected[$i]+" ")) {
            throw "Unexpected controller ownership at state ${i}: $($states[$i]); see $run"
        }
    }
    foreach ($index in @(2,3)) {
        if ($states[$index] -notmatch 'start=0 keyboard=0 switcher=0') {
            throw "Controller action leaked to shell UI: $($states[$index])"
        }
    }
    $joined = $log -join "`n"
    if ($joined -notmatch 'test hud A1 B1 L5 T10 R0' -or
        $joined -notmatch 'test hud A1 B1 L5 T10 R1' -or
        $joined -notmatch 'test hud A1 B0 L5 T10 R1' -or
        $joined -notmatch 'test hud A0 B0 L0 T0 R0' -or
        $states[-1] -notmatch 'keyboard=1' -or
        $joined -notmatch 'gameplay com.example.control-fixture running=1' -or
        $joined -match 'missing_release_cleanup') {
        throw "Flight snapshot/boost/release/shortcut restoration failed: $run"
    }
    if ($TrackFlyZip) {
        $track = Run-Scenario 'trackfly' $TrackFlyZip ($common + @(
            'pluginclick TRACKFLY 2','pluginclick TRACKFLY 2','settingsclose',
            'wait 6','gameplaystatus com.davidalanfisk.trackfly2','testgamehud com.davidalanfisk.trackfly2',
            'testfocus 1','testpad connected 1','wait 0.1','testgotooffer com.davidalanfisk.trackfly2',
            'testpad start 1','wait 0.2','testpad start 0',
            'testpad rt 1','testboost 1','testpad rb 1','testpad y 1','wait 0.8',
            'testcontrol','testgamehud com.davidalanfisk.trackfly2','gameplaystatus com.davidalanfisk.trackfly2',
            'testpad y 0','testpad x 1','testpad lx 0.3','testpad rx 0.3','testpad ry 0.2','wait 0.5',
            'testcontrol','testgamehud com.davidalanfisk.trackfly2',
            'testboost 0','testpad rb 0','testpad x 0','testpad rt 0','testpad lt 0.5','testpad lb 1',
            'wait 0.3','testpad b 1','wait 0.2','testcontrol',
            'gameplaystatus com.davidalanfisk.trackfly2','pluginerror',
            'pluginclick DISABLE TRACKFLY 2','wait 0.2','gameplaystatus com.davidalanfisk.trackfly2','quit'
        ))
        $joined = $track -join "`n"
        $states = @($track | Where-Object { $_ -like 'test control owner=*' })
        if ($states.Count -ne 3 -or $states[0] -notmatch 'owner=1' -or $states[1] -notmatch 'owner=1' -or
            $states[2] -notmatch 'owner=0' -or
            $states[0] -notmatch 'start=0 keyboard=0 switcher=0' -or
            $states[1] -notmatch 'start=0 keyboard=0 switcher=0' -or
            $joined -notmatch 'gameplay com.davidalanfisk.trackfly2 running=1' -or
            $joined -notmatch 'gameplay com.davidalanfisk.trackfly2 running=0 boxes=0 models=0' -or
            $joined -match 'pose exceeds|limit exceeded|test offer unavailable') {
            throw "TrackFly controls integration failed: $run\trackfly"
        }
    }
    Write-Output "Boost, Xbox ownership/focus/held-state routing and flight package integration passed: $run"
    return
}
if ($MotionOnly) {
    foreach ($case in @(
        @{ Name='speed-100'; Speed='100'; Start='0 400 110'; Expected=100.0 },
        @{ Name='speed-500'; Speed='500'; Start='0 400 110'; Expected=100.0 },
        @{ Name='speed-1000'; Speed='1000'; Start='0 400 110'; Expected=100.0 },
        @{ Name='offset-1000'; Speed='1000'; Start='200 400 110'; Expected=300.0 },
        @{ Name='blocked-1000'; Speed='1000'; Start='-30 50 -55'; Expected=-4.8 },
        @{ Name='over-limit'; Speed='1000.01'; Start='0 400 110'; Expected=0.0 }
    )) {
        $source = Join-Path $run ($case.Name + '-source')
        Copy-Item -LiteralPath (Join-Path $fixtures 'gameplay-control') -Destination $source -Recurse
        $script = @'
local active, remaining = false, 100
return {
    on_tick = function(dt)
        if not world_available() then return end
        local x,y,z,dx,dy,dz = view_pose()
        if not x then return end
        control_offer(x,y,z)
        if active and remaining > 0 then
            local step = SPEED * dt
            if SPEED <= 1000 then step = math.min(step,remaining) end
            remaining = remaining - step
            control_pose(x+step,y,z,math.atan(-dx,-dz),math.asin(math.max(-1,math.min(1,dy))))
        end
    end,
    on_input = function(key,value)
        if key == "control" then active = value == 1 end
    end
}
'@
        Set-Content -LiteralPath (Join-Path $source 'scripts\main.lua') -Value $script.Replace('SPEED',$case.Speed) -Encoding ASCII
        $log = Run-Scenario $case.Name $source ($common + @(
            'pluginclick CONTROL FIXTURE', 'pluginclick CONTROL FIXTURE', 'settingsclose',
            'fly on', ('cam ' + $case.Start + ' 0 0'), 'wait 0.3', 'testfocus 1',
            'testkey g', 'wait 2', 'testcontrol', 'gameplaystatus com.example.control-fixture',
            'pluginerror', 'testfocus 0', 'wait 0.2', 'testcontrol',
            'pluginclick DISABLE CONTROL FIXTURE', 'wait 0.2',
            'gameplaystatus com.example.control-fixture', 'quit'
        ))
        $states = @($log | Where-Object { $_ -like 'test control owner=*' })
        $joined = $log -join "`n"
        if ($case.Name -eq 'over-limit') {
            if ($states.Count -ne 2 -or $states[0] -notmatch 'owner=0' -or
                $joined -notmatch 'gameplay com.example.control-fixture running=0 boxes=0 models=0' -or
                $joined -notmatch 'Gameplay control pose exceeds motion limits') {
                throw "Over-limit control did not fail and clean up: $run\$($case.Name)"
            }
        } else {
            if ($states.Count -ne 2 -or $states[0] -notmatch 'owner=1.*pos=([-0-9.]+),' -or
                $states[1] -notmatch 'owner=0' -or
                $joined -notmatch 'gameplay com.example.control-fixture running=1' -or
                $joined -notmatch 'gameplay com.example.control-fixture running=0 boxes=0 models=0' -or
                $joined -match 'Gameplay control pose exceeds motion limits') {
                throw "Fast control/lifecycle failed: $run\$($case.Name)"
            }
            $states[0] -match 'pos=([-0-9.]+),' | Out-Null
            $x = [double]::Parse($Matches[1], [System.Globalization.CultureInfo]::InvariantCulture)
            if ([Math]::Abs($x - $case.Expected) -gt 0.01) {
                throw "Unexpected motion endpoint $x, expected $($case.Expected): $run\$($case.Name)"
            }
        }
    }
    Write-Output "Fast motion, city obstruction, rejection and cleanup passed: $run"
    return
}
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
    $joined -notmatch ', SOUND, HUD' -or
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
if ($ControlOnly) {
    Write-Output "Keyboard lease, callback cleanup, packaged car and parks input integration passed: $run"
    return
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
