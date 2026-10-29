; Inno Setup script. Built by installer\build-installer.ps1, which stages the files and passes /DAppVersion.
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif

[Setup]
AppId={{6C1E2B7A-4F5D-4B58-9A0B-5C1F3E2D7A11}
AppName=SciFi Shell
AppVersion={#AppVersion}
AppPublisher=David Fisk
DefaultDirName={autopf}\SciFiShell
DefaultGroupName=SciFi Shell
OutputDir=Output
OutputBaseFilename=SciFiShell-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\SciFiShell.exe
SetupIconFile=..\assets\icon\scifishell.ico
WizardImageFile=..\assets\icon\wizard.bmp
WizardSmallImageFile=..\assets\icon\wizard-small.bmp
CloseApplications=yes
RestartApplications=yes
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "stage\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion createallsubdirs

[Icons]
Name: "{group}\SciFi Shell"; Filename: "{app}\SciFiShell.exe"; WorkingDir: "{app}"
Name: "{group}\Check for SciFi Shell updates"; Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\update.ps1"" -Interactive"; WorkingDir: "{app}"
Name: "{autodesktop}\SciFi Shell"; Filename: "{app}\SciFiShell.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\SciFiShell.exe"; Description: "Launch SciFi Shell"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent
