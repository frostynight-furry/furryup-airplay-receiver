[Setup]
AppName=AirPlay Live Source
AppId={{57FCC983-B85E-4652-87D6-4FC2A8A53BC0}
AppVersion=0.1.0
DefaultDirName={commonappdata}\obs-studio\plugins\airplay-live-source
DirExistsWarning=no
DisableDirPage=yes
UsePreviousAppDir=no
OutputDir=.
OutputBaseFilename=AirPlay-Live-Source-Setup-v0.1.0
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
UninstallDisplayName=AirPlay Live Source
WizardStyle=modern
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Files]
Source: "artifact\airplay-live-source.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "artifact\libcrypto-3*-x64.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "artifact\ffmpeg-sdk-buildconf.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "artifact\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "artifact\UPSTREAM-LGPL-NOTICE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "artifact\GPL-3.0.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "artifact\LGPL-2.1-or-later.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "artifact\OPENSSL-LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "artifact\NOTICE.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "artifact\RELEASE-GATE.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "artifact\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "artifact\data\locale\*.ini"; DestDir: "{app}\data\locale"; Flags: ignoreversion

[Messages]
WelcomeLabel2=INTERNAL ALPHA - NOT CLEARED FOR PUBLIC DISTRIBUTION.%n%nThis installs AirPlay Live Source. It receives iPhone, iPad, and Mac screen mirroring directly in OBS without a capture window.%n%nRequirements:%n- OBS Studio 32.2.2%n- Apple Bonjour (install iTunes or Bonjour Print Services)%n%nClose uxplay-windows before using this plugin.
[Code]
const
  ObsUninstallKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio';

function ExpectedInstallDir: String;
begin
  Result := ExpandConstant('{commonappdata}') +
    '\obs-studio\plugins\airplay-live-source';
end;

function TryReadObsRegistration(RootKey: Integer;
  var DisplayVersion, DisplayIcon: String): Boolean;
begin
  Result := RegQueryStringValue(RootKey, ObsUninstallKey,
    'DisplayVersion', DisplayVersion) and
    RegQueryStringValue(RootKey, ObsUninstallKey,
    'DisplayIcon', DisplayIcon);
end;

function FindObsHost(var DisplayVersion, ObsExe: String): Boolean;
var
  ExeEnd: Integer;
begin
  Result := TryReadObsRegistration(HKLM32, DisplayVersion, ObsExe);
  if not Result then
    Result := TryReadObsRegistration(HKCU32, DisplayVersion, ObsExe);
  if not Result then
    Result := TryReadObsRegistration(HKLM64, DisplayVersion, ObsExe);
  if not Result then
    Result := TryReadObsRegistration(HKCU64, DisplayVersion, ObsExe);
  if not Result then
    Exit;

  ExeEnd := Pos('.exe', Lowercase(ObsExe));
  if ExeEnd > 0 then
    ObsExe := Copy(ObsExe, 1, ExeEnd + 3);
  StringChangeEx(ObsExe, '"', '', True);
  ObsExe := Trim(ObsExe);
  Result := FileExists(ObsExe);
end;

function FileHasVersion(const FileName, ExpectedVersion: String): Boolean;
var
  ActualVersion: String;
begin
  Result := FileExists(FileName) and
    GetVersionNumbersString(FileName, ActualVersion) and
    (CompareText(ActualVersion, ExpectedVersion) = 0);
end;

function ValidateObsHost: Boolean;
var
  DisplayVersion, ObsExe, ObsBin: String;
begin
  Result := False;
  if not FindObsHost(DisplayVersion, ObsExe) then
  begin
    MsgBox('OBS Studio was not found in the Windows installed-program ' +
      'registry. This internal alpha only installs into a registered ' +
      'OBS Studio 32.2.2 host.', mbError, MB_OK);
    Exit;
  end;
  if CompareText(DisplayVersion, '32.2.2') <> 0 then
  begin
    MsgBox('Unsupported OBS Studio version: ' + DisplayVersion + '.' +
      #13#10 + 'This internal alpha requires exactly OBS Studio 32.2.2.',
      mbError, MB_OK);
    Exit;
  end;

  ObsBin := ExtractFileDir(ObsExe);
  if not FileHasVersion(ObsExe, '32.2.2.0') or
    not FileHasVersion(ObsBin + '\avcodec-62.dll', '62.28.102.0') or
    not FileHasVersion(ObsBin + '\avutil-60.dll', '60.26.102.0') or
    not FileHasVersion(ObsBin + '\swscale-9.dll', '9.5.102.0') or
    not FileHasVersion(ObsBin + '\swresample-6.dll', '6.3.102.0') then
  begin
    MsgBox('The registered OBS installation does not contain the exact ' +
      'OBS 32.2.2 / FFmpeg n8.1.2 runtime validated for this alpha.' +
      #13#10 + 'No files were installed.', mbError, MB_OK);
    Exit;
  end;
  Result := True;
end;

function InitializeSetup: Boolean;
var
  LegacyPath: String;
begin
  LegacyPath := ExpandConstant('{commonappdata}') +
    '\obs-studio\plugins\obs-airplay-receiver';
  if DirExists(LegacyPath) then
  begin
    MsgBox('The upstream OBS AirPlay Receiver plugin is still installed.' +
      #13#10 + 'Uninstall it before installing AirPlay Live Source; both ' +
      'plugins use the same AirPlay ports.', mbError, MB_OK);
    Result := False;
    Exit;
  end;
  Result := ValidateObsHost;
end;

procedure InitializeWizard;
begin
  { Ignore a previous install path and any /DIR override. }
  WizardForm.DirEdit.Text := ExpectedInstallDir;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  if CompareText(ExpandConstant('{app}'), ExpectedInstallDir) <> 0 then
    Result := 'Unsafe installation directory was rejected. The plugin must ' +
      'be installed only under the fixed ProgramData OBS plugin directory.'
  else
    Result := '';
end;

function ExecuteNetsh(const Parameters: String): Boolean;
var
  ResultCode: Integer;
begin
  ResultCode := -1;
  Result := Exec(ExpandConstant('{sys}\netsh.exe'), Parameters, '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  UdpOK, TcpOK: Boolean;
begin
  if CurStep = ssPostInstall then
  begin
    ExecuteNetsh('advfirewall firewall delete rule ' +
      'name="AirPlay Live Source (UDP)"');
    ExecuteNetsh('advfirewall firewall delete rule ' +
      'name="AirPlay Live Source (TCP)"');
    UdpOK := ExecuteNetsh('advfirewall firewall add rule ' +
      'name="AirPlay Live Source (UDP)" dir=in action=allow protocol=UDP ' +
      'localport=6000-6001,7011 profile=private');
    TcpOK := ExecuteNetsh('advfirewall firewall add rule ' +
      'name="AirPlay Live Source (TCP)" dir=in action=allow protocol=TCP ' +
      'localport=7000,7100 profile=private');
    if not (UdpOK and TcpOK) then
      MsgBox('The plugin files were installed, but Windows Firewall rules ' +
        'could not be added. AirPlay discovery may work while streaming ' +
        'fails. Re-run this installer as administrator.', mbError, MB_OK);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  UdpOK, TcpOK: Boolean;
begin
  if CurUninstallStep = usUninstall then
  begin
    UdpOK := ExecuteNetsh('advfirewall firewall delete rule ' +
      'name="AirPlay Live Source (UDP)"');
    TcpOK := ExecuteNetsh('advfirewall firewall delete rule ' +
      'name="AirPlay Live Source (TCP)"');
    if not (UdpOK and TcpOK) then
      SuppressibleMsgBox('Plugin removal will continue, but one or more ' +
        'Windows Firewall rules could not be removed. Remove the two ' +
        '"AirPlay Live Source" rules manually.', mbError, MB_OK, IDOK);
  end;
end;

[InstallDelete]
; Remove same-named FFmpeg DLLs left by an earlier internal-alpha installer.
; The current build must resolve these modules from the OBS 32.2.2 host.
Type: files; Name: "{app}\bin\64bit\avcodec-62.dll"
Type: files; Name: "{app}\bin\64bit\avutil-60.dll"
Type: files; Name: "{app}\bin\64bit\swscale-9.dll"
Type: files; Name: "{app}\bin\64bit\swresample-6.dll"
