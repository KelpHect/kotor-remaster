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
};

struct OriginalSave {
    std::filesystem::path folder;
    std::string module;
    std::string area;
    std::string areaName;
    std::string saveName = "Engine Save";
    std::string pcName = "Player";
    std::string portrait = "po_pmhc4";
    Vec3 position;
    float yaw = 0.0f;
    std::int32_t gold = 0;
    std::int32_t xp = 0;
    std::int32_t hp = 1;
    std::int32_t maxHp = 1;
    std::int32_t level = 1;
    std::int32_t classId = 0;
    std::int32_t alignment = 50;
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
    std::vector<SavedInventoryItem> inventory;
    std::vector<std::pair<std::int32_t, bool>> party; // member id, leader
    std::vector<std::string> partyTags;
    std::map<std::string, std::int32_t> journal;
    std::vector<SavedGlobal> globals;
    std::vector<SavedObjectState> objects;
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
    // Nested module ERFs for previously visited modules. The current module is
    // rebuilt from live state; the rest are opaque retail state and are kept
    // verbatim so revisiting a module does not lose its saved GIT/ARE.
    std::vector<SavedResource> preservedModules;
};

// Parse a save folder. Throws FormatError with a precise reason when the
// container/GFF layout is invalid. Never writes to the source folder.
OriginalSave readOriginalSave(const std::filesystem::path& folder);

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

} // namespace ke::kotor
