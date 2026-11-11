// The audio script actions and the per-frame glue of the area ambience (game/Ambience.h).
// Retail handler bodies: SoundObject* 00543aa0..00543e60 call the server sound object
// (005c8bd0..005c8df0), which forwards to the client sound object; MusicBackground* and
// MusicBattle* (0053ffb0, 005400c0) call the area's music object; the volume setters
// clamp to 0..127 (005c8df0, 005d5950, 005d5b50).
#include "game/ScriptHost.h"

#include "game/Ambience.h"
#include "game/AudioSystem.h"
#include "game/GameSession.h"

#include <algorithm>

namespace ke {

namespace {

using Cells = std::optional<std::vector<script::Value>>;

constexpr std::size_t kNoEntity = static_cast<std::size_t>(-1);

// The audio system's area state is the live copy; scripts read the scene's copy back
// (GetMusicBackground*Track and the existing gates), so it follows every change.
void syncScene(ActionCall& call, AudioSystem* audio)
{
    if (audio != nullptr) {
        call.api().scene().areaAudio = audio->areaAudio();
    }
}

bool soundEntity(ActionCall& call, std::uint32_t& entity)
{
    const std::size_t index = call.api().entityFor(call.objectArgOrInvalid(0));
    if (index == kNoEntity) {
        return false;
    }
    entity = static_cast<std::uint32_t>(index);
    return true;
}

// ACT_413 SoundObjectPlay 00543be0 -> 005c8bd0.
Cells soundObjectPlay(ActionCall& call)
{
    std::uint32_t entity = 0;
    if (AudioSystem* audio = call.api().session.audioOrNull(); audio != nullptr && soundEntity(call, entity)) {
        audio->ambience().playSoundObject(entity);
    }
    return ActionCall::none();
}

// ACT_414 SoundObjectStop 00543e10 -> 005c8c70.
Cells soundObjectStop(ActionCall& call)
{
    std::uint32_t entity = 0;
    if (AudioSystem* audio = call.api().session.audioOrNull(); audio != nullptr && soundEntity(call, entity)) {
        audio->ambience().stopSoundObject(entity);
    }
    return ActionCall::none();
}

// ACT_415 SoundObjectSetVolume 00543da0 -> 005c8df0: 0..127, not a percentage.
Cells soundObjectSetVolume(ActionCall& call)
{
    std::uint32_t entity = 0;
    if (AudioSystem* audio = call.api().session.audioOrNull(); audio != nullptr && soundEntity(call, entity)) {
        audio->ambience().setSoundObjectVolume127(entity, static_cast<int>(call.intArg(1)));
    }
    return ActionCall::none();
}

// ACT_416 SoundObjectSetPosition 00543d10 -> 005c8e50.
Cells soundObjectSetPosition(ActionCall& call)
{
    std::uint32_t entity = 0;
    if (AudioSystem* audio = call.api().session.audioOrNull(); audio != nullptr && soundEntity(call, entity)) {
        audio->ambience().setSoundObjectPosition(entity, call.vectorArg(1));
    }
    return ActionCall::none();
}

// ACT_745 SoundObjectFadeAndStop 00543e60.
Cells soundObjectFadeAndStop(ActionCall& call)
{
    std::uint32_t entity = 0;
    if (AudioSystem* audio = call.api().session.audioOrNull(); audio != nullptr && soundEntity(call, entity)) {
        audio->ambience().fadeAndStopSoundObject(entity, static_cast<float>(call.floatArg(1)));
    }
    return ActionCall::none();
}

// ACT_124 SoundObjectSetFixedVariance 00543c30 -> 005c8da0.
Cells soundObjectSetFixedVariance(ActionCall& call)
{
    std::uint32_t entity = 0;
    if (AudioSystem* audio = call.api().session.audioOrNull(); audio != nullptr && soundEntity(call, entity)) {
        audio->ambience().setSoundObjectFixedVariance(entity, static_cast<float>(call.floatArg(1)));
    }
    return ActionCall::none();
}

// ACT_188 SoundObjectGetFixedVariance 00543aa0 reads sound object +0x248.
Cells soundObjectGetFixedVariance(ActionCall& call)
{
    std::uint32_t entity = 0;
    AudioSystem* audio = call.api().session.audioOrNull();
    if (audio == nullptr || !soundEntity(call, entity)) {
        return ActionCall::retFloat(0.0);
    }
    return ActionCall::retFloat(audio->ambience().soundObjectFixedVariance(entity));
}

// ACT_690 SoundObjectSetPitchVariance 00543ca0 -> 005c8d50.
Cells soundObjectSetPitchVariance(ActionCall& call)
{
    std::uint32_t entity = 0;
    if (AudioSystem* audio = call.api().session.audioOrNull(); audio != nullptr && soundEntity(call, entity)) {
        audio->ambience().setSoundObjectPitchVariance(entity, static_cast<float>(call.floatArg(1)));
    }
    return ActionCall::none();
}

// ACT_689 SoundObjectGetPitchVariance 00543b10 reads sound object +0x240.
Cells soundObjectGetPitchVariance(ActionCall& call)
{
    std::uint32_t entity = 0;
    AudioSystem* audio = call.api().session.audioOrNull();
    if (audio == nullptr || !soundEntity(call, entity)) {
        return ActionCall::retFloat(0.0);
    }
    return ActionCall::retFloat(audio->ambience().soundObjectPitchVariance(entity));
}

// ACT_691 SoundObjectGetVolume 00543b80 reads sound object +0x234.
Cells soundObjectGetVolume(ActionCall& call)
{
    std::uint32_t entity = 0;
    AudioSystem* audio = call.api().session.audioOrNull();
    if (audio == nullptr || !soundEntity(call, entity)) {
        return ActionCall::retInt(0);
    }
    return ActionCall::retInt(audio->ambience().soundObjectVolume127(entity));
}

// ACT_425/426 MusicBackgroundPlay / Stop 0053ffb0.
Cells musicBackgroundPlay(ActionCall& call)
{
    if (AudioSystem* audio = call.api().session.audioOrNull()) {
        audio->ambience().setAreaMusicEnabled(true);
    }
    return ActionCall::none();
}

Cells musicBackgroundStop(ActionCall& call)
{
    if (AudioSystem* audio = call.api().session.audioOrNull()) {
        audio->ambience().setAreaMusicEnabled(false);
    }
    return ActionCall::none();
}

// ACT_427 MusicBackgroundSetDelay (milliseconds, like the GIT MusicDelay).
Cells musicBackgroundSetDelay(ActionCall& call)
{
    if (AudioSystem* audio = call.api().session.audioOrNull()) {
        audio->ambience().setMusicDelay(static_cast<int>(call.intArg(1)));
        syncScene(call, audio);
    }
    return ActionCall::none();
}

Cells changeMusic(ActionCall& call, MusicLayer layer, bool night)
{
    if (AudioSystem* audio = call.api().session.audioOrNull()) {
        audio->ambience().changeMusicTrack(layer, night, static_cast<std::int32_t>(call.intArg(1)));
        syncScene(call, audio);
    }
    return ActionCall::none();
}

Cells musicBackgroundChangeDay(ActionCall& call)
{
    return changeMusic(call, MusicLayer::Area, false);
}

Cells musicBackgroundChangeNight(ActionCall& call)
{
    return changeMusic(call, MusicLayer::Area, true);
}

// ACT_432 MusicBattleChange 005400c0 (vtable +0x14 of the area music object).
Cells musicBattleChange(ActionCall& call)
{
    return changeMusic(call, MusicLayer::Battle, false);
}

// ACT_430/431 MusicBattlePlay / Stop 005400c0. presentation.battleMusic is kept for the
// world-state gate.
Cells musicBattlePlay(ActionCall& call)
{
    call.api().sim().presentation.battleMusic = true;
    if (AudioSystem* audio = call.api().session.audioOrNull()) {
        audio->ambience().setBattle(true);
    }
    return ActionCall::none();
}

Cells musicBattleStop(ActionCall& call)
{
    call.api().sim().presentation.battleMusic = false;
    if (AudioSystem* audio = call.api().session.audioOrNull()) {
        audio->ambience().setBattle(false);
    }
    return ActionCall::none();
}

// ACT_433/434 AmbientSoundPlay / Stop 0052e270.
Cells ambientSoundPlay(ActionCall& call)
{
    if (AudioSystem* audio = call.api().session.audioOrNull()) {
        audio->ambience().setAmbientEnabled(true);
    }
    return ActionCall::none();
}

Cells ambientSoundStop(ActionCall& call)
{
    if (AudioSystem* audio = call.api().session.audioOrNull()) {
        audio->ambience().setAmbientEnabled(false);
    }
    return ActionCall::none();
}

Cells changeAmbient(ActionCall& call, bool night)
{
    if (AudioSystem* audio = call.api().session.audioOrNull()) {
        audio->ambience().changeAmbientTrack(night, static_cast<std::int32_t>(call.intArg(1)));
        syncScene(call, audio);
    }
    return ActionCall::none();
}

Cells ambientSoundChangeDay(ActionCall& call)
{
    return changeAmbient(call, false);
}

Cells ambientSoundChangeNight(ActionCall& call)
{
    return changeAmbient(call, true);
}

// ACT_567/568 AmbientSoundSetDay/NightVolume: 0..127, the byte the area stores (the
// client passes it straight to the stream volume setter, 0068d530 -> 005d5b50).
Cells setAmbientVolume(ActionCall& call, bool night)
{
    if (AudioSystem* audio = call.api().session.audioOrNull()) {
        audio->ambience().setAmbientVolume(night, static_cast<std::int32_t>(call.intArg(1)));
        syncScene(call, audio);
    }
    return ActionCall::none();
}

Cells ambientSoundSetDayVolume(ActionCall& call)
{
    return setAmbientVolume(call, false);
}

Cells ambientSoundSetNightVolume(ActionCall& call)
{
    return setAmbientVolume(call, true);
}

Cells areaTrack(ActionCall& call, std::int32_t kotor::AreaAudio::*field)
{
    if (AudioSystem* audio = call.api().session.audioOrNull()) {
        return ActionCall::retInt(audio->areaAudio().*field);
    }
    return ActionCall::retInt(call.api().scene().areaAudio.*field);
}

Cells musicBackgroundGetDayTrack(ActionCall& call)
{
    return areaTrack(call, &kotor::AreaAudio::musicDayId);
}

Cells musicBackgroundGetNightTrack(ActionCall& call)
{
    return areaTrack(call, &kotor::AreaAudio::musicNightId);
}

Cells musicBackgroundGetBattleTrack(ActionCall& call)
{
    return areaTrack(call, &kotor::AreaAudio::musicBattleId);
}

// ACT_765 SetMusicVolume 00547ef0: a multiplier on the player's music setting.
Cells setMusicVolume(ActionCall& call)
{
    const float volume = static_cast<float>(call.floatArg(0, 1.0));
    call.api().sim().presentation.musicVolume = volume;
    if (AudioSystem* audio = call.api().session.audioOrNull()) {
        audio->setMusicScriptVolume(volume);
    }
    return ActionCall::none();
}

} // namespace

void registerAmbienceActions(HandlerTable& table)
{
    table.add(124, soundObjectSetFixedVariance);
    table.add(188, soundObjectGetFixedVariance);
    table.add(413, soundObjectPlay);
    table.add(414, soundObjectStop);
    table.add(415, soundObjectSetVolume);
    table.add(416, soundObjectSetPosition);
    table.add(425, musicBackgroundPlay);
    table.add(426, musicBackgroundStop);
    table.add(427, musicBackgroundSetDelay);
    table.add(428, musicBackgroundChangeDay);
    table.add(429, musicBackgroundChangeNight);
    table.add(430, musicBattlePlay);
    table.add(431, musicBattleStop);
    table.add(432, musicBattleChange);
    table.add(433, ambientSoundPlay);
    table.add(434, ambientSoundStop);
    table.add(435, ambientSoundChangeDay);
    table.add(436, ambientSoundChangeNight);
    table.add(558, musicBackgroundGetDayTrack);
    table.add(559, musicBackgroundGetNightTrack);
    table.add(567, ambientSoundSetDayVolume);
    table.add(568, ambientSoundSetNightVolume);
    table.add(569, musicBackgroundGetBattleTrack);
    table.add(689, soundObjectGetPitchVariance);
    table.add(690, soundObjectSetPitchVariance);
    table.add(691, soundObjectGetVolume);
    table.add(745, soundObjectFadeAndStop);
    table.add(765, setMusicVolume);
}

void tickAmbience(GameSession& session, float dt)
{
    AudioSystem* audio = session.audioOrNull();
    if (audio == nullptr) {
        return;
    }
    std::int32_t hour = 12;
    const std::int32_t state = gameDayState(session, hour);
    audio->setTimeOfDay(state, hour);
    ScriptApi api{session};
    // Battle music follows the excited timers of the creatures (004f3580 / 004ed110): a creature that
    // enters combat is excited for ExcitedDuration (10 s) and the music starts when an enemy is within 30 m.
    const kotor::Scene& scene = session.scene();
    const std::size_t none = static_cast<std::size_t>(-1);
    const auto bodyObject = [&](std::size_t index) {
        // The PC's body is the avatar entity, which has no script object of its own.
        return index == session.playerEntity() ? api.pc() : session.objectForEntityIndex(index);
    };
    const auto tick = [&](std::size_t index, std::uint32_t object, const kotor::Vec3& at) {
        const ActorCombat& combat = api.actor(object).combat;
        bool enemyNear = false;
        if (combat.inCombat) {
            for (std::size_t other = 0; other < scene.entities.size() && !enemyNear; ++other) {
                const kotor::Entity& candidate = scene.entities[other];
                if (other == index || candidate.kind != kotor::EntityKind::Creature || candidate.dead) {
                    continue;
                }
                const float dx = candidate.position.x - at.x;
                const float dy = candidate.position.y - at.y;
                const float dz = candidate.position.z - at.z;
                if (dx * dx + dy * dy + dz * dz > 900.0f) {
                    continue;
                }
                const std::uint32_t otherObject = bodyObject(other);
                enemyNear = otherObject != script::kObjectInvalid && isEnemy(api, object, otherObject);
            }
        }
        audio->ambience().exciteTick(object, combat.inCombat, combat.combatTimer, enemyNear, dt);
    };
    // Only the creature the player controls is excited into battle music: retail 004f3580 starts it only when
    // 004ae910 (004b2a50, a search of the client's player list for the object id) finds the creature, so two
    // creatures fighting each other leave the area music alone (the retail Endar trace plays no battle music
    // through its opening duels).
    const std::uint32_t player = session.controlledObject();
    bool pcTicked = false;
    for (std::size_t index = 0; index < scene.entities.size(); ++index) {
        const kotor::Entity& entity = scene.entities[index];
        if (entity.kind != kotor::EntityKind::Creature || entity.dead) {
            continue;
        }
        const std::uint32_t object = bodyObject(index);
        if (object == script::kObjectInvalid || object != player) {
            continue;
        }
        pcTicked = true;
        tick(index, object, entity.position);
    }
    // A session without an avatar entity (no third-person body) still has a PC with a combat state.
    if (!pcTicked && player != script::kObjectInvalid) {
        tick(none, player, session.controlledPosition());
    }
    audio->update(dt);
}

} // namespace ke
