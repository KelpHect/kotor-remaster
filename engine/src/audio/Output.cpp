#include "audio/Output.h"

#include <algorithm>
#include <cstring>

namespace ke::audio {

Output::Output(Mixer& mixer, int requestedFrames)
    : mixer_(&mixer)
{
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return;
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = 48000;
    (void)requestedFrames;
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                        &Output::callback, this);
    if (stream_ == nullptr) {
        return;
    }
    const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(stream_);
    if (device != 0) {
        const char* name = SDL_GetAudioDeviceName(device);
        if (name != nullptr) {
            device_ = name;
        }
    }
    SDL_ResumeAudioStreamDevice(stream_);
}

Output::~Output()
{
    if (stream_ != nullptr) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
}

void SDLCALL Output::callback(void* userdata, SDL_AudioStream* stream,
                              int additionalAmount, int totalAmount)
{
    static_cast<Output*>(userdata)->pull(stream, additionalAmount);
    (void)totalAmount;
}

void Output::pull(SDL_AudioStream* stream, int additionalAmount)
{
    if (stream == nullptr || mixer_ == nullptr || additionalAmount <= 0) {
        return;
    }
    const int frames = additionalAmount / static_cast<int>(sizeof(float) * 2);
    if (frames <= 0) {
        return;
    }
    buffer_.resize(static_cast<std::size_t>(frames) * 2);
    mixer_->mix(buffer_.data(), frames);
    SDL_PutAudioStreamData(stream, buffer_.data(), additionalAmount);
}

} // namespace ke::audio
