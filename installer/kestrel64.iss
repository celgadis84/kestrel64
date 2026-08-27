; Instalador de Windows para kestrel64 (Inno Setup 6).
;
; Se compila sobre lo que deje scripts/dist.sh, no sobre el arbol de build: asi el instalador
; y el zip portable llevan exactamente los mismos bytes, y no hay una tercera lista de DLL
; que se pueda quedar desincronizada.
;
;   sh scripts/dist.sh
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\kestrel64.iss
;
; NOTA sobre Vulkan: vulkan-1.dll NO se instala. La pone el driver de la GPU, y sobrescribir
; la del sistema con una copia ajena rompe otras aplicaciones. Si falta, el arreglo es
; actualizar el driver de video, no copiar la DLL.

#define AppName    "kestrel64"
#define AppVer     "0.0.1-M1"
#define AppExe     "kestrel64.exe"

[Setup]
AppId={{9C1F5A62-7C1D-4E56-9A0B-6E5B2C4D8F31}
AppName={#AppName}
AppVersion={#AppVer}
AppPublisher=Santiago Nieto
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
OutputDir=..
OutputBaseFilename={#AppName}-{#AppVer}-setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#AppExe}

[Languages]
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked
Name: "assoc";       Description: "Abrir las ROM .z64 / .n64 / .v64 con {#AppName}"; Flags: unchecked

[Files]
; dist\ entero: el .exe y las DLL del toolchain que dist.sh haya resuelto (ninguna si se
; compilo con -DKESTREL_STATIC=ON).
Source: "..\dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{group}\{#AppName}";           Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}";     Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; Asociacion de ROM. El emulador arranca en pausa sin --run, asi que el verbo lo lleva puesto:
; abrir una ROM desde el Explorador tiene que EJECUTARLA, no dejar una ventana negra.
Root: HKA; Subkey: "Software\Classes\.z64\OpenWithProgids"; ValueType: string; ValueName: "kestrel64.rom"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.n64\OpenWithProgids"; ValueType: string; ValueName: "kestrel64.rom"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.v64\OpenWithProgids"; ValueType: string; ValueName: "kestrel64.rom"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\kestrel64.rom"; ValueType: string; ValueName: ""; ValueData: "ROM de Nintendo 64"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\kestrel64.rom\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\kestrel64.rom\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" --run ""%1"""; Tasks: assoc

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
