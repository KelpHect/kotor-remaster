#pragma once

#include "audio/Audio.h"
#include "kotor/Formats.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ke {

// Area audio routing and playback over the deterministic mixer. The class owns
// decoded PCM caches and never mutates the game install.
class AudioSystem {
public:
    explicit AudioSystem(kotor::Install& install);

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    bool loadScene(const kotor::Scene& scene, std::string& error);
    void setListener(kotor::Vec3 position, float yaw);
    void setTlk(const kotor::Tlk* tlk) { tlk_ = tlk; }

    bool playMusic(std::int32_t musicId);
    bool playAmbient(std::int32_t ambientId);
    void stopMusic();
    void stopAmbient();

    bool playEffect(const std::string& resref, const kotor::Vec3* position = nullptr,
                    float volume = 1.0f, bool loop = false);
    bool playSoundObject(std::uint32_t entity);
    bool playCreatureSound(std::uint32_t entity, int slot);
    bool playFootstep(int footstepType, int surface, kotor::Vec3 position,
                      float volume = 0.6f);
    bool stopSoundObject(std::uint32_t entity);
    bool setSoundObjectVolume(std::uint32_t entity, float volume);
    bool setSoundObjectPosition(std::uint32_t entity, kotor::Vec3 position);
    bool playVoice(const std::string& resref);

    audio::Mixer& mixer() { return mixer_; }
    const audio::Mixer& mixer() const { return mixer_; }
    const kotor::AreaAudio& areaAudio() const { return areaAudio_; }
    std::size_t soundEmitterCount() const { return soundEmitters_.size(); }
    std::size_t loadedSoundCount() const { return sounds_.size(); }
    bool hasSound(const std::string& resref) const;
    bool decodeProbe(const std::string& resref, audio::SoundData& out, std::string& error) const;
    const audio::SoundData* sound(const std::string& resref) const;

private:
    std::shared_ptr<const audio::SoundData> loadSound(const std::string& resref,
                                                      std::string& error);
    std::string tableResource(const char* table, std::int32_t id) const;
    void indexLooseAudio();

    kotor::Install& install_;
    audio::Mixer mixer_;
    std::map<std::string, std::filesystem::path> looseFiles_;
    std::map<std::string, std::shared_ptr<const audio::SoundData>> sounds_;
    std::optional<kotor::TwoDa> musicTable_;
    std::optional<kotor::TwoDa> ambientTable_;
    std::optional<kotor::TwoDa> eaxTable_;
    std::optional<kotor::TwoDa> footstepTable_;
    kotor::AreaAudio areaAudio_;
    std::vector<kotor::SoundEmitter> soundEmitters_;
    std::map<std::uint32_t, std::string> entitySoundSets_;
    std::map<std::uint32_t, kotor::Vec3> entityPositions_;
    std::map<std::string, kotor::Ssf> soundSetCache_;
    std::map<std::uint32_t, int> objectVoices_;
    const kotor::Tlk* tlk_ = nullptr;
    std::uint32_t footstepCounter_ = 0;
    float musicVolume_ = 0.75f;
    float ambientVolume_ = 0.65f;
};

} // namespace ke
