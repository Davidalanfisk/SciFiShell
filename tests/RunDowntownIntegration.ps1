# Runs the normal Release build on a private Windows desktop with a separate profile.
# Does not activate the installed shell, use its profile, or inject physical input.
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [switch]$ArcadeOnly,
    [string]$ArcadeRom
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Threading;
public static class DowntownDesktop {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct Startup { public int cb; public string reserved,desktop,title; public int x,y,xSize,ySize,xChars,yChars,fill,flags; public short show,reserved2; public IntPtr reservedPtr,stdin,stdout,stderr; }
    [StructLayout(LayoutKind.Sequential)] struct Process { public IntPtr process,thread; public int pid,tid; }
    [StructLayout(LayoutKind.Sequential)] struct Security { public int length; public IntPtr descriptor; public int inherit; }
    [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
    static extern IntPtr CreateDesktop(string n,IntPtr d,IntPtr m,int f,uint a,IntPtr s);
    [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr d);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
    static extern bool CreateProcess(string a,StringBuilder c,IntPtr p,IntPtr t,bool i,uint f,IntPtr e,string cwd,ref Startup s,out Process r);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h,uint ms);
    [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr h,out uint c);
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr h,uint c);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    delegate bool WindowProc(IntPtr window,IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr desktop,WindowProc callback,IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr window,WindowProc callback,IntPtr data);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr window,StringBuilder text,int count);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr window,StringBuilder text,int count);
    [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr SendMessage(IntPtr window,uint message,IntPtr wparam,string text);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window,uint message,IntPtr wparam,IntPtr lparam);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
    static extern IntPtr CreateFile(string p,uint a,uint s,ref Security sec,uint c,uint f,IntPtr t);
    public static int Run(string exe,string log,string cwd,string dialogAction) {
        string name="SciFiDowntown_"+Guid.NewGuid().ToString("N");
        IntPtr desktop=CreateDesktop(name,IntPtr.Zero,IntPtr.Zero,0,0x01ff,IntPtr.Zero);
        if(desktop==IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
        try {
            Security sec=new Security { length=Marshal.SizeOf(typeof(Security)),inherit=1 };
            IntPtr output=CreateFile(log,0x40000000,1,ref sec,2,0x80,IntPtr.Zero);
            if(output==new IntPtr(-1)) throw new System.ComponentModel.Win32Exception();
            try {
                Startup si=new Startup(); si.cb=Marshal.SizeOf(typeof(Startup)); si.desktop=name;
                si.flags=0x100; si.stdout=output; si.stderr=output; Process pi;
                if(!CreateProcess(exe,new StringBuilder("\""+exe+"\""),IntPtr.Zero,IntPtr.Zero,true,0x08000000,IntPtr.Zero,cwd,ref si,out pi))
                    throw new System.ComponentModel.Win32Exception();
                try {
                    bool done=false, dialogHandled=false;
                    Thread dialogThread=null;
                    if(!String.IsNullOrEmpty(dialogAction)) {
                        dialogThread=new Thread(() => {
                            for(int i=0;i<600 && !done;i++) {
                                IntPtr dialog=IntPtr.Zero;
                                EnumDesktopWindows(desktop,(w,d) => {
                                    var title=new StringBuilder(256);GetWindowText(w,title,title.Capacity);
                                    if(title.ToString()=="Select a lawful NES ROM" && IsWindowVisible(w) && IsWindowEnabled(w)) {dialog=w;return false;}
                                    return true;
                                },IntPtr.Zero);
                                if(dialog!=IntPtr.Zero) {
                                    if(dialogAction=="cancel") {PostMessage(dialog,0x10,IntPtr.Zero,IntPtr.Zero);dialogHandled=true;return;}
                                    IntPtr edit=IntPtr.Zero,open=IntPtr.Zero;
                                    EnumChildWindows(dialog,(w,d) => {
                                        var cls=new StringBuilder(128);GetClassName(w,cls,cls.Capacity);
                                        if(cls.ToString()=="Edit" && GetDlgCtrlID(w)==1148) edit=w;
                                        if(cls.ToString()=="Button" && GetDlgCtrlID(w)==1) open=w;
                                        return true;
                                    },IntPtr.Zero);
                                    if(edit!=IntPtr.Zero && open!=IntPtr.Zero) {
                                        SendMessage(edit,0x000c,IntPtr.Zero,dialogAction);
                                        PostMessage(open,0x00f5,IntPtr.Zero,IntPtr.Zero);
                                        dialogHandled=true;return;
                                    }
                                }
                                Thread.Sleep(50);
                            }
                        });
                        dialogThread.IsBackground=true;dialogThread.Start();
                    }
                    if(WaitForSingleObject(pi.process,String.IsNullOrEmpty(dialogAction)?90000u:30000u)!=0) {
                        TerminateProcess(pi.process,99); WaitForSingleObject(pi.process,5000);
                        done=true;if(dialogThread!=null)dialogThread.Join();return 99;
                    }
                    done=true;if(dialogThread!=null)dialogThread.Join();
                    if(dialogThread!=null && !dialogHandled)return 98;
                    uint code; GetExitCodeProcess(pi.process,out code); return (int)code;
                } finally { CloseHandle(pi.thread); CloseHandle(pi.process); }
            } finally { CloseHandle(output); }
        } finally { CloseDesktop(desktop); }
    }
}
'@
$exe = (Resolve-Path -LiteralPath $Executable).Path
$root = Join-Path (Resolve-Path -LiteralPath $OutputDirectory).Path ('downtown-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$profile = Join-Path $root 'profile'
New-Item -ItemType Directory -Path $profile | Out-Null
foreach($folder in @('Desktop','Documents','Downloads','Pictures','Music','Videos')) {
    New-Item -ItemType Directory -Path (Join-Path $profile $folder) | Out-Null
}
$zip = Join-Path $root 'custom.zip'
$defaultZip = Join-Path $root 'default.zip'
$failedZip = Join-Path $root 'failed.zip'
$utf8 = New-Object System.Text.UTF8Encoding($false)
function New-Package([string]$path, [string]$manifest, [string]$script, [byte[]]$model = $null) {
    $archive = [System.IO.Compression.ZipFile]::Open($path, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($pair in @(@('plugin.json', $utf8.GetBytes($manifest)), @('scripts/main.lua', $utf8.GetBytes($script)))) {
            $stream = $archive.CreateEntry($pair[0]).Open()
            try { $stream.Write($pair[1], 0, $pair[1].Length) } finally { $stream.Dispose() }
        }
        if ($model) {
            $stream = $archive.CreateEntry('assets/building.glb').Open()
            try { $stream.Write($model, 0, $model.Length) } finally { $stream.Dispose() }
        }
    } finally { $archive.Dispose() }
}
# A tiny original triangle fixture; no doorway and no external files.
$json = '{"asset":{"version":"2.0"},"buffers":[{"byteLength":40}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":3}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5121,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0}'
while ($json.Length % 4) { $json += ' ' }
$buffer = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($buffer)
$writer.Write([uint32]0x46546c67); $writer.Write([uint32]2); $writer.Write([uint32](28+$json.Length+40))
$writer.Write([uint32]$json.Length); $writer.Write([uint32]0x4e4f534a); $writer.Write($utf8.GetBytes($json))
$writer.Write([uint32]40); $writer.Write([uint32]0x004e4942)
foreach ($v in @(0,0,0,1,0,0,0,1,0)) { $writer.Write([single]$v) }
$writer.Write([byte[]]@(0,1,2,0))
$model = $buffer.ToArray(); $writer.Dispose()
$manifest = '{"id":"com.example.downtown","name":"DISTRICT TEST","version":"1.0.0","author":"Test","description":"Host entrance fixture","entry":"scripts/main.lua","type":"gameplay","platforms":["any"],"permissions":["scene"],"api_version":"1","downtown_model":"assets/building.glb"}'
New-Package $zip $manifest 'return {}' $model
New-Package $failedZip ($manifest.Replace('com.example.downtown','com.example.failed').Replace('DISTRICT TEST','FAILED TEST').Replace(',"downtown_model":"assets/building.glb"','')) 'missing()'
$clock = Join-Path $PSScriptRoot '..\examples\plugins\clock'
New-Package $defaultZip ([System.IO.File]::ReadAllText((Join-Path $clock 'plugin.json'))) ([System.IO.File]::ReadAllText((Join-Path $clock 'scripts\main.lua')))

function Run-Scenario([string]$name, [string[]]$commands, [string]$dialogAction = '') {
    $control = Join-Path $profile 'SciFiShell\control.log'
    $previousLines = 0
    if (Test-Path -LiteralPath $control) { $previousLines = @(Get-Content -LiteralPath $control).Count }
    $script = Join-Path $root "$name.txt"
    Set-Content -LiteralPath $script -Value $commands -Encoding ASCII
    $saved = @{}
    $names = @('LOCALAPPDATA','APPDATA','USERPROFILE','HOME','SDL_AUDIODRIVER')
    Get-ChildItem Env: | Where-Object { $_.Name -like 'SCIFI_*' -or $_.Name -in $names } |
        ForEach-Object { $saved[$_.Name] = $_.Value }
    try {
        Get-ChildItem Env: | Where-Object { $_.Name -like 'SCIFI_*' } |
            ForEach-Object { Remove-Item -LiteralPath ("Env:\"+$_.Name) }
        $env:LOCALAPPDATA=$profile; $env:APPDATA=$profile; $env:USERPROFILE=$profile; $env:HOME=$profile
        $env:SDL_AUDIODRIVER='dummy'; $env:SCIFI_NO_UPDATE='1'; $env:SCIFI_SCRIPT=$script
        $code = [DowntownDesktop]::Run($exe, (Join-Path $root "$name-app.log"), (Split-Path -Parent $exe),$dialogAction)
    } finally {
        Get-ChildItem Env: | Where-Object { $_.Name -like 'SCIFI_*' -or $_.Name -in $names } |
            ForEach-Object { Remove-Item -LiteralPath ("Env:\"+$_.Name) }
        foreach ($key in $saved.Keys) { Set-Item -LiteralPath ("Env:\"+$key) -Value $saved[$key] }
    }
    if ($code -ne 0) { throw "$name exited $code; see $root" }
    $log = @(Get-Content -LiteralPath $control | Select-Object -Skip $previousLines)
    if ($log -match 'not found|pick blocked|pick failed|entrance unavailable|card unavailable|layout_clear=0|collision_safe=0|parity=0') { throw "Interaction failed: $root" }
    return $log
}
function Require([string[]]$log, [string]$pattern) {
    if (!($log -match $pattern)) { throw "Missing $pattern; see $root" }
}
if($ArcadeOnly) {
    $exterior=Join-Path $root 'arcade-exterior.png'
    $floor=Join-Path $root 'arcade-floor.png'
    $loft=Join-Path $root 'arcade-loft.png'
    $landing=Join-Path $root 'arcade-landing.png'
    $game=Join-Path $root 'arcade-game.png'
    $menu=Join-Path $root 'arcade-menu.png'
    $log=Run-Scenario 'arcade-originals' @(
        'wait 2','arcadefocus 1','downtownclick VISIT NEON CIRCUIT ARCADE','wait 1',"shot $exterior",
        'downtownenter org.scifishell.arcade','wait 1','arcadestatus',"shot $floor",
        'cam 9 1.7 4 0 0 0','walk 11 4','look 90 0','walk 7 3','look 0 0','walk 2 1',
        'wait .2','arcadegeometry','cam 0 6.5 -10 0 -8 0','wait 1',"shot $loft",
        'cam 1 6.5 -10 -90 -8 0','wait 1',"shot $landing",
        'cam 0 1.7 2 0 0 0','downtowncard 0','wait .5',"shot $menu",'arcadekey Return','wait 2',
        'arcadestatus',"shot $game",
        'arcadekey F9','arcadekey F10','arcadekey F2','wait .2','arcadestatus','downtownstatus',
        'arcadekey Escape','wait .2','arcadestatus',
        'arcadepad 0 -1 0','arcadepad 0 0 0','arcadepad 1 0 0','arcadepad 0 0 0',
        'arcadepad 0 1 0','arcadepad 0 0 0','arcadepad 0 0 1','wait 1','arcadestatus',
        'arcadevr 0 0 2','arcadevr 0 0 2','wait .2','arcadestatus',
        'cam -2 1.7 2 0 0 0','downtowncard 1','arcadevr 0 0 1','wait 1','arcadestatus',
        'arcadevr 0 0 2','arcadevr 0 0 2','leave',
        'pluginclick NEON CIRCUIT ARCADE','pluginstatus','settingsclose',
        'downtownenter org.scifishell.arcade','downtownstatus','downtowncard 2',
        'downtowncard 1','pluginstatus','settingsclose',
        'downtownenter org.scifishell.arcade','downtowncard 0','arcadekey Return','wait .2','arcadestatus',
        'arcadefocus 0','wait .2','arcadestatus','quit'
    )
    Require $log 'active=0 game=0 ticks=[1-9]'
    Require $log 'active=0 game=1 ticks=[1-9]'
    Require $log 'active=1 game=1 ticks=[1-9]'
    Require $log 'arcade floor=4.800'
    Require $log 'selected=-1 active=-1.*simulated_focus=0'
    Require $log 'selected=-1 active=-1'
    Require $log 'org.scifishell.arcade=off'
    Require $log 'org.scifishell.arcade=on'
    Require $log 'interior=org.scifishell.arcade cards=5'
    foreach($image in @($exterior,$floor,$loft,$landing,$game,$menu)) {
        if(!(Test-Path -LiteralPath $image) -or (Get-Item -LiteralPath $image).Length -lt 10000) {
            throw "Missing diagnostic image $image"
        }
        $log=Run-Scenario 'arcade-snack-pad' @(
            'wait 2','downtownclick VISIT NEON CIRCUIT ARCADE','wait .5','give 0','pluginstatus','testpada','wait .2','pluginstatus',
            'wait .2','pluginstatus','give 1','pluginstatus','testpada','wait .2','pluginstatus',
            'wait .2','pluginstatus','downtownstatus','quit'
        )
        $states=@($log | Where-Object {$_ -like 'plugins *'})
        if($states.Count -ne 6 -or $states[0] -notmatch 'holding=1' -or
           $states[1] -notmatch 'holding=0' -or $states[2] -notmatch 'holding=0' -or
           $states[3] -notmatch 'holding=1' -or $states[4] -notmatch 'holding=0' -or $states[5] -notmatch 'holding=0') {
            throw "Xbox snack interaction failed: $root"
        }
        Require $log 'city=1 interior=none'
    }
    $log=Run-Scenario 'arcade-persistence' @(
        'wait 2','arcadefocus 1','downtownclick VISIT NEON CIRCUIT ARCADE','downtownenter org.scifishell.arcade',
        'downtowncard 0','arcadekey Return','wait 1','arcadestatus','quit'
    )
    Require $log 'active=0 game=1 ticks=[1-9]'
    # Original NROM diagnostic, authored here; no commercial data or assets.
    $fixture=Join-Path $root 'diagnostic.nes'
    $rom=New-Object byte[] (16+16384+8192)
    $header=[byte[]]@(0x4e,0x45,0x53,0x1a,1,1)
    $header.CopyTo($rom,0)
    [byte[]]$program=@(0x78,0xd8,0xa2,0xff,0x9a,0xe8,0x8e,0,0x20,0x8e,1,0x20,
        0x2c,2,0x20,0x10,0xfb,0xa9,0x3f,0x8d,6,0x20,0xa9,0,0x8d,6,0x20,
        0xa9,0x21,0x8d,7,0x20,0x4c,0x20,0x80)
    $program.CopyTo($rom,16)
    for($i=0;$i -lt 3;$i++) { $rom[16+16384-6+$i*2]=0; $rom[16+16384-5+$i*2]=0x80 }
    [System.IO.File]::WriteAllBytes($fixture,$rom)
    $invalid=Join-Path $root 'invalid.nes'
    [System.IO.File]::WriteAllBytes($invalid,[byte[]]@(1,2,3))
    $log=Run-Scenario 'arcade-picker-cancel' @(
        'wait 2','arcadefocus 1','downtownclick VISIT NEON CIRCUIT ARCADE','downtownenter org.scifishell.arcade',
        'downtowncard 0','arcadekey Up','arcadekey Return','wait .5','arcadestatus',
        'arcadekey Down','arcadekey Return','wait 1','arcadestatus','quit'
    ) 'cancel'
    Require $log 'active=0 game=1 ticks=[1-9]'
    $log=Run-Scenario 'arcade-picker-invalid' @(
        'wait 2','arcadefocus 1','downtownclick VISIT NEON CIRCUIT ARCADE','downtownenter org.scifishell.arcade',
        'downtowncard 0','arcadekey Up','arcadekey Return','wait .5','arcadestatus','quit'
    ) $invalid
    Require $log 'notice=NES IMPORT LIMIT'
    $log=Run-Scenario 'arcade-picker-valid' @(
        'wait 2','arcadefocus 1','downtownclick VISIT NEON CIRCUIT ARCADE','downtownenter org.scifishell.arcade',
        'downtowncard 0','arcadekey Up','arcadekey Return','wait .5','arcadestatus',
        'arcadekey Down','arcadekey Return','wait 4','arcadestatus','quit'
    ) $fixture
    Require $log 'notice=ROM IMPORTED'
    Require $log 'active=0 game=4 ticks=[1-9]'
    $log=Run-Scenario 'arcade-nes' @(
        'wait 2','arcadefocus 1','downtownclick VISIT NEON CIRCUIT ARCADE','downtownenter org.scifishell.arcade',
        'downtowncard 0',"arcadeimport $fixture",'arcadekey Return','wait 5','arcadestatus',
        'arcadepad 0 0 1','arcadevr 1 0 1','wait 1','arcadestatus',
        'pluginclick NEON CIRCUIT ARCADE','arcadestatus','settingsclose',
        'pluginclick NEON CIRCUIT ARCADE','settingsclose','downtownenter org.scifishell.arcade',
        'downtowncard 0','arcadekey Return','wait 3','arcadestatus',
        'arcadekey Escape','wait .2','arcadestatus','arcadekey Escape','wait .2',
        'downtowncard 1','arcadekey Return','wait 1','arcadestatus','leave','arcadestatus','quit'
    )
    Require $log 'arcade import ok'
    Require $log 'active=0 game=4 ticks=[1-9]'
    Require $log 'active=1 game=1 ticks=[1-9]'
    Require $log 'arcade running=0 selected=-1 active=-1'
    if($log -match 'ARCADE_ERROR|TIMED OUT|INITIALIZATION FAILED|INTEGRITY CHECK FAILED') { throw "NES failed: $root" }
    if($ArcadeRom) {
        # Never capture screenshots of a user-supplied game.
        $originalHash=(Get-FileHash -LiteralPath $ArcadeRom -Algorithm SHA256).Hash
        $imported=Join-Path $profile 'SciFiShell\arcade\cabinet-0.nes'
        try {
            $log=Run-Scenario 'arcade-user-rom' @(
            'wait 2','arcadefocus 1','downtownclick VISIT NEON CIRCUIT ARCADE','downtownenter org.scifishell.arcade',
            'downtowncard 0',"arcadeimport $ArcadeRom",'arcadekey Return','wait 5','arcadestatus',
            'arcadepad 0 0 5','arcadepad 0 0 0','wait 2','arcadestatus','arcadekey Escape','wait .2','arcadestatus','quit'
        )
            Require $log 'arcade import ok'
            Require $log 'active=0 game=4 ticks=[1-9]'
            if($log -match 'ARCADE_ERROR|TIMED OUT') { throw "User ROM failed: $root" }
            if((Get-FileHash -LiteralPath $ArcadeRom -Algorithm SHA256).Hash -ne $originalHash -or
               (Get-FileHash -LiteralPath $imported -Algorithm SHA256).Hash -ne $originalHash) {
                throw 'User ROM changed or import is not an exact copy'
            }
        } finally {
            if(Test-Path -LiteralPath $imported) { Remove-Item -LiteralPath $imported }
        }
    }
    Write-Output "Arcade integration passed: $root"
    return
}
$log = Run-Scenario 'builtins' @(
    'wait 2', 'downtownstatus', 'downtownclick VISIT CAR AND TRACK', 'downtownenter org.scifishell.driving',
    'downtowncard 2', 'downtownstatus', 'downtowncard 1', 'pluginstatus', 'downtownstatus',
    'settingsclose', 'downtownenter org.scifishell.driving', 'downtowncard 1', 'pluginstatus',
    'settingsclose', 'downtownclick VISIT PARKS AND TOYS', 'downtownenter org.scifishell.parks',
    'downtowncard 2', 'downtownstatus', 'downtowncard 1', 'pluginstatus',
    'settingsclose', 'downtownenter org.scifishell.parks', 'downtowncard 1', 'pluginstatus', 'quit'
)
Require $log 'bounds=-68,36,108,148'
Require $log 'interior=org.scifishell.driving cards=5'
Require $log 'interior=org.scifishell.parks cards=5'
Require $log 'org.scifishell.driving=off'
Require $log 'org.scifishell.driving=on'
Require $log 'org.scifishell.parks=off'
Require $log 'org.scifishell.parks=on'
Require $log 'city=1 interior=none'
Require $log 'built=1 layout_clear=1'
$log = Run-Scenario 'lifecycle' @(
    'wait 2', "plugininstall $defaultZip", "plugininstall $zip", "plugininstall $failedZip",
    'downtownstatus', 'downtownclick VISIT CLOCK', 'downtownenter com.example.clock', 'downtownstatus',
    'downtowncard 1', 'pluginnotice', 'pluginstatus', 'downtownstatus',
    'settingsclose', 'downtownenter com.example.clock', 'downtowncard 1', 'pluginnotice', 'pluginstatus',
    'settingsclose', 'downtownenter com.example.clock', 'downtowncard 4', 'downtownstatus',
    'downtownenter com.example.clock', 'downtowncard 3', 'downtownclick NEXT PLOT', 'downtownclick MOVE / SWAP',
    'downtownstatus', 'downtownclick VISIT DISTRICT TEST', 'downtownenter com.example.downtown', 'downtownstatus',
    'downtowncard 1', 'pluginnotice', 'downtownstatus', 'settingsclose', 'pluginstatus',
    'pluginclick DISTRICT TEST', 'pluginnotice', 'pluginclick DISTRICT TEST', 'pluginnotice', 'pluginstatus',
    'settingsclose', 'downtownenter com.example.downtown', 'downtowncard 1', 'pluginnotice', 'pluginstatus',
    'settingsclose', 'downtownenter com.example.failed', 'downtowncard 1', 'pluginclick FAILED TEST',
    'pluginnotice', 'pluginstatus', 'settingsclose', 'downtownenter com.example.failed', 'downtownstatus',
    'leave', 'downtownclick VISIT DISTRICT TEST', 'wait 1', 'quit'
)
Require $log 'interior=com.example.clock cards=5'
Require $log 'com.example.downtown=4\(custom\).*com.example.clock=5\(default\)|com.example.clock=5\(default\).*com.example.downtown=4\(custom\)'
Require $log 'collision_safe=1'
Require $log 'built=1 layout_clear=1'
Require $log 'com.example.clock=on'
Require $log 'plugin notice PLUGIN ENABLED'
Require $log 'plugin notice PLUGIN DISABLED'
Require $log 'plugin notice.*ALLOW SCENE'
Require $log 'interior=com.example.failed cards=5'
Require $log "attempt to call a nil value"
$log = Run-Scenario 'restart-remove' @(
    'wait 2', 'downtownstatus', 'downtownclick VISIT DISTRICT TEST', 'downtownenter com.example.downtown',
    'downtowncard 2', 'pluginnotice', 'downtownstatus', 'settingsclose', 'pluginstatus',
    'pluginclick REMOVE DISTRICT TEST', 'pluginnotice', 'downtownstatus', 'pluginstatus',
    'settingsclose', 'downtownenter com.example.clock', 'downtowncard 2', 'pluginnotice',
    'pluginclick REMOVE CLOCK', 'pluginnotice', 'downtownstatus', 'quit'
)
Require $log 'CLICK REMOVE AGAIN'
Require $log 'PLUGIN UNINSTALLED'
Require $log 'com.example.clock=5\(default\).*com.example.downtown=4\(custom\)'
$last = ($log | Where-Object { $_ -like 'downtown *' })[-1]
if ($last -match 'com.example.clock=|com.example.downtown=') { throw "Removed buildings remain: $root" }
$log = Run-Scenario 'gap-fill' @('wait 2', "plugininstall $defaultZip", 'downtownstatus', 'quit')
Require $log 'com.example.clock=4\(default\)'
$commands=@('wait 2','settingsdistrictstatus',
    'settingsvisit settings.graphics','settingsenter settings.graphics',
    'cam 8.6 1.7 -6 -90 0','wait 1',("shot "+(Join-Path $root 'settings-console.png')),
    'cam 0 1.7 4 0 0','wait 0.3',("shot "+(Join-Path $root 'settings-gallery.png')),'leave',
    'settingsvisit settings.sound','cam -15 12 -112 -10 -12','wait 0.5',
    ("shot "+(Join-Path $root 'settings-exterior.png')),
    'cam 52 10 -310 24 -7','wait 0.5',("shot "+(Join-Path $root 'settings-sound.png')))
foreach($name in @('shell','graphics','windows','plugins','downtown','display','sound','network','mouse',
                   'personalisation','power','system','media-player','media-folders','media-tracks','media-queue','settings-district')) {
    $commands+=@("settingsvisit settings.$name","settingsenter settings.$name",'wait 0.15','settingsroomstatus','leave')
}
$commands+=@(
    'settingsvisit settings.graphics','settingsenter settings.graphics','settingsroomstatus',
    'settingsroomclick CROSSHAIR','settingsroomstatus','settingsroomclick FOG DENSITY','settingsroomstatus',
    'settingsroomminus FOG DENSITY','settingsroomstatus','settingsdistrictstatus','leave',
    'settingsmenuclick GRAPHICS|CROSSHAIR','settingsclose',
    'settingsvisit settings.graphics','settingsenter settings.graphics','settingsroomstatus',
    'settingsroomclick CITY','settingsdistrictstatus','settingsmenuclick GRAPHICS|CITY','settingsclose',
    'settingsdistrictclick ARRANGE DISPLAY','settingsdistrictclick NEXT PLOT','settingsdistrictclick MOVE / SWAP',
    'settingsdistrictstatus','settingsvisit settings.display','settingsenter settings.display',
    'settingsroomclick EXIT TO SETTINGS DISTRICT','settingsdistrictstatus',
    'settingsdistrictclick ARRANGE GRAPHICS','settingsdistrictclick PREVIOUS PLOT',
    'settingsdistrictclick PREVIOUS PLOT','settingsdistrictclick PREVIOUS PLOT',
    'settingsdistrictclick MOVE / SWAP','settingsdistrictstatus',
    'settingsvisit settings.graphics','settingsenter settings.graphics','wait 0.15','settingsroomstatus','leave',
    'settingsdistrictclick ARRANGE GRAPHICS'
)
for($i=0;$i -lt 18;$i++) { $commands+='settingsdistrictclick NEXT PLOT' }
$commands+=@('settingsdistrictclick MOVE / SWAP','settingsdistrictstatus',
    'settingsvisit settings.media-tracks','settingsenter settings.media-tracks','settingsroomclick SHOW',
    'wait 0.2','settingsroomstatus','settingsdistrictstatus','leave','quit')
$log=Run-Scenario 'settings-district' $commands
$screen=[System.Drawing.Bitmap]::new((Join-Path $root 'settings-console.png'))
try {
    $dark=0; $text=0; $pixels=0
    for($y=[int]($screen.Height*0.09);$y -lt $screen.Height*0.66;$y+=4) {
        for($x=[int]($screen.Width*0.33);$x -lt $screen.Width*0.66;$x+=4) {
            $p=$screen.GetPixel($x,$y)
            $v=[Math]::Max($p.R,[Math]::Max($p.G,$p.B))
            if($v -lt 80) { $dark++ }
            if($v -gt 140) { $text++ }
            $pixels++
        }
    }
    if($dark/$pixels -lt 0.85 -or $text/$pixels -lt 0.005 -or $text/$pixels -gt 0.15) {
        throw "Console render lost its dark surface/contrasting text: $root"
    }
    Write-Output ("Console render: dark={0:P1}, text={1:P1}" -f ($dark/$pixels),($text/$pixels))
} finally { $screen.Dispose() }
Require $log 'groups=17 buildings=17'
Require $log 'settings.graphics=1/'
Require $log 'settings.sound=1/'
Require $log 'settings.media-queue=1/'
Require $log 'settings room settings.graphics.*CROSSHAIR=OFF'
Require $log 'settings room settings.graphics.*CROSSHAIR=ON'
Require $log 'settings room settings.graphics.*FOG DENSITY=1.75'
Require $log 'settings room settings.graphics.*FOG DENSITY=1.60'
Require $log 'settings room settings.graphics.*aisle_halfwidth=9.600000.*max_eye=9.600000'
Require $log 'settings district city=0 interior=none'
Require $log 'settings district city=1 interior=none'
Require $log 'parity=1'
Require $log 'bounds=-136,136,-2188,-124'
Require $log 'bounds=-136,136,-460,-124'
$log=Run-Scenario 'settings-plugin-consent' @('wait 2',"plugininstall $zip",
    'settingsvisit settings.plugins','settingsenter settings.plugins','settingsroomclick DISTRICT TEST',
    'pluginnotice','pluginstatus','settingsdistrictstatus','settingsclose','pluginstatus',
    'settingsvisit settings.plugins','settingsenter settings.plugins','settingsroomclick DISTRICT TEST',
    'pluginnotice','pluginclick DISTRICT TEST','pluginnotice','pluginstatus','settingsclose',
    'settingsvisit settings.plugins','settingsenter settings.plugins','settingsroomclick REMOVE DISTRICT TEST',
    'pluginnotice','settingsdistrictstatus','pluginclick REMOVE DISTRICT TEST','pluginnotice','quit')
Require $log 'plugin notice.*ALLOW SCENE'
Require $log 'com.example.downtown=off'
Require $log 'com.example.downtown=on'
Require $log 'CLICK REMOVE AGAIN'
Require $log 'PLUGIN UNINSTALLED'
Require $log 'settings district city=1 interior=none'
$state=Join-Path $profile 'SciFiShell\settings-district.ini'
$saved=[System.IO.File]::ReadAllText($state)
$log=Run-Scenario 'settings-restart' @('wait 2','settingsdistrictstatus','settingsvisit settings.display',
    'settingsenter settings.display','settingsroomstatus','leave','quit')
if ([System.IO.File]::ReadAllText($state) -ne $saved) { throw "Settings placements changed on restart: $root" }
Require $log 'settings room settings.display.*parity=1'
Write-Output "Downtown normal Release integration passed. Artifacts: $root"
