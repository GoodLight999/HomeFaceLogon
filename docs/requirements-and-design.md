# Home Face Logon
## 要件定義・基本設計書 v0.2

- 作成日: 2026-06-16
- 対象: Windows 11 x64
- 想定読者: Windowsネイティブ開発の経験が浅い実装担当者
- 実装主体: agy（AIコーディングエージェント）＋人間による実機試験
- 文書の位置付け: MVPの要件定義、基本設計、実装順序、受入条件、公開リポジトリ運用を一体化したもの
- 公開方針: GitHub上の公開リポジトリとして開発する

---

## 0. 結論

本プロジェクトでは、既存のWindowsユーザープロファイルへ、一般的なRGB Webカメラの顔照合を利用してサインインする機能を実装する。

主対象は **個人用MicrosoftアカウントでWindowsへサインインしている、単一ユーザーの自宅デスクトップPC** とする。  
ローカルアカウントは主対象ではなく、開発時の回帰試験用としてのみ対応する。

実装方式はMicrosoftが正式に公開している **Credential Provider V2** とする。Windowsの認証機構、Winlogon、LSASS、SAM、Windows Helloを改造しない。顔照合に成功した場合にだけ、暗号化保存したMicrosoftアカウントのパスワードをWindows標準の認証経路へシリアライズして提出する。

これはWindows Hello Faceの再実装ではない。製品の性質は次のとおりである。

> 顔照合を、保存済みWindowsパスワードを使用する許可条件として利用する、個人PC向けCredential Provider

本方式は、Rohos Face Logonと同系統の現実的な妥協を意図的に採用する。顔認証による独自Authentication Package、Windows Hello互換、PIN再利用は目標にしない。個人開発では、Windows標準のパスワード認証へ既存パスワードを提出する方式を完成させることを優先する。

---

# 1. 背景と目的

既存製品Rohos Face Logonは、現行Windows上での安定性、保守状況、UI、カメラ互換性に問題がある。そこで、以下の条件に特化した現代版を個人開発する。

- 自宅の固定デスクトップPC
- 所有者は原則一人
- 顔写真、動画、ディープフェイクによる攻撃対策は要求しない
- Windows Hello対応IRカメラは要求しない
- 一般的なUSB Webカメラを利用する
- 最高水準の生体認証ではなく、日常のロック解除を便利にする
- Microsoftアカウントを主対象とする
- 標準のPIN・パスワードサインインは常に残す

## 1.1 セキュリティ境界と意図的な妥協

本プロジェクトは、企業向けMFA製品やWindows Hello相当の生体認証製品ではない。以下を設計上の前提とする。

- 顔照合成功後に、暗号化保存したWindowsパスワードを復号して提出する
- 写真、動画、ディープフェイクへの耐性は要求しない
- ローカル管理者権限を取得した攻撃者からパスワードを完全に保護することは要求しない
- Windows標準Providerより強い認証保証を主張しない
- 利便性、保守性、復旧性、コードの理解可能性を優先する
- 独自Authentication Package、LSASS拡張、カーネルドライバーは採用しない

これは暫定実装ではなく、MVPの正式な方式である。将来版でも、明確な必要性が生じない限り複雑な認証基盤へ移行しない。

---

# 2. 技術的成立性

## 2.1 Credential ProviderはWindowsの正式な拡張点である

Windowsは、サインイン画面で資格情報を集めるプラグイン機構としてCredential Provider Frameworkを公開している。

第三者は以下を実装できる。

- `ICredentialProvider`
- `ICredentialProviderCredential`
- V2では `ICredentialProviderCredential2`
- ユーザー列挙が必要な場合は `ICredentialProviderSetUserArray`

Credential Providerは認証結果を勝手に決定しない。資格情報を収集・シリアライズし、WindowsのLocal Security AuthorityおよびAuthentication Packageへ提出する。最終的なパスワード検証はWindows側が実行する。

本設計は、この公開された拡張点だけを使用する。

## 2.2 MicrosoftアカウントはCredential Providerのユーザー配列に現れる

Windows 8以降のCredential Provider V2では、Windows側から `ICredentialProviderUserArray` が渡される。各ユーザーについて、次の情報を取得できる。

- SID
- 表示名
- ユーザー名
- Qualified User Name
- Account Provider ID

特に `PKEY_Identity_QualifiedUserName` は、Microsoft公式資料上「認証バッファーのパックに使用される名前」である。Microsoftアカウントでは、次の概念形式になる。

```text
<online account provider name>\<email address>
```

実装では文字列を推測したり、`MicrosoftAccount\`を固定したりしない。Windowsが返したQualified User Nameをそのまま使用する。

## 2.3 オンラインID用の認証バッファー形式が公開されている

`CredPackAuthenticationBufferW` は、ユーザー名とパスワードを認証バッファーへ変換する公式APIである。

MicrosoftアカウントなどのオンラインIDでは、次のフラグを使用する。

```cpp
CRED_PACK_ID_PROVIDER_CREDENTIALS
```

この場合、資格情報は `SEC_WINNT_AUTH_IDENTITY_EX2` 形式に梱包され、オンラインIDプロバイダーを識別する情報を含む。

したがって、Microsoftアカウント対応は裏技ではなく、公開APIの想定範囲に含まれる。

---

# 3. 用語

| 用語 | 本文書での意味 |
|---|---|
| CP | Credential Provider |
| MSA | 個人用Microsoft Account |
| Qualified User Name | Windowsが認証バッファー條包用に提供する完全修飾ユーザー名 |
| Face Template | 顔画像から生成した特徴ベクトル |
| Enrollment | カメラで顔を撮影し、Face Templateを登録する処理 |
| Auto-submit | 顔照合成功後、ユーザーがボタンを押さずWindowsへ資格情報を提出する処理 |
| System Provider | Windows標準のPIN、パスワード、Windows Hello等のCredential Provider |
| Passwordless MSA | Microsoftアカウント自体からパスワードを削除した設定 |
| Hello-only設定 | 「このデバイスではMicrosoftアカウントにWindows Helloサインインのみを許可する」設定 |

---

# 4. MVP要件

## 4.1 必須対応

### 対象OS

- Windows 11 x64
- 最初は開発機に実際にインストールされているWindowsビルドを正式な試験対象とする
- ARM64は対象外
- Windows 10は対象外
- Windows Serverは対象外

### 対象アカウント

- 個人用Microsoftアカウント
- Microsoftアカウントに有効なパスワードが存在すること
- Windows上に既に作成され、通常利用できているユーザープロファイル
- 単一の設定済みSID
- ローカルアカウントは試験用として対応

### 対象シナリオ

- コールドブート後のサインイン
- サインアウト後のサインイン
- `Win + L` によるロック解除
- スリープ復帰後、Windowsが資格情報入力を要求した場合のロック解除
- オンライン状態
- Windows側のキャッシュ認証が成立する範囲でのオフライン状態

### 顔認識

- 一般的なUSB RGB Webカメラ
- 顔検出
- 顔のアラインメント
- 特徴ベクトル生成
- 登録済み特徴ベクトルとのコサイン類似度比較
- 複数フレーム連続一致
- 本人の顔が検出された場合の自動サインイン

### 復旧性

- Windows標準のPIN・パスワードProviderを絶対に無効化しない
- Credential Provider Filterを実装しない
- Providerが失敗しても標準の「サインイン オプション」から入れる
- Safe ModeまたはWindows回復環境から無効化できる手順を用意する
- レジストリを変更する緊急無効化スクリプトを用意する
- アンインストーラーを用意する

## 4.2 MVPで非対応

- パスワードを削除したPasswordless Microsoft Account
- Microsoft Entra ID
- Active Directoryドメインアカウント
- Windows HelloのPINを読み出す、保存する、再利用する処理
- Windows Hello Faceとしての登録
- Windows Biometric Framework用ドライバー
- 独自Authentication Package
- LSASSへの独自パッケージロード
- RDP
- UACのCredential UI
- `Run as different user`
- 複数ユーザーへの同時対応
- 顔写真、動画、ディープフェイク対策
- 赤外線カメラ
- 画面内のライブカメラプレビュー
- クラウド同期
- 外部サーバー
- モバイル連携
- Windows Store配布

## 4.3 Passwordless MSAについて

Microsoftアカウント自体からパスワードを削除している場合、本方式ではWindowsへ提出できるパスワードが存在しない。

この場合のMVPの動作は次のとおり。

1. 設定アプリでPasswordless MSAの可能性を説明する
2. パスワード登録ができない場合、セットアップを完了しない
3. Windows標準のPINまたはHelloをそのまま使用してもらう
4. 将来版で別方式を検討する

Windows HelloのPINは、端末に結び付いた鍵を解除するローカル秘密であり、通常のパスワードとして取得・再利用する設計にはしない。

---

# 5. ユースケース

## UC-01 初回セットアップ

1. ユーザーが設定アプリを管理者権限で起動する
2. アプリが現在のWindowsユーザーのSIDを取得する
3. 対象SIDを表示する
4. 使用するカメラを選択する
5. カメラ映像で顔登録を行う
6. Microsoftアカウントのパスワードを入力する
7. 設定アプリがFace Templateとパスワードを暗号化保存する
8. Credential Providerを有効化する
9. 「標準PIN・パスワードは削除しない」ことを確認表示する
10. ユーザーがロック画面で動作試験を行う

## UC-02 ロック解除

1. WindowsがLogon UIを開始する
2. Windowsが本Providerへユーザー配列を渡す
3. Providerが登録済みSIDと一致するユーザーを探す
4. ProviderがWindowsからQualified User Nameを取得する
5. ユーザーがFace Logonタイルを選択する
6. 顔認識を開始する
7. 顔が連続して一致する
8. ProviderがAuto-submit状態になる
9. Windowsが `GetSerialization()` を呼ぶ
10. Providerが保存済みパスワードを復号する
11. MSA用認証バッファーへ梱包する
12. Windowsが認証する
13. 成功時はデスクトップへ遷移する
14. Providerは平文パスワードを即時消去する

## UC-03 顔が一致しない

1. 顔認識を開始する
2. 一定時間内に一致しない
3. 「顔を確認できません。サインイン オプションからPINまたはパスワードを使用してください」と表示する
4. 自動提出しない
5. 標準Providerはそのまま利用できる

## UC-04 保存済みパスワードが古い

1. 顔照合は成功する
2. Windows認証が失敗する
3. `ReportResult()` で認証失敗を受け取る
4. 自動再試行しない
5. 「保存済みパスワードが無効です。標準のサインイン方法でログイン後、Face Logon設定を更新してください」と表示する
6. 平文パスワードを破棄する

## UC-05 カメラ故障・モデル欠落

1. 顔認識開始に失敗する
2. Providerは例外を外へ出さない
3. 自動提出しない
4. エラーコードだけをログへ記録する
5. 標準Providerからのログインを妨げない

---

# 6. アーキテクチャ

## 6.1 コンポーネント

```text
┌────────────────────────────────────────────────────────────┐
│ Windows LogonUI.exe                                        │
│                                                            │
│  ┌──────────────────────────────────────────────────────┐  │
│  │ FaceLogonProvider.dll                               │  │
│  │ - ICredentialProvider                              │  │
│  │ - ICredentialProviderSetUserArray                  │  │
│  │ - ICredentialProviderCredential2                   │  │
│  │ - 状態管理                                          │  │
│  │ - 資格情報シリアライズ                              │  │
│  │ - Auto-submit制御                                   │  │
│  └───────────────┬──────────────────────────────────────┘  │
└──────────────────┼─────────────────────────────────────────┘
                   │ Named Pipe（SYSTEM限定）
                   ▼
┌────────────────────────────────────────────────────────────┐
│ FaceLogonHost.exe                                          │
│ - Media Foundationによるカメラ取得                         │
│ - 顔検出                                                    │
│ - 顔アラインメント                                         │
│ - 顔特徴量生成                                              │
│ - 類似度判定                                                │
│ - タイムアウト                                              │
└────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────┐
│ FaceLogonSetup.exe                                         │
│ - 対象SID登録                                               │
│ - カメラ選択                                                │
│ - 顔登録                                                    │
│ - パスワード登録・更新                                      │
│ - 診断                                                      │
│ - 有効化・無効化                                            │
└────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────┐
│ %ProgramData%\HomeFaceLogon                                │
│ - config.json                                               │
│ - secret.bin  （DPAPI Machine Scope）                       │
│ - face.bin    （DPAPI Machine Scope）                       │
│ - models\                                                  │
│ - logs\                                                    │
└────────────────────────────────────────────────────────────┘
```

## 6.2 なぜHostを分離するか

Credential Provider DLLはLogonUIプロセスへ読み込まれる。OpenCV、ONNX、カメラドライバー由来の障害をLogonUIへ直接波及させるべきではない。

そのため、顔認識とカメラアクセスは原則として `FaceLogonHost.exe` へ分離する。

ただし、ログオン前セッションから起動したHostが対象Webカメラへアクセスできるかは、カメラ、ドライバー、Windowsビルドで差が出る可能性がある。最初に技術スパイクで確認する。

### アーキテクチャ決定ゲート

- Hostからカメラ取得可能  
  → Hostでカメラ取得・推論を完結する

- Hostからカメラ取得不能、Provider内では取得可能  
  → Provider内でMedia Foundationによるフレーム取得だけを行い、推論をHostへ渡す

- どちらも不能  
  → 対象カメラ・バックエンドの変更を試験し、MVP継続可否を判断する

カメラアクセスの成立を確認する前に、顔認識ライブラリをCredential Providerへ深く統合しないこと。

## 6.3 Hostの起動方針

- Providerタイルが選択された時だけ起動する
- `CreateProcessW` を使い、シェル依存APIは使わない
- コンソールウィンドウを出さない
- Providerが生成したランダムなセッションNonceを引数または安全なIPCで渡す
- Named Pipeは推測しにくい名前にする
- Pipe ACLはSYSTEMだけを許可する
- 一回のLogon UIセッションごとにHostを作り直す
- タイル選択解除、タイムアウト、Providerアンロード時にHostを終了する
- Job Objectへ入れ、親側終了時にHostも終了させる
- Hostの終了コードと内部エラーコードを分離する

## 6.4 IPCメッセージ

JSONではなく、固定長・バージョン付きの小さなバイナリプロトコルを推奨する。

```cpp
struct MessageHeader {
    uint32_t magic;          // 'HFLO'
    uint16_t version;        // 1
    uint16_t type;
    uint32_t payloadSize;
    uint64_t sessionNonceHi;
    uint64_t sessionNonceLo;
};
```

メッセージ種別:

- `START_SCAN`
- `CANCEL_SCAN`
- `STATUS`
- `MATCHED`
- `NO_MATCH`
- `CAMERA_ERROR`
- `MODEL_ERROR`
- `SHUTDOWN`

Hostはパスワードを受け取らない。  
Providerは生のカメラ画像を永続保存しない。

---

# 7. Credential Provider設計

## 7.1 実装インターフェイス

### Providerクラス

- `IUnknown`
- `ICredentialProvider`
- `ICredentialProviderSetUserArray`

### Credentialクラス

- `IUnknown`
- `ICredentialProviderCredential`
- `ICredentialProviderCredential2`

### UI更新

- `ICredentialProviderCredentialEvents2`

### 再列挙・Auto-submit

- `ICredentialProviderEvents::CredentialsChanged`

## 7.2 対応Usage Scenario

`SetUsageScenario()` は以下を受け入れる。

- `CPUS_LOGON`
- `CPUS_UNLOCK_WORKSTATION`

その他は `E_NOTIMPL` を返す。

Windows 10以降はLOGONとUNLOCKが統合される場合があるが、ポリシーや状況によって `CPUS_UNLOCK_WORKSTATION` が渡される場合がある。両方に対して正しい構造を生成する。

以下はMVPで拒否する。

- `CPUS_CREDUI`
- `CPUS_CHANGE_PASSWORD`
- `CPUS_PLAP`

## 7.3 ユーザー列挙

`SetUserArray()` でWindowsから渡されたユーザーを列挙する。

各ユーザーから次を取得する。

- `PKEY_Identity_PrimarySid`
- `PKEY_Identity_DisplayName`
- `PKEY_Identity_UserName`
- `PKEY_Identity_QualifiedUserName`
- `PKEY_Identity_ProviderID`

処理:

1. 設定済みSIDと一致するユーザーだけを対象にする
2. Qualified User Nameをメモリ上に保持する
3. Provider IDも保持する
4. 他ユーザーをフィルタリングしない
5. System Providerへ一切干渉しない
6. 一致ユーザーが見つからない場合、本Providerは資格情報を列挙しない

MicrosoftアカウントのQualified User Nameを自前で組み立ててはならない。

## 7.4 タイルUI

Credential Providerは任意のUIを直接描画しない。Windowsが提供するフィールド型の範囲で構成する。

フィールド案:

| ID | 型 | 用途 |
|---|---|---|
| 0 | `CPFT_TILE_IMAGE` | 顔アイコン |
| 1 | `CPFT_LARGE_TEXT` | `Face Logon` |
| 2 | `CPFT_SMALL_TEXT` | 状態表示 |
| 3 | `CPFT_COMMAND_LINK` | `再試行` |
| 4 | `CPFT_COMMAND_LINK` | `スキャンを停止` |
| 5 | `CPFT_SUBMIT_BUTTON` | デバッグ時の手動提出、正式版では原則非表示 |

ログオンタイル内にライブ動画プレビューを埋め込むことはMVP要件に含めない。

状態表示例:

- `カメラを準備しています`
- `顔をカメラへ向けてください`
- `顔を確認しました`
- `顔を確認できません`
- `カメラを利用できません`
- `保存済みパスワードを更新してください`

UI文字列は日本語リソースとしてDLLへ含める。コードへ直書きしない。

## 7.5 状態機械

```text
DISABLED
  └─> IDLE
        └─ SetSelected()
            └─> STARTING_HOST
                  ├─ success -> SCANNING
                  └─ failure -> ERROR

SCANNING
  ├─ match -> MATCHED
  ├─ timeout -> NO_MATCH
  ├─ cancel -> IDLE
  └─ error -> ERROR

MATCHED
  └─ set autoLogonPending=true
      └─ CredentialsChanged()
          └─ GetCredentialCount(autoLogon=true)
              └─ GetSerialization()
                  ├─ submit -> SUBMITTED
                  └─ error -> ERROR

SUBMITTED
  ├─ ReportResult(success) -> STOPPING
  └─ ReportResult(failure) -> AUTH_FAILED

AUTH_FAILED
  └─ 再試行はユーザー操作が必要
```

## 7.6 Auto-submit

顔照合成功時に以下を行う。

1. `autoLogonPending = true`
2. ステータスを更新
3. `ICredentialProviderEvents::CredentialsChanged()` を呼ぶ
4. 再列挙後の `GetCredentialCount()` で:
   - `pdwCount = 1`
   - `pdwDefault = 0`
   - `pbAutoLogonWithDefault = TRUE`
5. Logon UIが `GetSerialization()` を呼ぶ
6. `GetSerialization()` 開始時に `autoLogonPending` を消費する
7. 同じ顔照合イベントによる二重提出を禁止する

単なるUI表示更新には `CredentialsChanged()` を乱用しない。UI文字列の更新は `ICredentialProviderCredentialEvents2` を使用する。

---

# 8. Microsoftアカウント資格情報のシリアライズ

## 8.1 基本原則

Providerは、顔一致をもってWindows認証成功とはしない。

顔一致後、以下をWindowsへ提出する。

- Windowsが提供したQualified User Name
- 保存済みMicrosoftアカウントパスワード
- 正しいAuthentication Package
- ProviderのCLSID

## 8.2 MSA分岐

Microsoftアカウントと判断した場合:

```cpp
CredPackAuthenticationBufferW(
    CRED_PACK_ID_PROVIDER_CREDENTIALS,
    qualifiedUserName,
    password,
    buffer,
    &bufferSize
);
```

実装時の重要点:

- `qualifiedUserName` は `PKEY_Identity_QualifiedUserName` の戻り値を使用する
- 文字列を固定生成しない
- まずNULLバッファーで必要サイズを取得する
- `ERROR_INSUFFICIENT_BUFFER` 以外は失敗扱い
- 認証バッファーは使用後にゼロ化して解放する
- `ulAuthenticationPackage` はMicrosoft公式サンプルのNegotiate取得処理を基礎にする
- `clsidCredentialProvider` は本ProviderのCLSID
- 成功時だけ `CPGSR_RETURN_CREDENTIAL_FINISHED`

## 8.3 ローカルアカウント分岐

ローカルアカウントでは、Microsoft公式Credential Provider V2サンプルの `KERB_INTERACTIVE_UNLOCK_LOGON` 梱包処理を基礎にする。

ただし、ローカルアカウント対応は回帰試験用途であり、MSA経路の代用にはしない。

## 8.4 実装前の必須スパイク

顔認識を統合する前に、次を実証する。

1. 実際のMicrosoftアカウントユーザーがV2 Providerに列挙される
2. SIDを取得できる
3. Qualified User Nameを取得できる
4. 手動ボタン操作で保存済みパスワードをシリアライズできる
5. Microsoftアカウントへサインインできる
6. ロック解除でも成功する
7. オンライン・オフライン双方で挙動を記録する
8. Hello-only設定ON/OFFで挙動を記録する

このスパイクが成功するまで、顔認識統合を開始しない。

---

# 9. パスワード保存

## 9.1 方式

MVPはWindows DPAPIのMachine Scopeを使う。

```cpp
CryptProtectData(
    &input,
    L"HomeFaceLogon MSA password",
    nullptr,
    nullptr,
    nullptr,
    CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
    &output
);
```

保存先:

```text
%ProgramData%\HomeFaceLogon\secret.bin
```

## 9.2 ACL

`%ProgramData%\HomeFaceLogon` 以下は、継承を整理した明示ACLとする。

- `SYSTEM`: Full Control
- `Administrators`: Full Control
- 一般ユーザー: アクセス不可
- 対象ユーザー: 原則アクセス不可
- Setupアプリは管理者昇格して更新する

Machine Scope DPAPIは同一PC上の別ユーザーでも復号可能な性質を持つため、ファイルACLが必須である。

## 9.3 平文パスワードの扱い

- `std::wstring` の無秩序なコピーを避ける
- 復号後のバッファー所有権を明確にする
- `SecureZeroMemory()` 後に解放する
- 認証バッファーもゼロ化する
- ログへ出力しない
- デバッガー向けトレースへ出力しない
- 例外メッセージへ含めない
- クラッシュダンプへ残る時間を最小化する
- タイムアウトまたはキャンセルでも消去する

## 9.4 パスワード更新

設定アプリに「保存済みパスワードを更新」機能を用意する。

Windows認証失敗後にProviderが自動で新しいパスワードを要求する機能はMVPでは実装しない。標準Providerでログインした後、設定アプリから更新する。

---

# 10. 顔認識設計

## 10.1 推奨構成

- 顔検出: OpenCV YuNet
- 顔認識: OpenCV SFace
- 推論: OpenCV DNN CPU
- カメラ取得: Windows Media Foundation
- OpenCV `VideoCapture` は設定・診断アプリでは利用可
- Logon環境ではMedia Foundationを第一候補にする

理由:

- 小規模
- CPU動作
- ONNXモデル
- C++から利用可能
- OpenCV公式Model Zooに実装例がある
- 自宅PC用途に十分な速度を期待できる

## 10.2 Enrollment

登録時:

1. 顔が一つだけ検出されていること
2. 顔領域が一定サイズ以上であること
3. 極端なブレを除外すること
4. 正面、わずかな左右角度を含めること
5. 20～30フレーム取得すること
6. 品質条件を満たす10フレーム以上を採用すること
7. 各フレームをアラインメントすること
8. 特徴ベクトルをL2正規化すること
9. 平均ベクトルを生成すること
10. 平均ベクトルを再正規化すること
11. DPAPI Machine Scopeで暗号化保存すること

生の顔画像は既定で保存しない。

## 10.3 照合

- 1フレームだけで成功させない
- 顔検出成功
- 特徴抽出成功
- 類似度が閾値以上
- 直近5回中3回以上の一致
- 一致判定期間は最大10秒
- 一度成功したら同一セッションで再成功イベントを発火しない

閾値はモデルの公開例を盲目的に固定しない。設定アプリで本人サンプルと非本人サンプルを用いた簡易キャリブレーションを行い、初期値と実測値を設定ファイルへ保存する。

## 10.4 カメラ競合

- カメラを使用中のアプリが存在する場合の挙動を試験する
- ロック前にTeams、ブラウザー、OBS等がカメラを利用していた場合を試験する
- Frame Server経由で共有可能か確認する
- 開けない場合は数回だけ短い再試行を行う
- 永久リトライしない
- タイムアウト後は標準Providerへ誘導する

---

# 11. 設定ファイル

## 11.1 config.json

秘密情報を入れない。

```json
{
  "schemaVersion": 1,
  "enabled": true,
  "targetSid": "S-1-5-21-...",
  "cameraSymbolicLink": "...",
  "matchThreshold": 0.0,
  "requiredMatches": 3,
  "windowSize": 5,
  "scanTimeoutMs": 10000,
  "hostStartupTimeoutMs": 3000,
  "modelVersion": "sface-...",
  "createdAtUtc": "...",
  "updatedAtUtc": "..."
}
```

`matchThreshold` の具体値はEnrollment時に決める。

## 11.2 secret.bin

バージョン付きバイナリ。

```cpp
struct SecretFileHeader {
    uint32_t magic;
    uint16_t schemaVersion;
    uint16_t flags;
    uint32_t protectedBlobSize;
};
```

DPAPI保護済みBLOBを後続に格納する。

## 11.3 face.bin

- モデルID
- 次元数
- 正規化済み平均特徴ベクトル
- キャリブレーション情報
- DPAPI保護済み

モデル更新で特徴空間が変わるため、モデルIDが一致しない場合は再Enrollmentを要求する。

---

# 12. ログ設計

## 12.1 ログへ出してよいもの

- UTC日時
- コンポーネント名
- イベントID
- HRESULT
- Win32 Error
- 状態遷移
- カメラ初期化時間
- 推論時間
- 顔検出の有無
- 類似度の丸め値
- タイムアウト
- Windowsビルド
- DLL・モデルバージョン

## 12.2 ログへ出してはいけないもの

- パスワード
- 暗号化前パスワード
- 認証バッファー
- DPAPI復号結果
- 顔画像
- 完全なメールアドレス
- SID全文を大量に繰り返すこと
- Named PipeのNonce
- 秘密ファイルの中身

メールアドレスが必要な診断では、先頭数文字＋ドメイン伏字、またはハッシュを用いる。

## 12.3 保存先

```text
%ProgramData%\HomeFaceLogon\logs
```

ローテーション:

- 1ファイル 2 MB
- 最大5世代
- デバッグビルドとリリースビルドでレベル分離

---

# 13. 失敗時設計

## 13.1 Providerの原則

- 例外をCOM境界の外へ出さない
- すべての公開メソッドで引数検証
- 失敗時は自動ログオンしない
- 不明な状態では必ずFail Closed
- System Providerを隠さない
- Providerのエラーで再起動を要求しない

## 13.2 重いDLLのロード

OpenCV等はProvider DLLの `DllMain` でロードしない。

- `DllMain` は最小処理のみ
- カメラ・モデル初期化はタイル選択後
- `LoadLibraryExW` の失敗を処理する
- 依存DLL欠落時にLogonUIをクラッシュさせない
- 可能ならHost側だけにOpenCV依存を置く

## 13.3 緊急無効化

レジストリにアプリ独自の有効フラグを持つ。

```text
HKLM\Software\HomeFaceLogon
  Enabled = 0 or 1
```

ProviderはCOM生成直後の軽量処理でこの値を確認し、0なら資格情報を列挙しない。

さらに、次を用意する。

- `disable-provider.ps1`
- `enable-provider.ps1`
- `unregister-provider.ps1`
- `remove-files.ps1`
- WinRE用 `.reg`
- 手動で削除すべきCLSID一覧
- 依存DLLを含む完全なアンインストール手順

## 13.4 System Provider

次を禁止する。

- `ICredentialProviderFilter` 実装
- Microsoft ProviderのGUIDを無効化
- Group PolicyでパスワードProviderを消す
- PIN Providerを消す
- Windows Helloを削除する
- Face Logonを唯一のサインイン手段にする

---

# 14. インストール設計

## 14.1 開発版

PowerShellで行う。

1. DLL・EXE・モデルをProgram Filesへコピー
2. 設定用ディレクトリ作成
3. ACL設定
4. COM CLSID登録
5. Credential Providers配下へ登録
6. Enabled=0で初期登録
7. 設定完了後にEnabled=1
8. 変更内容をログへ記録

## 14.2 リリース版

MSIを推奨する。

- Machine-wide install
- x64限定
- Upgrade Code固定
- DLL更新時の再起動要否を明示
- アンインストール前にProviderを無効化
- 使用中DLLの処理
- Rollback
- 修復インストール
- バージョンダウングレード禁止

コード署名は個人利用MVPの動作条件ではないが、最終的には署名を検討する。署名がないこととMicrosoftの個別許可が必要であることは別問題である。

---

# 15. リポジトリ構成

```text
HomeFaceLogon/
├─ CMakeLists.txt または HomeFaceLogon.sln
├─ README.md
├─ SECURITY.md
├─ docs/
│  ├─ requirements-and-design.md
│  ├─ adr/
│  │  ├─ 0001-use-credential-provider-v2.md
│  │  ├─ 0002-msa-qualified-user-name.md
│  │  ├─ 0003-dpapi-machine-scope.md
│  │  └─ 0004-out-of-process-face-host.md
│  ├─ recovery.md
│  └─ test-matrix.md
├─ src/
│  ├─ provider/
│  ├─ host/
│  ├─ setup/
│  ├─ core/
│  ├─ crypto/
│  ├─ face/
│  └─ diagnostics/
├─ include/
├─ models/
├─ resources/
├─ tests/
│  ├─ unit/
│  ├─ integration/
│  └─ fixtures/
├─ tools/
│  ├─ install-dev.ps1
│  ├─ uninstall-dev.ps1
│  ├─ disable-provider.ps1
│  ├─ enable-provider.ps1
│  └─ collect-diagnostics.ps1
└─ third_party/
   └─ NOTICE.md
```

---

# 16. 実装フェーズ

## Phase 0: 環境と復旧手段

完了条件:

- Windows 11 VMを用意
- スナップショット取得
- Microsoft公式V2サンプルをビルド
- 登録・解除成功
- 標準Providerが残っていることを確認
- Safe ModeまたはWinREから無効化する手順を実地確認

このフェーズでは顔認識を実装しない。

## Phase 1: Microsoftアカウント認証スパイク

完了条件:

- V2 Providerでユーザー配列を取得
- 対象MSAのSID取得
- Qualified User Name取得
- Provider ID取得
- タイル表示
- 設定アプリからDPAPI保存
- 手動SubmitでMSAログオン成功
- ロック解除成功
- 認証失敗時に標準Providerへ戻れる
- 秘密がログへ出ない

**このフェーズが不成功ならプロジェクトを先へ進めない。**

## Phase 2: 通常デスクトップ上の顔認識

完了条件:

- Setupアプリでカメラ列挙
- Enrollment
- Face Template保存
- 本人照合
- 非本人照合
- 閾値調整
- 100回程度の認識試験
- モデル・ライセンス記録

## Phase 3: Logon環境のカメラスパイク

完了条件:

- Provider選択時にHost起動
- Hostがログオン前にカメラ取得
- タイムアウト
- キャンセル
- カメラ競合
- Host異常終了
- LogonUIがクラッシュしない

## Phase 4: 顔照合とAuto-submit統合

完了条件:

- 顔照合成功
- `CredentialsChanged`
- `GetCredentialCount(autoLogon=TRUE)`
- `GetSerialization`
- MSAログオン成功
- 二重提出なし
- 失敗後の無限ループなし

## Phase 5: ハードニング

完了条件:

- ACL
- ログローテーション
- エラー処理
- 依存DLL欠落
- 設定破損
- モデル破損
- カメラ抜去
- パスワード変更
- Windows Update後試験
- アンインストール
- 実機30回連続ロック解除

---

# 17. 受入条件

## 17.1 機能

- Microsoftアカウントの既存Windowsプロファイルで動作する
- ローカルアカウント作成を利用者へ要求しない
- 顔照合後に自動ログオンする
- コールドブート後とロック解除の両方で動作する
- カメラ未接続時に標準ログインを妨げない
- パスワード不一致時に標準ログインを妨げない
- PIN・パスワードProviderが常時利用できる
- 設定アプリから顔とパスワードを更新できる
- 無効化・アンインストールできる

## 17.2 安定性

- 30回連続のロック・解除に成功
- Hostを強制終了してもLogonUIが継続
- モデルファイルを削除してもLogonUIが継続
- config.jsonを破損させてもLogonUIが継続
- secret.binを破損させてもLogonUIが継続
- カメラを途中で抜いてもLogonUIが継続
- Providerが自動再試行ループへ入らない
- 認証失敗後に標準Providerを選択できる

## 17.3 秘密情報

- ログにパスワードが出ない
- ログに認証バッファーが出ない
- 平文パスワードがファイルへ保存されない
- Face Templateは暗号化される
- 秘密バッファーは使用後ゼロ化される
- `%ProgramData%` の秘密ファイルを一般ユーザーが読めない

---

# 18. 試験マトリクス

| 分類 | ケース |
|---|---|
| シナリオ | Boot / Sign-out / Lock / Sleep Resume |
| ネットワーク | Online / Offline |
| アカウント | MSA / Local test account |
| Windows設定 | Hello-only ON / OFF |
| カメラ | 正常 / 未接続 / 使用中 / 途中抜去 |
| 顔 | 本人 / 非本人 / 顔なし / 複数人 |
| 秘密 | 正常 / 古いパスワード / 破損BLOB |
| モデル | 正常 / 欠落 / 破損 / バージョン不一致 |
| Host | 正常 / 起動失敗 / ハング / 強制終了 |
| Provider | Enabled / Disabled |
| 更新 | Windows Update前後 / アプリ更新前後 |
| 復旧 | PowerShell無効化 / Safe Mode / WinRE |

補足:

Windows更新後の最初の自動サインインでは、Windows側のAutomatic Restart Sign-Onにより第三者Credential Providerが呼ばれない場合がある。これは既知のWindows挙動として試験結果を区別する。

---

# 19. agyへの実装規約

1. 非公開APIを使わない
2. Microsoft公式V2サンプルを基礎にする
3. System Providerをラップしない
4. Credential Provider Filterを作らない
5. Windowsバイナリをパッチしない
6. LSASSへ独自コードをロードしない
7. MSAのQualified User Nameを推測しない
8. 顔認識より先にMSAシリアライズを実証する
9. 一つのPRに複数フェーズを混ぜない
10. 各PRへ試験手順と復旧手順を書く
11. 失敗時は安全側へ倒す
12. HRESULTとWin32 Errorを失わない
13. `DllMain` を最小化する
14. COM参照カウントを単体試験する
15. 秘密バッファーをRAIIで管理し、破棄時にゼロ化する
16. ログに資格情報を出さない
17. 不明点を推測実装せず、技術スパイクとして切り出す
18. 公式資料と実機結果が矛盾した場合、実機結果を記録し設計を更新する
19. Windows VM上で復旧可能な状態を確認してから実機へ入れる
20. 「ビルドが通った」を完成条件にしない

---

# 20. 最初にagyへ渡す指示

以下の順序を崩さないこと。

1. この設計書を読む
2. 公式V2 Credential Providerサンプルを取得
3. Windows 11 x64向けにビルド
4. 開発用登録・解除スクリプトを作る
5. Providerを無効化する緊急スクリプトを作る
6. `ICredentialProviderSetUserArray` を実装
7. ユーザーのSID、Qualified User Name、Provider IDを秘密を含まない形で診断ログへ出す
8. 対象SIDのタイルを一つだけ列挙
9. 顔認識なしでMSAログオンを実証
10. 実証結果とエラーコードを報告
11. 承認前に顔認識へ着手しない

---

# 21. 参考資料

## Microsoft公式

### Credential Provider全体

- Credential Providers in Windows  
  https://learn.microsoft.com/en-us/windows/win32/secauthn/credential-providers-in-windows

- V2 Credential Provider Sample  
  https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/credential-provider/

- Microsoft Windows Classic Samples / CredentialProvider  
  https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/CredentialProvider

### ユーザー列挙

- ICredentialProviderSetUserArray  
  https://learn.microsoft.com/en-us/windows/win32/api/credentialprovider/nn-credentialprovider-icredentialprovidersetuserarray

- ICredentialProviderUserArray  
  https://learn.microsoft.com/en-us/windows/win32/api/credentialprovider/nn-credentialprovider-icredentialprovideruserarray

- ICredentialProviderUser  
  https://learn.microsoft.com/en-us/windows/win32/api/credentialprovider/nn-credentialprovider-icredentialprovideruser

- ICredentialProviderUser::GetStringValue  
  https://learn.microsoft.com/en-us/windows/win32/api/credentialprovider/nf-credentialprovider-icredentialprovideruser-getstringvalue

- ICredentialProviderUser::GetValue  
  https://learn.microsoft.com/en-us/windows/win32/api/credentialprovider/nf-credentialprovider-icredentialprovideruser-getvalue

### 資格情報梱包

- CredPackAuthenticationBufferW  
  https://learn.microsoft.com/en-us/windows/win32/api/wincred/nf-wincred-credpackauthenticationbufferw

- SEC_WINNT_AUTH_IDENTITY_EX2  
  https://learn.microsoft.com/en-us/windows/win32/api/sspi/ns-sspi-sec_winnt_auth_identity_ex2

- ICredentialProviderCredential::GetSerialization  
  https://learn.microsoft.com/en-us/windows/win32/api/credentialprovider/nf-credentialprovider-icredentialprovidercredential-getserialization

### Usage ScenarioとAuto-submit

- CREDENTIAL_PROVIDER_USAGE_SCENARIO  
  https://learn.microsoft.com/en-us/windows/win32/api/credentialprovider/ne-credentialprovider-credential_provider_usage_scenario

- ICredentialProvider::GetCredentialCount  
  https://learn.microsoft.com/en-us/windows/win32/api/credentialprovider/nf-credentialprovider-icredentialprovider-getcredentialcount

- ICredentialProviderEvents::CredentialsChanged  
  https://learn.microsoft.com/en-us/windows/win32/api/credentialprovider/nf-credentialprovider-icredentialproviderevents-credentialschanged

- ICredentialProviderCredentialEvents2  
  https://learn.microsoft.com/en-us/windows/win32/api/credentialprovider/nn-credentialprovider-icredentialprovidercredentialevents2

### UIフィールド

- CREDENTIAL_PROVIDER_FIELD_TYPE  
  https://learn.microsoft.com/en-us/windows/win32/api/credentialprovider/ne-credentialprovider-credential_provider_field_type

### 秘密情報

- CryptProtectData  
  https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata

- CryptUnprotectData  
  https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptunprotectdata

- Credential Providerの秘密情報破棄に関するBest Practices  
  https://learn.microsoft.com/en-us/windows/win32/api/credentialprovider/nn-credentialprovider-icredentialprovidercredential

### MicrosoftアカウントとPasswordless

- How to go passwordless with your Microsoft account  
  https://support.microsoft.com/en-us/accounts-billing/security/how-to-go-passwordless-with-your-microsoft-account

- Go passwordless in Windows  
  https://support.microsoft.com/en-us/windows/go-passwordless-in-windows-585a71d7-2295-4878-aeac-a014984df856

### 既知挙動

- Custom credential providers don't load when you first log on  
  https://learn.microsoft.com/en-us/troubleshoot/windows-client/user-profiles-and-logon/custom-credential-providers-dont-load-first-logon

## OpenCV公式

- OpenCV Zoo  
  https://github.com/opencv/opencv_zoo

- YuNet face detection  
  https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet

- SFace face recognition  
  https://github.com/opencv/opencv_zoo/tree/main/models/face_recognition_sface

---

# 22. 設計上の未確定事項

以下は資料だけで断定せず、スパイク結果で決める。

1. 対象WindowsビルドでのMSA認証バッファーの最終的な細部
2. Hello-only設定ONで本Providerが表示・認証されるか
3. オフライン時のMSAキャッシュ認証の具体的挙動
4. FaceLogonHostからログオン前に対象カメラを取得できるか
5. カメラFrame Serverとの競合
6. 使用するOpenCVビルド方式
7. SFaceの閾値
8. Hostの起動方式とセキュアデスクトップ上の実行条件
9. Windows Update後の互換性

未確定事項を、想像で「対応済み」と扱わないこと。

---

# 23. MVP完成の定義

次の条件をすべて満たした時だけMVP完成とする。

- 実際のMicrosoftアカウントで動く
- ローカルアカウントを利用者へ要求しない
- 顔照合から自動ログオンまで一連で動く
- 標準PIN・パスワードが常に残る
- Provider、Host、モデル、カメラの障害でLogonUIが使用不能にならない
- Safe ModeまたはWinREから無効化できる
- 平文パスワードを永続保存しない
- 30回連続ロック解除を通過する
- Windows再起動後にも動く
- 設定アプリからパスワードを更新できる
- アンインストールできる

---

# 24. GitHub公開・秘密情報管理

## 24.1 基本方針

本リポジトリは最初から公開されるものとして扱う。

リポジトリへ置くもの:

- ソースコード
- ビルド定義
- 設計書
- 復旧手順
- 秘密を含まないテストコード
- ダミー値だけを含む `*.example.*`
- 設定スキーマ
- モデル取得スクリプト
- モデルのライセンス、URL、SHA-256一覧
- 匿名化済みの試験結果

リポジトリへ置かないもの:

- Microsoftアカウントのメールアドレス
- Qualified User Nameの実値
- Windowsユーザー名
- コンピューター名
- 実SID
- カメラのSymbolic Link、VID/PID、デバイスインスタンスID
- 顔画像、動画、スクリーンショット
- Face Template、Embedding
- `secret.bin`、`face.bin`、DPAPI BLOB
- 実運用の `config.json`
- ログ、ETL、イベントログ、クラッシュダンプ
- 実機からexportしたレジストリ
- PFX、秘密鍵、コード署名用秘密情報
- 個人環境の試験レポート
- AIエージェントのローカル会話ログや作業用スクラッチ

## 24.2 データ配置

実行時データはGit作業ツリーへ保存しない。

正式な保存先:

```text
%ProgramData%\HomeFaceLogon\
```

開発中にローカルデータを作業ツリー内へ置く必要がある場合は、次のいずれかだけを使用する。

```text
local/
private/
runtime-data/
diagnostics/
captures/
test-results/
```

これらはすべて `.gitignore` 対象とする。

## 24.3 設定ファイル

- 実値入り設定: `config.local.json` または `local/config.json`
- 公開用雛形: `config.example.json`
- 検証用仕様: `config.schema.json`

`config.example.json` には次のような明白なダミー値だけを使う。

```json
{
  "targetSid": "S-1-5-21-0000000000-0000000000-0000000000-1001",
  "qualifiedUserName": "ExampleProvider\\user@example.invalid",
  "cameraSymbolicLink": "EXAMPLE_CAMERA_SYMBOLIC_LINK"
}
```

`.invalid` ドメインを使用し、実在するメールアドレスを例示しない。

## 24.4 顔モデル

ONNX等の大容量モデル本体は、ライセンスとGitHub上の容量を確認するまでコミットしない。

原則:

- `models/README.md` をコミットする
- `models/checksums.txt` をコミットする
- 公式配布元から取得する `tools/fetch-models.ps1` をコミットする
- ダウンロード後にSHA-256を検証する
- `models/*.onnx` 等は `.gitignore` する
- 再配布が明確に許可され、容量上の問題もない場合だけ別途判断する

## 24.5 `.gitignore` の位置付け

`.gitignore` は事故防止策であり、秘密管理の境界そのものではない。

- 秘密は最初から作業ツリー外へ保存する
- 公開前に `git status --ignored` を確認する
- `tools/check-public-tree.ps1` を実行する
- `git ls-files` で追跡対象を確認する
- 実データをテストFixtureへコピーしない
- ログをIssueやPull Requestへそのまま貼らない
- 診断情報を共有する場合は匿名化する

## 24.6 誤ってコミットした場合

秘密情報を一度でもコミットした場合、現在のファイルを削除するだけでは不十分である。

直ちに以下を行う。

1. pushを停止する
2. 保存済みMicrosoftアカウントパスワードを変更する
3. Face TemplateとDPAPI BLOBを再生成する
4. Git履歴から対象データを削除する
5. 公開済みの場合はGitHub上の履歴とキャッシュを考慮する
6. 再発防止のignore規則と検査を追加する

## 24.7 agyへの強制規約

agyは、最初の実装PRより前に次を作成する。

- ルート `.gitignore`
- `config.example.json`
- `tools/check-public-tree.ps1`
- `docs/public-repository-policy.md`

agyは次をしてはならない。

- 実SIDや実メールアドレスをソースへハードコードする
- ローカル診断ログをFixtureとしてコミットする
- 顔画像をテスト資産としてコミットする
- DPAPI BLOBを「暗号化済みだから安全」と判断してコミットする
- 秘密鍵やPFXをGitHub Actionsへ直接置く
- 実機の `.reg` exportをコミットする
- `.gitignore` へ追加しただけで、既に追跡済みのファイルが消えたと判断する

公開用テストデータは、すべて人工的なダミー値から作る。
