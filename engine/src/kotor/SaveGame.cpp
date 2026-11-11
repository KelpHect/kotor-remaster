#include "kotor/SaveGame.h"

#include "kotor/GffWrite.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>

namespace ke::kotor {

namespace {

constexpr std::uint32_t kIfo = 2014;
constexpr std::uint32_t kAre = 2012;
constexpr std::uint32_t kGit = 2023;
constexpr std::uint32_t kUtc = 2027;
constexpr std::uint32_t kInv = 2037;
constexpr std::uint32_t kFac = 2038;
constexpr std::uint32_t kSav = 2057;

struct ErfEntry {
    std::string resref;
    std::uint32_t type = 0;
    std::vector<std::uint8_t> data;
};

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void appendFloat(std::vector<std::uint8_t>& out, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendU32(out, bits);
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw FormatError("cannot open " + path.string());
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size < 0) {
        throw FormatError("cannot size " + path.string());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw FormatError("short read " + path.string());
    }
    return bytes;
}

void writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw FormatError("cannot write " + path.string());
    }
    if (!bytes.empty()) {
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!file) {
        throw FormatError("short write " + path.string());
    }
}

std::vector<ErfEntry> parseErf(const std::vector<std::uint8_t>& bytes, const std::string& label)
{
    if (bytes.size() < 160) {
        throw FormatError(label + ": shorter than a 160-byte ERF header");
    }
    if (std::memcmp(bytes.data(), "RIM ", 4) == 0) {
        const std::uint32_t count = readU32(bytes.data(), bytes.size(), 12, "RIM count");
        const std::uint32_t table = readU32(bytes.data(), bytes.size(), 16, "RIM table");
        if (count > 1'000'000u) {
            throw FormatError(label + ": implausible RIM entry count");
        }
        requireRange(bytes.size(), table, static_cast<std::size_t>(count) * 32u,
                     "RIM entry table");
        std::vector<ErfEntry> entries;
        entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t o = table + static_cast<std::size_t>(i) * 32u;
            ErfEntry entry;
            entry.resref = readCString(bytes.data(), bytes.size(), o, 16, "RIM resref");
            entry.type = readU16(bytes.data(), bytes.size(), o + 16, "RIM type");
            const std::uint32_t offset = readU32(bytes.data(), bytes.size(), o + 24,
                                                 "RIM offset");
            const std::uint32_t size = readU32(bytes.data(), bytes.size(), o + 28,
                                               "RIM size");
            requireRange(bytes.size(), offset, size, "RIM payload");
            entry.data.assign(bytes.begin() + offset, bytes.begin() + offset + size);
            entries.push_back(std::move(entry));
        }
        return entries;
    }
    if (std::memcmp(bytes.data(), "ERF ", 4) != 0 && std::memcmp(bytes.data(), "MOD ", 4) != 0
        && std::memcmp(bytes.data(), "SAV ", 4) != 0) {
        throw FormatError(label + ": not an ERF/MOD/SAV container");
    }
    const std::uint32_t count = readU32(bytes.data(), bytes.size(), 16, "ERF count");
    const std::uint32_t keyTable = readU32(bytes.data(), bytes.size(), 24, "ERF key table");
    const std::uint32_t resourceTable = readU32(bytes.data(), bytes.size(), 28, "ERF resource table");
    if (count > 1'000'000u) {
        throw FormatError(label + ": implausible entry count");
    }
    requireRange(bytes.size(), keyTable, static_cast<std::size_t>(count) * 24u, "ERF key table");
    requireRange(bytes.size(), resourceTable, static_cast<std::size_t>(count) * 8u,
                 "ERF resource table");
    std::vector<ErfEntry> entries;
    entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t ko = keyTable + static_cast<std::size_t>(i) * 24u;
        const std::size_t ro = resourceTable + static_cast<std::size_t>(i) * 8u;
        ErfEntry entry;
        entry.resref = readCString(bytes.data(), bytes.size(), ko, 16, "ERF resref");
        entry.type = readU16(bytes.data(), bytes.size(), ko + 20, "ERF type");
        const std::uint32_t offset = readU32(bytes.data(), bytes.size(), ro, "ERF offset");
        const std::uint32_t size = readU32(bytes.data(), bytes.size(), ro + 4, "ERF size");
        requireRange(bytes.size(), offset, size, "ERF payload");
        entry.data.assign(bytes.begin() + offset, bytes.begin() + offset + size);
        entries.push_back(std::move(entry));
    }
    return entries;
}

const ErfEntry* findEntry(const std::vector<ErfEntry>& entries, const std::string& resref)
{
    const std::string wanted = lower(resref);
    for (const ErfEntry& entry : entries) {
        if (lower(entry.resref) == wanted) {
            return &entry;
        }
    }
    return nullptr;
}

const ErfEntry* findEntryType(const std::vector<ErfEntry>& entries, std::uint32_t type)
{
    for (const ErfEntry& entry : entries) {
        if (entry.type == type) {
            return &entry;
        }
    }
    return nullptr;
}

const GffField* field(const Gff& gff, std::uint32_t structure, const char* label)
{
    return gff.field(structure, label);
}

std::string text(const Gff& gff, std::uint32_t structure, const char* label,
                 const std::string& fallback = {})
{
    const GffField* value = field(gff, structure, label);
    return value != nullptr ? gff.text(*value) : fallback;
}

std::int32_t numberInt(const Gff& gff, std::uint32_t structure, const char* label,
                       std::int32_t fallback = 0)
{
    const GffField* value = field(gff, structure, label);
    return value != nullptr ? static_cast<std::int32_t>(gff.number(*value)) : fallback;
}

float numberFloat(const Gff& gff, std::uint32_t structure, const char* label, float fallback = 0.0f)
{
    const GffField* value = field(gff, structure, label);
    return value != nullptr ? gff.number(*value) : fallback;
}

bool has(const Gff& gff, std::uint32_t structure, const char* label)
{
    return field(gff, structure, label) != nullptr;
}

std::vector<std::uint32_t> list(const Gff& gff, std::uint32_t structure, const char* label)
{
    const GffField* value = field(gff, structure, label);
    return value != nullptr ? gff.list(*value) : std::vector<std::uint32_t>{};
}

std::string firstLoc(const Gff& gff, std::uint32_t structure, const char* label)
{
    const GffField* value = field(gff, structure, label);
    return value != nullptr ? gff.locString(*value) : std::string{};
}

std::vector<std::uint8_t> binary(const Gff& gff, std::uint32_t structure, const char* label)
{
    const GffField* value = field(gff, structure, label);
    if (value == nullptr || value->type != GffType::Binary) {
        return {};
    }
    const auto& data = gff.fieldData();
    const std::uint32_t length = readU32(data.data(), data.size(), value->data, "GVT binary length");
    requireRange(data.size(), value->data + 4u, length, "GVT binary");
    return std::vector<std::uint8_t>(data.begin() + value->data + 4u,
                                     data.begin() + value->data + 4u + length);
}

// The creature tag of each available-NPC slot. Slot order is the nwscript NPC_*
// constants and the npc.2da rows (Bastila 0 .. Zaalbar 8); the strings are the
// tags the shipped p_* templates carry (npc.2da spells row 7 "T3M3", the
// template says T3M4). GameSession re-derives them from the 2DA at run time.
std::string npcTagFor(std::int32_t npc)
{
    static const std::array<const char*, kSavedNpcCount> tags = {
        "Bastila", "Cand", "Carth", "HK47", "Jolee", "Juhani", "Mission", "T3M4", "Zaalbar"};
    if (npc < 0 || npc >= kSavedNpcCount) {
        return {};
    }
    return tags[static_cast<std::size_t>(npc)];
}

Vec3 objectPosition(const Gff& gff, std::uint32_t structure)
{
    const char* x = has(gff, structure, "XPosition") ? "XPosition" : "X";
    const char* y = has(gff, structure, "YPosition") ? "YPosition" : "Y";
    const char* z = has(gff, structure, "ZPosition") ? "ZPosition" : "Z";
    return {numberFloat(gff, structure, x, 0.0f), numberFloat(gff, structure, y, 0.0f),
            numberFloat(gff, structure, z, 0.0f)};
}

float objectBearing(const Gff& gff, std::uint32_t structure)
{
    if (has(gff, structure, "Bearing")) {
        return numberFloat(gff, structure, "Bearing", 0.0f);
    }
    if (has(gff, structure, "XOrientation")) {
        return std::atan2(numberFloat(gff, structure, "YOrientation", 0.0f),
                          numberFloat(gff, structure, "XOrientation", 1.0f));
    }
    return 0.0f;
}

std::string objectTag(const Gff& gff, std::uint32_t structure)
{
    std::string tag = text(gff, structure, "Tag");
    if (tag.empty()) {
        tag = text(gff, structure, "TemplateResRef");
    }
    return tag;
}

// One item list as the game stores it. `worn`: an Equip_ItemList, whose struct type is the slot mask
// (the lowest bit is kept) and whose item resref is EquippedRes (a retail save writes no InventoryRes there).
std::vector<SavedInventoryItem> readItems(const Gff& gff, const GffField* itemList, bool worn)
{
    std::vector<SavedInventoryItem> items;
    if (itemList == nullptr) {
        return items;
    }
    for (const std::uint32_t raw : gff.list(*itemList)) {
        std::uint32_t structure = raw;
        if (const GffField* nested = field(gff, structure, "Item"); nested != nullptr
            && nested->type == GffType::Struct) {
            structure = gff.structIndex(*nested);
        }
        SavedInventoryItem item;
        if (worn) {
            item.equipped = true;
            item.slot = -1;
            const std::uint32_t mask = gff.structure(raw).typeId;
            for (std::int32_t bit = 0; bit < 32 && mask != 0; ++bit) {
                if ((mask & (1u << bit)) != 0) {
                    item.slot = bit;
                    break;
                }
            }
            item.resref = text(gff, structure, "EquippedRes");
        }
        if (item.resref.empty()) {
            item.resref = text(gff, structure, "InventoryRes");
        }
        if (item.resref.empty()) {
            item.resref = text(gff, structure, "InventoryResRef");
        }
        if (item.resref.empty()) {
            item.resref = text(gff, structure, "TemplateResref");
        }
        if (item.resref.empty()) {
            item.resref = text(gff, structure, "Tag");
        }
        item.name = firstLoc(gff, structure, "LocalizedName");
        if (item.name.empty()) {
            item.name = item.resref;
        }
        item.baseItem = numberInt(gff, structure, "BaseItem", 0);
        item.cost = numberInt(gff, structure, "Cost", 0);
        item.stack = std::max(1, numberInt(gff, structure, "StackSize", 1));
        item.identified = numberInt(gff, structure, "Identified", 1) != 0;
        item.upgrades = static_cast<std::uint32_t>(numberInt(gff, structure, "Upgrades", 0));
        item.newItem = numberInt(gff, structure, "NewItem", 0) != 0;
        {
            for (const std::uint32_t entry : list(gff, structure, "PropertiesList")) { // retail 00555790's layout
                ItemTemplate::Property property;
                property.name = numberInt(gff, entry, "PropertyName", 0);
                property.subtype = numberInt(gff, entry, "Subtype", 0);
                property.costTable = numberInt(gff, entry, "CostTable", 0);
                property.costValue = numberInt(gff, entry, "CostValue", 0);
                property.param1 = numberInt(gff, entry, "Param1", 0);
                property.param1Value = numberInt(gff, entry, "Param1Value", 0);
                property.chanceAppear = numberInt(gff, entry, "ChanceAppear", 100);
                property.usesPerDay = numberInt(gff, entry, "UsesPerDay", 255);
                property.useable = numberInt(gff, entry, "Useable", 1) != 0;
                property.upgradeType = numberInt(gff, entry, "UpgradeType", 255);
                item.properties.push_back(property);
            }
        }
        if (!item.resref.empty()) {
            items.push_back(std::move(item));
        }
    }
    return items;
}

std::vector<SavedInventoryItem> readInventory(const Gff& gff)
{
    return readItems(gff, field(gff, gff.rootIndex(), "ItemList"), false);
}

void readPlayer(const Gff& gff, std::uint32_t structure, OriginalSave& save)
{
    save.pcName = firstLoc(gff, structure, "FirstName");
    if (save.pcName.empty()) {
        save.pcName = firstLoc(gff, structure, "Mod_FirstName");
    }
    if (save.pcName.empty()) {
        save.pcName = "Player";
    }
    save.str = numberInt(gff, structure, "Str", 10);
    save.dex = numberInt(gff, structure, "Dex", 10);
    save.con = numberInt(gff, structure, "Con", 10);
    save.intel = numberInt(gff, structure, "Int", 10);
    save.wis = numberInt(gff, structure, "Wis", 10);
    save.cha = numberInt(gff, structure, "Cha", 10);
    // Retail creature loader 005afce0: for a PC (IsPC set, 005b1306) the current
    // HP is CurrentHitPoints + (max HP - HitPoints), with max HP from the
    // creature's max-HP getter (vtable +0x98, 005b1320), then set via 004cc280.
    // Force follows the same rule with the max-force getter (005b134a). A save
    // made straight after chargen holds HitPoints 0 / CurrentHitPoints 0 /
    // MaxHitPoints 6 and retail shows 6/6. The stored Max* fields stand in for
    // the getters, which retail wrote from the same values when it saved.
    const std::int32_t baseHp = numberInt(gff, structure, "HitPoints", 1);
    const std::int32_t currentHp = numberInt(gff, structure, "CurrentHitPoints", baseHp);
    save.maxHp = std::max(1, numberInt(gff, structure, "MaxHitPoints", baseHp));
    const bool isPc = numberInt(gff, structure, "IsPC", 1) != 0;
    save.hp = isPc ? currentHp + (save.maxHp - baseHp) : currentHp;
    save.xp = numberInt(gff, structure, "Experience", 0);
    save.alignment = std::clamp(numberInt(gff, structure, "GoodEvil", 50), 0, 100);
    save.gender = std::clamp(numberInt(gff, structure, "Gender", 0), 0, 1);
    save.portraitId = numberInt(gff, structure, "PortraitId", -1);
    save.appearance = numberInt(gff, structure, "Appearance_Type", -1);
    const std::int32_t baseForce = numberInt(gff, structure, "ForcePoints", 0);
    save.maxForcePoints = numberInt(gff, structure, "MaxForcePoints", baseForce);
    save.forcePoints = numberInt(gff, structure, "CurrentForce", baseForce);
    if (isPc) {
        save.forcePoints += save.maxForcePoints - baseForce;
    }
    const auto classes = list(gff, structure, "ClassList");
    if (!classes.empty()) {
        save.classId = numberInt(gff, classes.front(), "Class", 0);
        save.level = std::max(1, numberInt(gff, classes.front(), "ClassLevel", 1));
    }
    for (const std::uint32_t feat : list(gff, structure, "FeatList")) {
        save.feats.push_back(numberInt(gff, feat, "Feat", 0));
    }
    for (const std::uint32_t effectStruct : list(gff, structure, "EffectList")) {
        ActiveEffect effect;
        effect.spellId = numberInt(gff, effectStruct, "SpellId", -1);
        if (effect.spellId < 0) {
            continue;
        }
        effect.label = text(gff, effectStruct, "Label");
        effect.remaining = numberFloat(gff, effectStruct, "Duration", 0.0f);
        effect.armorBonus = numberInt(gff, effectStruct, "Armor", 0);
        effect.attackBonus = numberInt(gff, effectStruct, "Attack", 0);
        effect.damageBonus = numberInt(gff, effectStruct, "Damage", 0);
        effect.shield = numberInt(gff, effectStruct, "Shield", 0);
        effect.force = numberInt(gff, effectStruct, "Force", 0) != 0;
        effect.dispellable = numberInt(gff, effectStruct, "Dispellable", 1) != 0;
        if (effect.remaining > 0.0f) {
            save.effects.push_back(std::move(effect));
        }
    }
    save.forceSuppressedTimer = numberFloat(gff, structure, "ForceSuppressed", 0.0f);
    for (const std::uint32_t level : list(gff, structure, "LvlStatList")) {
        for (const std::uint32_t row : gff.structure(level).fieldRows) {
            const GffField& field = gff.fieldAt(row);
            if (field.label.rfind("KnownList", 0) != 0 || field.type != GffType::List) {
                continue;
            }
            for (const std::uint32_t spellStruct : gff.list(field)) {
                const std::int32_t spell = numberInt(gff, spellStruct, "Spell", -1);
                if (spell >= 0) {
                    save.powers.push_back(spell);
                }
            }
        }
    }
}

void readGlobals(const Gff& gff, OriginalSave& save)
{
    const auto names = [&](const char* label) {
        std::vector<std::string> out;
        for (const std::uint32_t structure : list(gff, gff.rootIndex(), label)) {
            out.push_back(text(gff, structure, "Name"));
        }
        return out;
    };
    const std::vector<std::string> numbers = names("CatNumber");
    const std::vector<std::string> booleans = names("CatBoolean");
    const std::vector<std::string> strings = names("CatString");
    const std::vector<std::string> locations = names("CatLocation");
    const std::vector<std::uint8_t> numberBytes = binary(gff, gff.rootIndex(), "ValNumber");
    const std::vector<std::uint8_t> booleanBytes = binary(gff, gff.rootIndex(), "ValBoolean");
    const std::vector<std::uint8_t> locationBytes = binary(gff, gff.rootIndex(), "ValLocation");
    save.globalBooleanBytes = booleanBytes.size();
    save.globalNumberBytes = numberBytes.size();
    save.globalLocationBytes = locationBytes.size();
    for (std::size_t i = 0; i < numbers.size(); ++i) {
        SavedGlobal value;
        value.type = SavedGlobal::Type::Number;
        value.name = numbers[i];
        // Retail keeps one byte per number and reads it sign-extended (00529240, 00538dc0).
        value.number = i < numberBytes.size() ? static_cast<std::int8_t>(numberBytes[i]) : 0;
        save.globals.push_back(std::move(value));
    }
    for (std::size_t i = 0; i < booleans.size(); ++i) {
        SavedGlobal value;
        value.type = SavedGlobal::Type::Boolean;
        value.name = booleans[i];
        const std::size_t byte = i / 8u;
        const int bit = 7 - static_cast<int>(i % 8u);
        value.boolean = byte < booleanBytes.size()
            && ((booleanBytes[byte] >> bit) & 1) != 0;
        save.globals.push_back(std::move(value));
    }
    for (std::size_t i = 0; i < locations.size(); ++i) {
        SavedGlobal value;
        value.type = SavedGlobal::Type::Location;
        value.name = locations[i];
        const std::size_t offset = i * 24u;
        if (offset + 24u <= locationBytes.size()) {
            for (int component = 0; component < 6; ++component) {
                value.location[static_cast<std::size_t>(component)] =
                    readF32(locationBytes.data(), locationBytes.size(),
                            offset + static_cast<std::size_t>(component) * 4u,
                            "GVT location");
            }
        }
        save.globals.push_back(std::move(value));
    }
    const auto stringValues = list(gff, gff.rootIndex(), "ValString");
    for (std::size_t i = 0; i < strings.size(); ++i) {
        SavedGlobal value;
        value.type = SavedGlobal::Type::String;
        value.name = strings[i];
        if (i < stringValues.size()) {
            value.text = text(gff, stringValues[i], "String");
        }
        save.globals.push_back(std::move(value));
    }
}

// PARTYTABLE. PT_MEMBERS lists NPC ids only (retail 00562c40 family: the PC is
// not a member and is id -1); PT_AVAIL_NPCS is indexed by NPC id.
void readParty(const Gff& gff, OriginalSave& save)
{
    save.party.clear();
    save.partyTags.clear();
    for (const std::uint32_t structure : list(gff, gff.rootIndex(), "PT_MEMBERS")) {
        const std::int32_t id = numberInt(gff, structure, "PT_MEMBER_ID", -1);
        const bool leader = numberInt(gff, structure, "PT_IS_LEADER", 0) != 0;
        if (id < 0 || id >= kSavedNpcCount) {
            continue;
        }
        save.party.emplace_back(id, leader);
        const std::string tag = npcTagFor(id);
        if (std::find(save.partyTags.begin(), save.partyTags.end(), tag)
            == save.partyTags.end()) {
            save.partyTags.push_back(tag);
        }
    }
    const auto available = list(gff, gff.rootIndex(), "PT_AVAIL_NPCS");
    for (std::size_t i = 0; i < available.size() && i < save.npcs.size(); ++i) {
        save.npcs[i].available = numberInt(gff, available[i], "PT_NPC_AVAIL", 0) != 0;
        save.npcs[i].select = numberInt(gff, available[i], "PT_NPC_SELECT", 1);
    }
    save.controlledNpc = numberInt(gff, gff.rootIndex(), "PT_CONTROLLED_NP", -1);
    save.solo = numberInt(gff, gff.rootIndex(), "PT_SOLOMODE", 0) != 0;
    save.xpPool = numberInt(gff, gff.rootIndex(), "PT_XP_POOL", 0);
    for (const std::uint32_t structure : list(gff, gff.rootIndex(), "JNL_Entries")) {
        const std::string plot = text(gff, structure, "JNL_PlotID");
        if (!plot.empty()) {
            save.journal[plot] = numberInt(gff, structure, "JNL_State", 0);
        }
    }
    save.gold = numberInt(gff, gff.rootIndex(), "PT_GOLD", save.gold);
    // LoadTableInfo 00565d20 reads 18 counts and 10 side cards. ENGINE CHOICE: a save with no list at
    // all (engine saves written before the lists were kept) keeps the new-game defaults.
    const auto pazaakCards = list(gff, gff.rootIndex(), "PT_PAZAAKCARDS");
    if (!pazaakCards.empty()) {
        for (std::size_t i = 0; i < save.pazaakCards.size(); ++i) {
            save.pazaakCards[i] = i < pazaakCards.size() ? numberInt(gff, pazaakCards[i], "PT_PAZAAKCOUNT", 0) : 0;
        }
        const auto sideList = list(gff, gff.rootIndex(), "PT_PAZSIDELIST");
        for (std::size_t i = 0; i < save.pazaakSideList.size(); ++i) {
            save.pazaakSideList[i] = i < sideList.size() ? numberInt(gff, sideList[i], "PT_PAZSIDECARD", 0) : 0;
        }
    }
    save.playedSeconds = numberInt(gff, gff.rootIndex(), "PT_PLAYEDSECONDS", save.playedSeconds);
    // 005661df reads 6 bytes and falls back to zeros.
    const std::vector<std::uint8_t> tutorials = binary(gff, gff.rootIndex(), "PT_TUT_WND_SHOWN");
    for (std::size_t i = 0; i < save.tutorialShown.size(); ++i) {
        save.tutorialShown[i] = i < tutorials.size() ? tutorials[i] : 0u;
    }
}

// AVAILNPC<n>: a stored creature in the same shape as the player node.
SavedNpc readNpcNode(const Gff& gff)
{
    const std::uint32_t root = gff.rootIndex();
    SavedNpc npc;
    npc.hasCreature = true;
    npc.templateResref = text(gff, root, "TemplateResRef");
    npc.tag = text(gff, root, "Tag");
    npc.name = firstLoc(gff, root, "FirstName");
    npc.str = numberInt(gff, root, "Str", 10);
    npc.dex = numberInt(gff, root, "Dex", 10);
    npc.con = numberInt(gff, root, "Con", 10);
    npc.intel = numberInt(gff, root, "Int", 10);
    npc.wis = numberInt(gff, root, "Wis", 10);
    npc.cha = numberInt(gff, root, "Cha", 10);
    // Retail 005afce0: a companion is loaded like any creature (current value through the per-level share).
    {
        const CreatureHitPoints hp = creatureHitPointsFromGff(gff, root);
        npc.hp = hp.current;
        npc.maxHp = std::max(1, hp.max);
        npc.baseHp = numberInt(gff, root, "HitPoints", -1);
    }
    npc.xp = numberInt(gff, root, "Experience", 0);
    npc.joiningXp = numberInt(gff, root, "JoiningXP", 0);
    npc.alignment = std::clamp(numberInt(gff, root, "GoodEvil", 50), 0, 100);
    npc.forcePoints = numberInt(gff, root, "CurrentForce", 0);
    npc.maxForcePoints = numberInt(gff, root, "MaxForcePoints",
                                   numberInt(gff, root, "ForcePoints", 0));
    // A multi-class NPC (a Jedi who started as a soldier) keeps only its first
    // class here; the engine character sheet has one class.
    const auto classes = list(gff, root, "ClassList");
    if (!classes.empty()) {
        npc.classId = numberInt(gff, classes.front(), "Class", 0);
        npc.level = std::max(1, numberInt(gff, classes.front(), "ClassLevel", 1));
        for (const std::uint32_t spell : list(gff, classes.front(), "KnownList0")) {
            const std::int32_t id = numberInt(gff, spell, "Spell", -1);
            if (id >= 0) {
                npc.powers.push_back(id);
            }
        }
    }
    for (const std::uint32_t level : list(gff, root, "LvlStatList")) {
        for (const std::uint32_t row : gff.structure(level).fieldRows) {
            const GffField& known = gff.fieldAt(row);
            if (known.label.rfind("KnownList", 0) != 0 || known.type != GffType::List) {
                continue;
            }
            for (const std::uint32_t spell : gff.list(known)) {
                const std::int32_t id = numberInt(gff, spell, "Spell", -1);
                if (id >= 0 && std::find(npc.powers.begin(), npc.powers.end(), id)
                                   == npc.powers.end()) {
                    npc.powers.push_back(id);
                }
            }
        }
    }
    for (const std::uint32_t feat : list(gff, root, "FeatList")) {
        npc.feats.push_back(numberInt(gff, feat, "Feat", 0));
    }
    for (const std::uint32_t skill : list(gff, root, "SkillList")) {
        npc.skills.push_back(numberInt(gff, skill, "Rank", 0));
    }
    if (const GffField* equip = field(gff, root, "Equip_ItemList"); equip != nullptr) {
        for (const std::uint32_t structure : gff.list(*equip)) {
            SavedInventoryItem item;
            item.resref = text(gff, structure, "EquippedRes");
            if (item.resref.empty()) {
                item.resref = text(gff, structure, "InventoryRes");
            }
            if (item.resref.empty()) {
                item.resref = text(gff, structure, "TemplateResRef");
            }
            if (item.resref.empty()) {
                item.resref = text(gff, structure, "Tag");
            }
            item.name = firstLoc(gff, structure, "LocalizedName");
            item.baseItem = numberInt(gff, structure, "BaseItem", 0);
            item.cost = numberInt(gff, structure, "Cost", 0);
            item.stack = std::max(1, numberInt(gff, structure, "StackSize", 1));
            item.identified = numberInt(gff, structure, "Identified", 1) != 0;
            item.equipped = true;
            // The struct type of a worn item is its slot mask; keep the lowest bit.
            item.slot = -1;
            const std::uint32_t mask = gff.structure(structure).typeId;
            for (std::int32_t bit = 0; bit < 32 && mask != 0; ++bit) {
                if ((mask & (1u << bit)) != 0) {
                    item.slot = bit;
                    break;
                }
            }
            if (!item.resref.empty()) {
                npc.inventory.push_back(std::move(item));
            }
        }
    }
    return npc;
}

// The "SWVarTable" struct of an object (or of the GIT root, the area's own table).
SavedVarTable readVarTable(const Gff& gff, std::uint32_t structure)
{
    SavedVarTable vars;
    const GffField* table = field(gff, structure, "SWVarTable");
    if (table == nullptr || table->type != GffType::Struct) {
        return vars;
    }
    const std::uint32_t at = gff.structIndex(*table);
    vars.present = true;
    std::size_t word = 0;
    for (const std::uint32_t entry : list(gff, at, "BitArray")) {
        if (word < vars.bits.size()) {
            // DWORD words: the raw bits (Gff::integer goes through a float and loses bits above 2^24).
            const GffField* variable = field(gff, entry, "Variable");
            vars.bits[word++] = variable != nullptr ? variable->data : 0u;
        }
    }
    std::size_t byte = 0;
    for (const std::uint32_t entry : list(gff, at, "ByteArray")) {
        if (byte < vars.bytes.size()) {
            vars.bytes[byte++] = static_cast<std::uint8_t>(numberInt(gff, entry, "Variable", 0));
        }
    }
    return vars;
}

void readObjects(const Gff& gff, OriginalSave& save)
{
    save.areaVars = readVarTable(gff, gff.rootIndex());
    struct ListSpec {
        const char* label;
        SavedObjectState::Kind kind;
    };
    const std::array<ListSpec, 8> lists = {{
        {"Creature List", SavedObjectState::Kind::Creature},
        {"Door List", SavedObjectState::Kind::Door},
        {"Placeable List", SavedObjectState::Kind::Placeable},
        {"TriggerList", SavedObjectState::Kind::Trigger},
        {"StoreList", SavedObjectState::Kind::Store},
        {"Encounter List", SavedObjectState::Kind::Encounter},
        {"WaypointList", SavedObjectState::Kind::Waypoint},
        {"SoundList", SavedObjectState::Kind::Sound},
    }};
    for (const ListSpec& spec : lists) {
        for (const std::uint32_t structure : list(gff, gff.rootIndex(), spec.label)) {
            SavedObjectState object;
            object.kind = spec.kind;
            object.tag = objectTag(gff, structure);
            object.position = objectPosition(gff, structure);
            object.bearing = objectBearing(gff, structure);
            if (spec.kind == SavedObjectState::Kind::Creature) {
                // Retail 005afce0: the stored current value is read back through the per-level share.
                const CreatureHitPoints hp = creatureHitPointsFromGff(gff, structure);
                object.hitPoints = hp.current;
                object.maxHitPoints = std::max(1, hp.max);
            } else {
                // Doors (0058a1f0) and placeables (00585670) write HP, the maximum, and CurrentHP.
                object.hitPoints = static_cast<std::int16_t>(numberInt(gff, structure, "CurrentHP", 1));
                object.maxHitPoints = std::max<std::int32_t>(
                    1, static_cast<std::int16_t>(numberInt(gff, structure, "HP", object.hitPoints)));
            }
            object.dead = numberInt(gff, structure, "Dead", 0) != 0
                || numberInt(gff, structure, "IsDead", 0) != 0
                || (spec.kind == SavedObjectState::Kind::Creature && object.hitPoints <= 0);
            object.open = numberInt(gff, structure, "OpenState", 0) != 0
                || numberInt(gff, structure, "Open", 0) != 0;
            object.locked = numberInt(gff, structure, "Locked", 0) != 0;
            for (const std::uint32_t entry : list(gff, structure, "VarTable")) {
                SavedObjectState::Local local;
                local.name = text(gff, entry, "Name");
                local.type = numberInt(gff, entry, "Type", 0);
                local.integer = numberInt(gff, entry, "Value", 0);
                local.number = numberFloat(gff, entry, "Value", 0.0f);
                if (local.type == 3) {
                    local.text = text(gff, entry, "Value");
                }
                if (!local.name.empty() && local.type >= 1 && local.type <= 3) {
                    object.locals.push_back(std::move(local));
                }
            }
            object.effects = readSavedEffects(gff, structure);
            {
                const SavedVarTable table = readVarTable(gff, structure);
                object.localTable = table.present;
                object.localBits = table.bits;
                object.localBytes = table.bytes;
            }
            if (const GffField* id = field(gff, structure, "ObjectId"); id != nullptr) {
                object.objectId = id->data;
            }
            if (spec.kind == SavedObjectState::Kind::Placeable) {
                for (const std::uint32_t item : list(gff, structure, "ItemList")) {
                    std::string resref = text(gff, item, "InventoryRes");
                    if (resref.empty()) {
                        resref = text(gff, item, "InventoryResRef");
                    }
                    if (resref.empty()) {
                        resref = lower(text(gff, item, "Tag")); // a retail save writes the item whole; Tag is its resref
                    }
                    if (!resref.empty()) {
                        object.loot.push_back(resref);
                    }
                }
            }
            if (!object.tag.empty() || object.position.x != 0.0f || object.position.y != 0.0f) {
                save.objects.push_back(std::move(object));
            }
        }
    }
}

// The IFO's pending delayed scripts (retail event queue, saved as "EventQueue") and the clock they are due on.
void readEventQueue(const Gff& ifo, OriginalSave& save)
{
    const auto dword = [&](std::uint32_t structure, const char* label) {
        const GffField* value = field(ifo, structure, label);
        // The raw 32 bits: Gff::integer goes through a float and rounds above 2^24 (a time of day in milliseconds).
        return value != nullptr ? value->data : 0u;
    };
    const std::uint32_t root = ifo.rootIndex();
    save.pauseDay = dword(root, "Mod_PauseDay");
    save.pauseTime = dword(root, "Mod_PauseTime");
    for (const std::uint32_t area : list(ifo, root, "Mod_Area_list")) {
        save.areaObjectId = dword(area, "ObjectId");
        break;
    }
    for (const std::uint32_t player : list(ifo, root, "Mod_PlayerList")) {
        save.playerObjectId = dword(player, "ObjectId");
        break;
    }
    for (const std::uint32_t entry : list(ifo, root, "EventQueue")) {
        const GffField* data = field(ifo, entry, "EventData");
        if (data == nullptr || data->type != GffType::Struct) {
            continue;
        }
        const std::uint32_t body = ifo.structIndex(*data);
        SavedScriptEvent event;
        event.day = dword(entry, "Day");
        event.time = dword(entry, "Time");
        event.objectId = dword(entry, "ObjectId");
        event.callerId = dword(entry, "CallerId");
        event.script = text(ifo, body, "Name");
        event.instruction = static_cast<std::int32_t>(dword(body, "InstructionPtr"));
        const GffField* stack = field(ifo, body, "Stack");
        if (stack != nullptr && stack->type == GffType::Struct) {
            const std::uint32_t frame = ifo.structIndex(*stack);
            event.basePointer = static_cast<std::int32_t>(dword(frame, "BasePointer"));
            for (const std::uint32_t cell : list(ifo, frame, "Stack")) {
                SavedStackCell out;
                out.type = static_cast<std::uint8_t>(dword(cell, "Type"));
                if (const GffField* value = field(ifo, cell, "Value"); value != nullptr) {
                    switch (out.type) {
                    case 4: out.real = ifo.number(*value); break;
                    case 5: out.text = ifo.text(*value); break;
                    default:
                        if (value->type != GffType::Struct) {
                            out.integer = static_cast<std::int32_t>(value->data);
                        }
                        break;
                    }
                }
                event.stack.push_back(std::move(out));
            }
        }
        if (!event.script.empty()) {
            save.events.push_back(std::move(event));
        }
    }
}

void readNfo(const Gff& gff, OriginalSave& save)
{
    save.areaName = text(gff, gff.rootIndex(), "AREANAME");
    const std::string last = text(gff, gff.rootIndex(), "LASTMODULE");
    if (!last.empty()) {
        save.module = last;
    }
    const std::string name = text(gff, gff.rootIndex(), "SAVEGAMENAME");
    if (!name.empty()) {
        save.saveName = name;
    }
    save.timePlayed = numberInt(gff, gff.rootIndex(), "TIMEPLAYED", 0);
    const std::string portrait = text(gff, gff.rootIndex(), "PORTRAIT0");
    if (!portrait.empty()) {
        save.portrait = portrait;
    }
}

// --- mutable-node helpers -------------------------------------------------

void patchInt(GffNode& node, const std::string& label, std::int64_t value)
{
    if (GffNodeField* existing = node.find(label); existing != nullptr) {
        existing->integer = value;
        return;
    }
    node.addInt32(label, static_cast<std::int32_t>(value));
}

// Replaces a node's "SWVarTable" (retail CSWVarTable, struct id 0 with BitArray and ByteArray lists).
void setVarTable(GffNode& node, const SavedVarTable& vars)
{
    node.remove("SWVarTable");
    auto table = std::make_shared<GffNode>(0);
    std::vector<GffNodePtr> bits;
    for (const std::uint32_t word : vars.bits) {
        auto entry = std::make_shared<GffNode>(0);
        entry->addUInt32("Variable", word);
        bits.push_back(std::move(entry));
    }
    std::vector<GffNodePtr> bytes;
    for (const std::uint8_t byte : vars.bytes) {
        auto entry = std::make_shared<GffNode>(0);
        entry->addUInt8("Variable", byte);
        bytes.push_back(std::move(entry));
    }
    table->addList("BitArray", std::move(bits));
    table->addList("ByteArray", std::move(bytes));
    node.addStruct("SWVarTable", std::move(table));
}

void patchFloat(GffNode& node, const std::string& label, float value)
{
    if (GffNodeField* existing = node.find(label); existing != nullptr) {
        if (existing->type == GffType::Float) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            existing->integer = bits;
        } else {
            existing->integer = static_cast<std::int64_t>(value);
        }
        return;
    }
    node.addFloat(label, value);
}

void patchString(GffNode& node, const std::string& label, const std::string& value)
{
    if (GffNodeField* existing = node.find(label); existing != nullptr) {
        existing->text = value;
        return;
    }
    node.addString(label, value);
}

void patchResRef(GffNode& node, const std::string& label, const std::string& value)
{
    if (GffNodeField* existing = node.find(label); existing != nullptr) {
        existing->text = value;
        return;
    }
    node.addResRef(label, value);
}

GffNodePtr buildItemNode(const SavedInventoryItem& source)
{
    auto item = std::make_shared<GffNode>(0);
    item->addInt32("BaseItem", source.baseItem);
    item->addString("Tag", source.resref);
    item->addUInt8("Identified", source.identified ? 1u : 0u);
    item->addLocString("Description", UINT32_MAX, {});
    item->addLocString("DescIdentified", UINT32_MAX, {});
    const std::vector<std::pair<std::uint32_t, std::string>> name = {
        {0, source.name.empty() ? source.resref : source.name}};
    item->addLocString("LocalizedName", UINT32_MAX, name);
    item->addUInt16("StackSize", static_cast<std::uint32_t>(std::max(1, source.stack)));
    item->addUInt8("Stolen", 0);
    item->addUInt32("Upgrades", source.upgrades);
    item->addUInt8("Dropable", 1);
    item->addUInt8("Pickpocketable", 1);
    item->addUInt8("ModelVariation", 1);
    item->addUInt8("Charges", 0);
    item->addUInt8("MaxCharges", 0);
    item->addUInt32("Cost", static_cast<std::uint32_t>(std::max(0, source.cost)));
    item->addUInt32("AddCost", static_cast<std::uint32_t>(std::max(0, source.cost)));
    item->addUInt8("Plot", 0);
    // Retail 00555790: one struct per property (struct id = its list index), words for the
    // name, subtype and cost value, bytes for the rest.
    std::vector<GffNodePtr> properties;
    for (const ItemTemplate::Property& property : source.properties) {
        auto entry = std::make_shared<GffNode>(static_cast<std::uint32_t>(properties.size()));
        entry->addUInt16("PropertyName", static_cast<std::uint32_t>(property.name) & 0xFFFFu);
        entry->addUInt16("Subtype", static_cast<std::uint32_t>(property.subtype) & 0xFFFFu);
        entry->addUInt8("CostTable", static_cast<std::uint32_t>(property.costTable) & 0xFFu);
        entry->addUInt16("CostValue", static_cast<std::uint32_t>(property.costValue) & 0xFFFFu);
        entry->addUInt8("Param1", static_cast<std::uint32_t>(property.param1) & 0xFFu);
        entry->addUInt8("Param1Value", static_cast<std::uint32_t>(property.param1Value) & 0xFFu);
        entry->addUInt8("ChanceAppear", static_cast<std::uint32_t>(property.chanceAppear) & 0xFFu);
        entry->addUInt8("UsesPerDay", static_cast<std::uint32_t>(property.usesPerDay) & 0xFFu);
        entry->addUInt8("Useable", property.useable ? 1u : 0u);
        entry->addUInt8("UpgradeType", static_cast<std::uint32_t>(property.upgradeType) & 0xFFu);
        properties.push_back(std::move(entry));
    }
    item->addList("PropertiesList", std::move(properties));
    item->addFloat("XPosition", 0.0f);
    item->addFloat("YPosition", 0.0f);
    item->addFloat("ZPosition", 0.0f);
    item->addFloat("XOrientation", 1.0f);
    item->addFloat("YOrientation", 0.0f);
    item->addFloat("ZOrientation", 0.0f);
    item->addUInt8("NonEquippable", 0);
    item->addUInt8("NewItem", source.newItem ? 1u : 0u);
    item->addUInt8("DELETING", 0);
    return item;
}

GffNodePtr buildPlayerNode(const OriginalSave& save)
{
    auto node = std::make_shared<GffNode>(0);
    const std::vector<std::pair<std::uint32_t, std::string>> first = {{0, save.pcName}};
    const std::vector<std::pair<std::uint32_t, std::string>> empty;
    node->addString("Mod_CommntyName", "");
    node->addUInt8("Mod_IsPrimaryPlr", 1);
    node->addLocString("Mod_FirstName", UINT32_MAX, first);
    node->addLocString("Mod_LastName", UINT32_MAX, empty);
    node->addUInt32("ObjectId", 0x7FFFFFFFu);
    node->addLocString("FirstName", UINT32_MAX, first);
    node->addLocString("LastName", UINT32_MAX, empty);
    node->addLocString("Description", UINT32_MAX, empty);
    node->addUInt8("IsPC", 1);
    node->addString("Tag", "PC");
    node->addResRef("Conversation", "");
    node->addUInt8("Interruptable", 1);
    node->addInt32("Age", 0);
    node->addUInt8("Gender", static_cast<std::uint32_t>(std::clamp(save.gender, 0, 1)));
    node->addUInt8("Race", 6);
    node->addString("Subrace", "");
    node->addUInt8("SubraceIndex", 0);
    node->addUInt8("StartingPackage", 0);
    node->addString("Deity", "");
    node->addUInt8("MClassLevUpIn", 0);
    node->addInt16("willbonus", 0);
    node->addInt16("fortbonus", 0);
    node->addInt16("refbonus", 0);
    node->addUInt32("Gold", 0);
    node->addInt8("RefSaveThrow", 2);
    node->addInt8("WillSaveThrow", 3);
    node->addInt8("FortSaveThrow", 4);
    node->addInt16("ArmorClass", 10);
    node->addUInt8("Str", static_cast<std::uint32_t>(save.str));
    node->addUInt8("Dex", static_cast<std::uint32_t>(save.dex));
    node->addUInt8("Int", static_cast<std::uint32_t>(save.intel));
    node->addUInt8("Wis", static_cast<std::uint32_t>(save.wis));
    node->addUInt8("Con", static_cast<std::uint32_t>(save.con));
    node->addUInt8("Cha", static_cast<std::uint32_t>(save.cha));
    node->addUInt8("NaturalAC", 0);
    node->addUInt16("SoundSetFile", 0);
    node->addUInt8("Plot", 0);
    node->addUInt8("Disarmable", 1);
    node->addUInt8("Min1HP", 0);
    node->addUInt8("PartyInteract", 0);
    node->addUInt8("NotReorienting", 0);
    node->addUInt8("BodyBag", 0);
    // HitPoints holds the base max so that 005afce0 reads back current = CurrentHitPoints.
    node->addInt16("HitPoints", static_cast<std::int16_t>(save.maxHp));
    node->addInt16("CurrentHitPoints", static_cast<std::int16_t>(save.hp));
    node->addInt16("MaxHitPoints", static_cast<std::int16_t>(save.maxHp));
    node->addInt16("PregameCurrent", static_cast<std::int16_t>(save.maxHp));
    node->addInt16("ForcePoints", static_cast<std::int16_t>(save.maxForcePoints));
    node->addInt16("CurrentForce", static_cast<std::int16_t>(save.forcePoints));
    node->addInt16("MaxForcePoints", static_cast<std::int16_t>(save.maxForcePoints));
    node->addUInt32("Experience", static_cast<std::uint32_t>(save.xp));
    node->addUInt8("MovementRate", 0);
    node->addUInt16("PortraitId", static_cast<std::uint32_t>(
                                         save.portraitId >= 0 ? save.portraitId : 26));
    node->addUInt8("GoodEvil", static_cast<std::uint32_t>(std::clamp(save.alignment, 0, 100)));
    node->addUInt8("Color_Skin", 0);
    node->addUInt8("Color_Hair", 0);
    node->addUInt8("Color_Tattoo1", 0);
    node->addUInt8("Color_Tattoo2", 0);
    node->addUInt32("Phenotype", 0);
    node->addUInt16("Appearance_Type", static_cast<std::uint32_t>(
                                              save.appearance >= 0 ? save.appearance : 6));
    node->addUInt8("Appearance_Head", 1);
    node->addUInt8("DuplicatingHead", 255);
    node->addUInt8("UseBackupHead", 0);
    node->addUInt8("Tail", 0);
    node->addUInt8("Wings", 0);
    node->addUInt16("FactionID", 0);
    node->addFloat("ChallengeRating", 0.0f);
    node->addUInt32("AIState", 0);

    auto classes = std::make_shared<GffNode>(0);
    classes->addInt32("Class", save.classId);
    classes->addInt16("ClassLevel", static_cast<std::int16_t>(save.level));
    node->addList("ClassList", {classes});
    std::vector<GffNodePtr> levels;
    const std::int32_t levelCount = std::max<std::int32_t>(1, save.level);
    for (std::int32_t level = 0; level < levelCount; ++level) {
        auto entry = std::make_shared<GffNode>(0);
        entry->addUInt8("LvlStatHitDie", 0);
        entry->addUInt8("LvlStatForce", 0);
        entry->addUInt8("LvlStatClass", static_cast<std::uint32_t>(save.classId));
        entry->addList("SkillList", {});
        entry->addList("FeatList", {});
        if (level == levelCount - 1) {
            std::vector<GffNodePtr> known;
            for (const std::int32_t power : save.powers) {
                auto spell = std::make_shared<GffNode>(0);
                spell->addInt32("Spell", power);
                known.push_back(std::move(spell));
            }
            entry->addList("KnownList0", std::move(known));
        }
        levels.push_back(std::move(entry));
    }
    node->addList("LvlStatList", std::move(levels));
    node->addUInt16("SkillPoints", 0);
    std::vector<GffNodePtr> skills;
    for (int i = 0; i < 8; ++i) {
        auto skill = std::make_shared<GffNode>(0);
        skill->addUInt8("Rank", static_cast<std::uint32_t>(
            i < static_cast<int>(save.skills.size()) ? std::max(0, save.skills[i]) : 0));
        skills.push_back(std::move(skill));
    }
    node->addList("SkillList", std::move(skills));
    std::vector<GffNodePtr> feats;
    for (const std::int32_t feat : save.feats) {
        auto entry = std::make_shared<GffNode>(0);
        entry->addUInt16("Feat", static_cast<std::uint32_t>(feat));
        feats.push_back(std::move(entry));
    }
    node->addList("FeatList", std::move(feats));
    node->addStruct("CombatInfo", std::make_shared<GffNode>(0));
    node->addUInt8("DetectMode", 1);
    node->addUInt8("StealthMode", 0);
    node->addUInt32("CreatureSize", 3);
    node->addUInt8("IsDestroyable", 1);
    node->addUInt8("IsRaiseable", 1);
    node->addUInt8("DeadSelectable", 1);
    node->addResRef("ScriptHeartbeat", "");
    node->addResRef("ScriptOnNotice", "");
    node->addResRef("ScriptSpellAt", "");
    node->addResRef("ScriptAttacked", "");
    node->addResRef("ScriptDamaged", "");
    node->addResRef("ScriptDisturbed", "");
    node->addResRef("ScriptEndRound", "");
    node->addResRef("ScriptDialogue", "");
    node->addResRef("ScriptSpawn", "");
    node->addResRef("ScriptRested", "");
    node->addResRef("ScriptDeath", "");
    node->addResRef("ScriptUserDefine", "");
    node->addResRef("ScriptOnBlocked", "");
    node->addResRef("ScriptEndDialogu", "");
    std::vector<GffNodePtr> equipped;
    for (const SavedInventoryItem& item : save.inventory) {
        if (item.equipped) {
            // The slot mask is the struct type, as in a UTC and as readItems reads it back.
            const std::uint32_t mask = item.slot >= 0 && item.slot < 32 ? (1u << item.slot) : 0u;
            auto worn = std::make_shared<GffNode>(mask);
            worn->fields() = buildItemNode(item)->fields();
            if (item.resref.size() <= 16) {
                worn->addResRef("EquippedRes", item.resref);
            }
            equipped.push_back(std::move(worn));
        }
    }
    node->addList("Equip_ItemList", std::move(equipped));
    node->addList("ItemList", {});
    node->addList("PerceptionList", {});
    node->addStruct("CombatRoundData", std::make_shared<GffNode>(0));
    node->addUInt32("AreaId", 1);
    node->addUInt8("AmbientAnimState", 0);
    node->addUInt32("Animation", 10000);
    node->addUInt8("CreatnScrptFird", 1);
    node->addUInt8("PM_IsDisguised", 0);
    node->addUInt8("Listening", 0);
    node->addList("ExpressionList", {});
    node->addFloat("XPosition", save.position.x);
    node->addFloat("YPosition", save.position.y);
    node->addFloat("ZPosition", save.position.z);
    node->addFloat("XOrientation", std::cos(save.yaw));
    node->addFloat("YOrientation", std::sin(save.yaw));
    node->addFloat("ZOrientation", 0.0f);
    node->addUInt32("JoiningXP", 0);
    node->addStruct("FollowInfo", std::make_shared<GffNode>(0));
    std::vector<GffNodePtr> effects;
    for (const ActiveEffect& source : save.effects) {
        if (source.remaining <= 0.0f) {
            continue;
        }
        auto effect = std::make_shared<GffNode>(0);
        effect->addInt32("SpellId", source.spellId);
        effect->addString("Label", source.label);
        effect->addFloat("Duration", source.remaining);
        effect->addInt32("Armor", source.armorBonus);
        effect->addInt32("Attack", source.attackBonus);
        effect->addInt32("Damage", source.damageBonus);
        effect->addInt32("Shield", source.shield);
        effect->addUInt8("Force", source.force ? 1u : 0u);
        effect->addUInt8("Dispellable", source.dispellable ? 1u : 0u);
        effects.push_back(std::move(effect));
    }
    node->addList("EffectList", std::move(effects));
    node->addFloat("ForceSuppressed", save.forceSuppressedTimer);
    node->addList("VarTable", {});
    node->addStruct("SWVarTable", std::make_shared<GffNode>(0));
    node->addList("ActionList", {});
    node->addUInt8("Commandable", 1);
    return node;
}

// A stored available-NPC creature: the player node shape with the NPC's own
// identity, class, abilities, powers, skills and worn items.
GffNodePtr buildNpcNode(const SavedNpc& npc)
{
    OriginalSave shape;
    shape.pcName = npc.name.empty() ? npc.tag : npc.name;
    shape.str = npc.str;
    shape.dex = npc.dex;
    shape.con = npc.con;
    shape.intel = npc.intel;
    shape.wis = npc.wis;
    shape.cha = npc.cha;
    shape.hp = npc.hp;
    shape.maxHp = npc.maxHp;
    shape.xp = npc.xp;
    shape.alignment = npc.alignment;
    shape.forcePoints = npc.forcePoints;
    shape.maxForcePoints = npc.maxForcePoints;
    shape.classId = npc.classId;
    shape.level = npc.level;
    shape.feats = npc.feats;
    shape.powers = npc.powers;
    shape.skills = npc.skills;
    GffNodePtr node = buildPlayerNode(shape);
    patchInt(*node, "IsPC", 0);
    {
        // Retail SaveStats 005b1b90: HitPoints is the base, CurrentHitPoints is base + current - maximum
        // (005afce0 adds the per-level share back on load), MaxHitPoints the maximum, PregameCurrent the
        // current value. The share is the Constitution modifier and the Toughness feats per level.
        const std::int32_t score = npc.con & 0xff;
        const std::int32_t conMod = (score + (score < 10 ? -11 : -10)) / 2;
        const auto has = [&](std::int32_t feat) {
            return std::find(npc.feats.begin(), npc.feats.end(), feat) != npc.feats.end();
        };
        const std::int32_t bonus = (has(0x7c) ? 2 : (has(0x54) ? 1 : 0)) + (has(0x5f) ? 2 : 0);
        const std::int32_t base = npc.baseHp >= 0
            ? npc.baseHp
            : std::max(0, npc.maxHp - std::max(1, npc.level) * (conMod + bonus));
        patchInt(*node, "HitPoints", base);
        patchInt(*node, "CurrentHitPoints", base + npc.hp - npc.maxHp);
        patchInt(*node, "PregameCurrent", npc.hp);
    }
    patchInt(*node, "Mod_IsPrimaryPlr", 0);
    node->remove("Mod_CommntyName");
    node->remove("Mod_FirstName");
    node->remove("Mod_LastName");
    patchString(*node, "Tag", npc.tag);
    patchResRef(*node, "TemplateResRef", npc.templateResref);
    patchInt(*node, "JoiningXP", npc.joiningXp);
    // Worn items carry their slot mask as the struct type, as a UTC does.
    std::vector<GffNodePtr> equipped;
    for (const SavedInventoryItem& item : npc.inventory) {
        if (!item.equipped) {
            continue;
        }
        const std::uint32_t mask = item.slot >= 0 && item.slot < 32 ? (1u << item.slot) : 0u;
        auto worn = std::make_shared<GffNode>(mask);
        worn->fields() = buildItemNode(item)->fields();
        if (item.resref.size() <= 16) { // A longer item tag is not a resref; the Tag field keeps it.
            worn->addResRef("EquippedRes", item.resref);
        }
        equipped.push_back(std::move(worn));
    }
    if (GffNodeField* list = node->find("Equip_ItemList"); list != nullptr) {
        list->list = std::move(equipped);
    }
    return node;
}

GffNodePtr buildPartyTable(const OriginalSave& save)
{
    auto node = std::make_shared<GffNode>(0);
    node->addInt32("PT_GOLD", save.gold);
    node->addInt32("PT_XP_POOL", save.xpPool);
    node->addUInt32("PT_PLAYEDSECONDS", static_cast<std::uint32_t>(save.playedSeconds));
    node->addInt32("PT_CONTROLLED_NP", save.controlledNpc);
    node->addUInt8("PT_SOLOMODE", save.solo ? 1u : 0u);
    node->addUInt8("PT_CHEAT_USED", 0);
    node->addUInt8("PT_NUM_MEMBERS", static_cast<std::uint32_t>(std::min<std::size_t>(save.party.size(), 255)));
    std::vector<GffNodePtr> members;
    for (const auto& [id, leader] : save.party) {
        auto member = std::make_shared<GffNode>(0);
        member->addInt32("PT_MEMBER_ID", id);
        member->addUInt8("PT_IS_LEADER", leader ? 1u : 0u);
        members.push_back(std::move(member));
    }
    node->addList("PT_MEMBERS", std::move(members));
    std::vector<GffNodePtr> available;
    for (const SavedNpc& slot : save.npcs) {
        auto npc = std::make_shared<GffNode>(0);
        npc->addUInt8("PT_NPC_AVAIL", slot.available ? 1u : 0u);
        npc->addUInt8("PT_NPC_SELECT", static_cast<std::uint32_t>(std::clamp(slot.select, 0, 255)));
        available.push_back(std::move(npc));
    }
    node->addList("PT_AVAIL_NPCS", std::move(available));
    node->addInt32("PT_AISTATE", 0);
    node->addInt32("PT_FOLLOWSTATE", 0);
    node->addStruct("GlxyMap", std::make_shared<GffNode>(0));
    // SaveTableInfo 005648c0: 18 INT counts and a 19th entry as BYTE, then the ten side cards.
    std::vector<GffNodePtr> pazaakCards;
    for (const std::int32_t count : save.pazaakCards) {
        auto entry = std::make_shared<GffNode>(0);
        entry->addInt32("PT_PAZAAKCOUNT", count);
        pazaakCards.push_back(std::move(entry));
    }
    auto unusedCount = std::make_shared<GffNode>(0);
    unusedCount->addUInt8("PT_PAZAAKCOUNT", 0);
    pazaakCards.push_back(std::move(unusedCount));
    node->addList("PT_PAZAAKCARDS", std::move(pazaakCards));
    std::vector<GffNodePtr> sideList;
    for (const std::int32_t kind : save.pazaakSideList) {
        auto entry = std::make_shared<GffNode>(0);
        entry->addInt32("PT_PAZSIDECARD", kind);
        sideList.push_back(std::move(entry));
    }
    node->addList("PT_PAZSIDELIST", std::move(sideList));
    node->addBinary("PT_TUT_WND_SHOWN", std::vector<std::uint8_t>(save.tutorialShown.begin(), save.tutorialShown.end()));
    node->addInt32("PT_LAST_GUI_PNL", 5);
    node->addList("PT_FB_MSG_LIST", {});
    node->addList("PT_DLG_MSG_LIST", {});
    node->addList("PT_COST_MULT_LIS", {});
    node->addInt32("JNL_SortOrder", 0);
    std::vector<GffNodePtr> journal;
    for (const auto& [plot, state] : save.journal) {
        auto entry = std::make_shared<GffNode>(0);
        entry->addString("JNL_PlotID", plot);
        entry->addInt32("JNL_State", state);
        entry->addUInt32("JNL_Date", 0);
        entry->addUInt32("JNL_Time", 0);
        journal.push_back(std::move(entry));
    }
    node->addList("JNL_Entries", std::move(journal));
    return node;
}

GffNodePtr buildGlobalVars(const OriginalSave& save)
{
    auto node = std::make_shared<GffNode>(0);
    std::vector<GffNodePtr> numberNames;
    std::vector<GffNodePtr> booleanNames;
    std::vector<GffNodePtr> stringNames;
    std::vector<GffNodePtr> locationNames;
    std::vector<std::uint8_t> numberBytes;
    std::vector<std::uint8_t> booleanBytes;
    std::vector<std::uint8_t> locationBytes;
    std::vector<GffNodePtr> stringValues;
    std::size_t booleanIndex = 0;
    for (const SavedGlobal& value : save.globals) {
        if (value.type == SavedGlobal::Type::Number) {
            auto name = std::make_shared<GffNode>(0);
            name->addString("Name", value.name);
            numberNames.push_back(std::move(name));
            // Retail 00529680 stores the low byte of the number (-128 to 127 as read back).
            numberBytes.push_back(static_cast<std::uint8_t>(value.number & 0xff));
        } else if (value.type == SavedGlobal::Type::Boolean) {
            auto name = std::make_shared<GffNode>(0);
            name->addString("Name", value.name);
            booleanNames.push_back(std::move(name));
            if ((booleanIndex / 8u) >= booleanBytes.size()) {
                booleanBytes.resize(booleanIndex / 8u + 1u, 0);
            }
            if (value.boolean) {
                booleanBytes[booleanIndex / 8u] |= static_cast<std::uint8_t>(
                    1u << (7u - static_cast<unsigned>(booleanIndex % 8u)));
            }
            ++booleanIndex;
        } else if (value.type == SavedGlobal::Type::Location) {
            auto name = std::make_shared<GffNode>(0);
            name->addString("Name", value.name);
            locationNames.push_back(std::move(name));
            for (const float component : value.location) {
                appendFloat(locationBytes, component);
            }
        } else {
            auto name = std::make_shared<GffNode>(0);
            name->addString("Name", value.name);
            stringNames.push_back(std::move(name));
            auto item = std::make_shared<GffNode>(0);
            item->addString("String", value.text);
            stringValues.push_back(std::move(item));
        }
    }
    node->addList("CatBoolean", std::move(booleanNames));
    node->addList("CatNumber", std::move(numberNames));
    node->addList("CatLocation", std::move(locationNames));
    node->addList("CatString", std::move(stringNames));
    // Retail CSWGlobalVariableTable save (005299b0): ValBoolean is count/8 + 1 bytes, ValNumber one
    // byte per number, and ValLocation a fixed 0x960 bytes (100 slots of 6 floats, unused ones zero).
    booleanBytes.resize(booleanIndex / 8u + 1u, 0);
    if (locationBytes.size() < 0x960u) {
        locationBytes.resize(0x960u, 0);
    }
    node->addBinary("ValBoolean", std::move(booleanBytes));
    node->addBinary("ValNumber", std::move(numberBytes));
    node->addBinary("ValLocation", std::move(locationBytes));
    node->addList("ValString", std::move(stringValues));
    return node;
}

GffNodePtr buildNfo(const OriginalSave& save)
{
    auto node = std::make_shared<GffNode>(0);
    node->addString("AREANAME", save.areaName);
    node->addString("LASTMODULE", save.module);
    node->addUInt32("TIMEPLAYED", static_cast<std::uint32_t>(save.timePlayed));
    node->addUInt8("CHEATUSED", 0);
    node->addString("SAVEGAMENAME", save.saveName);
    node->addInt32("GAMEPLAYHINT", 0);
    node->addInt32("STORYHINT", 0);
    for (int i = 1; i <= 6; ++i) {
        node->addString("LIVE" + std::to_string(i), "");
    }
    node->addInt32("LIVECONTENT", 0);
    node->addResRef("PORTRAIT0", save.portrait);
    node->addResRef("PORTRAIT1", save.partyPortrait1);
    node->addResRef("PORTRAIT2", save.partyPortrait2);
    return node;
}

GffNodePtr buildInventory(const OriginalSave& save)
{
    auto node = std::make_shared<GffNode>(0);
    std::vector<GffNodePtr> items;
    for (const SavedInventoryItem& source : save.inventory) {
        if (!source.equipped) {
            items.push_back(buildItemNode(source));
        }
    }
    node->addList("ItemList", std::move(items));
    return node;
}

GffNodePtr buildRepute()
{
    auto node = std::make_shared<GffNode>(0);
    node->addList("FactionList", {});
    node->addList("RepList", {});
    return node;
}

void patchIfo(GffNode& ifo, const OriginalSave& save)
{
    patchInt(ifo, "Mod_IsSaveGame", 1);
    patchInt(ifo, "Mod_IsNWMFile", 0);
    patchString(ifo, "Mod_Tag", save.module);
    patchString(ifo, "Mod_Entry_Area", save.area);
    patchFloat(ifo, "Mod_Entry_X", save.position.x);
    patchFloat(ifo, "Mod_Entry_Y", save.position.y);
    patchFloat(ifo, "Mod_Entry_Z", save.position.z);
    patchFloat(ifo, "Mod_Entry_Dir_X", std::cos(save.yaw));
    patchFloat(ifo, "Mod_Entry_Dir_Y", std::sin(save.yaw));
    if (GffNodeField* players = ifo.find("Mod_PlayerList"); players != nullptr) {
        players->type = GffType::List;
        players->list = {buildPlayerNode(save)};
    } else {
        ifo.addList("Mod_PlayerList", {buildPlayerNode(save)});
    }
}

void mergeTemplateFields(GffNode& target, const GffNode& source)
{
    for (const GffNodeField& field : source.fields()) {
        if (target.find(field.label) == nullptr) {
            target.fields().push_back(field);
        }
    }
}

std::uint32_t templateType(SavedObjectState::Kind kind)
{
    switch (kind) {
    case SavedObjectState::Kind::Creature: return kUtc;
    case SavedObjectState::Kind::Door: return 2042; // UTD
    case SavedObjectState::Kind::Placeable: return 2044; // UTP
    case SavedObjectState::Kind::Trigger: return 2032; // UTT
    case SavedObjectState::Kind::Store: return 2051; // UTM
    case SavedObjectState::Kind::Encounter: return 2040; // UTE
    default: return 0;
    }
}

void patchGit(GffNode& git, const OriginalSave& save, Install& install)
{
    git.remove("UseTemplates");
    // Retail 005061d0: the area map's explored bits, struct id 0x65, at the GIT's root.
    git.remove("AreaMap");
    if (save.areaMap.present && !save.areaMap.words.empty()) {
        auto map = std::make_shared<GffNode>(0x65);
        map->addInt32("AreaMapResX", save.areaMap.resX);
        map->addInt32("AreaMapResY", save.areaMap.resY);
        map->addUInt32("AreaMapDataSize", static_cast<std::uint32_t>(save.areaMap.words.size() * 4u));
        std::vector<std::uint8_t> bytes;
        bytes.reserve(save.areaMap.words.size() * 4u);
        for (const std::uint32_t word : save.areaMap.words) {
            for (int shift = 0; shift < 32; shift += 8) {
                bytes.push_back(static_cast<std::uint8_t>(word >> shift));
            }
        }
        map->addBinary("AreaMapData", std::move(bytes));
        git.addStruct("AreaMap", std::move(map));
    }
    if (save.areaVars.present) {
        setVarTable(git, save.areaVars);
    }
    struct ListSpec {
        const char* label;
        SavedObjectState::Kind kind;
    };
    const std::array<ListSpec, 8> lists = {{
        {"Creature List", SavedObjectState::Kind::Creature},
        {"Door List", SavedObjectState::Kind::Door},
        {"Placeable List", SavedObjectState::Kind::Placeable},
        {"TriggerList", SavedObjectState::Kind::Trigger},
        {"StoreList", SavedObjectState::Kind::Store},
        {"Encounter List", SavedObjectState::Kind::Encounter},
        {"WaypointList", SavedObjectState::Kind::Waypoint},
        {"SoundList", SavedObjectState::Kind::Sound},
    }};
    for (const ListSpec& spec : lists) {
        GffNodeField* listField = git.find(spec.label);
        if (listField == nullptr || listField->type != GffType::List) {
            continue;
        }
        for (const GffNodePtr& child : listField->list) {
            const GffNodeField* tagField = child->find("Tag");
            const std::string tag = tagField != nullptr ? tagField->text : std::string{};
            const SavedObjectState* state = nullptr;
            for (const SavedObjectState& candidate : save.objects) {
                if (candidate.kind == spec.kind && !candidate.tag.empty()
                    && lower(candidate.tag) == lower(tag)) {
                    state = &candidate;
                    break;
                }
            }
            if (state == nullptr) {
                continue;
            }
            const std::uint32_t type = templateType(spec.kind);
            if (type != 0 && !state->templateResref.empty()) {
                if (auto templateBytes = install.find(state->templateResref, type);
                    templateBytes.has_value()) {
                    try {
                        const Gff templateGff = Gff::parse(std::move(*templateBytes),
                                                          state->templateResref + ".template");
                        mergeTemplateFields(*child, *GffNode::fromGff(templateGff));
                    } catch (const FormatError&) {
                    }
                }
            }
            if (state->localTable) {
                SavedVarTable table;
                table.present = true;
                table.bits = state->localBits;
                table.bytes = state->localBytes;
                setVarTable(*child, table);
            }
            patchFloat(*child, "XPosition", state->position.x);
            patchFloat(*child, "YPosition", state->position.y);
            patchFloat(*child, "ZPosition", state->position.z);
            patchFloat(*child, "XOrientation", std::cos(state->bearing));
            patchFloat(*child, "YOrientation", std::sin(state->bearing));
            if (spec.kind == SavedObjectState::Kind::Creature) {
                patchInt(*child, "CurrentHitPoints", state->hitPoints);
                patchInt(*child, "MaxHitPoints", state->maxHitPoints);
                patchInt(*child, "Dead", state->dead ? 1 : 0);
            } else if (spec.kind == SavedObjectState::Kind::Door) {
                patchInt(*child, "OpenState", state->open ? 1 : 0);
                patchInt(*child, "Locked", state->locked ? 1 : 0);
            } else if (spec.kind == SavedObjectState::Kind::Placeable) {
                std::vector<GffNodePtr> items;
                for (const std::string& resref : state->loot) {
                    auto item = std::make_shared<GffNode>(0);
                    item->addResRef("InventoryRes", resref);
                    items.push_back(std::move(item));
                }
                if (GffNodeField* field = child->find("ItemList"); field != nullptr) {
                    field->type = GffType::List;
                    field->list = std::move(items);
                } else {
                    child->addList("ItemList", std::move(items));
                }
            }
        }
    }
}

} // namespace

OriginalSave readOriginalSave(const std::filesystem::path& folder,
                              const std::filesystem::path& rsvRoot)
{
    OriginalSave save;
    save.folder = folder;
    const std::filesystem::path outerPath = folder / "SAVEGAME.sav";
    const std::filesystem::path nfoPath = folder / "savenfo.res";
    const std::filesystem::path ptPath = folder / "PARTYTABLE.res";
    const std::filesystem::path gvPath = folder / "GLOBALVARS.res";

    if (std::filesystem::is_regular_file(nfoPath)) {
        readNfo(Gff::parse(readFile(nfoPath), "savenfo.res"), save);
    }
    if (std::filesystem::is_regular_file(ptPath)) {
        readParty(Gff::parse(readFile(ptPath), "PARTYTABLE.res"), save);
    }
    if (std::filesystem::is_regular_file(gvPath)) {
        readGlobals(Gff::parse(readFile(gvPath), "GLOBALVARS.res"), save);
    }
    if (!std::filesystem::is_regular_file(outerPath)) {
        throw FormatError(folder.string() + ": SAVEGAME.sav is missing");
    }
    const std::vector<ErfEntry> outer = parseErf(readFile(outerPath), "SAVEGAME.sav");
    const ErfEntry* inventoryEntry = findEntry(outer, "INVENTORY");
    if (inventoryEntry != nullptr) {
        save.inventory = readInventory(Gff::parse(inventoryEntry->data, "INVENTORY"));
    }
    if (const ErfEntry* repute = findEntry(outer, "REPUTE"); repute != nullptr) {
        save.repute = repute->data;
    }
    const ErfEntry* moduleEntry = nullptr;
    for (const ErfEntry& entry : outer) {
        if (entry.type == kSav) {
            if (save.module.empty() || lower(entry.resref) == lower(save.module)) {
                moduleEntry = &entry;
                if (lower(entry.resref) == lower(save.module)) {
                    break;
                }
            }
            if (moduleEntry == nullptr) {
                moduleEntry = &entry;
            }
        }
    }
    for (const ErfEntry& entry : outer) {
        const std::string name = lower(entry.resref);
        const bool known = entry.type == kSav || name == "inventory" || name == "repute";
        if (!known && name.size() == 9 && name.starts_with("availnpc") && name[8] >= '0'
            && name[8] <= '8' && entry.type == kUtc) {
            // A stored available-NPC creature is modelled (SavedNpc) and rewritten
            // from the party table; an unreadable one is preserved verbatim.
            try {
                SavedNpc stored = readNpcNode(Gff::parse(entry.data, entry.resref));
                SavedNpc& slot = save.npcs[static_cast<std::size_t>(name[8] - '0')];
                stored.available = slot.available;
                stored.select = slot.select;
                slot = std::move(stored);
                continue;
            } catch (const FormatError&) {
                save.preservedOuter.push_back({entry.resref, entry.type, entry.data});
                continue;
            }
        }
        if (!known && (name.starts_with("availnpc") || name == "pc"
                       || name == "partytable" || name == "globalvars")) {
            save.preservedOuter.push_back({entry.resref, entry.type, entry.data});
        }
    }
    if (moduleEntry == nullptr) {
        throw FormatError(folder.string() + ": SAVEGAME.sav has no nested module ERF");
    }
    for (const ErfEntry& entry : outer) {
        if (entry.type == kSav && lower(entry.resref) != lower(moduleEntry->resref)) {
            save.preservedModules.push_back({entry.resref, entry.type, entry.data});
        }
    }
    std::vector<ErfEntry> nested =
        parseErf(moduleEntry->data, moduleEntry->resref + ".sav");
    // Retail hazards: an external <module>.rsv can shadow the nested saved
    // module with no provenance check. Treat it as the cached module ERF.
    if (!rsvRoot.empty()) {
        std::error_code rsvError;
        if (std::filesystem::is_directory(rsvRoot, rsvError)) {
            for (const std::filesystem::directory_entry& item :
                 std::filesystem::directory_iterator(rsvRoot, rsvError)) {
                if (rsvError || !item.is_regular_file()
                    || item.path().extension() != ".rsv") {
                    continue;
                }
                if (lower(item.path().stem().string()) != lower(moduleEntry->resref)) {
                    continue;
                }
                try {
                    const Archive archive = Archive::open(item.path());
                    std::vector<ErfEntry> cached;
                    for (const ArchiveEntry& entry : archive.entries()) {
                        cached.push_back({entry.resref, entry.type, archive.read(entry)});
                    }
                    if (findEntryType(cached, kIfo) != nullptr) {
                        nested = std::move(cached);
                    }
                } catch (const FormatError&) {
                }
                break;
            }
        }
    }
    const ErfEntry* ifoEntry = findEntryType(nested, kIfo);
    const ErfEntry* areEntry = findEntryType(nested, kAre);
    const ErfEntry* gitEntry = findEntryType(nested, kGit);
    if (ifoEntry == nullptr) {
        throw FormatError(folder.string() + ": saved module has no IFO");
    }
    const Gff ifo = Gff::parse(ifoEntry->data, moduleEntry->resref + ".ifo");
    save.module = moduleEntry->resref;
    if (save.module.empty()) {
        save.module = text(ifo, ifo.rootIndex(), "Mod_Tag");
    }
    save.area = text(ifo, ifo.rootIndex(), "Mod_Entry_Area");
    if (save.area.empty() && areEntry != nullptr) {
        const Gff are = Gff::parse(areEntry->data, moduleEntry->resref + ".are");
        save.area = text(are, are.rootIndex(), "Tag");
    }
    if (save.areaName.empty()) {
        save.areaName = save.area;
    }
    save.position = {numberFloat(ifo, ifo.rootIndex(), "Mod_Entry_X", 0.0f),
                     numberFloat(ifo, ifo.rootIndex(), "Mod_Entry_Y", 0.0f),
                     numberFloat(ifo, ifo.rootIndex(), "Mod_Entry_Z", 0.0f)};
    save.yaw = std::atan2(numberFloat(ifo, ifo.rootIndex(), "Mod_Entry_Dir_Y", 1.0f),
                          numberFloat(ifo, ifo.rootIndex(), "Mod_Entry_Dir_X", 0.0f));
    readEventQueue(ifo, save);
    const std::vector<std::uint32_t> players = list(ifo, ifo.rootIndex(), "Mod_PlayerList");
    if (!players.empty()) {
        readPlayer(ifo, players.front(), save);
        // The PC stands where the player struct says (XPosition and XOrientation of Mod_PlayerList), not at
        // the module's entry point: Mod_Entry_* only names the arrival spot (the Taris save differs).
        if (has(ifo, players.front(), "XPosition")) {
            save.position = objectPosition(ifo, players.front());
            if (has(ifo, players.front(), "XOrientation")) {
                save.yaw = std::atan2(numberFloat(ifo, players.front(), "YOrientation", 1.0f),
                                      numberFloat(ifo, players.front(), "XOrientation", 0.0f));
            }
        }
        // Equipped items live in the player's Equip_ItemList (slot mask in the struct type).
        for (SavedInventoryItem& item : readItems(ifo, field(ifo, players.front(), "Equip_ItemList"), true)) {
            save.inventory.push_back(std::move(item));
        }
    }
    if (gitEntry != nullptr) {
        const Gff git = Gff::parse(gitEntry->data, moduleEntry->resref + ".git");
        save.gitBytes = gitEntry->data;
        if (const GffField* useTemplates = git.field(git.rootIndex(), "UseTemplates")) {
            save.hasUseTemplates = true;
            save.useTemplates = git.integer(*useTemplates) != 0;
        }
        readObjects(git, save);
        // Retail 00505da0.
        if (const GffField* map = git.field(git.rootIndex(), "AreaMap"); map != nullptr && map->type == GffType::Struct) {
            const std::uint32_t at = git.structIndex(*map);
            save.areaMap.present = true;
            save.areaMap.resX = numberInt(git, at, "AreaMapResX", 0);
            save.areaMap.resY = numberInt(git, at, "AreaMapResY", 0);
            const std::vector<std::uint8_t> bytes = binary(git, at, "AreaMapData");
            save.areaMap.words.assign(bytes.size() / 4u, 0u);
            for (std::size_t i = 0; i < save.areaMap.words.size(); ++i) {
                save.areaMap.words[i] = static_cast<std::uint32_t>(bytes[i * 4u]) | static_cast<std::uint32_t>(bytes[i * 4u + 1u]) << 8
                    | static_cast<std::uint32_t>(bytes[i * 4u + 2u]) << 16 | static_cast<std::uint32_t>(bytes[i * 4u + 3u]) << 24;
            }
        }
    }
    if (save.journal.empty() && save.gold == 0) {
        // Some unusual saves embed PARTYTABLE inside the outer ERF.
        if (const ErfEntry* embedded = findEntry(outer, "PARTYTABLE"); embedded != nullptr) {
            readParty(Gff::parse(embedded->data, "PARTYTABLE(embedded)"), save);
        }
    }
    if (save.globals.empty()) {
        if (const ErfEntry* embedded = findEntry(outer, "GLOBALVARS"); embedded != nullptr) {
            readGlobals(Gff::parse(embedded->data, "GLOBALVARS(embedded)"), save);
        }
    }
    return save;
}

bool writeOriginalSave(const std::filesystem::path& folder, const OriginalSave& save,
                       Install& install, bool overwrite)
{
    if (folder.empty()) {
        return false;
    }
    if (std::filesystem::exists(folder) && !overwrite) {
        const bool empty = std::filesystem::is_empty(folder);
        if (!empty) {
            return false;
        }
    }
    std::filesystem::create_directories(folder);

    auto ifoData = install.find("module", kIfo);
    auto areData = install.find(save.area, kAre);
    auto gitData = install.find(save.area, kGit);
    if (!ifoData.has_value() || !areData.has_value() || !gitData.has_value()) {
        throw FormatError("cannot resolve module/area templates for " + save.module + "/" + save.area);
    }
    Gff ifo = Gff::parse(std::move(*ifoData), save.module + ".ifo");
    Gff are = Gff::parse(std::move(*areData), save.area + ".are");
    Gff git = Gff::parse(std::move(*gitData), save.area + ".git");
    GffNodePtr ifoNode = GffNode::fromGff(ifo);
    GffNodePtr areNode = GffNode::fromGff(are);
    GffNodePtr gitNode = GffNode::fromGff(git);
    patchIfo(*ifoNode, save);
    patchGit(*gitNode, save, install);

    std::vector<ErfWriteEntry> nested;
    // Retail save order: area GIT, area ARE, then Module IFO.
    nested.push_back({save.area, kGit, writeGff("GIT ", *gitNode)});
    nested.push_back({save.area, kAre, writeGff("ARE ", *areNode)});
    nested.push_back({"Module", kIfo, writeGff("IFO ", *ifoNode)});
    const std::vector<std::uint8_t> nestedBytes = writeErf(nested, "MOD ", "V1.0");

    std::vector<ErfWriteEntry> outer;
    outer.push_back({save.module, kSav, nestedBytes});
    for (const SavedResource& module : save.preservedModules) {
        if (lower(module.resref) != lower(save.module)) {
            outer.push_back({module.resref, kSav, module.data});
        }
    }
    outer.push_back({"INVENTORY", kInv, writeGff("INV ", *buildInventory(save))});
    outer.push_back({"REPUTE", kFac, save.repute.empty()
                                          ? writeGff("FAC ", *buildRepute())
                                          : save.repute});
    for (const SavedResource& extra : save.preservedOuter) {
        outer.push_back({extra.resref, extra.type, extra.data});
    }
    for (std::size_t i = 0; i < save.npcs.size(); ++i) {
        if (save.npcs[i].hasCreature) {
            outer.push_back({"AVAILNPC" + std::to_string(i), kUtc,
                             writeGff("UTC ", *buildNpcNode(save.npcs[i]))});
        }
    }
    const std::vector<std::uint8_t> outerBytes = writeErf(outer, "MOD ", "V1.0");

    writeFile(folder / "SAVEGAME.sav", outerBytes);
    writeFile(folder / "PARTYTABLE.res", writeGff("PT  ", *buildPartyTable(save)));
    writeFile(folder / "GLOBALVARS.res", writeGff("GVT ", *buildGlobalVars(save)));
    writeFile(folder / "savenfo.res", writeGff("NFO ", *buildNfo(save)));
    writeFile(folder / "Screen.tga", save.screenTga.empty() ? makeSaveThumbnail() : save.screenTga);
    return true;
}

OriginalSaveCheck checkOriginalSave(const std::filesystem::path& folder)
{
    OriginalSaveCheck check;
    try {
        const OriginalSave save = readOriginalSave(folder);
        if (save.module.empty()) {
            check.errors.push_back("module is empty");
        }
        if (save.area.empty()) {
            check.errors.push_back("area is empty");
        }
        if (save.maxHp <= 0) {
            check.errors.push_back("invalid player hit points");
        }
        if (!std::filesystem::is_regular_file(folder / "Screen.tga")) {
            check.warnings.push_back("Screen.tga is missing");
        }
        check.ok = check.errors.empty();
    } catch (const std::exception& exception) {
        check.errors.push_back(exception.what());
    }
    return check;
}

std::vector<std::uint8_t> encodeSaveScreenshot(const std::uint8_t* rgba, int width, int height)
{
    if (rgba == nullptr || width <= 0 || height <= 0) {
        return {};
    }
    constexpr int size = 256;
    std::vector<std::uint8_t> out;
    out.reserve(18u + static_cast<std::size_t>(size) * size * 3u);
    const std::uint8_t header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                     size & 0xFF, size >> 8, size & 0xFF, size >> 8, 24, 0x20};
    out.insert(out.end(), header, header + 18);
    for (int y = 0; y < size; ++y) {
        const int sy0 = y * height / size;
        const int sy1 = std::max(sy0 + 1, (y + 1) * height / size);
        for (int x = 0; x < size; ++x) {
            const int sx0 = x * width / size;
            const int sx1 = std::max(sx0 + 1, (x + 1) * width / size);
            unsigned sum[3] = {0, 0, 0};
            unsigned count = 0;
            for (int sy = sy0; sy < sy1 && sy < height; ++sy) {
                for (int sx = sx0; sx < sx1 && sx < width; ++sx) {
                    const std::uint8_t* px = rgba + (static_cast<std::size_t>(sy) * width + sx) * 4u;
                    sum[0] += px[0];
                    sum[1] += px[1];
                    sum[2] += px[2];
                    ++count;
                }
            }
            count = std::max(1u, count);
            out.push_back(static_cast<std::uint8_t>(sum[2] / count)); // B
            out.push_back(static_cast<std::uint8_t>(sum[1] / count)); // G
            out.push_back(static_cast<std::uint8_t>(sum[0] / count)); // R
        }
    }
    return out;
}

std::vector<std::uint8_t> makeSaveThumbnail()
{
    constexpr int width = 256;
    constexpr int height = 256;
    std::vector<std::uint8_t> out;
    out.reserve(18u + static_cast<std::size_t>(width) * height * 3u);
    out.push_back(0); // id length
    out.push_back(0); // colour map type
    out.push_back(2); // uncompressed true-colour
    for (int i = 0; i < 5; ++i) {
        out.push_back(0);
    }
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(static_cast<std::uint8_t>(width & 0xFF));
    out.push_back(static_cast<std::uint8_t>((width >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(height & 0xFF));
    out.push_back(static_cast<std::uint8_t>((height >> 8) & 0xFF));
    out.push_back(24);
    out.push_back(0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::uint8_t r = static_cast<std::uint8_t>(20 + (x * 60) / width);
            const std::uint8_t g = static_cast<std::uint8_t>(30 + (y * 50) / height);
            const std::uint8_t b = static_cast<std::uint8_t>(60 + ((x + y) * 80) / (width + height));
            out.push_back(b);
            out.push_back(g);
            out.push_back(r);
        }
    }
    return out;
}

bool savedEffectApplies(const SavedEffect& effect)
{
    return !effect.skipOnLoad && (effect.subType & 7) != 3;
}

std::int32_t savedEffectScriptType(std::int32_t internalType)
{
    // Internal type -> nwscript EFFECT_TYPE_ constant: the straight comparisons of 00503a70.
    static const std::array<std::pair<std::int32_t, std::int32_t>, 56> kTable = {{
        {2, 1}, {7, 3}, {12, 7}, {15, 9}, {18, 11}, {21, 13}, {22, 15}, {24, 17}, {25, 18}, {31, 20},
        {32, 21}, {35, 31}, {5, 32}, {45, 33}, {46, 34}, {1, 36}, {3, 37}, {36, 38}, {37, 39}, {10, 40},
        {11, 41}, {13, 42}, {14, 43}, {16, 44}, {17, 45}, {48, 46}, {49, 47}, {28, 48}, {29, 49}, {26, 50},
        {27, 51}, {33, 52}, {34, 53}, {55, 54}, {56, 55}, {47, 56}, {74, 58}, {51, 59}, {61, 60}, {82, 61},
        {62, 62}, {63, 63}, {72, 64}, {70, 65}, {64, 66}, {73, 67}, {65, 68}, {52, 69}, {71, 70}, {75, 71},
        {76, 72}, {50, 73}, {101, 74}, {30, 75}, {100, 76}, {102, 77},
    }};
    for (const auto& [internal, script] : kTable) {
        if (internal == internalType) {
            return script;
        }
    }
    return -1;
}

std::vector<SavedEffect> readSavedEffects(const Gff& gff, std::uint32_t structure)
{
    std::vector<SavedEffect> effects;
    for (const std::uint32_t entry : list(gff, structure, "EffectList")) {
        SavedEffect effect;
        // DWORD fields are read exactly (number() goes through float); the 64-bit Id is not exposed by Gff.
        const auto exact = [&](const char* label) {
            const GffField* value = field(gff, entry, label);
            return value != nullptr ? value->data : 0u;
        };
        effect.type = numberInt(gff, entry, "Type", 0);
        effect.subType = numberInt(gff, entry, "SubType", 0);
        effect.duration = numberFloat(gff, entry, "Duration", 0.0f);
        effect.skipOnLoad = numberInt(gff, entry, "SkipOnLoad", 0) != 0;
        effect.expireDay = exact("ExpireDay");
        effect.expireTime = exact("ExpireTime");
        effect.creatorId = exact("CreatorId");
        effect.spellId = field(gff, entry, "SpellId") != nullptr ? exact("SpellId") : 0xFFFFFFFFu;
        effect.isExposed = numberInt(gff, entry, "IsExposed", 0) != 0;
        const std::int32_t count = numberInt(gff, entry, "NumIntegers", 0);
        std::vector<std::int32_t> saved;
        for (const std::uint32_t item : list(gff, entry, "IntList")) {
            saved.push_back(numberInt(gff, item, "Value", 0));
        }
        if (effect.type == 35 || effect.type == 38) {
            // LoadGameEffect pads these two types to 21 integers: entries below NumIntegers - 6 and from the 16th on
            // come from the list, the ones between are zero.
            effect.ints.assign(21, 0);
            std::size_t at = 0;
            for (std::int32_t step = 0; step < 21; ++step) {
                if (step < count - 6) {
                    if (at < saved.size()) {
                        effect.ints[at] = saved[at];
                        ++at;
                    }
                } else if (step >= 15) {
                    if (at < saved.size()) {
                        effect.ints[at] = saved[at];
                        ++at;
                    }
                } else {
                    effect.ints[at < 21 ? at : 20] = 0;
                }
            }
        } else if (count > 0) {
            effect.ints = saved;
        }
        for (const std::uint32_t item : list(gff, entry, "FloatList")) {
            if (effect.floats.size() < 4) {
                effect.floats.push_back(numberFloat(gff, item, "Value", 0.0f));
            }
        }
        for (const std::uint32_t item : list(gff, entry, "StringList")) {
            if (effect.strings.size() < 6) {
                effect.strings.push_back(text(gff, item, "Value"));
            }
        }
        for (const std::uint32_t item : list(gff, entry, "ObjectList")) {
            if (effect.objects.size() < 4) {
                const GffField* object = field(gff, item, "Value");
                effect.objects.push_back(object != nullptr ? object->data : 0u);
            }
        }
        effects.push_back(std::move(effect));
    }
    return effects;
}

} // namespace ke::kotor
