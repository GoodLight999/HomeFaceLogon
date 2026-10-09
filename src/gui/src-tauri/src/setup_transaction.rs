use std::fs;
use std::io::Write;
use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

pub const SETUP_FILES: [&str; 4] = ["secret.bin", "pin.bin", "face.bin", "config.json"];
static SEQUENCE: AtomicU64 = AtomicU64::new(0);

fn suffix() -> String {
    format!("{}-{}-{}", std::process::id(),
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos(),
        SEQUENCE.fetch_add(1, Ordering::Relaxed))
}

#[link(name = "kernel32")]
extern "system" {
    fn MoveFileExW(from: *const u16, to: *const u16, flags: u32) -> i32;
}

pub fn atomic_write(path: &Path, contents: &[u8]) -> Result<(), String> {
    let temp = path.with_file_name(format!(".{}-{}.tmp",
        path.file_name().ok_or("Missing file name")?.to_string_lossy(), suffix()));
    let result = (|| {
        let mut file = fs::OpenOptions::new().write(true).create_new(true).open(&temp)
            .map_err(|e| e.to_string())?;
        file.write_all(contents).and_then(|_| file.sync_all()).map_err(|e| e.to_string())?;
        drop(file);
        let from: Vec<u16> = temp.as_os_str().encode_wide().chain(Some(0)).collect();
        let to: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
        if unsafe { MoveFileExW(from.as_ptr(), to.as_ptr(), 1 | 8) } == 0 {
            return Err(std::io::Error::last_os_error().to_string());
        }
        Ok(())
    })();
    if result.is_err() { let _ = fs::remove_file(&temp); }
    result
}

#[derive(Clone)]
pub struct FileSnapshot {
    pub path: PathBuf,
    pub contents: Option<Vec<u8>>,
}

pub fn snapshot_files(dir: &Path) -> Result<Vec<FileSnapshot>, String> {
    SETUP_FILES.iter().map(|name| {
        let path = dir.join(name);
        let contents = match fs::read(&path) {
            Ok(bytes) => Some(bytes),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => None,
            Err(e) => return Err(e.to_string()),
        };
        Ok(FileSnapshot { path, contents })
    }).collect()
}

pub fn restore_files(snapshot: &[FileSnapshot]) -> Result<(), String> {
    let mut errors = Vec::new();
    for item in snapshot {
        let result = match &item.contents {
            Some(bytes) => atomic_write(&item.path, bytes),
            None => match fs::remove_file(&item.path) {
                Ok(()) => Ok(()),
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
                Err(e) => Err(e.to_string()),
            },
        };
        if let Err(e) = result { errors.push(format!("{}: {}", item.path.display(), e)); }
    }
    if errors.is_empty() { Ok(()) } else { Err(errors.join("\n")) }
}

// Preparation can fail or time out without changing the active enrollment.
// A disk backup remains available if committing or restoring ever fails.
pub fn transactional_setup(
    dir: &Path,
    previously_enabled: bool,
    mut set_enabled: impl FnMut(bool) -> Result<(), String>,
    prepare: impl FnOnce(&Path) -> Result<String, String>,
) -> Result<String, String> {
    fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    let snapshot = snapshot_files(dir)?;
    let workspace = dir.join(format!(".setup-{}", suffix()));
    let candidate = workspace.join("candidate");
    let backup = workspace.join("backup");
    fs::create_dir_all(&candidate).map_err(|e| e.to_string())?;
    fs::create_dir(&backup).map_err(|e| e.to_string())?;
    for item in &snapshot {
        if let Some(contents) = &item.contents {
            atomic_write(&backup.join(item.path.file_name().unwrap()), contents)?;
        }
    }
    atomic_write(&backup.join("enabled.txt"), if previously_enabled { b"1" } else { b"0" })?;
    let prepared = prepare(&candidate).and_then(|output| {
        for name in SETUP_FILES {
            if !fs::metadata(candidate.join(name)).map(|m| m.is_file() && m.len() > 0).unwrap_or(false) {
                return Err(format!("Setup did not produce {}", name));
            }
        }
        Ok(output)
    });
    let output = match prepared {
        Ok(output) => output,
        Err(error) => {
            let _ = fs::remove_dir_all(&workspace);
            return Err(format!("{}\n登録済みデータは変更しておりません。", error));
        }
    };
    // No consumer may start with a mixture of old and new credentials.
    if let Err(error) = set_enabled(false) {
        let _ = fs::remove_dir_all(&workspace);
        return Err(error);
    }
    let result = (|| {
        for name in SETUP_FILES {
            let bytes = fs::read(candidate.join(name)).map_err(|e| e.to_string())?;
            atomic_write(&dir.join(name), &bytes)?;
            if fs::read(dir.join(name)).map_err(|e| e.to_string())? != bytes {
                return Err(format!("Failed to verify {}", name));
            }
        }
        set_enabled(previously_enabled)
    })();
    if let Err(error) = result {
        // Attempt every file, even when one restore fails. Keep the provider
        // disabled if the files could not all be recovered.
        let rollback = restore_files(&snapshot).and_then(|_| set_enabled(previously_enabled));
        return match rollback {
            Ok(()) => {
                let _ = fs::remove_dir_all(&workspace);
                Err(format!("{}\n変更は元の状態へ戻しました。", error))
            }
            Err(e) => Err(format!("{}\n復元に失敗しました: {}\nバックアップ: {}", error, e, backup.display())),
        };
    }
    let _ = fs::remove_dir_all(&workspace);
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;
    struct Scratch(PathBuf);
    impl Scratch {
        fn new() -> Self {
            let dir = std::env::temp_dir().join(format!("hfl-transaction-test-{}", suffix()));
            fs::create_dir(&dir).unwrap();
            Self(dir)
        }
    }
    impl Drop for Scratch { fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); } }
    fn fill(dir: &Path, bytes: &[u8]) { for name in SETUP_FILES { fs::write(dir.join(name), bytes).unwrap(); } }
    fn expect(dir: &Path, bytes: &[u8]) { for name in SETUP_FILES { assert_eq!(fs::read(dir.join(name)).unwrap(), bytes); } }

    #[test]
    fn enrollment_or_password_failure_preserves_active_files_and_enabled_state() {
        for initial_setup in [false, true] {
            let dir = Scratch::new();
            if initial_setup { fill(&dir.0, b"old"); }
            let mut changes = Vec::new();
            let error = transactional_setup(&dir.0, initial_setup, |v| { changes.push(v); Ok(()) }, |candidate| {
                fs::write(candidate.join("face.bin"), b"new face").unwrap();
                fs::write(candidate.join("secret.bin"), b"partial secret").unwrap();
                Err("password helper failed".into())
            }).unwrap_err();
            assert!(error.contains("password helper failed"));
            assert!(changes.is_empty());
            if initial_setup { expect(&dir.0, b"old"); }
            else { for name in SETUP_FILES { assert!(!dir.0.join(name).exists()); } }
        }
    }
    #[test]
    fn incomplete_success_cannot_be_committed() {
        let dir = Scratch::new();
        fill(&dir.0, b"old");
        assert!(transactional_setup(&dir.0, true, |_| panic!("must not enable"), |_| Ok("fake success".into())).is_err());
        expect(&dir.0, b"old");
    }
    #[test]
    fn success_preserves_previous_enabled_state() {
        for enabled in [false, true] {
            let dir = Scratch::new();
            fill(&dir.0, b"old");
            let mut changes = Vec::new();
            transactional_setup(&dir.0, enabled, |v| { changes.push(v); Ok(()) }, |candidate| {
                fill(candidate, b"new"); Ok("ok".into())
            }).unwrap();
            expect(&dir.0, b"new");
            assert_eq!(changes, vec![false, enabled]);
        }
    }
    #[test]
    fn failed_activation_rolls_back_and_can_be_retried() {
        let dir = Scratch::new();
        fill(&dir.0, b"old");
        let mut calls = 0;
        assert!(transactional_setup(&dir.0, true, |_| {
            calls += 1;
            if calls == 2 { Err("injected activation failure".into()) } else { Ok(()) }
        }, |candidate| { fill(candidate, b"new"); Ok("ok".into()) }).is_err());
        expect(&dir.0, b"old");
        assert_eq!(calls, 3);
        transactional_setup(&dir.0, true, |_| Ok(()), |candidate| { fill(candidate, b"retry"); Ok("ok".into()) }).unwrap();
        expect(&dir.0, b"retry");
    }
    #[test]
    fn rollback_attempts_later_files_after_an_error() {
        let dir = Scratch::new();
        let blocked = dir.0.join("blocked");
        fs::create_dir(&blocked).unwrap();
        let later = dir.0.join("later");
        fs::write(&later, b"new").unwrap();
        assert!(restore_files(&[
            FileSnapshot { path: blocked, contents: Some(b"old".to_vec()) },
            FileSnapshot { path: later.clone(), contents: Some(b"old".to_vec()) },
        ]).is_err());
        assert_eq!(fs::read(later).unwrap(), b"old");
    }
}
