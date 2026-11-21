# Third-party notices

Attribution for code this project builds with, patches, or used as a reference. This file does
not license the project's own code; see `docs/LICENSE_PROPOSAL.md`. Pins and hashes are in
`docs/versions.md`. The licenses of the components built into the engine were checked against the pinned revisions
on 2026-11-21; re-check when a pin changes.

## Built into the engine

| Component | Source | Pinned revision | License | How it is used |
|---|---|---|---|---|
| SDL3 | https://github.com/libsdl-org/SDL | `829a65d769d935c4852f8159e964312c0957260a` | zlib | Fetched at configure time, linked |
| Vulkan-Headers | https://github.com/KhronosGroup/Vulkan-Headers | `6802bb4733b63ed5efd3adb308a6c885ef180ea1` | Apache-2.0 OR MIT | Fetched, headers only |
| volk | https://github.com/zeux/volk | `54fc0d7a72887cbfc5b030d26897b2b40ac53fb8` | MIT | Fetched, compiled in |
| Dear ImGui | https://github.com/ocornut/imgui | `c1aaac65e896a45f81f47a4d68f9959e0f86c859` | MIT | Fetched, compiled in |
| dr_mp3 (dr_libs) | https://github.com/mackron/dr_libs | vendored `engine/third_party/dr_libs/dr_mp3.h` | Public domain (Unlicense) OR MIT-0; text in `engine/third_party/dr_libs/LICENSE` | Vendored single header |

## Optional, not in the repository

| Component | Source | License | How it is used |
|---|---|---|---|
| FFmpeg (`avcodec`, `avformat`, `avutil`, `swscale`, `swresample`) | https://ffmpeg.org (BtbN Windows builds) | LGPL-2.1-or-later or GPL, depending on the build; the build pinned in `docs/versions.md` is a GPL one | Linked dynamically for Bink 1 decode when `KOTOR_FFMPEG_ROOT` is set; DLLs are copied beside the executable. See `docs/LEGAL_REVIEW.md` F3. |

## Patched, never shipped

| Component | Source | License | How it is used |
|---|---|---|---|
| nwnnsscomp (nwnexplorer) | https://github.com/virusman/nwnexplorer at `ead0f1762f5df9b88d5a0eb2726d68d457e80e48` | BSD-style, Copyright (c) 2002-2003 Edward T. Smith (see upstream file headers) | `tools/compiler/nwnnsscomp-historical.patch` is applied to an upstream checkout to build a local test oracle. The patch context contains upstream lines. |

## References and external oracles

Not copied into this repository and not linked into the engine.

| Component | Source | License | Role |
|---|---|---|---|
| bioware-kaitai-formats | https://github.com/OpenKOTOR/bioware-kaitai-formats | MIT | Format reference |
| PyKotor | https://github.com/OpenKOTOR/PyKotor | LGPL-3.0-or-later | External test oracle |
| xoreos, xoreos-tools | https://github.com/xoreos | GPL-3.0 | External process oracle; type tables consulted |
| KotOR.js | https://github.com/KobaltBlu/KotOR.js | GPL-3.0 | Type tables consulted |
| kotorblender | https://github.com/seedhartha/kotorblender | GPL-3.0 | External authoring tool |
| DeNCS / NCSDecomp | see `tools/README.md` | BSL 1.1 | Offline oracle |
| Ghidra | https://github.com/NationalSecurityAgency/ghidra | Apache-2.0 | Analysis tool; `tools/ghidra/*.java` are project-written scripts against its API |

Star Wars, Knights of the Old Republic and related names are trademarks of their owners. This
project is unofficial and is not affiliated with or endorsed by BioWare, Electronic Arts,
Lucasfilm, Disney or any platform holder.
