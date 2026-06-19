use serde::Serialize;
use std::os::windows::process::CommandExt;
use std::path::PathBuf;
use winreg::enums::*;
use winreg::RegKey;

const CREATE_NO_WINDOW: u32 = 0x08000000;

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
}

fn get_enabled() -> bool {
    let hk = RegKey::predef(HKEY_LOCAL_MACHINE);
    if let Ok(key) = hk.open_subkey("SOFTWARE\\HomeFaceLogon") {
        if let Ok(val) = key.get_value::<u32, _>("Enabled") {
            return val != 0;
        }
    }
    false
}

fn set_enabled(enabled: bool) -> Result<(), String> {
    let hk = RegKey::predef(HKEY_LOCAL_MACHINE);
    let (key, _) = hk
        .create_subkey("SOFTWARE\\HomeFaceLogon")
        .map_err(|e| e.to_string())?;
    let val: u32 = if enabled { 1 } else { 0 };
    key.set_value("Enabled", &val).map_err(|e| e.to_string())?;
    Ok(())
}

fn get_config_path() -> Result<PathBuf, String> {
    let program_data =
        std::env::var("ProgramData").map_err(|_| "ProgramData env var not found".to_string())?;
    let mut path = PathBuf::from(program_data);
    path.push("HomeFaceLogon");
    path.push("config.json");
    Ok(path)
}

fn find_setup_exe() -> Result<PathBuf, String> {
    // 1. Try standard installation path
    let path = PathBuf::from(r"C:\Program Files\HomeFaceLogon\FaceLogonSetup.exe");
    if path.exists() {
        return Ok(path);
    }

    // 2. Try development relative paths
    let paths = vec![
        r"..\x64\Debug\FaceLogonSetup.exe",
        r"..\x64\Release\FaceLogonSetup.exe",
        r"..\setup\x64\Debug\FaceLogonSetup.exe",
        r"..\setup\x64\Release\FaceLogonSetup.exe",
        r"..\..\x64\Debug\FaceLogonSetup.exe",
        r"..\..\x64\Release\FaceLogonSetup.exe",
        r"..\..\..\x64\Debug\FaceLogonSetup.exe",
        r"..\..\..\x64\Release\FaceLogonSetup.exe",
        r"..\..\..\..\x64\Debug\FaceLogonSetup.exe",
        r"..\..\..\..\x64\Release\FaceLogonSetup.exe",
    ];

    for p in paths {
        let pb = PathBuf::from(p);
        if pb.exists() {
            if let Ok(abs) = std::fs::canonicalize(pb) {
                return Ok(abs);
            }
        }
    }

    Err("FaceLogonSetup.exe not found. Please build the C++ solution in Release or Debug configuration first.".to_string())
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

    if let Ok(config_path) = get_config_path() {
        if config_path.exists() {
            if let Ok(content) = std::fs::read_to_string(config_path) {
                if let Ok(val) = serde_json::from_str::<serde_json::Value>(&content) {
                    if let Some(s) = val["targetSid"].as_str() {
                        sid = s.to_string();
                    }
                    if let Some(t) = val["matchThreshold"].as_f64() {
                        threshold = t;
                    }
                    if let Some(rm) = val["requiredMatches"].as_i64() {
                        required_matches = rm as i32;
                    }
                    if let Some(ws) = val["windowSize"].as_i64() {
                        window_size = ws as i32;
                    }
                    if let Some(st) = val["scanTimeoutMs"].as_i64() {
                        scan_timeout_ms = st as i32;
                    }
                    if let Some(le) = val["livenessEnabled"].as_bool() {
                        liveness_enabled = le;
                    }
                    if let Some(ci) = val["cameraIndex"].as_i64() {
                        camera_index = ci as i32;
                    }
                }
            }
        }
    }

    Ok(RegistryStatus {
        enabled,
        sid,
        threshold,
        required_matches,
        window_size,
        scan_timeout_ms,
        liveness_enabled,
        camera_index,
    })
}

#[tauri::command]
fn set_registry_status(
    enabled: bool,
    sid: String,
    threshold: f64,
    required_matches: i32,
    window_size: i32,
    scan_timeout_ms: i32,
    liveness_enabled: bool,
    camera_index: i32,
) -> Result<(), String> {
    set_enabled(enabled)?;

    let config_path = get_config_path()?;
    if let Some(parent) = config_path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }

    let config_val = serde_json::json!({
        "schemaVersion": 1,
        "enabled": enabled,
        "targetSid": sid,
        "matchThreshold": threshold,
        "requiredMatches": required_matches,
        "windowSize": window_size,
        "scanTimeoutMs": scan_timeout_ms,
        "livenessEnabled": liveness_enabled,
        "cameraIndex": camera_index,
        "hostStartupTimeoutMs": 3000,
        "modelVersion": "1.0"
    });

    let content = serde_json::to_string_pretty(&config_val).map_err(|e| e.to_string())?;
    std::fs::write(config_path, content).map_err(|e| e.to_string())?;

    Ok(())
}

#[tauri::command]
fn get_user_sid() -> Result<String, String> {
    let output = std::process::Command::new("powershell")
        .args(&[
            "-NoProfile",
            "-Command",
            "[System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value",
        ])
        .creation_flags(CREATE_NO_WINDOW)
        .output()
        .map_err(|e| format!("Failed to run PowerShell to get SID: {}", e))?;

    if output.status.success() {
        let sid = String::from_utf8_lossy(&output.stdout).trim().to_string();
        if !sid.is_empty() {
            return Ok(sid);
        }
    }

    let output = std::process::Command::new("whoami")
        .args(&["/user", "/fo", "csv"])
        .creation_flags(CREATE_NO_WINDOW)
        .output()
        .map_err(|e| format!("Failed to run whoami: {}", e))?;

    let out_str = String::from_utf8_lossy(&output.stdout);
    let lines: Vec<&str> = out_str.lines().collect();
    if lines.len() >= 2 {
        let fields: Vec<&str> = lines[1].split(',').collect();
        if fields.len() >= 2 {
            let sid = fields[1].trim_matches('"').trim().to_string();
            return Ok(sid);
        }
    }

    Err("Could not retrieve current user's SID.".to_string())
}

#[tauri::command]
fn run_setup_binary(sid: String, password: String, pin: String) -> Result<String, String> {
    use std::io::Write;
    use std::process::Stdio;

    let setup_exe = find_setup_exe()?;
    let mut child = std::process::Command::new(&setup_exe)
        .arg("--sid")
        .arg(&sid)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .creation_flags(CREATE_NO_WINDOW)
        .spawn()
        .map_err(|e| format!("Failed to start setup process: {}", e))?;

    {
        let stdin = child.stdin.as_mut().ok_or("Failed to open stdin")?;
        writeln!(stdin, "{}", password).map_err(|e| e.to_string())?;
        writeln!(stdin, "{}", pin).map_err(|e| e.to_string())?;
    }

    let output = child
        .wait_with_output()
        .map_err(|e| format!("Failed to wait for setup process: {}", e))?;

    if output.status.success() {
        Ok(String::from_utf8_lossy(&output.stdout).into_owned())
    } else {
        let err_msg = String::from_utf8_lossy(&output.stderr).into_owned();
        if err_msg.is_empty() {
            Err(String::from_utf8_lossy(&output.stdout).into_owned())
        } else {
            Err(err_msg)
        }
    }
}

#[tauri::command]
fn run_enroll(camera_index: i32) -> Result<String, String> {
    let setup_exe = find_setup_exe()?;
    let output = std::process::Command::new(&setup_exe)
        .arg("--enroll")
        .arg("--camera")
        .arg(camera_index.to_string())
        .creation_flags(CREATE_NO_WINDOW)
        .output()
        .map_err(|e| format!("Failed to start enrollment: {}", e))?;

    if output.status.success() {
        Ok(String::from_utf8_lossy(&output.stdout).into_owned())
    } else {
        Err(String::from_utf8_lossy(&output.stderr).into_owned())
    }
}

#[tauri::command]
fn run_verify(camera_index: i32) -> Result<String, String> {
    let setup_exe = find_setup_exe()?;
    let output = std::process::Command::new(&setup_exe)
        .arg("--verify")
        .arg("--camera")
        .arg(camera_index.to_string())
        .creation_flags(CREATE_NO_WINDOW)
        .output()
        .map_err(|e| format!("Failed to start verify: {}", e))?;

    if output.status.success() {
        Ok(String::from_utf8_lossy(&output.stdout).into_owned())
    } else {
        Err(String::from_utf8_lossy(&output.stderr).into_owned())
    }
}

#[tauri::command]
fn run_test() -> Result<String, String> {
    let setup_exe = find_setup_exe()?;
    let output = std::process::Command::new(&setup_exe)
        .arg("--test")
        .creation_flags(CREATE_NO_WINDOW)
        .output()
        .map_err(|e| format!("Failed to start test: {}", e))?;

    if output.status.success() {
        Ok(String::from_utf8_lossy(&output.stdout).into_owned())
    } else {
        Err(String::from_utf8_lossy(&output.stderr).into_owned())
    }
}

#[tauri::command]
fn read_log_file(name: String) -> Result<String, String> {
    // Allowlist filenames to prevent path traversal
    if name != "FaceLogonHost.log" && name != "FaceLogonSetup.log" && name != "FaceLogon.log" {
        return Err("Access denied: invalid log file name".to_string());
    }
    let program_data =
        std::env::var("ProgramData").map_err(|_| "ProgramData env var not found".to_string())?;
    let mut path = PathBuf::from(program_data);
    path.push("HomeFaceLogon");
    path.push("logs");
    path.push(name);

    if !path.exists() {
        return Ok("No log content found yet.".to_string());
    }

    let content = std::fs::read_to_string(path).map_err(|e| e.to_string())?;
    let lines: Vec<&str> = content.lines().collect();
    let start = if lines.len() > 100 {
        lines.len() - 100
    } else {
        0
    };
    Ok(lines[start..].join("\n"))
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .setup(|app| {
            if cfg!(debug_assertions) {
                app.handle().plugin(
                    tauri_plugin_log::Builder::default()
                        .level(log::LevelFilter::Info)
                        .build(),
                )?;
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            get_registry_status,
            set_registry_status,
            get_user_sid,
            run_setup_binary,
            run_enroll,
            run_verify,
            run_test,
            read_log_file
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
