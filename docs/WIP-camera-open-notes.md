# WIP: カメラ起動処理の共通化（作業メモ）

最終更新: 2026-10-18 / ブランチ `agent/camera-capture-common`

## 状態
- ビルド: Debug / Release とも成功（未インストール）
- `FaceLogonSetup` でのカメラプローブ: 3回連続成功（`--data-dir` に一時フォルダを指定、本番 ProgramData は未変更）
- **ロック画面（Credential Provider → Host）の実機テストは未実施**
- インストーラー（`HomeFaceLogon.iss`）は再ビルドしていない

## 変更内容
- `src/common/camera_capture.h`（新規）: Host / Setup 共通のカメラオープン処理
  - DirectShow 列挙でカメラ名・index を取得
  - MSMF を優先して開き、成功した方式とカメラ名をキャッシュ。次回からはその方式を先に試す
  - 最大3回リトライ。各段階（open / 最初のフレーム）の所要時間をログに出す
- `src/host/main.cpp`
  - `OpenCameraWithRetry` を共通モジュールで作り直した（ウォッチドッグはそのまま）
  - キャプチャループで空フレームを数回まで許容（1回で失敗扱いにしない）
- `src/setup/main.cpp`: 登録時と認証時で同じオープン処理を使うように統一
- `src/provider/CSampleCredential.cpp`: `Advise` で `_StopHostProcess()` を呼ばないよう変更
  - 理由: LogonUI が `SetSelected` の前に `Advise` を（再）発行すると、カメラが起動→停止→再起動していた。`UnAdvise` 側では引き続き停止する

## 計測（Logicool HD Webcam C270, 640x480）
| 回 | 方式 | open | 最初のフレーム | カメラ合計 | プロセス全体 |
|---|---|---|---|---|---|
| 1 | MSMF（既定） | 178ms | 1025ms | 1203ms | **6508ms** |
| 2 | MSMF（キャッシュ） | 138ms | 957ms | 1095ms | 1417ms |
| 3 | MSMF（キャッシュ） | 174ms | 962ms | 1137ms | 1380ms |

ベンチ（`msmf_params`, `OPENCV_VIDEOIO_MSMF_ENABLE_HW_TRANSFORMS=0`）: open≈150ms, first≈956ms, total≈1.1s

## 初回 6.5 秒の原因（2026-10-18 調査済み）
カメラ処理ではなく、**プロセス起動時の DLL 読み込み（main 到達前）**。内訳はおおよそ:
- Defender の初回スキャン: 一度も実行されていないハッシュの `opencv_world4100.dll`（64MB）を読み込むと
  `FaceLogonSetup.exe --capabilities`（何もせず終了）でも **3516ms**。2回目以降は 37ms
  （再現方法: exe と DLL を一時フォルダへコピーし末尾にバイトを足してハッシュを変えて実行）
- ディスクキャッシュが冷えた状態: 同じく `--capabilities` で **1153ms**（翌日の初回実行）
- カメラ処理: 約 1.2 秒（キャッシュ無し=既定の MSMF でも 1.2 秒。キャッシュ有無で差は無い）

つまり 6.5 秒はビルド直後の一回限り。ロック画面で効くのは「インストール/更新直後の最初のサインイン」と「再起動直後（ディスク冷え ~1 秒）」。

本番ログ（インストール済みの旧版, DSHOW）では、プロセス起動→モデル読込は 1 秒未満で、
遅いのは `CAP_DSHOW` の open 自体（4.7〜7.0 秒、速い時は 0.9 秒）。MSMF 化の効果はロック画面で要確認。

対策（このブランチで追加）:
- `installer/HomeFaceLogon.iss` の `[Run]` でインストール直後に `FaceLogonSetup.exe --capabilities` を実行し、Defender スキャンを先に済ませる
- `src/host/main.cpp` の起動ログに「プロセス生成→main 到達」のミリ秒を出す
  - 注意: Defender の exe スキャンはプロセス生成**前**に走るのでこの値には出ない（ビルド直後の実測: 接続まで 4.1 秒でもこの値は 79ms）
- `src/provider/CSampleCredential.cpp`: Host からのメッセージごとに「起動要求からの経過 ms」を `FaceLogon.log` に出す。ロック画面での体感時間はこちらで見る

## 偽パイプでの Host 通しテスト（2026-10-18, ユーザーセッション）
CP と同じ手順（message モードのパイプ + nonce）で Host を起動し、STATUS / MATCHED を受け取るスクリプトで計測。
- 通常: **起動→MATCHED 1.5〜1.7 秒**（10 回中 10 回成功）。旧版はカメラ open だけで 4.7〜7 秒
- カメラアプリが前面でカメラを使用中: MSMF/DSHOW とも取れない（Windows の排他。旧版も同じ）
- カメラアプリ使用中に Host 起動 → 3 秒後にアプリ終了:
  - 修正前: 解放→MATCHED 4.2〜6.2 秒、しかも 1 回の DSHOW 成功でキャッシュが **DSHOW に固定**された（ロック画面で遅い方）
  - 修正後: 解放→MATCHED **1.6〜2.8 秒**、キャッシュは MSMF のまま

見つかって直したもの:
- 使用中に開いた MSMF セッションは、カメラ解放後もフレームを返さない → MSMF が「開けたがフレーム無し」なら DSHOW に行く前に MSMF を 1 回開き直す。MSMF の最初のフレーム待ちは 3 秒に短縮（通常 0.9〜1.05 秒）
- キャッシュの降格条件: DSHOW 優先にするのは「MSMF 失敗→DSHOW 成功」が 2 セッション連続した時だけ。DSHOW 優先でも 10 セッションごとに MSMF を再試行（`src/common/camera_preference.h`、回帰テストあり）
- Advise 変更の回帰テストを追加（旧コードに戻すと FAIL することを確認済み）

## TODO（次回）
1. ~~初回だけ 6.5 秒かかる原因~~ → 上記で解決（Defender 初回スキャン + コールドキャッシュ）
2. インストールしてロック画面で実機テスト（下の「リリース判定」参照）
3. 最初のフレームまで約 1 秒かかる点の短縮余地を確認
4. ~~インストーラーを再ビルド~~

## リリース判定（ロック画面でしか確認できないもの）
- Host が SYSTEM / LogonUI 配下でも MSMF で開けるか（`FaceLogonHost.log` の `MSMF first frame`）
- 起動→MATCHED が数秒以内か（`FaceLogon.log` の `Host message type=4, N ms after host launch request`）
- スリープ復帰直後でも MSMF が速いか
- カメラアプリを開いたままロック → 顔認証できるか
- カメラが二重起動しないか（`FaceLogon.log` で Advise 後に `Starting FaceLogonHost` が 1 回だけ）

## 備考
- `dist/` は未追跡のビルド成果物（約 68MB）なのでコミットしていない
