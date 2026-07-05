;
; HomeFaceLogon - Inno Setup Script
;

[Setup]
AppName=HomeFaceLogon
AppVersion=0.3.2
AppPublisher=HomeFaceLogon Team
DefaultDirName={commonpf}\HomeFaceLogon
DefaultGroupName=HomeFaceLogon
OutputDir=..\installer_output
OutputBaseFilename=HomeFaceLogonSetup-v0.3.2
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

; ONNX Models
Source: "..\models\face_detection_yunet_2023mar.onnx"; DestDir: "{commonappdata}\HomeFaceLogon\models"; Flags: ignoreversion
Source: "..\models\face_recognition_sface_2021dec.onnx"; DestDir: "{commonappdata}\HomeFaceLogon\models"; Flags: ignoreversion

[Icons]
Name: "{group}\HomeFaceLogon 管理ツール"; Filename: "{app}\HomeFaceLogon.exe"
Name: "{group}\アンインストール"; Filename: "{uninstallexe}"
Name: "{commondesktop}\HomeFaceLogon 管理ツール"; Filename: "{app}\HomeFaceLogon.exe"; Tasks: desktopicon

[Registry]
; Create HomeFaceLogon App Registry Key
Root: HKLM; Subkey: "SOFTWARE\HomeFaceLogon"; ValueType: dword; ValueName: "Enabled"; ValueData: "0"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\HomeFaceLogon"; ValueType: string; ValueName: "Path"; ValueData: "{app}"; Flags: uninsdeletekey

[Run]
; Register the Credential Provider COM DLL
Filename: "regsvr32.exe"; Parameters: "/s ""{app}\FaceLogonProvider.dll"""; StatusMsg: "Credential Provider を登録しています..."; Flags: runhidden

; Launch the Tauri setup wizard manager at the end of installation
Filename: "{app}\HomeFaceLogon.exe"; Description: "HomeFaceLogon 管理ツールを起動して初期セットアップを行う"; Flags: postinstall nowait runascurrentuser

[UninstallRun]
; Unregister the Credential Provider COM DLL during uninstall
Filename: "regsvr32.exe"; Parameters: "/u /s ""{app}\FaceLogonProvider.dll"""; RunOnceId: "UnregisterProvider"; Flags: runhidden

[UninstallDelete]
Type: files; Name: "{commonappdata}\HomeFaceLogon\config.json"
Type: files; Name: "{commonappdata}\HomeFaceLogon\secret.bin"
Type: files; Name: "{commonappdata}\HomeFaceLogon\pin.bin"
Type: files; Name: "{commonappdata}\HomeFaceLogon\face.bin"
Type: filesandordirs; Name: "{commonappdata}\HomeFaceLogon\logs"
Type: dirifempty; Name: "{commonappdata}\HomeFaceLogon\models"
Type: dirifempty; Name: "{commonappdata}\HomeFaceLogon"
