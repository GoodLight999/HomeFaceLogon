# Walkthrough - Phase 5 Implementation Complete (PIN Fallback, Security Hardening, GUI, and Installer)

We have completed the implementation of **Phase 5: Product Level Finalization** for the `HomeFaceLogon` project!

---

## 🛠️ Summary of Changes

### 1. PIN Fallback Authentication (Phase A)
- **Changes**:
  - Replaced the Microsoft sample password field with a dedicated **PIN input field** (labeled "PINを入力") in [common.h](file:///c:/Users/nakan/Desktop/Workspace/HomeFaceLogon/src/provider/common.h).
  - Implemented `VerifyPin` in [config.cpp](file:///c:/Users/nakan/Desktop/Workspace/HomeFaceLogon/src/provider/config.cpp) using **PBKDF2-SHA256** and constant-time comparison to prevent timing attacks.
  - Updated `GetSerialization` in [CSampleCredential.cpp](file:///c:/Users/nakan/Desktop/Workspace/HomeFaceLogon/src/provider/CSampleCredential.cpp) to verify the entered PIN first. If verification succeeds, it decrypts and submits the credentials. If it fails, it tracks attempts.
  - Implemented **brute-force lockout** (5 attempts limit, followed by a 30-second lockout) with live status warnings on the logon tile.
  - Extended `FaceLogonSetup.exe` ([src/setup/main.cpp](file:///c:/Users/nakan/Desktop/Workspace/HomeFaceLogon/src/setup/main.cpp)) to accept a `--pin` argument and prompt interactively for PIN inputs (validating length 4–16 and digits-only). It hashes and writes the result to `C:\ProgramData\HomeFaceLogon\pin.bin`.

### 2. Security Hardening & Face Matching Precision (Phase B)
- **Changes**:
  - **Motion-Detection Liveness Check**: Configured `FaceLogonHost.exe` ([src/host/main.cpp](file:///c:/Users/nakan/Desktop/Workspace/HomeFaceLogon/src/host/main.cpp)) to compute frame difference (`cv::absdiff`) of consecutive grayscaled/blurred frames. Face detection and matching are executed *only* when motion is detected (threshold 0.5% pixels change) to block static photo spoofing.
  - **Sliding Window Consecutive Match**: Implemented sliding window track (using `requiredMatches` and `windowSize` parameters from `config.json`). A successful login is sent only if the face matches in at least 3 out of 5 frames.
  - **Scan Timeout**: Cameras are automatically turned off and scan is aborted after `scanTimeoutMs` (default 10 seconds) if no match is found, preventing battery drain and background usage.
  - **Useless Code Cleanup**: Fully removed all legacy sample UI code (Checkbox, ComboBox, Edit Text, and HideControls link) from [CSampleCredential.cpp](file:///c:/Users/nakan/Desktop/Workspace/HomeFaceLogon/src/provider/CSampleCredential.cpp) and [CSampleCredential.h](file:///c:/Users/nakan/Desktop/Workspace/HomeFaceLogon/src/provider/CSampleCredential.h).

### 3. Tauri v2 Administration GUI (Phase C)
- **Changes**:
  - Created a beautiful, modern, glassmorphic layout supporting dark and light modes at `src/gui/` using **React + TypeScript + Vite + Tauri v2**.
  - Built three main workspaces:
    1. **Setup Wizard**: Welcome introduction, Target Windows SID auto-detection and password entry, PIN creation, Face Enrollment (calls the OpenCV capture window), and Finished toggle card.
    2. **Configuration Settings**: Registry enable/disable toggle, camera index selector, matching threshold slider (0.300 to 0.500), sliding window size, required matches, and timeout parameters.
    3. **Diagnostics & Console logs**: Run verification test (launches camera to test matching), run system diagnostic, and a live log viewer streaming logs from `FaceLogonHost.log` and `FaceLogonSetup.log`.
  - Configured Rust backend commands in `src/gui/src-tauri/src/lib.rs` to read/write HKLM registries, read/write `config.json`, fetch user SID, launch setup processes, and stream logs.
  - Generated release installer bundles (Wix MSI & NSIS Setup) under `src/gui/src-tauri/target/release/bundle/`.

### 4. Inno Setup Package & Builder (Phase D)
- **Changes**:
  - Created `installer/HomeFaceLogon.iss` to package all binaries (`FaceLogonProvider.dll`, `FaceLogonHost.exe`, `FaceLogonSetup.exe`), OpenCV DLLs, Tauri GUI app (`app.exe` copied as `HomeFaceLogon.exe`), and ONNX model files.
  - Configured automated COM self-registration (`regsvr32`) during setup and clean unregistration upon removal.
  - Added helper script `tools/build-installer.ps1` to easily trigger Inno Setup compilation from a terminal.

---

## 🔍 Verification & Test Results

### 1. Build Verification
All C++ code and the Rust/React Tauri package compile successfully:
- **C++ Solution**: `x64\Release\FaceLogonProvider.dll`, `FaceLogonHost.exe`, and `FaceLogonSetup.exe` built successfully.
- **Tauri GUI App**: Built successfully. Executable located at `src\gui\src-tauri\target\release\app.exe`.
- **Installer Config**: ISS script successfully generated.

### 2. Public-Tree Compliance Check
```powershell
pwsh tools/check-public-tree.ps1
```
**Output**:
```text
Public-tree check passed. Inspected 44 file(s).
```

---

## 🚀 Manual Verification Request (Installer and setup wizard)

To test the newly developed management GUI and installers:

1. **Open the GUI Manager**:
   - Launch `src\gui\src-tauri\target\release\app.exe` as Administrator.
2. **Complete the Wizard**:
   - Click "セットアップを開始する" (Start Setup).
   - Enter your Windows Target SID (use "自動取得" to auto-fill) and sign-in password.
   - Enter a fallback PIN (e.g. `1234`).
   - Choose your camera index and click "カメラを起動して顔登録を開始". Verify that the camera window pops up, captures 30 samples, and closes.
   - Turn the registry enable switch to ON.
3. **Verify in Logon screen**:
   - Press `Win + L` to lock Windows.
   - Face the camera. Verify automatic logon is triggered and logs you in.
   - Lock again. Hide your face and wait for the 10-second timeout.
   - Type a wrong PIN. Verify that the correct attempt counter is displayed. Enter the wrong PIN 5 times and check if the 30-second lockout warning is shown.
