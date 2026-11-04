#pragma once

#include "kotor/Media.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ke::kotor {

struct BinkVideoFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

// Decoded Bink Audio (DCT/RDFT) at the stream's native rate, interleaved float.
struct BinkAudioTrack {
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> samples;

    std::size_t frameCount() const {
        return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
    }
    double durationSeconds() const {
        return sampleRate > 0
            ? static_cast<double>(frameCount()) / static_cast<double>(sampleRate) : 0.0;
    }
};

// Native Bink 1 pixel decoder. The container contract stays in Media.h; this
// class turns frame payloads into RGBA through the FFmpeg Bink decoder that
// produced the recorded oracle hash.
class BinkPlayer {
public:
    BinkPlayer();
    ~BinkPlayer();

    BinkPlayer(const BinkPlayer&) = delete;
    BinkPlayer& operator=(const BinkPlayer&) = delete;

    static bool available();

    bool open(const std::string& path, std::string& error);
    void close();
    bool isOpen() const;

    int width() const;
    int height() const;
    int frameCount() const;
    double framesPerSecond() const;
    bool hasAudio() const;
    const BinkAudioTrack& audio() const;
    double audioDurationSeconds() const;

    // Decode one frame by index. Sequential playback should ask for
    // decodedFrames() next; random access rewinds the demuxer.
    bool decodeFrame(int index, BinkVideoFrame& out, std::string& error);
    int decodedFrames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ke::kotor
