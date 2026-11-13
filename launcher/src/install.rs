//! Install detection and data-set validation (ADR 0003), ported from `engine/src/launcher/InstallDetect.*`.
//!
//! An install is judged by its data set (`chitin.key`, `dialog.tlk`, `data/*.bif`, the module folders),
//! never by the original executable, which is never opened. Detection is pure: every place it looks is
//! passed in, so the tests run on fixtures. Only [`system_sources`] touches the registry and the
//! environment, read-only. A new game is one more row in [`game_catalog`].

use std::collections::HashSet;
use std::fmt::Write as _;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use sha2::{Digest, Sha256};

use crate::paths::generic;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Severity {
    Warning,
    Error,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Issue {
    pub severity: Severity,
    /// Stable machine name, e.g. `missing_file`.
    pub code: &'static str,
    /// What to tell the player.
    pub message: String,
    /// Install-relative, forward slashes (may be empty).
    pub path: String,
}

/// A data set the project has seen and can name. The `chitin.key` hash identifies it.
#[derive(Debug, Clone)]
pub struct KnownDataSet {
    pub key_sha256: String,
    pub id: String,
    pub version: String,
    pub language_id: i32,
}

#[derive(Debug, Clone)]
pub struct RegistryLocation {
    /// HKLM when true, HKCU otherwise.
    pub local_machine: bool,
    /// Path under the hive (32-bit view).
    pub key: &'static str,
    pub value: &'static str,
}

#[derive(Debug, Clone)]
pub struct GameDef {
    /// Stable name used in the launcher config.
    pub id: &'static str,
    pub name: &'static str,
    /// The engine can play it today.
    pub launchable: bool,
    pub steam_app_id: u32,
    pub folder_names: Vec<&'static str>,
    pub registry: Vec<RegistryLocation>,
    pub required_files: Vec<&'static str>,
    pub required_dirs: Vec<&'static str>,
    pub expected_dirs: Vec<&'static str>,
    pub override_dir: &'static str,
    pub known: Vec<KnownDataSet>,
}

pub fn game_catalog() -> &'static [GameDef] {
    static CATALOG: OnceLock<Vec<GameDef>> = OnceLock::new();
    CATALOG.get_or_init(|| {
        vec![
            GameDef {
                id: "kotor1",
                name: "Knights of the Old Republic",
                launchable: true,
                steam_app_id: 32370,
                folder_names: vec!["swkotor", "Star Wars - KotOR", "Knights of the Old Republic"],
                registry: vec![
                    RegistryLocation {
                        local_machine: true,
                        key: "SOFTWARE\\GOG.com\\Games\\1207666283",
                        value: "path",
                    },
                    RegistryLocation { local_machine: true, key: "SOFTWARE\\BioWare\\SW\\KOTOR", value: "Path" },
                    RegistryLocation { local_machine: true, key: "SOFTWARE\\LucasArts\\KotOR", value: "InstallPath" },
                ],
                required_files: vec!["chitin.key", "dialog.tlk", "patch.erf"],
                required_dirs: vec!["data", "modules", "rims"],
                expected_dirs: vec!["lips", "TexturePacks", "streammusic", "streamwaves"],
                override_dir: "override",
                known: vec![KnownDataSet {
                    key_sha256: "2029ec1ee3f0e78a951320a7d9e3cfd0c21fc7ad6ec875e5de59a75f1c62df98".into(),
                    id: "kotor1-pc-1.0.3.0-english".into(),
                    version: "1.0.3.0".into(),
                    language_id: 0,
                }],
            },
            // Listed so it is detected and named; the engine does not play it yet (Sprint 18).
            GameDef {
                id: "kotor2",
                name: "Knights of the Old Republic II",
                launchable: false,
                steam_app_id: 208580,
                folder_names: vec!["Knights of the Old Republic II", "swkotor2"],
                registry: vec![
                    RegistryLocation {
                        local_machine: true,
                        key: "SOFTWARE\\GOG.com\\Games\\1421404581",
                        value: "path",
                    },
                    RegistryLocation { local_machine: true, key: "SOFTWARE\\LucasArts\\KotOR2", value: "Path" },
                ],
                required_files: vec!["chitin.key", "dialog.tlk"],
                required_dirs: vec!["data", "modules"],
                expected_dirs: vec!["lips", "TexturePacks", "streammusic", "streamvoice"],
                override_dir: "override",
                known: vec![KnownDataSet {
                    key_sha256: "47bea08efb5c582918fe2ff32fec845ce1cf604ce8bdd62f5b437004fb257412".into(),
                    id: "kotor2-pc-steam-english".into(),
                    version: String::new(),
                    language_id: 0,
                }],
            },
        ]
    })
}

pub fn find_game(id: &str) -> Option<&'static GameDef> {
    game_catalog().iter().find(|game| game.id == id)
}

/// The TLK language names the engine knows (`kotor::supportedLanguages()`).
pub const LANGUAGES: &[(u32, &str)] = &[
    (0, "English"),
    (1, "French"),
    (2, "German"),
    (3, "Italian"),
    (4, "Spanish"),
    (5, "Polish"),
    (6, "Russian"),
    (7, "Czech"),
    (8, "Hungarian"),
    (9, "Portuguese"),
    (10, "Chinese"),
    (11, "Japanese"),
    (12, "Korean"),
];

pub fn language_name(id: u32) -> &'static str {
    LANGUAGES.iter().find(|(lang, _)| *lang == id).map_or("Unknown", |(_, name)| name)
}

/// `kotor::languageId`: a number or a name (any case); `None` when unknown.
pub fn language_id(name_or_id: &str) -> Option<u32> {
    if !name_or_id.is_empty() && name_or_id.bytes().all(|byte| byte.is_ascii_digit()) {
        let value: u64 = name_or_id.parse().ok()?;
        return LANGUAGES.iter().find(|(id, _)| u64::from(*id) == value).map(|(id, _)| *id);
    }
    LANGUAGES.iter().find(|(_, name)| name.eq_ignore_ascii_case(name_or_id)).map(|(id, _)| *id)
}

#[derive(Debug, Clone, Default)]
pub struct ValidationReport {
    pub game_id: String,
    pub root: PathBuf,
    /// No error issue.
    pub valid: bool,
    /// `chitin.key` matches a known data set.
    pub recognized: bool,
    pub dataset_id: String,
    pub version: String,
    pub language_id: Option<u32>,
    pub language_name: String,
    pub key_sha256: String,
    pub bif_count: usize,
    pub key_entries: usize,
    pub tlk_strings: usize,
    pub override_files: usize,
    pub issues: Vec<Issue>,
}

impl ValidationReport {
    pub fn count(&self, severity: Severity) -> usize {
        self.issues.iter().filter(|issue| issue.severity == severity).count()
    }

    #[cfg(test)]
    pub fn modded(&self) -> bool {
        self.override_files > 0
    }

    #[cfg(test)]
    pub fn has(&self, code: &str) -> bool {
        self.issues.iter().any(|issue| issue.code == code)
    }

    fn add(&mut self, severity: Severity, code: &'static str, message: String, path: &str) {
        self.issues.push(Issue { severity, code, message, path: path.to_owned() });
    }
}

/// The entry of `dir` named `name`, ignoring case (the install may sit on a case-sensitive disk).
pub fn find_entry(dir: &Path, name: &str) -> Option<PathBuf> {
    let exact = dir.join(name);
    if exact.exists() {
        return Some(exact);
    }
    let wanted = name.to_ascii_lowercase();
    fs::read_dir(dir)
        .ok()?
        .flatten()
        .find(|entry| entry.file_name().to_string_lossy().to_ascii_lowercase() == wanted)
        .map(|entry| entry.path())
}

/// Resolves `data/foo.bif` or `data\foo.bif` under `root`, one segment at a time.
fn resolve_relative(root: &Path, relative: &str) -> Option<PathBuf> {
    let mut current = root.to_path_buf();
    for part in relative.split(['/', '\\']).filter(|part| !part.is_empty()) {
        current = find_entry(&current, part)?;
    }
    Some(current)
}

fn u32_at(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

fn u16_at(bytes: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([bytes[at], bytes[at + 1]])
}

const KEY_HEADER: usize = 64;
const KEY_BIF_RECORD: u64 = 12;
const KEY_ENTRY_RECORD: u64 = 22;
const TLK_HEADER: usize = 20;
const TLK_ENTRY_RECORD: u64 = 40;

fn check_key(report: &mut ValidationReport, key_path: &Path) {
    let blob = fs::read(key_path).unwrap_or_default();
    if blob.len() < KEY_HEADER || &blob[..8] != b"KEY V1  " {
        report.add(Severity::Error, "key_format", "chitin.key is not a KEY V1 file".into(), "chitin.key");
        return;
    }
    let bif_count = u32_at(&blob, 8);
    let key_count = u32_at(&blob, 12);
    let off_files = u32_at(&blob, 16);
    let off_keys = u32_at(&blob, 20);
    let size = blob.len() as u64;
    if (off_files as usize) < KEY_HEADER || u64::from(off_files) + u64::from(bif_count) * KEY_BIF_RECORD > size {
        report.add(
            Severity::Error,
            "key_format",
            "chitin.key archive table lies outside the file".into(),
            "chitin.key",
        );
        return;
    }
    if u64::from(off_keys) + u64::from(key_count) * KEY_ENTRY_RECORD > size {
        report.add(Severity::Error, "key_format", "chitin.key resource table is cut off".into(), "chitin.key");
        return;
    }
    report.bif_count = bif_count as usize;
    report.key_entries = key_count as usize;

    let root = report.root.clone();
    let mut missing = 0usize;
    let mut resized = 0usize;
    let mut first_missing: Vec<String> = Vec::new();
    for index in 0..bif_count as usize {
        let record = off_files as usize + index * KEY_BIF_RECORD as usize;
        let declared = u32_at(&blob, record);
        let name_offset = u32_at(&blob, record + 4) as usize;
        let name_length = usize::from(u16_at(&blob, record + 8));
        if (name_offset as u64) + (name_length as u64) > size {
            report.add(
                Severity::Error,
                "key_format",
                "chitin.key names an archive outside the file".into(),
                "chitin.key",
            );
            return;
        }
        let raw = &blob[name_offset..name_offset + name_length];
        let raw = &raw[..raw.iter().position(|byte| *byte == 0).unwrap_or(raw.len())];
        let name = String::from_utf8_lossy(raw).into_owned();
        match resolve_relative(&root, &name).filter(|found| found.is_file()) {
            None => {
                missing += 1;
                if first_missing.len() < 5 {
                    first_missing.push(name.replace('\\', "/"));
                }
            }
            Some(found) => {
                if fs::metadata(&found).map(|meta| meta.len()).unwrap_or(0) != u64::from(declared) {
                    resized += 1;
                }
            }
        }
    }
    if missing > 0 {
        let more = if missing > first_missing.len() { ", ..." } else { "" };
        report.add(
            Severity::Error,
            "missing_bif",
            format!("{missing} archive(s) named by chitin.key are missing ({}{more})", first_missing.join(", ")),
            "data",
        );
    }
    if resized > 0 {
        report.add(
            Severity::Warning,
            "bif_size",
            format!(
                "{resized} archive(s) differ in size from what chitin.key declares; the install is patched or modified"
            ),
            "data",
        );
    }
}

fn check_tlk(report: &mut ValidationReport, tlk_path: &Path) {
    let mut head = [0u8; TLK_HEADER];
    let read = fs::File::open(tlk_path).and_then(|mut file| {
        let size = file.metadata()?.len();
        file.read_exact(&mut head)?;
        Ok(size)
    });
    let size = match read {
        Ok(size) if &head[..8] == b"TLK V3.0" => size,
        _ => {
            report.add(Severity::Error, "tlk_format", "dialog.tlk is not a TLK V3.0 file".into(), "dialog.tlk");
            return;
        }
    };
    let language = u32_at(&head, 8);
    let count = u32_at(&head, 12);
    let strings_offset = u32_at(&head, 16);
    if TLK_HEADER as u64 + u64::from(count) * TLK_ENTRY_RECORD > size || u64::from(strings_offset) > size {
        report.add(Severity::Error, "tlk_format", "dialog.tlk string table is cut off".into(), "dialog.tlk");
        return;
    }
    report.language_id = Some(language);
    report.language_name = language_name(language).to_owned();
    report.tlk_strings = count as usize;
}

/// Lower-case hex SHA-256 of a file; empty when it cannot be read.
pub fn sha256_file(path: &Path) -> String {
    let Ok(mut file) = fs::File::open(path) else {
        return String::new();
    };
    let mut hasher = Sha256::new();
    let mut buffer = vec![0u8; 1 << 16];
    loop {
        match file.read(&mut buffer) {
            Ok(0) => break,
            Ok(read) => hasher.update(&buffer[..read]),
            Err(_) => return String::new(),
        }
    }
    hasher.finalize().iter().fold(String::with_capacity(64), |mut text, byte| {
        let _ = write!(text, "{byte:02x}");
        text
    })
}

fn has_any_bif(dir: &Path) -> bool {
    fs::read_dir(dir).is_ok_and(|entries| {
        entries
            .flatten()
            .any(|entry| Path::new(&entry.file_name()).extension().is_some_and(|ext| ext.eq_ignore_ascii_case("bif")))
    })
}

/// Reads the install and checks it against `game`. Never writes. `hash_key` false skips the SHA-256 of
/// `chitin.key` (the structure checks still run).
pub fn validate_install(root: &Path, game: &GameDef, hash_key: bool) -> ValidationReport {
    let mut report = ValidationReport { game_id: game.id.to_owned(), root: root.to_path_buf(), ..Default::default() };
    if !root.is_dir() {
        report.add(Severity::Error, "no_folder", "The folder does not exist".into(), "");
        return report;
    }

    let mut key_path = None;
    let mut tlk_path = None;
    for name in &game.required_files {
        match find_entry(root, name).filter(|found| found.is_file()) {
            None => report.add(Severity::Error, "missing_file", format!("Required file is missing: {name}"), name),
            Some(found) if name.eq_ignore_ascii_case("chitin.key") => key_path = Some(found),
            Some(found) if name.eq_ignore_ascii_case("dialog.tlk") => tlk_path = Some(found),
            Some(_) => {}
        }
    }
    for name in &game.required_dirs {
        match find_entry(root, name).filter(|found| found.is_dir()) {
            None => report.add(Severity::Error, "missing_dir", format!("Required folder is missing: {name}"), name),
            Some(found) if name.eq_ignore_ascii_case("data") && !has_any_bif(&found) => {
                report.add(Severity::Error, "no_bif", "The data folder holds no .bif archives".into(), name);
            }
            Some(_) => {}
        }
    }
    for name in &game.expected_dirs {
        if find_entry(root, name).is_none() {
            report.add(
                Severity::Warning,
                "missing_expected_dir",
                format!("Folder not found: {name} (some content may be unavailable)"),
                name,
            );
        }
    }

    if let Some(key_path) = &key_path {
        check_key(&mut report, key_path);
        if hash_key {
            report.key_sha256 = sha256_file(key_path);
            for known in &game.known {
                if known.key_sha256 == report.key_sha256 {
                    report.recognized = true;
                    report.dataset_id = known.id.clone();
                    report.version = known.version.clone();
                }
            }
            if !report.recognized {
                report.add(
                    Severity::Warning,
                    "unknown_dataset",
                    "chitin.key is not a data set this project has seen (another version, language or a modified install); it may still work".into(),
                    "chitin.key",
                );
            }
        }
    }
    if let Some(tlk_path) = &tlk_path {
        check_tlk(&mut report, tlk_path);
        if let (true, Some(language)) = (report.recognized, report.language_id) {
            let mismatch = game.known.iter().any(|known| {
                known.key_sha256 == report.key_sha256
                    && !known.version.is_empty()
                    && i64::from(known.language_id) != i64::from(language)
            });
            if mismatch {
                let message =
                    format!("dialog.tlk language ({}) differs from the reference data set", report.language_name);
                report.add(Severity::Warning, "language_mismatch", message, "dialog.tlk");
            }
        }
    }

    if !game.override_dir.is_empty()
        && let Some(folder) = find_entry(root, game.override_dir).filter(|found| found.is_dir())
    {
        report.override_files = fs::read_dir(&folder)
            .map(|entries| entries.flatten().filter(|entry| entry.path().is_file()).count())
            .unwrap_or(0);
        if report.override_files > 0 {
            let message = format!(
                "{} file(s) in {}: this install carries modifications. They are read in place and never changed or removed",
                report.override_files, game.override_dir
            );
            report.add(Severity::Warning, "modded", message, game.override_dir);
        }
    }

    report.valid = report.count(Severity::Error) == 0;
    report
}

/// The game whose data set best matches the folder, or `None` when it is none of the catalog.
pub fn identify_install(root: &Path) -> Option<&'static GameDef> {
    if !root.is_dir() {
        return None;
    }
    let key = find_entry(root, "chitin.key")?;
    let sha = sha256_file(&key);
    let mut best: Option<(&GameDef, i32)> = None;
    for game in game_catalog() {
        let mut score = 0i32;
        for name in game.required_files.iter().chain(game.required_dirs.iter()) {
            // A missing piece counts against the game.
            score += if find_entry(root, name).is_some() { 1 } else { -1 };
        }
        score += 1000 * game.known.iter().filter(|known| known.key_sha256 == sha).count() as i32;
        if best.is_none_or(|(_, best_score)| score > best_score) {
            best = Some((game, score));
        }
    }
    best.map(|(game, _)| game)
}

/// A readable multi-line report, for the launcher and for logs (`formatReport`).
pub fn format_report(report: &ValidationReport) -> String {
    let name = find_game(&report.game_id).map_or(report.game_id.as_str(), |game| game.name);
    let mut out = String::new();
    let _ = writeln!(out, "{name}: {}", if report.valid { "usable" } else { "not usable" });
    let _ = writeln!(out, "  folder: {}", generic(&report.root));
    if report.recognized {
        let _ = writeln!(out, "  data set: {}", report.dataset_id);
    } else if !report.key_sha256.is_empty() {
        let _ = writeln!(out, "  data set: not recognised (key {})", &report.key_sha256[..12]);
    }
    if report.language_id.is_some() {
        let _ = writeln!(out, "  language: {} ({} strings)", report.language_name, report.tlk_strings);
    }
    if report.bif_count > 0 {
        let _ = writeln!(out, "  archives: {}, resources: {}", report.bif_count, report.key_entries);
    }
    for issue in &report.issues {
        let label = match issue.severity {
            Severity::Error => "error: ",
            Severity::Warning => "warning: ",
        };
        let _ = writeln!(out, "  {label}{}", issue.message);
    }
    out
}

// ---- detection ----------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq)]
pub struct PathSource {
    pub path: PathBuf,
    /// "steam", "gog", "registry", "environment", "saved", "browsed", "common", "command line".
    pub origin: String,
}

impl PathSource {
    pub fn new(path: impl Into<PathBuf>, origin: &str) -> Self {
        Self { path: path.into(), origin: origin.to_owned() }
    }
}

#[derive(Debug, Clone, Default)]
pub struct DetectionSources {
    /// Each holds `steamapps/libraryfolders.vdf`.
    pub steam_roots: Vec<PathBuf>,
    /// Steam libraries known without the vdf.
    pub library_roots: Vec<PathBuf>,
    /// Folders that may hold a game folder directly.
    pub common_roots: Vec<PathBuf>,
    /// Folders that are an install root themselves.
    pub installs: Vec<PathSource>,
}

#[derive(Debug, Clone)]
pub struct DetectedInstall {
    pub game_id: String,
    pub origin: String,
    pub report: ValidationReport,
}

/// The library folders a Steam `libraryfolders.vdf` lists. Valve's text format: `"path"  "D:\\Library"`.
/// Only `"path"` keys matter; escapes are backslash pairs.
pub fn parse_library_folders(text: &str) -> Vec<PathBuf> {
    let bytes = text.as_bytes();
    let mut libraries = Vec::new();
    let mut at = 0;
    while let Some(found) = text[at..].find("\"path\"") {
        at += found + 6;
        let Some(open) = text[at..].find('"').map(|offset| at + offset) else {
            break;
        };
        let mut value = Vec::new();
        let mut index = open + 1;
        while index < bytes.len() && bytes[index] != b'"' {
            if bytes[index] == b'\\' && index + 1 < bytes.len() {
                index += 1;
            }
            value.push(bytes[index]);
            index += 1;
        }
        if !value.is_empty() {
            libraries.push(PathBuf::from(String::from_utf8_lossy(&value).into_owned()));
        }
        at = index.min(text.len());
        // `at` may sit inside a multi-byte character only if the vdf is not UTF-8; step to a boundary.
        while !text.is_char_boundary(at) {
            at += 1;
        }
    }
    libraries
}

/// The `installdir` of a Steam appmanifest.
pub fn parse_install_dir(text: &str) -> String {
    let Some(key) = text.find("\"installdir\"") else {
        return String::new();
    };
    let rest = &text[key + 12..];
    let Some(open) = rest.find('"') else {
        return String::new();
    };
    let rest = &rest[open + 1..];
    rest.find('"').map_or_else(String::new, |close| rest[..close].to_owned())
}

fn read_text(path: &Path) -> String {
    fs::read(path).map(|bytes| String::from_utf8_lossy(&bytes).into_owned()).unwrap_or_default()
}

fn path_key(path: &Path) -> String {
    let canonical = fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf());
    generic(&canonical).to_lowercase()
}

/// Every distinct install found, in the order the sources list them. Unreadable places are skipped.
pub fn detect_installs(sources: &DetectionSources) -> Vec<DetectedInstall> {
    let mut candidates: Vec<PathSource> = Vec::new();
    let mut libraries = sources.library_roots.clone();
    for steam in &sources.steam_roots {
        libraries.push(steam.clone());
        libraries.extend(parse_library_folders(&read_text(&steam.join("steamapps").join("libraryfolders.vdf"))));
    }
    for library in &libraries {
        let apps = library.join("steamapps");
        for game in game_catalog() {
            let mut names: Vec<String> = Vec::new();
            if game.steam_app_id != 0 {
                let manifest = apps.join(format!("appmanifest_{}.acf", game.steam_app_id));
                let install_dir = parse_install_dir(&read_text(&manifest));
                if !install_dir.is_empty() {
                    names.push(install_dir);
                }
            }
            names.extend(game.folder_names.iter().map(|name| (*name).to_owned()));
            for name in names {
                candidates.push(PathSource::new(apps.join("common").join(name), "steam"));
            }
        }
    }
    for common in &sources.common_roots {
        for game in game_catalog() {
            for name in &game.folder_names {
                candidates.push(PathSource::new(common.join(name), "common"));
            }
        }
    }
    candidates.extend(sources.installs.iter().cloned());

    let mut found = Vec::new();
    let mut seen = HashSet::new();
    for candidate in candidates {
        if candidate.path.as_os_str().is_empty() || !candidate.path.is_dir() {
            continue;
        }
        if !seen.insert(path_key(&candidate.path)) {
            continue;
        }
        let Some(game) = identify_install(&candidate.path) else {
            continue;
        };
        found.push(DetectedInstall {
            game_id: game.id.to_owned(),
            origin: candidate.origin,
            report: validate_install(&candidate.path, game, true),
        });
    }
    found
}

/// The real machine: Steam and GOG registry keys (read-only), `KOTOR_INSTALL`, fixed-drive library folders.
pub fn system_sources() -> DetectionSources {
    let mut sources = DetectionSources::default();
    if let Some(steam) = crate::platform::steam_path() {
        sources.steam_roots.push(steam);
    }
    for game in game_catalog() {
        for location in &game.registry {
            if let Some(path) =
                crate::platform::read_registry_path(location.local_machine, location.key, location.value)
            {
                let origin = if location.key.contains("GOG") { "gog" } else { "registry" };
                sources.installs.push(PathSource::new(path, origin));
            }
        }
    }
    for root in crate::platform::fixed_drive_roots() {
        for name in ["SteamLibrary", "Steam", "Program Files (x86)/Steam", "Program Files/Steam"] {
            if root.join(name).join("steamapps").is_dir() {
                sources.library_roots.push(root.join(name));
            }
        }
        for name in ["GOG Games", "Games", "Program Files (x86)/GOG Galaxy/Games", "Program Files (x86)/LucasArts"] {
            sources.common_roots.push(root.join(name));
        }
    }
    if let Some(value) = std::env::var_os("KOTOR_INSTALL").filter(|value| !value.is_empty()) {
        sources.installs.push(PathSource::new(value, "environment"));
    }
    sources
}

#[cfg(test)]
pub mod tests {
    use super::*;
    use crate::testutil::{work_dir, write_bytes, write_text};

    fn put32(out: &mut [u8], at: usize, value: u32) {
        out[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }

    /// A KEY V1 file naming the given archives, each declared as `size` bytes.
    pub fn make_key(bif_names: &[&str], size: u32) -> Vec<u8> {
        let mut names_at = 64 + bif_names.len() * 12;
        let names_length: usize = bif_names.iter().map(|name| name.len() + 1).sum();
        let keys_at = names_at + names_length;
        let mut key = vec![0u8; keys_at + 22];
        key[..8].copy_from_slice(b"KEY V1  ");
        put32(&mut key, 8, bif_names.len() as u32);
        put32(&mut key, 12, 1);
        put32(&mut key, 16, 64);
        put32(&mut key, 20, keys_at as u32);
        for (index, name) in bif_names.iter().enumerate() {
            put32(&mut key, 64 + index * 12, size);
            put32(&mut key, 64 + index * 12 + 4, names_at as u32);
            key[64 + index * 12 + 8..64 + index * 12 + 10].copy_from_slice(&((name.len() + 1) as u16).to_le_bytes());
            key[names_at..names_at + name.len()].copy_from_slice(name.as_bytes());
            names_at += name.len() + 1;
        }
        key
    }

    pub fn make_tlk(language: u32, count: u32) -> Vec<u8> {
        let mut tlk = vec![0u8; 20 + count as usize * 40 + 8];
        tlk[..8].copy_from_slice(b"TLK V3.0");
        put32(&mut tlk, 8, language);
        put32(&mut tlk, 12, count);
        put32(&mut tlk, 16, 20 + count * 40);
        tlk
    }

    /// A synthetic KotOR 1 shaped data set. There is no executable in it.
    pub fn make_k1(root: &Path, patch: bool, bifs: bool) -> PathBuf {
        let _ = fs::remove_dir_all(root);
        for dir in ["modules", "rims", "lips", "TexturePacks", "streammusic", "streamwaves", "data"] {
            fs::create_dir_all(root.join(dir)).unwrap();
        }
        write_bytes(&root.join("chitin.key"), &make_key(&["data\\one.bif", "data\\two.bif"], 16));
        write_bytes(&root.join("dialog.tlk"), &make_tlk(0, 3));
        if patch {
            write_text(&root.join("patch.erf"), "ERF V1.0");
        }
        if bifs {
            write_bytes(&root.join("data").join("one.bif"), &[1u8; 16]);
            write_bytes(&root.join("data").join("two.bif"), &[2u8; 16]);
        }
        root.to_path_buf()
    }

    fn k1() -> &'static GameDef {
        find_game("kotor1").unwrap()
    }

    #[test]
    fn fixture_without_executable_validates() {
        let work = work_dir("validate_good");
        let good = make_k1(&work.join("k1"), true, true);
        let report = validate_install(&good, k1(), true);
        assert!(report.valid);
        assert!(!good.join("swkotor.exe").exists());
        assert_eq!((report.bif_count, report.key_entries), (2, 1));
        assert_eq!(report.language_id, Some(0));
        assert_eq!(report.language_name, "English");
        assert_eq!(report.tlk_strings, 3);
        assert!(!report.recognized && report.has("unknown_dataset"));
        assert_eq!(report.count(Severity::Error), 0);
        assert!(!report.modded());
        assert!(format_report(&report).contains("usable"));

        let mut custom = k1().clone();
        custom.known = vec![KnownDataSet {
            key_sha256: sha256_file(&good.join("chitin.key")),
            id: "fixture-set".into(),
            version: "9.9".into(),
            language_id: 0,
        }];
        let report = validate_install(&good, &custom, true);
        assert!(report.recognized && report.dataset_id == "fixture-set" && report.version == "9.9");
        assert!(!report.has("unknown_dataset"));
        custom.known[0].language_id = 1;
        assert!(validate_install(&good, &custom, true).has("language_mismatch"));
    }

    #[test]
    fn missing_pieces_reject() {
        let work = work_dir("validate_missing");
        let no_patch = make_k1(&work.join("nopatch"), false, true);
        let report = validate_install(&no_patch, k1(), true);
        assert!(!report.valid && report.has("missing_file"));
        let no_bif = make_k1(&work.join("nobif"), true, false);
        let report = validate_install(&no_bif, k1(), true);
        assert!(!report.valid && report.has("no_bif") && report.has("missing_bif"));
        fs::create_dir_all(work.join("empty")).unwrap();
        assert!(!validate_install(&work.join("empty"), k1(), true).valid);
        assert!(!validate_install(&work.join("does_not_exist"), k1(), true).valid);
    }

    #[test]
    fn broken_headers_are_errors_not_crashes() {
        let work = work_dir("validate_cut");
        let cut = make_k1(&work.join("k1"), true, true);
        let mut key = make_key(&["data\\one.bif"], 16);
        key.truncate(40);
        write_bytes(&cut.join("chitin.key"), &key);
        assert!(validate_install(&cut, k1(), true).has("key_format"));
        write_text(
            &cut.join("chitin.key"),
            "not a key file, not even close to one, padded to be long enough to pass 64 bytes.....",
        );
        assert!(validate_install(&cut, k1(), true).has("key_format"));
        write_text(&cut.join("dialog.tlk"), "short");
        assert!(validate_install(&cut, k1(), true).has("tlk_format"));
    }

    #[test]
    fn resized_and_modded_installs_warn() {
        let work = work_dir("validate_warn");
        let patched = make_k1(&work.join("resized"), true, true);
        write_bytes(&patched.join("data").join("one.bif"), &[1u8; 40]);
        let report = validate_install(&patched, k1(), true);
        assert!(report.valid && report.has("bif_size"));

        let modded = make_k1(&work.join("modded"), true, true);
        write_text(&modded.join("Override").join("x.tga"), "x");
        write_text(&modded.join("Override").join("y.2da"), "y");
        let report = validate_install(&modded, k1(), true);
        assert!(report.valid && report.modded() && report.override_files == 2 && report.has("modded"));
        assert!(modded.join("Override").join("x.tga").exists());
    }

    #[test]
    fn mixed_case_folders_validate() {
        let work = work_dir("validate_case");
        let mixed = make_k1(&work.join("k1"), true, true);
        fs::rename(mixed.join("modules"), mixed.join("Modules_tmp")).unwrap();
        fs::rename(mixed.join("Modules_tmp"), mixed.join("MODULES")).unwrap();
        fs::rename(mixed.join("data"), mixed.join("Data_tmp")).unwrap();
        fs::rename(mixed.join("Data_tmp"), mixed.join("DATA")).unwrap();
        assert!(validate_install(&mixed, k1(), true).valid);
    }

    #[test]
    fn identify_and_detect() {
        let work = work_dir("detect");
        let k1_root = make_k1(&work.join("id_k1"), true, true);
        assert_eq!(identify_install(&k1_root).map(|game| game.id), Some("kotor1"));

        let k2 = work.join("id_k2");
        fs::create_dir_all(k2.join("modules")).unwrap();
        fs::create_dir_all(k2.join("data")).unwrap();
        write_bytes(&k2.join("chitin.key"), &make_key(&["data\\a.bif"], 8));
        write_bytes(&k2.join("dialog.tlk"), &make_tlk(0, 1));
        write_bytes(&k2.join("data").join("a.bif"), &[0u8; 8]);
        let found = identify_install(&k2).unwrap();
        assert!(found.id == "kotor2" && !found.launchable);
        assert!(identify_install(&work.join("id_none")).is_none());

        // libraryfolders.vdf in the main root lists a second library; the appmanifest names the folder.
        let steam = work.join("steam_main");
        let lib2 = work.join("steam_lib2");
        let vdf = format!(
            "\"libraryfolders\"\n{{\n\t\"0\"\n\t{{\n\t\t\"path\"\t\t\"{}\"\n\t}}\n\t\"1\"\n\t{{\n\t\t\"path\"\t\t\"{}\"\n\t}}\n}}\n",
            generic(&steam),
            generic(&lib2)
        );
        write_text(&steam.join("steamapps").join("libraryfolders.vdf"), &vdf);
        write_text(
            &lib2.join("steamapps").join("appmanifest_32370.acf"),
            "\"AppState\"\n{\n\t\"appid\"\t\t\"32370\"\n\t\"installdir\"\t\t\"custom folder\"\n}\n",
        );
        let custom = make_k1(&lib2.join("steamapps").join("common").join("custom folder"), true, true);

        let mut sources = DetectionSources { steam_roots: vec![steam], ..Default::default() };
        let detected = detect_installs(&sources);
        assert_eq!(detected.len(), 1);
        assert!(detected[0].game_id == "kotor1" && detected[0].origin == "steam" && detected[0].report.valid);

        let extra = make_k1(&work.join("extra_install"), true, true);
        sources.installs = vec![
            PathSource::new(custom, "saved"),
            PathSource::new(extra, "browsed"),
            PathSource::new(work.join("nowhere"), "registry"),
            PathSource::new(work.join("id_none"), "gog"),
        ];
        let detected = detect_installs(&sources);
        assert_eq!(detected.len(), 2, "duplicates collapse, missing folders are skipped");
        assert!(detected[1].origin == "browsed" && detected[1].report.valid);

        make_k1(&work.join("common_root").join("swkotor"), true, true);
        let common = DetectionSources { common_roots: vec![work.join("common_root")], ..Default::default() };
        let detected = detect_installs(&common);
        assert!(detected.len() == 1 && detected[0].origin == "common");

        let machine = system_sources();
        assert!(machine.steam_roots.len() < 100);
    }

    #[test]
    fn vdf_and_manifest_parsers() {
        let libs = parse_library_folders("\"path\"\t\t\"D:\\\\Games\\\\Lib\"\n\"path\" \"E:/Other\"\n\"label\" \"x\"");
        assert_eq!(libs.len(), 2);
        assert_eq!(generic(&libs[0]), "D:/Games/Lib");
        assert_eq!(generic(&libs[1]), "E:/Other");
        // A real libraryfolders.vdf shape, with nested apps blocks and a contentid.
        let real = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n\t\t\"label\"\t\t\"\"\n\t\t\"contentid\"\t\t\"123\"\n\t\t\"apps\"\n\t\t{\n\t\t\t\"228980\"\t\t\"1\"\n\t\t}\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"E:\\\\SteamLibrary\"\n\t}\n}\n";
        let libs = parse_library_folders(real);
        assert_eq!(libs, vec![PathBuf::from("C:\\Program Files (x86)\\Steam"), PathBuf::from("E:\\SteamLibrary")]);
        assert!(parse_library_folders("").is_empty());
        assert!(parse_library_folders("\"path\"").is_empty());
        assert_eq!(parse_library_folders("\"path\" \"unterminated"), vec![PathBuf::from("unterminated")]);
        assert_eq!(parse_install_dir("\"installdir\" \"abc def\""), "abc def");
        assert!(parse_install_dir("nothing").is_empty());
        assert!(parse_install_dir("\"installdir\" \"open").is_empty());
    }

    #[test]
    fn languages() {
        assert_eq!(language_id("french"), Some(1));
        assert_eq!(language_id("12"), Some(12));
        assert_eq!(language_id("13"), None);
        assert_eq!(language_id("klingon"), None);
        assert_eq!(language_name(99), "Unknown");
    }

    /// Reads a real install when one exists (read-only) to prove a real data set validates without the
    /// executable.
    #[test]
    fn real_install_if_present() {
        let candidates: Vec<PathBuf> = std::env::var_os("KOTOR_INSTALL")
            .map(PathBuf::from)
            .into_iter()
            .chain([PathBuf::from("E:/SteamLibrary/steamapps/common/swkotor")])
            .collect();
        let Some(real) = candidates.into_iter().find(|path| path.join("chitin.key").is_file()) else {
            eprintln!("SKIP real install checks: no install found");
            return;
        };
        let report = validate_install(&real, k1(), true);
        assert!(report.valid, "{}", format_report(&report));
        assert!(report.recognized && report.dataset_id == "kotor1-pc-1.0.3.0-english");
        assert!(report.language_id == Some(0) && report.tlk_strings > 1000);
        assert_eq!(identify_install(&real).map(|game| game.id), Some("kotor1"));
    }
}
