# Runs the normal Release build on a private Windows desktop with a separate profile.
# Does not activate the installed shell, use its profile, or inject physical input.
param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
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
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
    static extern IntPtr CreateFile(string p,uint a,uint s,ref Security sec,uint c,uint f,IntPtr t);
    public static int Run(string exe,string log,string cwd) {
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
                    if(WaitForSingleObject(pi.process,90000)!=0) {
                        TerminateProcess(pi.process,99); WaitForSingleObject(pi.process,5000); return 99;
                    }
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

function Run-Scenario([string]$name, [string[]]$commands) {
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
        $code = [DowntownDesktop]::Run($exe, (Join-Path $root "$name-app.log"), (Split-Path -Parent $exe))
    } finally {
        Get-ChildItem Env: | Where-Object { $_.Name -like 'SCIFI_*' -or $_.Name -in $names } |
            ForEach-Object { Remove-Item -LiteralPath ("Env:\"+$_.Name) }
        foreach ($key in $saved.Keys) { Set-Item -LiteralPath ("Env:\"+$key) -Value $saved[$key] }
    }
    if ($code -ne 0) { throw "$name exited $code; see $root" }
    $log = @(Get-Content -LiteralPath $control | Select-Object -Skip $previousLines)
    if ($log -match 'not found|pick blocked|pick failed|entrance unavailable|card unavailable|layout_clear=0|collision_safe=0') { throw "Interaction failed: $root" }
    return $log
}
function Require([string[]]$log, [string]$pattern) {
    if (!($log -match $pattern)) { throw "Missing $pattern; see $root" }
}
$log = Run-Scenario 'builtins' @(
    'wait 2', 'downtownstatus', 'downtownclick VISIT CAR AND TRACK', 'downtownenter org.scifishell.driving',
    'downtowncard 2', 'downtownstatus', 'downtowncard 1', 'pluginstatus', 'downtownstatus',
    'settingsclose', 'downtownenter org.scifishell.driving', 'downtowncard 1', 'pluginstatus',
    'settingsclose', 'downtownclick VISIT PARKS AND TOYS', 'downtownenter org.scifishell.parks',
    'downtowncard 2', 'downtownstatus', 'downtowncard 1', 'pluginstatus',
    'settingsclose', 'downtownenter org.scifishell.parks', 'downtowncard 1', 'pluginstatus', 'quit'
)
Require $log 'bounds=-68,4,108,148'
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
Require $log 'com.example.downtown=3\(custom\).*com.example.clock=4\(default\)|com.example.clock=4\(default\).*com.example.downtown=3\(custom\)'
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
Require $log 'com.example.clock=4\(default\).*com.example.downtown=3\(custom\)'
$last = ($log | Where-Object { $_ -like 'downtown *' })[-1]
if ($last -match 'com.example.clock=|com.example.downtown=') { throw "Removed buildings remain: $root" }
$log = Run-Scenario 'gap-fill' @('wait 2', "plugininstall $defaultZip", 'downtownstatus', 'quit')
Require $log 'com.example.clock=3\(default\)'
Write-Output "Downtown normal Release integration passed. Artifacts: $root"
