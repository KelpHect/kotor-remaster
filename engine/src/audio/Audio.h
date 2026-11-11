#pragma once

#include "kotor/Formats.h"

#include <cstdint>
#include <functional>
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
// Test hook only: while on, IMA ADPCM decodes with the pre-fix bugs (stereo stride of 8 bytes
// per channel, out-of-range step-index lookup for negative nibbles, block header sample
// dropped). The audioquality gate flips it to prove it fails on that decoder.
void setLegacyImaDecodeForTest(bool on);

enum class Bus { Music = 0, Ambient = 1, Effects = 2, Voice = 3, Movie = 4 };

// A soundeax.2da environment: the label and the retail environment id (the row
// index). The DSP parameters live in audio/Reverb.h (EaxEnvironment).
struct ReverbPreset {
    std::string label;
    int environment = 0;
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
    // A mute is a flag beside the stored volume: setBusMuted(bus, true) silences the
    // bus (and its reverb send) without touching busVolume(), so unmuting restores
    // the exact level.
    void setBusMuted(Bus bus, bool muted);
    bool busMuted(Bus bus) const;
    // A pause freezes every voice of the bus where it is (cursor, fades and all) and silences it;
    // clearing the flag continues from the same sample. Retail pauses sounds this way while a movie
    // plays (005d8560 mode 3 into 005d82c0 pauses every stream and sample, 005d83f0 resumes them).
    void setBusPaused(Bus bus, bool paused);
    bool busPaused(Bus bus) const;
    // Peak and RMS of one bus in the last mix() call, after its volume and mute, before
    // the master gain and the reverb (a meter for tests and tools).
    float busPeak(Bus bus) const;
    float busRms(Bus bus) const;
    // Environment reverb. PARAMETER-FAITHFUL, NOT BIT-EXACT MILES/EAX: see
    // audio/Reverb.h. Changes are queued and applied at the start of the next
    // mix() call, so they are safe to request from the game thread.
    //  - setReverb: switch to a soundeax label immediately (no crossfade).
    //  - setReverbEnvironment: switch to a soundeax row id (negative = off); with
    //    `crossfade` the old and new reverbs overlap for crossfadeSeconds().
    void setReverb(const std::string& label);
    void setReverbEnvironment(int environment, bool crossfade = true);
    const ReverbPreset& reverb() const;
    int reverbEnvironment() const;
    bool reverbFading() const;
    void setReverbCrossfadeSeconds(float seconds);
    // Fraction of a positional voice's signal sent to the reverb (retail keeps
    // one global effects level, default 0.6 at swkotor.exe 005d8813).
    void setReverbSend(float level);
    float reverbSend() const;
    // Denormal floats currently stored in the reverb memory (test hook).
    std::size_t reverbDenormalCount() const;

    int play(const std::shared_ptr<const SoundData>& data, Bus bus, float volume,
             bool loop, bool positional = false, kotor::Vec3 position = {},
             float minDistance = 1.0f, float maxDistance = 10.0f, float pan = 0.0f,
             float pitch = 1.0f);
    // --state-trace only (game/StateTrace.h): told on the caller's thread, under the mixer lock, each time a
    // voice starts. It must not call back into the mixer. Null when the flag is absent (one test per play).
    struct PlayInfo {
        int voice = 0;
        const std::string* name = nullptr;
        Bus bus = Bus::Effects;
        float volume = 0.0f;
        bool loop = false;
        bool positional = false;
        kotor::Vec3 position;
        float minDistance = 0.0f;
        float maxDistance = 0.0f;
    };
    using PlayObserver = std::function<void(const PlayInfo&)>;
    void setPlayObserver(PlayObserver observer);
    int playMusic(const std::shared_ptr<const SoundData>& data, float volume, bool loop);
    void stopMusic();
    void stopVoice(int voice);
    void stopBus(Bus bus);
    void stopAll();
    void setVoiceVolume(int voice, float volume);
    void setVoicePosition(int voice, kotor::Vec3 position);
    // A positional stream (retail CExoStreamingSoundSourceInternal::Update3DStream 005dc560) is not
    // posed by Miles: its own gain is pow(level * setting * f, 0.6) with f = (max - d) / (max - min)
    // beyond min, times 1 + 0.125 * dot behind the listener, times 1 - obstruction. The voice volume
    // then carries pow(level, 0.6) * setting^-0.4 (AudioSystem::voiceLevel).
    void setVoiceStream3D(int voice, bool on);
    // The share of the line to the listener that walls hide, 0 .. 1 (GetObstructionFactor 0050ccf0).
    void setVoiceObstruction(int voice, float obstruction);
    struct StreamProbe {
        int voice = 0;
        kotor::Vec3 position;
    };
    std::vector<StreamProbe> stream3DProbes() const;
    kotor::Vec3 listenerPosition() const;
    void setVoicePitch(int voice, float pitch);
    bool voiceActive(int voice) const;
    float voiceVolume(int voice) const;
    // Linear ramp of a per-voice multiplier (1.0 on a new voice) to `target` over
    // `seconds`, sample accurate inside mix(). `stopAtEnd` retires the voice once a
    // target of 0 is reached. seconds <= 0 jumps.
    void fadeVoice(int voice, float target, float seconds, bool stopAtEnd);
    float voiceFade(int voice) const;
    // Starts a voice with its fade multiplier at 0 (use with fadeVoice to fade in).
    void setVoiceFade(int voice, float multiplier);
    void setVoiceCursor(int voice, double seconds);
    double voiceCursor(int voice) const;
    void mix(float* interleavedStereo, int frames);

    std::size_t activeVoices() const;
    std::size_t activeVoices(Bus bus) const;
    float lastRms() const;
    float voiceGain(int voice) const;
    float voicePan(int voice) const;

    // Diagnostics: while capturing, every mix() call also appends each bus (after its volume
    // and mute, before the master gain), the reverb return and the final output to
    // <prefix>_music|ambient|effects|voice|movie|reverb|final.wav (16-bit stereo). No audio
    // device is involved, so a session run with --no-audio can be captured and played back.
    bool startCapture(const std::string& prefix);
    void stopCapture();
    bool capturing() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ke::audio
