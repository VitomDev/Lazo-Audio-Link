; Lazo Audio Link installer (Inno Setup 6).
;
; Per-user install without administrator rights. "Start with Windows" is off
; until it is turned on in Lazo's Settings; upgrades keep whatever the user
; chose. A silent upgrade starts Lazo again, in the background, only if it was
; running before. Uninstalling removes the startup entry and the settings.
;
; Built by build.ps1, which passes /DBuildDir=<folder with Lazo.exe>.

#define AppVersion "1.1.1"
#define AppName "Lazo Audio Link"
#define RunKey "Software\Microsoft\Windows\CurrentVersion\Run"
#define StartupApprovedKey "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"

#ifndef BuildDir
  #define BuildDir "..\build\app"
#endif

[Setup]
AppId={{533A70FB-B169-4958-AF60-1AD99C6ACBE0}
AppName={#AppName}
AppVerName={#AppName} {#AppVersion}
AppVersion={#AppVersion}
AppPublisher=VitomDev
AppPublisherURL=https://github.com/VitomDev/Lazo-Audio-Link
AppSupportURL=https://github.com/VitomDev/Lazo-Audio-Link/issues
AppCopyright=Copyright (c) 2026 VitomDev
VersionInfoVersion={#AppVersion}
VersionInfoCompany=VitomDev
VersionInfoDescription={#AppName} - PC audio to your phone Setup
DefaultDirName={localappdata}\Programs\{#AppName}
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputBaseFilename=LazoSetup
SetupIconFile=..\assets\lazo.ico
UninstallDisplayIcon={app}\Lazo.exe
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE
; Lazo is closed by the code below, through its own window.
CloseApplications=no

[Messages]
; Shown above the license on the wizard's license page.
LicenseLabel3=Lazo is free and open source. If you paid for it, you were scammed: the official download is only at github.com/VitomDev. Please read the license below and accept it to continue.

[Files]
Source: "{#BuildDir}\Lazo.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\Lazo.exe"

[Run]
; The usual "Launch" checkbox on the last page, checked by default.
Filename: "{app}\Lazo.exe"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent
; Silent upgrades (Vitool Box) bring back a Lazo that was running, quietly.
Filename: "{app}\Lazo.exe"; Parameters: "--background"; Flags: nowait; Check: WizardSilent and WasRunning

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\{#AppName}"

[Code]
var
  LazoWasRunning: Boolean;

// Lazo stops streaming, gives the speakers back and exits when its tray
// window gets WM_CLOSE.
procedure CloseRunningLazo;
var
  Window: HWND;
  Attempts: Integer;
begin
  Window := FindWindowByClassName('LazoAudioLinkTray');
  if Window = 0 then Exit;
  LazoWasRunning := True;
  PostMessage(Window, 16 { WM_CLOSE }, 0, 0);
  Attempts := 0;
  while (FindWindowByClassName('LazoAudioLinkTray') <> 0) and (Attempts < 50) do
  begin
    Sleep(100);
    Attempts := Attempts + 1;
  end;
  // Give the process a moment to exit after its window is gone.
  Sleep(300);
end;

function WasRunning: Boolean;
begin
  Result := LazoWasRunning;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  CloseRunningLazo;
  Result := '';
end;

function AutostartCommand: String;
begin
  Result := '"' + ExpandConstant('{app}\Lazo.exe') + '" --background';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  // Start with Windows stays off on a fresh install; an upgrade only
  // refreshes the path of an entry the user turned on.
  if (CurStep = ssPostInstall) and RegValueExists(HKCU, '{#RunKey}', '{#AppName}') then
    RegWriteStringValue(HKCU, '{#RunKey}', '{#AppName}', AutostartCommand);
end;

function InitializeUninstall: Boolean;
begin
  CloseRunningLazo;
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    RegDeleteValue(HKCU, '{#RunKey}', '{#AppName}');
    RegDeleteValue(HKCU, '{#StartupApprovedKey}', '{#AppName}');
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\{#AppName}');
  end;
end;
