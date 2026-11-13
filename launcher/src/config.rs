//! Launcher state and the options model, ported from `engine/src/launcher/LauncherConfig.*`.
//!
//! Two files are involved. The engine's own settings file (`settings.ini`, see [`crate::settings`]) holds
//! the in-game options. The launcher file (`launcher.ini`, text `key=value`) holds what only the launcher
//! needs: which install, the backend, the optional native font, the saves folder. Both are written
//! atomically and never while the engine runs (the launcher saves, starts the engine and exits).

use std::collections::BTreeMap;
use std::io;
use std::path::{Path, PathBuf};

use crate::install::{Severity, find_game, language_id, validate_install};
use crate::paths::{from_text, generic};
use crate::settings::{LINE_END, write_atomic};

#[derive(Debug, Clone, Default, PartialEq)]
pub struct LauncherConfig {
    values: BTreeMap<String, String>,
}

fn is_c_space(ch: char) -> bool {
    matches!(ch, ' ' | '\t' | '\n' | '\x0b' | '\x0c' | '\r')
}

impl LauncherConfig {
    /// A missing file is an empty config; unknown keys are kept and written back.
    pub fn load(path: &Path) -> Self {
        let mut config = Self::default();
        let Ok(bytes) = std::fs::read(path) else {
            return config;
        };
        let text = String::from_utf8_lossy(&bytes);
        for line in text.split('\n') {
            let line = line.trim_matches(is_c_space);
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            match line.find('=') {
                None | Some(0) => continue,
                Some(equals) => {
                    let key = line[..equals].trim_matches(is_c_space).to_owned();
                    let value = line[equals + 1..].trim_matches(is_c_space).to_owned();
                    config.values.insert(key, value);
                }
            }
        }
        config
    }

    pub fn to_bytes(&self) -> Vec<u8> {
        let mut out = String::from("# Launcher settings. Edited by the launcher; unknown keys are kept.");
        out.push_str(LINE_END);
        for (key, value) in &self.values {
            out.push_str(key);
            out.push('=');
            out.push_str(value);
            out.push_str(LINE_END);
        }
        out.into_bytes()
    }

    pub fn save(&self, path: &Path) -> io::Result<()> {
        write_atomic(path, &self.to_bytes())
    }

    pub fn get(&self, key: &str) -> &str {
        self.values.get(key).map_or("", String::as_str)
    }

    pub fn get_or<'a>(&'a self, key: &str, fallback: &'a str) -> &'a str {
        self.values.get(key).map_or(fallback, String::as_str)
    }

    /// Values cannot hold line breaks.
    pub fn set(&mut self, key: &str, value: &str) {
        let clean: String = value.chars().filter(|ch| *ch != '\n' && *ch != '\r').collect();
        self.values.insert(key.to_owned(), clean);
    }

    pub fn get_bool(&self, key: &str, fallback: bool) -> bool {
        match self.get(key).to_ascii_lowercase().as_str() {
            "1" | "true" | "yes" => true,
            "0" | "false" | "no" => false,
            _ => fallback,
        }
    }

    #[cfg(test)]
    pub fn get_int(&self, key: &str, fallback: i32) -> i32 {
        self.get(key).parse().unwrap_or(fallback)
    }

    /// Folders the player pointed at by hand, remembered across runs (`extra.0`, `extra.1`, ...).
    pub fn extra_installs(&self) -> Vec<String> {
        (0..)
            .map(|index| self.get(&format!("extra.{index}")).to_owned())
            .take_while(|value| !value.is_empty())
            .collect()
    }

    pub fn add_extra_install(&mut self, path: &str) {
        let existing = self.extra_installs();
        if !existing.iter().any(|known| known == path) {
            self.set(&format!("extra.{}", existing.len()), path);
        }
    }
}

/// Renderer choices the engine accepts (`--backend=`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Backend {
    Auto,
    Vulkan,
    Gl,
}

impl Backend {
    pub const ALL: [Backend; 3] = [Backend::Auto, Backend::Vulkan, Backend::Gl];

    pub fn name(self) -> &'static str {
        match self {
            Backend::Auto => "auto",
            Backend::Vulkan => "vulkan",
            Backend::Gl => "gl",
        }
    }

    pub fn parse(name: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|backend| backend.name() == name)
    }

    pub fn label(self) -> &'static str {
        match self {
            Backend::Auto => "Automatic",
            Backend::Vulkan => "Vulkan",
            Backend::Gl => "OpenGL",
        }
    }

    /// What the engine asks of the driver for this choice (engine/src/render: Vulkan 1.3 instance,
    /// OpenGL 4.5 core context; auto tries Vulkan, then falls back to OpenGL).
    pub fn api_summary(self) -> &'static str {
        match self {
            Backend::Auto => "Vulkan 1.3 or OpenGL 4.5",
            Backend::Vulkan => "Vulkan 1.3",
            Backend::Gl => "OpenGL 4.5",
        }
    }
}

/// What the player chose for one launch.
#[derive(Debug, Clone, PartialEq)]
pub struct LaunchChoices {
    pub game_id: String,
    pub install: PathBuf,
    /// The engine's settings.ini.
    pub settings_file: PathBuf,
    /// Empty: the engine's default.
    pub saves_dir: PathBuf,
    /// auto | vulkan | gl (kept as text so an unknown value is reported, as the C++ launcher does).
    pub backend: String,
    /// Empty: the install's own language.
    pub language: String,
    pub start_module: String,
    pub start_screen: String,
    /// Remaster options. Classic (the scaled original font) is the default; no font is bundled.
    pub native_font: bool,
    pub font_file: PathBuf,
}

impl Default for LaunchChoices {
    fn default() -> Self {
        Self {
            game_id: "kotor1".into(),
            install: PathBuf::new(),
            settings_file: PathBuf::new(),
            saves_dir: PathBuf::new(),
            backend: "auto".into(),
            language: String::new(),
            start_module: "end_m01aa".into(),
            start_screen: "mainmenu".into(),
            native_font: false,
            font_file: PathBuf::new(),
        }
    }
}

impl LaunchChoices {
    pub fn from_config(config: &LauncherConfig) -> Self {
        let defaults = Self::default();
        let game_id = config.get_or("game", &defaults.game_id).to_owned();
        Self {
            install: from_text(config.get(&format!("install.{game_id}"))),
            settings_file: from_text(config.get("settings.file")),
            saves_dir: from_text(config.get("saves.dir")),
            backend: config.get_or("backend", &defaults.backend).to_owned(),
            language: config.get("language").to_owned(),
            start_module: config.get_or("start.module", &defaults.start_module).to_owned(),
            start_screen: config.get_or("start.screen", &defaults.start_screen).to_owned(),
            native_font: config.get_bool("remaster.native_font", false),
            font_file: from_text(config.get("remaster.font_file")),
            game_id,
        }
    }

    pub fn to_config(&self, config: &mut LauncherConfig) {
        config.set("game", &self.game_id);
        config.set(&format!("install.{}", self.game_id), &generic(&self.install));
        config.set("backend", &self.backend);
        config.set("language", &self.language);
        config.set("saves.dir", &generic(&self.saves_dir));
        config.set("settings.file", &generic(&self.settings_file));
        config.set("start.module", &self.start_module);
        config.set("start.screen", &self.start_screen);
        config.set("remaster.native_font", if self.native_font { "1" } else { "0" });
        config.set("remaster.font_file", &generic(&self.font_file));
    }
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct LaunchPlan {
    /// `args[0]` is the engine executable.
    pub args: Vec<String>,
    /// Anything here blocks the launch.
    pub problems: Vec<String>,
}

impl LaunchPlan {
    pub fn ok(&self) -> bool {
        self.problems.is_empty()
    }
}

pub fn is_font_file(path: &Path) -> bool {
    path.extension()
        .map(|ext| ext.to_string_lossy().to_ascii_lowercase())
        .is_some_and(|ext| matches!(ext.as_str(), "ttf" | "otf" | "ttc"))
}

/// Checks the choices against the install and builds the engine command line (`buildLaunchPlan`). Reads
/// the install, never writes it.
pub fn build_launch_plan(choices: &LaunchChoices, engine: &Path) -> LaunchPlan {
    let mut plan = LaunchPlan::default();
    let Some(game) = find_game(&choices.game_id) else {
        plan.problems.push(format!("Unknown game: {}", choices.game_id));
        return plan;
    };
    if !game.launchable {
        plan.problems.push(format!("{} is detected but the engine cannot play it yet", game.name));
    }
    if choices.install.as_os_str().is_empty() {
        plan.problems.push("No install folder is chosen".into());
    } else {
        let report = validate_install(&choices.install, game, false);
        if !report.valid {
            plan.problems.extend(
                report
                    .issues
                    .iter()
                    .filter(|issue| issue.severity == Severity::Error)
                    .map(|issue| issue.message.clone()),
            );
        }
    }
    if Backend::parse(&choices.backend).is_none() {
        plan.problems.push(format!("Unknown backend: {}", choices.backend));
    }
    if !choices.language.is_empty() && language_id(&choices.language).is_none() {
        plan.problems.push(format!("Unknown language: {}", choices.language));
    }
    if choices.settings_file.as_os_str().is_empty() {
        plan.problems.push("No settings file is chosen".into());
    }
    if choices.native_font {
        if choices.font_file.as_os_str().is_empty() || !choices.font_file.is_file() {
            plan.problems.push("The native font file does not exist".into());
        } else if !is_font_file(&choices.font_file) {
            plan.problems.push("The native font must be a .ttf, .otf or .ttc file".into());
        }
    }
    if !engine.is_file() {
        plan.problems.push(format!("The engine executable was not found: {}", generic(engine)));
    }
    if !plan.ok() {
        return plan;
    }

    plan.args = vec![
        generic(engine),
        "--install".into(),
        generic(&choices.install),
        "--settings-file".into(),
        generic(&choices.settings_file),
    ];
    if !choices.saves_dir.as_os_str().is_empty() {
        plan.args.push("--saves-dir".into());
        plan.args.push(generic(&choices.saves_dir));
    }
    plan.args.push(format!("--backend={}", choices.backend));
    if !choices.language.is_empty() {
        plan.args.push("--language".into());
        plan.args.push(choices.language.clone());
    }
    if !choices.start_module.is_empty() {
        plan.args.push("--module".into());
        plan.args.push(choices.start_module.clone());
    }
    if !choices.start_screen.is_empty() {
        plan.args.push("--gui-screen".into());
        plan.args.push(choices.start_screen.clone());
    }
    if choices.native_font {
        plan.args.push("--ui-font".into());
        plan.args.push(generic(&choices.font_file));
    }
    plan
}

/// Font files in the given folders (non-recursive), sorted by file name ignoring case.
pub fn scan_fonts(folders: &[PathBuf]) -> Vec<PathBuf> {
    let mut fonts: Vec<PathBuf> = folders
        .iter()
        .filter_map(|folder| std::fs::read_dir(folder).ok())
        .flat_map(|entries| entries.flatten())
        .map(|entry| entry.path())
        .filter(|path| path.is_file() && is_font_file(path))
        .collect();
    fonts.sort_by_key(|path| path.file_name().map(|name| name.to_string_lossy().to_lowercase()));
    fonts
}

/// The system and per-user font folders.
pub fn system_font_folders() -> Vec<PathBuf> {
    let mut folders = Vec::new();
    if cfg!(windows) {
        if let Some(windows) = std::env::var_os("WINDIR") {
            folders.push(PathBuf::from(windows).join("Fonts"));
        }
        if let Some(local) = std::env::var_os("LOCALAPPDATA") {
            folders.push(PathBuf::from(local).join("Microsoft").join("Windows").join("Fonts"));
        }
    } else {
        folders.push(PathBuf::from("/usr/share/fonts"));
        folders.push(PathBuf::from("/usr/local/share/fonts"));
        if let Some(home) = std::env::var_os("HOME") {
            folders.push(PathBuf::from(&home).join(".fonts"));
            folders.push(PathBuf::from(home).join(".local").join("share").join("fonts"));
        }
    }
    folders
}

// ---- options screen model -------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OptionKind {
    Toggle,
    Slider,
    Choice(&'static [&'static str]),
    Integer,
}

#[derive(Debug, Clone, Copy)]
pub struct OptionDef {
    /// A settings.ini key.
    pub key: &'static str,
    /// Section of the options screen.
    pub group: &'static str,
    pub label: &'static str,
    pub kind: OptionKind,
    pub minimum: f32,
    pub maximum: f32,
}

const fn opt(
    key: &'static str,
    group: &'static str,
    label: &'static str,
    kind: OptionKind,
    minimum: f32,
    maximum: f32,
) -> OptionDef {
    OptionDef { key, group, label, kind, minimum, maximum }
}

use OptionKind::{Integer, Slider, Toggle};

/// Every engine setting the launcher offers, in screen order (`engineOptions()`).
pub const ENGINE_OPTIONS: &[OptionDef] = &[
    opt("graphics.fullscreen", "Display", "Fullscreen (borderless)", Toggle, 0.0, 1.0),
    opt("graphics.width", "Display", "Window width (0: default)", Integer, 0.0, 7680.0),
    opt("graphics.height", "Display", "Window height (0: default)", Integer, 0.0, 4320.0),
    opt("graphics.vsync", "Display", "Vertical sync", Toggle, 0.0, 1.0),
    opt("graphics.gamma", "Display", "Gamma", Slider, 0.0, 100.0),
    opt("graphics.shadows", "Display", "Shadows", Toggle, 0.0, 1.0),
    opt("graphics.softshadows", "Display", "Soft shadows", Toggle, 0.0, 1.0),
    opt("graphics.grass", "Display", "Grass", Toggle, 0.0, 1.0),
    opt("graphics.framebuffer", "Display", "Framebuffer effects", Toggle, 0.0, 1.0),
    opt("sound.music", "Sound", "Music volume", Slider, 0.0, 100.0),
    opt("sound.voice", "Sound", "Voice volume", Slider, 0.0, 100.0),
    opt("sound.effects", "Sound", "Effects volume", Slider, 0.0, 100.0),
    opt("sound.movie", "Sound", "Movie volume", Slider, 0.0, 100.0),
    opt("sound.forcesoftware", "Sound", "Force software mixing", Toggle, 0.0, 1.0),
    opt("gameplay.difficulty", "Gameplay", "Difficulty", OptionKind::Choice(&["Easy", "Normal", "Hard"]), 0.0, 2.0),
    opt("gameplay.autosave", "Gameplay", "Autosave", Toggle, 0.0, 1.0),
    opt("gameplay.autolevelup", "Gameplay", "Automatic level-up", Toggle, 0.0, 1.0),
    opt("gameplay.invertcamera", "Gameplay", "Invert camera", Toggle, 0.0, 1.0),
    opt("gameplay.reversecamera", "Gameplay", "Reverse camera controls", Toggle, 0.0, 1.0),
    opt("gameplay.disablemouse", "Gameplay", "Disable mouse movement", Toggle, 0.0, 1.0),
    opt("mouse.sensitivity", "Mouse", "Mouse sensitivity", Slider, 0.0, 100.0),
    opt("mouse.reversebuttons", "Mouse", "Swap mouse buttons", Toggle, 0.0, 1.0),
    opt("feedback.subtitles", "Feedback", "Subtitles", Toggle, 0.0, 1.0),
    opt("feedback.tooltips", "Feedback", "Tooltips", Toggle, 0.0, 1.0),
    opt("feedback.minimap", "Feedback", "Minimap", Toggle, 0.0, 1.0),
    opt("feedback.floatingnumbers", "Feedback", "Floating numbers", Toggle, 0.0, 1.0),
    opt("feedback.tutorialpopups", "Feedback", "Tutorial pop-ups", Toggle, 0.0, 1.0),
    opt("feedback.statussummary", "Feedback", "Status summary", Toggle, 0.0, 1.0),
    opt("feedback.hidequickmenu", "Feedback", "Hide quick menu", Toggle, 0.0, 1.0),
    opt("feedback.smallfont", "Feedback", "Small interface font", Toggle, 0.0, 1.0),
    opt("feedback.hideunequippable", "Feedback", "Hide unequippable items", Toggle, 0.0, 1.0),
    opt("autopause.enemysighted", "Auto-pause", "Enemy sighted", Toggle, 0.0, 1.0),
    opt("autopause.partykilled", "Auto-pause", "Party member down", Toggle, 0.0, 1.0),
    opt("autopause.actionmenu", "Auto-pause", "Action menu opened", Toggle, 0.0, 1.0),
    opt("autopause.endround", "Auto-pause", "End of combat round", Toggle, 0.0, 1.0),
    opt("autopause.triggers", "Auto-pause", "Triggers", Toggle, 0.0, 1.0),
    opt("autopause.minesighted", "Auto-pause", "Mine sighted", Toggle, 0.0, 1.0),
];

/// The resolution the engine opens with for these settings (Application.cpp: fullscreen wins; a stored
/// size of at least 640x480 is used; otherwise the 1280x720 default window).
pub fn resolution_summary(settings: &crate::settings::UserSettings) -> String {
    if settings.get("graphics.fullscreen") != 0.0 {
        return "Fullscreen, desktop size".into();
    }
    let width = settings.get("graphics.width");
    let height = settings.get("graphics.height");
    if width >= 640.0 && height >= 480.0 {
        format!("{}x{} windowed", width as i32, height as i32)
    } else {
        "1280x720 windowed (default)".into()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::install::tests::make_k1;
    use crate::settings::{DEFAULTS, UserSettings};
    use crate::testutil::{work_dir, write_text};
    use std::collections::HashSet;

    #[test]
    fn config_round_trip() {
        let work = work_dir("config");
        let file = work.join("launcher.ini");
        assert!(LauncherConfig::load(&file).get("game").is_empty());

        let mut config = LauncherConfig::default();
        config.set("unknown.key", "kept");
        config.set("note", "line\nbreak");
        let choices = LaunchChoices {
            game_id: "kotor1".into(),
            install: work.join("k1_good"),
            settings_file: work.join("settings.ini"),
            saves_dir: work.join("saves"),
            backend: "gl".into(),
            language: "english".into(),
            native_font: true,
            font_file: work.join("fonts").join("some font.ttf"),
            ..LaunchChoices::default()
        };
        choices.to_config(&mut config);
        config.add_extra_install("D:/a");
        config.add_extra_install("D:/b");
        config.add_extra_install("D:/a");
        config.save(&file).unwrap();
        assert!(!work.join("launcher.ini.tmp").exists());

        let back = LauncherConfig::load(&file);
        assert_eq!(back.get("unknown.key"), "kept");
        assert_eq!(back.get("note"), "linebreak");
        let read = LaunchChoices::from_config(&back);
        assert_eq!(generic(&read.install), generic(&choices.install));
        assert_eq!(generic(&read.font_file), generic(&choices.font_file));
        assert!(read.backend == "gl" && read.language == "english" && read.native_font);
        assert_eq!(back.extra_installs(), vec!["D:/a".to_owned(), "D:/b".to_owned()]);
        assert!(back.get_bool("remaster.native_font", false) && back.get_int("missing", 7) == 7);
        // The file is the C++ format: header comment, sorted keys, platform line ends.
        let text = String::from_utf8(std::fs::read(&file).unwrap()).unwrap();
        assert!(text.starts_with(&format!(
            "# Launcher settings. Edited by the launcher; unknown keys are kept.{LINE_END}backend=gl{LINE_END}"
        )));

        let fresh = LaunchChoices::from_config(&LauncherConfig::default());
        assert!(!fresh.native_font && fresh.font_file.as_os_str().is_empty() && fresh.backend == "auto");
    }

    #[test]
    fn config_parsing_tolerates_comments_and_spaces() {
        let work = work_dir("config_parse");
        let file = work.join("launcher.ini");
        write_text(&file, "# c\r\n  game = kotor2 \r\n=novalue\r\nnoequals\r\n\r\nbackend=vulkan");
        let config = LauncherConfig::load(&file);
        assert_eq!(config.get("game"), "kotor2");
        assert_eq!(config.get("backend"), "vulkan");
        assert_eq!(config.get(""), "");
    }

    #[test]
    fn launch_plan_matches_cpp_command_line() {
        let work = work_dir("plan");
        let good = make_k1(&work.join("plan_k1"), true, true);
        let engine = work.join("plan_engine.exe");
        write_text(&engine, "stub");
        write_text(&work.join("plan_font.ttf"), "stub");
        write_text(&work.join("plan_font.txt"), "stub");

        let mut choices = LaunchChoices {
            install: good.clone(),
            settings_file: work.join("plan_settings.ini"),
            saves_dir: work.join("plan_saves"),
            ..LaunchChoices::default()
        };
        let plan = build_launch_plan(&choices, &engine);
        assert!(plan.ok(), "{:?}", plan.problems);
        assert_eq!(
            plan.args,
            vec![
                generic(&engine),
                "--install".into(),
                generic(&good),
                "--settings-file".into(),
                generic(&work.join("plan_settings.ini")),
                "--saves-dir".into(),
                generic(&work.join("plan_saves")),
                "--backend=auto".into(),
                "--module".into(),
                "end_m01aa".into(),
                "--gui-screen".into(),
                "mainmenu".into(),
            ]
        );
        assert!(plan.args.iter().all(|arg| !arg.contains('\\')));

        choices.native_font = true;
        choices.font_file = work.join("plan_font.ttf");
        choices.backend = "vulkan".into();
        choices.language = "french".into();
        choices.saves_dir = PathBuf::new();
        let plan = build_launch_plan(&choices, &engine);
        assert!(plan.ok());
        assert_eq!(
            plan.args[5..],
            [
                "--backend=vulkan".to_owned(),
                "--language".into(),
                "french".into(),
                "--module".into(),
                "end_m01aa".into(),
                "--gui-screen".into(),
                "mainmenu".into(),
                "--ui-font".into(),
                generic(&work.join("plan_font.ttf")),
            ]
        );

        choices.font_file = work.join("plan_font.txt");
        assert!(!build_launch_plan(&choices, &engine).ok());
        choices.font_file = work.join("missing.ttf");
        assert!(!build_launch_plan(&choices, &engine).ok());
        choices.native_font = false;
        choices.backend = "directx".into();
        assert!(!build_launch_plan(&choices, &engine).ok());
        choices.backend = "auto".into();
        choices.language = "klingon".into();
        assert!(!build_launch_plan(&choices, &engine).ok());
        choices.language.clear();
        assert!(!build_launch_plan(&choices, &work.join("no_engine.exe")).ok());
        choices.install = work.join("missing_install");
        assert!(!build_launch_plan(&choices, &engine).ok());
        choices.install = PathBuf::new();
        assert!(!build_launch_plan(&choices, &engine).ok());
        choices.install = good;
        choices.game_id = "kotor2".into();
        let plan = build_launch_plan(&choices, &engine);
        assert!(plan.problems.iter().any(|problem| problem.contains("cannot play it yet")));
        choices.game_id = "nothing".into();
        assert_eq!(build_launch_plan(&choices, &engine).problems, vec!["Unknown game: nothing".to_owned()]);
    }

    #[test]
    fn every_engine_setting_has_an_option() {
        let display_only = ["graphics.fullscreen", "graphics.width", "graphics.height"];
        let mut seen = HashSet::new();
        for option in ENGINE_OPTIONS {
            assert!(DEFAULTS.iter().any(|(key, _)| *key == option.key) || display_only.contains(&option.key));
            assert!(seen.insert(option.key), "{} twice", option.key);
            assert!(option.maximum > option.minimum);
        }
        for (key, _) in DEFAULTS {
            assert!(seen.contains(key), "{key} has no option");
        }
    }

    #[test]
    fn fonts_scan_sorted() {
        let work = work_dir("fonts");
        let folder = work.join("fonts");
        for name in ["b.ttf", "A.OTF", "c.txt", "d.ttc"] {
            write_text(&folder.join(name), "x");
        }
        let fonts = scan_fonts(&[folder, work.join("no_such_folder")]);
        let names: Vec<_> = fonts.iter().map(|path| path.file_name().unwrap().to_string_lossy().into_owned()).collect();
        assert_eq!(names, ["A.OTF", "b.ttf", "d.ttc"]);
        assert!(
            is_font_file(Path::new("x.TTF")) && !is_font_file(Path::new("x.woff")) && !is_font_file(Path::new("x"))
        );
    }

    #[test]
    fn resolution_text_follows_engine_rules() {
        let mut settings = UserSettings::default();
        assert_eq!(resolution_summary(&settings), "1280x720 windowed (default)");
        settings.set("graphics.width", 1920.0);
        settings.set("graphics.height", 1080.0);
        assert_eq!(resolution_summary(&settings), "1920x1080 windowed");
        settings.set("graphics.fullscreen", 1.0);
        assert_eq!(resolution_summary(&settings), "Fullscreen, desktop size");
    }
}
