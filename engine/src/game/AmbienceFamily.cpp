// Area audio mix: the gate behind --family-test ambience.
//
// Every assertion is a measurement on the Mixer (voice counts per bus, voice gain and pan,
// start and end ticks of sound-object plays under a fixed dt, and the per-bus peak/RMS of
// captured Mixer::mix buffers) on shipped modules. Nothing here has been listened to: a
// pass proves the mix is built and shaped as asserted, not that it sounds right.
//
// Evidence for the rules (retail addresses) is in game/Ambience.cpp; rules marked INFERRED
// there are asserted here only as the engine's own behaviour.
#include "game/Ambience.h"

#include "audio/Audio.h"
#include "core/Log.h"
#include "game/AudioSystem.h"
#include "game/GameSession.h"
#include "game/ScriptHost.h"
#include "kotor/Formats.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace ke {

namespace {

using audio::Bus;
using Args = std::vector<std::vector<script::Value>>;

constexpr int kBlock = 480;          // 10 ms at 48 kHz
constexpr float kDt = 0.01f;         // the fixed audio tick
constexpr int kBuses = 5;

std::string fx(double value, int digits = 4)
{
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(digits);
    stream << value;
    return stream.str();
}

std::vector<script::Value> num(std::int64_t v) { return {script::Value::makeInt(v)}; }
std::vector<script::Value> real(double v) { return {script::Value::makeFloat(v)}; }
std::vector<script::Value> obj(std::uint32_t v) { return {script::Value::makeObject(v)}; }

std::string lowered(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

struct Gate {
    bool ok = true;
    bool quiet = false;
    int checks = 0;
    int failures = 0;

    void check(bool condition, const std::string& label)
    {
        ++checks;
        if (!quiet) {
            logInfo(concat("ambience: ", label, "=", condition ? 1 : 0));
        }
        if (!condition) {
            ok = false;
            ++failures;
        }
    }
    // The claim describes the OLD or a WRONG behaviour; the gate passes only when the
    // measurement refutes it.
    void refute(bool wrongClaim, const std::string& label)
    {
        check(!wrongClaim, "negative_" + label);
    }
    void note(const std::string& text) const
    {
        if (!quiet) {
            logInfo(concat("ambience: ", text));
        }
    }
};

// ---- measuring ---------------------------------------------------------------------------

struct Meter {
    float peak[kBuses] = {};
    float minRms[kBuses] = {};
    double sumSq[kBuses] = {};
    double masterSq = 0.0;
    double leftSq = 0.0;
    double rightSq = 0.0;
    double samples = 0.0;
    int blocks = 0;

    Meter()
    {
        for (float& value : minRms) {
            value = 1.0e9f;
        }
    }
    void add(const audio::Mixer& mixer, const std::vector<float>& buffer)
    {
        for (int bus = 0; bus < kBuses; ++bus) {
            const float rms = mixer.busRms(static_cast<Bus>(bus));
            sumSq[bus] += static_cast<double>(rms) * rms;
            peak[bus] = std::max(peak[bus], mixer.busPeak(static_cast<Bus>(bus)));
            minRms[bus] = std::min(minRms[bus], rms);
        }
        for (std::size_t i = 0; i + 1 < buffer.size(); i += 2) {
            leftSq += static_cast<double>(buffer[i]) * buffer[i];
            rightSq += static_cast<double>(buffer[i + 1]) * buffer[i + 1];
        }
        masterSq = leftSq + rightSq;
        samples += static_cast<double>(buffer.size());
        ++blocks;
    }
    float rms(Bus bus) const
    {
        return blocks > 0 ? static_cast<float>(std::sqrt(sumSq[static_cast<int>(bus)] / blocks)) : 0.0f;
    }
    float pk(Bus bus) const { return peak[static_cast<int>(bus)]; }
    float masterRms() const
    {
        return samples > 0.0 ? static_cast<float>(std::sqrt(masterSq / samples)) : 0.0f;
    }
    float leftRms() const
    {
        return samples > 0.0 ? static_cast<float>(std::sqrt(leftSq / (samples * 0.5))) : 0.0f;
    }
    float rightRms() const
    {
        return samples > 0.0 ? static_cast<float>(std::sqrt(rightSq / (samples * 0.5))) : 0.0f;
    }
};

void mixBlock(audio::Mixer& mixer, int frames, Meter* meter)
{
    std::vector<float> buffer(static_cast<std::size_t>(frames) * 2u, 0.0f);
    mixer.mix(buffer.data(), frames);
    if (meter != nullptr) {
        meter->add(mixer, buffer);
    }
}

// Audio-only ticks: the ambience update at a fixed dt, then one 10 ms mix block.
void audioTicks(AudioSystem& audio, int ticks, Meter* meter = nullptr)
{
    for (int i = 0; i < ticks; ++i) {
        audio.update(kDt);
        mixBlock(audio.mixer(), kBlock, meter);
    }
}

// Whole-session ticks (the real per-frame path: clock, combat state, listener).
void gameTicks(GameSession& game, int ticks, Meter* meter = nullptr)
{
    for (int i = 0; i < ticks; ++i) {
        game.update(1.0f / 30.0f, InputState{});
        mixBlock(game.audio().mixer(), 1600, meter);
    }
}

std::shared_ptr<const audio::SoundData> loadShared(AudioSystem& audio, const std::string& name)
{
    audio::SoundData data;
    std::string error;
    if (!audio.decodeProbe(name, data, error)) {
        return nullptr;
    }
    audio::resample(data, 48000, 2);
    return std::make_shared<const audio::SoundData>(std::move(data));
}

struct Pool {
    std::vector<std::string> names;       // loud, 0.5 .. 6 s
    std::vector<double> durations;
    std::size_t shortest = 0;
};

Pool buildPool(GameSession& game)
{
    Pool pool;
    std::set<std::string> seen;
    AudioSystem& audio = game.audio();
    for (const kotor::SoundEmitter& emitter : game.scene().soundEmitters) {
        for (const std::string& name : emitter.sounds) {
            if (pool.names.size() >= 8 || !seen.insert(lowered(name)).second) {
                continue;
            }
            audio::SoundData data;
            std::string error;
            if (!audio.decodeProbe(name, data, error) || data.sampleRate <= 0 || data.channels <= 0) {
                continue;
            }
            const double duration = data.durationSeconds();
            const std::size_t window = std::min(
                data.samples.size(),
                static_cast<std::size_t>(data.sampleRate) * static_cast<std::size_t>(data.channels) / 4u);
            double sum = 0.0;
            for (std::size_t i = 0; i < window; ++i) {
                sum += static_cast<double>(data.samples[i]) * data.samples[i];
            }
            const double rms = window > 0 ? std::sqrt(sum / static_cast<double>(window)) : 0.0;
            if (duration >= 0.5 && duration <= 6.0 && rms > 0.01) {
                pool.names.push_back(name);
                pool.durations.push_back(duration);
            }
        }
    }
    for (std::size_t i = 1; i < pool.durations.size(); ++i) {
        if (pool.durations[i] < pool.durations[pool.shortest]) {
            pool.shortest = i;
        }
    }
    return pool;
}

kotor::SoundEmitter fixtureEmitter(std::uint32_t entity, std::vector<std::string> sounds)
{
    kotor::SoundEmitter e;
    e.entity = entity;
    e.templateResref = "ambience_fixture";
    e.sounds = std::move(sounds);
    e.position = {0.0f, 0.0f, 0.0f};
    e.minDistance = 2.0f;
    e.maxDistance = 60.0f;
    e.volume127 = 127;
    e.volume = 1.0f;
    e.active = true;
    e.continuous = true;
    e.looping = false;
    e.positional = true;
    e.random = false;
    e.interval = 0;
    e.intervalVariation = 0;
    e.times = 3;
    e.pitchVariation = 0.0f;
    e.fixedVariance = 1.0f;
    return e;
}

// A silent area (no music, no ambient loop) holding only `emitters`, reverb off.
void loadFixture(AudioSystem& audio, const std::vector<kotor::SoundEmitter>& emitters,
                 kotor::Vec3 listener, float yaw = 0.0f, std::uint64_t seed = 12345)
{
    kotor::Scene fixture;
    fixture.module = "ambience_fixture";
    fixture.soundEmitters = emitters;
    std::string error;
    audio.loadScene(fixture, error);
    audio.mixer().setReverbEnvironment(-1, false);
    audio.ambience().setSeed(seed);
    audio.setTimeOfDay(1, 12);
    audio.setListener(listener, yaw);
    mixBlock(audio.mixer(), kBlock, nullptr);   // applies the queued reverb change
}

struct PlayRecord {
    int tick = 0;
    int endTick = -1;
    int voice = 0;
    std::string sound;
    float pitch = 1.0f;
    float measuredPitch = 1.0f;
    float voiceVolume = 0.0f;
    int volume127 = 0;
    kotor::Vec3 position;
};

// Runs update + mix ticks and records every play of emitter `index`. Stops once `want`
// plays have started and the last one ended, or after `maxTicks`.
std::vector<PlayRecord> collectPlays(AudioSystem& audio, std::size_t want, int maxTicks,
                                     std::size_t index = 0)
{
    std::vector<PlayRecord> plays;
    int lastVoice = 0;
    int open = -1;
    audio::Mixer& mixer = audio.mixer();
    for (int tick = 0; tick < maxTicks; ++tick) {
        audio.update(kDt);
        const AreaAmbience::SoundObjectInfo info = audio.ambience().soundObject(index);
        bool started = false;
        if (info.voice != 0 && info.voice != lastVoice && mixer.voiceActive(info.voice)) {
            PlayRecord record;
            record.tick = tick;
            record.voice = info.voice;
            record.sound = info.sound;
            record.pitch = info.lastPitch;
            record.voiceVolume = mixer.voiceVolume(info.voice);
            record.volume127 = static_cast<int>(info.volume127);
            record.position = info.lastPosition;
            plays.push_back(record);
            open = static_cast<int>(plays.size()) - 1;
            lastVoice = info.voice;
            started = true;
        }
        const double cursorBefore = started ? mixer.voiceCursor(plays.back().voice) : 0.0;
        mixBlock(mixer, kBlock, nullptr);
        if (started) {
            const double cursor = mixer.voiceCursor(plays.back().voice);
            plays.back().measuredPitch = cursor >= 0.0 && cursorBefore >= 0.0
                ? static_cast<float>((cursor - cursorBefore) * mixer.sampleRate() / kBlock) : 0.0f;
        }
        if (open >= 0 && !mixer.voiceActive(plays[static_cast<std::size_t>(open)].voice)) {
            plays[static_cast<std::size_t>(open)].endTick = tick;
            open = -1;
        }
        if (open < 0 && plays.size() >= want) {
            break;
        }
    }
    return plays;
}

std::vector<int> gapsTicks(const std::vector<PlayRecord>& plays)
{
    std::vector<int> gaps;
    for (std::size_t i = 0; i + 1 < plays.size(); ++i) {
        if (plays[i].endTick >= 0) {
            gaps.push_back(plays[i + 1].tick - plays[i].endTick);
        }
    }
    return gaps;
}

// ---- helpers over the session -------------------------------------------------------------

std::optional<std::vector<script::Value>> act(GameSession& game, std::uint16_t id, const Args& args)
{
    return game.actionAsForTest(game.scriptWorld().pc, id, args);
}

void setClock(GameSession& game, int hour)
{
    act(game, 12, {num(hour), num(0), num(0), num(0)});
}

bool switchModule(GameSession& game, const std::string& module)
{
    // The module loader the minigame entry uses: a plain load with the default spawn.
    const bool loaded = game.enterMiniGameModule(module);
    gameTicks(game, 6);
    return loaded && game.module() == module;
}

// The 2DA cell the engine should resolve for a music row, read straight from the install.
struct MusicRows {
    std::optional<kotor::TwoDa> table;
    std::string cell(int row, const std::string& column) const
    {
        const std::string key = std::to_string(row);
        if (!table.has_value() || table->rowIndex(key) < 0) {
            return {};
        }
        return table->cell(key, column);
    }
};

MusicRows readMusicRows(GameSession& game)
{
    MusicRows rows;
    auto bytes = game.install().find("ambientmusic", kotor::resType(kotor::ResType::TwoDa));
    if (bytes.has_value()) {
        rows.table = kotor::TwoDa::parse(std::move(*bytes), "ambientmusic");
    }
    return rows;
}

std::size_t voicesOn(AudioSystem& audio, Bus bus) { return audio.mixer().activeVoices(bus); }

// Music-bus voices that are not the battle stinger (it plays on its own for a few seconds).
std::size_t trackVoices(AudioSystem& audio)
{
    const int stinger = audio.ambience().stingerVoice();
    const std::size_t all = voicesOn(audio, Bus::Music);
    return stinger != 0 && audio.mixer().voiceActive(stinger) ? all - 1 : all;
}

// ---- sections ------------------------------------------------------------------------------

// End of a test run on shipped end_m01aa: the world at load.
void sectionLoad(Gate& g, GameSession& game, float& masterFactor)
{
    AudioSystem& audio = game.audio();
    audio::Mixer& mixer = audio.mixer();
    AreaAmbience& amb = audio.ambience();
    const kotor::Scene& scene = game.scene();

    mixer.stopAll();
    std::string error;
    audio.loadScene(scene, error);
    audio.setTimeOfDay(1, 12);
    amb.setSeed(7);

    std::size_t inactive = 0;
    std::size_t positionalActive = 0;
    std::size_t globalActive = 0;
    std::size_t sampleActive = 0;
    std::size_t withoutSounds = 0;
    for (const kotor::SoundEmitter& e : scene.soundEmitters) {
        if (e.sounds.empty()) {
            ++withoutSounds;
        }
        if (!e.active || e.sounds.empty()) {
            ++inactive;
        } else if (e.positional) {
            ++positionalActive;
            // Looping positional sounds are streams (0066b420): they never take one of the 16 Miles 3D voices.
            sampleActive += e.looping ? 0u : 1u;
        } else {
            ++globalActive;
        }
    }
    g.note(concat("end_m01aa emitters=", scene.soundEmitters.size(), " inactive=", inactive,
                  " positional_active=", positionalActive, " global_active=", globalActive,
                  " without_sounds=", withoutSounds));
    g.check(scene.soundEmitters.size() == 45, "load_emitters_45");
    g.check(inactive == 4, "load_inactive_4");
    g.check(amb.soundObjectCount() == scene.soundEmitters.size(), "load_runtime_has_every_emitter");

    // Before the first listener position nothing of the area has started its emitters.
    const std::size_t before = voicesOn(audio, Bus::Effects);
    g.note(concat("effects_voices_before_listener=", before));
    g.check(before == 0, "load_effects_voices_before_listener_0");
    g.check(voicesOn(audio, Bus::Music) == 1, "load_music_voices_1");
    g.check(voicesOn(audio, Bus::Ambient) == 1, "load_ambient_voices_1");

    const kotor::Vec3 at = game.actorPosition(game.scriptWorld().pc);
    audio.setListener(at, 0.0f);
    const std::size_t after = voicesOn(audio, Bus::Effects);
    std::size_t positionalPlaying = 0;
    std::size_t samplePlaying = 0;
    std::size_t globalPlaying = 0;
    std::size_t inactivePlaying = 0;
    std::size_t audible = 0;
    bool gainsMatch = true;
    for (std::size_t i = 0; i < scene.soundEmitters.size(); ++i) {
        const kotor::SoundEmitter& e = scene.soundEmitters[i];
        const AreaAmbience::SoundObjectInfo info = amb.soundObject(i);
        if (!info.playing) {
            continue;
        }
        if (!e.active || e.sounds.empty()) {
            ++inactivePlaying;
        }
        if (e.positional) {
            ++positionalPlaying;
            samplePlaying += e.looping ? 0u : 1u;
            const float dx = info.lastPosition.x - at.x;
            const float dy = info.lastPosition.y - at.y;
            const float dz = info.lastPosition.z - at.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float span = std::max(e.maxDistance, e.minDistance + 0.01f) - e.minDistance;
            const float atten = std::clamp((std::max(e.maxDistance, e.minDistance + 0.01f) - distance)
                                               / span, 0.0f, 1.0f);
            // A looping positional sound is a stream with retail's own falloff (005dc560, linear in
            // the rendered amplitude); the rest keep the squared ramp. The listener's yaw is 0, so a
            // source at -x of it is behind.
            float expected = info.volume127 / 127.0f * atten * atten;
            if (e.looping) {
                float f = distance > e.minDistance ? atten : 1.0f;
                if (distance > 1.0e-4f && dx * 1.0f / distance < 0.0f) {
                    f *= 1.0f + 0.125f * (dx / distance);
                }
                expected = mixer.voiceVolume(info.voice) * std::max(f, 0.0f);
            }
            if (std::fabs(mixer.voiceGain(info.voice) - expected) > 1.0e-4f) {
                gainsMatch = false;
            }
            audible += expected > 0.0f ? 1u : 0u;
        } else {
            ++globalPlaying;
        }
    }
    g.note(concat("effects_voices_after_listener=", after, " positional_playing=",
                  positionalPlaying, " global_playing=", globalPlaying, " audible=", audible));
    g.check(after > 0, "load_effects_voices_after_listener_gt_0");
    g.check(globalPlaying == globalActive, "load_every_active_global_emitter_plays");
    g.check(samplePlaying == std::min<std::size_t>(sampleActive, AreaAmbience::kMax3dVoices),
            "load_positional_voices_capped_at_16");
    g.check(amb.activeSoundVoices() == samplePlaying, "load_budget_counter_matches");
    g.check(after == positionalPlaying + globalPlaying, "load_effects_voices_are_the_emitters");
    g.check(inactivePlaying == 0, "load_inactive_emitters_silent");
    g.check(gainsMatch, "load_positional_gain_matches_distance_formula");
    // The old engine left Effects at 0 here (emitters were silent); the measured value is
    // what the refutation below is about.
    g.refute(after == 0, "old_area_emitters_silent_at_load");

    // The load movie is still on at this point. Retail pauses the world sounds for a movie (sound mode 3,
    // 005d82c0) instead of muting them, so the bus flag the isolation below sets no longer ends it.
    audio.setMovieMode(false);

    // Bus isolation against the master buffer: Music alone, then Ambient alone.
    float factor[2] = {0.0f, 0.0f};
    for (int pass = 0; pass < 2; ++pass) {
        for (const Bus bus : {Bus::Music, Bus::Ambient, Bus::Effects, Bus::Voice}) {
            mixer.setBusMuted(bus, !(pass == 0 ? bus == Bus::Music : bus == Bus::Ambient));
        }
        Meter isolated;
        audioTicks(audio, 5, &isolated);
        const Bus bus = pass == 0 ? Bus::Music : Bus::Ambient;
        factor[pass] = isolated.rms(bus) > 0.0f ? isolated.masterRms() / isolated.rms(bus) : 0.0f;
        g.note(concat("isolated_", pass == 0 ? "music" : "ambient", " bus_rms=",
                      fx(isolated.rms(bus), 5), " master_rms=", fx(isolated.masterRms(), 5),
                      " ratio=", fx(factor[pass], 4)));
        g.check(isolated.rms(bus) > 0.0f, std::string("load_isolated_") + (pass == 0 ? "music" : "ambient")
                                              + "_bus_rms_gt_0");
    }
    for (const Bus bus : {Bus::Music, Bus::Ambient, Bus::Effects, Bus::Voice}) {
        mixer.setBusMuted(bus, false);
    }
    masterFactor = factor[0];
    g.check(factor[0] > 0.5f && factor[0] <= 1.0f && std::fabs(factor[0] - factor[1]) < 0.01f,
            "load_bus_rms_matches_captured_buffer_through_one_master_gain");

    // Captured mix of the first two seconds, per bus.
    Meter meter;
    audioTicks(audio, 200, &meter);
    g.note(concat("mix2s peak music=", fx(meter.pk(Bus::Music)), " ambient=", fx(meter.pk(Bus::Ambient)),
                  " effects=", fx(meter.pk(Bus::Effects)), " voice=", fx(meter.pk(Bus::Voice)),
                  " movie=", fx(meter.pk(Bus::Movie))));
    g.note(concat("mix2s rms music=", fx(meter.rms(Bus::Music), 5), " ambient=",
                  fx(meter.rms(Bus::Ambient), 5), " effects=", fx(meter.rms(Bus::Effects), 5),
                  " voice=", fx(meter.rms(Bus::Voice), 5), " movie=", fx(meter.rms(Bus::Movie), 5)));
    g.check(meter.pk(Bus::Music) > 0.0f && meter.rms(Bus::Music) > 0.0f, "mix_music_bus_nonzero");
    g.check(meter.pk(Bus::Ambient) > 0.0f && meter.rms(Bus::Ambient) > 0.0f, "mix_ambient_bus_nonzero");
    g.check(meter.pk(Bus::Effects) > 0.0f && meter.rms(Bus::Effects) > 0.0f, "mix_effects_bus_nonzero");
    g.check(meter.pk(Bus::Voice) == 0.0f && meter.pk(Bus::Movie) == 0.0f, "mix_voice_and_movie_buses_silent");
    g.check(meter.masterRms() > 0.0f, "mix_master_buffer_nonzero");
}

// The user volumes come from swkotor.ini; the ambient bus follows Sound Effects Volume.
void sectionUserVolumes(Gate& g, GameSession& game)
{
    AudioSystem& audio = game.audio();
    const AudioSystem::UserVolumes& v = audio.userVolumes();
    g.note(concat("user volumes music=", fx(v.music, 2), " voice=", fx(v.voice, 2), " effects=",
                  fx(v.effects, 2), " movie=", fx(v.movie, 2)));
    // swkotor.ini [Sound Options] in the install: Music 85, Voiceover 85, Effects 85, Movie 100.
    g.check(std::fabs(v.music - 0.85f) < 1e-4f && std::fabs(v.voice - 0.85f) < 1e-4f
                && std::fabs(v.effects - 0.85f) < 1e-4f && std::fabs(v.movie - 1.0f) < 1e-4f,
            "ini_defaults_85_85_85_100");
    audio::Mixer& mixer = audio.mixer();
    g.check(std::fabs(mixer.busVolume(Bus::Music) - v.music * audio.musicScriptVolume()) < 1e-4f
                && std::fabs(mixer.busVolume(Bus::Effects) - v.effects) < 1e-4f
                && std::fabs(mixer.busVolume(Bus::Ambient) - v.effects) < 1e-4f
                && std::fabs(mixer.busVolume(Bus::Voice) - v.voice) < 1e-4f
                && std::fabs(mixer.busVolume(Bus::Movie) - v.movie) < 1e-4f,
            "bus_volumes_follow_the_ini");
}

// SetMovieMode: the four world buses mute, the Movie bus and the stored volumes stay.
void sectionMovieMode(Gate& g, GameSession& game, const Pool& pool, float masterFactor)
{
    AudioSystem& audio = game.audio();
    audio::Mixer& mixer = audio.mixer();
    const auto data = loadShared(audio, pool.names.front());
    g.check(data != nullptr, "movie_helper_sound_decodes");
    if (data == nullptr) {
        return;
    }
    mixer.setReverbEnvironment(-1, false);
    const int effectsVoice = mixer.play(data, Bus::Effects, 1.0f, true, false);
    const int voiceVoice = mixer.play(data, Bus::Voice, 1.0f, true, false);
    const int movieVoice = mixer.play(data, Bus::Movie, 1.0f, true, false);
    float stored[kBuses];
    for (int bus = 0; bus < kBuses; ++bus) {
        stored[bus] = mixer.busVolume(static_cast<Bus>(bus));
    }
    mixBlock(mixer, kBlock, nullptr);

    mixer.setVoiceCursor(movieVoice, 0.0);
    Meter normal;
    audioTicks(audio, 100, &normal);
    g.note(concat("movie_off rms music=", fx(normal.rms(Bus::Music), 5), " ambient=",
                  fx(normal.rms(Bus::Ambient), 5), " effects=", fx(normal.rms(Bus::Effects), 5),
                  " voice=", fx(normal.rms(Bus::Voice), 5), " movie=", fx(normal.rms(Bus::Movie), 5)));
    for (const Bus bus : {Bus::Music, Bus::Ambient, Bus::Effects, Bus::Voice, Bus::Movie}) {
        g.check(normal.rms(bus) > 0.0f, "movie_off_bus_rms_gt_0_" + std::to_string(static_cast<int>(bus)));
    }

    audio.setMovieMode(true);
    mixer.setVoiceCursor(movieVoice, 0.0);
    Meter muted;
    audioTicks(audio, 100, &muted);
    g.note(concat("movie_on rms music=", fx(muted.rms(Bus::Music), 5), " ambient=",
                  fx(muted.rms(Bus::Ambient), 5), " effects=", fx(muted.rms(Bus::Effects), 5),
                  " voice=", fx(muted.rms(Bus::Voice), 5), " movie=", fx(muted.rms(Bus::Movie), 5)));
    g.check(audio.movieMode(), "movie_mode_flag_on");
    for (const Bus bus : {Bus::Music, Bus::Ambient, Bus::Effects, Bus::Voice}) {
        g.check(muted.rms(bus) == 0.0f && muted.pk(bus) == 0.0f && mixer.busPaused(bus) && !mixer.busMuted(bus),
                "movie_on_world_bus_silent_" + std::to_string(static_cast<int>(bus)));
    }
    const float movieRatio = normal.rms(Bus::Movie) > 0.0f ? muted.rms(Bus::Movie) / normal.rms(Bus::Movie) : 0.0f;
    g.note(concat("movie_bus_ratio_on_over_off=", fx(movieRatio, 5)));
    g.check(std::fabs(movieRatio - 1.0f) < 0.001f && !mixer.busMuted(Bus::Movie), "movie_on_movie_bus_unchanged");
    bool volumesKept = true;
    for (int bus = 0; bus < kBuses; ++bus) {
        volumesKept = volumesKept && mixer.busVolume(static_cast<Bus>(bus)) == stored[bus];
    }
    g.check(volumesKept, "movie_on_stored_bus_volumes_untouched");
    // Only the Movie bus reaches the output while the mode is on.
    const float expectedMaster = muted.rms(Bus::Movie) * masterFactor;
    g.note(concat("movie_on master_rms=", fx(muted.masterRms(), 5), " expected_from_movie_bus=",
                  fx(expectedMaster, 5)));
    g.check(expectedMaster > 0.0f && std::fabs(muted.masterRms() - expectedMaster) < 0.02f * expectedMaster,
            "movie_on_captured_buffer_is_the_movie_bus_only");
    g.check(voicesOn(audio, Bus::Music) >= 1 && voicesOn(audio, Bus::Ambient) >= 1,
            "movie_on_voices_keep_running_muted");

    audio.setMovieMode(false);
    mixer.setVoiceCursor(movieVoice, 0.0);
    Meter back;
    audioTicks(audio, 100, &back);
    g.check(!audio.movieMode() && back.rms(Bus::Music) > 0.0f && back.rms(Bus::Ambient) > 0.0f
                && back.rms(Bus::Effects) > 0.0f && back.rms(Bus::Voice) > 0.0f,
            "movie_off_again_world_buses_return");
    g.check(std::fabs(back.rms(Bus::Movie) - normal.rms(Bus::Movie)) < 0.001f * normal.rms(Bus::Movie),
            "movie_off_again_movie_bus_unchanged");
    mixer.stopVoice(effectsVoice);
    mixer.stopVoice(voiceVoice);
    mixer.stopVoice(movieVoice);
}

// UTS Volume is 0..127: read the shipped templates independently and measure the voices.
void sectionVolumeScale(Gate& g, GameSession& game, const Pool& pool)
{
    AudioSystem& audio = game.audio();
    audio::Mixer& mixer = audio.mixer();
    int mismatches = 0;
    int read = 0;
    int over100 = 0;
    int atMax = 0;
    int maxSeen = 0;
    for (const kotor::SoundEmitter& e : game.scene().soundEmitters) {
        auto bytes = game.install().find(e.templateResref, kotor::resType(kotor::ResType::Uts));
        if (!bytes.has_value()) {
            continue;
        }
        const kotor::Gff uts = kotor::Gff::parse(std::move(*bytes), e.templateResref + ".uts");
        const kotor::GffField* field = uts.field(uts.rootIndex(), "Volume");
        if (field == nullptr) {
            continue;
        }
        const int raw = uts.integer(*field);
        ++read;
        maxSeen = std::max(maxSeen, raw);
        over100 += raw > 100 ? 1 : 0;
        atMax += raw == 127 ? 1 : 0;
        if (e.volume127 != std::clamp(raw, 0, 127)
            || std::fabs(e.volume - static_cast<float>(std::clamp(raw, 0, 127)) / 127.0f) > 1.0e-6f) {
            ++mismatches;
        }
    }
    g.note(concat("uts volume read=", read, " mismatches=", mismatches, " over100=", over100,
                  " at127=", atMax, " max=", maxSeen));
    g.check(read == 45 && mismatches == 0, "volume_scene_matches_independent_gff_read");
    g.check(over100 > 0 && maxSeen == 127, "volume_shipped_data_exceeds_100_so_it_is_not_a_percentage");

    // Rendered level against a unit-gain reference of the same sound.
    const auto data = loadShared(audio, pool.names.front());
    if (data == nullptr) {
        g.check(false, "volume_helper_sound_decodes");
        return;
    }
    auto measure = [&](int volume127) {
        kotor::SoundEmitter e = fixtureEmitter(1000, {pool.names.front()});
        e.positional = false;
        e.looping = true;
        e.volume127 = volume127;
        e.volume = static_cast<float>(volume127) / 127.0f;
        loadFixture(audio, {e}, {0.0f, 0.0f, 0.0f});
        const AreaAmbience::SoundObjectInfo info = audio.ambience().soundObject(0);
        mixer.setVoiceCursor(info.voice, 0.0);
        Meter m;
        mixBlock(mixer, 4800, &m);
        return std::make_pair(mixer.voiceVolume(info.voice), m.rms(Bus::Effects));
    };
    mixer.stopAll();
    const int reference = mixer.play(data, Bus::Effects, 1.0f, true, false);
    Meter ref;
    mixBlock(mixer, 4800, &ref);
    mixer.stopVoice(reference);
    const float refRms = ref.rms(Bus::Effects);
    const auto full = measure(127);
    const auto half = measure(64);
    const float ratioFull = refRms > 0.0f ? full.second / refRms : 0.0f;
    const float ratioHalf = refRms > 0.0f ? half.second / refRms : 0.0f;
    g.note(concat("volume render reference_rms=", fx(refRms, 5), " v127_ratio=", fx(ratioFull, 4),
                  " v64_ratio=", fx(ratioHalf, 4), " v64_voice_volume=", fx(half.first, 4)));
    // A looping area-wide sound streams in priority group 4 (Volume 95): 95/127 times Volume/127 (005dc930).
    const float group4 = static_cast<float>(audio.priorityGroup(AudioSystem::kGroupLoopAreaWide).volume) / 127.0f;
    g.check(refRms > 0.0f && std::fabs(ratioFull - group4) < 0.003f, "volume_127_renders_at_the_group_level");
    g.check(std::fabs(ratioHalf - group4 * 64.0f / 127.0f) < 0.003f
                && std::fabs(half.first - group4 * 64.0f / 127.0f) < 1e-5f,
            "volume_64_renders_at_64_over_127_of_the_group_level");
    // The old scale divided by 100: Volume 64 would have rendered at 0.64 (and 127 at 1.27).
    g.refute(std::fabs(ratioHalf - 0.64f) < 0.003f || std::fabs(half.first - 0.64f) < 1e-3f,
             "old_volume_over_100_scale");
    g.refute(ratioFull > 1.2f, "old_volume_127_over_100_is_1_27");
    g.refute(std::fabs(ratioFull - 1.0f) < 0.003f, "old_volume_127_ignores_the_priority_group");
}

// Positional gain and pan against distance, and the rendered level and channel balance.
void sectionPositional(Gate& g, GameSession& game, const Pool& pool)
{
    AudioSystem& audio = game.audio();
    audio::Mixer& mixer = audio.mixer();
    kotor::SoundEmitter e = fixtureEmitter(1000, {pool.names.front()});
    e.looping = true;
    e.minDistance = 2.0f;
    e.maxDistance = 10.0f;
    loadFixture(audio, {e}, {2.0f, 0.0f, 0.0f});
    const int voice = audio.ambience().soundObject(0).voice;
    g.check(voice != 0 && mixer.voiceActive(voice), "positional_fixture_voice_live");

    auto levelAt = [&](float x, float y, float yaw, Meter& meter) {
        audio.setListener({x, y, 0.0f}, yaw);
        mixer.setVoiceCursor(voice, 0.0);
        mixBlock(mixer, 4800, &meter);
    };
    Meter unit;
    levelAt(2.0f, 0.0f, 0.0f, unit);
    const float unitRms = unit.rms(Bus::Effects);
    g.check(unitRms > 0.0f, "positional_unit_level_audible");
    // Looping positional sound: priority group 5 (Volume 100), a stream, so 100/127 at full level.
    const float groupLevel5 = audio.voiceLevel(AudioSystem::kGroupLoopPositional, 127, true, true);
    bool gainOk = true;
    bool renderOk = true;
    float previous = 2.0f;
    float gainBeyond = 1.0f;
    bool monotonic = true;
    std::string table;
    for (const float d : {0.0f, 1.0f, 2.0f, 3.5f, 5.0f, 7.5f, 9.0f, 10.0f, 12.0f}) {
        audio.setListener({d, 0.0f, 0.0f}, 0.0f);
        // Retail 005dc560: f is linear between min and max, a source behind the listener (here always,
        // the listener looks along +x and the emitter sits at the origin) keeps 1 + 0.125 * dot = 0.875,
        // and Miles renders the amplitude linearly in that product.
        const auto retailGain = [](float distance) {
            float f = distance > 2.0f ? std::clamp((10.0f - distance) / (10.0f - 2.0f), 0.0f, 1.0f) : 1.0f;
            if (distance > 1.0e-4f) {
                f *= 0.875f;
            }
            return f;
        };
        const float expected = retailGain(d);
        const float gain = mixer.voiceGain(voice) / groupLevel5;
        Meter m;
        levelAt(d, 0.0f, 0.0f, m);
        const float ratio = unitRms > 0.0f ? m.rms(Bus::Effects) / unitRms : 0.0f;
        gainOk = gainOk && std::fabs(gain - expected) < 1.0e-5f;
        renderOk = renderOk && std::fabs(ratio - expected / retailGain(2.0f)) < 0.004f;
        monotonic = monotonic && gain <= previous + 1e-6f;
        previous = gain;
        if (d >= 12.0f) {
            gainBeyond = gain;
        }
        table += concat(" d", fx(d, 1), "=", fx(gain, 4), "/", fx(ratio, 4));
    }
    g.note("positional gain/rendered ratio:" + table);
    g.check(gainOk, "positional_voice_gain_follows_retail_stream_falloff");
    g.check(renderOk, "positional_rendered_level_follows_gain");
    g.check(monotonic, "positional_gain_never_rises_with_distance");
    g.refute(gainBeyond > 1e-6f, "gain_beyond_max_distance_is_not_zero");

    // Pan: yaw 0 puts the right-hand vector at -y; hard sides must leave the other channel dark.
    struct Side {
        float x;
        float y;
        float yaw;
        float pan;
    };
    // A looping positional sound is a stream: pan = 1 - |dot| / 2 on the right, |dot| / 2 on the left, clamped to
    // 0.05 .. 0.95 (005dc560), and Mss32 2112a940 renders left = (1 - pan)^0.3, right = pan^0.3 times the volume,
    // so a source exactly at the side (dot 0) has pan 0.95 and one straight ahead or behind is the centre.
    constexpr float kSide = 0.9f; // 2 * 0.95 - 1, the value Mixer::voicePan reports
    for (const Side side : {Side{0.0f, 5.0f, 0.0f, kSide}, Side{0.0f, -5.0f, 0.0f, -kSide},
                            Side{5.0f, 0.0f, 0.0f, 0.0f}, Side{-5.0f, 0.0f, 1.5707963f, kSide}}) {
        Meter m;
        audio.setListener({side.x, side.y, 0.0f}, side.yaw);
        const float pan = mixer.voicePan(voice);
        levelAt(side.x, side.y, side.yaw, m);
        const std::string tag = fx(side.pan, 3);
        g.note(concat("pan expected=", tag, " measured=", fx(pan, 4), " left_rms=", fx(m.leftRms(), 5),
                      " right_rms=", fx(m.rightRms(), 5)));
        g.check(std::fabs(pan - side.pan) < 1e-3f, "pan_value_" + tag + "_at_" + fx(side.x, 0) + "_" + fx(side.y, 0));
        // The constant-power ratio of the two channels at that pan.
        const float ratio = std::pow(0.95f / 0.05f, 0.3f);
        if (side.pan > 0.5f) {
            g.check(m.leftRms() > 0.0f && std::fabs(m.rightRms() / m.leftRms() - ratio) < 0.05f * ratio,
                    "pan_right_side_is_the_retail_stream_ratio");
        } else if (side.pan < -0.5f) {
            g.check(m.rightRms() > 0.0f && std::fabs(m.leftRms() / m.rightRms() - ratio) < 0.05f * ratio,
                    "pan_left_side_is_the_retail_stream_ratio");
        }
    }
}

// Interval timing, list order, loop, pitch, volume and position variance under a fixed dt.
void sectionSequencing(Gate& g, GameSession& game, const Pool& pool)
{
    AudioSystem& audio = game.audio();
    const std::string shortName = pool.names[pool.shortest];

    // Interval 1500, no variance: the gap after each sound is exactly 150 ticks.
    {
        kotor::SoundEmitter e = fixtureEmitter(1000, {shortName});
        e.interval = 1500;
        loadFixture(audio, {e}, {5.0f, 0.0f, 0.0f});
        const std::vector<PlayRecord> plays = collectPlays(audio, 6, 4000);
        const std::vector<int> gaps = gapsTicks(plays);
        bool exact = gaps.size() >= 5;
        for (const int gap : gaps) {
            exact = exact && std::abs(gap - 150) <= 1;
        }
        const double duration = audio.sound(shortName) != nullptr ? audio.sound(shortName)->durationSeconds() : 0.0;
        bool cadence = plays.size() >= 6;
        for (std::size_t i = 0; i + 1 < plays.size(); ++i) {
            cadence = cadence && std::abs((plays[i + 1].tick - plays[i].tick) - static_cast<int>(std::lround(duration / kDt) + 150)) <= 2;
        }
        g.note(concat("interval1500 plays=", plays.size(), " gaps_ticks=", gaps.empty() ? 0 : gaps.front(),
                      "..", gaps.empty() ? 0 : gaps.back(), " sound_s=", fx(duration, 3)));
        g.check(exact, "interval_1500_gap_is_150_ticks");
        g.check(cadence, "interval_1500_start_to_start_is_duration_plus_interval");
        // A gate that demanded zero tolerance on the variance case would fail; see below.
    }
    // Interval 500 with variance 250: gaps stay inside 250..750 ms and actually vary.
    {
        kotor::SoundEmitter e = fixtureEmitter(1000, {shortName});
        e.interval = 500;
        e.intervalVariation = 250;
        loadFixture(audio, {e}, {5.0f, 0.0f, 0.0f});
        const std::vector<PlayRecord> plays = collectPlays(audio, 40, 20000);
        const std::vector<int> gaps = gapsTicks(plays);
        std::set<int> distinct(gaps.begin(), gaps.end());
        bool inside = !gaps.empty();
        double mean = 0.0;
        for (const int gap : gaps) {
            inside = inside && gap >= 24 && gap <= 76;
            mean += gap;
        }
        mean = gaps.empty() ? 0.0 : mean / static_cast<double>(gaps.size());
        g.note(concat("interval500+-250 plays=", plays.size(), " distinct_gaps=", distinct.size(),
                      " mean_ticks=", fx(mean, 2)));
        g.check(plays.size() >= 40 && inside, "interval_variance_gaps_within_250_750_ms");
        g.check(distinct.size() >= 8 && mean > 40.0 && mean < 60.0, "interval_variance_is_random_and_centred");
        g.refute(distinct.size() <= 1, "gaps_all_identical_with_variance");
    }
    // Interval 0, continuous: the next play follows on the very next tick.
    {
        kotor::SoundEmitter e = fixtureEmitter(1000, {shortName});
        loadFixture(audio, {e}, {5.0f, 0.0f, 0.0f});
        const std::vector<PlayRecord> plays = collectPlays(audio, 4, 4000);
        const std::vector<int> gaps = gapsTicks(plays);
        bool tight = gaps.size() >= 3;
        for (const int gap : gaps) {
            tight = tight && gap <= 1;
        }
        g.check(tight, "interval_0_continuous_restarts_next_tick");
    }
    // Non-continuous list of three: played once in list order with the interval between
    // entries, then Active clears and it stays silent.
    {
        const std::vector<std::string> three = {pool.names[0], pool.names[1], pool.names[2]};
        kotor::SoundEmitter e = fixtureEmitter(1000, three);
        e.continuous = false;
        e.interval = 300;
        loadFixture(audio, {e}, {5.0f, 0.0f, 0.0f});
        const std::vector<PlayRecord> plays = collectPlays(audio, 4, 3000);
        audioTicks(audio, 500);
        const AreaAmbience::SoundObjectInfo info = audio.ambience().soundObject(0);
        bool order = plays.size() == 3;
        for (std::size_t i = 0; i < plays.size() && i < 3; ++i) {
            order = order && lowered(plays[i].sound) == lowered(three[i]);
        }
        const std::vector<int> gaps = gapsTicks(plays);
        bool spaced = gaps.size() == 2;
        for (const int gap : gaps) {
            spaced = spaced && std::abs(gap - 30) <= 1;
        }
        g.note(concat("non_continuous plays=", plays.size(), " active_after=", info.active ? 1 : 0));
        g.check(order, "sequence_non_continuous_plays_the_list_once_in_order");
        g.check(spaced, "sequence_interval_between_list_entries");
        g.check(!info.active && !info.playing, "sequence_non_continuous_clears_active_at_the_end");
    }
    // Continuous, not random: the list repeats in order. Random: a different order each wrap.
    {
        const std::vector<std::string> four = {pool.names[0], pool.names[1], pool.names[2], pool.names[3]};
        kotor::SoundEmitter e = fixtureEmitter(1000, four);
        loadFixture(audio, {e}, {5.0f, 0.0f, 0.0f});
        const std::vector<PlayRecord> plain = collectPlays(audio, 12, 20000);
        bool repeats = plain.size() >= 12;
        for (std::size_t i = 0; i < plain.size(); ++i) {
            repeats = repeats && lowered(plain[i].sound) == lowered(four[i % 4]);
        }
        g.check(repeats, "sequence_continuous_repeats_list_order");

        e.random = true;
        loadFixture(audio, {e}, {5.0f, 0.0f, 0.0f});
        const std::vector<PlayRecord> shuffled = collectPlays(audio, 40, 60000);
        bool firstPass = shuffled.size() >= 40;
        bool permutations = shuffled.size() >= 40;
        int differing = 0;
        for (std::size_t wrap = 0; wrap * 4 + 3 < shuffled.size(); ++wrap) {
            std::set<std::string> names;
            bool same = true;
            for (std::size_t k = 0; k < 4; ++k) {
                const std::string name = lowered(shuffled[wrap * 4 + k].sound);
                names.insert(name);
                same = same && name == lowered(four[k]);
            }
            permutations = permutations && names.size() == 4;
            if (wrap == 0) {
                firstPass = same;
            } else if (!same) {
                ++differing;
            }
        }
        g.note(concat("random list wraps_differing_from_list_order=", differing, " of 9"));
        g.check(firstPass, "sequence_random_first_pass_is_list_order");
        g.check(permutations, "sequence_random_every_wrap_plays_each_sound_once");
        g.check(differing >= 5, "sequence_random_reshuffles_at_the_wrap");
    }
    // Looping: one voice that never ends.
    {
        kotor::SoundEmitter e = fixtureEmitter(1000, {shortName});
        e.looping = true;
        loadFixture(audio, {e}, {5.0f, 0.0f, 0.0f});
        const int voice = audio.ambience().soundObject(0).voice;
        audioTicks(audio, 1000);
        const AreaAmbience::SoundObjectInfo info = audio.ambience().soundObject(0);
        g.check(voice != 0 && info.voice == voice && info.playing, "looping_one_voice_stays_live_for_10s");
    }
}

void sectionVariance(Gate& g, GameSession& game, const Pool& pool)
{
    AudioSystem& audio = game.audio();
    const std::string shortName = pool.names[pool.shortest];

    // PitchVariation 0.3: the rate moves up or down by up to 30 % on every play.
    {
        kotor::SoundEmitter e = fixtureEmitter(1000, {shortName});
        e.pitchVariation = 0.3f;
        loadFixture(audio, {e}, {5.0f, 0.0f, 0.0f});
        const std::vector<PlayRecord> plays = collectPlays(audio, 60, 30000);
        std::set<int> distinct;
        bool bounds = !plays.empty();
        bool measured = !plays.empty();
        double mean = 0.0;
        for (const PlayRecord& play : plays) {
            bounds = bounds && play.pitch >= 0.7f - 1e-5f && play.pitch <= 1.3f + 1e-5f;
            measured = measured && std::fabs(play.measuredPitch - play.pitch) < 0.002f;
            distinct.insert(static_cast<int>(std::lround(play.pitch * 1000.0f)));
            mean += play.pitch;
        }
        mean = plays.empty() ? 0.0 : mean / static_cast<double>(plays.size());
        g.note(concat("pitch_variation plays=", plays.size(), " distinct=", distinct.size(), " mean=", fx(mean, 3)));
        g.check(bounds && plays.size() >= 60, "pitch_variation_stays_within_plus_minus_30_percent");
        g.check(measured, "pitch_variation_voice_cursor_advances_at_the_recorded_rate");
        g.check(distinct.size() >= 20 && mean > 0.9 && mean < 1.1, "pitch_variation_differs_every_play");
    }
    // FixedVariance: a constant rate in 0.5..2.0; outside that the client ignores it.
    for (const float variance : {1.5f, 0.75f, 3.0f, 0.25f}) {
        kotor::SoundEmitter e = fixtureEmitter(1000, {shortName});
        e.fixedVariance = variance;
        loadFixture(audio, {e}, {5.0f, 0.0f, 0.0f});
        const std::vector<PlayRecord> plays = collectPlays(audio, 1, 2000);
        const float expected = variance >= 0.5f && variance <= 2.0f ? variance : 1.0f;
        g.check(!plays.empty() && std::fabs(plays.front().measuredPitch - expected) < 0.002f,
                "fixed_variance_" + fx(variance, 2) + "_plays_at_" + fx(expected, 2));
    }
    // VolumeVrtn 20 around Volume 92: each play draws 72..111 of 127.
    {
        kotor::SoundEmitter e = fixtureEmitter(1000, {shortName});
        e.volume127 = 92;
        e.volume = 92.0f / 127.0f;
        e.volumeVariation = 20;
        loadFixture(audio, {e}, {5.0f, 0.0f, 0.0f});
        const std::vector<PlayRecord> plays = collectPlays(audio, 60, 30000);
        std::set<int> distinct;
        bool bounds = !plays.empty();
        bool voices = !plays.empty();
        double mean = 0.0;
        for (const PlayRecord& play : plays) {
            bounds = bounds && play.volume127 >= 72 && play.volume127 <= 112;
            voices = voices && std::fabs(play.voiceVolume - audio.voiceLevel(AudioSystem::kGroupSinglePositional, play.volume127, false, true)) < 1e-5f;
            distinct.insert(play.volume127);
            mean += play.volume127;
        }
        mean = plays.empty() ? 0.0 : mean / static_cast<double>(plays.size());
        g.note(concat("volume_variation plays=", plays.size(), " distinct=", distinct.size(), " mean=", fx(mean, 2)));
        g.check(bounds && plays.size() >= 60, "volume_variation_within_72_112");
        g.check(voices, "volume_variation_voice_volume_is_drawn_over_127");
        g.check(distinct.size() >= 8 && mean > 85.0 && mean < 99.0, "volume_variation_differs_every_play");
    }
    // RandomPosition 20 x 10: the play lands in that box around the object.
    {
        kotor::SoundEmitter e = fixtureEmitter(1000, {shortName});
        e.randomPosition = true;
        e.randomRangeX = 20.0f;
        e.randomRangeY = 10.0f;
        loadFixture(audio, {e}, {5.0f, 0.0f, 0.0f});
        const std::vector<PlayRecord> plays = collectPlays(audio, 60, 30000);
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        bool inside = !plays.empty();
        for (const PlayRecord& play : plays) {
            inside = inside && std::fabs(play.position.x) <= 10.0f + 1e-3f && std::fabs(play.position.y) <= 5.0f + 1e-3f;
            minX = std::min(minX, play.position.x);
            maxX = std::max(maxX, play.position.x);
            minY = std::min(minY, play.position.y);
            maxY = std::max(maxY, play.position.y);
        }
        g.note(concat("random_position x=", fx(minX, 2), "..", fx(maxX, 2), " y=", fx(minY, 2), "..", fx(maxY, 2)));
        g.check(inside && plays.size() >= 60, "random_position_stays_inside_the_range_box");
        g.check(maxX - minX > 8.0f && maxY - minY > 4.0f, "random_position_moves_every_play");
    }
    // The same seed repeats a run exactly; another seed does not.
    {
        auto run = [&](std::uint64_t seed) {
            const std::vector<std::string> four = {pool.names[0], pool.names[1], pool.names[2], pool.names[3]};
            kotor::SoundEmitter e = fixtureEmitter(1000, four);
            e.random = true;
            e.pitchVariation = 0.3f;
            e.volume127 = 92;
            e.volume = 92.0f / 127.0f;
            e.volumeVariation = 20;
            e.randomPosition = true;
            e.randomRangeX = 20.0f;
            e.randomRangeY = 10.0f;
            e.interval = 100;
            e.intervalVariation = 50;
            loadFixture(audio, {e}, {5.0f, 0.0f, 0.0f}, 0.0f, seed);
            const std::vector<PlayRecord> plays = collectPlays(audio, 12, 40000);
            std::string signature;
            for (const PlayRecord& play : plays) {
                signature += concat(play.sound, ",", play.tick, ",", fx(play.pitch, 5), ",", play.volume127, ",",
                                    fx(play.position.x, 4), ",", fx(play.position.y, 4), ";");
            }
            return signature;
        };
        const std::string a1 = run(42);
        const std::string a2 = run(42);
        const std::string b = run(43);
        g.check(!a1.empty() && a1 == a2, "seed_same_seed_repeats_every_draw");
        g.check(a1 != b, "seed_other_seed_differs");
    }
}

// Times and Hours gate playback by the time of day.
void sectionTimes(Gate& g, GameSession& game, const Pool& pool)
{
    AudioSystem& audio = game.audio();
    AreaAmbience& amb = audio.ambience();
    std::vector<kotor::SoundEmitter> list;
    const int times[4] = {1, 2, 3, 0};
    for (int i = 0; i < 4; ++i) {
        kotor::SoundEmitter e = fixtureEmitter(1000 + static_cast<std::uint32_t>(i), {pool.names.front()});
        e.positional = false;
        e.looping = true;
        e.times = times[i];
        e.hours = i == 3 ? (1u << 23u) : 0u;
        list.push_back(e);
    }
    loadFixture(audio, list, {0.0f, 0.0f, 0.0f});
    auto playing = [&]() {
        std::string state;
        for (std::size_t i = 0; i < 4; ++i) {
            state += amb.soundObject(i).playing ? '1' : '0';
        }
        return state;
    };
    audioTicks(audio, 5);
    const std::string day = playing();
    audio.setTimeOfDay(2, 23);
    audioTicks(audio, 5);
    const std::string night = playing();
    audio.setTimeOfDay(3, 6);
    audioTicks(audio, 5);
    const std::string dawn = playing();
    audio.setTimeOfDay(4, 18);
    audioTicks(audio, 5);
    const std::string dusk = playing();
    audio.setTimeOfDay(1, 12);
    audioTicks(audio, 5);
    const std::string noon = playing();
    g.note("times (day-only night-only always hours{23}): day=" + day + " night=" + night + " dawn=" + dawn
           + " dusk=" + dusk + " noon=" + noon);
    g.check(day == "1010", "times_day_state");
    g.check(night == "0111", "times_night_state_and_hour_23_mask");
    g.check(dawn == "1010", "times_dawn_counts_as_day");
    g.check(dusk == "0110", "times_dusk_counts_as_night_hour_18_outside_mask");
    g.check(noon == "1010", "times_back_to_day_restarts_the_day_sounds");
    g.refute(day == night, "day_and_night_play_the_same_emitters");
    g.check(amb.setSoundObjectTimes(1000, 3, 0) && amb.soundObjectCount() == 4, "times_hook_changes_a_live_emitter");
    audio.setTimeOfDay(2, 23);
    audioTicks(audio, 5);
    g.check(playing() == "1111", "times_day_only_made_always_plays_at_night");
}

// Only 16 positional sounds get a voice; the nearest win.
void sectionBudget(Gate& g, GameSession& game, const Pool& pool)
{
    AudioSystem& audio = game.audio();
    AreaAmbience& amb = audio.ambience();
    std::vector<kotor::SoundEmitter> list;
    // One-shot samples that outlast the section take the 16 voices; looping ones are streams and take none.
    std::size_t longest = 0;
    for (std::size_t i = 1; i < pool.durations.size(); ++i) {
        if (pool.durations[i] > pool.durations[longest]) {
            longest = i;
        }
    }
    for (int i = 0; i < 24; ++i) {
        kotor::SoundEmitter e = fixtureEmitter(2000 + static_cast<std::uint32_t>(i), {pool.names[longest]});
        e.looping = false;
        e.position = {3.0f * static_cast<float>(i), 0.0f, 0.0f};
        e.maxDistance = 200.0f;
        list.push_back(e);
    }
    loadFixture(audio, list, {0.0f, 0.0f, 0.0f});
    audioTicks(audio, 200);
    std::size_t nearest = 0;
    std::size_t farthest = 0;
    for (std::size_t i = 0; i < 24; ++i) {
        (i < 16 ? nearest : farthest) += amb.soundObject(i).playing ? 1u : 0u;
    }
    g.note(concat("budget playing nearest16=", nearest, " farthest8=", farthest, " effects_voices=",
                  voicesOn(audio, Bus::Effects)));
    g.check(amb.activeSoundVoices() == 16 && voicesOn(audio, Bus::Effects) == 16, "budget_16_positional_voices");
    g.check(nearest == 16 && farthest == 0, "budget_goes_to_the_nearest_16");
    g.refute(voicesOn(audio, Bus::Effects) > 16, "more_than_16_positional_voices");
    amb.stopSoundObject(2000);
    audioTicks(audio, 100);
    g.check(voicesOn(audio, Bus::Effects) == 16 && amb.soundObject(16).playing && !amb.soundObject(0).playing,
            "budget_a_freed_voice_goes_to_the_nearest_waiting_emitter");
}

// The script actions, on fixtures bound to real GIT sound entities of end_m01aa.
void sectionScripts(Gate& g, GameSession& game, const Pool& pool)
{
    AudioSystem& audio = game.audio();
    audio::Mixer& mixer = audio.mixer();
    AreaAmbience& amb = audio.ambience();
    const auto& shipped = game.scene().soundEmitters;
    if (shipped.size() < 3) {
        g.check(false, "script_fixture_needs_three_real_sound_entities");
        return;
    }
    const std::uint32_t a = shipped[0].entity;
    const std::uint32_t b = shipped[1].entity;
    const std::uint32_t c = shipped[2].entity;
    const std::uint32_t objA = game.objectForEntityIndex(a);
    const std::uint32_t objB = game.objectForEntityIndex(b);
    const std::uint32_t objC = game.objectForEntityIndex(c);
    g.check(objA != script::kObjectInvalid && objB != script::kObjectInvalid && objC != script::kObjectInvalid,
            "script_real_sound_objects_resolve");

    kotor::SoundEmitter looping = fixtureEmitter(a, {pool.names.front()});
    looping.looping = true;
    looping.minDistance = 2.0f;
    looping.maxDistance = 10.0f;
    kotor::SoundEmitter dormant = fixtureEmitter(b, {pool.names[pool.shortest]});
    dormant.active = false;
    dormant.continuous = false;
    dormant.positional = false;
    kotor::SoundEmitter spare = fixtureEmitter(c, {pool.names.front()});
    spare.looping = true;
    spare.positional = false;
    loadFixture(audio, {looping, dormant, spare}, {2.0f, 0.0f, 0.0f});
    audioTicks(audio, 5);

    // Active=0 stays silent until SoundObjectPlay (413).
    audioTicks(audio, 300);
    g.check(!amb.soundObject(1).playing && !amb.soundObject(1).active, "script_active_0_emitter_silent");
    act(game, 413, {obj(objB)});
    g.check(amb.soundObject(1).playing, "script_413_sound_object_play_starts_an_inactive_emitter");
    audioTicks(audio, static_cast<int>(pool.durations[pool.shortest] / kDt) + 100);
    g.check(!amb.soundObject(1).playing && !amb.soundObject(1).active,
            "script_413_non_continuous_plays_once_then_deactivates");

    // 415 / 691: volume 0..127, clamped.
    act(game, 415, {obj(objA), num(200)});
    const auto got200 = act(game, 691, {obj(objA)});
    g.check(got200.has_value() && !got200->empty() && got200->front().asInt() == 127, "script_415_volume_clamps_to_127");
    act(game, 415, {obj(objA), num(40)});
    const auto got40 = act(game, 691, {obj(objA)});
    const int liveVoice = amb.soundObject(0).voice;
    g.check(got40.has_value() && !got40->empty() && got40->front().asInt() == 40, "script_691_reads_back_40");
    g.note(concat("script volume 40: voice_volume=", fx(mixer.voiceVolume(liveVoice), 5)));
    const float level40 = audio.voiceLevel(AudioSystem::kGroupLoopPositional, 40, true, true);
    g.check(std::fabs(mixer.voiceVolume(liveVoice) - level40) < 1e-5f, "script_415_live_voice_volume_is_40_over_127_of_the_group");
    g.refute(std::fabs(mixer.voiceVolume(liveVoice) - 0.40f) < 1e-3f, "old_script_volume_over_100");

    // 416: moving the object moves the voice (gain and pan follow).
    const float gainNear = mixer.voiceGain(liveVoice);
    act(game, 416, {obj(objA), {script::Value::makeFloat(8.0), script::Value::makeFloat(0.0), script::Value::makeFloat(0.0)}});
    const float gainFar = mixer.voiceGain(liveVoice);
    g.note(concat("script position: gain ", fx(gainNear, 4), " -> ", fx(gainFar, 4)));
    g.check(std::fabs(amb.soundObject(0).lastPosition.x - 8.0f) < 1e-4f && gainFar < gainNear
                && std::fabs(gainFar - 0.5f * level40) < 1e-4f,
            "script_416_set_position_moves_the_voice");

    // 689 / 690 and 124 / 188: pitch variance and fixed variance read back as written.
    act(game, 690, {obj(objA), real(0.5)});
    const auto pv = act(game, 689, {obj(objA)});
    g.check(pv.has_value() && !pv->empty() && std::fabs(pv->front().asFloat() - 0.5) < 1e-6, "script_689_690_pitch_variance_round_trip");
    act(game, 124, {obj(objA), real(1.5)});
    const auto fv = act(game, 188, {obj(objA)});
    g.check(fv.has_value() && !fv->empty() && std::fabs(fv->front().asFloat() - 1.5) < 1e-6, "script_124_188_fixed_variance_round_trip");
    g.check(std::fabs(amb.soundObject(0).lastPitch - 1.5f) < 1e-5f, "script_124_in_range_changes_the_live_pitch");
    act(game, 124, {obj(objA), real(3.0)});
    const auto fv3 = act(game, 188, {obj(objA)});
    g.check(fv3.has_value() && !fv3->empty() && std::fabs(fv3->front().asFloat() - 3.0) < 1e-6
                && std::fabs(amb.soundObject(0).lastPitch - 1.5f) < 1e-5f,
            "script_124_out_of_range_is_stored_but_the_client_pitch_is_unchanged");

    // 745: fade and stop. The voice multiplier falls linearly and the voice then ends.
    act(game, 745, {obj(objA), real(1.0)});
    audioTicks(audio, 50);
    const float mid = mixer.voiceFade(liveVoice);
    audioTicks(audio, 60);
    g.note(concat("script fade: at 0.5 s multiplier=", fx(mid, 3), " after 1.1 s active=",
                  mixer.voiceActive(liveVoice) ? 1 : 0));
    g.check(mid > 0.4f && mid < 0.6f, "script_745_fade_is_half_way_at_half_the_time");
    g.check(!mixer.voiceActive(liveVoice) && !amb.soundObject(0).active, "script_745_voice_ends_and_emitter_deactivates");

    // 414: stop an emitter that is playing.
    const int spareVoice = amb.soundObject(2).voice;
    g.check(spareVoice != 0 && mixer.voiceActive(spareVoice), "script_spare_emitter_live");
    act(game, 414, {obj(objC)});
    audioTicks(audio, 300);
    g.check(!mixer.voiceActive(spareVoice) && !amb.soundObject(2).playing && !amb.soundObject(2).active,
            "script_414_sound_object_stop_ends_the_voice_and_stays_off");
    act(game, 413, {obj(objC)});
    g.check(amb.soundObject(2).playing, "script_413_restarts_a_stopped_emitter");
}

// Area music layers on the shipped end_m01aa: MusicDelay, menu, loading and conversation tracks.
void sectionMusicLayers(Gate& g, GameSession& game, const MusicRows& rows)
{
    AudioSystem& audio = game.audio();
    audio::Mixer& mixer = audio.mixer();
    AreaAmbience& amb = audio.ambience();
    mixer.stopAll();
    std::string error;
    audio.loadScene(game.scene(), error);
    audio.setTimeOfDay(1, 12);
    audio.setListener(game.actorPosition(game.scriptWorld().pc), 0.0f);
    audioTicks(audio, 20);

    const std::string day = rows.cell(game.scene().areaAudio.musicDayId, "resource");
    g.note(concat("end_m01aa music day row=", game.scene().areaAudio.musicDayId, " resource=", day,
                  " delay_ms=", audio.areaAudio().musicDelay));
    g.check(!day.empty() && lowered(amb.musicResource()) == lowered(day) && amb.musicLayer() == MusicLayer::Area,
            "music_area_track_is_the_day_row_resource");
    g.check(audio.areaAudio().musicDelay == 5000, "music_delay_5000_ms_from_the_gff");

    // MusicDelay: jump the cursor to 0.35 s before the end, then measure the silence.
    const int old = amb.musicVoice();
    const audio::SoundData* data = audio.sound(amb.musicResource());
    g.check(old != 0 && data != nullptr, "music_delay_track_decoded");
    if (old == 0 || data == nullptr) {
        return;
    }
    mixer.setVoiceCursor(old, data->durationSeconds() - 0.35);
    int endTick = -1;
    int startTick = -1;
    Meter silence;
    Meter before;
    int newVoice = 0;
    for (int tick = 0; tick < 1000 && startTick < 0; ++tick) {
        audio.update(kDt);
        Meter* meter = endTick >= 0 ? &silence : &before;
        mixBlock(mixer, kBlock, meter);
        if (endTick < 0 && !mixer.voiceActive(old)) {
            endTick = tick;
        }
        if (endTick >= 0 && amb.musicVoice() != old && amb.musicVoice() != 0 && mixer.voiceActive(amb.musicVoice())) {
            startTick = tick;
            newVoice = amb.musicVoice();
        }
        if (endTick >= 0 && tick == endTick + 5) {
            g.check(std::fabs(amb.musicDelayLeftMs() - 4950.0) < 60.0, "music_delay_counts_down_from_5000");
        }
    }
    const int gap = startTick - endTick;
    g.note(concat("music_delay end_tick=", endTick, " start_tick=", startTick, " gap_ticks=", gap,
                  " silent_bus_rms=", fx(silence.rms(Bus::Music), 6)));
    g.check(endTick > 0 && startTick > endTick && std::abs(gap - 500) <= 2, "music_delay_silence_lasts_5000_ms");
    g.check(before.rms(Bus::Music) > 0.0f, "music_delay_track_audible_before_the_end");
    // The gap blocks before the restart tick are silent on the Music bus.
    g.check(silence.blocks > 400 && silence.minRms[static_cast<int>(Bus::Music)] == 0.0f,
            "music_delay_music_bus_is_silent_in_the_gap");
    g.check(newVoice != 0 && lowered(amb.musicResource()) == lowered(day), "music_delay_replays_the_same_track");
    g.refute(gap <= 1, "music_delay_ignored");

    // Menu, loading screen and conversation layers sit above the area track.
    // ambientmusic row 1 names a stream with no file in the install; the main menu theme
    // is named in the executable (retail 005f9af0).
    const std::string menu = "mus_theme_cult";
    g.note("ambientmusic row 1 resource=" + rows.cell(1, "resource") + " exists_in_install="
           + std::to_string(audio.hasSound(rows.cell(1, "resource")) ? 1 : 0));
    g.check(!audio.hasSound(rows.cell(1, "resource")) && audio.hasSound(menu), "music_menu_row_1_has_no_file_the_exe_names_the_theme");
    const bool menuStarted = audio.playMenuMusic();
    g.note(concat("menu row 1 resource=", menu, " started=", menuStarted ? 1 : 0, " layer=",
                  static_cast<int>(amb.musicLayer()), " playing=", amb.musicResource()));
    g.check(menuStarted && amb.musicLayer() == MusicLayer::Ui && lowered(amb.musicResource()) == lowered(menu),
            "music_menu_layer_plays_the_main_menu_theme");
    audioTicks(audio, 120);
    g.check(voicesOn(audio, Bus::Music) == 1, "music_menu_crossfade_finished_one_music_voice");
    g.check(audio.playLoadScreenMusic() && lowered(amb.musicResource()) == "mus_loadscreen", "music_loadscreen_track");
    audioTicks(audio, 120);
    audio.setCombatState(true);
    audio.playConversationTrack(day);
    g.check(amb.musicLayer() == MusicLayer::Ui, "music_ui_layer_wins_over_conversation_and_battle");
    audio.stopUiMusic();
    audioTicks(audio, 120);
    g.check(amb.musicLayer() == MusicLayer::Conversation && lowered(amb.musicResource()) == lowered(day),
            "music_conversation_track_wins_over_battle");
    audio.stopConversationTrack();
    audioTicks(audio, 120);
    g.check(amb.musicLayer() == MusicLayer::Battle, "music_battle_resumes_after_conversation");
    audio.setCombatState(false);
    audioTicks(audio, 120);
    g.check(amb.musicLayer() == MusicLayer::Area && trackVoices(audio) == 1,
            "music_back_to_the_area_track_one_voice");
}

// tar_m03af: day/night switching, battle music and the stinger, looping, the ambient volume.
void sectionTaris(Gate& g, GameSession& game, const MusicRows& rows)
{
    if (!switchModule(game, "tar_m03af")) {
        g.check(false, "taris_module_loads");
        return;
    }
    AudioSystem& audio = game.audio();
    audio::Mixer& mixer = audio.mixer();
    AreaAmbience& amb = audio.ambience();
    const kotor::AreaAudio& area = audio.areaAudio();
    g.note(concat("tar_m03af music day=", area.musicDayId, " night=", area.musicNightId, " battle=", area.musicBattleId,
                  " delay=", area.musicDelay, " ambient day/night volume=", area.ambientDayVolume, "/",
                  area.ambientNightVolume, " emitters=", audio.soundEmitterCount()));
    g.check(audio.soundEmitterCount() == 6 && area.musicDayId != area.musicNightId, "taris_data_6_emitters_distinct_day_night_music");

    const std::string dayRes = rows.cell(area.musicDayId, "resource");
    const std::string nightRes = rows.cell(area.musicNightId, "resource");
    const std::string battleRes = rows.cell(area.musicBattleId, "resource");

    // The module's own dawn and dusk hours may be unset (always day); night needs them.
    setClock(game, 23);
    {
        std::int32_t hour = 0;
        if (gameDayState(game, hour) != 2) {
            ScriptApi api{game};
            api.sim().clockDawnHour = 6;
            api.sim().clockDuskHour = 18;
            g.note("tar_m03af IFO has no dawn/dusk hours: set to 6 and 18 for the day/night checks");
        }
    }
    setClock(game, 12);
    const double clock0 = game.scriptWorld().time;
    gameTicks(game, 30);
    g.note(concat("taris world clock advanced ", fx(game.scriptWorld().time - clock0, 3), " s in 30 ticks; movie=",
                  game.moviePlaying() ? 1 : 0, " ended=", game.gameEnded() ? 1 : 0));
    {
        std::int32_t hour = 0;
        g.check(gameDayState(game, hour) == 1 && hour == 12, "taris_clock_noon_is_day_state_1");
    }

    // ---- day -> night crossfade ----
    gameTicks(game, 30);
    const int dayVoice = amb.musicVoice();
    g.check(amb.musicLayer() == MusicLayer::Area && lowered(amb.musicResource()) == lowered(dayRes) && voicesOn(audio, Bus::Music) == 1,
            "taris_day_plays_the_day_row_one_music_voice");
    const int ambientVoice = amb.ambientVoice();
    const float ambientDay = mixer.voiceVolume(ambientVoice);
    const float scale = amb.ambientScale();
    g.note(concat("taris ambient day volume=", fx(ambientDay, 5), " scale=", fx(scale, 3),
                  " level=", fx(amb.ambientLevel(), 5)));
    const float ambientGroup = static_cast<float>(audio.priorityGroup(AudioSystem::kGroupLoopAreaWide).volume) / 127.0f;
    g.check(std::fabs(amb.ambientLevel() - ambientGroup * static_cast<float>(area.ambientDayVolume) / 127.0f * scale) < 1e-5f
                && std::fabs(ambientDay - amb.ambientLevel()) < 1e-5f,
            "taris_ambient_day_level_is_volume_over_127_times_room_scale");
    g.refute(std::fabs(amb.ambientLevel() - static_cast<float>(area.ambientDayVolume) / 100.0f * scale) < 1e-4f,
             "old_ambient_volume_over_100");

    setClock(game, 23);
    Meter fade;
    gameTicks(game, 1, &fade);
    {
        std::int32_t hour = 0;
        const std::int32_t state = gameDayState(game, hour);
        g.note(concat("taris clock after SetTime(23): hour=", hour, " day_state=", state, " ambience_is_day=",
                      amb.isDay() ? 1 : 0, " music_layer=", static_cast<int>(amb.musicLayer()), " resource=",
                      amb.musicResource()));
    }
    const int nightVoice = amb.musicVoice();
    g.check(nightVoice != 0 && nightVoice != dayVoice && voicesOn(audio, Bus::Music) == 2, "taris_night_starts_a_second_music_voice_crossfading");
    gameTicks(game, 14, &fade);
    const float oldFade = mixer.voiceFade(dayVoice);
    const float newFade = mixer.voiceFade(nightVoice);
    g.note(concat("crossfade at ~0.5 s: old=", fx(oldFade, 3), " new=", fx(newFade, 3)));
    g.check(oldFade > 0.3f && oldFade < 0.7f && newFade > 0.3f && newFade < 0.7f && std::fabs(oldFade + newFade - 1.0f) < 0.12f,
            "taris_crossfade_is_linear_and_sums_to_one");
    gameTicks(game, 30, &fade);
    g.check(!mixer.voiceActive(dayVoice) && voicesOn(audio, Bus::Music) == 1 && lowered(amb.musicResource()) == lowered(nightRes),
            "taris_night_track_plays_alone_after_the_fade");
    g.check(fade.minRms[static_cast<int>(Bus::Music)] > 0.0f, "taris_crossfade_never_drops_the_music_bus_to_silence");
    g.check(amb.ambientVoice() == ambientVoice, "taris_same_ambient_loop_keeps_its_voice_at_night");
    const float ambientNight = mixer.voiceVolume(ambientVoice);
    g.note(concat("taris ambient night volume=", fx(ambientNight, 5), " (day ", fx(ambientDay, 5), ")"));
    g.check(std::fabs(ambientNight - ambientGroup * static_cast<float>(area.ambientNightVolume) / 127.0f * amb.ambientScale()) < 1e-5f
                && area.ambientNightVolume != area.ambientDayVolume && ambientNight != ambientDay,
            "taris_ambient_night_volume_differs_from_day");

    // ---- MusicDelay 0: the area track loops with no gap ----
    // One 0.2 s mix straddling the end of the track: the bus RMS must equal the RMS
    // computed straight from the decoded samples with the index wrapped, not the RMS of
    // the same window with silence after the end.
    {
        const audio::SoundData* data = audio.sound(amb.musicResource());
        const int voice = amb.musicVoice();
        g.check(data != nullptr && voice != 0 && audio.areaAudio().musicDelay == 0, "taris_loop_track_decoded");
        if (data != nullptr && voice != 0 && data->frameCount() > 400000) {
            const std::size_t frames = data->frameCount();
            const std::size_t start = frames - 4800;
            constexpr std::size_t window = 240000;   // 0.1 s before the end, 4.9 s after the wrap
            mixer.setVoiceCursor(voice, static_cast<double>(start) / mixer.sampleRate());
            const float gain = mixer.voiceVolume(voice) * mixer.busVolume(Bus::Music);
            const std::size_t channels = static_cast<std::size_t>(data->channels);
            auto at = [&](std::size_t frame, std::size_t channel) {
                return data->samples[frame * channels + std::min(channel, channels - 1)];
            };
            double wrapped = 0.0;
            double cut = 0.0;
            for (std::size_t i = 0; i < window; ++i) {
                const std::size_t frame = (start + i) % frames;
                const double l = at(frame, 0);
                const double r = at(frame, 1);
                wrapped += l * l + r * r;
                if (start + i < frames) {
                    cut += l * l + r * r;
                }
            }
            const float expectWrapped = static_cast<float>(gain * std::sqrt(wrapped / (static_cast<double>(window) * 2.0)));
            const float expectCut = static_cast<float>(gain * std::sqrt(cut / (static_cast<double>(window) * 2.0)));
            Meter wrap;
            mixBlock(mixer, static_cast<int>(window), &wrap);
            const float measured = wrap.rms(Bus::Music);
            g.note(concat("loop wrap: measured_rms=", fx(measured, 5), " expected_wrapped=", fx(expectWrapped, 5),
                          " expected_if_cut=", fx(expectCut, 5), " cursor_after=", fx(mixer.voiceCursor(voice), 3)));
            g.check(amb.musicVoice() == voice && mixer.voiceActive(voice) && mixer.voiceCursor(voice) < 6.0,
                    "taris_music_delay_0_loops_the_same_voice");
            g.check(expectWrapped > 2.0f * expectCut, "taris_loop_window_discriminates_wrap_from_cut");
            g.check(expectWrapped > 0.0f && std::fabs(measured - expectWrapped) < 0.01f * expectWrapped,
                    "taris_loop_wrap_renders_the_start_of_the_track_with_no_gap");
            g.refute(std::fabs(measured - expectCut) < 0.01f * expectCut, "loop_wrap_ends_in_silence");
        } else {
            g.check(false, "taris_loop_track_long_enough");
        }
    }

    // ---- back to day, then battle by script (430 / 431) ----
    setClock(game, 12);
    gameTicks(game, 50);
    g.check(amb.musicLayer() == MusicLayer::Area && lowered(amb.musicResource()) == lowered(dayRes) && voicesOn(audio, Bus::Music) == 1,
            "taris_back_to_day_one_voice");
    const int areaVoice = amb.musicVoice();
    act(game, 430, {});
    gameTicks(game, 1);
    const int battleVoice = amb.musicVoice();
    const int stinger = amb.stingerVoice();
    const std::string stingerRes = amb.stingerResource();
    g.note(concat("battle music resource=", amb.musicResource(), " row ", area.musicBattleId, " stinger=", stingerRes,
                  " music voices=", voicesOn(audio, Bus::Music)));
    g.check(amb.battle() && amb.musicLayer() == MusicLayer::Battle && lowered(amb.musicResource()) == lowered(battleRes)
                && battleVoice != areaVoice,
            "battle_430_switches_to_the_battle_row_track");
    const std::set<std::string> offered = {lowered(rows.cell(area.musicBattleId, "stinger1")),
                                           lowered(rows.cell(area.musicBattleId, "stinger2")),
                                           lowered(rows.cell(area.musicBattleId, "stinger3"))};
    g.check(stinger != 0 && mixer.voiceActive(stinger) && offered.count(lowered(stingerRes)) == 1
                && lowered(stingerRes) != lowered(amb.musicResource()),
            "battle_stinger_is_one_of_the_rows_stinger_columns_and_plays");
    g.check(voicesOn(audio, Bus::Music) >= 3, "battle_music_stinger_and_fading_area_track_overlap");
    gameTicks(game, 40);
    g.check(!mixer.voiceActive(areaVoice) && mixer.voiceActive(battleVoice), "battle_area_track_faded_out_battle_track_live");
    act(game, 430, {});
    gameTicks(game, 5);
    g.check(amb.stingerVoice() == stinger && amb.musicVoice() == battleVoice, "battle_430_again_does_not_restart_or_re_sting");
    act(game, 431, {});
    gameTicks(game, 1);
    g.check(!amb.battle() && amb.musicLayer() == MusicLayer::Area && lowered(amb.musicResource()) == lowered(dayRes),
            "battle_431_returns_to_the_area_track");
    gameTicks(game, 45);
    g.check(!mixer.voiceActive(battleVoice), "battle_431_battle_track_ends_after_the_fade");

    // ---- battle by a scripted combat state of the party ----
    // GameSession::update does not tick combat while a conversation is up, so an open
    // conversation (end_m01aa opens one on entry) holds combatTimer and the combat state
    // never ends. Record whether one is open, then close it (state only, no end scripts).
    g.note(concat("conversation open before the combat check=", game.dialogue().active ? 1 : 0));
    game.clearDialogueForTest();
    {
        ScriptApi api{game};
        ActorCombat& combat = api.actor(api.pc()).combat;
        combat.inCombat = true;
        combat.combatTimer = 8.0f;
    }
    // Retail SetExcitedState 004f3580: the battle music starts only when an enemy of the excited creature is
    // within 30 m. The end_m01aa creatures start out neutral, so the combat flag alone starts nothing; the
    // arming call below states an enemy within 30 m, as tickAmbience does when one stands there.
    const int stingerBefore = amb.stingerVoice();
    gameTicks(game, 2);
    g.check(!amb.battle(), "battle_combat_with_no_enemy_in_30_m_starts_no_music");
    {
        ScriptApi api{game};
        ActorCombat& combat = api.actor(api.pc()).combat;
        combat.inCombat = false;
        combat.combatTimer = 0.0f;
    }
    gameTicks(game, 2);
    {
        ScriptApi api{game};
        ActorCombat& combat = api.actor(api.pc()).combat;
        combat.inCombat = true;
        combat.combatTimer = 8.0f;
        amb.exciteTick(api.pc(), true, 8.0f, true, 0.0f);
    }
    gameTicks(game, 2);
    {
        ScriptApi api{game};
        g.note(concat("combat flag after 2 ticks=", api.actor(api.pc()).combat.inCombat ? 1 : 0, " timer=",
                      fx(api.actor(api.pc()).combat.combatTimer, 2), " battle=", amb.battle() ? 1 : 0));
    }
    g.check(amb.battle() && amb.musicLayer() == MusicLayer::Battle && amb.stingerVoice() != stingerBefore,
            "battle_party_combat_starts_battle_music_and_a_fresh_stinger");
    int fall = -1;
    for (int tick = 0; tick < 600 && fall < 0; ++tick) {
        gameTicks(game, 1);
        if (!amb.battle()) {
            fall = tick;
        }
    }
    g.note(concat("combat state ended after about ", fx(fall / 30.0, 2), " s"));
    g.check(fall >= 0, "battle_ends_when_the_combat_state_ends");
    if (fall >= 0) {
        gameTicks(game, 45);
        g.check(amb.musicLayer() == MusicLayer::Area && lowered(amb.musicResource()) == lowered(dayRes) && voicesOn(audio, Bus::Music) <= 2,
                "battle_end_returns_to_the_area_track");
    } else {
        ScriptApi api{game};
        api.actor(api.pc()).combat.inCombat = false;
    }
}

// korr_m33aa: per-room AmbientScale scales the ambient loop, ramping at 1.0 per second.
void sectionRoomScale(Gate& g, GameSession& game)
{
    if (!switchModule(game, "korr_m33aa")) {
        g.check(false, "korr_module_loads");
        return;
    }
    AudioSystem& audio = game.audio();
    audio::Mixer& mixer = audio.mixer();
    AreaAmbience& amb = audio.ambience();
    const std::vector<kotor::AreaRoomAudio>& rooms = audio.areaAudio().rooms;
    const kotor::AreaRoomAudio* from = nullptr;
    const kotor::AreaRoomAudio* wanted = nullptr;
    for (const kotor::AreaRoomAudio& room : rooms) {
        if (from == nullptr && room.ambientScale >= 0.3f) {
            from = &room;
        }
    }
    for (const kotor::AreaRoomAudio& room : rooms) {
        if (from != nullptr && wanted == nullptr && std::fabs(room.ambientScale - from->ambientScale) > 0.15f) {
            wanted = &room;
        }
    }
    g.check(from != nullptr && wanted != nullptr, "room_scale_two_shipped_rooms_with_different_scales_exist");
    if (from == nullptr || wanted == nullptr) {
        return;
    }
    audio.setListenerRoom(from->room);
    audioTicks(audio, 150);
    const float baseline = amb.ambientScale();
    g.note(concat("korr_m33aa rooms=", rooms.size(), " from=", from->room, " scale=", fx(from->ambientScale, 3),
                  " to=", wanted->room, " scale=", fx(wanted->ambientScale, 3), " baseline=", fx(baseline, 3)));
    g.check(std::fabs(baseline - from->ambientScale) < 1e-4f, "room_scale_first_room_scale_applied");
    const int voice = amb.ambientVoice();
    g.check(voice != 0 && mixer.voiceActive(voice), "room_scale_ambient_loop_live");
    const kotor::AreaAudio& area = audio.areaAudio();
    const float level0 = static_cast<float>(audio.priorityGroup(AudioSystem::kGroupLoopAreaWide).volume) / 127.0f
        * static_cast<float>(amb.isDay() ? area.ambientDayVolume : area.ambientNightVolume) / 127.0f;
    mixer.setVoiceCursor(voice, 0.0);
    Meter beforeScale;
    mixBlock(mixer, 4800, &beforeScale);

    const float target = wanted->ambientScale;
    audio.setListenerRoom(wanted->room);
    audioTicks(audio, 10);
    const float early = amb.ambientScale();
    const float expectedEarly = baseline < target ? std::min(target, baseline + 0.1f) : std::max(target, baseline - 0.1f);
    g.note(concat("room scale after 0.1 s=", fx(early, 3), " expected=", fx(expectedEarly, 3)));
    g.check(std::fabs(early - expectedEarly) < 0.02f, "room_scale_ramps_at_1_0_per_second");
    audioTicks(audio, static_cast<int>(std::fabs(target - baseline) * 100.0f) + 30);
    g.check(std::fabs(amb.ambientScale() - target) < 1e-4f
                && std::fabs(mixer.voiceVolume(voice) - level0 * target) < 1e-5f,
            "room_scale_settles_and_the_voice_level_is_volume_over_127_times_scale");
    mixer.setVoiceCursor(voice, 0.0);
    Meter afterScale;
    mixBlock(mixer, 4800, &afterScale);
    const float renderedRatio = beforeScale.rms(Bus::Ambient) > 0.0f
        ? afterScale.rms(Bus::Ambient) / beforeScale.rms(Bus::Ambient) : 0.0f;
    const float expectedRatio = baseline > 0.0f ? target / baseline : 0.0f;
    g.note(concat("room scale rendered ratio=", fx(renderedRatio, 4), " expected=", fx(expectedRatio, 4)));
    g.check(beforeScale.rms(Bus::Ambient) > 0.0f && std::fabs(renderedRatio - expectedRatio) < 0.01f,
            "room_scale_ambient_bus_rms_follows_the_scale");
    g.refute(std::fabs(renderedRatio - 1.0f) < 0.005f, "room_scale_ignored");
}

} // namespace

int runAmbienceFamilyTest(GameSession& game)
{
    Gate g;
    // The gate must be able to fail: a probe gate fed a false statement has to report it.
    {
        Gate probe;
        probe.quiet = true;
        probe.check(false, "probe");
        g.check(!probe.ok && probe.failures == 1, "gate_detects_a_failing_check");
        Gate negative;
        negative.quiet = true;
        negative.refute(true, "probe");
        g.check(!negative.ok, "gate_detects_a_true_wrong_claim");
    }
    if (game.module() != "end_m01aa") {
        g.note("the gate expects --module end_m01aa");
        return 70;
    }
    const Pool pool = buildPool(game);
    g.note(concat("sound pool=", pool.names.size(), " shortest=", pool.names.empty() ? std::string("-") : pool.names[pool.shortest]));
    g.check(pool.names.size() >= 6, "pool_of_six_shipped_sounds");
    if (pool.names.size() < 6) {
        logInfo("ambience: FAIL");
        return 70;
    }
    const MusicRows rows = readMusicRows(game);
    g.check(rows.table.has_value(), "ambientmusic_2da_read");

    float masterFactor = 0.0f;
    sectionLoad(g, game, masterFactor);
    sectionUserVolumes(g, game);
    sectionMovieMode(g, game, pool, masterFactor);
    sectionVolumeScale(g, game, pool);
    sectionPositional(g, game, pool);
    sectionSequencing(g, game, pool);
    sectionVariance(g, game, pool);
    sectionTimes(g, game, pool);
    sectionBudget(g, game, pool);
    sectionScripts(g, game, pool);
    sectionMusicLayers(g, game, rows);
    sectionTaris(g, game, rows);
    sectionRoomScale(g, game);

    logInfo(concat("ambience: checks=", g.checks, " failures=", g.failures));
    logInfo(g.ok ? "ambience: PASS" : "ambience: FAIL");
    return g.ok ? 0 : 71;
}

} // namespace ke
