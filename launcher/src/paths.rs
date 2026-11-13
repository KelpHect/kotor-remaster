//! Path text as the C++ launcher writes it: `std::filesystem::path::generic_u8string()`, forward slashes.

use std::path::{Path, PathBuf};

/// The generic (forward-slash) UTF-8 form of a path.
pub fn generic(path: &Path) -> String {
    let text = path.to_string_lossy();
    if cfg!(windows) { text.replace('\\', "/") } else { text.into_owned() }
}

/// A path from config text; empty text is an empty path.
pub fn from_text(text: &str) -> PathBuf {
    PathBuf::from(text)
}

/// `%APPDATA%/kotor-remaster/<app>`, the folder `SDL_GetPrefPath("kotor-remaster", app)` names. Not created
/// here; saving creates it.
pub fn pref_folder(app: &str) -> PathBuf {
    let base = if cfg!(windows) {
        std::env::var_os("APPDATA").map(PathBuf::from)
    } else {
        std::env::var_os("XDG_DATA_HOME")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".local").join("share")))
    };
    base.unwrap_or_else(|| PathBuf::from(".")).join("kotor-remaster").join(app)
}

/// The folder holding this executable.
pub fn exe_folder() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(Path::to_path_buf))
        .unwrap_or_else(|| PathBuf::from("."))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn generic_uses_forward_slashes() {
        if cfg!(windows) {
            assert_eq!(generic(Path::new("C:\\a\\b c\\d")), "C:/a/b c/d");
        }
        assert_eq!(generic(Path::new("a/b")), "a/b");
    }
}
