; Instalador do Qlip (Inno Setup 6). Gerado por scripts/build-release.ps1:
;   ISCC.exe /DAppVersion=0.2.0 installer\Qlip.iss
; Instala por usuário (sem pedir administrador) em %LOCALAPPDATA%\Programs\Qlip.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#define SourceDir "..\dist\Qlip-portable"

[Setup]
AppId={{6F1C4E2A-9B7D-4C38-A5E1-2D8F0B9C7A41}
AppName=Qlip
AppVersion={#AppVersion}
AppVerName=Qlip {#AppVersion}
AppPublisher=Pedro Queiroz
AppPublisherURL=https://qlipapp.vercel.app
AppSupportURL=https://github.com/PQueirozDev/Qlip/issues
AppUpdatesURL=https://github.com/PQueirozDev/Qlip/releases
DefaultDirName={localappdata}\Programs\Qlip
DefaultGroupName=Qlip
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 2004 ou mais novo (captura WGC e overlay fora da gravação).
MinVersion=10.0.19041
SetupIconFile=..\assets\icons\qlip.ico
UninstallDisplayIcon={app}\Qlip.exe
UninstallDisplayName=Qlip
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputDir=..\dist
OutputBaseFilename=Qlip-Setup
CloseApplications=force
RestartApplications=no

[Languages]
Name: "ptbr"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Tasks]
Name: "desktopicon"; Description: "Criar atalho na área de trabalho"; GroupDescription: "Atalhos:"
Name: "startup"; Description: "Iniciar o Qlip com o Windows"; GroupDescription: "Inicialização:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\Qlip.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\THIRD_PARTY.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\FFMPEG-LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Qlip"; Filename: "{app}\Qlip.exe"; Comment: "Gravador de tela e replay instantâneo"
Name: "{autodesktop}\Qlip"; Filename: "{app}\Qlip.exe"; Tasks: desktopicon

[Registry]
; Mesmo valor que o próprio app grava ao marcar "Iniciar com o Windows".
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Qlip"; ValueData: """{app}\Qlip.exe"""; Tasks: startup

[Run]
Filename: "{app}\Qlip.exe"; Description: "Abrir o Qlip agora"; Flags: nowait postinstall skipifsilent

[Code]
// Fecha o Qlip que está aberto do jeito normal (como "Sair" na bandeja): a gravação em andamento é finalizada.
// Versões sem suporte à mensagem (0.1.0) ou que não fecham a tempo: encerra à força só a cópia instalada.
function CloseQlip(): Boolean;
var
  Wnd: HWND;
  I, ResultCode: Integer;
begin
  Result := True;
  Wnd := FindWindowByClassName('QlipMainWindow');
  if Wnd = 0 then
    Exit;
  PostMessage(Wnd, RegisterWindowMessage('Qlip.RequestExit'), 0, 0);
  for I := 1 to 100 do
  begin
    Sleep(200);
    if FindWindowByClassName('QlipMainWindow') = 0 then
      Exit;
  end;
  Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    '-NoProfile -NonInteractive -Command "Get-Process Qlip -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq ''' +
    ExpandConstant('{app}\Qlip.exe') + ''' } | Stop-Process -Force"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := FindWindowByClassName('QlipMainWindow') = 0;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  CloseQlip();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    CloseQlip();
  // A inicialização com o Windows pode ter sido ligada pelo app: remove sempre ao desinstalar.
  // Configurações (%APPDATA%\Qlip) e vídeos ficam no PC.
  if CurUninstallStep = usPostUninstall then
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Qlip');
end;
