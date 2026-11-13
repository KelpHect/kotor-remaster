//! The launcher's state and actions, apart from drawing (the `Launcher` struct of the C++ `main.cpp`).

use std::path::{Path, PathBuf};

use crate::config::{LaunchChoices, LaunchPlan, LauncherConfig, build_launch_plan};
use crate::install::{
    DetectedInstall, DetectionSources, PathSource, detect_installs, find_game, identify_install, system_sources,
};
use crate::paths::{exe_folder, from_text, generic, pref_folder};
use crate::settings::UserSettings;

/// Paths and switches from the command line. Test runs pass the first three so nothing reaches the
/// player's profile.
#[derive(Debug, Clone, Default)]
pub struct CommandLine {
    pub config_file: Option<PathBuf>,
    pub settings_file: Option<PathBuf>,
    pub saves_dir: Option<PathBuf>,
    pub engine: Option<PathBuf>,
    pub install: Option<PathBuf>,
    /// Skip the machine's registry, drives and environment (tests and screenshots of a fixture).
    pub no_system_scan: bool,
    /// Extra Steam root, used for detection and cached art (tests and screenshots).
    pub steam_root: Option<PathBuf>,
}

pub struct LauncherState {
    pub cl: CommandLine,
    pub config_path: PathBuf,
    pub settings_path: PathBuf,
    pub engine_path: PathBuf,
    pub config: LauncherConfig,
    pub settings: UserSettings,
    pub choices: LaunchChoices,
    pub installs: Vec<DetectedInstall>,
    pub selected: Option<usize>,
    pub status: String,
    pub config_dirty: bool,
}

impl LauncherState {
    /// Loads the config and settings; does not scan (see [`LauncherState::sources`]).
    pub fn new(cl: CommandLine) -> Self {
        let config_path = cl.config_file.clone().unwrap_or_else(|| pref_folder("launcher").join("launcher.ini"));
        let config = LauncherConfig::load(&config_path);
        let mut choices = LaunchChoices::from_config(&config);
        let settings_path = cl
            .settings_file
            .clone()
            .or_else(|| (!choices.settings_file.as_os_str().is_empty()).then(|| choices.settings_file.clone()))
            .unwrap_or_else(|| match &cl.config_file {
                // Share the engine's own file unless this run keeps its files elsewhere.
                None => pref_folder("engine").join("settings.ini"),
                Some(file) => file.parent().unwrap_or(Path::new(".")).join("settings.ini"),
            });
        choices.settings_file = settings_path.clone();
        if let Some(saves) = &cl.saves_dir {
            choices.saves_dir = saves.clone();
        }
        let engine_path = cl.engine.clone().unwrap_or_else(|| exe_folder().join(engine_file_name()));
        let mut settings = UserSettings::default();
        settings.load(&settings_path);
        settings.dirty = false;
        Self {
            cl,
            config_path,
            settings_path,
            engine_path,
            config,
            settings,
            choices,
            installs: Vec::new(),
            selected: None,
            status: String::new(),
            config_dirty: false,
        }
    }

    /// Where to look, in priority order (command line, saved choice, browsed folders, the machine).
    pub fn sources(&self) -> DetectionSources {
        let mut sources = if self.cl.no_system_scan { DetectionSources::default() } else { system_sources() };
        if let Some(steam) = &self.cl.steam_root {
            sources.steam_roots.insert(0, steam.clone());
        }
        for extra in self.config.extra_installs() {
            sources.installs.push(PathSource::new(from_text(&extra), "browsed"));
        }
        if !self.choices.install.as_os_str().is_empty() {
            sources.installs.insert(0, PathSource::new(self.choices.install.clone(), "saved"));
        }
        if let Some(install) = &self.cl.install {
            sources.installs.insert(0, PathSource::new(install.clone(), "command line"));
        }
        sources
    }

    /// Runs detection synchronously (the window runs [`detect_installs`] on a worker and calls
    /// [`LauncherState::apply_scan`]).
    pub fn rescan(&mut self) {
        let found = detect_installs(&self.sources());
        self.apply_scan(found);
    }

    pub fn apply_scan(&mut self, installs: Vec<DetectedInstall>) {
        self.installs = installs;
        let launchable = |install: &DetectedInstall| {
            install.report.valid && find_game(&install.game_id).is_some_and(|game| game.launchable)
        };
        let saved = (!self.choices.install.as_os_str().is_empty())
            .then(|| self.installs.iter().position(|install| same_path(&install.report.root, &self.choices.install)))
            .flatten();
        let selected = if self.cl.install.is_some() && !self.installs.is_empty() {
            Some(0)
        } else {
            saved
                .or_else(|| self.installs.iter().position(launchable))
                .or_else(|| (!self.installs.is_empty()).then_some(0))
        };
        if let Some(index) = selected {
            self.select(index);
        }
        self.selected = selected;
        self.status = if self.installs.is_empty() {
            "No install was found. Use Change to point at your game folder.".into()
        } else {
            String::new()
        };
    }

    pub fn select(&mut self, index: usize) {
        if let Some(install) = self.installs.get(index) {
            self.selected = Some(index);
            self.choices.game_id = install.game_id.clone();
            self.choices.install = install.report.root.clone();
        }
    }

    /// The selected install, if any.
    pub fn current(&self) -> Option<&DetectedInstall> {
        self.selected.and_then(|index| self.installs.get(index))
    }

    /// The install of a catalog game, if detected.
    pub fn install_of(&self, game_id: &str) -> Option<(usize, &DetectedInstall)> {
        self.installs.iter().enumerate().find(|(_, install)| install.game_id == game_id)
    }

    /// A folder the player browsed to. Returns false (and sets the status) when it is no known data set.
    pub fn add_folder(&mut self, folder: &Path) -> bool {
        let Some(game) = identify_install(folder) else {
            self.status = "That folder does not look like a supported game data set (no chitin.key).".into();
            return false;
        };
        self.config.add_extra_install(&generic(folder));
        self.choices.install = folder.to_path_buf();
        self.choices.game_id = game.id.to_owned();
        self.config_dirty = true;
        true
    }

    pub fn plan(&self) -> LaunchPlan {
        build_launch_plan(&self.choices, &self.engine_path)
    }

    pub fn save_all(&mut self) -> bool {
        let mut ok = true;
        self.choices.to_config(&mut self.config);
        if let Err(error) = self.config.save(&self.config_path) {
            self.status = format!("Could not write {}: {error}", generic(&self.config_path));
            ok = false;
        }
        if self.settings.dirty {
            match self.settings.save(&self.settings_path) {
                Ok(()) => self.settings.dirty = false,
                Err(error) => {
                    self.status = format!("Could not write {}: {error}", generic(&self.settings_path));
                    ok = false;
                }
            }
        }
        self.config_dirty = false;
        ok
    }

    pub fn unsaved(&self) -> bool {
        self.config_dirty || self.settings.dirty
    }

    /// Saves, then starts the engine with the plan's command line. The caller closes the launcher on
    /// success, so the settings file is never written by two processes.
    pub fn play(&mut self) -> bool {
        let plan = self.plan();
        if let Some(problem) = plan.problems.first() {
            self.status = problem.clone();
            return false;
        }
        if !self.save_all() {
            return false; // never start the engine on settings that did not save
        }
        match std::process::Command::new(&plan.args[0]).args(&plan.args[1..]).spawn() {
            Ok(_child) => true, // dropping the handle leaves the engine running on its own
            Err(error) => {
                self.status = format!("Could not start the engine: {error}");
                false
            }
        }
    }
}

pub fn engine_file_name() -> &'static str {
    if cfg!(windows) { "kotor_engine.exe" } else { "kotor_engine" }
}

fn same_path(a: &Path, b: &Path) -> bool {
    generic(a).eq_ignore_ascii_case(&generic(b))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::install::tests::make_k1;
    use crate::testutil::work_dir;

    #[test]
    fn state_uses_explicit_paths_and_selects_install() {
        let work = work_dir("state");
        let install = make_k1(&work.join("game"), true, true);
        let cl = CommandLine {
            config_file: Some(work.join("cfg").join("launcher.ini")),
            install: Some(install.clone()),
            no_system_scan: true,
            ..Default::default()
        };
        let mut state = LauncherState::new(cl);
        assert_eq!(state.settings_path, work.join("cfg").join("settings.ini"));
        state.rescan();
        assert_eq!(state.selected, Some(0));
        assert_eq!(state.installs[0].origin, "command line");
        assert_eq!(generic(&state.choices.install), generic(&install));
        state.settings.set("sound.music", 12.0);
        assert!(state.save_all());
        assert!(work.join("cfg").join("launcher.ini").is_file());
        assert!(work.join("cfg").join("settings.ini").is_file());
        assert!(!state.add_folder(&work.join("nothing_here")));
    }
}
