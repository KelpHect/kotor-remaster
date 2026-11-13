//! Test helpers. Every test writes under `KE_LAUNCHER_TEST_DIR` (or the system temp folder), never into a
//! game install or the player's profile.

use std::fs;
use std::path::{Path, PathBuf};

/// A fresh, empty folder for one test.
pub fn work_dir(name: &str) -> PathBuf {
    let base = std::env::var_os("KE_LAUNCHER_TEST_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| std::env::temp_dir().join("kotor-launcher-tests"));
    let dir = base.join(name);
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).expect("test folder");
    dir
}

pub fn write_bytes(path: &Path, bytes: &[u8]) {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).expect("parent folder");
    }
    fs::write(path, bytes).expect("fixture write");
}

pub fn write_text(path: &Path, text: &str) {
    write_bytes(path, text.as_bytes());
}
