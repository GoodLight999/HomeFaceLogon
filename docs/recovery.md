# 回復・緊急無効化手順 (Recovery Guide)

本 Credential Provider の誤動作やクラッシュによって Windows サインイン画面（LogonUI）が操作不能になった場合、以下のいずれかの手順で本 Provider を安全に無効化し、通常のサインイン手段（PIN やパスワード）でサインインすることができます。

---

## 方法 1. セーフモード起動による無効化（推奨）

Windows が動作しているが、サインイン画面で本 Provider が原因でエラーが発生する場合は、セーフモードで起動して無効化スクリプトを実行します。

1.  サインイン画面の右下にある電源アイコンをクリックします。
2.  `Shift` キーを押しながら「再起動」をクリックします。
3.  再起動後、オプションの選択画面で「トラブルシューティング」 ＞ 「詳細オプション」 ＞ 「スタートアップ設定」 ＞ 「再起動」を選択します。
4.  再起動後、キーボードの `4` または `F4` を押して「セーフモード」で起動します。
5.  セーフモードで通常通りサインインし、管理者権限でコマンドプロンプトまたは PowerShell を開きます。
6.  本プロジェクトの `tools/disable-provider.ps1` スクリプトを実行するか、以下のレジストリコマンドを実行します。

```cmd
reg add HKLM\SOFTWARE\HomeFaceLogon /v Enabled /t REG_DWORD /d 0 /f
```

---

## 方法 2. Windows 回復環境 (WinRE) コマンドプロンプトからの無効化

サインイン画面が完全にフリーズするなどでセーフモードでのサインインも不可能な場合は、Windows 回復環境 (WinRE) のコマンドプロンプトからレジストリハイブを直接ロードして無効化します。

1.  PC を強制終了し、起動中に再び強制終了することを 2〜3 回繰り返すか、インストールメディア（USB）から起動して Windows 回復環境 (WinRE) を開始します。
2.  「トラブルシューティング」 ＞ 「詳細オプション」 ＞ 「コマンド プロンプト」を選択します。
3.  レジストリの `SOFTWARE` ハイブが保存されているドライブ（通常は `C:` ですが、回復環境内では `D:` などに割り当てられている場合があります）を確認します。以下のコマンドで `C:\Windows\System32\config\SOFTWARE` の存在を確認してください。

```cmd
dir C:\Windows\System32\config\SOFTWARE
```

4.  以下のコマンドを実行して、オフラインのレジストリハイブを仮のキー（`HKLM\TempSoftware`）へロードします。

```cmd
reg load HKLM\TempSoftware C:\Windows\System32\config\SOFTWARE
```

5.  ロードしたキーに対して、有効フラグ `Enabled` を `0` に変更します。

```cmd
reg add HKLM\TempSoftware\HomeFaceLogon /v Enabled /t REG_DWORD /d 0 /f
```

6.  レジストリハイブをアンロードします。

```cmd
reg unload HKLM\TempSoftware
```

7.  コマンドプロンプトを閉じ、PC を再起動して通常のサインイン画面が表示されることを確認します。

---

## 手動アンインストール (COM CLSID 削除)

回復環境やセーフモードから完全に登録を削除したい場合、コマンドプロンプトから以下のレジストリキーを削除してください。

```cmd
:: Credential Provider 登録の削除
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\{5fd3d285-0dd9-4362-8855-e0abaacd4af6}" /f

:: COM CLSID の削除
reg delete "HKLM\SOFTWARE\Classes\CLSID\{5fd3d285-0dd9-4362-8855-e0abaacd4af6}" /f

:: アプリ設定の削除
reg delete "HKLM\SOFTWARE\HomeFaceLogon" /f
```
