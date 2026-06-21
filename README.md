# HomeFaceLogon

[English](#english) | [日本語](#日本語)

---

# English

A Windows 11 x64 Credential Provider and background host system that enables facial recognition sign-in and unlock using a standard RGB webcam (built with OpenCV YuNet + SFace).

> [!WARNING]
> **CRITICAL SECURITY DISCLAIMER / TECHNICAL LIMITATIONS**
> This software is **NOT** a secure biometric authentication solution. It is designed solely as a **"casual home prank/snooping prevention tool"** for single-user home PCs.
> - **Vulnerable to Photo/Video Spoofing:** Since it works with a standard 2D RGB camera (no IR or depth sensor support), it can easily be bypassed by holding up a printed photo or video of your face.
> - **WANT REAL SECURITY? BUY A WINDOWS HELLO COMPATIBLE IR CAMERA.**
> - **Credentials Wrapper:** This tool is not a low-level authentication replacement. It is a convenience helper that decrypts and submits your local account password (secured via DPAPI) when a face match is detected.
> - **No Hiding of Defaults:** It does not disable or hide standard Windows sign-in options (PIN/Password).

## 👁️ Features

1. **Facial Recognition Sign-In**  
   Automatically starts the camera when the lock screen appears, and signs you in once the face matches the registered template (OpenCV SFace 128-dimensional vector).
2. **PIN Fallback**  
   Provides a fallback PIN (4–16 alphanumeric/symbolic characters) login UI when facial recognition fails (e.g., in low-light rooms).
3. **Security & Brute-Force Protection**  
   - Passwords are encrypted and stored locally using the Windows **Data Protection API (DPAPI)**.
   - PINs are hashed using **PBKDF2-SHA256** and verified using constant-time comparison to prevent timing attacks.
   - Failsafe lockout: 5 consecutive incorrect PIN attempts will **lock out PIN authentication for 30 seconds**.
   - Basic liveness detection using frame-to-frame pixel difference (optional) and a sliding-window consensus verification (e.g., matching 3 out of 5 consecutive frames).
4. **Tauri v2 Management GUI**  
   A modern dashboard for configuring credentials, registering PINs, enrolling face templates with live previews, diagnostics, and log viewing.
5. **One-Click Installer**  
   Built with Inno Setup to automate DLL registration and setup runtime directories.

---

## 🏗️ Architecture

For stability and security, the application is split into a lightweight DLL running inside LogonUI.exe and an isolated helper process doing the heavy CV lifting.

```
┌─────────────────────────────────────────────────────────────┐
│ Windows LogonUI.exe                                         │
│                                                             │
│  ┌───────────────────────────────────────────────────────┐  │
│  │ FaceLogonProvider.dll (Credential Provider DLL)       │  │
│  │ - Renders UI tiles on the lock screen                 │  │
│  │ - Performs secure local PIN validation                │  │
│  │ - Decrypts and serializes password credentials        │  │
│  └──────────────────────────┬────────────────────────────┘  │
└─────────────────────────────┼───────────────────────────────┘
                              │
                  Bidirectional Named Pipe Connection
       (\\.\pipe\HomeFaceLogonPipe_[ProcessId]_[SessionNonce])
                              │
┌─────────────────────────────▼───────────────────────────────┐
│ FaceLogonHost.exe (Helper Process running as SYSTEM)        │
│ - OpenCV YuNet face detection & SFace feature extraction    │
│ - Motion-based liveness check, sliding-window consensus     │
│ - Sends scan results back to DLL and terminates on complete │
└─────────────────────────────────────────────────────────────┘
```

---

## ⚙️ Configuration Options (`config.json`)

Adjust parameters in `%ProgramData%\HomeFaceLogon\config.json`:

| Key | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `enabled` | boolean | `false` | Enables/Disables the Credential Provider. |
| `targetSid` | string | `""` | Target user's Security Identifier (SID) for auto-login. |
| `cameraIndex` | int | `0` | Camera device index. |
| `matchThreshold` | double | `0.363` | SFace cosine similarity threshold. Higher values are stricter. |
| `requiredMatches` | int | `3` | Required match count inside the sliding window. |
| `windowSize` | int | `5` | Sliding window size (number of recent frames tracked). |
| `scanTimeoutMs` | int | `10000` | Scan timeout in milliseconds before auto-canceling. |
| `livenessEnabled` | boolean | `false` | Enables frame-difference motion check. |

---

## 🚀 Installation & Setup

### For Users

1. **Run Installer**  
   Run `HomeFaceLogonSetup.exe` as Administrator to install.
2. **Setup Wizard**  
   The management GUI will launch automatically.
   - **Step 1:** Select target user, fetch SID, and input your Windows password.
   - **Step 2:** Choose a secure PIN (4-16 chars) for lock screen fallback.
   - **Step 3:** Face camera to register your face template (takes a few seconds).
   - **Step 4:** Toggle "Enable Facial Recognition Sign-In" to ON.
3. **Verify**  
   Lock screen (`Win + L`) to test.

---

## 🛠️ Developer Build Instructions

### Prerequisites
- Visual Studio 2022 (with "Desktop development with C++" workload)
- Node.js (v18+) & npm
- Rust toolchain (for Tauri)
- Inno Setup 6 (for packaging)

### 1. Download Dependencies & Models
Run these scripts in PowerShell to download OpenCV and ONNX models:
```powershell
# Fetch OpenCV binaries
pwsh tools/fetch-opencv.ps1

# Fetch YuNet & SFace models
pwsh tools/fetch-models.ps1
```

### 2. Build C++ Solution
```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" HomeFaceLogon.sln /p:Configuration=Release /p:Platform=x64
```

### 3. Build Tauri GUI App
```powershell
cd src/gui
npm install
npm run tauri build
```

### 4. Build Installer Package
```powershell
pwsh tools/build-installer.ps1
```
The output setup file will be generated in `installer_output/`.

---

## 🚨 Emergency Disable & Recovery

If the login screen freezes or behaves incorrectly, you can bypass the provider:

### Option A: Safe Mode (Recommended)
1. Hold `Shift` while clicking "Restart" on the lock screen.
2. Go to: Troubleshoot > Advanced options > Startup Settings > Restart.
3. Press `4` or `F4` to enter Safe Mode.
4. Sign in normally and run this in an Administrator command prompt:
   ```cmd
   reg add HKLM\SOFTWARE\HomeFaceLogon /v Enabled /t REG_DWORD /d 0 /f
   ```

### Option B: WinRE Command Prompt
1. Boot into Windows Recovery Environment (WinRE) and open a Command Prompt.
2. Load the registry hive:
   ```cmd
   reg load HKLM\TempSoftware C:\Windows\System32\config\SOFTWARE
   ```
3. Disable the provider:
   ```cmd
   reg add HKLM\TempSoftware\HomeFaceLogon /v Enabled /t REG_DWORD /d 0 /f
   ```
4. Unload and reboot:
   ```cmd
   reg unload HKLM\TempSoftware
   ```

---
---

# 日本語

一般的な RGB Web カメラを使用し、OpenCV YuNet + SFace で顔認証による Windows サインインおよびロック解除機能を実現する Credential Provider とバックグラウンドホストのシステムです。

> [!WARNING]
> **【セキュリティに関する重要事項・技術的限界】**
> 本ソフトウェアは、**個人所有の自宅PC（単一ユーザー環境）における「簡易的なイタズラ・覗き見防止」**を前提としたジョーク・ホビー向けツールであり、**堅牢なセキュリティ対策としては機能しません。**
> - **写真や動画によるなりすまし対策はありません:** 一般的な2D RGBカメラを使用するため、顔写真や動画をカメラにかざすだけで突破される可能性があります（赤外線・深度検知等には非対応）。
> - **真面目なセキュリティが欲しければ、Windows Hello対応のIRカメラをご購入ください。**
> - **資格情報のラッパー:** 本ツールはWindows Helloのような低レベルの認証機構自体を置き換えるものではありません。顔一致を検知した際に、安全に保管されたパスワード（DPAPI保護）を自動的に復号してサインインを代行する支援ユーティリティです。
> - **標準機能の維持:** Windows 標準のサインインオプション（パスワード/PIN）を無効化したり隠したりすることはありません。

## 👁️ 主な機能

1. **顔認証サインイン**  
   ロック画面が表示されるとカメラが自動で起動し、登録された顔特徴量（OpenCV SFace 128次元ベクトル）と一致した場合に自動ログインします。
2. **PIN フォールバック機能**  
   暗い部屋などで顔認証が失敗した時のために、画面上の入力欄からログインできる独自のPINコード（4〜16文字の半角英数字・記号）を設定できます。
3. **セキュリティ保護とブルートフォース対策**  
   - パスワードはローカルマシンのセキュリティ境界である **DPAPI (Data Protection API)** で暗号化され、安全に保管されます。
   - PIN は **PBKDF2-SHA256** でハッシュ化され、タイミング攻撃対策を施した定数時間比較で照合されます。
   - 間違った PIN を5回連続で入力すると、**30秒間 PIN 認証がロックアウト**されます。
   - スキャン時のなりすましを軽減するため、**フレーム差分を用いた動体検知（Liveness検知）**、および **スライディング窓（5フレーム中3フレーム一致）による連続一致判定**を実装しています。
4. **Tauri v2 管理 GUI ツール**  
   資格情報の設定、PIN 登録、カメラプレビューを伴う顔情報の登録、診断テスト、ログ閲覧を行える管理者用ダッシュボードを同梱しています。
5. **ワンクリックインストーラー**  
   Inno Setup を用いたパッケージ作成に対応しており、DLL の登録や ProgramData フォルダの作成を自動化します。

---

## 🏗️ アーキテクチャ

本ソフトウェアは安定性とセキュリティのために、LogonUI で動作する軽量 DLL と、カメラ/AI 処理を実行するホストプロセスの2つのコンポーネントに分離されています。

```
┌─────────────────────────────────────────────────────────────┐
│ Windows LogonUI.exe                                         │
│                                                             │
│  ┌───────────────────────────────────────────────────────┐  │
│  │ FaceLogonProvider.dll (Credential Provider DLL)       │  │
│  │ - Windows ロック画面での認証タイル表示                │  │
│  │ - 独自の安全なローカル PIN 検証                       │  │
│  │ - 暗号化パスワード(secret.bin)の復号とシリアライズ    │  │
│  └──────────────────────────┬────────────────────────────┘  │
└─────────────────────────────┼───────────────────────────────┘
                              │
                    双方向 Named Pipe 接続
      (\\.\pipe\HomeFaceLogonPipe_[ProcessId]_[SessionNonce])
                              │
┌─────────────────────────────▼───────────────────────────────┐
│ FaceLogonHost.exe (顔認証バックグラウンドプロセス)            │
│ - OpenCV YuNet による顔検出 & SFace による特徴抽出          │
│ - 動体検知 (Liveness), スライディング窓判定                 │
│ - スキャン完了/タイムアウト時に結果を DLL に返却して自動終了│
└─────────────────────────────────────────────────────────────┘
```

---

## ⚙️ 設定オプション (`config.json`)

`%ProgramData%\HomeFaceLogon\config.json` にて、動作パラメータを詳細に調整できます。

| キー名 | 型 | デフォルト値 | 説明 |
| :--- | :--- | :--- | :--- |
| `enabled` | boolean | `false` | 本プロバイダーを有効にするか。 |
| `targetSid` | string | `""` | 自動サインイン対象となる Windows ユーザーの SID。 |
| `cameraIndex` | int | `0` | 使用するカメラデバイスのインデックス。 |
| `matchThreshold` | double | `0.363` | SFace のコサイン類似度しきい値。高いほど厳格になります。 |
| `requiredMatches` | int | `3` | 一致と判定するために、スライディング窓内で必要な一致フレーム数。 |
| `windowSize` | int | `5` | 一致判定に使用するスライディング窓のサイズ（過去フレーム数）。 |
| `scanTimeoutMs` | int | `10000` | スキャン開始から自動終了するまでの時間（ミリ秒）。 |
| `livenessEnabled` | boolean | `false` | フレーム差分による動体検知を有効にするか。 |

---

## 🚀 インストール & セットアップ

### ユーザー向け手順

1. **インストーラーの実行**  
   管理者権限でインストーラー（`HomeFaceLogonSetup.exe`）を実行し、インストールを行います。
2. **管理ツールの起動**  
   インストール完了後に起動する「HomeFaceLogon 管理ツール」でセットアップウィザードを進めます。
   - **ステップ 1:** 対象ユーザーの SID を自動取得し、Microsoft アカウントのパスワードを入力します。
   - **ステップ 2:** ロック画面用の PIN（4〜16文字の半角英数字・記号）を設定します。
   - **ステップ 3:** カメラを選択して顔登録を開始します。カメラをまっすぐ見つめると、数秒で特徴量の抽出が完了します。
   - **ステップ 4:** 「顔認証サインインを有効化」をオンにしてウィザードを閉じます。
3. **確認**  
   `Win + L` でロック画面を表示し、カメラが起動して自動サインインまたは PIN で解除できるか確認します。

---

## 🛠️ 開発者向けビルド手順

### 前提条件
- Visual Studio 2022 (C++ によるデスクトップ開発ワークロード)
- Node.js (v18以上) & npm
- Rust ツールチェーン (Tauriビルド用)
- Inno Setup 6 (インストーラー作成時)

### 1. 依存ライブラリとモデルの取得
以下のスクリプトを実行して、OpenCV のバイナリおよび ONNX モデルファイルを取得します。
```powershell
# OpenCV のダウンロードと展開
pwsh tools/fetch-opencv.ps1

# YuNet & SFace ONNX モデルの取得
pwsh tools/fetch-models.ps1
```

### 2. C++ ソリューションのビルド
```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" HomeFaceLogon.sln /p:Configuration=Release /p:Platform=x64
```

### 3. Tauri GUI アプリのビルド
```powershell
cd src/gui
npm install
npm run tauri build
```

### 4. インストーラーのビルド
```powershell
pwsh tools/build-installer.ps1
```
ビルドされたインストーラーは `installer_output/` ディレクトリに生成されます。

---

## 🚨 緊急無効化・リカバリ手順

顔認証やプロバイダーの誤動作により Windows サインイン画面が操作不能になった場合は、以下の手順で無効化できます。

### 方法 1: セーフモードによる無効化（推奨）
1. サインイン画面右下の電源アイコンをクリックし、`Shift` キーを押しながら「再起動」をクリックします。
2. 再起動後、「トラブルシューティング」 ＞ 「詳細オプション」 ＞ 「スタートアップ設定」 ＞ 「再起動」を選択します。
3. 再起動後、キーボードの `4` または `F4` を押し「セーフモード」で起動します。
4. セーフモードで通常通りサインインし、管理者権限でコマンドプロンプトを開き、以下を実行して無効化します。
   ```cmd
   reg add HKLM\SOFTWARE\HomeFaceLogon /v Enabled /t REG_DWORD /d 0 /f
   ```

### 方法 2: 回復環境 (WinRE) コマンドプロンプトからの無効化
1. Windows が起動しない場合、自動修復画面から「詳細オプション」 ＞ 「トラブルシューティング」 ＞ 「詳細オプション」 ＞ 「コマンド プロンプト」を開きます。
2. レジストリ SOFTWARE ハイブを一時キーにロードします。
   ```cmd
   reg load HKLM\TempSoftware C:\Windows\System32\config\SOFTWARE
   ```
3. 有効化フラグを 0 に変更します。
   ```cmd
   reg add HKLM\TempSoftware\HomeFaceLogon /v Enabled /t REG_DWORD /d 0 /f
   ```
4. ハイブをアンロードして PC を再起動します。
   ```cmd
   reg unload HKLM\TempSoftware
   ```
