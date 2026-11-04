# Pinned build inputs

`engine/cmake/Dependencies.cmake` fetches immutable archives and checks their
SHA-256 at configure time. Vendored single-header libraries are pinned by the
recorded file hash.

| Dependency | Version / commit | SHA-256 |
|---|---|---|
| SDL3 | `829a65d769d935c4852f8159e964312c0957260a` | `48e15cf1a6928453d77afa955d1fdfbbca9169e2bc3d76d111acb9b8bb076d66` |
| Vulkan-Headers | `6802bb4733b63ed5efd3adb308a6c885ef180ea1` | `9044fbb356160e43201dc2094acb44c8a906722a63a35f48fc0f051579db9bb6` |
| volk | `54fc0d7a72887cbfc5b030d26897b2b40ac53fb8` | `4944450dc3943871a28ce993eaf995309eb29e7000e091309968dba4cd321648` |
| Dear ImGui | `c1aaac65e896a45f81f47a4d68f9959e0f86c859` | `b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114` |
| dr_mp3 | vendored `engine/third_party/dr_libs/dr_mp3.h` | `f5c8020eeebb79877184432efdd2651f24869f5bb8f379d45450fd0aed1ee915` |
| FFmpeg (Bink pixel decode) | BtbN `n9.0.2-45-g1e65c6cd5d-20261103` win64 gpl-shared | `8dcbbea210aa7f6bbd65bf8b81517b25bd5a22846da57ecf563b9b4807892ca7` (zip) |

dr_mp3 is used for the MP3 payload in zero-length-data RIFF music streams; its
public-domain/MIT-0 license is kept beside the header.

FFmpeg is an optional local toolchain dependency (`KOTOR_FFMPEG_ROOT`,
default `C:/dev-overflow/ffmpeg-dev/ffmpeg-n9.0-latest-win64-gpl-shared-9.0`).
The engine links `avcodec`, `avformat`, `avutil` and `swscale` for Bink 1 pixel
decode and copies the matching runtime DLLs beside the executable. It is not
vendored into the repository; the Bink pixel oracle is the same FFmpeg decoder.
