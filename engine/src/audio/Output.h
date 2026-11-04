#pragma once

#include "audio/Audio.h"

#include <SDL3/SDL.h>

#include <vector>

namespace ke::audio {

// SDL3 audio device feeding the deterministic mixer. The mixer remains the
// source of truth; this class only pulls interleaved stereo frames.
class Output {
public:
    explicit Output(Mixer& mixer, int requestedFrames = 1024);
    ~Output();

    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    bool valid() const { return stream_ != nullptr; }
    const std::string& device() const { return device_; }

private:
    static void SDLCALL callback(void* userdata, SDL_AudioStream* stream,
                                 int additionalAmount, int totalAmount);
    void pull(SDL_AudioStream* stream, int additionalAmount);

    Mixer* mixer_ = nullptr;
    SDL_AudioStream* stream_ = nullptr;
    std::string device_;
    std::vector<float> buffer_;
};

} // namespace ke::audio
