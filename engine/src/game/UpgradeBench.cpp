#include "game/GameSession.h"
#include "game/ScriptHost.h"

#include "core/Log.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>

// The upgrade bench: ShowUpgradeScreen (retail 00543990 -> 0062e760) opens the upgrade screens
// (upgradesel, upgradeitems, upgrade). What ships: upgrade.2da maps an upgrade item's template to
// an upgrade type (0 saber crystals, 1..3 melee, 4..7 ranged, 8..9 armor), upgradetypes.2da names
// the types, and a weapon or armor UTI lists, in its PropertiesList, one property per upgrade row it
// can take, each tagged with that row in UpgradeType (255 for the item's own properties). The save
// stores which are installed as the item's `Upgrades` bit mask (bit N = upgrade.2da row N).
//
// What is NOT modelled: the property an installed upgrade adds is not applied to combat, because the
// engine applies no weapon or armor properties anywhere; installing changes the item (what it shows,
// what is saved) and spends the upgrade item, nothing more.

namespace ke {

namespace {

std::string lowerOf(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

const std::vector<GameSession::UpgradeRow>& GameSession::upgradeRows()
{
    if (!upgradeRowsLoaded_) {
        upgradeRowsLoaded_ = true;
        if (auto data = install().find("upgrade", kotor::resType(kotor::ResType::TwoDa)); data.has_value()) {
            try {
                const kotor::TwoDa table = kotor::TwoDa::parse(std::move(*data), "upgrade.2da");
                for (std::size_t row = 0; row < table.rowCount(); ++row) {
                    const std::string index = std::to_string(row);
                    UpgradeRow entry;
                    entry.label = table.cell(index, "label");
                    entry.templ = lowerOf(table.cell(index, "template"));
                    entry.type = std::atoi(table.cell(index, "upgradetype").c_str());
                    upgradeRows_.push_back(std::move(entry));
                }
            } catch (const kotor::FormatError&) {
            }
        }
    }
    return upgradeRows_;
}

int GameSession::upgradeRowOf(const std::string& resref)
{
    const std::string wanted = lowerOf(resref);
    const std::vector<UpgradeRow>& rows = upgradeRows();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].templ == wanted) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const kotor::ItemTemplate* GameSession::upgradeTemplate(const std::string& resref)
{
    auto cached = itemTemplates_.find(resref);
    if (cached == itemTemplates_.end()) {
        if (auto loaded = kotor::loadItemTemplate(install(), resref); loaded.has_value()) {
            cached = itemTemplates_.emplace(resref, std::move(*loaded)).first;
        } else {
            return nullptr;
        }
    }
    return &cached->second;
}

std::vector<GameSession::UpgradeSlot> GameSession::upgradeSlots(const InventoryItem& item)
{
    std::vector<UpgradeSlot> slots;
    const kotor::ItemTemplate* templ = upgradeTemplate(item.resref);
    if (templ == nullptr) {
        return slots;
    }
    const std::vector<UpgradeRow>& rows = upgradeRows();
    std::map<int, UpgradeSlot> byType;
    for (const kotor::ItemTemplate::Property& property : templ->properties) {
        if (property.upgradeType < 0 || static_cast<std::size_t>(property.upgradeType) >= rows.size()) {
            continue;
        }
        const int type = rows[static_cast<std::size_t>(property.upgradeType)].type;
        UpgradeSlot& slot = byType[type];
        slot.type = type;
        if (std::find(slot.rows.begin(), slot.rows.end(), property.upgradeType) == slot.rows.end()) {
            slot.rows.push_back(property.upgradeType);
        }
    }
    for (auto& [type, slot] : byType) {
        std::sort(slot.rows.begin(), slot.rows.end());
        slots.push_back(std::move(slot));
    }
    return slots;
}

int GameSession::upgradeCategory(const InventoryItem& item)
{
    const std::vector<UpgradeSlot> slots = upgradeSlots(item);
    if (slots.empty()) {
        return -1;
    }
    const int type = slots.front().type;
    if (type == 0) {
        return 0; // lightsaber
    }
    if (type >= 4 && type <= 7) {
        return 1; // ranged
    }
    if (type >= 1 && type <= 3) {
        return 2; // melee
    }
    return 3; // armor
}

int GameSession::upgradeInstalled(const InventoryItem& item, int slotType)
{
    const std::vector<UpgradeRow>& rows = upgradeRows();
    for (std::size_t row = 0; row < rows.size() && row < 32; ++row) {
        if ((item.upgrades >> row & 1u) != 0 && rows[row].type == slotType) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

bool GameSession::assembleUpgrades(std::size_t inventoryIndex, const std::map<int, int>& rowPerType)
{
    if (inventoryIndex >= character_.inventory.size()) {
        return false;
    }
    const std::vector<UpgradeRow>& rows = upgradeRows();
    // Spending and returning upgrade items moves the pack's rows: the target is kept by its object.
    const InventoryItem target = character_.inventory[inventoryIndex];
    const std::vector<UpgradeSlot> slots = upgradeSlots(target);
    std::uint32_t bits = target.upgrades;
    bool changed = false;
    for (const UpgradeSlot& slot : slots) {
        const auto wanted = rowPerType.find(slot.type);
        if (wanted == rowPerType.end()) {
            continue;
        }
        InventoryItem probe = target;
        probe.upgrades = bits;
        const int current = upgradeInstalled(probe, slot.type);
        const int desired = wanted->second;
        if (desired == current) {
            continue;
        }
        if (desired >= 0) {
            if (static_cast<std::size_t>(desired) >= rows.size()
                || std::find(slot.rows.begin(), slot.rows.end(), desired) == slot.rows.end()) {
                continue; // not a row this slot takes
            }
            // The new upgrade item is spent first: nothing changes when the pack does not hold it.
            if (!removeInventoryItem(rows[static_cast<std::size_t>(desired)].templ, 1)) {
                continue;
            }
            bits |= 1u << desired;
        }
        if (current >= 0) {
            // The old upgrade comes back to the pack.
            const std::string& templ = rows[static_cast<std::size_t>(current)].templ;
            const kotor::ItemTemplate* item = upgradeTemplate(templ);
            addInventoryItem(templ, item != nullptr ? item->baseItem : 64, 1, item != nullptr ? item->cost : 0);
            bits &= ~(1u << current);
        }
        changed = true;
    }
    if (changed) {
        for (InventoryItem& row : character_.inventory) {
            if (row.object == target.object && lowerOf(row.resref) == lowerOf(target.resref)) {
                row.upgrades = bits;
                break;
            }
        }
        syncInventoryObjects(); // the item object carries the bits too (the next sync reads them)
        logEvent("upgrades " + target.resref + " = " + std::to_string(bits));
    }
    return changed;
}

bool GameSession::upgradeScreenOpen() const { return sim_->presentation.upgradeScreen; }
std::uint32_t GameSession::upgradeTarget() const { return sim_->presentation.upgradeItem; }
void GameSession::closeUpgradeScreen()
{
    sim_->presentation.upgradeScreen = false;
    sim_->presentation.upgradeItem = script::kObjectInvalid;
}
void GameSession::openUpgradeScreenForTest(std::uint32_t item)
{
    sim_->presentation.upgradeItem = item;
    sim_->presentation.upgradeScreen = true;
}

} // namespace ke
