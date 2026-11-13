//! The launcher window (GPUI). Home: game rail, banner, details, play. Settings: every engine option and
//! the remaster and launch options. Nothing animates, so the window only redraws on input or state changes.

use std::cell::Cell;
use std::collections::HashMap;
use std::path::PathBuf;
use std::rc::Rc;
use std::time::Instant;

use gpui::{
    AnyElement, App, Bounds, BoxShadow, Context, FontWeight, IntoElement, MouseButton, MouseDownEvent, MouseMoveEvent,
    ObjectFit, PathPromptOptions, Pixels, Render, SharedString, Window, WindowControlArea, canvas, div, fill, img,
    linear_color_stop, linear_gradient, point, prelude::*, px, size, svg,
};

use crate::art::{GameArt, steam_cached_art};
use crate::assets::FONT_FAMILY;
use crate::config::{
    Backend, ENGINE_OPTIONS, OptionKind, is_font_file, resolution_summary, scan_fonts, system_font_folders,
};
use crate::install::{
    DetectedInstall, LANGUAGES, Severity, ValidationReport, detect_installs, find_game, game_catalog, validate_install,
};
use crate::news::{NewsItem, NewsSource, PlaceholderNews};
use crate::paths::generic;
use crate::settings::default_of;
use crate::state::LauncherState;
use crate::theme;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum View {
    Home,
    Settings,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Picker {
    Language,
    Font,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Info {
    About,
    News,
}

enum Verify {
    Running,
    Done(Box<ValidationReport>),
}

pub struct Probe {
    pub started: Instant,
    pub file: PathBuf,
    pub exit: bool,
    pub first_render_ms: Option<f64>,
}

pub struct LauncherView {
    state: LauncherState,
    view: View,
    scanning: bool,
    verify: Option<Verify>,
    info: Option<Info>,
    picker: Option<Picker>,
    fonts: Option<Vec<PathBuf>>,
    /// The native font loaded for the preview and its family name.
    preview: Option<(PathBuf, Option<SharedString>)>,
    renders: u64,
    art: HashMap<&'static str, GameArt>,
    news: Vec<NewsItem>,
    slider_bounds: HashMap<&'static str, Rc<Cell<Bounds<Pixels>>>>,
    dragging: Option<&'static str>,
    probe: Option<Probe>,
}

impl LauncherView {
    pub fn new(state: LauncherState, view: View, probe: Option<Probe>, cx: &mut Context<Self>) -> Self {
        let steam_root = state
            .cl
            .steam_root
            .clone()
            .or_else(|| if state.cl.no_system_scan { None } else { crate::platform::steam_path() });
        let art = game_catalog()
            .iter()
            .map(|game| {
                let art =
                    steam_root.as_deref().map(|root| steam_cached_art(root, game.steam_app_id)).unwrap_or_default();
                (game.id, art)
            })
            .collect();
        let mut this = Self {
            state,
            view,
            scanning: false,
            verify: None,
            info: None,
            picker: None,
            fonts: None,
            preview: None,
            renders: 0,
            art,
            news: PlaceholderNews.items(),
            slider_bounds: HashMap::new(),
            dragging: None,
            probe,
        };
        this.rescan(cx);
        this
    }

    /// Detection hashes and stats files, so it runs on a worker; the window shows at once.
    fn rescan(&mut self, cx: &mut Context<Self>) {
        self.scanning = true;
        let sources = self.state.sources();
        cx.spawn(async move |this, cx| {
            let found = cx.background_executor().spawn(async move { detect_installs(&sources) }).await;
            this.update(cx, |this, cx| {
                this.state.apply_scan(found);
                this.scanning = false;
                cx.notify();
            })
            .ok();
        })
        .detach();
    }

    fn verify_files(&mut self, cx: &mut Context<Self>) {
        let Some(install) = self.state.current() else {
            self.state.status = "Pick an install first.".into();
            cx.notify();
            return;
        };
        let Some(game) = find_game(&install.game_id) else {
            return;
        };
        let root = install.report.root.clone();
        self.verify = Some(Verify::Running);
        cx.notify();
        cx.spawn(async move |this, cx| {
            let report = cx.background_executor().spawn(async move { validate_install(&root, game, true) }).await;
            this.update(cx, |this, cx| {
                if let Some(index) = this.state.installs.iter().position(|install| install.report.root == report.root) {
                    this.state.installs[index].report = report.clone();
                }
                this.verify = Some(Verify::Done(Box::new(report)));
                cx.notify();
            })
            .ok();
        })
        .detach();
    }

    fn browse_install(&mut self, cx: &mut Context<Self>) {
        let receiver = cx.prompt_for_paths(PathPromptOptions {
            files: false,
            directories: true,
            multiple: false,
            prompt: Some("Choose the game folder".into()),
        });
        cx.spawn(async move |this, cx| {
            if let Ok(Ok(Some(paths))) = receiver.await
                && let Some(folder) = paths.into_iter().next()
            {
                this.update(cx, |this, cx| {
                    if this.state.add_folder(&folder) {
                        this.rescan(cx);
                    }
                    cx.notify();
                })
                .ok();
            }
        })
        .detach();
    }

    fn browse_file(&mut self, target: BrowseTarget, cx: &mut Context<Self>) {
        let directories = matches!(target, BrowseTarget::Saves);
        let prompt = matches!(target, BrowseTarget::Engine).then(|| "Choose kotor_engine.exe".into());
        let receiver =
            cx.prompt_for_paths(PathPromptOptions { files: !directories, directories, multiple: false, prompt });
        cx.spawn(async move |this, cx| {
            if let Ok(Ok(Some(paths))) = receiver.await
                && let Some(path) = paths.into_iter().next()
            {
                this.update(cx, |this, cx| {
                    match target {
                        BrowseTarget::Saves => this.state.choices.saves_dir = path,
                        BrowseTarget::Font => this.state.choices.font_file = path,
                        // Remembered at once in the launcher config, so the next start finds it.
                        BrowseTarget::Engine => this.state.set_engine(&path),
                    }
                    if !matches!(target, BrowseTarget::Engine) {
                        this.state.config_dirty = true;
                    }
                    cx.notify();
                })
                .ok();
            }
        })
        .detach();
    }

    fn play(&mut self, cx: &mut Context<Self>) {
        if self.state.play() {
            cx.quit();
        } else {
            cx.notify();
        }
    }

    fn set_option(&mut self, key: &'static str, value: f32, cx: &mut Context<Self>) {
        self.state.settings.set(key, value);
        cx.notify();
    }
}

#[derive(Debug, Clone, Copy)]
enum BrowseTarget {
    Saves,
    Font,
    Engine,
}

// ---- small building blocks ------------------------------------------------------------------------------------------

fn icon(name: &'static str, size_px: f32, color: impl Into<gpui::Hsla>) -> impl IntoElement {
    svg().path(SharedString::new_static(name)).size(px(size_px)).flex_none().text_color(color.into())
}

fn caps(text: &str, size_px: f32) -> gpui::Div {
    caps_n(text, size_px, 1)
}

/// Capitals with `level` steps of letter spacing (0: none).
fn caps_n(text: &str, size_px: f32, level: usize) -> gpui::Div {
    div().text_size(px(size_px)).whitespace_nowrap().child(spaced(&text.to_uppercase(), level))
}

fn spaced(text: &str, level: usize) -> String {
    theme::tracked_by(text, level)
}

/// Rounds a picture's corners. A cover-fitted image is larger than its box and clips to a plain
/// rectangle, so the corners outside the radius are painted over in the colour behind the box.
/// `corners`: top-left, top-right, bottom-right, bottom-left.
fn corner_patches(radius: f32, corners: [bool; 4], top: gpui::Rgba, bottom: gpui::Rgba) -> impl IntoElement {
    canvas(
        |_, _, _| {},
        move |bounds, (), window, _| {
            let (x0, y0) = (bounds.origin.x, bounds.origin.y);
            let (x1, y1) = (x0 + bounds.size.width, y0 + bounds.size.height);
            let r = px(radius);
            // (corner, point on the top/bottom edge, point on the side edge, colour)
            let shapes = [
                (point(x0, y0), point(x0 + r, y0), point(x0, y0 + r), top),
                (point(x1, y0), point(x1 - r, y0), point(x1, y0 + r), top),
                (point(x1, y1), point(x1 - r, y1), point(x1, y1 - r), bottom),
                (point(x0, y1), point(x0 + r, y1), point(x0, y1 - r), bottom),
            ];
            for (index, (corner, along, side, colour)) in shapes.into_iter().enumerate() {
                if !corners[index] {
                    continue;
                }
                let mut path = gpui::PathBuilder::fill();
                path.move_to(corner);
                path.line_to(along);
                // A quarter circle as a cubic Bézier (0.5523 of the radius toward the corner).
                let k = 0.5523;
                let c1 = point(along.x + (corner.x - along.x) * k, along.y);
                let c2 = point(side.x, side.y + (corner.y - side.y) * k);
                path.cubic_bezier_to(side, c1, c2);
                path.close();
                if let Ok(path) = path.build() {
                    window.paint_path(path, colour);
                }
            }
        },
    )
    .absolute()
    .top_0()
    .left_0()
    .size_full()
}

fn rail_rule() -> gpui::Div {
    div().h(px(1.0)).w(px(310.0)).bg(theme::divider_light())
}

/// Darkens a hero pane so its title reads over any picture; the pane that is not selected more so.
fn veil(selected: bool, left_radius: f32, right_radius: f32) -> impl IntoElement {
    div()
        .absolute()
        .top_0()
        .left_0()
        .size_full()
        .rounded_l(px(left_radius))
        .rounded_r(px(right_radius))
        .bg(gpui::rgba(if selected { 0x05091250 } else { 0x050912a8 }))
}

/// The hero's two title lines for a game: a small line above the large title.
fn hero_lines(game_id: &str, name: &'static str) -> (&'static str, &'static str) {
    match game_id {
        "kotor2" => (name, "The Sith Lords"),
        _ => match name.find(" Old ") {
            Some(at) => (&name[..at], &name[at + 1..]),
            None => ("", name),
        },
    }
}

/// A quiet abstract backdrop drawn in code: deterministic specks of light on the navy gradient.
fn starfield(seed: u32, count: usize, brightness: f32) -> impl IntoElement {
    canvas(
        |_, _, _| {},
        move |bounds, (), window, _| {
            let mut state = seed.wrapping_mul(2_654_435_761).max(1);
            let mut next = move || {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                (state % 10_000) as f32 / 10_000.0
            };
            for _ in 0..count {
                let x = bounds.origin.x + bounds.size.width * next();
                let y = bounds.origin.y + bounds.size.height * next();
                let big = next() > 0.92;
                let dot = if big { 2.0 } else { 1.0 };
                let alpha = (0.15 + 0.6 * next()) * brightness;
                let color = gpui::hsla(0.6, 0.25, 0.85, alpha);
                window.paint_quad(fill(Bounds::new(point(x, y), size(px(dot), px(dot))), color));
            }
        },
    )
    .absolute()
    .top_0()
    .left_0()
    .size_full()
}

/// Banner or card art: the player's own Steam cache image, else the abstract backdrop.
fn art_layer(path: Option<&PathBuf>, seed: u32, hue_shift: bool, left_radius: f32, right_radius: f32) -> AnyElement {
    match path {
        Some(path) => img(path.clone())
            .object_fit(ObjectFit::Cover)
            .absolute()
            .top_0()
            .left_0()
            .size_full()
            .rounded_l(px(left_radius))
            .rounded_r(px(right_radius))
            .into_any_element(),
        None => {
            let (top, bottom) = if hue_shift { (0x1b1534, 0x070b18) } else { (0x0f2038, 0x060b16) };
            div()
                .absolute()
                .top_0()
                .left_0()
                .size_full()
                .rounded_l(px(left_radius))
                .rounded_r(px(right_radius))
                .overflow_hidden()
                .bg(linear_gradient(
                    150.0,
                    linear_color_stop(gpui::rgb(top), 0.0),
                    linear_color_stop(gpui::rgb(bottom), 1.0),
                ))
                .child(starfield(seed, 140, 1.0))
                .child(
                    // An original abstract body of light, not derived from any game art.
                    div().absolute().right(px(-60.0)).top(px(-40.0)).size(px(260.0)).rounded_full().bg(
                        linear_gradient(
                            210.0,
                            linear_color_stop(gpui::rgba(if hue_shift { 0x6f5aa055 } else { 0x4f6f9a50 }), 0.0),
                            linear_color_stop(gpui::rgba(0x0a102000), 1.0),
                        ),
                    ),
                )
                .into_any_element()
        }
    }
}

/// A path as the platform writes it (Steam library paths come back with forward slashes).
fn native_path(path: &std::path::Path) -> String {
    let text = path.to_string_lossy();
    if cfg!(windows) { text.replace('/', "\\") } else { text.into_owned() }
}

fn status_of(install: Option<&DetectedInstall>) -> (&'static str, gpui::Rgba) {
    match install {
        None => ("Not found", theme::text_muted()),
        Some(install) => {
            let game = find_game(&install.game_id);
            if !game.is_some_and(|game| game.launchable) {
                ("Not playable yet", theme::amber())
            } else if !install.report.valid {
                ("Not usable", theme::red())
            } else if install.report.count(Severity::Warning) > 0 {
                ("Ready, with notes", theme::green())
            } else {
                ("Ready", theme::green())
            }
        }
    }
}

// ---- layout, measured from the mockup at its 1536x850 window ------------------------------------------------------------

/// Rail width, gap to the main column and the main column's right margin.
const RAIL_W: f32 = 335.0;
const MAIN_PL: f32 = 17.0;
const MAIN_PR: f32 = 20.0;
const TITLEBAR_H: f32 = 125.0;
const HERO_H: f32 = 294.0;
/// Where the slanted hero divider meets the top and bottom edges, as a share of the hero width.
const SPLIT_TOP: f32 = 0.525;
const SPLIT_BOTTOM: f32 = 0.486;
const RADIUS: f32 = 8.0;

// ---- render ----------------------------------------------------------------------------------------------------------

impl Render for LauncherView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.renders += 1;
        if let Some(probe) = &mut self.probe {
            // Measurement only (--probe-file): time to the first render, then a running render count so
            // an idle window can be shown not to redraw.
            let elapsed = probe.started.elapsed().as_secs_f64() * 1000.0;
            let first = *probe.first_render_ms.get_or_insert(elapsed);
            let _ = std::fs::write(
                &probe.file,
                format!("first_render_ms={first:.1}\nrenders={}\nlast_render_ms={elapsed:.1}\n", self.renders),
            );
            if probe.exit {
                cx.quit();
            }
        }
        let main_width = (f32::from(window.viewport_size().width) - RAIL_W - MAIN_PL - MAIN_PR).max(600.0);
        let body = match self.view {
            View::Home => self.render_home(main_width, cx).into_any_element(),
            View::Settings => self.render_settings(cx).into_any_element(),
        };
        let maximized = window.is_maximized();
        div()
            .id("root")
            .size_full()
            .relative()
            .flex()
            .flex_col()
            .font_family(FONT_FAMILY)
            .text_color(theme::text())
            .bg(linear_gradient(
                180.0,
                linear_color_stop(theme::bg_top(), 0.0),
                linear_color_stop(theme::bg_bottom(), 1.0),
            ))
            .on_mouse_up(MouseButton::Left, cx.listener(|this, _, _, _| this.dragging = None))
            // Left out under a dialog: the renderer would otherwise show its specks above the dimmed backdrop.
            .when(self.verify.is_none() && self.info.is_none(), |root| root.child(starfield(7, 90, 0.35)))
            .child(self.render_titlebar(maximized))
            .child(
                div().flex().flex_row().w_full().flex_1().min_h_0().child(self.render_rail(cx)).child(
                    div()
                        .flex_1()
                        .min_w_0()
                        .overflow_hidden()
                        .flex()
                        .flex_col()
                        .pt(px(2.0))
                        .pl(px(MAIN_PL))
                        .pr(px(MAIN_PR))
                        .child(body),
                ),
            )
            .when(!maximized, |root| {
                // The thin window frame; the system rounds the window's corners to the same radius.
                root.child(
                    div()
                        .absolute()
                        .top_0()
                        .left_0()
                        .size_full()
                        .rounded(px(RADIUS))
                        .border_1()
                        .border_color(theme::frame()),
                )
            })
            .when_some(self.verify.as_ref(), |root, verify| {
                root.child(gpui::deferred(self.render_verify(verify, cx)).with_priority(1))
            })
            .when_some(self.info, |root, info| root.child(gpui::deferred(self.render_info(info, cx)).with_priority(1)))
    }
}

impl LauncherView {
    fn render_titlebar(&self, maximized: bool) -> impl IntoElement {
        let control = |id: &'static str, icon_name: &'static str, area: WindowControlArea, close: bool| {
            div()
                .id(id)
                .w(px(62.0))
                .h(px(48.0))
                .flex()
                .items_center()
                .justify_center()
                .rounded(px(4.0))
                .window_control_area(area)
                .hover(move |style| style.bg(if close { gpui::rgba(0xc4302bcc) } else { gpui::rgba(0xffffff14) }))
                .child(icon(icon_name, 24.0, theme::text()))
        };
        div()
            .id("titlebar")
            .relative()
            .w_full()
            .h(px(TITLEBAR_H))
            .flex_none()
            .window_control_area(WindowControlArea::Drag)
            .child(div().absolute().left(px(13.0)).top(px(14.0)).child(icon("icons/mark.svg", 85.0, theme::text())))
            .child(
                div()
                    .absolute()
                    .left(px(113.0))
                    .top(px(22.0))
                    .flex()
                    .flex_col()
                    .child(
                        div()
                            .text_size(px(34.0))
                            .line_height(px(40.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .whitespace_nowrap()
                            .child(spaced(&theme::PRODUCT_NAME.to_uppercase(), 1)),
                    )
                    .child(
                        div()
                            .flex()
                            .flex_row()
                            .items_center()
                            .gap(px(20.0))
                            .child(
                                caps_n("Launcher", 21.0, 1)
                                    .line_height(px(26.0))
                                    .font_weight(FontWeight::MEDIUM)
                                    .text_color(gpui::rgb(0xd2d6dd)),
                            )
                            .child(div().flex_1().h(px(1.0)).bg(gpui::rgb(0x9aa1ab))),
                    )
                    .child(
                        caps_n(theme::PRODUCT_LINE, 15.5, 1)
                            .mt(px(6.0))
                            .line_height(px(20.0))
                            .text_color(gpui::rgb(0xc9ced6)),
                    ),
            )
            .child(
                div()
                    .absolute()
                    .top(px(10.0))
                    .right(px(8.0))
                    .flex()
                    .flex_row()
                    .child(control("min", "icons/min.svg", WindowControlArea::Min, false))
                    .child(control(
                        "max",
                        if maximized { "icons/restore.svg" } else { "icons/max.svg" },
                        WindowControlArea::Max,
                        false,
                    ))
                    .child(control("close", "icons/close.svg", WindowControlArea::Close, true)),
            )
    }

    fn render_rail(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let nav = |id: &'static str,
                   icon_name: &'static str,
                   label: &'static str,
                   view: View,
                   current: View,
                   cx: &mut Context<Self>| {
            let active = view == current;
            let colour = if active { theme::text() } else { theme::text_dim() };
            div()
                .id(id)
                .flex()
                .flex_row()
                .items_center()
                .h(px(40.0))
                .pl(px(13.0))
                .gap(px(17.0))
                .rounded(px(4.0))
                .cursor_pointer()
                .hover(|style| style.bg(theme::panel_hover()))
                .child(icon(icon_name, 23.0, colour))
                .child(caps_n(label, 18.5, 1).font_weight(FontWeight::MEDIUM).text_color(colour))
                .on_click(cx.listener(move |this, _, _, cx| {
                    this.view = view;
                    this.picker = None;
                    cx.notify();
                }))
        };
        let mut games = div().flex().flex_col();
        for (position, game) in game_catalog().iter().enumerate() {
            let found = self.state.install_of(game.id);
            let selected = found.is_some_and(|(index, _)| Some(index) == self.state.selected);
            let (status, status_color) = status_of(found.map(|(_, install)| install));
            let version = found
                .map(|(_, install)| install.report.version.clone())
                .filter(|version| !version.is_empty())
                .map_or_else(|| "Data".to_owned(), |version| format!("Data {version}"));
            let art = self.art.get(game.id).and_then(|art| art.header.as_ref());
            let index = found.map(|(index, _)| index);
            if position > 0 {
                games = games.child(rail_rule().mt(px(9.0)));
            }
            games = games.child(
                div()
                    .id(SharedString::from(format!("card-{}", game.id)))
                    .relative()
                    .w(px(310.0))
                    .h(px(124.0))
                    .mt(px(12.0))
                    .rounded(px(RADIUS))
                    .border_1()
                    .border_color(if selected { theme::gold_border_bright() } else { theme::panel_border() })
                    .when(index.is_some(), |card| card.cursor_pointer())
                    .when(found.is_none(), |card| card.opacity(0.55))
                    .overflow_hidden()
                    .child(art_layer(art, 11 + game.steam_app_id % 97, game.id == "kotor2", RADIUS, RADIUS))
                    .child(corner_patches(RADIUS, [true; 4], gpui::rgb(0x0a1422), gpui::rgb(0x0a1422)))
                    .when(art.is_none(), |card| {
                        card.child(
                            div()
                                .absolute()
                                .left(px(16.0))
                                .bottom(px(14.0))
                                .right(px(12.0))
                                .child(caps(game.name, 16.0).font_weight(FontWeight::SEMIBOLD).whitespace_normal()),
                        )
                    })
                    .when(selected, |card| {
                        card.shadow(vec![BoxShadow {
                            color: theme::hsla(gpui::rgba(0xdcb46240)),
                            offset: point(px(0.0), px(0.0)),
                            blur_radius: px(12.0),
                            spread_radius: px(0.0),
                        }])
                    })
                    .on_click(cx.listener(move |this, _, _, cx| {
                        if let Some(index) = index {
                            this.state.select(index);
                            this.state.config_dirty = true;
                            cx.notify();
                        }
                    })),
            );
            games = games.child(
                div()
                    .flex()
                    .flex_row()
                    .items_center()
                    .gap(px(10.0))
                    .mt(px(9.0))
                    .ml(px(13.0))
                    .h(px(18.0))
                    .child(caps_n(&version, 15.0, 1).font_weight(FontWeight::MEDIUM).text_color(theme::text_dim()))
                    .child(div().w(px(1.0)).h(px(14.0)).bg(theme::text_muted()))
                    .child(caps_n(status, 15.0, 1).font_weight(FontWeight::MEDIUM).text_color(status_color)),
            );
        }
        let current = self.view;
        div()
            .flex()
            .flex_col()
            .w(px(RAIL_W))
            .flex_none()
            .pl(px(10.0))
            .pt(px(11.0))
            .bg(gpui::rgba(0x0e1a2a70))
            .rounded_tr(px(RADIUS))
            .child(nav("nav-home", "icons/home-fill.svg", "Home", View::Home, current, cx))
            .child(nav("nav-settings", "icons/gear-fill.svg", "Settings", View::Settings, current, cx))
            .child(rail_rule().mt(px(12.0)))
            .child(
                caps_n("Your games", 17.0, 1)
                    .font_weight(FontWeight::MEDIUM)
                    .line_height(px(20.0))
                    .mt(px(19.0))
                    .ml(px(13.0))
                    .text_color(theme::text_dim()),
            )
            .child(games)
            .child(rail_rule().mt(px(10.0)))
            .child(self.render_news(cx))
    }

    fn render_news(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let mut panel = div()
            .flex()
            .flex_col()
            .flex_none()
            .mt(px(15.0))
            .w(px(310.0))
            .h(px(179.0))
            .pt(px(16.0))
            .px(px(15.0))
            .pb(px(16.0))
            .rounded(px(RADIUS))
            .border_1()
            .border_color(theme::panel_border())
            .bg(gpui::rgba(0x0b152199))
            .child(
                caps_n("News & updates", 17.0, 1)
                    .font_weight(FontWeight::MEDIUM)
                    .line_height(px(20.0))
                    .text_color(theme::text_dim()),
            );
        if let Some(item) = self.news.first() {
            panel = panel
                .child(
                    div().mt(px(12.0)).text_size(px(18.0)).line_height(px(22.0)).truncate().child(item.title.clone()),
                )
                .child(
                    div()
                        .mt(px(5.0))
                        .text_size(px(16.5))
                        .line_height(px(25.0))
                        .text_color(gpui::rgb(0xc3c7ce))
                        .line_clamp(2)
                        .child(item.body.clone()),
                );
        }
        panel.child(
            div()
                .id("news-all")
                .flex()
                .flex_row()
                .justify_end()
                .items_center()
                .gap(px(10.0))
                .mt_auto()
                .pr(px(11.0))
                .cursor_pointer()
                .hover(|style| style.opacity(0.8))
                .child(caps_n("View all news", 15.0, 1).font_weight(FontWeight::MEDIUM).text_color(theme::text_dim()))
                .child(icon("icons/chevron.svg", 14.0, theme::text_dim()))
                .on_click(cx.listener(|this, _, _, cx| {
                    this.info = Some(Info::News);
                    cx.notify();
                })),
        )
    }

    fn render_hero(&self, width: f32, cx: &mut Context<Self>) -> impl IntoElement {
        let height = HERO_H;
        let top = width * SPLIT_TOP;
        let bottom = width * SPLIT_BOTTOM;
        let games = game_catalog();
        let pane_state = |game: &crate::install::GameDef| {
            let found = self.state.install_of(game.id);
            let selected = found.is_some_and(|(index, _)| Some(index) == self.state.selected);
            (found.map(|(index, _)| index), selected)
        };
        let (left_index, left_selected) = pane_state(&games[0]);
        let (right_index, right_selected) = pane_state(&games[1]);
        let left_art = self.art.get(games[0].id).and_then(|art| art.hero.clone());
        let right_art = self.art.get(games[1].id).and_then(|art| art.hero.clone());
        let left_seed = 31 + games[0].steam_app_id % 89;
        let right_seed = 31 + games[1].steam_app_id % 89;
        // One pane's picture and veils, at a fixed size so every clipped piece of it lines up.
        let left_content = move |selected: bool| {
            div()
                .absolute()
                .top_0()
                .left_0()
                .w(px(top))
                .h(px(height))
                .child(art_layer(left_art.as_ref(), left_seed, false, RADIUS, 0.0))
                .child(veil(selected, RADIUS, 0.0))
        };
        fn select(
            index: Option<usize>,
            cx: &mut Context<LauncherView>,
        ) -> impl Fn(&gpui::ClickEvent, &mut Window, &mut App) + 'static {
            cx.listener(move |this, _, _, cx| {
                if let Some(index) = index {
                    this.state.select(index);
                    this.state.config_dirty = true;
                    cx.notify();
                }
            })
        }
        let mut hero = div().relative().w(px(width)).h(px(height)).flex_none();
        // Right pane: a rectangle from the divider's lowest point to the right edge.
        hero = hero.child(
            div()
                .id("hero-kotor2")
                .absolute()
                .top_0()
                .left(px(bottom))
                .w(px(width - bottom))
                .h(px(height))
                .overflow_hidden()
                .when(right_index.is_some(), |pane| pane.cursor_pointer())
                .child(art_layer(right_art.as_ref(), right_seed, true, 0.0, RADIUS))
                .child(veil(right_selected, 0.0, RADIUS))
                .on_click(select(right_index, cx)),
        );
        // Left pane: the rectangle left of the divider's lowest point...
        hero = hero.child(
            div()
                .id("hero-kotor1")
                .absolute()
                .top_0()
                .left_0()
                .w(px(bottom))
                .h(px(height))
                .overflow_hidden()
                .when(left_index.is_some(), |pane| pane.cursor_pointer())
                .child(left_content(left_selected))
                .on_click(select(left_index, cx)),
        );
        // ...plus the sliver between the slanted divider and that rectangle, in thin rows of the same picture.
        const ROW: f32 = 2.0;
        let rows = (height / ROW).ceil() as usize;
        for row in 0..rows {
            let y = row as f32 * ROW;
            let edge = top + (bottom - top) * ((y + ROW * 0.5) / height).min(1.0);
            if edge <= bottom {
                continue;
            }
            hero = hero.child(
                div()
                    .absolute()
                    .top(px(y))
                    .left(px(bottom))
                    .w(px(edge - bottom))
                    .h(px(ROW))
                    .overflow_hidden()
                    .child(div().absolute().top(px(-y)).left(px(-bottom)).child(left_content(left_selected))),
            );
        }
        hero = hero.child(corner_patches(RADIUS, [true; 4], gpui::rgb(0x091423), gpui::rgb(0x08111e)));
        // Outlines: gold around the selected pane, grey around the other, and the gold divider.
        hero = hero.child(
            canvas(
                |_, _, _| {},
                move |bounds, (), window, _| {
                    let at = |x: f32, y: f32| point(bounds.origin.x + px(x), bounds.origin.y + px(y));
                    let (w, h, r) = (width - 0.5, height - 0.5, RADIUS);
                    let mut left = gpui::PathBuilder::stroke(px(1.2));
                    left.move_to(at(top, 0.5));
                    left.line_to(at(r, 0.5));
                    left.curve_to(at(0.5, r), at(0.5, 0.5));
                    left.line_to(at(0.5, h - r));
                    left.curve_to(at(r, h), at(0.5, h));
                    left.line_to(at(bottom, h));
                    let mut right = gpui::PathBuilder::stroke(px(1.2));
                    right.move_to(at(top, 0.5));
                    right.line_to(at(w - r, 0.5));
                    right.curve_to(at(w, r), at(w, 0.5));
                    right.line_to(at(w, h - r));
                    right.curve_to(at(w - r, h), at(w, h));
                    right.line_to(at(bottom, h));
                    let mut divider = gpui::PathBuilder::stroke(px(1.6));
                    divider.move_to(at(top, 0.0));
                    divider.line_to(at(bottom, height));
                    let gold = theme::gold_border_bright();
                    let grey = theme::panel_border_light();
                    let (left_colour, right_colour) =
                        if right_selected && !left_selected { (grey, gold) } else { (gold, grey) };
                    for (path, colour) in [(left, left_colour), (right, right_colour), (divider, theme::gold())] {
                        if let Ok(path) = path.build() {
                            window.paint_path(path, colour);
                        }
                    }
                },
            )
            .absolute()
            .top_0()
            .left_0()
            .size_full(),
        );
        // Titles: a small line, the large title and the spaced product line, as the mockup's lockups.
        let (k1_small, k1_large) = hero_lines(games[0].id, games[0].name);
        let (k2_small, k2_large) = hero_lines(games[1].id, games[1].name);
        let middle = (top + bottom) * 0.5;
        hero = hero
            .child(
                div()
                    .absolute()
                    .top(px(51.0))
                    .left_0()
                    .w(px(middle))
                    .flex()
                    .flex_col()
                    .items_center()
                    .child(
                        caps_n(k1_small, 24.0, 1)
                            .line_height(px(30.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(theme::title_white()),
                    )
                    .child(
                        caps_n(k1_large, 50.0, 1)
                            .mt(px(14.0))
                            .line_height(px(60.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(theme::gold_title()),
                    )
                    .child(
                        caps_n(theme::PRODUCT_NAME, 17.0, 2)
                            .mt(px(48.0))
                            .line_height(px(22.0))
                            .font_weight(FontWeight::MEDIUM)
                            .text_color(theme::title_white()),
                    ),
            )
            .child(
                div()
                    .absolute()
                    .top(px(81.0))
                    .left(px(middle))
                    .right(px(0.0))
                    .pr(px(width * 0.074))
                    .flex()
                    .flex_col()
                    .items_center()
                    .child(
                        caps_n(k2_small, 20.0, 1)
                            .line_height(px(26.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(theme::title_white()),
                    )
                    .child(
                        caps_n(k2_large, 50.0, 0)
                            .mt(px(7.0))
                            .line_height(px(58.0))
                            .font_weight(FontWeight::MEDIUM)
                            .text_color(gpui::rgb(0xe8e9ee)),
                    )
                    .child(div().mt(px(23.0)).w(px(200.0)).h(px(1.0)).bg(gpui::rgba(0xffffff1f)))
                    .child(
                        caps_n(theme::PRODUCT_NAME, 17.0, 2)
                            .mt(px(10.0))
                            .line_height(px(22.0))
                            .font_weight(FontWeight::MEDIUM)
                            .text_color(theme::title_white()),
                    ),
            );
        hero
    }

    fn render_home(&self, width: f32, cx: &mut Context<Self>) -> impl IntoElement {
        let install = self.state.current();
        let game =
            install.and_then(|install| find_game(&install.game_id)).or_else(|| find_game(&self.state.choices.game_id));
        let title = game.map_or("No game selected", |game| game.name);
        let description = match (game, install) {
            (_, None) if self.scanning => "Looking for game installs on this computer...".to_owned(),
            (_, None) => "No install found. Use Change below to point at the folder of your own copy of the game.".to_owned(),
            (Some(game), Some(_)) if !game.launchable => {
                "Your install was found and checked. This engine cannot play this game yet; it will once the work on it is done.".to_owned()
            }
            (_, Some(install)) => {
                let report = &install.report;
                let mut text = String::from("Runs your own copy of the game on a new engine. The launcher reads the install and never changes it.");
                if report.recognized {
                    text.push_str(&format!(" Data set {}", report.dataset_id));
                } else {
                    text.push_str(" Data set not recognised; it may still work");
                }
                if report.language_id.is_some() {
                    text.push_str(&format!(", {} text, {} archives.", report.language_name, report.bif_count));
                } else {
                    text.push('.');
                }
                text
            }
        };
        let backend = Backend::parse(&self.state.choices.backend).unwrap_or(Backend::Auto);
        let info_row = |icon_name: &'static str, label: &'static str, value: String, last: bool| {
            div()
                .flex()
                .flex_row()
                .items_center()
                .h(px(57.0))
                .mx(px(12.0))
                .when(!last, |row| row.border_b_1().border_color(theme::panel_border()))
                .child(div().w(px(58.0)).pl(px(10.0)).child(icon(icon_name, 28.0, theme::text_dim())))
                .child(
                    caps_n(label, 17.0, 1)
                        .font_weight(FontWeight::MEDIUM)
                        .w(px(150.0))
                        .flex_none()
                        .text_color(theme::text_dim()),
                )
                .child(div().flex_1().min_w_0().text_size(px(16.5)).truncate().child(value))
        };
        let plan = self.state.plan();
        let ready = plan.ok() && !self.scanning;
        let engine_missing = !self.state.engine_found();
        let (state_text, state_color) = if self.scanning {
            ("Scanning for installs".to_owned(), theme::text_dim())
        } else if game.is_some_and(|game| !game.launchable) {
            ("Not playable yet".to_owned(), theme::amber())
        } else if engine_missing {
            ("Engine not found".to_owned(), theme::amber())
        } else if let Some(problem) = plan.problems.first() {
            (problem.clone(), theme::amber())
        } else {
            ("Ready to play".to_owned(), theme::green())
        };
        let outline_button = |id: &'static str, icon_name: &'static str, label: &'static str, width: f32| {
            div()
                .id(id)
                .flex()
                .flex_row()
                .items_center()
                .gap(px(18.0))
                .pl(px(31.0))
                .w(px(width))
                .h(px(68.0))
                .rounded(px(5.0))
                .border_1()
                .border_color(gpui::rgb(0x3a4453))
                .bg(gpui::rgba(0x0e1825d0))
                .cursor_pointer()
                .hover(|style| style.bg(theme::panel_hover()).border_color(gpui::rgb(0x56616f)))
                .child(icon(icon_name, 36.0, theme::text()))
                .child(caps_n(label, 20.0, 1).font_weight(FontWeight::MEDIUM))
        };
        let install_path = if self.state.choices.install.as_os_str().is_empty() {
            "Not chosen".to_owned()
        } else {
            native_path(&self.state.choices.install)
        };
        let play_ink = if ready { theme::play_text() } else { theme::text_muted() };

        div()
            .flex()
            .flex_col()
            .size_full()
            .child(self.render_hero(width, cx))
            .child(
                div()
                    .flex()
                    .flex_row()
                    .mt(px(21.0))
                    .child(
                        div()
                            .flex_1()
                            .min_w_0()
                            .flex()
                            .flex_col()
                            .pl(px(21.0))
                            .child(div().h(px(1.0)).w(px(397.0)).bg(gpui::linear_gradient(
                                90.0,
                                linear_color_stop(theme::gold_border(), 0.0),
                                linear_color_stop(gpui::rgba(0x8c6d3c40), 1.0),
                            )))
                            .child(
                                caps_n(title, 28.0, 1)
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .line_height(px(34.0))
                                    .mt(px(14.0))
                                    .pl(px(23.0))
                                    .text_color(theme::gold()),
                            )
                            .child(
                                caps_n(theme::PRODUCT_NAME, 19.0, 2)
                                    .font_weight(FontWeight::MEDIUM)
                                    .line_height(px(24.0))
                                    .mt(px(7.0))
                                    .pl(px(23.0))
                                    .text_color(gpui::rgb(0xc9ced6)),
                            )
                            .child(
                                div()
                                    .mt(px(13.0))
                                    .pl(px(23.0))
                                    .pr(px(24.0))
                                    .max_w(px(600.0))
                                    .text_size(px(18.5))
                                    .line_height(px(29.0))
                                    .text_color(gpui::rgb(0xd0d4da))
                                    .line_clamp(3)
                                    .child(description),
                            ),
                    )
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .w(px(441.0))
                            .flex_none()
                            .mt(px(12.0))
                            .mr(px(35.0))
                            .rounded(px(RADIUS))
                            .border_1()
                            .border_color(theme::panel_border_light())
                            .bg(gpui::rgba(0x0b1521cc))
                            .child(info_row("icons/engine.svg", "Engine", theme::ENGINE_VERSION.to_owned(), false))
                            .child(info_row(
                                "icons/display.svg",
                                "Resolution",
                                resolution_summary(&self.state.settings),
                                false,
                            ))
                            .child(info_row("icons/api.svg", "API", backend.api_summary().to_owned(), true)),
                    ),
            )
            .child(
                div()
                    .flex()
                    .flex_row()
                    .items_start()
                    .mt(px(32.0))
                    .pl(px(43.0))
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .w(px(420.0))
                            .flex_none()
                            .child(
                                div()
                                    .id("play")
                                    .relative()
                                    .flex()
                                    .flex_row()
                                    .items_center()
                                    .justify_center()
                                    .w(px(420.0))
                                    .h(px(68.0))
                                    .rounded(px(5.0))
                                    .border_1()
                                    .when(ready, |button| {
                                        button
                                            .border_color(theme::play_border())
                                            .bg(linear_gradient(
                                                180.0,
                                                linear_color_stop(theme::play_top(), 0.0),
                                                linear_color_stop(theme::play_bottom(), 1.0),
                                            ))
                                            .cursor_pointer()
                                            .hover(|style| style.opacity(0.92))
                                            .shadow(vec![BoxShadow {
                                                color: theme::hsla(gpui::rgba(0xc9a05240)),
                                                offset: point(px(0.0), px(2.0)),
                                                blur_radius: px(14.0),
                                                spread_radius: px(0.0),
                                            }])
                                    })
                                    .when(!ready, |button| {
                                        button.border_color(gpui::rgb(0x3a3f47)).bg(gpui::rgb(0x1a2029))
                                    })
                                    .child(
                                        caps_n("Play", 33.0, 1).font_weight(FontWeight::SEMIBOLD).text_color(play_ink),
                                    )
                                    .child(div().absolute().left(px(327.0)).top(px(12.0)).child(icon(
                                        "icons/play.svg",
                                        42.0,
                                        play_ink,
                                    )))
                                    .on_click(cx.listener(|this, _, _, cx| this.play(cx))),
                            )
                            .child(
                                div()
                                    .flex()
                                    .flex_row()
                                    .items_center()
                                    .gap(px(10.0))
                                    .mt(px(14.0))
                                    .ml(px(8.0))
                                    .h(px(20.0))
                                    .child(div().size(px(9.0)).flex_none().rounded_full().bg(state_color))
                                    .child(
                                        caps_n(&state_text, 16.0, 1)
                                            .font_weight(FontWeight::MEDIUM)
                                            .text_color(theme::text_dim())
                                            .truncate()
                                            .min_w_0(),
                                    )
                                    .when(engine_missing && !self.scanning, |row| {
                                        row.child(div().w(px(1.0)).h(px(14.0)).bg(theme::text_muted())).child(
                                            div()
                                                .id("locate-engine")
                                                .cursor_pointer()
                                                .hover(|style| style.opacity(0.8))
                                                .child(
                                                    caps_n("Locate engine", 16.0, 1)
                                                        .font_weight(FontWeight::MEDIUM)
                                                        .text_color(theme::gold()),
                                                )
                                                .on_click(cx.listener(|this, _, _, cx| {
                                                    this.browse_file(BrowseTarget::Engine, cx)
                                                })),
                                        )
                                    }),
                            ),
                    )
                    .child(div().w(px(62.0)).flex_none())
                    .child(
                        outline_button("verify", "icons/shield.svg", "Verify files", 275.0)
                            .mt(px(2.0))
                            .on_click(cx.listener(|this, _, _, cx| this.verify_files(cx))),
                    )
                    .child(div().w(px(24.0)).flex_none())
                    .child(
                        outline_button("game-settings", "icons/gear-fill.svg", "Game settings", 295.0)
                            .mt(px(2.0))
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.view = View::Settings;
                                cx.notify();
                            })),
                    ),
            )
            .child(div().flex_1())
            .child(self.render_footer(install_path, cx))
    }

    fn render_footer(&self, install_path: String, cx: &mut Context<Self>) -> impl IntoElement {
        let link = |id: &'static str, label: &'static str| {
            div()
                .id(id)
                .cursor_pointer()
                .hover(|style| style.opacity(0.75))
                .child(caps_n(label, 15.0, 1).font_weight(FontWeight::MEDIUM).text_color(gpui::rgb(0xc9ced6)))
                .on_click(cx.listener(|this, _, _, cx| {
                    this.info = Some(Info::About);
                    cx.notify();
                }))
        };
        let bar = || div().w(px(1.0)).h(px(28.0)).bg(theme::panel_border_light());
        div()
            .flex()
            .flex_row()
            .items_center()
            .w_full()
            .h(px(70.0))
            .flex_none()
            .border_t_1()
            .border_color(theme::divider())
            .pl(px(28.0))
            .pr(px(6.0))
            .child(
                div()
                    .flex()
                    .flex_col()
                    .min_w_0()
                    .flex_1()
                    .child(
                        caps_n("Install location", 13.0, 1)
                            .font_weight(FontWeight::MEDIUM)
                            .line_height(px(16.0))
                            .text_color(theme::text_dim()),
                    )
                    .child(
                        div()
                            .flex()
                            .flex_row()
                            .items_center()
                            .gap(px(18.0))
                            .mt(px(4.0))
                            .child(
                                div()
                                    .text_size(px(14.5))
                                    .line_height(px(18.0))
                                    .text_color(gpui::rgb(0xc3c7ce))
                                    .truncate()
                                    .max_w(px(460.0))
                                    .child(install_path),
                            )
                            .child(div().w(px(1.0)).h(px(16.0)).bg(theme::text_muted()))
                            .child(
                                div()
                                    .id("change")
                                    .cursor_pointer()
                                    .hover(|style| style.text_color(theme::gold()))
                                    .child(
                                        caps_n("Change", 14.0, 1)
                                            .font_weight(FontWeight::MEDIUM)
                                            .text_color(theme::text_dim()),
                                    )
                                    .on_click(cx.listener(|this, _, _, cx| this.browse_install(cx))),
                            )
                            .when(!self.state.status.is_empty(), |row| {
                                row.child(
                                    div()
                                        .text_size(px(12.5))
                                        .text_color(theme::amber())
                                        .truncate()
                                        .child(self.state.status.clone()),
                                )
                            }),
                    ),
            )
            .child(
                div()
                    .flex()
                    .flex_row()
                    .items_center()
                    .flex_none()
                    .child(link("support", "Support"))
                    .child(div().w(px(31.0)))
                    .child(bar())
                    .child(div().w(px(29.0)))
                    .child(link("feedback", "Feedback"))
                    .child(div().w(px(34.0)))
                    .child(bar())
                    .child(div().w(px(28.0)))
                    .child(
                        div()
                            .id("about")
                            .size(px(42.0))
                            .flex()
                            .items_center()
                            .justify_center()
                            .rounded_full()
                            .border_1()
                            .border_color(gpui::rgb(0x7d8692))
                            .bg(gpui::rgba(0x0b1521cc))
                            .cursor_pointer()
                            .hover(|style| style.bg(theme::panel_hover()))
                            .child(icon("icons/mark.svg", 26.0, gpui::rgb(0xc9ced6)))
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.info = Some(Info::About);
                                cx.notify();
                            })),
                    ),
            )
    }

    /// About (the footer links and emblem) and all news (the news panel's link).
    fn render_info(&self, info: Info, cx: &mut Context<Self>) -> impl IntoElement {
        let mut panel = div()
            .id("info-panel")
            .flex()
            .flex_col()
            .gap(px(8.0))
            .w(px(620.0))
            .p(px(24.0))
            .rounded(px(RADIUS))
            .border_1()
            .border_color(theme::gold_border())
            .bg(theme::panel())
            .occlude();
        match info {
            Info::About => {
                panel = panel
                    .child(
                        caps_n(theme::PRODUCT_NAME, 20.0, 2)
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(theme::gold()),
                    )
                    .child(div().text_size(px(14.0)).text_color(theme::text_dim()).child(format!(
                        "{}. Engine version {}.",
                        theme::PRODUCT_LINE,
                        theme::ENGINE_VERSION
                    )))
                    .child(
                        div()
                            .text_size(px(14.0))
                            .line_height(px(21.0))
                            .text_color(theme::text())
                            .child(theme::DISCLAIMER.join(" ")),
                    )
                    .child(
                        div()
                            .text_size(px(13.5))
                            .text_color(theme::text_muted())
                            .child("Support and feedback channels are not set up yet."),
                    );
            }
            Info::News => {
                panel = panel.child(
                    caps_n("News & updates", 20.0, 1).font_weight(FontWeight::SEMIBOLD).text_color(theme::gold()),
                );
                for item in &self.news {
                    panel = panel
                        .child(div().mt(px(6.0)).text_size(px(16.0)).child(item.title.clone()))
                        .child(div().text_size(px(14.0)).text_color(theme::text_dim()).child(item.body.clone()));
                }
            }
        }
        panel = panel.child(
            div().flex().flex_row().justify_end().mt(px(10.0)).child(
                div()
                    .id("info-close")
                    .px(px(22.0))
                    .py(px(9.0))
                    .rounded(px(4.0))
                    .border_1()
                    .border_color(gpui::rgb(0x323c4a))
                    .cursor_pointer()
                    .hover(|style| style.bg(theme::panel_hover()))
                    .child(caps("Close", 13.0).font_weight(FontWeight::MEDIUM))
                    .on_click(cx.listener(|this, _, _, cx| {
                        this.info = None;
                        cx.notify();
                    })),
            ),
        );
        div()
            .id("info-backdrop")
            .absolute()
            .top_0()
            .left_0()
            .size_full()
            .flex()
            .items_center()
            .justify_center()
            .bg(gpui::rgba(0x02050ab0))
            .on_click(cx.listener(|this, _, _, cx| {
                this.info = None;
                cx.notify();
            }))
            .child(panel)
    }

    fn render_verify(&self, verify: &Verify, cx: &mut Context<Self>) -> impl IntoElement {
        let mut panel = div()
            .id("verify-panel")
            .flex()
            .flex_col()
            .w(px(640.0))
            .max_h(px(480.0))
            .p(px(24.0))
            .rounded(px(8.0))
            .border_1()
            .border_color(theme::gold_border())
            .bg(theme::panel())
            .occlude()
            .child(caps("Verify files", 18.0).font_weight(FontWeight::SEMIBOLD).text_color(theme::gold()));
        match verify {
            Verify::Running => {
                panel = panel.child(
                    div()
                        .mt(px(16.0))
                        .text_size(px(14.0))
                        .text_color(theme::text_dim())
                        .child("Checking the data set..."),
                );
            }
            Verify::Done(report) => {
                let (headline, color) = if !report.valid {
                    ("Not usable", theme::red())
                } else if report.count(Severity::Warning) > 0 {
                    ("Usable, with notes", theme::amber())
                } else {
                    ("Usable", theme::green())
                };
                let mut facts = vec![format!("Folder: {}", native_path(&report.root))];
                if report.recognized {
                    facts.push(format!("Data set: {}", report.dataset_id));
                } else if !report.key_sha256.is_empty() {
                    facts.push(format!("Data set not recognised (key {})", &report.key_sha256[..12]));
                }
                if report.language_id.is_some() {
                    facts.push(format!("Language: {}   Strings: {}", report.language_name, report.tlk_strings));
                }
                if report.bif_count > 0 {
                    facts.push(format!("Archives: {}   Resources: {}", report.bif_count, report.key_entries));
                }
                let mut list = div().id("verify-list").flex().flex_col().mt(px(12.0)).gap(px(6.0)).overflow_y_scroll();
                list = list.child(caps(headline, 15.0).font_weight(FontWeight::SEMIBOLD).text_color(color));
                for fact in facts {
                    list = list.child(div().text_size(px(13.5)).text_color(theme::text_dim()).child(fact));
                }
                for issue in &report.issues {
                    let color = match issue.severity {
                        Severity::Error => theme::red(),
                        Severity::Warning => theme::amber(),
                    };
                    list = list.child(div().text_size(px(13.5)).text_color(color).child(issue.message.clone()));
                }
                if report.issues.is_empty() {
                    list = list.child(div().text_size(px(13.5)).text_color(theme::text_dim()).child("No issues."));
                }
                panel = panel.child(list);
            }
        }
        panel = panel.child(
            div().flex().flex_row().justify_end().mt(px(18.0)).child(
                div()
                    .id("verify-close")
                    .px(px(22.0))
                    .py(px(9.0))
                    .rounded(px(4.0))
                    .border_1()
                    .border_color(gpui::rgb(0x323c4a))
                    .cursor_pointer()
                    .hover(|style| style.bg(theme::panel_hover()))
                    .child(caps("Close", 13.0).font_weight(FontWeight::MEDIUM))
                    .on_click(cx.listener(|this, _, _, cx| {
                        this.verify = None;
                        cx.notify();
                    })),
            ),
        );
        div()
            .absolute()
            .top_0()
            .left_0()
            .size_full()
            .flex()
            .items_center()
            .justify_center()
            .bg(gpui::rgba(0x02050ab0))
            .child(panel)
    }

    // ---- settings view -----------------------------------------------------------------------------------------------

    fn render_settings(&mut self, cx: &mut Context<Self>) -> impl IntoElement {
        let mut content = div()
            .id("settings-scroll")
            .flex()
            .flex_col()
            .flex_1()
            .min_h_0()
            .overflow_y_scroll()
            .pr(px(10.0))
            .gap(px(14.0));
        content = content.child(self.render_remaster_section(cx));
        content = content.child(self.render_launch_section(cx));
        let mut group = "";
        let mut section: Option<gpui::Div> = None;
        for option in ENGINE_OPTIONS {
            if option.group != group {
                if let Some(done) = section.take() {
                    content = content.child(done);
                }
                group = option.group;
                section = Some(section_box(option.group, None));
            }
            let row = self.render_option(option, cx);
            section = section.map(|section| section.child(row));
        }
        if let Some(done) = section {
            content = content.child(done);
        }
        content = content.child(
            section_box("About", None)
                .child(div().text_size(px(13.5)).text_color(theme::text_dim()).child(format!(
                    "{} {}. {}.",
                    theme::PRODUCT_NAME,
                    theme::ENGINE_VERSION,
                    theme::PRODUCT_LINE
                )))
                .child(
                    div()
                        .text_size(px(13.5))
                        .line_height(px(20.0))
                        .text_color(theme::text_dim())
                        .child(theme::DISCLAIMER.join(" ")),
                ),
        );
        let unsaved = self.state.unsaved();
        div()
            .flex()
            .flex_col()
            .size_full()
            .min_h_0()
            .child(
                div()
                    .flex()
                    .flex_row()
                    .items_center()
                    .h(px(44.0))
                    .flex_none()
                    .child(caps("Settings", 22.0).font_weight(FontWeight::SEMIBOLD).text_color(theme::gold()))
                    .child(div().flex_1())
                    .child(
                        div()
                            .text_size(px(12.0))
                            .text_color(theme::text_muted())
                            .truncate()
                            .child(format!("Settings file: {}", generic(&self.state.settings_path))),
                    ),
            )
            .child(div().h(px(1.0)).w_full().bg(theme::gold_line()).mb(px(12.0)))
            .child(content)
            .child(
                div()
                    .flex()
                    .flex_row()
                    .items_center()
                    .gap(px(14.0))
                    .h(px(62.0))
                    .flex_none()
                    .border_t_1()
                    .border_color(theme::divider())
                    .child(
                        caps(if unsaved { "Unsaved changes" } else { "All changes saved" }, 12.0)
                            .text_color(if unsaved { theme::amber() } else { theme::text_muted() }),
                    )
                    .when(!self.state.status.is_empty(), |row| {
                        row.child(
                            div()
                                .text_size(px(12.0))
                                .text_color(theme::amber())
                                .truncate()
                                .child(self.state.status.clone()),
                        )
                    })
                    .child(div().flex_1())
                    .child(small_button("restore", "Restore engine defaults", false).on_click(cx.listener(
                        |this, _, _, cx| {
                            for option in ENGINE_OPTIONS {
                                this.state.settings.set(option.key, default_of(option.key).unwrap_or(0.0));
                            }
                            cx.notify();
                        },
                    )))
                    .child(small_button("save", "Save", true).on_click(cx.listener(|this, _, _, cx| {
                        if this.state.save_all() {
                            this.state.status.clear();
                        }
                        cx.notify();
                    }))),
            )
    }

    fn render_remaster_section(&mut self, cx: &mut Context<Self>) -> impl IntoElement {
        let native = self.state.choices.native_font;
        let mut section = section_box(
            "Remaster",
            Some(
                "Classic keeps the original interface font, scaled. Native font draws interface text with a font from your own system. No font is shipped with the engine.",
            ),
        );
        section = section.child(setting_row(
            "Interface font",
            segmented(
                "font-mode",
                &["Classic (default)", "Native font"],
                usize::from(native),
                cx.listener(|this, index: &usize, _, cx| {
                    this.state.choices.native_font = *index == 1;
                    this.state.config_dirty = true;
                    if *index == 1 && this.fonts.is_none() {
                        this.fonts = Some(scan_fonts(&system_font_folders()));
                    }
                    cx.notify();
                }),
            ),
        ));
        if native {
            let font = &self.state.choices.font_file;
            let name =
                font.file_name().map_or_else(|| "Choose a font".to_owned(), |name| name.to_string_lossy().into_owned());
            section = section.child(setting_row(
                "Font file",
                div()
                    .flex()
                    .flex_row()
                    .gap(px(8.0))
                    .child(dropdown_button("font-pick", &name, 260.0).on_click(cx.listener(|this, _, _, cx| {
                        if this.fonts.is_none() {
                            this.fonts = Some(scan_fonts(&system_font_folders()));
                        }
                        this.picker = if this.picker == Some(Picker::Font) { None } else { Some(Picker::Font) };
                        cx.notify();
                    })))
                    .child(
                        small_button("font-browse", "Browse", false)
                            .on_click(cx.listener(|this, _, _, cx| this.browse_file(BrowseTarget::Font, cx))),
                    ),
            ));
            if self.picker == Some(Picker::Font) {
                let fonts = self.fonts.clone().unwrap_or_default();
                let items: Vec<(String, PathBuf)> = fonts
                    .into_iter()
                    .map(|path| {
                        (path.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default(), path)
                    })
                    .collect();
                section = section.child(self.picker_list("font-list", items, font.clone(), cx, |this, path| {
                    this.state.choices.font_file = path;
                }));
            }
            let valid = !font.as_os_str().is_empty() && font.is_file() && is_font_file(font);
            let note = if font.as_os_str().is_empty() {
                ("No font chosen yet.", theme::text_muted())
            } else if !valid {
                ("That is not a .ttf, .otf or .ttc font file.", theme::red())
            } else {
                ("The engine receives this font with --ui-font.", theme::text_muted())
            };
            section = section.child(div().pl(px(4.0)).text_size(px(12.5)).text_color(note.1).child(note.0));
            if valid && let Some(family) = self.preview_family(cx) {
                section = section.child(
                    div()
                        .mt(px(6.0))
                        .p(px(12.0))
                        .rounded(px(4.0))
                        .border_1()
                        .border_color(theme::panel_border())
                        .font_family(family)
                        .text_size(px(22.0))
                        .child("The quick brown fox jumps over the lazy dog 0123456789"),
                );
            }
        }
        section
    }

    /// Loads the chosen native font once (read-only) and returns its family name for the preview.
    fn preview_family(&mut self, cx: &mut Context<Self>) -> Option<SharedString> {
        let font = self.state.choices.font_file.clone();
        if self.preview.as_ref().is_none_or(|(path, _)| *path != font) {
            let family = std::fs::read(&font).ok().and_then(|bytes| {
                let family = crate::fontname::family_name(&bytes)?;
                cx.text_system().add_fonts(vec![std::borrow::Cow::Owned(bytes)]).ok()?;
                Some(SharedString::from(family))
            });
            self.preview = Some((font, family));
        }
        self.preview.as_ref().and_then(|(_, family)| family.clone())
    }

    fn render_launch_section(&mut self, cx: &mut Context<Self>) -> impl IntoElement {
        let backend = Backend::ALL.iter().position(|backend| backend.name() == self.state.choices.backend).unwrap_or(0);
        let labels: Vec<&'static str> = Backend::ALL.iter().map(|backend| backend.label()).collect();
        let language = if self.state.choices.language.is_empty() {
            "Install default".to_owned()
        } else {
            self.state.choices.language.clone()
        };
        let saves = if self.state.choices.saves_dir.as_os_str().is_empty() {
            "Engine default".to_owned()
        } else {
            native_path(&self.state.choices.saves_dir)
        };
        let mut section = section_box("Launch", None)
            .child(setting_row(
                "Renderer",
                segmented(
                    "backend",
                    &labels,
                    backend,
                    cx.listener(|this, index: &usize, _, cx| {
                        this.state.choices.backend = Backend::ALL[*index].name().to_owned();
                        this.state.config_dirty = true;
                        cx.notify();
                    }),
                ),
            ))
            .child(setting_row(
                "Language",
                dropdown_button("language-pick", &language, 260.0).on_click(cx.listener(|this, _, _, cx| {
                    this.picker = if this.picker == Some(Picker::Language) { None } else { Some(Picker::Language) };
                    cx.notify();
                })),
            ));
        if self.picker == Some(Picker::Language) {
            let mut items = vec![("Install default".to_owned(), String::new())];
            items.extend(LANGUAGES.iter().map(|(_, name)| ((*name).to_owned(), (*name).to_owned())));
            section = section.child(self.picker_list(
                "language-list",
                items,
                self.state.choices.language.clone(),
                cx,
                |this, value| {
                    this.state.choices.language = value;
                },
            ));
        }
        let engine_text =
            if self.state.engine_found() { native_path(&self.state.engine_path) } else { "Not found".to_owned() };
        section
            .child(setting_row(
                "Engine",
                div()
                    .flex()
                    .flex_row()
                    .items_center()
                    .gap(px(8.0))
                    .child(
                        div()
                            .max_w(px(420.0))
                            .truncate()
                            .text_size(px(13.0))
                            .text_color(if self.state.engine_found() { theme::text_dim() } else { theme::amber() })
                            .child(engine_text),
                    )
                    .child(
                        small_button("engine-browse", "Browse", false)
                            .on_click(cx.listener(|this, _, _, cx| this.browse_file(BrowseTarget::Engine, cx))),
                    )
                    .child(small_button("engine-detect", "Find", false).on_click(cx.listener(|this, _, _, cx| {
                        if !this.state.detect_engine() {
                            this.state.status = "No engine build was found near the launcher.".into();
                        }
                        cx.notify();
                    }))),
            ))
            .child(setting_row(
                "Saves folder",
                div()
                    .flex()
                    .flex_row()
                    .items_center()
                    .gap(px(8.0))
                    .child(
                        div()
                            .max_w(px(260.0))
                            .truncate()
                            .text_size(px(13.0))
                            .text_color(theme::text_dim())
                            .child(saves),
                    )
                    .child(
                        small_button("saves-browse", "Browse", false)
                            .on_click(cx.listener(|this, _, _, cx| this.browse_file(BrowseTarget::Saves, cx))),
                    )
                    .child(small_button("saves-clear", "Default", false).on_click(cx.listener(|this, _, _, cx| {
                        this.state.choices.saves_dir = PathBuf::new();
                        this.state.config_dirty = true;
                        cx.notify();
                    }))),
            ))
    }

    fn picker_list<T: Clone + PartialEq + 'static>(
        &self,
        id: &'static str,
        items: Vec<(String, T)>,
        current: T,
        cx: &mut Context<Self>,
        apply: fn(&mut Self, T),
    ) -> impl IntoElement {
        let mut list = div()
            .id(id)
            .flex()
            .flex_col()
            .max_h(px(220.0))
            .overflow_y_scroll()
            .rounded(px(4.0))
            .border_1()
            .border_color(theme::panel_border())
            .bg(gpui::rgb(0x08101a));
        for (index, (label, value)) in items.into_iter().enumerate() {
            let chosen = value == current;
            list = list.child(
                div()
                    .id(index)
                    .px(px(12.0))
                    .py(px(5.0))
                    .text_size(px(13.5))
                    .cursor_pointer()
                    .when(chosen, |row| row.text_color(theme::gold()))
                    .hover(|style| style.bg(theme::panel_hover()))
                    .child(label)
                    .on_click(cx.listener(move |this, _, _, cx| {
                        apply(this, value.clone());
                        this.state.config_dirty = true;
                        this.picker = None;
                        cx.notify();
                    })),
            );
        }
        list
    }

    fn render_option(&mut self, option: &'static crate::config::OptionDef, cx: &mut Context<Self>) -> AnyElement {
        let key = option.key;
        let value = self.state.settings.get(key);
        let control: AnyElement = match option.kind {
            OptionKind::Toggle => toggle(key, value != 0.0)
                .on_click(cx.listener(move |this, _, _, cx| {
                    let on = this.state.settings.get(key) != 0.0;
                    this.set_option(key, if on { 0.0 } else { 1.0 }, cx);
                }))
                .into_any_element(),
            OptionKind::Choice(labels) => {
                let selected = (value.clamp(option.minimum, option.maximum) as usize).min(labels.len() - 1);
                segmented(
                    key,
                    labels,
                    selected,
                    cx.listener(move |this, index: &usize, _, cx| this.set_option(key, *index as f32, cx)),
                )
                .into_any_element()
            }
            OptionKind::Slider => self.slider(option, value, cx).into_any_element(),
            OptionKind::Integer => {
                let presets: [(f32, &str); 6] = [
                    (0.0, "Default"),
                    (1280.0, "1280"),
                    (1600.0, "1600"),
                    (1920.0, "1920"),
                    (2560.0, "2560"),
                    (3840.0, "3840"),
                ];
                let heights: [(f32, &str); 6] = [
                    (0.0, "Default"),
                    (720.0, "720"),
                    (900.0, "900"),
                    (1080.0, "1080"),
                    (1440.0, "1440"),
                    (2160.0, "2160"),
                ];
                let list = if key == "graphics.height" { heights } else { presets };
                let (minimum, maximum) = (option.minimum, option.maximum);
                div()
                    .flex()
                    .flex_row()
                    .items_center()
                    .gap(px(6.0))
                    .child(stepper(SharedString::from(format!("{key}-down")), "-").on_click(cx.listener(
                        move |this, _, _, cx| {
                            let next = (this.state.settings.get(key) - 10.0).clamp(minimum, maximum);
                            this.set_option(key, next, cx);
                        },
                    )))
                    .child(div().w(px(56.0)).text_size(px(14.0)).text_center().child(format!("{}", value as i32)))
                    .child(stepper(SharedString::from(format!("{key}-up")), "+").on_click(cx.listener(
                        move |this, _, _, cx| {
                            let next = (this.state.settings.get(key) + 10.0).clamp(minimum, maximum);
                            this.set_option(key, next, cx);
                        },
                    )))
                    .child(div().w(px(8.0)))
                    .children(list.into_iter().map(|(preset, label)| {
                        chip(SharedString::from(format!("{key}-{label}")), label, value == preset)
                            .on_click(cx.listener(move |this, _, _, cx| this.set_option(key, preset, cx)))
                    }))
                    .into_any_element()
            }
        };
        setting_row(option.label, control).into_any_element()
    }

    fn slider(
        &mut self,
        option: &'static crate::config::OptionDef,
        value: f32,
        cx: &mut Context<Self>,
    ) -> impl IntoElement {
        let key = option.key;
        let (minimum, maximum) = (option.minimum, option.maximum);
        let bounds = self.slider_bounds.entry(key).or_insert_with(|| Rc::new(Cell::new(Bounds::default()))).clone();
        let fraction = ((value - minimum) / (maximum - minimum)).clamp(0.0, 1.0);
        let pick = move |bounds: &Rc<Cell<Bounds<Pixels>>>, x: Pixels| -> f32 {
            let area = bounds.get();
            let width = f32::from(area.size.width).max(1.0);
            let at = (f32::from(x - area.origin.x) / width).clamp(0.0, 1.0);
            (minimum + at * (maximum - minimum)).round()
        };
        let down_bounds = bounds.clone();
        let move_bounds = bounds.clone();
        div()
            .flex()
            .flex_row()
            .items_center()
            .gap(px(14.0))
            .child(
                div()
                    .id(SharedString::from(format!("{key}-slider")))
                    .relative()
                    .w(px(240.0))
                    .h(px(22.0))
                    .cursor_pointer()
                    .child(
                        canvas(move |area, _, _| bounds.set(area), |_, (), _, _| {})
                            .absolute()
                            .top_0()
                            .left_0()
                            .size_full(),
                    )
                    .child(
                        div()
                            .absolute()
                            .top(px(9.0))
                            .left_0()
                            .w_full()
                            .h(px(4.0))
                            .rounded(px(2.0))
                            .bg(gpui::rgb(0x263241)),
                    )
                    .child(
                        div()
                            .absolute()
                            .top(px(9.0))
                            .left_0()
                            .w(gpui::relative(fraction))
                            .h(px(4.0))
                            .rounded(px(2.0))
                            .bg(theme::gold()),
                    )
                    .child(
                        div()
                            .absolute()
                            .top(px(4.0))
                            .left(gpui::relative(fraction))
                            .ml(px(-7.0))
                            .size(px(14.0))
                            .rounded_full()
                            .bg(theme::text())
                            .border_2()
                            .border_color(theme::gold()),
                    )
                    .on_mouse_down(
                        MouseButton::Left,
                        cx.listener(move |this, event: &MouseDownEvent, _, cx| {
                            this.dragging = Some(key);
                            let next = pick(&down_bounds, event.position.x);
                            this.set_option(key, next, cx);
                        }),
                    )
                    .on_mouse_move(cx.listener(move |this, event: &MouseMoveEvent, _, cx| {
                        if this.dragging == Some(key) && event.pressed_button == Some(MouseButton::Left) {
                            let next = pick(&move_bounds, event.position.x);
                            if next != this.state.settings.get(key) {
                                this.set_option(key, next, cx);
                            }
                        }
                    })),
            )
            .child(div().w(px(36.0)).text_size(px(14.0)).text_right().child(format!("{}", value as i32)))
    }
}

// ---- settings widgets -----------------------------------------------------------------------------------------------

fn section_box(title: &str, note: Option<&str>) -> gpui::Div {
    div()
        .flex()
        .flex_col()
        .flex_none()
        .gap(px(4.0))
        .p(px(18.0))
        .rounded(px(6.0))
        .border_1()
        .border_color(theme::panel_border())
        .bg(gpui::rgba(0x0b1521cc))
        .child(caps(title, 14.0).font_weight(FontWeight::SEMIBOLD).text_color(theme::gold()).mb(px(4.0)))
        .when_some(note, |section, note| {
            section.child(
                div()
                    .text_size(px(13.0))
                    .line_height(px(19.0))
                    .text_color(theme::text_dim())
                    .mb(px(6.0))
                    .child(note.to_owned()),
            )
        })
}

fn setting_row(label: &str, control: impl IntoElement) -> gpui::Div {
    div()
        .flex()
        .flex_row()
        .items_center()
        .min_h(px(40.0))
        .border_b_1()
        .border_color(gpui::rgba(0x26324166))
        .child(div().w(px(250.0)).flex_none().text_size(px(14.5)).text_color(theme::text()).child(label.to_owned()))
        .child(control)
}

fn toggle(key: &'static str, on: bool) -> gpui::Stateful<gpui::Div> {
    div()
        .id(SharedString::new_static(key))
        .relative()
        .w(px(40.0))
        .h(px(22.0))
        .rounded_full()
        .cursor_pointer()
        .border_1()
        .border_color(if on { theme::gold_border() } else { theme::panel_border() })
        .bg(if on { gpui::rgb(0x6e5527) } else { gpui::rgb(0x111b27) })
        .child(
            div()
                .absolute()
                .top(px(3.0))
                .left(px(if on { 21.0 } else { 3.0 }))
                .size(px(14.0))
                .rounded_full()
                .bg(if on { theme::gold() } else { theme::text_muted() }),
        )
}

fn segmented(
    id: &str,
    labels: &[&str],
    selected: usize,
    on_pick: impl Fn(&usize, &mut Window, &mut App) + 'static,
) -> impl IntoElement {
    let on_pick = Rc::new(on_pick);
    let mut row =
        div().flex().flex_row().rounded(px(4.0)).border_1().border_color(theme::panel_border()).overflow_hidden();
    for (index, label) in labels.iter().enumerate() {
        let chosen = index == selected;
        let on_pick = on_pick.clone();
        row = row.child(
            div()
                .id(SharedString::from(format!("{id}-{index}")))
                .px(px(14.0))
                .py(px(6.0))
                .text_size(px(13.5))
                .cursor_pointer()
                .when(index > 0, |cell| cell.border_l_1().border_color(theme::panel_border()))
                .when(chosen, |cell| cell.bg(gpui::rgb(0x3b2f19)).text_color(theme::gold()))
                .when(!chosen, |cell| cell.text_color(theme::text_dim()).hover(|style| style.bg(theme::panel_hover())))
                .child((*label).to_owned())
                .on_click(move |_, window, cx| on_pick(&index, window, cx)),
        );
    }
    row
}

fn dropdown_button(id: &'static str, label: &str, width: f32) -> gpui::Stateful<gpui::Div> {
    div()
        .id(id)
        .flex()
        .flex_row()
        .items_center()
        .justify_between()
        .w(px(width))
        .h(px(32.0))
        .px(px(12.0))
        .rounded(px(4.0))
        .border_1()
        .border_color(theme::panel_border())
        .bg(gpui::rgb(0x0a131e))
        .cursor_pointer()
        .hover(|style| style.border_color(gpui::rgb(0x4a5566)))
        .child(div().text_size(px(13.5)).truncate().child(label.to_owned()))
        .child(icon("icons/chevron-down.svg", 14.0, theme::text_dim()))
}

fn small_button(id: &'static str, label: &str, primary: bool) -> gpui::Stateful<gpui::Div> {
    div()
        .id(id)
        .px(px(16.0))
        .h(px(32.0))
        .flex()
        .items_center()
        .rounded(px(4.0))
        .border_1()
        .cursor_pointer()
        .when(primary, |button| {
            button
                .border_color(theme::play_border())
                .bg(linear_gradient(
                    180.0,
                    linear_color_stop(theme::play_top(), 0.0),
                    linear_color_stop(theme::play_bottom(), 1.0),
                ))
                .text_color(theme::play_text())
                .hover(|style| style.opacity(0.9))
        })
        .when(!primary, |button| button.border_color(gpui::rgb(0x323c4a)).hover(|style| style.bg(theme::panel_hover())))
        .child(caps(label, 12.5).font_weight(FontWeight::SEMIBOLD))
}

fn stepper(id: SharedString, label: &'static str) -> gpui::Stateful<gpui::Div> {
    div()
        .id(id)
        .size(px(28.0))
        .flex()
        .items_center()
        .justify_center()
        .rounded(px(4.0))
        .border_1()
        .border_color(theme::panel_border())
        .cursor_pointer()
        .hover(|style| style.bg(theme::panel_hover()))
        .text_size(px(15.0))
        .child(label)
}

fn chip(id: SharedString, label: &'static str, on: bool) -> gpui::Stateful<gpui::Div> {
    div()
        .id(id)
        .px(px(9.0))
        .py(px(4.0))
        .rounded(px(4.0))
        .text_size(px(12.5))
        .cursor_pointer()
        .border_1()
        .border_color(if on { theme::gold_border() } else { theme::panel_border() })
        .text_color(if on { theme::gold() } else { theme::text_dim() })
        .hover(|style| style.bg(theme::panel_hover()))
        .child(label)
}
