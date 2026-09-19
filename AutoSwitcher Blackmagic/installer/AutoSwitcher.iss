[Setup]
AppName=AutoSwitcher
AppVersion=1.0.0
AppPublisher=AutoSwitcher
DefaultDirName={localappdata}\Programs\AutoSwitcher
DefaultGroupName=AutoSwitcher
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=C:\Users\miigl\Dev\AutoSwitcher\installer
OutputBaseFilename=AutoSwitcher-Setup
SetupIconFile=C:\Users\miigl\Dev\AutoSwitcher\installer\icon.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Files]
Source: "C:\Users\miigl\Dev\AutoSwitcher\dist\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Tasks]
Name: "desktopicon"; Description: "Crear acceso directo en el escritorio"; GroupDescription: "Accesos directos:"

[Icons]
Name: "{group}\AutoSwitcher"; Filename: "{app}\AutoSwitcher.exe"
Name: "{userdesktop}\AutoSwitcher"; Filename: "{app}\AutoSwitcher.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\AutoSwitcher.exe"; Description: "Abrir AutoSwitcher ahora"; Flags: nowait postinstall skipifsilent
