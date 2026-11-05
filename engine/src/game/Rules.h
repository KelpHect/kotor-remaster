#pragma once

// P5 character sheet and d20 rules loaded from the shipped 2DA tables. The
// formulas are engine-side and data-driven; script effects arrive through the
// action runtime, not through copied game code.

#include "kotor/Formats.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ke {

struct InventoryItem {
    std::string resref;
    std::string name;
    std::int32_t baseItem = 0;
    std::int32_t cost = 0;
    std::int32_t stack = 1;
    std::int32_t slot = -1; // baseitems.2da equipableslots bit index, -1 = carried
    bool equipped = false;
    bool identified = true;
    // The script-world item object that stands for this entry (0 until the item
    // model in ScriptItems.cpp links it). The two are one model: this row is the
    // PC-facing view, the object carries tag, plot, stolen and charges.
    std::uint32_t object = 0;
};

struct CharacterSheet {
    std::string name = "Player";
    std::int32_t classId = 0;
    std::int32_t level = 1;
    std::int32_t xp = 0;
    std::int32_t hp = 1;
    std::int32_t maxHp = 1;
    std::int32_t fp = 0;
    std::int32_t maxFp = 0;
    std::int32_t gold = 0;
    std::int32_t str = 10;
    std::int32_t dex = 10;
    std::int32_t con = 10;
    std::int32_t intel = 10;
    std::int32_t wis = 10;
    std::int32_t cha = 10;
    std::int32_t alignment = 50; // 0 dark .. 50 neutral .. 100 light
    std::map<std::string, std::int32_t> skills;
    std::set<std::int32_t> feats;
    std::set<std::int32_t> powers;
    std::int32_t skillPoints = 0;
    std::int32_t featPoints = 0;
    std::int32_t attributePoints = 0;
    std::vector<InventoryItem> inventory;
    std::vector<kotor::ActiveEffect> effects;
    float forceSuppressedTimer = 0.0f;
};

struct AttackResult {
    std::int32_t roll = 0;
    std::int32_t total = 0;
    bool hit = false;
    bool critical = false;
    std::int32_t damage = 0;
};

struct ForcePowerInfo {
    std::int32_t id = 0;
    std::string label;
    std::int32_t cost = 0;
    char alignment = '-'; // G, E or -
    std::int32_t range = 0; // metres; 0 = self/personal
};

struct MerchantStockItem {
    InventoryItem item;
    bool infinite = false;
    std::int32_t remaining = 1;
};

struct MerchantState {
    std::size_t entity = 0;
    std::string resref;
    std::string name;
    std::int32_t markUp = 100;
    std::int32_t markDown = 40;
    std::int32_t buySellFlag = 3;
    std::int32_t gold = 0;
    std::vector<MerchantStockItem> stock;
};

struct EncounterState {
    bool spawned = false;
    bool exhausted = false;
    bool triggered = false; // the PC has entered and the wave was spawned
    bool inside = false;    // PC was inside the volume on the last tick
    std::int32_t spawnedCount = 0;
    std::int32_t respawnsRemaining = 0;
    float resetTime = 0.0f;
    float resetTimer = 0.0f;
    std::vector<std::size_t> entities;
};

struct CompanionState {
    std::string tag;
    CharacterSheet sheet;
    bool active = true;
    float attackTimer = 0.0f;
};

struct PortraitInfo {
    std::string resref;
    std::uint32_t appearance = 0;
    int sex = 0;
};

// Read-only rules facade. One instance per install; tables are loaded once.
class Rules {
public:
    explicit Rules(kotor::Install& install);

    bool loaded() const { return loaded_; }
    const std::string& error() const { return error_; }

    CharacterSheet createCharacter(std::int32_t classId, std::int32_t level,
                                   std::int32_t xp = 0) const;
    std::int32_t levelForXp(std::int32_t xp) const;
    std::int32_t xpForLevel(std::int32_t level) const;
    std::int32_t baseAttackBonus(std::int32_t classId, std::int32_t level) const;
    std::int32_t savingThrow(std::int32_t classId, std::int32_t level, int which) const;
    std::int32_t savingThrow(const CharacterSheet& sheet, int which) const;
    std::int32_t maxHitPoints(std::int32_t classId, std::int32_t level,
                              std::int32_t constitution) const;
    std::int32_t skillPointsForLevel(std::int32_t classId, std::int32_t intelligence) const;
    std::int32_t featsForLevel(std::int32_t classId, std::int32_t level) const;
    bool canTakeFeat(const CharacterSheet& sheet, std::int32_t featId) const;
    bool canTakePower(const CharacterSheet& sheet, std::int32_t powerId) const;
    std::string featName(std::int32_t featId) const;
    std::string powerName(std::int32_t powerId) const;
    std::string animationName(std::int32_t animationId) const;
    bool animationLooping(std::int32_t animationId) const;
    bool animationStationary(std::int32_t animationId) const;
    std::int32_t featCount() const {
        return feat_ ? static_cast<std::int32_t>(feat_->rowCount()) : 0;
    }
    std::optional<ForcePowerInfo> forcePower(std::int32_t powerId) const;
    std::int32_t maxForcePoints(std::int32_t classId, std::int32_t level,
                                std::int32_t wisdom, std::int32_t charisma) const;
    std::int32_t forcePowerSaveDC(const CharacterSheet& sheet) const;
    std::int32_t featAttackBonus(const CharacterSheet& sheet) const;
    std::int32_t featDamageBonus(const CharacterSheet& sheet) const;
    std::int32_t twoWeaponPenalty(const CharacterSheet& sheet) const;
    std::int32_t toughnessBonus(const CharacterSheet& sheet) const;
    std::int32_t damageReduction(const CharacterSheet& sheet) const;
    std::int32_t sneakAttackDice(const CharacterSheet& sheet) const;
    std::int32_t skillBonus(const CharacterSheet& sheet, const std::string& skill) const;
    std::int32_t forceJumpBonus(const CharacterSheet& sheet) const;
    bool hasLightsaber(const CharacterSheet& sheet) const;
    std::int32_t jediDefenseBonus(const CharacterSheet& sheet) const;
    bool jediDeflects(const CharacterSheet& sheet, std::int32_t attackTotal,
                      std::int32_t deflectionRoll) const;
    bool allowsStealth(const CharacterSheet& sheet) const;
    bool hasFeatLabel(const CharacterSheet& sheet, const std::string& needle) const;

    int equipSlotFor(std::int32_t baseItem) const;
    int itemCost(std::int32_t baseItem) const;
    // baseitems.2da "stacking" (1 when the column is absent) and the
    // "equipableslots" bit mask (0 when the item cannot be equipped).
    std::int32_t maxStack(std::int32_t baseItem) const;
    std::uint32_t equipMask(std::int32_t baseItem) const;
    // Any baseitems.2da cell as an integer; `fallback` when absent or "****".
    std::int32_t baseItemInt(std::int32_t baseItem, const std::string& column,
                             std::int32_t fallback = 0) const;
    std::string baseItemString(std::int32_t baseItem, const std::string& column) const;
    InventoryItem makeItem(const std::string& resref, std::int32_t baseItem) const;
    bool canEquip(const CharacterSheet& sheet, const InventoryItem& item) const;

    std::int32_t armorClass(const CharacterSheet& sheet) const;
    std::int32_t attackBonus(const CharacterSheet& sheet) const;
    std::int32_t damageBonus(const CharacterSheet& sheet) const;
    std::int32_t initiativeBonus(const CharacterSheet& sheet) const;
    AttackResult resolveAttack(const CharacterSheet& attacker, std::int32_t targetAc,
                               std::uint32_t& rng, std::int32_t damageDice,
                               std::int32_t damageDie, std::int32_t damageBonus,
                               std::int32_t criticalThreat = 20,
                               std::int32_t attackModifier = 0) const;
    void weaponDamage(const CharacterSheet& sheet, std::int32_t& dice, std::int32_t& die,
                      std::int32_t& bonus) const;

    std::int32_t classHitDie(std::int32_t classId) const;
    std::string className(std::int32_t classId) const;
    std::string classConstant(std::int32_t classId) const;
    std::string classDescription(std::int32_t classId) const;
    std::int32_t playableClassCount() const { return 3; }
    std::int32_t portraitCount() const;
    PortraitInfo portrait(std::int32_t index) const;
    std::int32_t skillCount() const;
    std::string skillName(std::int32_t index) const;
    bool skillIsClassSkill(std::int32_t classId, std::int32_t skillIndex) const;
    bool featIsAvailable(std::int32_t classId, std::int32_t featId) const;

    // ---- creature combat (retail attack, AC and XP tables) ------------------
    // Fills entity.attackBonus, defense, saves, damage range, range and
    // stats.mainHand/offHand from the UTC sheet and the equipped items. Once only.
    void deriveCreatureCombat(kotor::Entity& entity) const;
    // acbonus.2da for one class at one level.
    std::int32_t classArmorBonus(std::int32_t classId, std::int32_t level) const;
    // Kill award before per-member NPC scaling. retail 004f19e0.
    std::int32_t killXp(std::int32_t leaderLevel, float challengeRating,
                        std::int32_t partyCount) const;
    // ranges.2da PrimaryRange (sight) / SecondaryRange (hearing). retail 005afce0.
    float perceptionRange(std::int32_t row, bool hearing) const;
    // difficultyopt.2da MULTIPLIER for the default setting row. retail 004f3830.
    float partyDamageMultiplier() const;
    // A spells.2da or feat.2da cell (`power` picks the table).
    std::int32_t talentInt(bool power, std::int32_t id, const std::string& column,
                           std::int32_t fallback = -1) const;
    std::string powerImpactScript(std::int32_t powerId) const;
    std::size_t powerCount() const { return spells_ ? spells_->rowCount() : 0; }

private:
    std::int32_t cellInt(const kotor::TwoDa& table, const std::string& row,
                         const std::string& column, std::int32_t fallback = 0) const;

    bool loaded_ = false;
    std::string error_;
    std::optional<kotor::TwoDa> classes_;
    std::optional<kotor::TwoDa> exptable_;
    std::optional<kotor::TwoDa> skills_;
    std::optional<kotor::TwoDa> feat_;
    std::optional<kotor::TwoDa> portraits_;
    std::optional<kotor::TwoDa> baseitems_;
    std::optional<kotor::TwoDa> featgain_;
    std::optional<kotor::TwoDa> spells_;
    std::optional<kotor::TwoDa> animations_;
    std::optional<kotor::TwoDa> xptable_;
    std::optional<kotor::TwoDa> npc_;
    std::optional<kotor::TwoDa> ranges_;
    std::optional<kotor::TwoDa> acbonus_;
    std::optional<kotor::TwoDa> difficultyopt_;
    std::optional<kotor::TwoDa> costTables_;
    kotor::Install* install_ = nullptr;
    // iprp_* cost tables and item templates are read on demand and never change.
    mutable std::map<std::string, std::optional<kotor::TwoDa>> propertyTables_;
    mutable std::map<std::string, std::optional<kotor::ItemTemplate>> itemTemplates_;
    std::map<std::string, kotor::TwoDa> attackTables_;
    std::map<std::string, kotor::TwoDa> saveTables_;
    std::map<std::string, kotor::TwoDa> skillTables_;
};

} // namespace ke
