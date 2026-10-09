;
; HomeFaceLogon - Inno Setup Script
;

[Setup]
AppName=HomeFaceLogon
AppVersion=0.3.6
AppPublisher=HomeFaceLogon Team
DefaultDirName={commonpf}\HomeFaceLogon
DefaultGroupName=HomeFaceLogon
OutputDir=..\installer_output
OutputBaseFilename=HomeFaceLogonSetup-v0.3.6-auth-lifecycle
Compression=lzma2/max
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64
PrivilegesRequired=admin
SetupIconFile=..\src\gui\src-tauri\icons\icon.ico

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Dirs]
Name: "{commonappdata}\HomeFaceLogon"; Permissions: system-full admins-full users-readexec
Name: "{commonappdata}\HomeFaceLogon\models"; Permissions: system-full admins-full users-readexec
Name: "{commonappdata}\HomeFaceLogon\logs"; Permissions: system-full admins-full users-readexec

[Files]
; C++ binaries
Source: "..\x64\Release\FaceLogonProvider.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace
Source: "..\x64\Release\FaceLogonHost.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\x64\Release\FaceLogonSetup.exe"; DestDir: "{app}"; Flags: ignoreversion

; OpenCV Release DLLs
Source: "..\x64\Release\opencv_world4100.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\x64\Release\opencv_videoio_msmf4100_64.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\x64\Release\opencv_videoio_ffmpeg4100_64.dll"; DestDir: "{app}"; Flags: ignoreversion

; Tauri Manager GUI app
Source: "..\src\gui\src-tauri\target\release\app.exe"; DestDir: "{app}"; DestName: "HomeFaceLogon.exe"; Flags: ignoreversion
Source: "..\src\provider\icons\LICENSE-lucide.txt"; DestDir: "{app}"; Flags: ignoreversion

; ONNX Models
Source: "..\models\face_detection_yunet_2023mar.onnx"; DestDir: "{commonappdata}\HomeFaceLogon\models"; Flags: ignoreversion
Source: "..\models\face_recognition_sface_2021dec.onnx"; DestDir: "{commonappdata}\HomeFaceLogon\models"; Flags: ignoreversion

[Icons]
Name: "{group}\HomeFaceLogon 管理ツール"; Filename: "{app}\HomeFaceLogon.exe"
Name: "{group}\アンインストール"; Filename: "{uninstallexe}"
Name: "{commondesktop}\HomeFaceLogon 管理ツール"; Filename: "{app}\HomeFaceLogon.exe"; Tasks: desktopicon

[Registry]
; Create HomeFaceLogon App Registry Key
Root: HKLM; Subkey: "SOFTWARE\HomeFaceLogon"; ValueType: dword; ValueName: "Enabled"; ValueData: "0"; Flags: createvalueifdoesntexist uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\HomeFaceLogon"; ValueType: string; ValueName: "Path"; ValueData: "{app}"; Flags: uninsdeletekey

; The provider exports class-factory functions, not DllRegisterServer.
; Register its COM class and credential-provider tile directly in the 64-bit registry.
Root: HKLM64; Subkey: "SOFTWARE\Classes\CLSID\{{5fd3d285-0dd9-4362-8855-e0abaacd4af6}"; ValueType: string; ValueName: ""; ValueData: "HomeFaceLogon Credential Provider"; Flags: uninsdeletekey
Root: HKLM64; Subkey: "SOFTWARE\Classes\CLSID\{{5fd3d285-0dd9-4362-8855-e0abaacd4af6}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\FaceLogonProvider.dll"
Root: HKLM64; Subkey: "SOFTWARE\Classes\CLSID\{{5fd3d285-0dd9-4362-8855-e0abaacd4af6}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
Root: HKLM64; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\{{5fd3d285-0dd9-4362-8855-e0abaacd4af6}"; ValueType: string; ValueName: ""; ValueData: "HomeFaceLogon Credential Provider"; Flags: uninsdeletekey

[Run]
; Launch the Tauri setup wizard manager at the end of installation
Filename: "{app}\HomeFaceLogon.exe"; Description: "HomeFaceLogon 管理ツールを起動して初期セットアップを行う"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
Type: files; Name: "{commonappdata}\HomeFaceLogon\config.json"
Type: files; Name: "{commonappdata}\HomeFaceLogon\secret.bin"
Type: files; Name: "{commonappdata}\HomeFaceLogon\pin.bin"
Type: files; Name: "{commonappdata}\HomeFaceLogon\face.bin"
Type: filesandordirs; Name: "{commonappdata}\HomeFaceLogon\logs"
Type: dirifempty; Name: "{commonappdata}\HomeFaceLogon\models"
Type: dirifempty; Name: "{commonappdata}\HomeFaceLogon"
