import { useState, useEffect } from "react";
import { invoke } from "@tauri-apps/api/core";

interface CameraDevice {
  index: number;
  name: string;
}

interface RegistryStatus {
  enabled: boolean;
  sid: string;
  threshold: number;
  required_matches: number;
  window_size: number;
  scan_timeout_ms: number;
  liveness_enabled: boolean;
  camera_index: number;
  setup_complete: boolean;
}

function App() {
  const [activeTab, setActiveTab] = useState<
    "wizard" | "settings" | "diagnostics"
  >("wizard");
  const [wizardStep, setWizardStep] = useState<number>(0);

  // Wizard Input States
  const [sid, setSid] = useState<string>("");
  const [password, setPassword] = useState<string>("");
  const [pin, setPin] = useState<string>("");
  const [cameraIndex, setCameraIndex] = useState<number>(0);
  const [cameraDevices, setCameraDevices] = useState<CameraDevice[]>([]);
  const [cameraListError, setCameraListError] = useState<string>("");

  // App Config State (Synchronized with registry & config.json)
  const [config, setConfig] = useState<RegistryStatus>({
    enabled: false,
    sid: "",
    threshold: 0.363,
    required_matches: 3,
    window_size: 5,
    scan_timeout_ms: 10000,
    liveness_enabled: false,
    camera_index: 0,
    setup_complete: false,
  });

  const isConfigured = config.setup_complete;

  // Logging & Diagnostics States
  const [logType, setLogType] = useState<
    "FaceLogonHost.log" | "FaceLogonSetup.log"
  >("FaceLogonHost.log");
  const [logContent, setLogContent] =
    useState<string>("ログを読み込んでいます...");
  const [diagOutput, setDiagOutput] = useState<string>("");
  const [diagRunning, setDiagRunning] = useState<boolean>(false);

  // UI States
  const [loading, setLoading] = useState<boolean>(false);
  const [statusMsg, setStatusMsg] = useState<string>("");
  const [statusType, setStatusType] = useState<"info" | "success" | "error">(
    "info",
  );

  // Load configuration on mount
  useEffect(() => {
    loadConfig();
    loadCameras();
  }, []);

  // Poll logs when on diagnostics tab
  useEffect(() => {
    let interval: any;
    if (activeTab === "diagnostics") {
      refreshLogs();
      interval = setInterval(refreshLogs, 3000);
    }
    return () => {
      if (interval) clearInterval(interval);
    };
  }, [activeTab, logType]);

  const loadConfig = async () => {
    try {
      const res = await invoke<RegistryStatus>("get_registry_status");
      setConfig(res);
      setCameraIndex(res.camera_index);
      setSid(res.sid.toString());
    } catch (err: any) {
      showStatus(`設定の読み込みに失敗しました: ${err}`, "error");
    }
  };

  const loadCameras = async () => {
    try {
      const devices = await invoke<CameraDevice[]>("list_cameras");
      setCameraDevices(devices);
      setCameraListError("");
      if (!devices.some((device) => device.index === cameraIndex)) {
        setCameraIndex(devices[0].index);
      }
    } catch (err: any) {
      setCameraDevices([]);
      setCameraListError(String(err));
    }
  };

  const showStatus = (
    msg: string,
    type: "info" | "success" | "error" = "info",
  ) => {
    setStatusMsg(msg);
    setStatusType(type);
    setTimeout(() => {
      setStatusMsg("");
    }, 6000);
  };

  const handleAutoDetectSid = async () => {
    setLoading(true);
    try {
      const detectedSid = await invoke<string>("get_user_sid");
      setSid(detectedSid);
      showStatus("SIDを自動検出しました。", "success");
    } catch (err: any) {
      showStatus(`SIDの検出に失敗しました: ${err}`, "error");
    } finally {
      setLoading(false);
    }
  };

  const handleSaveSetupCredentials = async () => {
    if (!sid) {
      showStatus("SIDを入力してください。", "error");
      return;
    }
    if (!password) {
      showStatus("Windowsサインインパスワードを入力してください。", "error");
      return;
    }
    if (pin.length < 4 || pin.length > 16 || !/^[\x21-\x7E]+$/.test(pin)) {
      showStatus("PINは4〜16文字の半角英数字・記号（スペース除く）で入力してください。", "error");
      return;
    }
    // Do not write partial credentials yet. Commit the entire setup only after
    // camera probing and face enrollment succeed.
    setWizardStep(3);
    await loadCameras();
  };

  const handleStartEnrollment = async () => {
    if (!cameraDevices.some((device) => device.index === cameraIndex)) {
      showStatus("一覧から有効なカメラを選択してください。", "error");
      return;
    }
    setLoading(true);
    try {
      showStatus("カメラ確認後に顔登録を開始します。失敗時は設定を元に戻します。", "info");
      await invoke<string>("run_complete_setup", { sid, password, pin, cameraIndex });
      setPassword("");
      setPin("");
      showStatus("資格情報・PIN・顔情報を一括で登録しました。", "success");
      setWizardStep(4);
      await loadConfig();
    } catch (err: any) {
      showStatus(`セットアップに失敗しました: ${err}`, "error");
    } finally {
      setLoading(false);
    }
  };

  const handleSaveSettings = async () => {
    setLoading(true);
    try {
      await invoke("set_registry_status", {
        enabled: config.enabled,
        sid: config.sid,
        threshold: config.threshold,
        requiredMatches: config.required_matches,
        windowSize: config.window_size,
        scanTimeoutMs: config.scan_timeout_ms,
        livenessEnabled: config.liveness_enabled,
        cameraIndex: config.camera_index,
      });
      showStatus("設定を保存し適用しました。", "success");
      loadConfig();
    } catch (err: any) {
      showStatus(`設定の保存に失敗しました: ${err}`, "error");
    } finally {
      setLoading(false);
    }
  };

  const handleRunVerifyTest = async () => {
    setDiagRunning(true);
    setDiagOutput(
      "顔照合テストを実行中... カメラが起動します。カメラを見てください。\n",
    );
    try {
      const res = await invoke<string>("run_verify", { cameraIndex: config.camera_index });
      setDiagOutput((prev) => prev + "【結果】成功\n" + res);
      showStatus("照合テストが正常に完了しました。", "success");
    } catch (err: any) {
      setDiagOutput((prev) => prev + "【結果】失敗\nエラー: " + err);
      showStatus(`照合テストに失敗しました: ${err}`, "error");
    } finally {
      setDiagRunning(false);
    }
  };

  const handleRunDiagnostics = async () => {
    setDiagRunning(true);
    setDiagOutput("システム診断を実行中...\n");
    try {
      const res = await invoke<string>("run_test");
      setDiagOutput((prev) => prev + "【診断結果】\n" + res);
      showStatus("診断テストが完了しました。", "success");
    } catch (err: any) {
      setDiagOutput((prev) => prev + "【診断結果】エラー\n" + err);
      showStatus(`診断テスト中にエラーが発生しました: ${err}`, "error");
    } finally {
      setDiagRunning(false);
    }
  };

  const refreshLogs = async () => {
    try {
      const content = await invoke<string>("read_log_file", { name: logType });
      setLogContent(content);
    } catch (err: any) {
      setLogContent(`ログの取得に失敗しました: ${err}`);
    }
  };

  return (
    <div className="app-container">
      {/* Sidebar Navigation */}
      <aside className="sidebar">
        <div className="brand">
          <div className="brand-icon">F</div>
          <div>
            <h1 className="brand-title">HomeFaceLogon</h1>
            <span className="brand-subtitle">顔認証サインイン設定</span>
          </div>
        </div>

        <nav className="nav-menu">
          <div
            className={`nav-item ${activeTab === "wizard" ? "active" : ""}`}
            onClick={() => setActiveTab("wizard")}
          >
            <svg
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              strokeWidth="2"
              strokeLinecap="round"
              strokeLinejoin="round"
            >
              <path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.77-3.77a6 6 0 0 1-7.94 7.94l-6.91 6.91a2.12 2.12 0 0 1-3-3l6.91-6.91a6 6 0 0 1 7.94-7.94l-3.76 3.76z" />
            </svg>
            {isConfigured
              ? "再セットアップ（上書き）"
              : "セットアップウィザード"}
          </div>

          <div
            className={`nav-item ${activeTab === "settings" ? "active" : ""}`}
            onClick={() => {
              setActiveTab("settings");
              loadConfig();
            }}
          >
            <svg
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              strokeWidth="2"
              strokeLinecap="round"
              strokeLinejoin="round"
            >
              <circle cx="12" cy="12" r="3" />
              <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z" />
            </svg>
            環境設定
          </div>

          <div
            className={`nav-item ${activeTab === "diagnostics" ? "active" : ""}`}
            onClick={() => setActiveTab("diagnostics")}
          >
            <svg
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              strokeWidth="2"
              strokeLinecap="round"
              strokeLinejoin="round"
            >
              <rect x="2" y="3" width="20" height="14" rx="2" ry="2" />
              <line x1="8" y1="21" x2="16" y2="21" />
              <line x1="12" y1="17" x2="12" y2="21" />
            </svg>
            動作確認とログ
          </div>
        </nav>

        <div className="sidebar-footer">
          <div>管理者権限実行中</div>
          <div>v0.3.5 camera recovery</div>
        </div>
      </aside>

      {/* Main Content Area */}
      <main className="main-content">
        {statusMsg && (
          <div className={`alert-banner alert-${statusType}`}>
            {statusType === "error" ? (
              <svg
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                strokeWidth="2"
              >
                <circle cx="12" cy="12" r="10" />
                <line x1="12" y1="8" x2="12" y2="12" />
                <line x1="12" y1="16" x2="12.01" y2="16" />
              </svg>
            ) : (
              <svg
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                strokeWidth="2"
              >
                <circle cx="12" cy="12" r="10" />
                <line x1="12" y1="16" x2="12" y2="12" />
                <line x1="12" y1="8" x2="12.01" y2="8" />
              </svg>
            )}
            <div>{statusMsg}</div>
          </div>
        )}

        {/* Tab 1: Wizard */}
        {activeTab === "wizard" && (
          <div>
            <div className="page-header">
              <h2 className="page-title">
                {isConfigured
                  ? "資格情報と顔データの再設定（すべて上書き）"
                  : "セットアップウィザード"}
              </h2>
              <p className="page-subtitle">
                {isConfigured
                  ? "登録済みの情報を新しい設定で完全に上書きします。"
                  : "Windowsサインインに顔認証とPINの追加を順を追って設定します。"}
              </p>
            </div>

            {/* Step Indicators */}
            {wizardStep > 0 && (
              <div className="steps-container">
                <div className="steps-line"></div>

                <div
                  className={`step-node ${wizardStep === 1 ? "active" : ""} ${wizardStep > 1 ? "completed" : ""}`}
                >
                  <div className="step-circle">
                    {wizardStep > 1 ? "✓" : "1"}
                  </div>
                  <span className="step-label">資格情報入力</span>
                </div>

                <div
                  className={`step-node ${wizardStep === 2 ? "active" : ""} ${wizardStep > 2 ? "completed" : ""}`}
                >
                  <div className="step-circle">
                    {wizardStep > 2 ? "✓" : "2"}
                  </div>
                  <span className="step-label">PIN設定</span>
                </div>

                <div
                  className={`step-node ${wizardStep === 3 ? "active" : ""} ${wizardStep > 3 ? "completed" : ""}`}
                >
                  <div className="step-circle">
                    {wizardStep > 3 ? "✓" : "3"}
                  </div>
                  <span className="step-label">顔登録</span>
                </div>

                <div
                  className={`step-node ${wizardStep === 4 ? "active" : ""}`}
                >
                  <div className="step-circle">4</div>
                  <span className="step-label">完了</span>
                </div>
              </div>
            )}

            {/* Step 0: Welcome Screen */}
            {wizardStep === 0 && (
              <div className="card success-card">
                <div
                  className="success-icon"
                  style={{
                    background: "var(--accent-light)",
                    color: "var(--accent)",
                  }}
                >
                  🔒
                </div>
                <h3 className="success-title">
                  {isConfigured
                    ? "登録済み設定の上書き"
                    : "HomeFaceLogon へようこそ"}
                </h3>
                <p
                  className="success-desc"
                  style={{ maxWidth: "500px", margin: "0 auto 24px" }}
                >
                  {isConfigured ? (
                    <>
                      すでに顔認証ログインがセットアップされています。
                      <br />
                      すべての設定（パスワード、PIN、顔データ）を上書きして再設定する場合は、下のボタンから開始してください。
                    </>
                  ) : (
                    <>
                      このウィザードでは、Windowsの標準カメラを使ってサインインできる「簡易顔認証」の設定を行います。
                      <br />
                      ※この機能は自宅等のイタズラ防止レベルのセキュリティを前提としています。写真等でのなりすまし対策はありません。
                    </>
                  )}
                </p>
                <button
                  className="btn btn-primary"
                  onClick={() => setWizardStep(1)}
                >
                  {isConfigured
                    ? "再セットアップを開始する（既存の設定を上書き）"
                    : "セットアップを開始する"}
                </button>
              </div>
            )}

            {/* Step 1: Target SID & Windows Password */}
            {wizardStep === 1 && (
              <div className="card">
                <h3 className="card-title">
                  ステップ 1: サインイン資格情報の紐付け
                </h3>
                <p
                  style={{
                    fontSize: "13px",
                    color: "var(--text-secondary)",
                    marginBottom: "20px",
                  }}
                >
                  自動サインインを実行するために、対象となるWindowsユーザーのSIDと、Microsoftアカウント（またはローカルアカウント）の現在のパスワードを入力します。パスワードはローカルマシンのセキュリティ境界(DPAPI)で暗号化され、安全に保管されます。
                </p>

                <div className="form-group">
                  <label className="form-label">対象ユーザーのSID</label>
                  <div className="form-input-container">
                    <input
                      type="text"
                      className="form-input"
                      placeholder="S-1-5-21-..."
                      value={sid}
                      onChange={(e) => setSid(e.target.value)}
                    />
                    <button
                      className="btn btn-secondary"
                      onClick={handleAutoDetectSid}
                      disabled={loading}
                    >
                      自動取得
                    </button>
                  </div>
                  <span className="form-help">
                    サインインを自動化するWindowsユーザーのセキュリティIDです。
                  </span>
                </div>

                <div className="form-group">
                  <label className="form-label">
                    Windowsサインインパスワード
                  </label>
                  <input
                    type="password"
                    className="form-input"
                    placeholder="現在のサインイン用パスワード"
                    value={password}
                    onChange={(e) => setPassword(e.target.value)}
                  />
                  <span className="form-help">
                    サインインを実行するために入力します。パスワードは直接送信されず暗号化されます。
                  </span>
                </div>

                <div
                  style={{
                    display: "flex",
                    gap: "12px",
                    justifyContent: "flex-end",
                    marginTop: "24px",
                  }}
                >
                  <button
                    className="btn btn-secondary"
                    onClick={() => setWizardStep(0)}
                  >
                    戻る
                  </button>
                  <button
                    className="btn btn-primary"
                    onClick={() => setWizardStep(2)}
                    disabled={!sid || !password}
                  >
                    次へ
                  </button>
                </div>
              </div>
            )}

            {/* Step 2: Backup PIN */}
            {wizardStep === 2 && (
              <div className="card">
                <h3 className="card-title">
                  ステップ 2: バックアップ PIN の設定
                </h3>
                <p
                  style={{
                    fontSize: "13px",
                    color: "var(--text-secondary)",
                    marginBottom: "20px",
                  }}
                >
                  顔認証が暗い部屋などで失敗した時のために、ロック画面からログインできる独自のPINを設定します。
                  <br />
                  PINは4文字から16文字の半角英数字・記号（スペース除く）で指定してください。
                </p>

                <div className="form-group">
                  <label className="form-label">
                    ローカル PIN（半角英数字・記号 4〜16文字）
                  </label>
                  <input
                    type="password"
                    className="form-input"
                    placeholder="新しい PIN を入力してください"
                    value={pin}
                    onChange={(e) => setPin(e.target.value)}
                    maxLength={16}
                  />
                </div>

                <div
                  style={{
                    display: "flex",
                    gap: "12px",
                    justifyContent: "flex-end",
                    marginTop: "24px",
                  }}
                >
                  <button
                    className="btn btn-secondary"
                    onClick={() => setWizardStep(1)}
                  >
                    戻る
                  </button>
                  <button
                    className="btn btn-primary"
                    onClick={handleSaveSetupCredentials}
                    disabled={loading || pin.length < 4}
                  >
                    {loading ? "保存中..." : "パスワード・PINを保存して次へ"}
                  </button>
                </div>
              </div>
            )}

            {/* Step 3: Face Registration */}
            {wizardStep === 3 && (
              <div className="card">
                <h3 className="card-title">ステップ 3: 顔情報の登録</h3>
                <p
                  style={{
                    fontSize: "13px",
                    color: "var(--text-secondary)",
                    marginBottom: "20px",
                  }}
                >
                  カメラを起動して顔写真を登録します。「登録開始」を押すと別ウィンドウでカメラが起動します。カメラをまっすぐ見つめて、30回分のサンプルが取得されるまでお待ちください（数秒で終了します）。
                </p>

                <div className="form-group">
                  <label className="form-label">
                    使用するカメラのインデックス
                  </label>
                  <div className="form-input-container">
                    <select
                      className="form-input"
                      value={cameraIndex}
                      onChange={(e) => setCameraIndex(parseInt(e.target.value))}
                      style={{ maxWidth: "420px" }}
                      disabled={cameraDevices.length === 0}
                    >
                      {cameraDevices.length === 0 ? (
                        <option value={-1}>カメラが見つかりません</option>
                      ) : (
                        cameraDevices.map((device) => (
                          <option key={device.index} value={device.index}>
                            [{device.index}] {device.name}
                          </option>
                        ))
                      )}
                    </select>
                    <button className="btn btn-secondary" onClick={loadCameras} disabled={loading}>
                      再読み込み
                    </button>
                  </div>
                  {cameraListError && (
                    <span className="form-help" style={{ color: "var(--danger)" }}>
                      {cameraListError}
                    </span>
                  )}
                </div>

                <div
                  style={{
                    display: "flex",
                    gap: "12px",
                    justifyContent: "flex-end",
                    marginTop: "24px",
                  }}
                >
                  <button
                    className="btn btn-secondary"
                    onClick={() => setWizardStep(2)}
                  >
                    戻る
                  </button>
                  <button
                    className="btn btn-success"
                    onClick={handleStartEnrollment}
                    disabled={loading || cameraDevices.length === 0}
                  >
                    {loading
                      ? "カメラ起動中..."
                      : "カメラを起動して顔登録を開始"}
                  </button>
                </div>
              </div>
            )}

            {/* Step 4: Finished */}
            {wizardStep === 4 && (
              <div className="card success-card">
                <div className="success-icon">✓</div>
                <h3 className="success-title">
                  セットアップがすべて完了しました！
                </h3>
                <p className="success-desc">
                  顔認証とバックアップPINの登録が完了しました。
                  <br />
                  サインイン画面でHomeFaceLogonタイルを有効にするには、以下のスイッチをオンにしてください。
                </p>

                <div
                  className="form-group"
                  style={{
                    display: "flex",
                    justifyContent: "center",
                    alignItems: "center",
                    gap: "16px",
                    margin: "24px 0",
                  }}
                >
                  <span style={{ fontWeight: 600 }}>
                    顔認証サインインを有効化:
                  </span>
                  <label className="switch">
                    <input
                      type="checkbox"
                      checked={config.enabled}
                      onChange={async (e) => {
                        const nextVal = e.target.checked;
                        try {
                          await invoke("set_registry_status", {
                            enabled: nextVal,
                            sid: config.sid,
                            threshold: config.threshold,
                            requiredMatches: config.required_matches,
                            windowSize: config.window_size,
                            scanTimeoutMs: config.scan_timeout_ms,
                            livenessEnabled: config.liveness_enabled,
                            cameraIndex: config.camera_index,
                          });
                          setConfig((prev) => ({ ...prev, enabled: nextVal }));
                          showStatus(
                            `顔認証サインインを${nextVal ? "有効" : "無効"}にしました。`,
                            "success",
                          );
                        } catch (err: any) {
                          showStatus(`無効化に失敗しました: ${err}`, "error");
                        }
                      }}
                    />
                    <span className="slider-switch"></span>
                  </label>
                </div>

                <button
                  className="btn btn-primary"
                  onClick={() => setWizardStep(0)}
                >
                  ウィザードを閉じる
                </button>
              </div>
            )}
          </div>
        )}

        {/* Tab 2: Settings */}
        {activeTab === "settings" && (
          <div>
            <div className="page-header">
              <h2 className="page-title">環境設定</h2>
              <p className="page-subtitle">
                顔認証エンジンの動作パラメータやカメラの設定を調整します。
              </p>
            </div>

            <div className="card">
              <h3 className="card-title">基本設定</h3>
              <div
                className="form-group"
                style={{
                  display: "flex",
                  justifyContent: "space-between",
                  alignItems: "center",
                }}
              >
                <div>
                  <div style={{ fontWeight: 600 }}>
                    顔認証サインインの有効化
                  </div>
                  <span className="form-help">
                    ロック画面やログイン時にHomeFaceLogonを表示します。
                  </span>
                </div>
                <label className="switch">
                  <input
                    type="checkbox"
                    checked={config.enabled}
                    onChange={async (e) => {
                      const nextVal = e.target.checked;
                      try {
                        await invoke("set_registry_status", {
                          enabled: nextVal,
                          sid: config.sid,
                          threshold: config.threshold,
                          requiredMatches: config.required_matches,
                          windowSize: config.window_size,
                          scanTimeoutMs: config.scan_timeout_ms,
                          livenessEnabled: config.liveness_enabled,
                          cameraIndex: config.camera_index,
                        });
                        setConfig((prev) => ({ ...prev, enabled: nextVal }));
                        showStatus(
                          `顔認証サインインを${nextVal ? "有効" : "無効"}にしました。`,
                          "success",
                        );
                      } catch (err: any) {
                        showStatus(`設定の変更に失敗しました: ${err}`, "error");
                      }
                    }}
                  />
                  <span className="slider-switch"></span>
                </label>
              </div>

              <div className="form-group" style={{ marginTop: "20px" }}>
                <label className="form-label">対象WindowsユーザーのSID</label>
                <div className="form-input-container">
                  <input
                    type="text"
                    className="form-input"
                    value={config.sid.toString()}
                    onChange={(e) =>
                      setConfig((prev) => ({ ...prev, sid: e.target.value }))
                    }
                  />
                  <button
                    className="btn btn-secondary"
                    onClick={async () => {
                      try {
                        const detectedSid =
                          await invoke<string>("get_user_sid");
                        setConfig((prev) => ({ ...prev, sid: detectedSid }));
                      } catch (e: any) {
                        showStatus(`検出エラー: ${e}`, "error");
                      }
                    }}
                  >
                    自動取得
                  </button>
                </div>
              </div>

              <div className="form-group" style={{ marginTop: "20px" }}>
                <label className="form-label">
                  使用するカメラ
                </label>
                <div className="form-input-container">
                  <select
                    className="form-input"
                    value={config.camera_index}
                    onChange={(e) => setConfig((prev) => ({ ...prev, camera_index: parseInt(e.target.value) }))}
                    style={{ maxWidth: "420px" }}
                    disabled={cameraDevices.length === 0}
                  >
                    {cameraDevices.length === 0 ? (
                      <option value={-1}>カメラが見つかりません</option>
                    ) : (
                      cameraDevices.map((device) => (
                        <option key={device.index} value={device.index}>
                          [{device.index}] {device.name}
                        </option>
                      ))
                    )}
                  </select>
                  <button className="btn btn-secondary" onClick={loadCameras} disabled={loading}>
                    再読み込み
                  </button>
                </div>
                <span className="form-help">Windowsが報告した具体的なデバイス名から選択します。</span>
              </div>
            </div>

            <div className="card">
              <h3 className="card-title">
                顔認識パラメータ（セキュリティと速度の調整）
              </h3>

              <div className="form-group">
                <label className="form-label">
                  顔照合のしきい値（コサイン類似度）
                </label>
                <div className="slider-container">
                  <input
                    type="range"
                    className="slider-input"
                    min={0.3}
                    max={1.0}
                    step={0.005}
                    value={config.threshold}
                    onChange={(e) =>
                      setConfig((prev) => ({
                        ...prev,
                        threshold: parseFloat(e.target.value),
                      }))
                    }
                  />
                  <span className="slider-value">
                    {config.threshold.toFixed(3)}
                  </span>
                </div>
                <span className="form-help">
                  推奨値:
                  0.363。値を大きくすると認証が厳格になりますが、認識されにくくなります。値を小さくすると認識されやすくなりますが、他人がログインできるリスクが高まります。
                </span>
              </div>

              <div className="form-group">
                <label className="form-label">必要一致フレーム数</label>
                <div className="slider-container">
                  <input
                    type="range"
                    className="slider-input"
                    min={1}
                    max={10}
                    step={1}
                    value={config.required_matches}
                    onChange={(e) =>
                      setConfig((prev) => ({
                        ...prev,
                        required_matches: parseInt(e.target.value),
                      }))
                    }
                  />
                  <span className="slider-value">
                    {config.required_matches} 回
                  </span>
                </div>
                <span className="form-help">
                  一致したと判定するために、スライディング窓内で最低限必要な一致フレーム数です（誤検知防止）。
                </span>
              </div>

              <div className="form-group">
                <label className="form-label">
                  スライディング窓サイズ（履歴フレーム数）
                </label>
                <div className="slider-container">
                  <input
                    type="range"
                    className="slider-input"
                    min={1}
                    max={20}
                    step={1}
                    value={config.window_size}
                    onChange={(e) =>
                      setConfig((prev) => ({
                        ...prev,
                        window_size: parseInt(e.target.value),
                      }))
                    }
                  />
                  <span className="slider-value">
                    {config.window_size} フレーム
                  </span>
                </div>
                <span className="form-help">
                  過去何フレームの一致履歴を追跡するか（デフォルト: 5）。
                </span>
              </div>

              <div className="form-group">
                <label className="form-label">スキャンタイムアウト時間</label>
                <div className="slider-container">
                  <input
                    type="range"
                    className="slider-input"
                    min={3000}
                    max={30000}
                    step={1000}
                    value={config.scan_timeout_ms}
                    onChange={(e) =>
                      setConfig((prev) => ({
                        ...prev,
                        scan_timeout_ms: parseInt(e.target.value),
                      }))
                    }
                  />
                  <span className="slider-value">
                    {(config.scan_timeout_ms / 1000).toFixed(0)} 秒
                  </span>
                </div>
                <span className="form-help">
                  顔検出が見つからない場合や不一致の場合、自動的にカメラを停止するまでの時間です。
                </span>
              </div>

              <div
                className="form-group"
                style={{
                  display: "flex",
                  justifyContent: "space-between",
                  alignItems: "center",
                  marginTop: "20px",
                  padding: "12px 0",
                  borderTop: "1px solid var(--border-color)",
                }}
              >
                <div>
                  <div style={{ fontWeight: 600 }}>
                    動体検知（Liveness Check）を有効にする
                  </div>
                  <span className="form-help">
                    フレーム間の差分からユーザーの動きを検知します。写真によるなりすましを防ぐことができますが、じっとしていると認証されにくくなります。家庭内利用で使い勝手を優先する場合はオフ（推奨）にしてください。
                  </span>
                </div>
                <label className="switch">
                  <input
                    type="checkbox"
                    checked={config.liveness_enabled}
                    onChange={async (e) => {
                      const nextVal = e.target.checked;
                      try {
                        await invoke("set_registry_status", {
                          enabled: config.enabled,
                          sid: config.sid,
                          threshold: config.threshold,
                          requiredMatches: config.required_matches,
                          windowSize: config.window_size,
                          scanTimeoutMs: config.scan_timeout_ms,
                          livenessEnabled: nextVal,
                          cameraIndex: config.camera_index,
                        });
                        setConfig((prev) => ({
                          ...prev,
                          liveness_enabled: nextVal,
                        }));
                        showStatus(
                          `動体検知を${nextVal ? "有効" : "無効"}にしました。`,
                          "success",
                        );
                      } catch (err: any) {
                        showStatus(`設定の変更に失敗しました: ${err}`, "error");
                      }
                    }}
                  />
                  <span className="slider-switch"></span>
                </label>
              </div>
            </div>

            <div
              style={{
                display: "flex",
                gap: "12px",
                justifyContent: "flex-end",
                marginTop: "24px",
              }}
            >
              <button className="btn btn-secondary" onClick={loadConfig}>
                リセット
              </button>
              <button
                className="btn btn-primary"
                onClick={handleSaveSettings}
                disabled={loading}
              >
                設定を保存して適用
              </button>
            </div>
          </div>
        )}

        {/* Tab 3: Diagnostics */}
        {activeTab === "diagnostics" && (
          <div>
            <div className="page-header">
              <h2 className="page-title">診断ツールとリアルタイムログ</h2>
              <p className="page-subtitle">
                登録された顔とカメラが正しく連動して動作しているかを検証します。
              </p>
            </div>

            <div className="card">
              <h3 className="card-title">動作テスト</h3>
              <p
                style={{
                  fontSize: "13px",
                  color: "var(--text-secondary)",
                  marginBottom: "16px",
                }}
              >
                実際に顔認識エンジンをシミュレーション起動し、登録済みの顔と正しく照合されるかをテストします。
                <br />
                ログの動きや類似度スコアを確認できます。
              </p>

              <div
                style={{ display: "flex", gap: "12px", marginBottom: "24px" }}
              >
                <button
                  className="btn btn-primary"
                  onClick={handleRunVerifyTest}
                  disabled={diagRunning}
                >
                  {diagRunning ? "照合テスト実行中..." : "顔照合テストの実行"}
                </button>
                <button
                  className="btn btn-secondary"
                  onClick={handleRunDiagnostics}
                  disabled={diagRunning}
                >
                  システム構成診断の実行
                </button>
              </div>

              {diagOutput && (
                <div style={{ marginBottom: "20px" }}>
                  <div className="form-label">診断出力</div>
                  <pre className="log-console" style={{ height: "150px" }}>
                    {diagOutput}
                  </pre>
                </div>
              )}
            </div>

            <div className="card">
              <h3 className="card-title">ログモニター</h3>
              <div
                className="form-group"
                style={{
                  display: "flex",
                  gap: "12px",
                  marginBottom: "16px",
                  alignItems: "center",
                }}
              >
                <label className="form-label" style={{ marginBottom: 0 }}>
                  ログファイル選択:
                </label>
                <select
                  className="form-input"
                  value={logType}
                  onChange={(e: any) => setLogType(e.target.value)}
                  style={{ maxWidth: "240px" }}
                >
                  <option value="FaceLogonHost.log">
                    FaceLogonHost.log (顔照合ホストのログ)
                  </option>
                  <option value="FaceLogonSetup.log">
                    FaceLogonSetup.log (セットアップ・テストのログ)
                  </option>
                </select>
                <button
                  className="btn btn-secondary btn-sm"
                  onClick={refreshLogs}
                >
                  更新
                </button>
              </div>

              <pre className="log-console">{logContent}</pre>
            </div>
          </div>
        )}
      </main>
    </div>
  );
}

export default App;
