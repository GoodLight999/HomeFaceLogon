use serde::Serialize;
use std::io::{Read, Write};
use std::os::windows::process::CommandExt;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};
use winreg::enums::*;
use winreg::RegKey;

const CREATE_NO_WINDOW: u32 = 0x08000000;
mod setup_transaction;
use setup_transaction::{atomic_write, transactional_setup, SETUP_FILES};
static SETUP_OPERATION: std::sync::Mutex<()> = std::sync::Mutex::new(());

// Tauri synchronous commands run on the UI thread. Keep helper waits off that
// thread, while preventing two camera operations or setup writes from racing.
async fn background_operation<T: Send + 'static>(
    operation: impl FnOnce() -> Result<T, String> + Send + 'static,
) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let _guard = SETUP_OPERATION.try_lock()
            .map_err(|_| "別の処理が実行中です。完了してから再試行してください。".to_string())?;
        operation()
    }).await.map_err(|e| e.to_string())?
}

#[derive(Serialize)]
pub struct RegistryStatus {
    pub enabled: bool,
    pub sid: String,
    pub threshold: f64,
    pub required_matches: i32,
    pub window_size: i32,
    pub scan_timeout_ms: i32,
    pub liveness_enabled: bool,
    pub camera_index: i32,
    pub setup_complete: bool,
}

#[derive(Serialize)]
pub struct CameraDevice {
    pub index: i32,
    pub name: String,
}

fn get_enabled() -> bool {
    let hk = RegKey::predef(HKEY_LOCAL_MACHINE);
    hk.open_subkey("SOFTWARE\\HomeFaceLogon")
        .and_then(|key: RegKey| key.get_value::<u32, _>("Enabled"))
        .map(|value| value != 0)
        .unwrap_or(false)
}

fn set_enabled(enabled: bool) -> Result<(), String> {
    let hk = RegKey::predef(HKEY_LOCAL_MACHINE);
    let (key, _) = hk.create_subkey("SOFTWARE\\HomeFaceLogon").map_err(|e| e.to_string())?;
    key.set_value("Enabled", &(if enabled { 1u32 } else { 0u32 }))
        .map_err(|e| e.to_string())
}

fn data_dir() -> Result<PathBuf, String> {
    Ok(PathBuf::from(std::env::var("ProgramData")
        .map_err(|_| "ProgramData env var not found".to_string())?)
        .join("HomeFaceLogon"))
}

fn config_path() -> Result<PathBuf, String> {
    Ok(data_dir()?.join("config.json"))
}

fn find_setup_exe() -> Result<PathBuf, String> {
    if let Ok(executable) = std::env::current_exe() {
        if let Some(parent) = executable.parent() {
            let adjacent = parent.join("FaceLogonSetup.exe");
            if adjacent.is_file() { return Ok(adjacent); }
            // Development builds must use their own helper, not an older install.
            for ancestor in parent.ancestors() {
                if ancestor.join("HomeFaceLogon.sln").is_file() {
                    for configuration in ["Release", "Debug"] {
                        let candidate = ancestor.join("x64").join(configuration).join("FaceLogonSetup.exe");
                        if candidate.is_file() { return Ok(candidate); }
                    }
                    break;
                }
            }
        }
    }
    let installed = PathBuf::from(r"C:\Program Files\HomeFaceLogon\FaceLogonSetup.exe");
    if installed.exists() { return Ok(installed); }
    for path in [
        r"..\x64\Debug\FaceLogonSetup.exe", r"..\x64\Release\FaceLogonSetup.exe",
        r"..\setup\x64\Debug\FaceLogonSetup.exe", r"..\setup\x64\Release\FaceLogonSetup.exe",
        r"..\..\x64\Debug\FaceLogonSetup.exe", r"..\..\x64\Release\FaceLogonSetup.exe",
        r"..\..\..\x64\Debug\FaceLogonSetup.exe", r"..\..\..\x64\Release\FaceLogonSetup.exe",
        r"..\..\..\..\x64\Debug\FaceLogonSetup.exe", r"..\..\..\..\x64\Release\FaceLogonSetup.exe",
    ] {
        let candidate = PathBuf::from(path);
        if candidate.exists() {
            if let Ok(abs) = std::fs::canonicalize(candidate) { return Ok(abs); }
        }
    }
    Err("FaceLogonSetup.exe not found. Build Release x64 first.".to_string())
}

fn run_setup_process(args: &[String], stdin_content: Option<&str>, timeout: Duration) -> Result<String, String> {
    let setup_exe = find_setup_exe()?;
    if args.iter().any(|arg| arg == "--data-dir") {
        let mut probe = Command::new(&setup_exe);
        probe.arg("--capabilities");
        let output = run_process(probe, None, Duration::from_secs(15))
            .map_err(|_| "FaceLogonSetup.exeが古いため、安全に登録できません。管理ツールと設定ツールを一緒に更新してください。".to_string())?;
        validate_helper_capabilities(&output)?;
    }
    let mut command = Command::new(&setup_exe);
    command.args(args);
    run_process(command, stdin_content, timeout)
}

fn validate_helper_capabilities(output: &str) -> Result<(), String> {
    let capabilities: serde_json::Value = serde_json::from_str(output.trim()).map_err(|e| e.to_string())?;
    if capabilities["protocolVersion"] != 1 || capabilities["isolatedDataDirectory"] != true {
        return Err("設定ツールが作業用ディレクトリでの登録に対応していません。".into());
    }
    Ok(())
}

fn run_process(mut command: Command, stdin_content: Option<&str>, timeout: Duration) -> Result<String, String> {
    let started = Instant::now();
    let mut child = command
        .stdin(if stdin_content.is_some() { Stdio::piped() } else { Stdio::null() })
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .creation_flags(CREATE_NO_WINDOW)
        .spawn()
        .map_err(|e| format!("Failed to start helper: {}", e))?;

    let stdout_reader = child.stdout.take().map(|mut stream| thread::spawn(move || {
        let mut bytes = Vec::new(); let _ = stream.read_to_end(&mut bytes); bytes
    }));
    let stderr_reader = child.stderr.take().map(|mut stream| thread::spawn(move || {
        let mut bytes = Vec::new(); let _ = stream.read_to_end(&mut bytes); bytes
    }));

    // A stuck helper may never read stdin. Do not block the timeout supervisor
    // while writing credentials into a full pipe.
    let stdin_writer = if let Some(content) = stdin_content {
        let mut stdin = child.stdin.take().ok_or("Failed to open setup stdin")?;
        let mut bytes = content.as_bytes().to_vec();
        Some(thread::spawn(move || {
            let result = stdin.write_all(&bytes);
            bytes.fill(0);
            result
        }))
    } else { None };

    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status,
            Ok(None) if started.elapsed() < timeout => thread::sleep(Duration::from_millis(100)),
            Ok(None) => {
                let _ = child.kill(); let _ = child.wait();
                if let Some(writer) = stdin_writer { let _ = writer.join(); }
                let stdout = stdout_reader.and_then(|r| r.join().ok()).unwrap_or_default();
                let stderr = stderr_reader.and_then(|r| r.join().ok()).unwrap_or_default();
                return Err(format!("Camera helper timed out after {} seconds and was terminated.\n{}{}",
                    timeout.as_secs(), String::from_utf8_lossy(&stdout), String::from_utf8_lossy(&stderr)));
            }
            Err(e) => {
                let _ = child.kill(); let _ = child.wait();
                if let Some(writer) = stdin_writer { let _ = writer.join(); }
                return Err(e.to_string());
            }
        }
    };

    let input_result = match stdin_writer {
        Some(writer) => writer.join().map_err(|_| "Input writer failed".to_string())?
            .map_err(|e| format!("Failed to send credentials: {}", e)),
        None => Ok(()),
    };
    let stdout = stdout_reader.and_then(|r| r.join().ok()).unwrap_or_default();
    let stderr = stderr_reader.and_then(|r| r.join().ok()).unwrap_or_default();
    let out = String::from_utf8_lossy(&stdout).into_owned();
    let err = String::from_utf8_lossy(&stderr).into_owned();
    if status.success() { input_result?; Ok(out) } else if err.trim().is_empty() { Err(out) } else { Err(err) }
}

fn setup_files_complete() -> bool {
    let Ok(dir) = data_dir() else { return false; };
    SETUP_FILES.iter().all(|name| std::fs::metadata(dir.join(name))
        .map(|m| m.is_file() && m.len() > 0).unwrap_or(false))
}

#[tauri::command]
fn get_registry_status() -> Result<RegistryStatus, String> {
    let enabled = get_enabled();
    let mut sid = String::new();
    let mut threshold = 0.363;
    let mut required_matches = 3;
    let mut window_size = 5;
    let mut scan_timeout_ms = 10000;
    let mut liveness_enabled = false;
    let mut camera_index = 0;
    if let Ok(path) = config_path() {
        if let Ok(content) = std::fs::read_to_string(path) {
            if let Ok(v) = serde_json::from_str::<serde_json::Value>(&content) {
                sid = v["targetSid"].as_str().unwrap_or_default().to_string();
                threshold = v["matchThreshold"].as_f64().unwrap_or(threshold);
                required_matches = v["requiredMatches"].as_i64().unwrap_or(3) as i32;
                window_size = v["windowSize"].as_i64().unwrap_or(5) as i32;
                scan_timeout_ms = v["scanTimeoutMs"].as_i64().unwrap_or(10000) as i32;
                liveness_enabled = v["livenessEnabled"].as_bool().unwrap_or(false);
                camera_index = v["cameraIndex"].as_i64().unwrap_or(0) as i32;
            }
        }
    }
    let setup_complete = !sid.is_empty() && setup_files_complete();
    Ok(RegistryStatus { enabled, sid, threshold, required_matches, window_size,
        scan_timeout_ms, liveness_enabled, camera_index, setup_complete })
}

#[tauri::command]
fn set_registry_status(enabled: bool, sid: String, threshold: f64, required_matches: i32,
    window_size: i32, scan_timeout_ms: i32, liveness_enabled: bool, camera_index: i32) -> Result<(), String> {
    let _guard = SETUP_OPERATION.try_lock()
        .map_err(|_| "別の処理が実行中です。完了してから設定を保存してください。".to_string())?;
    if enabled && (!setup_files_complete() || sid.trim().is_empty()) {
        return Err("セットアップが不完全です。資格情報・PIN・顔登録を完了してください。".to_string());
    }
    let path = config_path()?;
    if let Some(parent) = path.parent() { std::fs::create_dir_all(parent).map_err(|e| e.to_string())?; }
    let previous = match std::fs::read(&path) {
        Ok(bytes) => Some(bytes),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
        Err(e) => return Err(e.to_string()),
    };
    let mut value = match &previous {
        Some(bytes) => serde_json::from_slice::<serde_json::Value>(bytes).map_err(|e| e.to_string())?,
        None => serde_json::json!({ "schemaVersion": 1, "hostStartupTimeoutMs": 65000, "modelVersion": "1.0" }),
    };
    if !value.is_object() { return Err("Config must be an object".into()); }
    let updates = serde_json::json!({
        "enabled": enabled, "targetSid": sid, "matchThreshold": threshold,
        "requiredMatches": required_matches, "windowSize": window_size,
        "scanTimeoutMs": scan_timeout_ms, "livenessEnabled": liveness_enabled, "cameraIndex": camera_index,
    });
    value.as_object_mut().unwrap().extend(updates.as_object().unwrap().clone());
    atomic_write(&path, &serde_json::to_vec_pretty(&value).map_err(|e| e.to_string())?)?;
    if let Err(error) = set_enabled(enabled) {
        let restore = match previous {
            Some(bytes) => atomic_write(&path, &bytes),
            None => std::fs::remove_file(&path).map_err(|e| e.to_string()),
        };
        restore.map_err(|e| format!("{}; failed to restore config: {}", error, e))?;
        return Err(error);
    }
    Ok(())
}

#[tauri::command]
async fn get_user_sid() -> Result<String, String> {
    background_operation(get_user_sid_blocking).await
}

fn get_user_sid_blocking() -> Result<String, String> {
    let output = Command::new("powershell").args(["-NoProfile", "-Command",
        "[System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value"])
        .creation_flags(CREATE_NO_WINDOW).output().map_err(|e| e.to_string())?;
    let sid = String::from_utf8_lossy(&output.stdout).trim().to_string();
    if !sid.is_empty() { return Ok(sid); }
    Err("Could not retrieve current user's SID.".to_string())
}

#[tauri::command]
async fn list_cameras() -> Result<Vec<CameraDevice>, String> {
    background_operation(list_cameras_blocking).await
}

fn list_cameras_blocking() -> Result<Vec<CameraDevice>, String> {
    let output = run_setup_process(&["--list-cameras".to_string()], None, Duration::from_secs(15))?;
    let devices: Vec<CameraDevice> = output.lines().filter_map(|line| {
        let (index, name) = line.split_once('\t')?;
        Some(CameraDevice { index: index.trim().parse().ok()?, name: name.trim().to_string() })
    }).collect();
    if devices.is_empty() { Err("利用可能なカメラが見つかりません。".to_string()) } else { Ok(devices) }
}

#[tauri::command]
async fn run_complete_setup(sid: String, password: String, pin: String, camera_index: i32) -> Result<String, String> {
    background_operation(move || complete_setup(sid, password, pin, camera_index)).await
}

fn validate_setup_input(sid: &str, password: &str, pin: &str, camera_index: i32) -> Result<(), String> {
    if sid.trim().is_empty() || password.is_empty() || pin.is_empty() {
        return Err("SID、パスワード、PINは必須です。".to_string());
    }
    if password.contains(['\r', '\n', '\0']) {
        return Err("パスワードに改行またはNUL文字を含めることはできません。".to_string());
    }
    if !(4..=16).contains(&pin.len()) || !pin.bytes().all(|c| (33..=126).contains(&c)) {
        return Err("PINは4〜16文字の半角英数字・記号（スペース除く）で入力してください。".to_string());
    }
    if !sid.starts_with("S-1-") || !sid.bytes().all(|c| c.is_ascii_digit() || c == b'S' || c == b'-') || camera_index < 0 {
        return Err("SIDまたはカメラ番号が無効です。".to_string());
    }
    Ok(())
}

fn complete_setup(sid: String, password: String, pin: String, camera_index: i32) -> Result<String, String> {
    validate_setup_input(&sid, &password, &pin, camera_index)?;
    let dir = data_dir()?;
    let previous_config = match std::fs::read(dir.join("config.json")) {
        Ok(bytes) => Some(serde_json::from_slice::<serde_json::Value>(&bytes).map_err(|e| e.to_string())?),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
        Err(e) => return Err(e.to_string()),
    };
    let enabled = get_enabled();
    transactional_setup(&dir, enabled, set_enabled, |candidate| {
        let directory = candidate.to_str().ok_or("Invalid setup directory")?.to_string();
        let common = vec!["--data-dir".to_string(), directory, "--camera".into(), camera_index.to_string()];
        let run = |mode: &str, timeout: u64| {
            let mut args = common.clone(); args.push(mode.to_string());
            run_setup_process(&args, None, Duration::from_secs(timeout))
        };
        run("--probe-camera", 30)?;
        run("--enroll", 180)?;
        let mut args = common; args.extend(["--sid".into(), sid.clone()]);
        let output = run_setup_process(&args, Some(&format!("{}\n{}\n", password, pin)), Duration::from_secs(30))?;
        // Re-registration changes credentials, SID and device. Preserve other settings.
        let path = candidate.join("config.json");
        let mut config = match previous_config {
            Some(value) if value.is_object() => value,
            Some(_) => return Err("Existing config is not an object".into()),
            None => serde_json::from_slice(&std::fs::read(&path).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?,
        };
        config["targetSid"] = serde_json::json!(sid);
        config["cameraIndex"] = serde_json::json!(camera_index);
        config["enabled"] = serde_json::json!(enabled);
        atomic_write(&path, &serde_json::to_vec_pretty(&config).map_err(|e| e.to_string())?)?;
        Ok(output)
    })
}

#[tauri::command]
async fn run_enroll(camera_index: i32) -> Result<String, String> {
    background_operation(move || {
        if !setup_files_complete() { return Err("資格情報とPINを含む初期セットアップを先に完了してください。".into()); }
        let dir = data_dir()?;
        let snapshot = setup_transaction::snapshot_files(&dir)?;
        transactional_setup(&dir, get_enabled(), set_enabled, |candidate| {
            for item in &snapshot {
                if let Some(bytes) = &item.contents {
                    atomic_write(&candidate.join(item.path.file_name().unwrap()), bytes)?;
                }
            }
            run_setup_process(&["--enroll".into(), "--camera".into(), camera_index.to_string(),
                "--data-dir".into(), candidate.to_str().ok_or("Invalid setup directory")?.into()],
                None, Duration::from_secs(180))
        })
    }).await
}
#[tauri::command]
async fn run_verify(camera_index: i32) -> Result<String, String> {
    background_operation(move || run_setup_process(&["--verify".into(), "--camera".into(), camera_index.to_string()], None, Duration::from_secs(90))).await
}
#[tauri::command]
async fn run_test() -> Result<String, String> {
    background_operation(|| run_setup_process(&["--test".into()], None, Duration::from_secs(60))).await
}
#[tauri::command]
fn read_log_file(name: String) -> Result<String, String> {
    if name != "FaceLogonHost.log" && name != "FaceLogonSetup.log" && name != "FaceLogon.log" {
        return Err("Access denied: invalid log file name".to_string());
    }
    let path = data_dir()?.join("logs").join(name);
    if !path.exists() { return Ok("No log content found yet.".to_string()); }
    let content = std::fs::read_to_string(path).map_err(|e| e.to_string())?;
    let lines: Vec<&str> = content.lines().collect();
    Ok(lines[lines.len().saturating_sub(100)..].join("\n"))
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .setup(|app| {
            if cfg!(debug_assertions) {
                app.handle().plugin(tauri_plugin_log::Builder::default()
                    .level(log::LevelFilter::Info).build())?;
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![get_registry_status, set_registry_status,
            get_user_sid, list_cameras, run_complete_setup, run_enroll, run_verify,
            run_test, read_log_file])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

#[cfg(test)]
mod tests {
    use super::*;

    fn powershell(script: &str) -> Command {
        let mut command = Command::new("powershell.exe");
        command.args(["-NoProfile", "-NonInteractive", "-Command", script]);
        command
    }

    #[test]
    fn a_stuck_helper_is_terminated_and_the_next_operation_runs() {
        let started = Instant::now();
        let error = run_process(powershell("[Threading.Thread]::Sleep(20000)"),
            None, Duration::from_millis(300)).unwrap_err();
        assert!(error.contains("timed out"), "{error}");
        assert!(started.elapsed() < Duration::from_secs(10));
        let output = run_process(powershell("Write-Output 'recovered'"),
            None, Duration::from_secs(15)).unwrap();
        assert!(output.contains("recovered"));
    }

    #[test]
    fn both_output_pipes_are_drained_without_deadlock() {
        let output = run_process(powershell(
            "[Console]::Out.Write(('o' * 100000)); [Console]::Error.Write(('e' * 100000))"),
            None, Duration::from_secs(15)).unwrap();
        assert_eq!(output.len(), 100000);
    }

    #[test]
    fn stdin_is_closed_and_failure_reports_stderr() {
        let output = run_process(powershell("[Console]::Out.Write([Console]::In.ReadToEnd())"),
            Some("test input\n"), Duration::from_secs(15)).unwrap();
        assert_eq!(output, "test input\n");
        let error = run_process(powershell("[Console]::Error.Write('camera unavailable'); exit 1"),
            None, Duration::from_secs(15)).unwrap_err();
        assert_eq!(error, "camera unavailable");
    }

    #[test]
    fn timeout_still_runs_when_helper_does_not_read_stdin() {
        let input = "x".repeat(100000);
        let started = Instant::now();
        let error = run_process(powershell("[Threading.Thread]::Sleep(20000)"),
            Some(&input), Duration::from_millis(300)).unwrap_err();
        assert!(error.contains("timed out"), "{error}");
        assert!(started.elapsed() < Duration::from_secs(10));
    }

    #[test]
    fn setup_input_accepts_unicode_but_rejects_protocol_breaking_passwords() {
        assert!(validate_setup_input("S-1-5-21-123", "テスト🔑", "1234", 0).is_ok());
        for password in ["a\nb", "a\rb", "a\0b"] {
            assert!(validate_setup_input("S-1-5-21-123", password, "1234", 0).is_err());
        }
        assert!(validate_setup_input("S-1-5-21-123", "dummy", "１２３４", 0).is_err());
    }

    #[test]
    fn old_or_incompatible_helper_cannot_run_isolated_enrollment() {
        assert!(validate_helper_capabilities(r#"{"protocolVersion":1,"isolatedDataDirectory":true}"#).is_ok());
        for output in ["old helper prompts", "{}", r#"{"protocolVersion":0,"isolatedDataDirectory":true}"#,
            r#"{"protocolVersion":1,"isolatedDataDirectory":false}"#] {
            assert!(validate_helper_capabilities(output).is_err());
        }
    }
}
