//! The engine's user settings file (`settings.ini`), byte for byte as `ke::UserSettings` in
//! `engine/src/game/UserSettings.cpp` reads and writes it.
//!
//! Format: one `key=value` per line, keys in byte order (a `std::map`), values printed the way an
//! `std::ostream` prints a `float` (`%g`, six significant digits), then one `movie=<name>` line per seen
//! movie in byte order (a `std::set`). The C++ file is opened in text mode, so on Windows every line ends
//! in CRLF. Saving writes `<path>.tmp` and renames it over the file.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};

/// The line ending `std::ofstream` writes in text mode on this platform.
pub const LINE_END: &str = if cfg!(windows) { "\r\n" } else { "\n" };

/// The shipped default of every known key (`UserSettings::defaults()`).
pub const DEFAULTS: &[(&str, f32)] = &[
    ("gameplay.invertcamera", 0.0),
    ("gameplay.autolevelup", 0.0),
    ("gameplay.autosave", 1.0),
    ("gameplay.reversecamera", 0.0),
    ("gameplay.disablemouse", 1.0),
    ("gameplay.difficulty", 1.0),
    ("sound.music", 80.0),
    ("sound.voice", 100.0),
    ("sound.effects", 100.0),
    ("sound.movie", 100.0),
    ("sound.forcesoftware", 0.0),
    ("graphics.gamma", 50.0),
    ("graphics.shadows", 1.0),
    ("graphics.grass", 1.0),
    ("graphics.framebuffer", 1.0),
    ("graphics.vsync", 1.0),
    ("graphics.softshadows", 0.0),
    ("autopause.enemysighted", 1.0),
    ("autopause.partykilled", 1.0),
    ("autopause.actionmenu", 0.0),
    ("autopause.endround", 0.0),
    ("autopause.triggers", 1.0),
    ("autopause.minesighted", 1.0),
    ("mouse.sensitivity", 50.0),
    ("mouse.reversebuttons", 0.0),
    ("feedback.hideunequippable", 0.0),
    ("feedback.tutorialpopups", 1.0),
    ("feedback.subtitles", 1.0),
    ("feedback.minimap", 1.0),
    ("feedback.floatingnumbers", 1.0),
    ("feedback.statussummary", 1.0),
    ("feedback.hidequickmenu", 0.0),
    ("feedback.smallfont", 0.0),
    ("feedback.tooltips", 1.0),
];

/// The default of `key`, if the engine knows it.
pub fn default_of(key: &str) -> Option<f32> {
    DEFAULTS.iter().find(|(name, _)| *name == key).map(|(_, value)| *value)
}

#[derive(Debug, Clone, PartialEq)]
pub struct UserSettings {
    values: BTreeMap<String, f32>,
    pub seen_movies: BTreeSet<String>,
    pub dirty: bool,
}

impl Default for UserSettings {
    fn default() -> Self {
        Self {
            values: DEFAULTS.iter().map(|(key, value)| ((*key).to_owned(), *value)).collect(),
            seen_movies: BTreeSet::new(),
            dirty: false,
        }
    }
}

impl UserSettings {
    /// The value of `key`; 0 when the key is unknown (as the C++ class).
    pub fn get(&self, key: &str) -> f32 {
        self.values.get(key).copied().unwrap_or(0.0)
    }

    pub fn set(&mut self, key: &str, value: f32) {
        let slot = self.values.entry(key.to_owned()).or_insert(0.0);
        if *slot != value {
            *slot = value;
            self.dirty = true;
        }
    }

    /// Loads `path` over the current values. `Ok(false)` when the file does not exist or cannot be opened.
    pub fn load(&mut self, path: &Path) -> bool {
        let Ok(bytes) = fs::read(path) else {
            return false;
        };
        self.load_bytes(&bytes);
        true
    }

    pub fn load_bytes(&mut self, bytes: &[u8]) {
        for raw in bytes.split(|byte| *byte == b'\n') {
            // Text mode turns CRLF into LF; a lone trailing CR is all that can remain.
            let line = raw.strip_suffix(b"\r").unwrap_or(raw);
            let Some(equals) = line.iter().position(|byte| *byte == b'=') else {
                continue;
            };
            let key = String::from_utf8_lossy(&line[..equals]).into_owned();
            let value = String::from_utf8_lossy(&line[equals + 1..]).into_owned();
            if key == "movie" {
                self.seen_movies.insert(value);
            } else {
                self.values.insert(key, strtof(&value));
            }
        }
        self.dirty = false;
    }

    /// The exact bytes `save` writes.
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut out = String::new();
        for (key, value) in &self.values {
            out.push_str(key);
            out.push('=');
            out.push_str(&format_g(*value));
            out.push_str(LINE_END);
        }
        for movie in &self.seen_movies {
            out.push_str("movie=");
            out.push_str(movie);
            out.push_str(LINE_END);
        }
        out.into_bytes()
    }

    /// Writes a temporary file beside `path` and renames it over, so a crash never leaves a cut-off file.
    pub fn save(&self, path: &Path) -> io::Result<()> {
        write_atomic(path, &self.to_bytes())
    }
}

/// Writes `<path>.tmp`, flushes it and renames it over `path` (replacing it). The temporary is removed
/// when anything fails.
pub fn write_atomic(path: &Path, bytes: &[u8]) -> io::Result<()> {
    if let Some(parent) = path.parent().filter(|parent| !parent.as_os_str().is_empty()) {
        // Like the C++ code, a failure here surfaces when the file is opened.
        let _ = fs::create_dir_all(parent);
    }
    let mut temporary: PathBuf = path.as_os_str().to_owned().into();
    temporary.as_mut_os_string().push(".tmp");
    let result = (|| {
        let mut file = fs::File::create(&temporary)?;
        file.write_all(bytes)?;
        file.sync_all()?;
        drop(file);
        fs::rename(&temporary, path)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    result
}

/// `std::ostream << float` with the default flags: `%g` with six significant digits, as MSVC prints it.
pub fn format_g(value: f32) -> String {
    let value = f64::from(value);
    if value.is_nan() {
        return "nan".to_owned();
    }
    if value.is_infinite() {
        return if value < 0.0 { "-inf" } else { "inf" }.to_owned();
    }
    if value == 0.0 {
        return if value.is_sign_negative() { "-0" } else { "0" }.to_owned();
    }
    const PRECISION: i32 = 6;
    // Scientific form with PRECISION significant digits gives the rounded exponent %g decides on.
    let scientific = format!("{:.*e}", (PRECISION - 1) as usize, value);
    let (mantissa, exponent) = scientific.split_once('e').expect("{:e} always has an exponent");
    let exponent: i32 = exponent.parse().expect("{:e} exponent is an integer");
    if (-4..PRECISION).contains(&exponent) {
        let decimals = (PRECISION - 1 - exponent) as usize;
        strip_zeros(format!("{value:.decimals$}"))
    } else {
        let mantissa = strip_zeros(mantissa.to_owned());
        let sign = if exponent < 0 { '-' } else { '+' };
        format!("{mantissa}e{sign}{:02}", exponent.abs())
    }
}

fn strip_zeros(text: String) -> String {
    if !text.contains('.') {
        return text;
    }
    text.trim_end_matches('0').trim_end_matches('.').to_owned()
}

/// `std::strtof(text, nullptr)`: the longest leading decimal number, 0 when there is none.
pub fn strtof(text: &str) -> f32 {
    let trimmed = text.trim_start_matches([' ', '\t', '\n', '\x0b', '\x0c', '\r']);
    let bytes = trimmed.as_bytes();
    let lower = trimmed.to_ascii_lowercase();
    let (sign, rest) = match bytes.first() {
        Some(b'-') => (-1.0f32, &lower[1..]),
        Some(b'+') => (1.0, &lower[1..]),
        _ => (1.0, &lower[..]),
    };
    if rest.starts_with("inf") {
        return sign * f32::INFINITY;
    }
    if rest.starts_with("nan") {
        return f32::NAN;
    }
    let mut end = usize::from(matches!(bytes.first(), Some(b'-' | b'+')));
    let digits_start = end;
    let mut digits = 0;
    while end < bytes.len() && bytes[end].is_ascii_digit() {
        end += 1;
        digits += 1;
    }
    if end < bytes.len() && bytes[end] == b'.' {
        end += 1;
        while end < bytes.len() && bytes[end].is_ascii_digit() {
            end += 1;
            digits += 1;
        }
    }
    if digits == 0 {
        return 0.0;
    }
    if end < bytes.len() && (bytes[end] == b'e' || bytes[end] == b'E') {
        let mut at = end + 1;
        if at < bytes.len() && (bytes[at] == b'-' || bytes[at] == b'+') {
            at += 1;
        }
        let exponent_start = at;
        while at < bytes.len() && bytes[at].is_ascii_digit() {
            at += 1;
        }
        if at > exponent_start {
            end = at;
        }
    }
    let _ = digits_start;
    trimmed[..end].parse::<f32>().unwrap_or(0.0)
}

#[cfg(test)]
mod tests {
    use super::*;

    const CPP_DEFAULTS: &[u8] = include_bytes!("../tests/fixtures/defaults_cpp.ini");
    const CPP_CUSTOM: &[u8] = include_bytes!("../tests/fixtures/custom_cpp.ini");

    fn custom() -> UserSettings {
        // The same calls tests/fixtures/gen_settings.cpp makes on ke::UserSettings.
        let mut settings = UserSettings::default();
        settings.set("sound.music", 35.0);
        settings.set("graphics.width", 1920.0);
        settings.set("graphics.height", 1080.0);
        settings.set("graphics.fullscreen", 1.0);
        settings.set("autopause.endround", 1.0);
        settings.set("graphics.gamma", 33.3333);
        settings.set("mouse.sensitivity", 0.1);
        settings.set("test.tiny", 0.000_012_34);
        settings.set("test.big", 1_234_567.0);
        settings.set("test.negative", -2.5);
        settings.set("test.round", 999_999.5);
        settings.set("test.third", 1.0 / 3.0);
        settings.set("Test.Upper", 7.0);
        for movie in ["leclogo", "biologo", "01a"] {
            settings.seen_movies.insert(movie.to_owned());
        }
        settings
    }

    #[test]
    fn defaults_match_cpp_bytes() {
        assert_eq!(UserSettings::default().to_bytes(), CPP_DEFAULTS);
    }

    #[test]
    fn custom_values_match_cpp_bytes() {
        assert_eq!(String::from_utf8(custom().to_bytes()).unwrap(), String::from_utf8(CPP_CUSTOM.to_vec()).unwrap());
    }

    #[test]
    fn cpp_file_round_trips_byte_identical() {
        let mut settings = UserSettings::default();
        settings.load_bytes(CPP_CUSTOM);
        assert!(!settings.dirty);
        assert_eq!(settings.get("sound.music"), 35.0);
        assert_eq!(settings.get("graphics.width"), 1920.0);
        assert!(settings.seen_movies.contains("biologo"));
        assert_eq!(settings.to_bytes(), CPP_CUSTOM);
    }

    #[test]
    fn save_is_atomic_and_reloads() {
        let dir = crate::testutil::work_dir("settings_save");
        let path = dir.join("nested").join("settings.ini");
        let mut settings = custom();
        settings.save(&path).unwrap();
        assert!(!dir.join("nested").join("settings.ini.tmp").exists());
        assert_eq!(fs::read(&path).unwrap(), CPP_CUSTOM);
        settings.set("sound.music", 10.0);
        settings.save(&path).unwrap();
        let mut again = UserSettings::default();
        assert!(again.load(&path));
        assert_eq!(again.get("sound.music"), 10.0);
        assert!(!UserSettings::default().load(&dir.join("missing.ini")));
    }

    #[test]
    fn set_marks_dirty_only_on_change() {
        let mut settings = UserSettings::default();
        settings.set("sound.music", 80.0);
        assert!(!settings.dirty);
        settings.set("sound.music", 81.0);
        assert!(settings.dirty);
        assert_eq!(settings.get("no.such.key"), 0.0);
    }

    #[test]
    fn g_format_and_strtof() {
        for (value, text) in [
            (80.0f32, "80"),
            (0.5, "0.5"),
            (100000.0, "100000"),
            (1_000_000.0, "1e+06"),
            (0.0001, "0.0001"),
            (0.00001, "1e-05"),
            (-0.0, "-0"),
            (123.456_79, "123.457"),
        ] {
            assert_eq!(format_g(value), text, "{value}");
        }
        assert_eq!(strtof("  12.5xyz"), 12.5);
        assert_eq!(strtof("1e3"), 1000.0);
        assert_eq!(strtof("1e"), 1.0);
        assert_eq!(strtof("abc"), 0.0);
        assert_eq!(strtof("-.5"), -0.5);
        assert_eq!(strtof(""), 0.0);
    }
}
