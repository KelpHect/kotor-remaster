//! Embedded assets: the UI font (Barlow, SIL OFL 1.1, see `assets/fonts/OFL.txt`) and the project's own
//! line icons, drawn for this launcher. No game art, logo or font is embedded.

use std::borrow::Cow;

use gpui::{AssetSource, Result, SharedString};

pub const FONT_FAMILY: &str = "Barlow";

pub const FONTS: [&[u8]; 3] = [
    include_bytes!("../assets/fonts/Barlow-Regular.ttf"),
    include_bytes!("../assets/fonts/Barlow-Medium.ttf"),
    include_bytes!("../assets/fonts/Barlow-SemiBold.ttf"),
];

macro_rules! icon {
    ($body:expr) => {
        concat!(
            r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="white" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">"#,
            $body,
            "</svg>"
        )
    };
}

const ICONS: &[(&str, &str)] = &[
    ("icons/home.svg", icon!(r#"<path d="M3 11.2 12 3.5l9 7.7"/><path d="M5.5 9.5V20h5v-6h3v6h5V9.5"/>"#)),
    (
        "icons/gear.svg",
        icon!(
            r#"<circle cx="12" cy="12" r="3.2"/><path d="M12 2.8v2.4M12 18.8v2.4M2.8 12h2.4M18.8 12h2.4M5.5 5.5l1.7 1.7M16.8 16.8l1.7 1.7M5.5 18.5l1.7-1.7M16.8 7.2l1.7-1.7"/><circle cx="12" cy="12" r="6.3"/>"#
        ),
    ),
    (
        "icons/shield.svg",
        icon!(
            r#"<path d="M12 2.8 4.5 5.8v5.6c0 4.6 3.2 8.3 7.5 9.8 4.3-1.5 7.5-5.2 7.5-9.8V5.8z"/><path d="m8.6 12 2.4 2.4 4.4-4.6"/>"#
        ),
    ),
    (
        "icons/play.svg",
        r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill="white" d="M6 3.5v17l14-8.5z"/></svg>"#,
    ),
    ("icons/engine.svg", icon!(r#"<path d="M4 6h16M4 12h10M4 18h16"/><circle cx="18" cy="12" r="2"/>"#)),
    ("icons/display.svg", icon!(r#"<circle cx="12" cy="12" r="8.6"/><path d="m9 15 1.8-4.2L15 9l-1.8 4.2z"/>"#)),
    (
        "icons/api.svg",
        icon!(r#"<circle cx="12" cy="12" r="8.6"/><path d="M15.2 9.2a4 4 0 1 0 0 5.6"/><path d="M12 12h3.6"/>"#),
    ),
    ("icons/folder.svg", icon!(r#"<path d="M3 6.5h6l2 2.2h10V19H3z"/>"#)),
    ("icons/min.svg", icon!(r#"<path d="M5 12h14"/>"#)),
    ("icons/max.svg", icon!(r#"<rect x="5" y="5" width="14" height="14"/>"#)),
    ("icons/restore.svg", icon!(r#"<rect x="5" y="8" width="11" height="11"/><path d="M8 5h11v11"/>"#)),
    ("icons/close.svg", icon!(r#"<path d="M5.5 5.5l13 13M18.5 5.5l-13 13"/>"#)),
    ("icons/chevron.svg", icon!(r#"<path d="m9 5 7 7-7 7"/>"#)),
    ("icons/chevron-down.svg", icon!(r#"<path d="m5 9 7 7 7-7"/>"#)),
    ("icons/back.svg", icon!(r#"<path d="m15 5-7 7 7 7"/>"#)),
    ("icons/check.svg", icon!(r#"<path d="m5 12.5 4.5 4.5L19 7.5"/>"#)),
    ("icons/refresh.svg", icon!(r#"<path d="M19.5 12a7.5 7.5 0 1 1-2.2-5.3"/><path d="M19.5 4.5v4.2h-4.2"/>"#)),
    ("icons/warning.svg", icon!(r#"<path d="M12 3.5 2.5 20h19z"/><path d="M12 10v4.5M12 17.2v.1"/>"#)),
    // The launcher's own mark: two nested chevrons. Abstract, not derived from any game or brand.
    (
        "icons/mark.svg",
        r#"<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><path fill="white" d="M4 8h14l14 34L46 8h14L32 58z"/><path fill="white" opacity=".55" d="M20 8h6l6 15 6-15h6L32 38z"/></svg>"#,
    ),
];

pub struct Assets;

impl AssetSource for Assets {
    fn load(&self, path: &str) -> Result<Option<Cow<'static, [u8]>>> {
        Ok(ICONS.iter().find(|(name, _)| *name == path).map(|(_, svg)| Cow::Borrowed(svg.as_bytes())))
    }

    fn list(&self, path: &str) -> Result<Vec<SharedString>> {
        Ok(ICONS.iter().filter(|(name, _)| name.starts_with(path)).map(|(name, _)| SharedString::from(*name)).collect())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn icons_load_and_fonts_embed() {
        assert!(Assets.load("icons/home.svg").unwrap().is_some());
        assert!(Assets.load("icons/none.svg").unwrap().is_none());
        assert_eq!(Assets.list("icons/").unwrap().len(), ICONS.len());
        assert!(FONTS.iter().all(|font| font.len() > 10_000));
    }
}
