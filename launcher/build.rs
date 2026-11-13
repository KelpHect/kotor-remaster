//! Build script: reads the engine version from the engine's own CMake project so the launcher shows the
//! real number and never a hard-coded one.

use std::{env, fs, path::Path};

fn main() {
    let manifest = env::var("CARGO_MANIFEST_DIR").expect("cargo sets CARGO_MANIFEST_DIR");
    let cmake = Path::new(&manifest).join("../engine/CMakeLists.txt");
    println!("cargo:rerun-if-changed={}", cmake.display());
    let version = fs::read_to_string(&cmake).ok().and_then(|text| engine_version(&text)).unwrap_or_default();
    println!("cargo:rustc-env=KE_ENGINE_VERSION={version}");
}

/// The `VERSION x.y.z` inside `project(kotor_engine ...)`.
fn engine_version(text: &str) -> Option<String> {
    let start = text.find("project(")?;
    let body = &text[start..start + text[start..].find(')')?];
    let mut words = body.split_whitespace();
    while let Some(word) = words.next() {
        if word == "VERSION" {
            return words.next().map(str::to_owned);
        }
    }
    None
}
