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
use crate::theme::{self, tracked};

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
        let receiver =
            cx.prompt_for_paths(PathPromptOptions { files: !directories, directories, multiple: false, prompt: None });
        cx.spawn(async move |this, cx| {
            if let Ok(Ok(Some(paths))) = receiver.await
                && let Some(path) = paths.into_iter().next()
            {
                this.update(cx, |this, cx| {
                    match target {
                        BrowseTarget::Saves => this.state.choices.saves_dir = path,
                        BrowseTarget::Font => this.state.choices.font_file = path,
                    }
                    this.state.config_dirty = true;
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
}

// ---- small building blocks ------------------------------------------------------------------------------------------

fn icon(name: &'static str, size_px: f32, color: impl Into<gpui::Hsla>) -> impl IntoElement {
    svg().path(SharedString::new_static(name)).size(px(size_px)).flex_none().text_color(color.into())
}

fn caps(text: &str, size_px: f32) -> gpui::Div {
    div().text_size(px(size_px)).whitespace_nowrap().child(tracked(&text.to_uppercase()))
}

fn hline() -> gpui::Div {
    div().h(px(1.0)).w_full().bg(theme::divider())
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
fn art_layer(path: Option<&PathBuf>, seed: u32, hue_shift: bool) -> AnyElement {
    match path {
        Some(path) => {
            img(path.clone()).object_fit(ObjectFit::Cover).absolute().top_0().left_0().size_full().into_any_element()
        }
        None => {
            let (top, bottom) = if hue_shift { (0x1b1534, 0x070b18) } else { (0x0f2038, 0x060b16) };
            div()
                .absolute()
                .top_0()
                .left_0()
                .size_full()
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

/// A game name as two title lines: "Knights of the" / "Old Republic".
fn title_lines(name: &str) -> (&str, &str) {
    match name.find(" Old ") {
        Some(at) => (&name[..at], &name[at + 1..]),
        None => ("", name),
    }
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
        let body = match self.view {
            View::Home => self.render_home(cx).into_any_element(),
            View::Settings => self.render_settings(cx).into_any_element(),
        };
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
            .border_1()
            .border_color(gpui::rgb(0x1c2939))
            .on_mouse_up(MouseButton::Left, cx.listener(|this, _, _, _| this.dragging = None))
            .child(starfield(7, 90, 0.35))
            .child(self.render_titlebar(window))
            .child(div().flex().flex_row().w_full().flex_1().min_h_0().child(self.render_rail(cx)).child(
                div().flex_1().min_w_0().overflow_hidden().flex().flex_col().pr(px(16.0)).pl(px(14.0)).child(body),
            ))
            .when_some(self.verify.as_ref(), |root, verify| {
                root.child(gpui::deferred(self.render_verify(verify, cx)).with_priority(1))
            })
    }
}

impl LauncherView {
    fn render_titlebar(&self, window: &mut Window) -> impl IntoElement {
        let maximized = window.is_maximized();
        let control = |id: &'static str, icon_name: &'static str, area: WindowControlArea, close: bool| {
            div()
                .id(id)
                .w(px(52.0))
                .h(px(40.0))
                .flex()
                .items_center()
                .justify_center()
                .window_control_area(area)
                .hover(move |style| style.bg(if close { gpui::rgba(0xc4302bcc) } else { gpui::rgba(0xffffff14) }))
                .child(icon(icon_name, 20.0, theme::text()))
        };
        div()
            .id("titlebar")
            .relative()
            .flex()
            .flex_row()
            .w_full()
            .h(px(100.0))
            .flex_none()
            .window_control_area(WindowControlArea::Drag)
            .child(
                div()
                    .flex()
                    .flex_row()
                    .items_center()
                    .pl(px(16.0))
                    .pt(px(10.0))
                    .gap(px(16.0))
                    .child(icon("icons/mark.svg", 62.0, theme::text()))
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .child(
                                caps(theme::PRODUCT_NAME, 30.0).font_weight(FontWeight::MEDIUM).line_height(px(36.0)),
                            )
                            .child(
                                div()
                                    .flex()
                                    .flex_row()
                                    .items_center()
                                    .gap(px(14.0))
                                    .mt(px(4.0))
                                    .child(caps("Launcher", 17.0).text_color(theme::text_dim()))
                                    .child(div().w(px(190.0)).h(px(1.0)).bg(theme::text_muted())),
                            )
                            .child(caps(theme::PRODUCT_LINE, 11.5).mt(px(9.0)).text_color(theme::text_dim())),
                    ),
            )
            .child(div().flex_1())
            .child(
                div()
                    .flex()
                    .flex_row()
                    .pt(px(14.0))
                    .pr(px(10.0))
                    .gap(px(2.0))
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
            div()
                .id(id)
                .flex()
                .flex_row()
                .items_center()
                .h(px(34.0))
                .pl(px(18.0))
                .gap(px(13.0))
                .rounded(px(4.0))
                .cursor_pointer()
                .hover(|style| style.bg(theme::panel_hover()))
                .child(icon(icon_name, 21.0, if active { theme::text() } else { theme::text_dim() }))
                .child(caps(label, 15.0).font_weight(FontWeight::MEDIUM).text_color(if active {
                    theme::text()
                } else {
                    theme::text_dim()
                }))
                .on_click(cx.listener(move |this, _, _, cx| {
                    this.view = view;
                    this.picker = None;
                    cx.notify();
                }))
        };
        let mut games = div().flex().flex_col();
        for game in game_catalog() {
            let found = self.state.install_of(game.id);
            let selected = found.is_some_and(|(index, _)| Some(index) == self.state.selected);
            let (status, status_color) = status_of(found.map(|(_, install)| install));
            let version = found
                .map(|(_, install)| install.report.version.clone())
                .filter(|version| !version.is_empty())
                .map_or_else(|| "Data".to_owned(), |version| format!("Data {version}"));
            let art = self.art.get(game.id).and_then(|art| art.header.as_ref());
            let index = found.map(|(index, _)| index);
            games = games.child(
                div()
                    .id(SharedString::from(format!("card-{}", game.id)))
                    .relative()
                    .w(px(258.0))
                    .h(px(104.0))
                    .ml(px(2.0))
                    .mt(px(if game.id == "kotor1" { 10.0 } else { 18.0 }))
                    .rounded(px(6.0))
                    .overflow_hidden()
                    .border_1()
                    .border_color(if selected { theme::gold_border() } else { theme::panel_border() })
                    .when(index.is_some(), |card| card.cursor_pointer())
                    .when(found.is_none(), |card| card.opacity(0.55))
                    .child(art_layer(art, 11 + game.steam_app_id % 97, game.id == "kotor2"))
                    .when(art.is_none(), |card| {
                        card.child(
                            div()
                                .absolute()
                                .left(px(14.0))
                                .bottom(px(12.0))
                                .right(px(10.0))
                                .child(caps(game.name, 15.0).font_weight(FontWeight::SEMIBOLD).whitespace_normal()),
                        )
                    })
                    .when(selected, |card| {
                        card.shadow(vec![BoxShadow {
                            color: theme::hsla(gpui::rgba(0xdcb46233)),
                            offset: point(px(0.0), px(0.0)),
                            blur_radius: px(10.0),
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
                    .gap(px(8.0))
                    .mt(px(9.0))
                    .ml(px(12.0))
                    .child(caps(&version, 12.0).text_color(theme::text_dim()))
                    .child(div().w(px(1.0)).h(px(11.0)).bg(theme::text_muted()))
                    .child(caps(status, 12.0).text_color(status_color)),
            );
        }
        let current = self.view;
        div()
            .flex()
            .flex_col()
            .w(px(280.0))
            .flex_none()
            .pl(px(8.0))
            .pt(px(22.0))
            .child(nav("nav-home", "icons/home.svg", "Home", View::Home, current, cx))
            .child(div().h(px(6.0)))
            .child(nav("nav-settings", "icons/gear.svg", "Settings", View::Settings, current, cx))
            .child(hline().mt(px(14.0)).w(px(262.0)))
            .child(
                caps("Your games", 13.0)
                    .font_weight(FontWeight::MEDIUM)
                    .mt(px(18.0))
                    .ml(px(12.0))
                    .text_color(theme::text_dim()),
            )
            .child(games)
            .child(hline().mt(px(14.0)).w(px(262.0)))
            .child(self.render_news())
    }

    fn render_news(&self) -> impl IntoElement {
        let mut panel = div()
            .flex()
            .flex_col()
            .mt(px(13.0))
            .w(px(260.0))
            .flex_1()
            .mb(px(10.0))
            .min_h(px(120.0))
            .p(px(14.0))
            .rounded(px(6.0))
            .border_1()
            .border_color(theme::panel_border())
            .bg(gpui::rgba(0x0b152199))
            .child(caps("News & updates", 14.0).font_weight(FontWeight::SEMIBOLD).text_color(theme::text_dim()));
        for item in self.news.iter().take(1) {
            panel = panel.child(div().mt(px(10.0)).text_size(px(14.0)).child(item.title.clone())).child(
                div()
                    .mt(px(4.0))
                    .text_size(px(13.0))
                    .line_height(px(19.0))
                    .text_color(theme::text_dim())
                    .child(item.body.clone()),
            );
            if item.link.is_some() {
                panel = panel.child(
                    div()
                        .flex()
                        .flex_row()
                        .justify_end()
                        .items_center()
                        .gap(px(6.0))
                        .mt_auto()
                        .child(caps("View all news", 12.0).text_color(theme::text_dim()))
                        .child(icon("icons/chevron.svg", 12.0, theme::text_dim())),
                );
            }
        }
        panel
    }

    fn render_hero(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let mut hero = div()
            .flex()
            .flex_row()
            .w_full()
            .h(px(248.0))
            .flex_none()
            .rounded(px(8.0))
            .overflow_hidden()
            .border_1()
            .border_color(gpui::rgb(0x5a4630));
        let games = game_catalog();
        for (position, game) in games.iter().enumerate() {
            let found = self.state.install_of(game.id);
            let selected = found.is_some_and(|(index, _)| Some(index) == self.state.selected);
            let index = found.map(|(index, _)| index);
            let art = self.art.get(game.id).and_then(|art| art.hero.as_ref());
            let first = position == 0;
            let pane = div()
                .id(SharedString::from(format!("hero-{}", game.id)))
                .relative()
                .h_full()
                .overflow_hidden()
                .when(first, |pane| pane.w(gpui::relative(0.56)))
                .when(!first, |pane| pane.flex_1().border_l_2().border_color(gpui::rgb(0x8c6d3c)))
                .when(index.is_some(), |pane| pane.cursor_pointer())
                .child(art_layer(art, 31 + game.steam_app_id % 89, !first))
                .child(
                    // Readability veil over the image, heavier on the pane that is not selected.
                    div().absolute().top_0().left_0().size_full().bg(linear_gradient(
                        90.0,
                        linear_color_stop(gpui::rgba(if selected { 0x05091240 } else { 0x050912a0 }), 0.0),
                        linear_color_stop(gpui::rgba(if selected { 0x05091210 } else { 0x05091280 }), 1.0),
                    )),
                )
                .child(
                    // Darker toward the title so it reads over any art.
                    div().absolute().top_0().left_0().size_full().bg(linear_gradient(
                        90.0,
                        linear_color_stop(gpui::rgba(0x05091200), if first { 0.25 } else { 0.0 }),
                        linear_color_stop(gpui::rgba(if first { 0x050912b8 } else { 0x05091270 }), 1.0),
                    )),
                )
                .child({
                    let (top, bottom) = title_lines(game.name);
                    let colour = if selected { theme::text() } else { theme::text_dim() };
                    div()
                        .absolute()
                        .top_0()
                        .bottom_0()
                        .left(gpui::relative(if first { 0.30 } else { 0.0 }))
                        .right_0()
                        .flex()
                        .flex_col()
                        .items_center()
                        .justify_center()
                        .child(caps(top, 21.0).font_weight(FontWeight::MEDIUM).text_color(colour))
                        .child(
                            caps(bottom, if first { 40.0 } else { 36.0 })
                                .font_weight(FontWeight::SEMIBOLD)
                                .line_height(px(46.0))
                                .text_color(if selected && first { theme::gold() } else { colour }),
                        )
                        .child(
                            div()
                                .flex()
                                .flex_row()
                                .items_center()
                                .gap(px(12.0))
                                .mt(px(16.0))
                                .child(div().w(px(34.0)).h(px(1.0)).bg(theme::gold_line()))
                                .child(
                                    caps(theme::PRODUCT_NAME, 13.0)
                                        .font_weight(FontWeight::MEDIUM)
                                        .text_color(theme::text_dim()),
                                )
                                .child(div().w(px(34.0)).h(px(1.0)).bg(theme::gold_line())),
                        )
                })
                .on_click(cx.listener(move |this, _, _, cx| {
                    if let Some(index) = index {
                        this.state.select(index);
                        this.state.config_dirty = true;
                        cx.notify();
                    }
                }));
            hero = hero.child(pane);
        }
        hero
    }

    fn render_home(&self, cx: &mut Context<Self>) -> impl IntoElement {
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
                .h(px(48.0))
                .mx(px(12.0))
                .when(!last, |row| row.border_b_1().border_color(theme::panel_border()))
                .child(div().w(px(36.0)).pl(px(6.0)).child(icon(icon_name, 21.0, theme::text_dim())))
                .child(
                    caps(label, 13.0)
                        .font_weight(FontWeight::MEDIUM)
                        .w(px(112.0))
                        .flex_none()
                        .text_color(theme::text_dim()),
                )
                .child(div().flex_1().min_w_0().text_size(px(14.0)).truncate().child(value))
        };
        let plan = self.state.plan();
        let ready = plan.ok() && !self.scanning;
        let (state_text, state_color) = if self.scanning {
            ("Scanning for installs".to_owned(), theme::text_dim())
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
                .justify_center()
                .gap(px(14.0))
                .w(px(width))
                .h(px(56.0))
                .rounded(px(4.0))
                .border_1()
                .border_color(gpui::rgb(0x323c4a))
                .bg(gpui::rgba(0x0b1521cc))
                .cursor_pointer()
                .hover(|style| style.bg(theme::panel_hover()).border_color(gpui::rgb(0x4a5566)))
                .child(icon(icon_name, 26.0, theme::text()))
                .child(caps(label, 15.5).font_weight(FontWeight::MEDIUM))
        };
        let install_path = if self.state.choices.install.as_os_str().is_empty() {
            "Not chosen".to_owned()
        } else {
            native_path(&self.state.choices.install)
        };

        div()
            .flex()
            .flex_col()
            .size_full()
            .child(self.render_hero(cx))
            .child(
                div()
                    .flex()
                    .flex_row()
                    .mt(px(16.0))
                    .child(
                        div()
                            .flex_1()
                            .min_w_0()
                            .flex()
                            .flex_col()
                            .pl(px(18.0))
                            .pr(px(24.0))
                            .child(div().h(px(1.0)).w(px(520.0)).bg(theme::gold_line()))
                            .child(
                                caps(title, 22.0)
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .mt(px(18.0))
                                    .pl(px(18.0))
                                    .text_color(theme::gold()),
                            )
                            .child(
                                caps(theme::PRODUCT_NAME, 14.5).mt(px(6.0)).pl(px(18.0)).text_color(theme::text_dim()),
                            )
                            .child(
                                div()
                                    .mt(px(12.0))
                                    .pl(px(18.0))
                                    .text_size(px(15.0))
                                    .line_height(px(24.0))
                                    .text_color(gpui::rgb(0xc3c7ce))
                                    .line_clamp(3)
                                    .child(description),
                            ),
                    )
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .w(px(384.0))
                            .flex_none()
                            .mt(px(12.0))
                            .mr(px(18.0))
                            .rounded(px(6.0))
                            .border_1()
                            .border_color(theme::panel_border())
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
                    .mt(px(26.0))
                    .pl(px(36.0))
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .child(
                                div()
                                    .id("play")
                                    .relative()
                                    .flex()
                                    .flex_row()
                                    .items_center()
                                    .justify_center()
                                    .w(px(352.0))
                                    .h(px(57.0))
                                    .rounded(px(4.0))
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
                                    .child(caps("Play", 25.0).font_weight(FontWeight::SEMIBOLD).text_color(if ready {
                                        theme::play_text()
                                    } else {
                                        theme::text_muted()
                                    }))
                                    .child(div().absolute().right(px(50.0)).top(px(16.0)).child(icon(
                                        "icons/play.svg",
                                        25.0,
                                        if ready { theme::play_text() } else { theme::text_muted() },
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
                                    .ml(px(6.0))
                                    .w(px(352.0))
                                    .child(div().size(px(8.0)).flex_none().rounded_full().bg(state_color))
                                    .child(
                                        caps(&state_text, 13.0)
                                            .font_weight(FontWeight::MEDIUM)
                                            .text_color(theme::text_dim())
                                            .truncate()
                                            .min_w_0(),
                                    ),
                            ),
                    )
                    .child(div().w(px(53.0)))
                    .child(
                        outline_button("verify", "icons/shield.svg", "Verify files", 230.0)
                            .on_click(cx.listener(|this, _, _, cx| this.verify_files(cx))),
                    )
                    .child(div().w(px(21.0)))
                    .child(outline_button("game-settings", "icons/gear.svg", "Game settings", 246.0).on_click(
                        cx.listener(|this, _, _, cx| {
                            this.view = View::Settings;
                            cx.notify();
                        }),
                    )),
            )
            .child(div().flex_1())
            .child(self.render_footer(install_path, cx))
    }

    fn render_footer(&self, install_path: String, cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .flex()
            .flex_row()
            .items_center()
            .w_full()
            .h(px(62.0))
            .flex_none()
            .border_t_1()
            .border_color(theme::divider())
            .px(px(22.0))
            .child(
                div()
                    .flex()
                    .flex_col()
                    .min_w_0()
                    .flex_1()
                    .child(
                        caps("Install location", 11.0).font_weight(FontWeight::MEDIUM).text_color(theme::text_muted()),
                    )
                    .child(
                        div()
                            .flex()
                            .flex_row()
                            .items_center()
                            .gap(px(12.0))
                            .mt(px(5.0))
                            .child(
                                div()
                                    .text_size(px(12.5))
                                    .text_color(theme::text_dim())
                                    .truncate()
                                    .max_w(px(420.0))
                                    .child(install_path),
                            )
                            .child(div().w(px(1.0)).h(px(12.0)).bg(theme::text_muted()))
                            .child(
                                div()
                                    .id("change")
                                    .cursor_pointer()
                                    .hover(|style| style.text_color(theme::gold()))
                                    .child(
                                        caps("Change", 12.0)
                                            .font_weight(FontWeight::MEDIUM)
                                            .text_color(theme::text_dim()),
                                    )
                                    .on_click(cx.listener(|this, _, _, cx| this.browse_install(cx))),
                            )
                            .when(!self.state.status.is_empty(), |row| {
                                row.child(
                                    div()
                                        .text_size(px(12.0))
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
                    .flex_col()
                    .items_end()
                    .flex_none()
                    .text_size(px(11.0))
                    .line_height(px(16.0))
                    .text_color(theme::text_muted())
                    .child(theme::DISCLAIMER[0])
                    .child(theme::DISCLAIMER[1]),
            )
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
        section.child(setting_row(
            "Saves folder",
            div()
                .flex()
                .flex_row()
                .items_center()
                .gap(px(8.0))
                .child(div().max_w(px(260.0)).truncate().text_size(px(13.0)).text_color(theme::text_dim()).child(saves))
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
