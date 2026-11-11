#pragma once

// Gameplay sound effects and visual effects (visualeffects.2da). Everything here is data
// driven from the install: visualeffects/spells/weaponsounds/baseitems/ammunitiontypes/
// placeableobjsnds/genericdoors/placeables/appearance/appearancesndset/
// prioritygroups/footstepsounds/surfacemat 2DAs and the animation event tables inside shipped MDLs.
// Evidence addresses (swkotor.exe 1.0.3.0) are in Effects.cpp; anything inferred is
// labelled there. Nobody has listened to the sounds: the gate (--family-test sfx) proves
// them at the mixer (which voice, which resref, which bus, gain and pan by distance).

#include "audio/Audio.h"
#include "kotor/Formats.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ke {

class GameSession;

// One visualeffects.2da row, columns by name (retail loader 006a1880 reads the same set).
struct VisualEffectRow {
    std::int32_t id = 0; // the row label, i.e. the VFX_* constant
    std::string label;
    char typeFd = 'F';   // Type_FD: F fire-and-forget, D duration
    std::string headModel;                                // Imp_HeadCon_Node -> node "headconjure"
    std::string impactModel;                              // Imp_Impact_Node  -> node "impact"
    std::string rootModel[4];                             // Imp_Root_{S,M,L,H}_Node -> ground
    std::string cessationHead, cessationImpact, cessationRoot[4];
    std::int32_t progImpact = 0, progDuration = 0, progCessation = 0; // hard-coded ProgFX ids
    std::string soundImpact, soundDuration, soundCessation;
    std::int32_t shakeType = 0;
    float shakeDelay = 0.0f, shakeDuration = 0.0f;
};

enum class FxPhase { Impact, Duration, Cessation, Done };

// One effect model playing on (or at) something.
struct FxInstance {
    std::uint32_t id = 0;                 // unique, never reused
    std::int32_t vfx = 0;                 // visualeffects.2da row label (or -1: spell/cast model)
    std::string model;                    // effect MDL resref
    std::string node;                     // target node the model hangs on: impact, headconjure, handconjure, root
    std::size_t target = static_cast<std::size_t>(-1); // scene entity index, or npos for a place
    std::uint32_t targetObject = 0;       // script object of the target (0: none)
    std::int64_t ownerEffect = 0;         // EffectData id that keeps a duration effect alive (0: none)
    kotor::Vec3 place;                    // used when the target is not an entity
    float placeBearing = 0.0f;
    kotor::Vec3 position;                 // last world placement of the model origin
    kotor::Mat3 orientation{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    std::shared_ptr<kotor::Mdl> mdl;
    char typeFd = 'F';
    FxPhase phase = FxPhase::Impact;
    float time = 0.0f;                    // inside the current phase's clip
    float age = 0.0f;
    float hold = 0.0f;                    // seconds left of a temporary duration effect
    bool permanent = false;
    bool released = false;
    std::string clip;                     // the clip being sampled
    std::size_t slot = static_cast<std::size_t>(-1);  // scene.entityMeshes index of the baked geometry
    std::size_t emitters = 0;             // emitters registered for this instance
    int durationVoice = 0;                // looping spellduration voice
    std::uint32_t tag = 0;                // EmitterInstance::entity tag of this instance
    std::size_t beamTarget = static_cast<std::size_t>(-1); // EffectBeam: where its bolts end
};

// A gameplay sound that was started, with the mixer's view of it right after the start.
struct SfxRecord {
    std::string event;    // what caused it: footstep, swing, hit, parry, clash, shot, impact, door_opened, ...
    std::string resref;
    audio::Bus bus = audio::Bus::Effects;
    int voice = 0;        // mixer voice id (0: nothing started)
    float volume = 0.0f;  // 0..1, from soundgain.2da (127 = 1)
    float minDistance = 0.0f;
    float maxDistance = 0.0f;
    bool positional = false;
    bool loop = false;
    kotor::Vec3 position;
    float gain = 0.0f;    // Mixer::voiceGain at the start (distance attenuation included)
    float pan = 0.0f;     // Mixer::voicePan at the start
    std::size_t entity = static_cast<std::size_t>(-1);
};

// What a door or placeable did (placeableobjsnds.2da columns).
enum class ObjectSound { Opened, Closed, Destroyed, Used, Locked };

class EffectDirector {
    friend int runEffectsFamilyTest(GameSession& game);

public:
    explicit EffectDirector(GameSession& session);
    ~EffectDirector();

    EffectDirector(const EffectDirector&) = delete;
    EffectDirector& operator=(const EffectDirector&) = delete;

    // ---- visual effects ----------------------------------------------------------
    // The row for a VFX_* id, or null.
    const VisualEffectRow* row(std::int32_t id);

    struct Spawn {
        std::int32_t vfx = 0;
        std::size_t target = static_cast<std::size_t>(-1); // entity index
        std::uint32_t targetObject = 0;
        kotor::Vec3 place;           // when no entity: where
        float placeBearing = 0.0f;
        bool temporary = false;      // DURATION_TYPE_TEMPORARY
        float seconds = 0.0f;
        bool permanent = false;      // DURATION_TYPE_PERMANENT
        std::int64_t ownerEffect = 0;
        bool sounds = true;
        std::size_t beamSource = static_cast<std::size_t>(-1); // EffectBeam: the entity it comes from
        std::string beamNode = "rhand";                        // and the node on it (BODY_NODE_*)
    };
    // Starts every model of the row (head, impact, root by creature size) and its sounds.
    // Returns the instance ids; empty for an unknown row.
    std::vector<std::uint32_t> spawn(const Spawn& request);
    // Starts one named effect model on a node of an entity (spell conjure models).
    std::uint32_t spawnModel(const std::string& model, const std::string& node, std::size_t target,
                             kotor::Vec3 place, float bearing);
    // Moves a duration effect into its cessation (a removed or expired effect).
    void release(std::uint32_t instance);
    void releaseOwned(std::int64_t ownerEffect);

    const std::vector<FxInstance>& instances() const { return instances_; }
    const FxInstance* instance(std::uint32_t id) const;
    std::size_t liveShells() const { return shells_.size(); }
    std::size_t liveInstances() const { return instances_.size(); }
    // Bumped each time a baked geometry slot was added to the scene: the frontend uploads
    // the scene again when it changes (slots are reused for the same model afterwards).
    std::uint32_t topologyRevision() const { return topology_; }

    // Screen shake started by a row (visualeffects shaketype/shakedelay/shakeduration).
    struct Shake {
        std::int32_t type = 0;
        float delay = 0.0f, duration = 0.0f, elapsed = 0.0f;
        bool active = false;
    };
    const Shake& shake() const { return shake_; }

    // ---- hard-coded camera and frame programs (retail 006a54d0 ranges) ------------
    // ProgFX 1500 (ApplyFOV 006a4580, UpdateFOV 006a4e70, RemoveFOV 006a49a0): the follow camera's
    // view angle while the program drives it; `dolly` keeps the subject's size (stick +0x84).
    struct CameraZoom {
        float viewAngle = 55.0f;
        bool dolly = false;
    };
    std::optional<CameraZoom> cameraZoom() const;
    // ProgFX 1601/1602 (006a14e0, 006a5920, 006a1570): AurEnableSpeedBlur's flag (007a6880) and
    // AurSetSpeedBlurRatio's ratio (0078d43c, 0.98 before anything sets it).
    bool speedBlurOn() const { return speedBlurOn_; }
    float speedBlurRatio() const { return speedBlurRatio_; }
    // ProgFX 1800 (006a15d0 -> CSWCObject::StartFlash 0063d540): entities whose models carry the
    // flash byte (Gob +0x17c, 00449af0) and are drawn through the distortion pass only.
    const std::vector<std::uint32_t>& distortionEntities() const { return distortion_; }
    // The ring texture's phase for this frame (RingDistortTextureController +0x3c, 00460a90).
    float distortionPhase() const { return distortionPhaseShown_; }

    // ---- per-frame --------------------------------------------------------------
    void update(float dt);
    // The scene was replaced (module load): drop instances, slots and pending sounds.
    void resetScene();

    // ---- sound log (mixer-level proof) -------------------------------------------
    const std::vector<SfxRecord>& log() const { return log_; }
    void clearLog() { log_.clear(); }
    void setLogging(bool on) { logging_ = on; }

    // ---- animation events ---------------------------------------------------------
    // `entity` played `clip` from `from` to `to` seconds (to < from: the clip wrapped).
    // Fires the MDL event table entries inside the interval.
    void animationAdvanced(std::size_t entity, const std::shared_ptr<kotor::Mdl>& model,
                           const std::string& clip, float from, float to, float duration, bool looped);
    // The creature's current animation, every frame whether or not its room is drawn: retail
    // CSWCCreature::UpdateRollingFootstepSound 00610840 (called from the creature update at 0061b35c) follows the
    // animation id, not the pose, so a droid in a room nobody sees still rolls.
    void creatureClip(std::size_t entity, const std::string& clip) { rollingSound(entity, clip); }
    // True when the clip `entity` is playing carries the named MDL event (any case).
    bool clipCarries(std::size_t entity, const char* eventName) const;
    // Fires a named MDL event for an entity (what animationAdvanced calls).
    bool fireEvent(std::size_t entity, const std::string& eventName);

    // ---- sounds ------------------------------------------------------------------
    // Footstep of a creature on the floor under it (footstepsounds.2da by appearance
    // footsteptype and surfacemat). Returns the voice or 0.
    int footstep(std::size_t entity);
    // Landing / fall sound (appearancesndset.2da FallDirt/Hard/Metal/Water).
    int hitGround(std::size_t entity);
    // weaponsounds.2da swing/clash/parry/hit by the attacker's weapon.
    int swing(std::size_t attacker, std::uint32_t attackerObject, int kind /*0 short 1 long 2 twirl*/);
    int clash(std::size_t attacker, std::uint32_t attackerObject);
    int parry(std::size_t attacker, std::uint32_t attackerObject);
    int hit(std::size_t attacker, std::uint32_t attackerObject, std::size_t target,
            std::uint32_t targetObject);
    // Ranged weapon: ammunitiontypes.2da shot sound at the shooter / impact sound at the target.
    int shot(std::size_t attacker, std::uint32_t attackerObject, bool power);
    int shotImpact(std::size_t attacker, std::uint32_t attackerObject, std::size_t target, bool power);
    // The resolved attack of one creature on another (ScriptCombat performAttack).
    // result: 0 hit, 1 miss, 2 critical, 3 deflected/reflected.
    void attackResolved(std::uint32_t attackerObject, std::uint32_t targetObject, int result,
                        bool offhand);
    // Lightsaber power up / hum / power down (baseitems.2da powerupsnd, poweredsnd,
    // powerdownsnd) for a creature that holds a lightsaber and starts or ends a fight.
    void saberUp(std::uint32_t object);
    void saberDown(std::uint32_t object);
    bool saberHumming(std::uint32_t object) const;
    // Doors and placeables (placeableobjsnds.2da row = the object's soundapptype).
    int objectSound(std::size_t entity, ObjectSound which);
    // A Force power starts to be cast: spells.2da castsound at the caster and the
    // casthandvisual model on its handconjure node.
    void spellCast(std::size_t caster, std::int32_t spell);
    // Creature voice slot (SSF) by variant range: first..first+count-1, one picked at random.
    bool voice(std::size_t entity, int firstSlot, int count, const char* event);
    // Creature sound-set sound with the retail send rules. Direct: played at once (select 005f4d60,
    // BarkString, stealth 00613930). Broadcast: the server's BroadcastVoiceChat (004f2520) only reaches
    // a creature within 30 m (900 sq) of the controlled creature (death 004e0ac0, critical hit
    // 005bb440, poison 004db320). AttackGrunt: AddFireForgetAnimation (006183d0) plays slot 9..11
    // (enum 10..12) for the controlled creature always and for the others one time in five. Pain:
    // HitEvent (00617eb0), same one-in-five rule, slot 12 or 13. BattleCry: SetCombatState (004f2610)
    // on entering combat, 30 percent, one of slots 0..4 (004ec8e0).
    enum class VoiceKind { Direct, Broadcast, AttackGrunt, Pain, BattleCry };
    bool speak(std::size_t entity, int slot, VoiceKind kind);
    // PriorityGroups.2da level (0..1) and near/far distances of the group an effect category plays in.
    void gain(const std::string& category, float& volume, float& minDistance, float& maxDistance);
    // One-shot (or loop) through the category's priority group; logs and returns the voice.
    int emit(const char* event, const std::string& resref, const std::string& category,
             const kotor::Vec3* position, std::size_t entity = static_cast<std::size_t>(-1),
             bool loop = false, float volumeScale = 1.0f);

    // Pseudo-random choice shared by the sound variants (rand() % n in retail).
    std::uint32_t pick(std::uint32_t n);
    void seed(std::uint32_t value) { rng_ = value == 0 ? 1u : value; }

    // The surfacemat.2da row under (x, y) at height z, or -1.
    int surfaceAt(kotor::Vec3 at) const;
    // Creature position and bearing from an entity index (the PC avatar included).
    bool entityPlacement(std::size_t entity, kotor::Vec3& position, float& bearing) const;

    // Table access for the gate.
    std::string cell2(const char* table, const std::string& row, const std::string& column);
    // The baseitems.2da row the creature holds in the main hand (-1: unarmed).
    std::int32_t weaponBaseItem(std::uint32_t object, bool offhand) const;

private:
    struct Tables;
    struct Slot {
        std::size_t mesh = 0;
        bool inUse = false;
    };

    // A force shell (retail 006a1220): a texture over one creature for as long as its effect holds.
    struct Shell {
        std::uint32_t id = 0;
        std::size_t entity = 0;
        kotor::TextureBatch look; // the texture's own batch (its TXI grid and blending)
        std::uint32_t targetObject = 0;
        std::int64_t ownerEffect = 0;
        bool permanent = false;
        float left = 0.0f;
        // Retail keeps one shell per model (00448130 replaces the mesh's single shell texture; 004485c0
        // clears it whichever instance asks). A shell replaced or cleared by another stops showing.
        bool shown = true;
    };

    Tables& tables();
    void startProgFx(const VisualEffectRow& row, std::int32_t prog, const Spawn& request, std::vector<std::uint32_t>& ids);
    void applyShell(const Shell& shell);
    void removeShell(const Shell& shell);
    void updateShells(float dt);
    // One running hard-coded camera or frame program (1500, 1601/1602, 1800).
    struct HardFx {
        std::int32_t prog = 0;
        std::size_t entity = 0;
        std::uint32_t targetObject = 0;
        std::int64_t ownerEffect = 0;
        bool permanent = false;
        float left = 0.0f;       // seconds of a temporary effect
        bool applied = false;    // retail +0xf4
        float timerMs = 0.0f;    // retail +0x9c (1500: 4750 ms)
        bool restoring = false;  // retail +0xc7 (1500: the way back has started)
        float savedViewAngle = 55.0f; // retail +0xe4
    };
    void startHardFx(std::int32_t prog, const Spawn& request);
    void updateHardFx(float dt);
    void applyHardFx(HardFx& fx);
    void removeHardFx(HardFx& fx, bool force);
    float currentViewAngle() const;
    void beginViewAngle(float target, float seconds);
    kotor::Vec3 beamEnd(std::size_t entity) const;
    void ensureEmitters(FxInstance& fx);
    void driveEmitters(FxInstance& fx);
    void dropInstance(std::size_t index);
    void placeInstance(FxInstance& fx);
    void bakeInstance(FxInstance& fx);
    std::string phaseClip(const FxInstance& fx) const;
    std::uint32_t startModel(const VisualEffectRow* vrow, const std::string& model,
                             const std::string& node, const Spawn& request);
    int weaponRowFor(std::uint32_t attackerObject, std::size_t attackerEntity, bool offhand);
    std::string armorMaterial(std::size_t targetEntity, std::uint32_t targetObject);
    int playWeaponColumn(const char* event, int weaponRow, const std::string& column,
                         const char* category, std::size_t entity);

    GameSession& session_;
    std::unique_ptr<Tables> tables_;
    std::vector<FxInstance> instances_;
    std::vector<Shell> shells_;
    std::map<std::string, std::vector<Slot>> slots_;
    std::map<std::string, std::shared_ptr<kotor::Mdl>> models_;
    std::map<std::int32_t, VisualEffectRow> rows_;
    std::vector<SfxRecord> log_;
    std::map<std::uint32_t, int> saberVoices_;
    std::map<std::size_t, int> rollingVoices_; // looped rolling or floating droid sound per entity
    std::map<int, std::string> rollingCell_;   // footstepsounds.2da row -> its Rolling cell (cached)
    void rollingSound(std::size_t entity, const std::string& clip);
    Shake shake_;
    std::vector<HardFx> hardFx_;
    // The module camera's view angle (Camera +0x1d0) and its animation (+0x1d4, 0045bef0 / 0045d220),
    // while a 1500 program owns it.
    struct ViewAngle {
        bool driven = false;
        bool dolly = false;
        float value = 55.0f;
        bool animating = false;
        float start = 0.0f, target = 0.0f, duration = 0.0f, elapsed = 0.0f;
    } view_;
    int speedBlurCount_ = 0; // DAT_00833bcc
    bool speedBlurOn_ = false;
    float speedBlurRatio_ = 0.98f;
    std::size_t speedEntity_ = static_cast<std::size_t>(-1);
    kotor::Vec3 speedLast_;
    float playerSpeed_ = 0.0f;
    std::vector<std::uint32_t> distortion_;
    float distortionPhase_ = 0.0f;
    float distortionPhaseShown_ = 0.0f;
    std::uint32_t nextId_ = 1;
    std::uint32_t topology_ = 0;
    std::uint32_t rng_ = 0x2545F491u;
    bool logging_ = true;
};

} // namespace ke
