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
    std::map<std::string, std::int32_t> skills;
    std::set<std::int32_t> feats;
    std::set<std::int32_t> powers;
    std::int32_t skillPoints = 0;
    std::int32_t featPoints = 0;
    std::vector<InventoryItem> inventory;
};

struct AttackResult {
    std::int32_t roll = 0;
    std::int32_t total = 0;
    bool hit = false;
    bool critical = false;
    std::int32_t damage = 0;
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
    std::vector<MerchantStockItem> stock;
};

struct EncounterState {
    bool spawned = false;
    std::int32_t spawnedCount = 0;
    std::vector<std::size_t> entities;
};

struct CompanionState {
    std::string tag;
    CharacterSheet sheet;
    bool active = true;
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
    std::int32_t maxHitPoints(std::int32_t classId, std::int32_t level,
                              std::int32_t constitution) const;
    std::int32_t skillPointsForLevel(std::int32_t classId, std::int32_t intelligence) const;
    std::int32_t featsForLevel(std::int32_t classId, std::int32_t level) const;
    bool canTakeFeat(const CharacterSheet& sheet, std::int32_t featId) const;

    int equipSlotFor(std::int32_t baseItem) const;
    int itemCost(std::int32_t baseItem) const;
    InventoryItem makeItem(const std::string& resref, std::int32_t baseItem) const;
    bool canEquip(const CharacterSheet& sheet, const InventoryItem& item) const;

    std::int32_t armorClass(const CharacterSheet& sheet) const;
    std::int32_t attackBonus(const CharacterSheet& sheet) const;
    std::int32_t damageBonus(const CharacterSheet& sheet) const;
    std::int32_t initiativeBonus(const CharacterSheet& sheet) const;
    AttackResult resolveAttack(const CharacterSheet& attacker, std::int32_t targetAc,
                               std::uint32_t& rng, std::int32_t damageDice,
                               std::int32_t damageDie, std::int32_t damageBonus) const;
    void weaponDamage(const CharacterSheet& sheet, std::int32_t& dice, std::int32_t& die,
                      std::int32_t& bonus) const;

    std::int32_t classHitDie(std::int32_t classId) const;
    std::string className(std::int32_t classId) const;
    std::string classConstant(std::int32_t classId) const;

private:
    std::int32_t cellInt(const kotor::TwoDa& table, const std::string& row,
                         const std::string& column, std::int32_t fallback = 0) const;

    bool loaded_ = false;
    std::string error_;
    std::optional<kotor::TwoDa> classes_;
    std::optional<kotor::TwoDa> exptable_;
    std::optional<kotor::TwoDa> skills_;
    std::optional<kotor::TwoDa> feat_;
    std::optional<kotor::TwoDa> baseitems_;
    std::optional<kotor::TwoDa> featgain_;
    std::map<std::string, kotor::TwoDa> attackTables_;
    std::map<std::string, kotor::TwoDa> saveTables_;
    std::map<std::string, kotor::TwoDa> skillTables_;
};

} // namespace ke
