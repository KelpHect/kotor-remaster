# Toolchain and dependency versions (Lane 08)

All values below were read from the host or from the pinned inputs actually used to
build `kotor_engine.exe`. Nothing was installed system-wide.

## Host

| Item | Value |
|---|---|
| OS | Windows 11 Pro 10.0.26300 (build 26300.9711) |
| CPU | 13th Gen Intel Core i7-13700KF |
| GPU | AMD Radeon RX 9070 XT, driver 26.10.1 (LLPC), Vulkan driver 2.0.416 |
| Vulkan runtime | Loader 1.4.357; device API 1.4.359 |
| Validation layer | `VK_LAYER_KHRONOS_validation` is **not** installed (only AMD/OBS/Steam layers) |
| OpenGL | 4.5.0 Core Profile Context 26.10.1.261008 |
| Python / JDK on host | Python 3.15, JDK 21 (not required by this build) |

## Build toolchain (detected, not downloaded)

`vswhere.exe` reported **Visual Studio Build Tools 2026** at
`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools` (18.11.12306.390).
It already ships MSVC, CMake and Ninja, so no portable CMake/Ninja/LLVM download was
needed.

| Tool | Version | Location |
|---|---|---|
| MSVC compiler | 19.51.36260 (toolset 14.51.36231) | VS Build Tools 2026 |
| Windows SDK | 10.0.26100.0 | Windows Kits |
| CMake | 4.3.1-msvc1 | VS-bundled `.../CMake/bin/cmake.exe` |
| Ninja | 1.13.2 | VS-bundled `.../CMake/Ninja/ninja.exe` |
| Generator used | Ninja (MSVC x64, RelWithDebInfo) | `scripts/vs-env.bat` loads `vcvars64.bat` |

A second preset, `vs2026`, uses the **Visual Studio 18 2026** generator and needs no
vcvars shell. Both presets are defined in `CMakePresets.json`.

## Pinned third-party inputs

Fetched by `cmake/Dependencies.cmake` via `FetchContent` from immutable commit
archives. Each archive is verified with its SHA-256 at configure time.
Local copies and checksums are in `vendor/archives/` (`SHA256SUMS.txt`).

| Dependency | Version / tag | Commit | Archive SHA-256 |
|---|---|---|---|
| SDL | 3.4.18 | `829a65d769d935c4852f8159e964312c0957260a` | `48e15cf1a6928453d77afa955d1fdfbbca9169e2bc3d76d111acb9b8bb076d66` |
| Dear ImGui | v1.93.0 | `c1aaac65e896a45f81f47a4d68f9959e0f86c859` | `b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114` |
| Vulkan-Headers | vulkan-sdk-1.4.363.0 | `6802bb4733b63ed5efd3adb308a6c885ef180ea1` | `9044fbb356160e43201dc2094acb44c8a906722a63a35f48fc0f051579db9bb6` |
| volk | vulkan-sdk-1.4.363.0 | `54fc0d7a72887cbfc5b030d26897b2b40ac53fb8` | `4944450dc3943871a28ce993eaf995309eb29e7000e091309968dba4cd321648` |

Source URLs (commit archives):

- `https://github.com/libsdl-org/SDL/archive/829a65d769d935c4852f8159e964312c0957260a.tar.gz`
- `https://github.com/ocornut/imgui/archive/c1aaac65e896a45f81f47a4d68f9959e0f86c859.tar.gz`
- `https://github.com/KhronosGroup/Vulkan-Headers/archive/6802bb4733b63ed5efd3adb308a6c885ef180ea1.tar.gz`
- `https://github.com/zeux/volk/archive/54fc0d7a72887cbfc5b030d26897b2b40ac53fb8.tar.gz`

SDL is built statically; Vulkan-Headers and volk are header/loader-glue only (no Vulkan
SDK or import library is required). The Vulkan loader is loaded at runtime by volk from
the system `vulkan-1.dll`.

## Built artifact

- `build/msvc-release/kotor_engine.exe`
- SHA-256 at time of writing:
  `7c078438d5faad55a6d8605d4024f8bdff839db40ac5e09821a1b0d80617bac8`
- Full and filtered build logs: `docs/build-release.log`,
  `docs/build-release-summary.log`
