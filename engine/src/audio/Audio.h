#pragma once

#include "kotor/Formats.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ke::audio {

struct SoundData {
    std::string name;
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> samples; // interleaved

    std::size_t frameCount() const;
    double durationSeconds() const;
};

bool decodeWav(const std::vector<std::uint8_t>& bytes, const std::string& name,
               SoundData& out, std::string& error);
bool decodeMp3(const std::vector<std::uint8_t>& bytes, const std::string& name,
               SoundData& out, std::string& error);
bool decodeAudio(const std::vector<std::uint8_t>& bytes, const std::string& name,
                 SoundData& out, std::string& error);
bool resample(SoundData& data, int sampleRate, int channels);

enum class Bus { Music = 0, Ambient = 1, Effects = 2, Voice = 3 };

struct ReverbPreset {
    std::string label;
    float delayMs = 0.0f;
    float feedback = 0.0f;
    float wet = 0.0f;
};

ReverbPreset reverbPreset(const std::string& label);
const std::vector<std::string>& reverbLabels();

class Mixer {
public:
    Mixer();
    ~Mixer();

    void setSampleRate(int sampleRate);
    int sampleRate() const;

    void setListener(kotor::Vec3 position, float yaw);
    void setBusVolume(Bus bus, float volume);
    float busVolume(Bus bus) const;
    void setReverb(const std::string& label);
    const ReverbPreset& reverb() const;

    int play(const std::shared_ptr<const SoundData>& data, Bus bus, float volume,
             bool loop, bool positional = false, kotor::Vec3 position = {},
             float minDistance = 1.0f, float maxDistance = 10.0f, float pan = 0.0f,
             float pitch = 1.0f);
    int playMusic(const std::shared_ptr<const SoundData>& data, float volume, bool loop);
    void stopMusic();
    void stopVoice(int voice);
    void stopBus(Bus bus);
    void stopAll();
    void setVoiceVolume(int voice, float volume);
    void setVoicePosition(int voice, kotor::Vec3 position);
    void mix(float* interleavedStereo, int frames);

    std::size_t activeVoices() const;
    std::size_t activeVoices(Bus bus) const;
    float lastRms() const;
    float voiceGain(int voice) const;
    float voicePan(int voice) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ke::audio
