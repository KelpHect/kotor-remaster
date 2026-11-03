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

The CTest suite also includes the Sprint 6 script/rules gates, which need the
user's read-only install:

```bat
build\msvc-release\kotor_engine.exe --module end_m01aa --script-test --null --headless
build\msvc-release\kotor_engine.exe --module end_m01aa --dialogue-test --null --headless
build\msvc-release\kotor_engine.exe --module end_m01aa --rules-test --null --headless
build\msvc-release\kotor_engine.exe --module end_m01aa --systems-test --null --headless
build\msvc-release\kotor_engine.exe --module tar_m02ac --merchant-test --null --headless
build\msvc-release\kotor_engine.exe --module tar_m03aa --encounter-test --null --headless
build\msvc-release\kotor_engine.exe --module end_m01aa --party-test --null --headless
build\msvc-release\kotor_engine.exe --module end_m01aa --ui-test --null --headless
build\msvc-release\kotor_engine.exe --module danm13 --loot-test --null --headless
build\msvc-release\kotor_engine.exe --module tar_m03ab --encounter-reset-test --null --headless
build\msvc-release\kotor_engine.exe --module end_m01aa --delayed-test "tests\fixtures\delay_test.ncs" --null --headless
build\msvc-release\kotor_engine.exe --module end_m01aa --combat-force-test --combat-force-script "tests\fixtures\force_script_test.ncs" --null --headless
build\msvc-release\kotor_engine.exe --profile-info "E:\SteamLibrary\steamapps\common\swkotor" --null --headless
build\msvc-release\kotor_engine.exe --original-save-load "..\artifacts\lanes\06-dynamic\profile\saves\000000 - QUICKSAVE" --null --headless
build\msvc-release\kotor_engine.exe --module end_m01aa --original-save-create "build\msvc-release\000010 - ENGINE" --null --headless
```

Original save slots written by the engine can be checked independently:

```powershell
python ..\tools\resources\kotor_save.py validate "build\msvc-release\000010 - ENGINE"
```

`kotor_vm_corpus` runs the native VM over the extracted 8,666-script corpus and
diffs it against the regenerated Python oracles; it is registered only when the
research artifacts are present. `scripts/check_vm_corpus.py` writes the parity
report and `scripts/check_install_readonly.py` proves the install was not
modified.

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
src/game/                   player session, 2DA rules, d20 rounds, save state
src/kotor/                  archives, GFF, geometry, textures, scene, templates
src/script/                 NCS decoder/VM, action table/runtime, world, corpus runner
src/render/                 backend interface + Vulkan/GL/null implementations
src/core/Log.*              logging helper
scripts/                    VS env, configure, build, smoke, VM/install checkers
docs/                       versions, build log, summary
```
