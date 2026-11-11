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
    std::uint32_t upgrades = 0; // installed upgrades: bit N is upgrade.2da row N (the save's Upgrades field)
    // The script-world item object that stands for this entry (0 until the item
    // model in ScriptItems.cpp links it). The two are one model: this row is the
    // PC-facing view, the object carries tag, plot, stolen and charges.
    std::uint32_t object = 0;
};

struct CharacterSheet {
    std::string name = "Player";
    std::int32_t classId = 0;
    std::int32_t secondClassId = -1; // AddMultiClass; -1 = none.
    std::int32_t secondClassLevel = 0;
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
    std::int32_t gender = 0;     // gender.2da row: 0 male, 1 female (retail stats byte +0x31)
    std::int32_t portraitRow = -1; // portraits.2da row, -1 = unset
    std::map<std::string, std::int32_t> skills;
    std::set<std::int32_t> feats;
    std::set<std::int32_t> powers;
    std::int32_t skillPoints = 0;
    std::int32_t featPoints = 0;
    std::int32_t attributePoints = 0;
    std::int32_t powerPoints = 0; // Force powers to pick at level-up (classpowergain.2da)
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
    bool wiped = false;     // the last spawned creature died (00594310): inactive until the reset time has run
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
    int row = -1; // portraits.2da row
    std::uint32_t appearanceSmall = 0; // Appearance_S column
    std::uint32_t appearanceLarge = 0; // Appearance_L column
    // BaseResRefE, VE, VVE, VVVE: dark-side art, mildest first. retail 00647e80.
    std::string darkVariants[4];
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
    // combatanimations.2da hit1 of an attack clip in ms (retail 005b4f30 GetWeaponImpactTime, row
    // = the clip's animations.2da row), or -1 when the clip has no row or no hit.
    // 005b4f30: column subAttack+1 of the clip's combatanimations row (hit1, hit2, hit3).
    std::int32_t attackImpactMs(const std::string& clip, int subAttack = 0) const;
    bool animationLooping(std::int32_t animationId) const;
    bool animationFireAndForget(std::int32_t animationId) const;
    // Script ANIMATION_* constant to clip: action 0x540550 maps it to a server animation id and the switch at 0069f650 to an
    // animations.2da row (ids still >= 10000 are dialoganimations rows). Returns a row, or an id >= 10000, or the input unchanged.
    static std::int32_t scriptAnimationTarget(std::int32_t scriptId);
    std::string scriptAnimationName(std::int32_t scriptId) const;
    bool scriptAnimationLooping(std::int32_t scriptId) const;
    bool scriptAnimationFireAndForget(std::int32_t scriptId) const;
    bool animationStationary(std::int32_t animationId) const;
    // A DLG AnimList id that names a dialoganimations.2da row (the id itself below 1200, or
    // id - 10000), as the model animation to play ("" when the row has no name or the id is a
    // cutscene clip). The 2DA holds labels (Talk_Normal); the shipped creature models name
    // the clips tlknorm, tlkplead and so on, so the label is mapped (inferred, matched
    // against the animation names in the shipped models).
    std::string dialogAnimationName(std::int32_t animationId) const;
    bool dialogAnimationLooping(std::int32_t animationId) const;
    std::int32_t featCount() const {
        return feat_ ? static_cast<std::int32_t>(feat_->rowCount()) : 0;
    }
    std::optional<ForcePowerInfo> forcePower(std::int32_t powerId) const;
    // retail 0059b8a0 CSWSpell::GetAdjustedForcePoints: party members (creature +0xa88)
    // pay spells.2da forcepoints x forceadjust.2da (row goodEvil/10, GoodCost for 'G',
    // EvilCost for 'E', 1.0 otherwise), truncated; everyone else pays the flat byte.
    std::int32_t adjustedForceCost(std::int32_t powerId, std::int32_t goodEvil, bool partyMember) const;
    // spells.2da conjtime / casttime / catchtime in milliseconds (retail CSWSpell +0x40,
    // +0xa8, +0xec, read by 00514af0 CSWSCreature::AIActionCastSpell).
    struct SpellTimes {
        std::int32_t conjure = 0;
        std::int32_t cast = 0;
        std::int32_t catchTime = 0;
    };
    SpellTimes spellTimes(std::int32_t powerId) const;
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
    // One hand's to-hit (005a7770 melee, 005a7b60 ranged) and damage range (005a9e10), as the
    // equip screen shows them (006b9970). offhand: the left weapon slot.
    // doubleWeapon (005a7770 param 5): the right hand's weaponwield 3 weapon serves both columns.
    std::int32_t handAttackBonus(const CharacterSheet& sheet, bool offhand, bool doubleWeapon = false) const;
    void handDamageRange(const CharacterSheet& sheet, bool offhand, std::int32_t& low, std::int32_t& high,
                         bool doubleWeapon = false) const;
    // To-hit and damage the equipped weapon's own item properties add (004f3fe0 effect bonuses).
    void weaponPropertyBonus(const InventoryItem& item, std::int32_t& attack, std::int32_t& damage) const;
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
    // portraits.2da rows a new player character may use: Sex == gender, ForPC == 1
    // (retail 006f9430). Row numbers are table row indices.
    std::vector<std::int32_t> pcPortraitRows(std::int32_t gender) const;
    PortraitInfo portraitRow(std::int32_t row) const;
    // The portrait art for an alignment (0 dark .. 100 light): the BaseResRef*
    // variants apply below 41, falling back to the base art when the row has none.
    std::string portraitResrefFor(const PortraitInfo& info, std::int32_t alignment) const;
    // appearance.2da row for a portrait row and a base class: Soldier uses
    // Appearance_L, Scout AppearanceNumber, Scoundrel Appearance_S (retail 006dc3c0).
    // Jedi classes use the Scout (medium) body.
    std::int32_t portraitAppearance(std::int32_t row, std::int32_t classId) const;
    // Inverse of the above for a stored appearance id, -1 when no PC row has it.
    std::int32_t portraitRowForAppearance(std::int32_t appearance) const;
    // Feats the class receives automatically at `level` (feat.2da <prefix>_list == 3
    // with no recommendation rank and <prefix>_granted <= level).
    std::vector<std::int32_t> grantedFeats(std::int32_t classId, std::int32_t level) const;
    // What the shipped tables recommend for a class (the character generation's Recommended button):
    // classes.2da str..cha are a full 30-point spend (checked against the point costs for the three
    // base classes), skills.2da <prefix>_reco and feat.2da <prefix>_recom are 1-based priorities
    // (0: not recommended). Ability order is STR DEX CON INT WIS CHA.
    std::array<std::int32_t, 6> recommendedAbilities(std::int32_t classId) const;
    std::int32_t skillRecommendRank(std::int32_t classId, std::int32_t skillIndex) const;
    std::int32_t featRecommendRank(std::int32_t classId, std::int32_t featId) const;
    // The row a class recommends at `rank` (1 upward): the row whose rank is `rank`, else the
    // (rank+1)th row with no rank at all, else -1. Retail 005be430 (skills), 005be3c0 (feats) and
    // 005be6d0 (powers, light_recom or dark_recom by alignment).
    std::int32_t skillForRecommendRank(std::int32_t classId, std::int32_t rank) const;
    std::int32_t featForRecommendRank(std::int32_t classId, std::int32_t rank) const;
    std::int32_t powerForRecommendRank(bool darkSide, std::int32_t rank) const;
    // classes.2da primaryabil as a sheet index (STR DEX CON INT WIS CHA), -1 when none. Retail 006f7390.
    std::int32_t primaryAbility(std::int32_t classId) const;
    // classpowergain.2da: Force powers a Jedi class picks at `level`, 0 for the others (retail 00649070).
    std::int32_t powersForLevel(std::int32_t classId, std::int32_t level) const;
    // What the level-up power list may offer: the row is a learnable power, every prerequisite of its
    // `_`-separated list is known, and the class has reached the power's level (retail 00649b50, 006497a0).
    bool canLearnPowerAtLevelUp(const CharacterSheet& sheet, std::int32_t powerId) const;
    // Skill points at character creation (retail doubles-and-quadruples rule).
    std::int32_t chargenSkillPoints(std::int32_t classId, std::int32_t intelligence) const;
    // Highest rank for a skill at a level and the cost of one rank.
    std::int32_t maxSkillRank(std::int32_t classId, std::int32_t skillIndex,
                              std::int32_t level) const;
    std::int32_t skillRankCost(std::int32_t classId, std::int32_t skillIndex) const;
    // classes.2da cell helpers for the Jedi/base classes.
    std::int32_t classForceDie(std::int32_t classId) const;
    std::int32_t classSkillPointBase(std::int32_t classId) const;
    std::string classColumnPrefix(std::int32_t classId) const;
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
    std::optional<kotor::TwoDa> classPowerGain_;
    std::optional<kotor::TwoDa> animations_;
    std::optional<kotor::TwoDa> combatAnimations_;
    std::optional<kotor::TwoDa> dialogAnimations_;
    std::optional<kotor::TwoDa> xptable_;
    std::optional<kotor::TwoDa> npc_;
    std::optional<kotor::TwoDa> ranges_;
    std::optional<kotor::TwoDa> acbonus_;
    std::optional<kotor::TwoDa> difficultyopt_;
    std::optional<kotor::TwoDa> forceadjust_;
    std::optional<kotor::TwoDa> costTables_;
    kotor::Install* install_ = nullptr;
    // iprp_* cost tables and item templates are read on demand and never change.
    const kotor::TwoDa* propertyCostTable(const kotor::ItemTemplate::Property& property) const;
    std::int32_t propertyCostValue(const kotor::ItemTemplate::Property& property) const;
    mutable std::map<std::string, std::optional<kotor::TwoDa>> propertyTables_;
    mutable std::map<std::string, std::optional<kotor::ItemTemplate>> itemTemplates_;
    std::map<std::string, kotor::TwoDa> attackTables_;
    std::map<std::string, kotor::TwoDa> saveTables_;
    std::map<std::string, kotor::TwoDa> skillTables_;
};

} // namespace ke
