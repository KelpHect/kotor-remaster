#pragma once

// Independent reference for the audio decoders and measurements for the quality gate.
//
// The engine decodes WAV (PCM, IMA ADPCM), MP3 and BMU itself (Audio.cpp, dr_mp3). The
// reference below decodes the same resources through libavformat/libavcodec, which share no
// code with those decoders, so a layout or sample-format bug in the engine shows up as a
// low correlation. Nothing here is used at run time by the game; it is gate and tooling code.
#include "audio/Audio.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ke::audio {

// Where the real audio starts inside a shipped file. KotOR files come in these shapes:
//  - a plain RIFF/WAVE at offset 0 (PCM or IMA ADPCM);
//  - a 58-byte RIFF header whose `data` chunk is empty, followed by MP3 frames (music, VO);
//  - a 470-byte fake MP3 frame (Xing/LAME tag) followed by the real RIFF/WAVE (area sounds);
//  - a "BMU " tag followed by MP3 frames; a bare MP3.
struct AudioLayout {
    bool valid = false;
    std::size_t offset = 0;   // first byte the container starts at
    bool mp3 = false;         // true: MP3 frames, false: RIFF/WAVE
    std::string description;  // short label, e.g. "riff@470"
};
AudioLayout locateAudio(const std::vector<std::uint8_t>& bytes);

// Decodes `bytes` through FFmpeg at the layout locateAudio found. Output keeps the file's own
// sample rate and channel count; samples are float in [-1, 1]. False when FFmpeg is not built in.
bool referenceDecode(const std::vector<std::uint8_t>& bytes, SoundData& out, std::string& error,
                     AudioLayout* layout = nullptr);
bool referenceAvailable();

// ---- measurements ------------------------------------------------------------------------------

struct Correlation {
    double value = 0.0;       // normalised cross-correlation at `lag`
    int lag = 0;              // samples the second signal is shifted against the first
    std::size_t compared = 0;
};
// Best normalised correlation of two mono signals over lags in [-maxLag, maxLag] (found on a
// short loud window, then evaluated over at most `limit` samples).
Correlation correlate(const std::vector<float>& a, const std::vector<float>& b, int maxLag,
                      std::size_t limit);
// Channel average of an interleaved signal.
std::vector<float> mono(const SoundData& data);
// Spectral flatness (geometric / arithmetic mean of the power spectrum, 0 tonal .. 1 white noise)
// averaged over 2048-sample windows of `signal`, between `lowHz` and `highHz`.
double spectralFlatness(const std::vector<float>& signal, int sampleRate, double lowHz,
                        double highHz);
// Largest |x[n] - x[n-1]| and the count of steps above `threshold` in an interleaved stereo buffer.
struct StepStats {
    float maxStep = 0.0f;
    std::size_t over = 0;
    std::size_t clipped = 0;   // samples with |x| >= 1
    std::size_t nonFinite = 0; // NaN or Inf
    std::size_t samples = 0;
};
StepStats stepStats(const std::vector<float>& stereo, float threshold);

// 16-bit PCM WAV writer for captures (clamps; returns false when the file cannot be written).
bool writeWav16(const std::string& path, const std::vector<float>& interleaved, int channels,
                int sampleRate);

} // namespace ke::audio
