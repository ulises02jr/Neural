; ============================================================
;  NeuralWorship - Instalador para Windows (Inno Setup 6)
;  Genera:  installer\NeuralWorship-Setup.exe
;
;  ANTES de compilar este .iss, corré  build_windows.bat
;  (crea la carpeta  dist\NeuralWorship  que este instalador empaqueta).
; ============================================================

#define MyAppName "NeuralWorship"
#define MyAppVersion "1.0"
#define MyAppPublisher "Mi Iglesia Internacional"
#define MyAppExeName "NeuralWorship.exe"

[Setup]
; AppId identifica la app para futuras actualizaciones/desinstalación (no cambiar entre versiones)
AppId={{8F3B1C2A-6D4E-4A7B-9C10-2E5F7A9B3D61}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=installer
OutputBaseFilename=NeuralWorship-Setup
SetupIconFile=icono.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64
; Program Files requiere permisos de administrador
PrivilegesRequired=admin
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
; Inno Setup 6 incluye Spanish.isl en su carpeta Languages.
; Si diera error "file not found", cambiá esta línea por:  MessagesFile: "compiler:Default.isl"
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
Name: "desktopicon"; Description: "Crear un acceso directo en el Escritorio"; GroupDescription: "Accesos directos:"; Flags: checkedonce

[Files]
; Empaqueta TODA la carpeta que genera PyInstaller (exe + _internal con recursos)
Source: "dist\NeuralWorship\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion
; WebView2 Runtime: bootstrapper oficial de Microsoft (redistribuible, ~2 MB).
; Descargalo UNA vez y ponelo en la carpeta  redist\  antes de compilar.
; Se instala solo si la PC no lo tiene.
Source: "redist\MicrosoftEdgeWebview2Setup.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall; Check: WebView2Missing

[Icons]
Name: "{group}\NeuralWorship"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Desinstalar NeuralWorship"; Filename: "{uninstallexe}"
Name: "{autodesktop}\NeuralWorship"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; 1) Instala WebView2 en silencio si falta (necesario para la ventana de la app)
Filename: "{tmp}\MicrosoftEdgeWebview2Setup.exe"; Parameters: "/silent /install"; StatusMsg: "Instalando componente de Microsoft (WebView2)..."; Check: WebView2Missing
; 2) Abre la app al terminar
Filename: "{app}\{#MyAppExeName}"; Description: "Abrir NeuralWorship ahora"; Flags: nowait postinstall skipifsilent

[Code]
function WebView2Missing: Boolean;
var v: string;
begin
  { Detecta el WebView2 Runtime por su client-id conocido (HKLM 64/32 y HKCU) }
  Result := True;
  if RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', v) and (v <> '') then
    Result := False
  else if RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', v) and (v <> '') then
    Result := False
  else if RegQueryStringValue(HKCU, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', v) and (v <> '') then
    Result := False;
end;
