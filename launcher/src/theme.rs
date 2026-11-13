//! Colours, sizes and text helpers for the launcher's look: dark navy, gold accents, wide-tracked
//! capitals. Every value lives here so the screens stay consistent.

use gpui::{Hsla, Rgba, rgb};

/// The product name. The docs give no name for the engine yet (ADR 0003 open question 4: "Public-release
/// legal review and naming"), so this neutral placeholder is the one place to change it.
pub const PRODUCT_NAME: &str = "Remaster Engine";
pub const PRODUCT_LINE: &str = "Independent open-source engine";
pub const DISCLAIMER: [&str; 2] = [
    "Not affiliated with or endorsed by the makers or publishers of any game it can run.",
    "It needs your own copy of the game and ships none of its files.",
];

/// The engine version from `engine/CMakeLists.txt`, read at build time.
pub const ENGINE_VERSION: &str = env!("KE_ENGINE_VERSION");

pub fn bg_top() -> Rgba {
    rgb(0x0a1525)
}
pub fn bg_bottom() -> Rgba {
    rgb(0x050c16)
}
pub fn panel() -> Rgba {
    rgb(0x0b1521)
}
pub fn panel_hover() -> Rgba {
    rgb(0x111e2d)
}
pub fn panel_border() -> Rgba {
    rgb(0x263241)
}
pub fn divider() -> Rgba {
    rgb(0x18232f)
}
pub fn gold() -> Rgba {
    rgb(0xdcb462)
}
pub fn gold_border() -> Rgba {
    rgb(0x8c6d3c)
}
pub fn gold_line() -> Rgba {
    rgb(0x5f5442)
}
pub fn text() -> Rgba {
    rgb(0xe6e9ef)
}
pub fn text_dim() -> Rgba {
    rgb(0xa4a9b2)
}
pub fn text_muted() -> Rgba {
    rgb(0x67707b)
}
pub fn green() -> Rgba {
    rgb(0x5fc15e)
}
pub fn amber() -> Rgba {
    rgb(0xe3a948)
}
pub fn red() -> Rgba {
    rgb(0xe36a5c)
}
pub fn play_top() -> Rgba {
    rgb(0xd8b463)
}
pub fn play_bottom() -> Rgba {
    rgb(0x8d6a27)
}
pub fn play_border() -> Rgba {
    rgb(0xecd08c)
}
pub fn play_text() -> Rgba {
    rgb(0x22180a)
}
pub fn hsla(color: Rgba) -> Hsla {
    color.into()
}

/// Wide letter spacing for capitals (the text system has no tracking): a hair space between letters,
/// three between words.
pub fn tracked(text: &str) -> String {
    let mut out = String::with_capacity(text.len() * 4);
    for (index, ch) in text.chars().enumerate() {
        if index > 0 {
            out.push('\u{200A}');
        }
        if ch == ' ' {
            out.push('\u{2009}');
        } else {
            out.push(ch);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tracking_keeps_letters() {
        let spaced = tracked("AB C");
        assert_eq!(spaced.chars().filter(|ch| ch.is_alphabetic()).collect::<String>(), "ABC");
        assert!(!ENGINE_VERSION.is_empty(), "build script read the engine version");
    }
}
