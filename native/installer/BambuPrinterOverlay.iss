#ifndef MySourceRoot
  #error MySourceRoot must point to the validated package staging directory.
#endif
#ifndef MyOutputDir
  #define MyOutputDir "."
#endif
#ifndef MyVersion
  #define MyVersion "0.3.0"
#endif

[Setup]
AppId={{5F33D178-13DB-4B2D-9E31-BD4BA93BDA01}
AppName=Bambu Printer Overlay
AppVersion={#MyVersion}
AppPublisher=Bambu2OBS contributors
AppPublisherURL=https://github.com/fluidman/Bambu2OBS
DefaultDirName={commonappdata}\obs-studio\plugins
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
OutputDir={#MyOutputDir}
OutputBaseFilename=Bambu-Printer-Overlay-{#MyVersion}-Windows-x64-Setup
UninstallDisplayName=Bambu Printer Overlay
VersionInfoVersion={#MyVersion}.0
VersionInfoDescription=Bambu Printer Overlay OBS plugin installer

[Files]
Source: "{#MySourceRoot}\bambu-printer-overlay\*"; DestDir: "{app}\bambu-printer-overlay"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; The bin folder is the pre-OBS 33 layout used by earlier builds.
Type: filesandordirs; Name: "{app}\bambu-printer-overlay\bin"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\bambu-printer-overlay"

[Code]
function InitializeSetup(): Boolean;
var
  ResultCode: Integer;
begin
  { tasklist exits 0 either way, so find.exe supplies the answer. }
  Exec(ExpandConstant('{cmd}'), '/C tasklist /FI "IMAGENAME eq obs64.exe" | find /I "obs64.exe"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  if ResultCode = 0 then
  begin
    SuppressibleMsgBox('Close OBS Studio before installing Bambu Printer Overlay.', mbError, MB_OK, IDOK);
    Result := False;
  end
  else
    Result := True;
end;
