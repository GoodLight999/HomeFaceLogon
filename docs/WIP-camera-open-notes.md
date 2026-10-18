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

## TODO（次回）
1. 初回だけプロセス全体が 6.5 秒かかる原因を調べる（カメラ処理は 1.2 秒なので、残り約 5 秒はカメラ以外。コールドスタート / DLL・モデル読み込みが怪しいが未確認）
2. インストールしてロック画面で実機テスト（Advise 変更の効果、カメラの二重起動が無くなったかを含む）
3. 最初のフレームまで約 1 秒かかる点の短縮余地を確認
4. インストーラーを再ビルド

## 備考
- `dist/` は未追跡のビルド成果物（約 68MB）なのでコミットしていない
