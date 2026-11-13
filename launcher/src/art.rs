//! Game art for the banner and the game cards. Nothing is shipped: the only images are the ones the
//! player's own Steam client has already cached for a game they own (`Steam/appcache/librarycache`),
//! opened read-only. Without them the UI draws an original abstract background in code.

use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct GameArt {
    /// Wide banner (`library_hero.jpg`).
    pub hero: Option<PathBuf>,
    /// Small landscape capsule (`header.jpg`).
    pub header: Option<PathBuf>,
}

/// Looks in Steam's library cache for an app's images. Handles the per-app folder layout
/// (`librarycache/<appid>/library_hero.jpg`, also one hashed subfolder deep) and the older flat layout
/// (`librarycache/<appid>_library_hero.jpg`).
pub fn steam_cached_art(steam_root: &Path, app_id: u32) -> GameArt {
    let cache = steam_root.join("appcache").join("librarycache");
    GameArt { hero: find_image(&cache, app_id, "library_hero.jpg"), header: find_image(&cache, app_id, "header.jpg") }
}

fn find_image(cache: &Path, app_id: u32, name: &str) -> Option<PathBuf> {
    let folder = cache.join(app_id.to_string());
    let direct = folder.join(name);
    if direct.is_file() {
        return Some(direct);
    }
    if let Ok(entries) = std::fs::read_dir(&folder) {
        for entry in entries.flatten() {
            let nested = entry.path().join(name);
            if nested.is_file() {
                return Some(nested);
            }
        }
    }
    let flat = cache.join(format!("{app_id}_{name}"));
    flat.is_file().then_some(flat)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testutil::{work_dir, write_text};

    #[test]
    fn finds_both_layouts() {
        let work = work_dir("art");
        let cache = work.join("appcache").join("librarycache");
        write_text(&cache.join("32370").join("library_hero.jpg"), "x");
        write_text(&cache.join("32370").join("abc123").join("header.jpg"), "x");
        write_text(&cache.join("208580_header.jpg"), "x");
        let one = steam_cached_art(&work, 32370);
        assert!(one.hero.is_some() && one.header.is_some());
        let two = steam_cached_art(&work, 208580);
        assert!(two.hero.is_none() && two.header.is_some());
        assert_eq!(steam_cached_art(&work, 1), GameArt::default());
    }
}
