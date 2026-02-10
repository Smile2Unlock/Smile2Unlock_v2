; Smile2Unlock Inno Setup Script
#define MyAppName "Smile2Unlock"
#define MyAppVersion "2.0"
#define MyAppPublisher "Smile2Unlock Team"
#define MyAppURL "https://github.com/Smile2Unlock/Smile2Unlock_v2"
#define MyAppExeName "SampleV2CredentialProvider.dll"

[Setup]
AppId={{5FD3D285-0DD9-4362-8855-E0ABAACD4AF6}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DisableDirPage=yes
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
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

[Files]
; Copy all files from installer-files directory
Source: "installer-files\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; Copy the main DLL to System32
Source: "installer-files\SampleV2CredentialProvider.dll"; DestDir: "{sys}"; Flags: ignoreversion 64bit

[Registry]
; Register the Credential Provider
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\{{5fd3d285-0dd9-4362-8855-e0abaacd4af6}}"; ValueType: string; ValueName: ""; ValueData: "SampleV2CredentialProvider"; Flags: uninsdeletekey
Root: HKCR; Subkey: "CLSID\{{5fd3d285-0dd9-4362-8855-e0abaacd4af6}}"; ValueType: string; ValueName: ""; ValueData: "SampleV2CredentialProvider"; Flags: uninsdeletekey
Root: HKCR; Subkey: "CLSID\{{5fd3d285-0dd9-4362-8855-e0abaacd4af6}}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "SampleV2CredentialProvider.dll"; Flags: uninsdeletevalue
Root: HKCR; Subkey: "CLSID\{{5fd3d285-0dd9-4362-8855-e0abaacd4af6}}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"; Flags: uninsdeletevalue

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
  if not IsAdminLoggedOn then
  begin
    MsgBox('This installer requires administrative privileges.', mbError, MB_OK);
    Result := False;
  end;
end;

