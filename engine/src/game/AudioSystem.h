#pragma once

#include "audio/Audio.h"
#include "kotor/Formats.h"

#include <cstdint>
#include <functional>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ke {

class AreaAmbience;

// Area audio routing and playback over the deterministic mixer. The class owns
// decoded PCM caches and never mutates the game install.
class AudioSystem {
public:
    explicit AudioSystem(kotor::Install& install);

    ~AudioSystem();

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    bool loadScene(const kotor::Scene& scene, std::string& error);
    void setListener(kotor::Vec3 position, float yaw);
    // Applies the EAX environment of the ARE room (RoomName, case-insensitive) the
    // listener stands in. Retail does this per room from ARE Rooms/EnvAudio
    // (swkotor.exe 005ee860); an unknown or empty room leaves the environment as is.
    void setListenerRoom(const std::string& roomName);
    int roomEnvironment() const { return roomEnvironment_; }
    // How much of the line from a sound to the listener walls hide, 0 .. 1 (retail GetObstructionFactor
    // 0050ccf0). The session supplies it; updateObstruction hands it to every positional stream whose
    // source or the listener has moved half a metre since its last answer (GetClosestRecentSound 005d6ef0).
    using ObstructionProbe = std::function<float(kotor::Vec3 source, kotor::Vec3 listener)>;
    void setObstructionProbe(ObstructionProbe probe) { obstructionProbe_ = std::move(probe); }
    void updateObstruction();
    // soundeax.2da label of environment `row` (empty when the row does not exist).
    std::string eaxLabel(int row) const;
    void setTlk(const kotor::Tlk* tlk) { tlk_ = tlk; }

    bool playMusic(std::int32_t musicId);
    // Plays a streammusic resource by name in a loop (the main menu theme).
    bool playMusicResource(const std::string& resref, float volume = 1.0f);
    bool playAmbient(std::int32_t ambientId);
    void stopMusic();
    void stopAmbient();

    bool playEffect(const std::string& resref, const kotor::Vec3* position = nullptr,
                    float volume = 1.0f, bool loop = false);

    // ---- PriorityGroups.2da (retail 005d7bd0 loads the 27 rows; 005db800 / 005dc930 apply them) ----
    // swkotor.exe does not read soundgain.2da (the string is absent from the executable): the level,
    // the distances, the concurrency cap and the pitch variance of every sound come from the row of
    // the priority group its source was given.
    struct PriorityGroup {
        int priority = 0;           // Priority: a smaller number wins a 3D voice (005d8eb0)
        int volume = 127;           // Volume 0..127 (constructor default 005d6c10)
        int maxPlaying = 255;       // MaxPlaying
        bool interrupt = true;      // Interrupt: the new sound interrupts one playing when the cap is reached
        int fadeTimeMs = 0;         // FadeTime
        float maxVolumeDist = 20.0f; // MaxVolumeDist: full level inside it (constructor default 20)
        float minVolumeDist = 10.0f; // MinVolumeDist: silent beyond it (constructor default 10)
        float playbackVariance = 0.0f; // PlaybackVariance: random rate change of every play, 0..1
    };
    enum PriorityGroupId : int {
        kGroupUnmaskable = 0, kGroupStinger = 1, kGroupMusic = 2, kGroupOneShotStream = 3,
        kGroupLoopAreaWide = 4, kGroupLoopPositional = 5, kGroupLoopPlayer = 6, kGroupLoopNonPlayer = 7,
        kGroupPlayerChat = 8, kGroupNonPlayerChat = 9, kGroupScripted = 10, kGroupGui = 11,
        kGroupAreaSpell = 12, kGroupSpell = 13, kGroupProjectile = 14, kGroupCombat = 15,
        kGroupLargeVoice = 16, kGroupSmallVoice = 17, kGroupLargeFootstep = 18,
        kGroupPlayerFootstep = 19, kGroupSmallFootstep = 20, kGroupSingleGlobal = 21,
        kGroupSinglePositional = 22, kGroupDefault = 23, kGroupMiniGameEnemy = 24, kGroupRoom = 25,
        kGroupBark = 26
    };
    const PriorityGroup& priorityGroup(int group) const;
    // The volume setting a group answers to (retail 005d6a90 / 005d6ad0): the dialog volume for the
    // chat, creature voice and bark groups, the music volume for stingers and streamed music, the
    // sound-effects volume for everything else.
    static audio::Bus groupBus(int group);
    // Retail 005db800: the sample level is floor(groupVolume * sourceVolume / 127) / 127, the user
    // setting is the bus. Returns the 0..1 level before the bus.
    float groupLevel(int group, int sourceVolume127 = 127) const;
    // ExcitedDuration.2da (005c2b80): milliseconds of the Damage (1) and SpellCast (2) rows.
    int excitedDurationMs(int row) const { return row >= 0 && row < 3 ? excitedMs_[row] : 0; }
    // The mixer voice volume for a sound of this group: a stream scales linearly (005dc930), a 2D
    // sample by groupLevel, a 3D sample by pow(level * setting, 0.6) (005db800).
    float voiceLevel(int group, int sourceVolume127, bool stream, bool positional) const;

    struct GroupSound {
        int group = kGroupDefault;
        const kotor::Vec3* position = nullptr; // null: 2D
        float zOffset = 0.0f;                  // 005d5e10 adds it to the position's Z
        int sourceVolume127 = 127;             // CExoSoundSource::SetVolume byte (default 127)
        float fixedVariance = 1.0f;            // constant rate factor (accepted 0.5..2.0, 005db980)
        float pitchVariance = -1.0f;           // < 0: the group's PlaybackVariance; else 0..1 (005dba40)
        bool loop = false;
        bool stream = false;                   // a streaming source (voice, music): linear level
        bool useGroupDistances = true;         // false: minDistance / maxDistance below
        float minDistance = 0.0f;
        float maxDistance = 0.0f;
    };
    // Starts a sound the way retail does through a priority group: group level and bus, the group's
    // distances, the MaxPlaying cap (Interrupt stops the oldest playing sound of the group, otherwise
    // the new sound is dropped, 005db4d0) and the random rate of PlaybackVariance (005db3d0).
    // Returns the mixer voice or -1.
    int playGroupSound(const std::string& resref, const GroupSound& request);
    // Creature footstep, retail CSWCCreature::PlayFootstep 0061a2d0.
    struct FootstepRequest {
        int footstepType = 0;      // appearance.2da footsteptype, -1 blank
        int sizeCategory = 3;      // appearance.2da sizecategory
        bool isPlayer = false;     // the controlled creature (CClientExoApp::GetPlayerCreatureId)
        bool stealth = false;      // creature+0x194 == 1: footstepsounds row 0
        int surface = -1;          // surfacemat.2da row under the creature (-1: none)
        kotor::Vec3 position;
    };
    // `resource`, when given, receives the footstep sound that was picked.
    int playCreatureFootstep(const FootstepRequest& request, std::string* resource = nullptr);
    // Retail CSWCCreature::PlaySoundSetSound 00611470. `slot` is the SSF index (retail passes slot + 1).
    struct SoundSetVoice {
        bool isPlayer = false;   // the controlled creature: Player_Chat group 8
        bool isParty = false;    // another party member (GetPlayerByGameObjectID 005ed590): NonPlayer_Chat group 9
        int sizeCategory = 3;    // others: medium and small 17, large 16
        bool dead = false;       // a dead creature only plays its death cry (slot 15)
    };
    // Plays one sound-set slot of `entity` at the entity's position; a creature has one voice, the
    // previous one is cut. Returns true when a voice started.
    bool playSoundSetSlot(std::uint32_t entity, int slot, const SoundSetVoice& who);
    // The listener, as the camera's followed creature (005f5370).
    kotor::Vec3 listenerPosition() const { return listener_; }
    // rand() stand-in shared by the footstep column and the playback variance (deterministic).
    std::uint32_t random();
    // Sound objects (GIT SoundList): AreaAmbience runs them from area load on;
    // these are the script-facing controls (volumes here are 0..1, the script
    // actions use ambience() for the retail 0..127 scale).
    // Gameplay sound effect (effects bus) with an explicit distance model; returns the
    // mixer voice or -1 when the resource cannot be decoded. `volume` is 0..1.
    int playSfx(const std::string& resref, const kotor::Vec3* position, float volume,
                float minDistance, float maxDistance, bool loop = false);
    // guisounds.2da row ids (retail 00409f00 reads the table by row).
    static constexpr int kGuiClicked = 0, kGuiEntered = 1, kGuiError = 2, kGuiCheckbox = 3,
                         kGuiPanelAdded = 4, kGuiPanelRemoved = 5, kGuiActionUse = 6,
                         kGuiActionScroll = 7, kGuiActionQueue = 8, kGuiInventoryAdd = 9,
                         kGuiInventorySelect = 10, kGuiInventoryDrop = 11, kGuiLevelUp = 12,
                         kGuiQuestNew = 13, kGuiQuestComplete = 14, kGuiPrompt = 15,
                         kGuiUpgrade = 16;
    // UI sound by guisounds.2da row (see kGui*): the screens call this on a click,
    // hover, error or panel open. Returns the mixer voice or -1.
    int playGuiSound(int row);
    bool playSoundObject(std::uint32_t entity);
    bool playCreatureSound(std::uint32_t entity, int slot);
    // The sound set (SSF resref) an entity speaks with; for entities that appear after loadScene.
    void setCreatureSoundSet(std::uint32_t entity, const std::string& resref);
    bool playFootstep(int footstepType, int surface, kotor::Vec3 position,
                      float volume = 0.6f);
    bool stopSoundObject(std::uint32_t entity);
    bool setSoundObjectVolume(std::uint32_t entity, float volume);
    bool setSoundObjectPosition(std::uint32_t entity, kotor::Vec3 position);
    bool playVoice(const std::string& resref);

    // The player's music setting (options): survives area changes.
    void setMusicVolume(float volume)
    {
        musicVolume_ = volume;
        applyMusicBus();
    }
    float musicVolume() const { return musicVolume_; }
    // SetMusicVolume (ACT_765): a script multiplier on top of the player's setting.
    void setMusicScriptVolume(float volume)
    {
        musicScriptVolume_ = volume;
        applyMusicBus();
    }
    float musicScriptVolume() const { return musicScriptVolume_; }

    // ---- area ambience (game/Ambience.h) ----------------------------------------
    // Ticks fades, MusicDelay, the sound objects and the ambient room scale.
    void update(float dt);
    // Retail time-of-day state (1 day, 2 night, 3 dawn, 4 dusk) and the game hour.
    void setTimeOfDay(int dayState, int hour);
    // The party's combat state: rising edge starts the area's battle track and a
    // stinger, falling edge returns to the area track.
    void setCombatState(bool inCombat);
    // The conversation's AmbientTrack (a streammusic resref) overrides the area and
    // battle tracks until stopped. Called by the dialogue code.
    void playConversationTrack(const std::string& resref);
    void stopConversationTrack();
    // Menu and loading-screen music (streammusic resources; the main-menu theme is
    // named by retail 005f9af0, not by ambientmusic.2da). They win over every other
    // layer and end with the next area load or stopUiMusic().
    bool playMenuMusic(bool alternate = false);
    bool playLoadScreenMusic();
    void stopUiMusic();
    // While a movie plays the world buses are muted (the Movie bus is left alone),
    // without touching the stored bus volumes.
    void setMovieMode(bool on);
    bool movieMode() const { return movieMode_; }
    AreaAmbience& ambience() { return *ambience_; }
    const AreaAmbience& ambience() const { return *ambience_; }
    // The retail defaults read from swkotor.ini [Sound Options] (percent / 100).
    struct UserVolumes {
        float music = 0.85f;    // "Music Volume"
        float voice = 0.85f;    // "Voiceover Volume"
        float effects = 0.85f;  // "Sound Effects Volume" (also drives the ambient loop)
        float movie = 1.0f;     // "Movie Volume"
    };
    const UserVolumes& userVolumes() const { return userVolumes_; }
    void setUserVolumes(const UserVolumes& volumes);
    audio::Mixer& mixer() { return mixer_; }
    const audio::Mixer& mixer() const { return mixer_; }
    const kotor::AreaAudio& areaAudio() const { return areaAudio_; }
    std::size_t soundEmitterCount() const { return soundEmitters_.size(); }
    std::size_t loadedSoundCount() const { return sounds_.size(); }
    bool hasSound(const std::string& resref) const;
    bool decodeProbe(const std::string& resref, audio::SoundData& out, std::string& error) const;
    const audio::SoundData* sound(const std::string& resref) const;

private:
    friend class AreaAmbience;
    void applyMusicBus();
    void readUserVolumes();
    std::shared_ptr<const audio::SoundData> loadSound(const std::string& resref,
                                                      std::string& error);
    std::string tableResource(const char* table, std::int32_t id) const;
    void indexLooseAudio();

    kotor::Install& install_;
    audio::Mixer mixer_;
    std::map<std::string, std::filesystem::path> looseFiles_;
    std::map<std::string, std::shared_ptr<const audio::SoundData>> sounds_;
    std::map<std::string, std::uint64_t> soundLastUse_;
    std::set<std::string> soundsEverLoaded_; // hasSound stays true after an eviction
    std::uint64_t soundUseClock_ = 0;
    std::size_t soundCacheBytes_ = 0;
    void trimSoundCache(const std::string& keep);
    std::optional<kotor::TwoDa> musicTable_;
    std::optional<kotor::TwoDa> ambientTable_;
    std::optional<kotor::TwoDa> eaxTable_;
    std::optional<kotor::TwoDa> footstepTable_;
    std::optional<kotor::TwoDa> guiSoundTable_;
    kotor::AreaAudio areaAudio_;
    std::vector<kotor::SoundEmitter> soundEmitters_;
    std::map<std::uint32_t, std::string> entitySoundSets_;
    std::map<std::uint32_t, kotor::Vec3> entityPositions_;
    std::map<std::string, kotor::Ssf> soundSetCache_;
    const kotor::Tlk* tlk_ = nullptr;
    std::uint32_t footstepCounter_ = 0;
    std::vector<PriorityGroup> groups_;
    std::optional<kotor::TwoDa> surfaceTable_;
    kotor::Vec3 listener_{};
    std::uint32_t rng_ = 0x9E3779B9u;
    struct GroupVoice {
        int voice = 0;
        int group = 0;
        std::uint64_t serial = 0;
    };
    std::vector<GroupVoice> groupVoices_;
    std::uint64_t groupSerial_ = 0;
    std::map<std::uint32_t, int> creatureVoices_; // entity -> sound-set voice
    ObstructionProbe obstructionProbe_;
    struct ObstructionSample {
        kotor::Vec3 source;
        kotor::Vec3 listener;
    };
    std::map<int, ObstructionSample> obstructionCache_; // voice -> where it was last measured
    void loadPriorityGroups();
    int excitedMs_[3] = {0, 10000, 10000};
    int enforceGroupCap(int group);
    float musicVolume_ = 0.85f;
    float musicScriptVolume_ = 1.0f;
    float ambientVolume_ = 0.85f;
    UserVolumes userVolumes_;
    bool movieMode_ = false;
    std::unique_ptr<AreaAmbience> ambience_;
    std::string currentRoom_;
    int roomEnvironment_ = -1;
};

} // namespace ke
