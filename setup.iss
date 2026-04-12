; Smile2Unlock Inno Setup Script
#define MyAppName "Smile2Unlock"
#define VersionFileHandle FileOpen(AddBackslash(SourcePath) + "version.txt")
#if VersionFileHandle == 0
  #error "Failed to open version.txt"
#endif
#define MyAppVersion Trim(FileRead(VersionFileHandle))
#expr FileClose(VersionFileHandle)
#define MyAppPublisher "Smile2Unlock Team"
#define MyAppURL "https://github.com/Smile2Unlock/Smile2Unlock_v2"
#define MyAppExeName "Smile2Unlock.exe"

[Setup]
AppId={{5FD3D285-0DD9-4362-8855-E0ABAACD4AF6}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
LicenseFile=LICENSE
OutputDir=Output
OutputBaseFilename=Smile2Unlock-Setup
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
UninstallDisplayIcon={sys}\SampleV2CredentialProvider.dll

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: checkedonce

[Files]
; Copy all files from installer-files directory
Source: "installer-files\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; Copy the main DLL to System32
Source: "installer-files\SampleV2CredentialProvider.dll"; DestDir: "{sys}"; Flags: ignoreversion 64bit

[Icons]
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Check: ShouldCreateStartMenuShortcut

[Registry]
; Register the Credential Provider
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\{{5fd3d285-0dd9-4362-8855-e0abaacd4af6}}"; ValueType: string; ValueName: ""; ValueData: "SampleV2CredentialProvider"; Flags: uninsdeletekey
Root: HKCR; Subkey: "CLSID\{{5fd3d285-0dd9-4362-8855-e0abaacd4af6}}"; ValueType: string; ValueName: ""; ValueData: "SampleV2CredentialProvider"; Flags: uninsdeletekey
Root: HKCR; Subkey: "CLSID\{{5fd3d285-0dd9-4362-8855-e0abaacd4af6}}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "SampleV2CredentialProvider.dll"; Flags: uninsdeletevalue
Root: HKCR; Subkey: "CLSID\{{5fd3d285-0dd9-4362-8855-e0abaacd4af6}}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"; Flags: uninsdeletevalue
; Remove application configuration on uninstall
Root: HKLM; Subkey: "SOFTWARE\Smile2Unlock_v2"; ValueType: string; ValueName: "__installer_cleanup__"; ValueData: ""; Flags: uninsdeletekey

[UninstallDelete]
; Remove the whole installation directory, including files generated after install
Type: filesandordirs; Name: "{app}"

[Code]
function ShouldCreateStartMenuShortcut(): Boolean;
begin
  Result := ExpandConstant('{param:CreateStartMenuShortcut|1}') = '1';
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not IsAdminLoggedOn then
  begin
    MsgBox('This installer requires administrative privileges.', mbError, MB_OK);
    Result := False;
  end;
end;

