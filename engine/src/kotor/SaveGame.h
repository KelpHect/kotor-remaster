#pragma once

// Native original KotOR save support. A save folder contains:
//   SAVEGAME.sav      ERF MOD V1.0; one nested <module>.sav ERF per module
//   PARTYTABLE.res    PT GFF (gold, party, journal)
//   GLOBALVARS.res    GVT GFF (script globals)
//   savenfo.res       NFO GFF (module/area/name)
//   Screen.tga        screenshot
// The reader and writer below use the engine's own GFF/ERF code; the Python
// save lane remains the independent validator and byte-parity oracle.

#include "kotor/Formats.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ke::kotor {

struct SavedInventoryItem {
    std::string resref;
    std::string name;
    std::int32_t baseItem = 0;
    std::int32_t cost = 0;
    std::int32_t stack = 1;
    std::int32_t slot = -1;
    bool equipped = false;
    bool identified = true;
    std::uint32_t upgrades = 0; // the item's Upgrades bit mask (bit N = upgrade.2da row N)
    bool newItem = false;       // retail 0055ccd0 / 0055fcd0: "NewItem", bit 7 of the item's +0x288
    std::vector<ItemTemplate::Property> properties; // the item's PropertiesList, written by retail 00555790
};

// The explored bits of the save's area map: the GIT's "AreaMap" struct, written by retail 005061d0
// and read back by 00505da0 (copied only when the live map has the same size, 00578fc0). Bit
// (ResX + 1) * row + col of the 32-bit words.
struct SavedAreaMap {
    bool present = false;
    std::int32_t resX = 0;
    std::int32_t resY = 0;
    std::vector<std::uint32_t> words;
};

struct SavedGlobal {
    enum class Type { Number, Boolean, String, Location };
    Type type = Type::Number;
    std::string name;
    std::int32_t number = 0;
    bool boolean = false;
    std::string text;
    std::array<float, 6> location{};
};

struct SavedResource {
    std::string resref;
    std::uint32_t type = 0;
    std::vector<std::uint8_t> data;
};

// An object's script variables as the saved GIT keeps them (struct "SWVarTable", retail CSWVarTable): "BitArray"
// is three DWORDs of local booleans (index i is bit i % 32 of word i / 32) and "ByteArray" the eight local numbers.
// The area's own table sits at the GIT root. A save without the struct (an engine save) leaves `present` false.
struct SavedVarTable {
    bool present = false;
    std::array<std::uint32_t, 3> bits{};
    std::array<std::uint8_t, 8> bytes{};
    bool any() const
    {
        for (const std::uint32_t word : bits) {
            if (word != 0) {
                return true;
            }
        }
        for (const std::uint8_t byte : bytes) {
            if (byte != 0) {
                return true;
            }
        }
        return false;
    }
};

// One cell of a saved script stack (IFO "EventQueue" > EventData > Stack > Stack): Type is the VM cell type
// (3 integer, 4 float, 5 string, 6 object, 16 and up engine structures), Value its payload.
struct SavedStackCell {
    std::uint8_t type = 0;
    std::int64_t integer = 0;
    double real = 0.0;
    std::string text;
};

// A delayed script situation still pending at the save (retail event queue entry, DelayCommand and the like).
// day/time are the world clock (day, milliseconds into the day) at which it is due.
struct SavedScriptEvent {
    std::uint32_t day = 0;
    std::uint32_t time = 0;
    std::uint32_t objectId = 0;
    std::uint32_t callerId = 0;
    std::string script;
    std::int32_t instruction = 0; // byte offset of the closure body in the script
    std::int32_t basePointer = 0; // cells
    std::vector<SavedStackCell> stack;
};
// One saved game effect (retail CGameEffect::SaveGameEffect 00503790 / LoadGameEffect 005043a0).
struct SavedEffect {
    std::uint64_t id = 0;      // Id (64-bit): not read, Gff exposes no 64-bit value
    std::int32_t type = 0;     // the internal effect type, not the nwscript EFFECT_TYPE_ constant
    std::int32_t subType = 0;  // low 3 bits: duration type (1 temporary, 2 permanent, 3 innate); 8 magical flag
    float duration = 0.0f;
    bool skipOnLoad = false;
    std::uint32_t expireDay = 0;
    std::uint32_t expireTime = 0;
    std::uint32_t creatorId = 0;
    std::uint32_t spellId = 0xFFFFFFFFu;
    bool isExposed = false;
    std::vector<std::int32_t> ints;
    std::vector<float> floats;
    std::vector<std::string> strings;
    std::vector<std::uint32_t> objects;
};

// CSWSObject::LoadEffectList 004d1be0: an effect with SkipOnLoad set, or with duration type 3 (innate: the
// item or feat that gave it applies it again), is dropped; every other one is applied again.
bool savedEffectApplies(const SavedEffect& effect);
// CGameEffect::GetEffectTypeScriptConstant 00503a70; -1 for an internal type with no constant.
std::int32_t savedEffectScriptType(std::int32_t internalType);
// The EffectList of the GIT struct `structure` (LoadGameEffect field layout).
std::vector<SavedEffect> readSavedEffects(const Gff& gff, std::uint32_t structure);

struct SavedObjectState {
    enum class Kind { Creature, Door, Placeable, Trigger, Store, Encounter, Waypoint, Sound, Other };
    Kind kind = Kind::Other;
    std::string tag;
    std::string templateResref;
    Vec3 position;
    float bearing = 0.0f;
    std::int32_t hitPoints = 1;
    std::int32_t maxHitPoints = 1;
    bool dead = false;
    bool open = false;
    bool locked = false;
    std::vector<std::string> loot;
    // The object's script variables (retail 004d1cf0 LoadObjectState -> 0059aa80 CSWSScriptVarTable and
    // 0059b0f0 CSWVarTable): "VarTable" entries by name (type 1 int, 2 float, 3 string) and the SWVarTable
    // bit words (booleans 0..95) and bytes (numbers 0..7).
    struct Local {
        std::string name;
        std::int32_t type = 0;
        std::int32_t integer = 0;
        float number = 0.0f;
        std::string text;
    };
    std::vector<Local> locals;
    std::array<std::uint32_t, 3> localBits{};
    std::array<std::uint8_t, 8> localBytes{};
    bool localTable = false; // the SWVarTable struct was present in the GIT (an engine save without it leaves the table alone)
    std::vector<SavedEffect> effects; // EffectList (LoadEffectList 004d1be0)
    std::uint32_t objectId = 0; // the GIT "ObjectId" the saved event queue refers to
};

// One available NPC: a PARTYTABLE slot (PT_AVAIL_NPCS entry) plus the stored
// creature (AVAILNPC<n>.utc in the outer ERF). The slot index is the nwscript
// NPC_* constant, which is also the npc.2da row.
struct SavedNpc {
    bool available = false;    // PT_NPC_AVAIL
    std::int32_t select = 1;   // PT_NPC_SELECT
    bool hasCreature = false;  // an AVAILNPC<n> creature was read / is to be written
    std::string templateResref;
    std::string tag;
    std::string name;
    std::int32_t classId = 0;
    std::int32_t level = 1;
    std::int32_t xp = 0;
    std::int32_t hp = 1;
    std::int32_t maxHp = 1;
    std::int32_t baseHp = -1;  // the stored HitPoints (retail stats +0xe0); -1 derives it from maxHp
    std::int32_t forcePoints = 0;
    std::int32_t maxForcePoints = 0;
    std::int32_t str = 10;
    std::int32_t dex = 10;
    std::int32_t con = 10;
    std::int32_t intel = 10;
    std::int32_t wis = 10;
    std::int32_t cha = 10;
    std::int32_t alignment = 50;
    std::int32_t joiningXp = 0;
    std::vector<std::int32_t> feats;
    std::vector<std::int32_t> powers;
    std::vector<std::int32_t> skills;
    std::vector<SavedInventoryItem> inventory; // worn items
};

constexpr int kSavedNpcCount = 9;

struct OriginalSave {
    std::filesystem::path folder;
    std::string module;
    std::string area;
    std::string areaName;
    std::string saveName = "Engine Save";
    std::string pcName = "Player";
    std::string portrait = "po_pmhc4";
    std::string partyPortrait1; // savenfo PORTRAIT1 / PORTRAIT2: the other party members, if any
    std::string partyPortrait2;
    Vec3 position;
    float yaw = 0.0f;
    std::int32_t gold = 0;
    // GLOBALVARS binary field sizes as read (retail 005299b0 writes count/8 + 1, count, 0x960).
    std::size_t globalBooleanBytes = 0;
    std::size_t globalNumberBytes = 0;
    std::size_t globalLocationBytes = 0;
    std::int32_t xp = 0;
    std::int32_t hp = 1;
    std::int32_t maxHp = 1;
    std::int32_t level = 1;
    std::int32_t classId = 0;
    std::int32_t alignment = 50;
    std::int32_t gender = 0;      // player node "Gender": 0 male, 1 female
    std::int32_t portraitId = -1; // player node "PortraitId" (portraits.2da row)
    std::int32_t appearance = -1; // player node "Appearance_Type" (appearance.2da row)
    std::int32_t forcePoints = 0;
    std::int32_t maxForcePoints = 0;
    std::int32_t str = 10;
    std::int32_t dex = 10;
    std::int32_t con = 10;
    std::int32_t intel = 10;
    std::int32_t wis = 10;
    std::int32_t cha = 10;
    std::int32_t playedSeconds = 0;
    std::int32_t timePlayed = 0;
    std::vector<std::int32_t> feats;
    std::vector<std::int32_t> powers;
    std::vector<std::int32_t> skills; // SkillList order (ComputerUse..TreatInjury)
    std::vector<SavedInventoryItem> inventory;
    // PT_MEMBERS: NPC ids only (the PC is not a member), and whether each leads.
    std::vector<std::pair<std::int32_t, bool>> party;
    std::array<SavedNpc, kSavedNpcCount> npcs;
    std::int32_t controlledNpc = -1; // PT_CONTROLLED_NP; -1 is the PC
    bool solo = false;               // PT_SOLOMODE
    std::int32_t xpPool = 0;         // PT_XP_POOL
    // PT_PAZAAKCARDS (18 counts; retail adds a 19th BYTE entry) and PT_PAZSIDELIST (10 kinds, -1 empty);
    // ClearTable 00563200 defaults: +1..+5 twice each.
    std::array<std::int32_t, 18> pazaakCards{2, 2, 2, 2, 2};
    std::array<std::int32_t, 10> pazaakSideList{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    // PT_TUT_WND_SHOWN: the tutorial windows already shown, 6 bytes from the client's +0xba8 (written
    // at 00564eaa, read at 005661df); row r is bit (r & 7) of byte r >> 3 (0062f420).
    std::array<std::uint8_t, 6> tutorialShown{};
    std::vector<std::string> partyTags;
    std::map<std::string, std::int32_t> journal;
    std::vector<SavedGlobal> globals;
    std::vector<SavedObjectState> objects;
    SavedVarTable areaVars;
    // World clock at the save (IFO Mod_PauseDay/Mod_PauseTime, milliseconds into the day) and the delayed
    // script situations pending then, with the object ids they name.
    std::uint32_t pauseDay = 0;
    std::uint32_t pauseTime = 0;
    std::uint32_t areaObjectId = 0;
    std::uint32_t playerObjectId = 0;
    std::vector<SavedScriptEvent> events;
    std::vector<std::uint8_t> gitBytes; // the module GIT as the save holds it (every object whole)
    SavedAreaMap areaMap;
    // Saved GITs are flattened snapshots: the writer removes UseTemplates and
    // readers must not re-template GIT objects.
    bool hasUseTemplates = false;
    bool useTemplates = false;
    std::vector<ActiveEffect> effects;
    float forceSuppressedTimer = 0.0f;
    // Raw REPUTE payload from the source slot, when present. The engine does
    // not model faction reputation yet, so preserving the parsed retail FAC
    // is safer than replacing it with an empty one on round trip.
    std::vector<std::uint8_t> repute;
    // Other outer resources that carry retail-only state (AVAILNPC*, PC,
    // embedded PARTYTABLE/GLOBALVARS). Preserved verbatim on round trip.
    std::vector<SavedResource> preservedOuter;
    // Screen.tga bytes (see encodeSaveScreenshot); empty writes the synthetic thumbnail.
    std::vector<std::uint8_t> screenTga;
    // Nested module ERFs for previously visited modules. The current module is
    // rebuilt from live state; the rest are opaque retail state and are kept
    // verbatim so revisiting a module does not lose its saved GIT/ARE.
    std::vector<SavedResource> preservedModules;
};

// Parse a save folder. Throws FormatError with a precise reason when the
// container/GFF layout is invalid. Never writes to the source folder.
OriginalSave readOriginalSave(const std::filesystem::path& folder,
                              const std::filesystem::path& rsvRoot = {});

// Write a complete save folder from live state and the module currently bound
// to `install`. Refuses to overwrite a non-empty folder unless `overwrite`.
bool writeOriginalSave(const std::filesystem::path& folder, const OriginalSave& save,
                       Install& install, bool overwrite = false);

// Independent structural checks used by the gate (not a substitute for the
// Python validator): every required sidecar/nested resource is present.
struct OriginalSaveCheck {
    bool ok = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

OriginalSaveCheck checkOriginalSave(const std::filesystem::path& folder);

// Small deterministic TGA used when a live screenshot is unavailable.
std::vector<std::uint8_t> makeSaveThumbnail();

// A live frame (top-down RGBA8) as the save's Screen.tga: 256x256, 24-bit uncompressed,
// top-left origin, the whole frame (HUD included) box-filtered to a square as retail's own
// saves are (docs/evidence: a retail Screen.tga is 256x256 and shows the 4:3 frame squeezed).
std::vector<std::uint8_t> encodeSaveScreenshot(const std::uint8_t* rgba, int width, int height);

} // namespace ke::kotor
