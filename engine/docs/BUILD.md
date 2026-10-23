# Building and running the engine scaffold

## Prerequisites

- Windows 10/11 x64.
- Visual Studio 2026 Build Tools (or VS 2022+ with the MSVC C++ workload). The
  `msvc-*` presets use the VS-bundled CMake 4.3 and Ninja 1.13 through
  `scripts/vs-env.bat` (which calls `vcvars64.bat`); no system PATH changes.
- Network access on first configure: CMake fetches the four pinned archives and
  verifies their SHA-256 hashes. Subsequent builds use `build/<preset>/_deps`.

If your VS installation is elsewhere, set `KOTOR_VS_PATH` before running the scripts.

## Quick start (cmd.exe)

```bat
cd engine
scripts\configure.bat msvc-release
scripts\build.bat msvc-release
ctest --preset msvc-release
```

Equivalent manual commands (from a shell where `scripts\vs-env.bat` was called):

```bat
cmake --preset msvc-release
cmake --build --preset msvc-release
```

`msvc-debug` builds with assertions and `/W4 /WX`; `msvc-release` is
RelWithDebInfo.

### Visual Studio generator (no vcvars)

```bat
"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --preset vs2026
cmake --build --preset vs2026-release
```

The `vs2026` preset targets `Visual Studio 18 2026` with `-A x64`.

## Runtime

```
build\msvc-release\kotor_engine.exe --vulkan
build\msvc-release\kotor_engine.exe --gl --frames 120 --seconds 10
build\msvc-release\kotor_engine.exe --headless --frames 5
build\msvc-release\kotor_engine.exe --self-test
```

Backends: `--vulkan` (primary), `--gl` / `--opengl` (4.5 core fallback),
`--null` (logical loop). With no backend flag, Vulkan is tried first and OpenGL is
used only if Vulkan fails (`--no-fallback` disables that). `--headless` forces the
SDL dummy video driver; it is only useful for the null backend or for checking the
graceful Vulkan failure path.

Useful options: `--frames N`, `--seconds S`, `--width`, `--height`, `--hidden`,
`--validate` (enables `VK_LAYER_KHRONOS_validation` when installed), `--vsync` /
`--no-vsync`, `--title`.

Exit codes: `0` success, `2` usage error, `3` SDL init failure, `4` window failure,
`5` backend initialization failure, `6` fatal frame error, `7` ImGui self-test failure.

## Tests

```bat
ctest --preset msvc-release --output-on-failure
powershell -ExecutionPolicy Bypass -File scripts\smoke.ps1
powershell -ExecutionPolicy Bypass -File scripts\smoke.ps1 -SkipGpu   # CI without a GPU
```

`smoke.ps1` runs the ImGui self-test, the null headless loop, the graceful Vulkan
surface failure under the dummy driver, and 120-frame Vulkan/OpenGL runs. It uses
per-case timeouts and kills any process that overruns; every run self-terminates.

## Dependency pinning / offline notes

`cmake/Dependencies.cmake` contains the exact commit archives and SHA-256 hashes.
To reconfigure from scratch, delete `build/<preset>`; the next configure downloads
and re-verifies the archives. If you need a fully offline build, pre-populate
`build/<preset>/_deps` or point `FETCHCONTENT_SOURCE_DIR_<NAME>` at trusted local
copies; the hashes only protect the download path.

## Layout

```
CMakeLists.txt              first-party target, warnings, tests
CMakePresets.json           Ninja/MSVC and VS 2026 presets
cmake/Dependencies.cmake    pinned FetchContent inputs
src/app/                    argument parsing, application loop, UI
src/render/                 backend interface + Vulkan/GL/null implementations
src/core/Log.*              logging helper
scripts/                    VS env, configure, build, smoke
docs/                       versions, build log, summary
```
