; The Windows installer (Inno Setup 6): the release folder's files into
; Program Files (or the user's own programs folder, without admin rights),
; Start menu and optional desktop shortcuts, .mvp projects opened by Mutant Video Producer,
; and an uninstaller. The release workflow builds it:
;   iscc /DAppVersion=0.8.0 /DSourceDir=MutantVideoProducer-0.8.0-win64 installer\mvp.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\Release\bin"
#endif

[Setup]
AppId={{6B0E6C1F-5A0F-4C8B-9B57-4D1E0E7C2A31}
AppName=Mutant Video Producer
AppVersion={#AppVersion}
AppVerName=Mutant Video Producer {#AppVersion}
AppPublisher=Mutant Video Producer
AppPublisherURL=https://github.com/jeddyhhh/MutantVideoProducer
AppSupportURL=https://github.com/jeddyhhh/MutantVideoProducer/issues
AppUpdatesURL=https://github.com/jeddyhhh/MutantVideoProducer/releases
DefaultDirName={autopf}\Mutant Video Producer
DefaultGroupName=Mutant Video Producer
; The shared upgrade identity can still have Montage paths saved in the registry.
; Always offer the renamed defaults; the destination remains editable by the user.
UsePreviousAppDir=no
UsePreviousGroup=no
DisableProgramGroupPage=yes
; Program Files for all users by default (asks for admin rights); the first page offers "only for me" instead.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=.
OutputBaseFilename=MutantVideoProducer-{#AppVersion}-win64-setup
SetupIconFile=..\data\mvp.ico
UninstallDisplayIcon={app}\MutantVideoProducer.exe
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; A normal standalone installer: all editor files are embedded in this EXE.
DiskSpanning=no
InfoAfterFile={#SourceDir}\INSTALL-NOTES.txt
ChangesAssociations=yes
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "associate"; Description: "Open .mvp projects with Mutant Video Producer"; GroupDescription: "Files:"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion; Excludes: "mvp_selftest.exe,mvp_*_test.exe,montage.exe,montage_selftest.exe,montage_*_test.exe,*.pdb"

[InstallDelete]
; Only files owned by the previous product in the selected installation.
Type: files; Name: "{app}\montage.exe"
Type: files; Name: "{group}\Montage.lnk"
Type: files; Name: "{group}\Uninstall Montage.lnk"
Type: files; Name: "{autodesktop}\Montage.lnk"

[Icons]
Name: "{group}\Mutant Video Producer"; Filename: "{app}\MutantVideoProducer.exe"
Name: "{group}\Uninstall Mutant Video Producer"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Mutant Video Producer"; Filename: "{app}\MutantVideoProducer.exe"; Tasks: desktopicon

[Registry]
; HKA: the machine's or the user's classes, as installed.
Root: HKA; Subkey: "Software\Classes\.mvp"; Flags: uninsdeletekeyifempty; Tasks: associate
Root: HKA; Subkey: "Software\Classes\.mvp\OpenWithProgids"; ValueType: string; ValueName: "MutantVideoProducer.Project"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: associate
Root: HKA; Subkey: "Software\Classes\MutantVideoProducer.Project"; ValueType: string; ValueName: ""; ValueData: "Mutant Video Producer Project"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\MutantVideoProducer.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\MutantVideoProducer.exe,0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\MutantVideoProducer.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\MutantVideoProducer.exe"" ""%1"""; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Applications\MutantVideoProducer.exe\SupportedTypes"; ValueType: string; ValueName: ".mvp"; ValueData: ""; Flags: uninsdeletekey

[Run]
Filename: "{app}\MutantVideoProducer.exe"; Description: "{cm:LaunchProgram,Mutant Video Producer}"; Flags: nowait postinstall skipifsilent runasoriginaluser
