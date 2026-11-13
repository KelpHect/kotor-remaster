//! The few operating-system reads the launcher needs: registry values (read-only, 32-bit view), fixed
//! drives, and attaching to the parent console for the command-line modes.

use std::path::PathBuf;

#[cfg(windows)]
mod imp {
    use std::ffi::OsString;
    use std::os::windows::ffi::{OsStrExt, OsStringExt};
    use std::path::PathBuf;

    use windows_sys::Win32::Foundation::ERROR_SUCCESS;
    use windows_sys::Win32::Storage::FileSystem::{GetDriveTypeW, GetLogicalDrives};
    use windows_sys::Win32::System::Console::{ATTACH_PARENT_PROCESS, AttachConsole};
    use windows_sys::Win32::System::Registry::{
        HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, RRF_RT_REG_SZ, RRF_SUBKEY_WOW6432KEY, RegGetValueW,
    };

    const DRIVE_FIXED: u32 = 3;

    fn wide(text: &str) -> Vec<u16> {
        std::ffi::OsStr::new(text).encode_wide().chain(Some(0)).collect()
    }

    /// A REG_SZ value as a path, read through the 32-bit view (these installers are 32-bit).
    pub fn read_registry_path(local_machine: bool, key: &str, value: &str) -> Option<PathBuf> {
        let hive = if local_machine { HKEY_LOCAL_MACHINE } else { HKEY_CURRENT_USER };
        let key = wide(key);
        let value = wide(value);
        let flags = RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY;
        let mut buffer = vec![0u16; 1024];
        let mut size = (buffer.len() * 2) as u32;
        // SAFETY: the key and value names are NUL-terminated; buffer and size describe a writable buffer.
        let status = unsafe {
            RegGetValueW(
                hive,
                key.as_ptr(),
                value.as_ptr(),
                flags,
                std::ptr::null_mut(),
                buffer.as_mut_ptr().cast(),
                &mut size,
            )
        };
        if status != ERROR_SUCCESS {
            return None;
        }
        let length = (size as usize / 2).min(buffer.len());
        let text = &buffer[..length];
        let text = &text[..text.iter().position(|unit| *unit == 0).unwrap_or(text.len())];
        (!text.is_empty()).then(|| PathBuf::from(OsString::from_wide(text)))
    }

    pub fn fixed_drive_roots() -> Vec<PathBuf> {
        // SAFETY: no arguments; returns a bit mask.
        let drives = unsafe { GetLogicalDrives() };
        (0..26u8)
            .filter(|letter| drives & (1 << letter) != 0)
            .map(|letter| format!("{}:/", char::from(b'A' + letter)))
            // SAFETY: the root name is NUL-terminated.
            .filter(|root| unsafe { GetDriveTypeW(wide(root).as_ptr()) } == DRIVE_FIXED)
            .map(PathBuf::from)
            .collect()
    }

    /// Lets `--list` and friends print into the console that started a GUI-subsystem exe.
    pub fn attach_parent_console() {
        // SAFETY: plain call; failure (no parent console) is fine.
        unsafe {
            AttachConsole(ATTACH_PARENT_PROCESS);
        }
    }
}

#[cfg(not(windows))]
mod imp {
    use std::path::PathBuf;

    pub fn read_registry_path(_local_machine: bool, _key: &str, _value: &str) -> Option<PathBuf> {
        None
    }

    pub fn fixed_drive_roots() -> Vec<PathBuf> {
        Vec::new()
    }

    pub fn attach_parent_console() {}
}

pub use imp::{attach_parent_console, fixed_drive_roots, read_registry_path};

/// Steam's own folder (`HKCU\Software\Valve\Steam\SteamPath`).
pub fn steam_path() -> Option<PathBuf> {
    read_registry_path(false, "Software\\Valve\\Steam", "SteamPath")
}
