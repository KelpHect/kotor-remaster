//! Launcher: finds user-owned game installs, validates their data set (never the executable), holds the
//! engine and remaster options, and starts the engine with the choice. It reads the install and never
//! writes to it (ADR 0003). Neutral branding: no game artwork, text or font is shipped.
//!
//! ```text
//! kotor_launcher                         the window
//! kotor_launcher --list                  print detected installs and their validation, then exit
//! kotor_launcher --check <folder>        validate one folder (exit 0 usable, 2 not usable)
//! kotor_launcher --print-command         print the engine command line the window would start
//! ```
//!
//! Paths: `--config-file`, `--settings-file`, `--saves-dir`, `--engine`, `--install`. Test runs pass the
//! first three so nothing is written to the player's profile. Window: `--view home|settings`,
//! `--no-system-scan`, `--steam-root <dir>`, `--probe-file <file>` (writes the time to first render),
//! `--probe-exit` (quit after that first render).

#![cfg_attr(not(test), windows_subsystem = "windows")]

mod art;
mod assets;
mod config;
mod fontname;
mod install;
mod news;
mod paths;
mod platform;
mod settings;
mod state;
#[cfg(test)]
mod testutil;
mod theme;
mod ui;

use std::path::PathBuf;
use std::process::ExitCode;
use std::time::Instant;

use gpui::{App, AppContext, Application, Bounds, TitlebarOptions, WindowBounds, WindowOptions, point, px, size};

use crate::install::{format_report, identify_install, validate_install};
use crate::state::{CommandLine, LauncherState};
use crate::ui::{LauncherView, Probe, View};

#[derive(Debug, Default)]
struct Args {
    paths: CommandLine,
    list: bool,
    print_command: bool,
    check: Option<PathBuf>,
    view: Option<View>,
    probe_file: Option<PathBuf>,
    probe_exit: bool,
}

fn parse_args(args: impl IntoIterator<Item = String>) -> Result<Args, String> {
    let mut parsed = Args::default();
    let mut args = args.into_iter();
    while let Some(arg) = args.next() {
        let mut value = |name: &str| args.next().ok_or_else(|| format!("{name} needs a value"));
        match arg.as_str() {
            "--list" => parsed.list = true,
            "--print-command" => parsed.print_command = true,
            "--check" => parsed.check = Some(PathBuf::from(value("--check")?)),
            "--config-file" => parsed.paths.config_file = Some(PathBuf::from(value("--config-file")?)),
            "--settings-file" => parsed.paths.settings_file = Some(PathBuf::from(value("--settings-file")?)),
            "--saves-dir" => parsed.paths.saves_dir = Some(PathBuf::from(value("--saves-dir")?)),
            "--engine" => parsed.paths.engine = Some(PathBuf::from(value("--engine")?)),
            "--install" => parsed.paths.install = Some(PathBuf::from(value("--install")?)),
            "--steam-root" => parsed.paths.steam_root = Some(PathBuf::from(value("--steam-root")?)),
            "--no-system-scan" => parsed.paths.no_system_scan = true,
            "--view" => {
                parsed.view = Some(match value("--view")?.as_str() {
                    "home" => View::Home,
                    "settings" => View::Settings,
                    other => return Err(format!("unknown view: {other}")),
                })
            }
            "--probe-file" => parsed.probe_file = Some(PathBuf::from(value("--probe-file")?)),
            "--probe-exit" => parsed.probe_exit = true,
            other => return Err(format!("unknown argument: {other}")),
        }
    }
    Ok(parsed)
}

fn main() -> ExitCode {
    let started = Instant::now();
    let args = match parse_args(std::env::args().skip(1)) {
        Ok(args) => args,
        Err(error) => {
            platform::attach_parent_console();
            eprintln!("{error}");
            return ExitCode::from(2);
        }
    };
    if let Some(folder) = &args.check {
        platform::attach_parent_console();
        let Some(game) = identify_install(folder) else {
            println!("not a recognised game data set: {}", folder.display());
            return ExitCode::from(2);
        };
        let report = validate_install(folder, game, true);
        print!("{}", format_report(&report));
        return if report.valid { ExitCode::SUCCESS } else { ExitCode::from(2) };
    }
    if args.list || args.print_command {
        platform::attach_parent_console();
        let mut state = LauncherState::new(args.paths);
        state.rescan();
        if args.list {
            for install in &state.installs {
                println!("{}  found via: {}", format_report(&install.report), install.origin);
            }
            if state.installs.is_empty() {
                println!("no installs found");
            }
            return ExitCode::SUCCESS;
        }
        let plan = state.plan();
        for problem in &plan.problems {
            println!("problem: {problem}");
        }
        for arg in &plan.args {
            println!("{arg}");
        }
        return if plan.ok() { ExitCode::SUCCESS } else { ExitCode::from(2) };
    }
    run_window(args, started);
    ExitCode::SUCCESS
}

fn run_window(args: Args, started: Instant) {
    let state = LauncherState::new(args.paths);
    let view = args.view.unwrap_or(View::Home);
    let probe = args.probe_file.map(|file| Probe { started, file, exit: args.probe_exit, first_render_ms: None });
    Application::new().with_assets(assets::Assets).run(move |cx: &mut App| {
        let _ =
            cx.text_system().add_fonts(assets::FONTS.iter().map(|font| std::borrow::Cow::Borrowed(*font)).collect());
        // The mockup's window, 1536x850 logical pixels.
        let bounds = Bounds::centered(None, size(px(1536.0), px(850.0)), cx);
        let options = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(bounds)),
            titlebar: Some(TitlebarOptions {
                title: Some(format!("{} launcher", theme::PRODUCT_NAME).into()),
                appears_transparent: true,
                traffic_light_position: Some(point(px(12.0), px(12.0))),
            }),
            window_min_size: Some(size(px(1280.0), px(800.0))),
            app_id: Some("kotor-remaster-launcher".into()),
            ..Default::default()
        };
        let opened = cx.open_window(options, move |window, cx| {
            use raw_window_handle::{HasWindowHandle, RawWindowHandle};
            if let Ok(handle) = window.window_handle()
                && let RawWindowHandle::Win32(win32) = handle.as_raw()
            {
                platform::round_window(win32.hwnd.get(), theme::FRAME_RGB);
            }
            cx.new(|cx| LauncherView::new(state, view, probe, cx))
        });
        if opened.is_err() {
            cx.quit();
            return;
        }
        cx.on_window_closed(|cx| cx.quit()).detach();
        cx.activate(true);
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(list: &[&str]) -> Result<Args, String> {
        parse_args(list.iter().map(|arg| (*arg).to_owned()))
    }

    #[test]
    fn command_line() {
        let parsed =
            args(&["--list", "--config-file", "a/launcher.ini", "--view", "settings", "--no-system-scan"]).unwrap();
        assert!(parsed.list && parsed.paths.no_system_scan);
        assert_eq!(parsed.paths.config_file, Some(PathBuf::from("a/launcher.ini")));
        assert_eq!(parsed.view, Some(View::Settings));
        assert!(args(&["--check"]).is_err());
        assert!(args(&["--bogus"]).is_err());
        assert!(args(&["--view", "nowhere"]).is_err());
    }
}
