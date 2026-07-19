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
const SETUP_FILES: [&str; 4] = ["secret.bin", "pin.bin", "config.json", "face.bin"];

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

#[derive(Clone)]
struct FileSnapshot {
    path: PathBuf,
    contents: Option<Vec<u8>>,
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
    let mut child = Command::new(&setup_exe)
        .args(args)
        .stdin(if stdin_content.is_some() { Stdio::piped() } else { Stdio::null() })
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .creation_flags(CREATE_NO_WINDOW)
        .spawn()
        .map_err(|e| format!("Failed to start {}: {}", setup_exe.display(), e))?;

    let stdout_reader = child.stdout.take().map(|mut stream| thread::spawn(move || {
        let mut bytes = Vec::new(); let _ = stream.read_to_end(&mut bytes); bytes
    }));
    let stderr_reader = child.stderr.take().map(|mut stream| thread::spawn(move || {
        let mut bytes = Vec::new(); let _ = stream.read_to_end(&mut bytes); bytes
    }));

    if let Some(content) = stdin_content {
        let mut stdin = child.stdin.take().ok_or("Failed to open setup stdin")?;
        stdin.write_all(content.as_bytes()).map_err(|e| e.to_string())?;
    }

    let started = Instant::now();
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status,
            Ok(None) if started.elapsed() < timeout => thread::sleep(Duration::from_millis(100)),
            Ok(None) => {
                let _ = child.kill(); let _ = child.wait();
                let stdout = stdout_reader.and_then(|r| r.join().ok()).unwrap_or_default();
                let stderr = stderr_reader.and_then(|r| r.join().ok()).unwrap_or_default();
                return Err(format!("Camera helper timed out after {} seconds and was terminated.\n{}{}",
                    timeout.as_secs(), String::from_utf8_lossy(&stdout), String::from_utf8_lossy(&stderr)));
            }
            Err(e) => { let _ = child.kill(); let _ = child.wait(); return Err(e.to_string()); }
        }
    };

    let stdout = stdout_reader.and_then(|r| r.join().ok()).unwrap_or_default();
    let stderr = stderr_reader.and_then(|r| r.join().ok()).unwrap_or_default();
    let out = String::from_utf8_lossy(&stdout).into_owned();
    let err = String::from_utf8_lossy(&stderr).into_owned();
    if status.success() { Ok(out) } else if err.trim().is_empty() { Err(out) } else { Err(err) }
}

fn snapshot_files() -> Result<Vec<FileSnapshot>, String> {
    let dir = data_dir()?;
    SETUP_FILES.iter().map(|name| {
        let path = dir.join(name);
        let contents = if path.exists() { Some(std::fs::read(&path).map_err(|e| e.to_string())?) } else { None };
        Ok(FileSnapshot { path, contents })
    }).collect()
}

fn restore_files(snapshot: &[FileSnapshot]) -> Result<(), String> {
    for item in snapshot {
        match &item.contents {
            Some(contents) => {
                if let Some(parent) = item.path.parent() { std::fs::create_dir_all(parent).map_err(|e| e.to_string())?; }
                std::fs::write(&item.path, contents).map_err(|e| e.to_string())?;
            }
            None => { if item.path.exists() { std::fs::remove_file(&item.path).map_err(|e| e.to_string())?; } }
        }
    }
    Ok(())
}

fn setup_files_complete() -> bool {
    let Ok(dir) = data_dir() else { return false; };
    SETUP_FILES.iter().all(|name| std::fs::metadata(dir.join(name))
        .map(|m| m.is_file() && m.len() > 0).unwrap_or(false))
}

#[tauri::command]
fn get_registry_status() -> Result<RegistryStatus, String> {
    let mut enabled = get_enabled();
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
    if enabled && !setup_complete { let _ = set_enabled(false); enabled = false; }
    Ok(RegistryStatus { enabled, sid, threshold, required_matches, window_size,
        scan_timeout_ms, liveness_enabled, camera_index, setup_complete })
}

#[tauri::command]
fn set_registry_status(enabled: bool, sid: String, threshold: f64, required_matches: i32,
    window_size: i32, scan_timeout_ms: i32, liveness_enabled: bool, camera_index: i32) -> Result<(), String> {
    if enabled && (!setup_files_complete() || sid.trim().is_empty()) {
        return Err("セットアップが不完全です。資格情報・PIN・顔登録を完了してください。".to_string());
    }
    let path = config_path()?;
    if let Some(parent) = path.parent() { std::fs::create_dir_all(parent).map_err(|e| e.to_string())?; }
    let value = serde_json::json!({
        "schemaVersion": 1, "enabled": enabled, "targetSid": sid,
        "matchThreshold": threshold, "requiredMatches": required_matches,
        "windowSize": window_size, "scanTimeoutMs": scan_timeout_ms,
        "livenessEnabled": liveness_enabled, "cameraIndex": camera_index,
        "hostStartupTimeoutMs": 65000, "modelVersion": "1.0"
    });
    std::fs::write(path, serde_json::to_string_pretty(&value).map_err(|e| e.to_string())?)
        .map_err(|e| e.to_string())?;
    set_enabled(enabled)
}

#[tauri::command]
fn get_user_sid() -> Result<String, String> {
    let output = Command::new("powershell").args(["-NoProfile", "-Command",
        "[System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value"])
        .creation_flags(CREATE_NO_WINDOW).output().map_err(|e| e.to_string())?;
    let sid = String::from_utf8_lossy(&output.stdout).trim().to_string();
    if !sid.is_empty() { return Ok(sid); }
    Err("Could not retrieve current user's SID.".to_string())
}

#[tauri::command]
fn list_cameras() -> Result<Vec<CameraDevice>, String> {
    let output = run_setup_process(&["--list-cameras".to_string()], None, Duration::from_secs(15))?;
    let devices: Vec<CameraDevice> = output.lines().filter_map(|line| {
        let (index, name) = line.split_once('\t')?;
        Some(CameraDevice { index: index.trim().parse().ok()?, name: name.trim().to_string() })
    }).collect();
    if devices.is_empty() { Err("利用可能なカメラが見つかりません。".to_string()) } else { Ok(devices) }
}

#[tauri::command]
fn run_complete_setup(sid: String, password: String, pin: String, camera_index: i32) -> Result<String, String> {
    if sid.trim().is_empty() || password.is_empty() || pin.is_empty() {
        return Err("SID、パスワード、PINは必須です。".to_string());
    }
    let snapshot = snapshot_files()?;
    let result = (|| {
        run_setup_process(&["--probe-camera".into(), "--camera".into(), camera_index.to_string()], None, Duration::from_secs(30))?;
        run_setup_process(&["--enroll".into(), "--camera".into(), camera_index.to_string()], None, Duration::from_secs(180))?;
        run_setup_process(&["--sid".into(), sid, "--camera".into(), camera_index.to_string()],
            Some(&format!("{}\n{}\n", password, pin)), Duration::from_secs(30))
    })();
    match result {
        Ok(output) => Ok(output),
        Err(error) => {
            let rollback = restore_files(&snapshot);
            let _ = set_enabled(false);
            match rollback {
                Ok(()) => Err(format!("{}\n変更は元の状態へ戻しました。", error)),
                Err(e) => Err(format!("{}\nロールバックにも失敗しました: {}", error, e)),
            }
        }
    }
}

#[tauri::command]
fn run_enroll(camera_index: i32) -> Result<String, String> {
    run_setup_process(&["--enroll".into(), "--camera".into(), camera_index.to_string()], None, Duration::from_secs(180))
}
#[tauri::command]
fn run_verify(camera_index: i32) -> Result<String, String> {
    run_setup_process(&["--verify".into(), "--camera".into(), camera_index.to_string()], None, Duration::from_secs(90))
}
#[tauri::command]
fn run_test() -> Result<String, String> {
    run_setup_process(&["--test".into()], None, Duration::from_secs(60))
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
