#pragma once

// Area ambience: the music director (day/night, battle, conversation and UI layers,
// stingers, MusicDelay), the ambient loop, and the GIT SoundList sound-object runtime.
// It sits behind AudioSystem (which owns it) and drives audio::Mixer. Evidence comes
// from swkotor.exe 1.0.3.0 (addresses in Ambience.cpp); anything inferred is labelled
// there. Nothing here has been listened to: the gate (--family-test ambience) measures
// the mixer.

#include "audio/Audio.h"
#include "kotor/Formats.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ke {

class AudioSystem;
class GameSession;

// Which source owns the music bus. Higher layers win (retail 0068dd30: the override
// track, then battle, then the area background).
enum class MusicLayer { None = 0, Area = 1, Battle = 2, Conversation = 3, Ui = 4 };

// Retail time-of-day state (004c6210): 1 day, 2 night, 3 dawn, 4 dusk.
std::int32_t gameDayState(GameSession& session, std::int32_t& hour);
// Per-frame glue called from GameSession::update: time of day and the party's combat
// state go to the audio system, then the audio system is ticked.
void tickAmbience(GameSession& session, float dt);
// Script handlers for the audio actions (ids 124, 188, 413-416, 425-436, 558, 559,
// 567-569, 689-691, 745, 765); registered last so they override older stubs.
class HandlerTable;
void registerAmbienceActions(HandlerTable& table);
// --family-test ambience.
int runAmbienceFamilyTest(GameSession& game);

class AreaAmbience {
public:
    static constexpr int kMax3dVoices = 16;      // swkotor.ini "Number 3D Voices"
    static constexpr float kMusicFade = 1.0f;    // retail 0068dd30 fades out over 1000 ms (PriorityGroups row 2 FadeTime)
    static constexpr float kAmbientFadeIn = 1.0f;  // PriorityGroups row 4 FadeTime 1000 ms
    static constexpr float kAmbientFadeOut = 6.0f; // 0068ebf7: FadeAndStop(0x1770) on the ambient loop
    static constexpr double kMusicRetryMs = 10000.0; // 0068e1f0 stores 0x2710 in +0xd0 when a play fails

    explicit AreaAmbience(AudioSystem& owner);

    // Rebuilds everything from the owner's area audio and emitters (after loadScene).
    void startArea();
    void update(float dt);
    void setListener(kotor::Vec3 position);
    void setListenerRoomScale(float scale);
    void setSeed(std::uint64_t seed) { rngState_ = seed; }

    // ---- time of day ------------------------------------------------------------
    void setTimeOfDay(std::int32_t dayState, std::int32_t hour);
    // Dawn counts as day and dusk as night (retail 0066ae90 and 0068d690 switch on the
    // state this way: 0, 1, 3 are one side, 2 and 4 the other).
    bool isDay() const;

    // ---- music ------------------------------------------------------------------
    // The legacy "play this ambientmusic row now" call: becomes the area track until
    // the next area load.
    bool forceMusicRow(std::int32_t row);
    void setAreaMusicEnabled(bool on);
    bool areaMusicEnabled() const { return areaMusicOn_; }
    void setMusicDelay(int milliseconds);
    void changeMusicTrack(MusicLayer layer, bool night, std::int32_t row);
    void setBattle(bool on);
    bool battle() const { return battleOn_; }
    // Edge-triggered: a rising edge starts battle music, a falling edge stops it.
    void setCombat(bool inCombat);
    // Retail's battle music rule (CSWSCreature::SetExcitedState 004f3580 / UpdateExcitedStateTimer 004ed110):
    // a creature that is (re)armed for combat gets an excited timer of ExcitedDuration.2da (10 s); when the
    // new time is longer than the one left and an enemy stands within 30 m the battle music starts, and
    // when the timer runs out it stops. Call once per tick for every creature.
    void exciteTick(std::uint32_t object, bool inCombat, float combatTimer, bool enemyWithin30m, float dt);
    void playConversationTrack(const std::string& resref);
    void stopConversationTrack();
    bool playUiMusic(const std::string& resref, float volume);
    bool playUiMusicRow(std::int32_t row, float volume);
    void stopUiMusic();
    void stopAllMusic();
    MusicLayer musicLayer() const { return music_.layer; }
    const std::string& musicResource() const { return music_.resource; }
    int musicVoice() const { return music_.voice; }
    int stingerVoice() const { return stingerVoice_; }
    const std::string& stingerResource() const { return stingerResource_; }
    double musicDelayLeftMs() const { return delayLeftMs_; }
    // Chosen resref of an ambientmusic row ("resource" column), or empty.
    std::string musicRowResource(std::int32_t row) const;

    // ---- ambient loop -----------------------------------------------------------
    bool forceAmbientRow(std::int32_t row);
    void setAmbientEnabled(bool on);
    bool ambientEnabled() const { return ambientOn_; }
    void changeAmbientTrack(bool night, std::int32_t row);
    void setAmbientVolume(bool night, std::int32_t volume127);
    // The voice level the ambient loop plays at: area volume (0..127)/127 for the
    // current half of the day, times the room's AmbientScale (ramped at 1.0/s).
    float ambientLevel() const;
    float ambientScale() const { return roomScale_; }
    int ambientVoice() const { return ambientVoice_; }
    const std::string& ambientResource() const { return ambientResource_; }

    // ---- sound objects ----------------------------------------------------------
    struct SoundObjectInfo {
        bool found = false;
        bool active = false;
        bool playing = false;
        int voice = 0;
        int index = 0;
        std::string sound;
        double intervalLeftMs = 0.0;
        float volume127 = 0.0f;
        float lastPitch = 1.0f;
        kotor::Vec3 lastPosition;
    };
    std::size_t soundObjectCount() const { return emitters_.size(); }
    SoundObjectInfo soundObject(std::size_t emitterIndex) const;
    // Emitter of a scene entity, or size_t(-1).
    std::size_t emitterForEntity(std::uint32_t entity) const;
    bool playSoundObject(std::uint32_t entity);
    bool stopSoundObject(std::uint32_t entity);
    bool fadeAndStopSoundObject(std::uint32_t entity, float seconds);
    bool setSoundObjectVolume127(std::uint32_t entity, int volume);
    int soundObjectVolume127(std::uint32_t entity) const;
    bool setSoundObjectPosition(std::uint32_t entity, kotor::Vec3 position);
    bool setSoundObjectPitchVariance(std::uint32_t entity, float variance);
    float soundObjectPitchVariance(std::uint32_t entity) const;
    bool setSoundObjectFixedVariance(std::uint32_t entity, float variance);
    float soundObjectFixedVariance(std::uint32_t entity) const;
    std::size_t activeSoundVoices() const;
    // Times (1 day, 2 night, 3 always, 0 = the Hours bit mask) and Hours of a sound
    // object: the UTS values, exposed so a gate can exercise the gating (retail 0066ae90).
    bool setSoundObjectTimes(std::uint32_t entity, int times, std::uint32_t hours);

private:
    struct Rng {
        std::uint64_t* state;
        std::uint64_t next();
        float uniform();                      // [0, 1)
        int below(int n);                     // [0, n), n > 0
    };

    struct MusicState {
        int voice = 0;
        std::string resource;
        MusicLayer layer = MusicLayer::None;
        bool loops = true;
    };
    struct Desired {
        MusicLayer layer = MusicLayer::None;
        std::string resource;
        float volume = 1.0f;
    };
    struct Emitter {
        kotor::SoundEmitter def;
        float fixedClient = 1.0f;     // the factor the client sample holds (0.5..2.0 only)
        bool active = false;
        bool stopping = false;        // faded out by SoundObjectFadeAndStop
        bool needsVoice = false;      // an admitted play lost its voice (budget)
        bool first = true;
        int voice = 0;
        int index = 0;
        std::vector<int> order;
        double intervalMs = 0.0;
        double retryMs = 0.0;
        float lastPitch = 1.0f;
        kotor::Vec3 lastPosition;
        int lastVolume127 = 0;
    };

    Desired desiredMusic() const;
    void refreshMusic(bool immediate);
    int startMusicVoice(const std::string& resource, MusicLayer layer, bool fadeIn,
                        float volume);
    void playStinger();
    float layerGain(MusicLayer layer) const;
    void refreshAmbient(bool immediate);
    void applyAmbientVolume();
    bool timesAllow(const Emitter& emitter) const;
    bool voiceLive(const Emitter& emitter) const;
    void updateEmitter(Emitter& emitter, float dt);
    bool startEmitterSound(Emitter& emitter);
    void shuffle(Emitter& emitter);
    float distanceTo(const kotor::Vec3& at) const;
    std::int32_t musicIdFor(MusicLayer layer, bool night) const;
    Rng rng() { return Rng{&rngState_}; }

    AudioSystem& owner_;
    std::uint64_t rngState_ = 0x9E3779B97F4A7C15ULL;
    kotor::Vec3 listener_;
    bool listenerSet_ = false;
    bool timeKnown_ = false;
    bool emittersPending_ = false;
    std::int32_t dayState_ = 1;
    std::int32_t hour_ = 12;

    // music
    MusicState music_;
    bool areaMusicOn_ = true;
    bool battleOn_ = false;
    bool prevCombat_ = false;
    std::int32_t forcedMusicRow_ = -1;
    std::string conversationTrack_;
    std::string uiResource_;
    float uiVolume_ = 1.0f;
    double delayLeftMs_ = -1.0;
    double musicRetryMs_ = -1.0;
    struct Excited {
        double leftMs = 0.0;
        float lastTimer = 0.0f;
        bool wasIn = false;
    };
    std::map<std::uint32_t, Excited> excited_;
    int stingerVoice_ = 0;
    std::string stingerResource_;

    // ambient
    bool ambientOn_ = true;
    std::int32_t forcedAmbientRow_ = -1;
    int ambientVoice_ = 0;
    std::string ambientResource_;
    float roomScale_ = 1.0f;
    float roomScaleTarget_ = 1.0f;

    std::vector<Emitter> emitters_;
};

} // namespace ke
