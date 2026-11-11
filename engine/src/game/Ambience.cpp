#include "game/Ambience.h"

#include "game/AudioSystem.h"

#include <algorithm>
#include <cmath>

// Evidence (swkotor.exe 1.0.3.0), used below and labelled where only inferred:
//  - 005c9040 / 005c86d0: the server sound object loads and saves Active, Positional,
//    Looping, Volume, VolumeVrtn, Times, PitchVariation, Hours, GeneratedType,
//    Interval, IntervalVrtn, MinDistance, MaxDistance, Continuous, Random,
//    FixedVariance, RandomPosition, RandomRangeX/Y and Sounds. It does not read
//    Elevation or Priority: both are toolset-only and are ignored here.
//    005c8f30 holds the constructor defaults (Volume 127, Active 1, Positional 1,
//    Times 3, FixedVariance 1.0, MinDistance 10, MaxDistance 20).
//  - 005c8df0: Volume is clamped to 0..127 (the stream and sample volume setters
//    5d5950 / 5d5b50 clamp to 0x7f too); the sample gain is volume / 127.
//  - 0066bbd0: the client sound object tick. The sounds of the list play in order,
//    one after the other; after a sound ends the Interval (0066b000: Interval +- a
//    random up to IntervalVrtn, floored at 0) runs; at the end of the list a
//    Continuous object starts again (a Random one reshuffles the list first, 0066b980)
//    and a non-continuous one stops and clears Active. The first sound plays at once.
//  - 0066afa0: each play draws its volume as Volume + (rand % (2*VolumeVrtn) -
//    VolumeVrtn), clamped to 0..127.
//  - 0066ae90: Times gates playback by the time-of-day state (1 day only, 2 night only,
//    3 always; Times 0 uses the Hours bit mask for the current hour).
//  - 005db980 / 005db3d0: FixedVariance is a constant playback-rate factor accepted only
//    in 0.5..2.0; PitchVariation (clamped to 0..1) shifts the rate by a random amount of
//    up to that fraction up or down on every play.
//  - 0068d690: the client switches the area music and ambient loop when the time-of-day
//    state changes (states 0, 1, 3 are day, 2 and 4 night).
//  - 0068dd30: music layers with priority (override 4, battle 2, background 1) and a
//    1000 ms fade out; 0068e1f0 is the music start itself (FadeAndStop 800 ms when the area
//    track stops, 1000 ms on a layer switch, a failed play retries after 10 s).
//  - PriorityGroups.2da: the area, battle and menu music play in group 2 (Streams_Music,
//    volume 64, music volume), the stinger in group 1, the conversation AmbientTrack and the
//    ambient loop in group 4 (Looping_AreaWide, volume 95, sound-effects volume); streams
//    scale linearly by group/127 * source/127 (005dc930).
//  - 0068db30: a stinger is chosen at random among the non-empty Stinger1..3 columns
//    of the battle row and played once as its own stream.
//  - 004f3580 / 004ed110: a creature entering combat arms a timer that starts the
//    battle track; when the timer expires the track stops (ScriptCombat's 8 s combat
//    state stands in for that timer: the DURATION 2DA it reads was not traced).
//  - swkotor.ini [Sound Options] "Number 3D Voices=16".
// Inferred (no retail trace): the fade-in length, the full-extent box of
// RandomPosition, the MusicDelay replay rule (the area track plays once, then waits
// MusicDelay ms before it plays again; 0 loops without a gap), and the hand-off of a
// stolen voice when more than 16 positional sounds want to play.

namespace ke {

namespace {

bool realName(const std::string& text)
{
    return !text.empty() && text != "****";
}

constexpr double kRetryMs = 250.0;

} // namespace

std::uint64_t AreaAmbience::Rng::next()
{
    *state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = *state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

float AreaAmbience::Rng::uniform()
{
    return static_cast<float>(next() >> 40) / 16777216.0f;
}

int AreaAmbience::Rng::below(int n)
{
    return n <= 1 ? 0 : static_cast<int>(next() % static_cast<std::uint64_t>(n));
}

AreaAmbience::AreaAmbience(AudioSystem& owner)
    : owner_(owner)
{
}

// ---- lifecycle -----------------------------------------------------------------------

void AreaAmbience::startArea()
{
    music_ = {};
    battleOn_ = false;
    prevCombat_ = false;
    forcedMusicRow_ = -1;
    conversationTrack_.clear();
    uiResource_.clear();
    uiVolume_ = 1.0f;
    areaMusicOn_ = true;
    delayLeftMs_ = -1.0;
    musicRetryMs_ = -1.0;
    excited_.clear();
    stingerVoice_ = 0;
    stingerResource_.clear();
    ambientOn_ = true;
    forcedAmbientRow_ = -1;
    ambientVoice_ = 0;
    ambientResource_.clear();
    roomScale_ = 1.0f;
    roomScaleTarget_ = 1.0f;
    timeKnown_ = false;

    emitters_.clear();
    for (const kotor::SoundEmitter& def : owner_.soundEmitters_) {
        Emitter emitter;
        emitter.def = def;
        emitter.active = def.active && !def.sounds.empty();
        emitter.fixedClient = def.fixedVariance >= 0.5f && def.fixedVariance <= 2.0f
            ? def.fixedVariance : 1.0f;
        emitter.order.resize(def.sounds.size());
        for (std::size_t i = 0; i < emitter.order.size(); ++i) {
            emitter.order[i] = static_cast<int>(i);
        }
        emitter.lastPosition = def.position;
        emitters_.push_back(std::move(emitter));
    }
    emittersPending_ = true;
    refreshMusic(true);
    refreshAmbient(true);
}

void AreaAmbience::setListener(kotor::Vec3 position)
{
    listener_ = position;
    listenerSet_ = true;
    if (emittersPending_) {
        // The first listener position after a load decides which of the nearest 16
        // positional sounds get a voice, so the start waits for it.
        emittersPending_ = false;
        update(0.0f);
    }
}

void AreaAmbience::setListenerRoomScale(float scale)
{
    roomScaleTarget_ = std::clamp(scale, 0.0f, 4.0f);
}

void AreaAmbience::update(float dt)
{
    audio::Mixer& mixer = owner_.mixer_;
    emittersPending_ = false;

    // AmbientScale ramps at 1.0 per second (docs/recon/51_AUDIO.md, retail 005ee860).
    if (roomScale_ != roomScaleTarget_) {
        const float step = std::max(dt, 0.0f);
        roomScale_ = roomScale_ < roomScaleTarget_
            ? std::min(roomScaleTarget_, roomScale_ + step)
            : std::max(roomScaleTarget_, roomScale_ - step);
        applyAmbientVolume();
    }

    // MusicDelay: the area track plays once, then waits before it plays again.
    if (music_.layer == MusicLayer::Area && !music_.loops
        && !(music_.voice != 0 && mixer.voiceActive(music_.voice))) {
        if (delayLeftMs_ < 0.0) {
            delayLeftMs_ = static_cast<double>(std::max(owner_.areaAudio_.musicDelay, 0));
        } else {
            delayLeftMs_ -= static_cast<double>(dt) * 1000.0;
        }
        if (delayLeftMs_ <= 0.0) {
            music_ = {};
            delayLeftMs_ = -1.0;
            refreshMusic(true);
        }
    }

    if (musicRetryMs_ >= 0.0) {
        musicRetryMs_ -= static_cast<double>(dt) * 1000.0;
        if (musicRetryMs_ <= 0.0) {
            musicRetryMs_ = -1.0;
            refreshMusic(false);
        }
    }

    // Nearest first, so a full 3D voice budget goes to the closest sounds.
    std::vector<std::size_t> order(emitters_.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return distanceTo(emitters_[a].def.position) < distanceTo(emitters_[b].def.position);
    });
    for (const std::size_t index : order) {
        updateEmitter(emitters_[index], dt);
    }
}

// ---- time of day -----------------------------------------------------------------------

bool AreaAmbience::isDay() const
{
    return dayState_ != 2 && dayState_ != 4;
}

void AreaAmbience::setTimeOfDay(std::int32_t dayState, std::int32_t hour)
{
    const bool wasDay = isDay();
    const bool known = timeKnown_;
    dayState_ = dayState;
    hour_ = hour;
    timeKnown_ = true;
    if (!known || wasDay != isDay()) {
        // The first report after a load picks the right half without a crossfade.
        refreshMusic(!known);
        refreshAmbient(!known);
    }
}

// ---- music -------------------------------------------------------------------------------

std::string AreaAmbience::musicRowResource(std::int32_t row) const
{
    std::string resource = owner_.tableResource("ambientmusic", row);
    return realName(resource) ? resource : std::string{};
}

AreaAmbience::Desired AreaAmbience::desiredMusic() const
{
    const kotor::AreaAudio& area = owner_.areaAudio_;
    if (!uiResource_.empty()) {
        return {MusicLayer::Ui, uiResource_, uiVolume_};
    }
    if (!conversationTrack_.empty()) {
        return {MusicLayer::Conversation, conversationTrack_, 1.0f};
    }
    if (battleOn_) {
        const std::string resource = musicRowResource(area.musicBattleId);
        if (!resource.empty()) {
            return {MusicLayer::Battle, resource, 1.0f};
        }
    }
    if (areaMusicOn_) {
        const std::int32_t row = forcedMusicRow_ >= 0
            ? forcedMusicRow_
            : (isDay() ? area.musicDayId : area.musicNightId);
        const std::string resource = musicRowResource(row);
        if (!resource.empty()) {
            return {MusicLayer::Area, resource, 1.0f};
        }
    }
    return {};
}

int AreaAmbience::startMusicVoice(const std::string& resource, MusicLayer layer, bool fadeIn,
                                  float volume)
{
    std::string error;
    const std::shared_ptr<const audio::SoundData> data = owner_.loadSound(resource, error);
    if (!data) {
        return 0;
    }
    const bool loops = layer != MusicLayer::Area || owner_.areaAudio_.musicDelay <= 0;
    audio::Mixer& mixer = owner_.mixer_;
    const int voice = mixer.play(data, layer == MusicLayer::Conversation ? audio::Bus::Effects
                                                                         : audio::Bus::Music,
                                 volume * layerGain(layer), loops, false);
    if (voice != 0 && fadeIn) {
        mixer.setVoiceFade(voice, 0.0f);
        mixer.fadeVoice(voice, 1.0f, kMusicFade, false);
    }
    music_.loops = loops;
    return voice;
}

void AreaAmbience::refreshMusic(bool immediate)
{
    audio::Mixer& mixer = owner_.mixer_;
    const Desired wanted = desiredMusic();
    const bool alive = music_.voice != 0 && mixer.voiceActive(music_.voice);
    const bool waiting = music_.layer == MusicLayer::Area && !music_.loops && !alive
        && delayLeftMs_ >= 0.0;
    if (wanted.layer == music_.layer && wanted.resource == music_.resource
        && (alive || waiting)) {
        if (alive && music_.layer == MusicLayer::Ui) {
            mixer.setVoiceVolume(music_.voice, wanted.volume * layerGain(wanted.layer));
        }
        return;
    }
    const MusicLayer previous = music_.layer;
    if (alive) {
        if (immediate) {
            mixer.stopVoice(music_.voice);
        } else {
            // 0068e1f0: stopping the area track fades it out over 800 ms, a switch to another layer
            // over 1000 ms.
            const bool stoppingArea = wanted.layer == MusicLayer::None && music_.layer == MusicLayer::Area;
            mixer.fadeVoice(music_.voice, 0.0f, stoppingArea ? 0.8f : kMusicFade, true);
        }
    }
    music_ = {};
    delayLeftMs_ = -1.0;
    musicRetryMs_ = -1.0;
    if (wanted.layer == MusicLayer::None) {
        return;
    }
    const int voice = startMusicVoice(wanted.resource, wanted.layer, alive && !immediate,
                                      wanted.volume);
    if (voice == 0) {
        // 0068e1f0: a play that fails is tried again 10 s later.
        musicRetryMs_ = kMusicRetryMs;
        return;
    }
    musicRetryMs_ = -1.0;
    music_.voice = voice;
    music_.resource = wanted.resource;
    music_.layer = wanted.layer;
    if (wanted.layer == MusicLayer::Battle && previous != MusicLayer::Battle) {
        playStinger();
    }
}

float AreaAmbience::layerGain(MusicLayer layer) const
{
    // 0068e610 / 0068e1f0 / 005f9af0 pass group 2; the conversation AmbientTrack (0062a8b0) group 4.
    const int group = layer == MusicLayer::Conversation ? AudioSystem::kGroupLoopAreaWide
                                                        : AudioSystem::kGroupMusic;
    return static_cast<float>(owner_.priorityGroup(group).volume) / 127.0f;
}

void AreaAmbience::playStinger()
{
    const std::optional<kotor::TwoDa>& table = owner_.musicTable_;
    const std::int32_t row = owner_.areaAudio_.musicBattleId;
    if (!table.has_value() || row < 0) {
        return;
    }
    const std::string key = std::to_string(row);
    if (table->rowIndex(key) < 0) {
        return;
    }
    std::vector<std::string> stingers;
    for (const char* column : {"stinger1", "stinger2", "stinger3"}) {
        const std::string name = table->cell(key, column);
        if (realName(name)) {
            stingers.push_back(name);
        }
    }
    if (stingers.empty()) {
        return;
    }
    const std::string& pick = stingers[static_cast<std::size_t>(
        rng().below(static_cast<int>(stingers.size())))];
    std::string error;
    const std::shared_ptr<const audio::SoundData> data = owner_.loadSound(pick, error);
    if (!data) {
        return;
    }
    // 0068db30: group 1 (Stingers), volume 64, at most one at a time.
    stingerVoice_ = owner_.mixer_.play(
        data, audio::Bus::Music,
        static_cast<float>(owner_.priorityGroup(AudioSystem::kGroupStinger).volume) / 127.0f,
        false, false);
    stingerResource_ = pick;
}

bool AreaAmbience::forceMusicRow(std::int32_t row)
{
    if (musicRowResource(row).empty()) {
        return false;
    }
    forcedMusicRow_ = row;
    areaMusicOn_ = true;
    refreshMusic(false);
    return true;
}

void AreaAmbience::setAreaMusicEnabled(bool on)
{
    areaMusicOn_ = on;
    if (on) {
        forcedMusicRow_ = -1;
    }
    refreshMusic(false);
}

void AreaAmbience::setMusicDelay(int milliseconds)
{
    owner_.areaAudio_.musicDelay = milliseconds;
}

void AreaAmbience::changeMusicTrack(MusicLayer layer, bool night, std::int32_t row)
{
    kotor::AreaAudio& area = owner_.areaAudio_;
    const std::string resource = musicRowResource(row);
    if (layer == MusicLayer::Battle) {
        area.musicBattleId = row;
        area.musicBattle = resource;
    } else if (night) {
        area.musicNightId = row;
        area.musicNight = resource;
    } else {
        area.musicDayId = row;
        area.musicDay = resource;
    }
    if (layer == MusicLayer::Area) {
        forcedMusicRow_ = -1;
    }
    refreshMusic(false);
}

void AreaAmbience::setBattle(bool on)
{
    if (battleOn_ == on) {
        return;
    }
    battleOn_ = on;
    refreshMusic(false);
}

void AreaAmbience::setCombat(bool inCombat)
{
    if (inCombat && !prevCombat_) {
        setBattle(true);
    } else if (!inCombat && prevCombat_) {
        setBattle(false);
    }
    prevCombat_ = inCombat;
}

void AreaAmbience::exciteTick(std::uint32_t object, bool inCombat, float combatTimer,
                              bool enemyWithin30m, float dt)
{
    Excited& state = excited_[object];
    const bool armed = inCombat && (!state.wasIn || combatTimer > state.lastTimer + 1.0e-4f);
    state.wasIn = inCombat;
    state.lastTimer = combatTimer;
    if (armed) {
        const double duration = static_cast<double>(owner_.excitedDurationMs(1)); // row 1: Damage
        if (duration > state.leftMs) {
            state.leftMs = duration;
            if (enemyWithin30m) {
                setBattle(true);
            }
        }
        return;
    }
    if (state.leftMs > 0.0) {
        state.leftMs -= static_cast<double>(dt) * 1000.0;
        if (state.leftMs <= 0.0) {
            state.leftMs = 0.0;
            setBattle(false);
        }
    }
}

void AreaAmbience::playConversationTrack(const std::string& resref)
{
    conversationTrack_ = resref;
    refreshMusic(false);
}

void AreaAmbience::stopConversationTrack()
{
    if (conversationTrack_.empty()) {
        return;
    }
    conversationTrack_.clear();
    refreshMusic(false);
}

bool AreaAmbience::playUiMusic(const std::string& resref, float volume)
{
    std::string error;
    if (resref.empty() || !owner_.loadSound(resref, error)) {
        return false;
    }
    uiResource_ = resref;
    uiVolume_ = volume;
    refreshMusic(false);
    return true;
}

bool AreaAmbience::playUiMusicRow(std::int32_t row, float volume)
{
    return playUiMusic(musicRowResource(row), volume);
}

void AreaAmbience::stopUiMusic()
{
    if (uiResource_.empty()) {
        return;
    }
    uiResource_.clear();
    refreshMusic(false);
}

void AreaAmbience::stopAllMusic()
{
    areaMusicOn_ = false;
    battleOn_ = false;
    conversationTrack_.clear();
    uiResource_.clear();
    refreshMusic(false);
}

// ---- ambient loop -----------------------------------------------------------------------

float AreaAmbience::ambientLevel() const
{
    const kotor::AreaAudio& area = owner_.areaAudio_;
    const int volume = std::clamp(isDay() ? area.ambientDayVolume : area.ambientNightVolume, 0, 127);
    // 0068ea10 plays the loop in group 4 (Looping_AreaWide, volume 95).
    return static_cast<float>(owner_.priorityGroup(AudioSystem::kGroupLoopAreaWide).volume) / 127.0f
        * static_cast<float>(volume) / 127.0f * roomScale_;
}

void AreaAmbience::applyAmbientVolume()
{
    if (ambientVoice_ != 0 && owner_.mixer_.voiceActive(ambientVoice_)) {
        owner_.mixer_.setVoiceVolume(ambientVoice_, ambientLevel());
    }
}

void AreaAmbience::refreshAmbient(bool immediate)
{
    audio::Mixer& mixer = owner_.mixer_;
    const kotor::AreaAudio& area = owner_.areaAudio_;
    std::string resource;
    if (ambientOn_) {
        const std::int32_t row = forcedAmbientRow_ >= 0
            ? forcedAmbientRow_
            : (isDay() ? area.ambientDayId : area.ambientNightId);
        resource = owner_.tableResource("ambientsound", row);
        if (!realName(resource)) {
            resource.clear();
        }
    }
    const bool alive = ambientVoice_ != 0 && mixer.voiceActive(ambientVoice_);
    if (alive && resource == ambientResource_) {
        applyAmbientVolume();
        return;
    }
    if (alive) {
        if (immediate) {
            mixer.stopVoice(ambientVoice_);
        } else {
            mixer.fadeVoice(ambientVoice_, 0.0f, kAmbientFadeOut, true);
        }
    }
    ambientVoice_ = 0;
    ambientResource_.clear();
    if (resource.empty()) {
        return;
    }
    std::string error;
    const std::shared_ptr<const audio::SoundData> data = owner_.loadSound(resource, error);
    if (!data) {
        return;
    }
    const int voice = mixer.play(data, audio::Bus::Ambient, ambientLevel(), true, false);
    if (voice != 0 && alive && !immediate) {
        mixer.setVoiceFade(voice, 0.0f);
        mixer.fadeVoice(voice, 1.0f, kAmbientFadeIn, false);
    }
    ambientVoice_ = voice;
    ambientResource_ = resource;
}

bool AreaAmbience::forceAmbientRow(std::int32_t row)
{
    const std::string resource = owner_.tableResource("ambientsound", row);
    if (!realName(resource)) {
        return false;
    }
    forcedAmbientRow_ = row;
    ambientOn_ = true;
    refreshAmbient(false);
    return true;
}

void AreaAmbience::setAmbientEnabled(bool on)
{
    ambientOn_ = on;
    if (on) {
        forcedAmbientRow_ = -1;
    }
    refreshAmbient(false);
}

void AreaAmbience::changeAmbientTrack(bool night, std::int32_t row)
{
    kotor::AreaAudio& area = owner_.areaAudio_;
    const std::string resource = owner_.tableResource("ambientsound", row);
    if (night) {
        area.ambientNightId = row;
        area.ambientNight = resource;
    } else {
        area.ambientDayId = row;
        area.ambientDay = resource;
    }
    forcedAmbientRow_ = -1;
    refreshAmbient(false);
}

void AreaAmbience::setAmbientVolume(bool night, std::int32_t volume127)
{
    kotor::AreaAudio& area = owner_.areaAudio_;
    (night ? area.ambientNightVolume : area.ambientDayVolume) = std::clamp(volume127, 0, 127);
    applyAmbientVolume();
}

// ---- sound objects -----------------------------------------------------------------------

float AreaAmbience::distanceTo(const kotor::Vec3& at) const
{
    const float dx = at.x - listener_.x;
    const float dy = at.y - listener_.y;
    const float dz = at.z - listener_.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool AreaAmbience::timesAllow(const Emitter& emitter) const
{
    if (emitter.def.times == 0) {
        return ((emitter.def.hours >> static_cast<std::uint32_t>(std::clamp(hour_, 0, 31))) & 1u) != 0;
    }
    return isDay() ? emitter.def.times != 2 : emitter.def.times != 1;
}

bool AreaAmbience::voiceLive(const Emitter& emitter) const
{
    return emitter.voice != 0 && owner_.mixer_.voiceActive(emitter.voice);
}

std::size_t AreaAmbience::activeSoundVoices() const
{
    std::size_t count = 0;
    for (const Emitter& emitter : emitters_) {
        if (emitter.def.positional && voiceLive(emitter)) {
            ++count;
        }
    }
    return count;
}

void AreaAmbience::shuffle(Emitter& emitter)
{
    // Retail 0066b980 draws random entries without replacement into a new list.
    std::vector<int> pool = emitter.order;
    emitter.order.clear();
    while (!pool.empty()) {
        const std::size_t pick = static_cast<std::size_t>(
            rng().below(static_cast<int>(pool.size())));
        emitter.order.push_back(pool[pick]);
        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(pick));
    }
}

bool AreaAmbience::startEmitterSound(Emitter& emitter)
{
    const kotor::SoundEmitter& def = emitter.def;
    audio::Mixer& mixer = owner_.mixer_;
    const bool resume = emitter.needsVoice;
    const int count = static_cast<int>(def.sounds.size());
    const int slot = emitter.order[static_cast<std::size_t>(std::clamp(emitter.index, 0, count - 1))];
    std::string error;
    const std::shared_ptr<const audio::SoundData> data =
        owner_.loadSound(def.sounds[static_cast<std::size_t>(slot)], error);
    if (!data) {
        // Retail drops an unplayable sound from the list (0066bec3); here the object
        // simply tries the next entry after a second.
        emitter.voice = 0;
        emitter.needsVoice = false;
        emitter.intervalMs = 1000.0;
        return false;
    }

    int volume = def.volume127;
    if (def.volumeVariation > 0) {
        volume += rng().below(2 * def.volumeVariation) - def.volumeVariation;
    }
    volume = std::clamp(volume, 0, 127);

    float pitch = emitter.fixedClient;
    const float variation = std::clamp(def.pitchVariation, 0.0f, 1.0f);
    if (variation > 0.0f) {
        const float amount = rng().uniform() * variation;
        pitch *= rng().below(2) == 0 ? 1.0f + amount : 1.0f - amount;
    }

    kotor::Vec3 at = def.position;
    if (def.randomPosition) {
        // INFERRED: RandomRangeX/Y is the full width of a box centred on the object.
        at.x += (rng().uniform() - 0.5f) * def.randomRangeX;
        at.y += (rng().uniform() - 0.5f) * def.randomRangeY;
    }

    auto scheduleInterval = [&]() {
        if (resume) {
            return;
        }
        if (def.continuous || emitter.index < count - 1) {
            double interval = def.interval;
            if (def.interval != 0 && def.intervalVariation > 0) {
                interval += rng().below(2 * def.intervalVariation) - def.intervalVariation;
            }
            emitter.intervalMs = std::max(interval, 0.0);
        } else {
            emitter.intervalMs = 0.0;
        }
    };

    if (def.positional && activeSoundVoices() >= static_cast<std::size_t>(kMax3dVoices)) {
        // Over the 3D voice budget: retail 005d8eb0 steals the playing 3D sound whose priority group has the
        // largest Priority number, and only when that number is greater than the new sound's.
        const float mine = distanceTo(at);
        const auto priorityOf = [&](const kotor::SoundEmitter& def) {
            const int group = def.looping
                ? (def.positional ? AudioSystem::kGroupLoopPositional : AudioSystem::kGroupLoopAreaWide)
                : (def.positional ? AudioSystem::kGroupSinglePositional : AudioSystem::kGroupSingleGlobal);
            return owner_.priorityGroup(group).priority;
        };
        const int newPriority = priorityOf(def);
        Emitter* farthest = nullptr;
        int worstPriority = newPriority;
        for (Emitter& other : emitters_) {
            if (&other != &emitter && other.def.positional && voiceLive(other)) {
                const int priority = priorityOf(other.def);
                if (priority > worstPriority) {
                    worstPriority = priority;
                    farthest = &other;
                }
            }
        }
        if (farthest != nullptr) {
            mixer.stopVoice(farthest->voice);
            farthest->voice = 0;
            farthest->needsVoice = true;
            farthest->retryMs = kRetryMs;
        } else if (mine >= def.maxDistance && !def.looping) {
            // Inaudible anyway: the play counts as played, silently.
            emitter.voice = 0;
            emitter.needsVoice = false;
            scheduleInterval();
            return true;
        } else {
            emitter.needsVoice = true;
            emitter.retryMs = kRetryMs;
            return false;
        }
    }

    // 0066b420 picks the priority group from the Looping and Positional flags; looping sounds
    // stream (linear group/127 * volume/127, 005dc930), one-shots are samples (005db800).
    const int group = def.looping
        ? (def.positional ? AudioSystem::kGroupLoopPositional : AudioSystem::kGroupLoopAreaWide)
        : (def.positional ? AudioSystem::kGroupSinglePositional : AudioSystem::kGroupSingleGlobal);
    const float level = owner_.voiceLevel(group, volume, def.looping, def.positional);
    emitter.voice = mixer.play(data, audio::Bus::Effects, level,
                               def.looping, def.positional, at, def.minDistance,
                               def.maxDistance, 0.0f, pitch);
    if (emitter.voice != 0 && def.looping && def.positional) {
        mixer.setVoiceStream3D(emitter.voice, true); // a looping positional sound is a stream (0066b420)
    }
    emitter.lastPitch = pitch;
    emitter.lastPosition = at;
    emitter.lastVolume127 = volume;
    emitter.needsVoice = false;
    scheduleInterval();
    return true;
}

void AreaAmbience::updateEmitter(Emitter& emitter, float dt)
{
    audio::Mixer& mixer = owner_.mixer_;
    const bool live = voiceLive(emitter);
    if (emitter.stopping) {
        if (!live) {
            emitter.stopping = false;
            emitter.voice = 0;
        }
        return;
    }
    const bool allowed = timesAllow(emitter);
    if (!emitter.active || !allowed || emitter.def.sounds.empty()) {
        if (live) {
            mixer.stopVoice(emitter.voice);
        }
        emitter.voice = 0;
        if (!allowed && emitter.active) {
            // The hours window closed: the next window starts from the first sound.
            emitter.first = true;
            emitter.intervalMs = 0.0;
            emitter.needsVoice = false;
        }
        return;
    }
    if (live) {
        return;
    }
    emitter.voice = 0;
    if (emitter.retryMs > 0.0) {
        emitter.retryMs -= static_cast<double>(dt) * 1000.0;
        if (emitter.retryMs > 0.0) {
            return;
        }
        emitter.retryMs = 0.0;
    }
    if (emitter.needsVoice) {
        startEmitterSound(emitter);
        return;
    }
    if (emitter.intervalMs > 0.0) {
        emitter.intervalMs -= static_cast<double>(dt) * 1000.0;
        if (emitter.intervalMs > 0.0) {
            return;
        }
        emitter.intervalMs = 0.0;
    }
    const int count = static_cast<int>(emitter.def.sounds.size());
    if (emitter.first) {
        emitter.first = false;
        emitter.index = 0;
    } else if (emitter.index < count - 1) {
        ++emitter.index;
    } else if (emitter.def.continuous) {
        if (emitter.def.random) {
            shuffle(emitter);
        }
        emitter.index = 0;
    } else {
        emitter.active = false;
        return;
    }
    startEmitterSound(emitter);
}

std::size_t AreaAmbience::emitterForEntity(std::uint32_t entity) const
{
    for (std::size_t i = 0; i < emitters_.size(); ++i) {
        if (emitters_[i].def.entity == entity) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

AreaAmbience::SoundObjectInfo AreaAmbience::soundObject(std::size_t index) const
{
    SoundObjectInfo info;
    if (index >= emitters_.size()) {
        return info;
    }
    const Emitter& emitter = emitters_[index];
    info.found = true;
    info.active = emitter.active;
    info.playing = voiceLive(emitter);
    info.voice = emitter.voice;
    info.index = emitter.index;
    if (!emitter.def.sounds.empty() && !emitter.order.empty()) {
        info.sound = emitter.def.sounds[static_cast<std::size_t>(
            emitter.order[static_cast<std::size_t>(
                std::clamp(emitter.index, 0, static_cast<int>(emitter.order.size()) - 1))])];
    }
    info.intervalLeftMs = emitter.intervalMs;
    info.volume127 = static_cast<float>(emitter.lastVolume127);
    info.lastPitch = emitter.lastPitch;
    info.lastPosition = emitter.lastPosition;
    return info;
}

bool AreaAmbience::playSoundObject(std::uint32_t entity)
{
    const std::size_t index = emitterForEntity(entity);
    if (index == static_cast<std::size_t>(-1) || emitters_[index].def.sounds.empty()) {
        return false;
    }
    Emitter& emitter = emitters_[index];
    // Retail 005c8bd0 sets Active and tells the clients, which restart from the
    // first sound (0066b050).
    if (voiceLive(emitter)) {
        owner_.mixer_.stopVoice(emitter.voice);
    }
    emitter.voice = 0;
    emitter.active = true;
    emitter.stopping = false;
    emitter.first = true;
    emitter.intervalMs = 0.0;
    emitter.retryMs = 0.0;
    emitter.needsVoice = false;
    updateEmitter(emitter, 0.0f);
    return true;
}

bool AreaAmbience::stopSoundObject(std::uint32_t entity)
{
    const std::size_t index = emitterForEntity(entity);
    if (index == static_cast<std::size_t>(-1)) {
        return false;
    }
    Emitter& emitter = emitters_[index];
    if (voiceLive(emitter)) {
        owner_.mixer_.stopVoice(emitter.voice);
    }
    emitter.voice = 0;
    emitter.active = false;
    emitter.stopping = false;
    emitter.first = true;
    emitter.intervalMs = 0.0;
    emitter.needsVoice = false;
    return true;
}

bool AreaAmbience::fadeAndStopSoundObject(std::uint32_t entity, float seconds)
{
    const std::size_t index = emitterForEntity(entity);
    if (index == static_cast<std::size_t>(-1)) {
        return false;
    }
    Emitter& emitter = emitters_[index];
    emitter.active = false;
    emitter.first = true;
    emitter.intervalMs = 0.0;
    emitter.needsVoice = false;
    if (voiceLive(emitter)) {
        owner_.mixer_.fadeVoice(emitter.voice, 0.0f, std::max(seconds, 0.0f), true);
        emitter.stopping = voiceLive(emitter);
    }
    return true;
}

bool AreaAmbience::setSoundObjectVolume127(std::uint32_t entity, int volume)
{
    const std::size_t index = emitterForEntity(entity);
    if (index == static_cast<std::size_t>(-1)) {
        return false;
    }
    Emitter& emitter = emitters_[index];
    emitter.def.volume127 = std::clamp(volume, 0, 127);   // retail 005c8df0
    emitter.def.volume = static_cast<float>(emitter.def.volume127) / 127.0f;
    if (voiceLive(emitter)) {
        const int group = emitter.def.looping
            ? (emitter.def.positional ? AudioSystem::kGroupLoopPositional
                                      : AudioSystem::kGroupLoopAreaWide)
            : (emitter.def.positional ? AudioSystem::kGroupSinglePositional
                                      : AudioSystem::kGroupSingleGlobal);
        owner_.mixer_.setVoiceVolume(emitter.voice,
                                     owner_.voiceLevel(group, emitter.def.volume127,
                                                       emitter.def.looping, emitter.def.positional));
        emitter.lastVolume127 = emitter.def.volume127;
    }
    return true;
}

int AreaAmbience::soundObjectVolume127(std::uint32_t entity) const
{
    const std::size_t index = emitterForEntity(entity);
    return index == static_cast<std::size_t>(-1) ? 0 : emitters_[index].def.volume127;
}

bool AreaAmbience::setSoundObjectPosition(std::uint32_t entity, kotor::Vec3 position)
{
    const std::size_t index = emitterForEntity(entity);
    if (index == static_cast<std::size_t>(-1)) {
        return false;
    }
    Emitter& emitter = emitters_[index];
    emitter.def.position = position;
    emitter.lastPosition = position;
    if (voiceLive(emitter) && emitter.def.positional) {
        owner_.mixer_.setVoicePosition(emitter.voice, position);
    }
    return true;
}

bool AreaAmbience::setSoundObjectPitchVariance(std::uint32_t entity, float variance)
{
    const std::size_t index = emitterForEntity(entity);
    if (index == static_cast<std::size_t>(-1)) {
        return false;
    }
    emitters_[index].def.pitchVariation = variance;   // read back raw; clamped at play time
    return true;
}

float AreaAmbience::soundObjectPitchVariance(std::uint32_t entity) const
{
    const std::size_t index = emitterForEntity(entity);
    return index == static_cast<std::size_t>(-1) ? 0.0f : emitters_[index].def.pitchVariation;
}

bool AreaAmbience::setSoundObjectFixedVariance(std::uint32_t entity, float variance)
{
    const std::size_t index = emitterForEntity(entity);
    if (index == static_cast<std::size_t>(-1)) {
        return false;
    }
    Emitter& emitter = emitters_[index];
    emitter.def.fixedVariance = variance;
    if (variance >= 0.5f && variance <= 2.0f) {   // retail 005db980 refuses the rest
        const float previous = emitter.fixedClient;
        emitter.fixedClient = variance;
        if (voiceLive(emitter)) {
            emitter.lastPitch = emitter.lastPitch / previous * variance;
            owner_.mixer_.setVoicePitch(emitter.voice, emitter.lastPitch);
        }
    }
    return true;
}

bool AreaAmbience::setSoundObjectTimes(std::uint32_t entity, int times, std::uint32_t hours)
{
    const std::size_t index = emitterForEntity(entity);
    if (index == static_cast<std::size_t>(-1)) {
        return false;
    }
    emitters_[index].def.times = times;
    emitters_[index].def.hours = hours;
    return true;
}

float AreaAmbience::soundObjectFixedVariance(std::uint32_t entity) const
{
    const std::size_t index = emitterForEntity(entity);
    return index == static_cast<std::size_t>(-1) ? 1.0f : emitters_[index].def.fixedVariance;
}

} // namespace ke
