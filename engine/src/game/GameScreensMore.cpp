#include "game/ChargenGui.h"
#include "game/GameScreens.h"

#include "core/Log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>

// The rest of the in-game shipped screens: equipment, abilities, messages and the galaxy
// map. Same pattern as GameScreens.cpp: the session keeps the model, the screen binds the
// shipped .gui to it and turns control actions into session calls.

namespace ke {

namespace {

std::optional<kotor::TwoDa> table(kotor::Install& install, const char* name)
{
    auto data = install.find(name, kotor::resType(kotor::ResType::TwoDa));
    if (!data.has_value()) {
        return std::nullopt;
    }
    try {
        return kotor::TwoDa::parse(std::move(*data), name);
    } catch (const kotor::FormatError&) {
        return std::nullopt;
    }
}

std::string cellOf(const std::optional<kotor::TwoDa>& data, std::size_t row, const char* column)
{
    if (!data || row >= data->rowCount()) {
        return {};
    }
    const int index = data->columnIndex(column);
    if (index < 0) {
        return {};
    }
    const std::string value = data->cell(row, static_cast<std::size_t>(index));
    return value == "****" ? std::string{} : value;
}

std::uint32_t strrefOf(const std::string& cell)
{
    return cell.empty() ? UINT32_MAX : static_cast<std::uint32_t>(std::strtoul(cell.c_str(), nullptr, 10));
}

std::string lowerOfText(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string alnumLower(const std::string& text)
{
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) != 0) {
            out += static_cast<char>(std::tolower(c));
        }
    }
    return out;
}

// The equipment screen's nine slots: the control suffix, the inventory slot index
// (INVENTORY_SLOT_*) and the slot's TLK name.
struct EquipSlot {
    const char* suffix;
    int index;
    std::uint32_t name;
    // What an empty slot shows: the slot's own silhouette from swpc_tex_gui.erf (the shipped
    // LBL_INV_* labels carry no fill; behaviour confirmed by the art's names and the third-party
    // note in the report: kotorJS/xoreos pick the same names).
    const char* empty;
};
constexpr EquipSlot kEquipSlots[] = {
    {"HEAD", 0, 31375, "ihead"},          {"IMPLANT", 9, 31388, "iimplant"},   {"BODY", 1, 31380, "iarmor"},
    {"ARM_L", 7, 31376, "iforearm_l"},    {"WEAP_L", 5, 31378, "iweap_l"},     {"BELT", 10, 31382, "ibelt"},
    {"WEAP_R", 4, 31379, "iweap_r"},      {"ARM_R", 8, 31377, "iforearm_r"},   {"HANDS", 3, 31383, "ihands"},
};

constexpr std::size_t kMaxLogLines = 200;

std::string signedNumber(int value)
{
    return (value >= 0 ? "+" : "") + std::to_string(value);
}

} // namespace

// ---- helpers --------------------------------------------------------------------

std::string GameScreens::listTag() const
{
    if (bound_ == "store") {
        return sellMode_ ? "LB_INVITEMS" : "LB_SHOPITEMS";
    }
    if (bound_ == "abilities") {
        return "LB_ABILITY";
    }
    if (bound_ == "messages") {
        return showDialogLog_ ? "LB_DIALOG" : "LB_MESSAGES";
    }
    if (bound_ == "ftchrgen") {
        return "LB_FEATS";
    }
    return "LB_ITEMS";
}

void GameScreens::recordDialogue()
{
    // What the PC said when a reply was chosen goes in before the entry it led to (retail logs the reply's
    // line at its start, AppendToDialogBuffer 00630560 from 006bd260), also when the reply ended the talk.
    for (std::string& spoken : game_.takeSpokenReplies()) {
        if (spoken.find_first_not_of(' ', spoken.find(':') + 1) == std::string::npos) {
            continue;
        }
        dialogueLog_.push_back(std::move(spoken));
        if (dialogueLog_.size() > kMaxLogLines) {
            dialogueLog_.erase(dialogueLog_.begin());
        }
    }
    const DialogueView& view = game_.dialogue();
    if (!view.active) {
        lastDialogueEntry_ = UINT32_MAX;
        return;
    }
    if (view.entry == lastDialogueEntry_ || view.text.empty()) {
        return;
    }
    lastDialogueEntry_ = view.entry;
    // AppendToDialogBuffer (00630560) logs a line only when its text is not empty and the speaker is a game
    // object; the opening cutscene's blank (one-space) entries never reach retail's Messages list.
    if (view.text.find_first_not_of(' ') == std::string::npos) {
        return;
    }
    dialogueLog_.push_back(view.speaker.empty() ? view.text : view.speaker + ": " + view.text);
    if (dialogueLog_.size() > kMaxLogLines) {
        dialogueLog_.erase(dialogueLog_.begin());
    }
}

// The item an equip-list row stands for: the worn one (row 0 in the worn mode, which may be a
// companion's own row) or a carried one from the shared list.
const InventoryItem* GameScreens::rowItem(std::size_t row) const
{
    if (row >= equipRows_.size()) {
        return nullptr;
    }
    if (row == 0) {
        return nullptr; // "None"
    }
    if (equipWornRow_ && row == 1) {
        return itemByObject(equipWornObject_);
    }
    return equipRows_[row] < game_.inventory().size() ? &game_.inventory()[equipRows_[row]] : nullptr;
}

const InventoryItem* GameScreens::itemByObject(std::uint32_t object) const
{
    if (object == 0 || object == script::kObjectInvalid) {
        return nullptr;
    }
    for (const InventoryItem& item : game_.inventory()) {
        if (item.object == object) {
            return &item;
        }
    }
    // What a companion wears is that companion's own row (the item model keeps it off the shared list).
    for (const CompanionState& companion : game_.companions()) {
        for (const InventoryItem& item : companion.sheet.inventory) {
            if (item.object == object) {
                return &item;
            }
        }
    }
    return nullptr;
}

std::string GameScreens::moreStateKey() const
{
    std::string key;
    if (bound_ == "equip") {
        const std::uint32_t pc = viewedObject();
        key += "|" + std::to_string(equipSlot_) + (equipSelecting_ ? "s" : "p");
        for (const EquipSlot& slot : kEquipSlots) {
            key += "," + std::to_string(game_.wornItemObject(pc, slot.index));
        }
    } else if (bound_ == "abilities") {
        const CharacterSheet& sheet = viewedSheet();
        key += "|" + std::to_string(abilityTab_) + "," + std::to_string(sheet.feats.size()) + ","
            + std::to_string(sheet.powers.size()) + "," + std::to_string(sheet.level);
    } else if (bound_ == "messages") {
        key += "|" + std::to_string(showDialogLog_) + "," + std::to_string(dialogueLog_.size()) + ","
            + std::to_string(game_.feedbackLog().size());
    } else if (!levelHubScreen_.empty() && bound_ == levelHubScreen_) {
        const CharacterSheet& sheet = game_.character();
        key += "|hub" + std::to_string(levelLit_) + "," + std::to_string(sheet.level) + "," + std::to_string(sheet.maxHp)
            + "," + std::to_string(sheet.str + sheet.dex + sheet.con + sheet.intel + sheet.wis + sheet.cha);
    } else if (bound_ == "abchrgen" || bound_ == "skchrgen" || bound_ == "ftchrgen" || bound_ == "pwrlvlup") {
        const CharacterSheet& sheet = game_.character();
        key += "|" + std::to_string(levelStep_) + "," + std::to_string(levelHover()) + ","
            + std::to_string(sheet.attributePoints) + "," + std::to_string(sheet.skillPoints) + ","
            + std::to_string(sheet.featPoints) + "," + std::to_string(sheet.feats.size()) + ","
            + std::to_string(gui_.listCellValue("LB_FEATS")) + ","
            + std::to_string(sheet.powerPoints) + "," + std::to_string(sheet.powers.size());
    } else if (bound_ == "galaxymap") {
        key += "|" + std::to_string(planet_) + "," + std::to_string(game_.galaxyMap().highlighted);
    } else if (bound_.rfind("upgrade", 0) == 0) {
        key += "|" + upgradeStateKey();
    } else if (bound_ == "confirm_box") {
        key += "|" + std::to_string(game_.pendingTutorialWindow()) + "," + std::to_string(tutorialPage_) + ","
            + std::to_string(game_.gameOverMessage());
    } else if (bound_ == "map") {
        key += "|" + std::to_string(mapNote_) + "," + std::to_string(mapPin_) + ","
            + std::to_string(game_.mapPins().size()) + "," + std::to_string(game_.presentation().returnOption)
            + "," + std::to_string(game_.presentation().returnStrref);
    }
    return key;
}

// ---- equipment ------------------------------------------------------------------

void GameScreens::refreshEquipment()
{
    refreshMemberSwitch();
    const std::uint32_t pc = viewedObject(); // the member shown (the PC unless one was picked)
    const CharacterSheet& sheet = viewedSheet();

    // The slot buttons carry what is worn there.
    int worn = 0;
    const InventoryItem* wornInSelected = nullptr;
    // A double weapon (weaponwield 3) in the right hand shows again in the left slot at a quarter of
    // its brightness (006b9970 sets the float at this+0x10c8 from 1.0 to 0.25 for it; paired frame
    // req_eq_b_dblsword: the same blade, dark, in the left slot).
    const InventoryItem* rightHand = itemByObject(game_.wornItemObject(pc, 4));
    const bool doubleRight = rightHand != nullptr
        && game_.rules().baseItemInt(rightHand->baseItem, "weaponwield", 0) == 3;
    for (const EquipSlot& slot : kEquipSlots) {
        const InventoryItem* item = itemByObject(game_.wornItemObject(pc, slot.index));
        const std::string label = std::string("LBL_INV_") + slot.suffix;
        const bool ghost = slot.index == 5 && item == nullptr && doubleRight;
        if (const kotor::GuiControl* control = gui_.control(label)) {
            const InventoryItem* shown = ghost ? rightHand : item;
            const std::string icon = shown != nullptr ? itemInfo(*shown).icon : std::string{};
            gui_.setFill(label, icon.empty() ? std::string(slot.empty) : icon);
            const float level = ghost ? 0.25f : 1.0f;
            gui_.setFillColor(label, {level, level, level});
        }
        gui_.setSelected(std::string("BTN_INV_") + slot.suffix, slot.index == equipSlot_);
        worn += item != nullptr ? 1 : 0;
        if (slot.index == equipSlot_) {
            wornInSelected = item;
            gui_.setText("LBL_SLOTNAME", tlk(slot.name));
        }
    }
    (void)worn;

    // Damage and To Hit of each hand, as 006b9970 fills them: a wielded weapon shows its range
    // (005a9e10, "%d-%d") and to-hit (005a7770 / 005a7b60: "+%d" above 0, else "%d"); an empty
    // right hand shows the bare-hand values, an empty left hand leaves both labels blank. A double
    // weapon (weaponwield 3) in the right hand fills the left column too, as its off-hand end
    // (006b9970 runs that slot twice, first into LBL_ATKL with the off-hand flag).
    {
        const InventoryItem* rightWeapon = itemByObject(game_.wornItemObject(pc, 4));
        const bool doubleWeapon = rightWeapon != nullptr
            && game_.rules().baseItemInt(rightWeapon->baseItem, "weaponwield", 0) == 3;
        const auto hand = [&](int slotIndex, bool offhand, const char* damageLabel, const char* hitLabel) {
            const InventoryItem* weapon = itemByObject(game_.wornItemObject(pc, slotIndex));
            if (weapon == nullptr && offhand && !doubleWeapon) {
                gui_.setText(damageLabel, "");
                gui_.setText(hitLabel, "");
                return;
            }
            std::int32_t low = 1;
            std::int32_t high = 1;
            game_.rules().handDamageRange(sheet, offhand, low, high, doubleWeapon);
            gui_.setText(damageLabel, std::to_string(low) + "-" + std::to_string(high));
            const std::int32_t hit = game_.rules().handAttackBonus(sheet, offhand, doubleWeapon);
            gui_.setText(hitLabel, hit < 1 ? std::to_string(hit) : "+" + std::to_string(hit));
            // 006b9970 colours both numbers: green (DAT_007a23e4, 71/235/28 on the retail frames) when
            // the range with effects (004f3fe0) is above the range without them, and for the to-hit
            // when it is above the base attack bonus (005a60d0); else the panel blue.
            std::int32_t propertyAttack = 0;
            std::int32_t propertyDamage = 0;
            const InventoryItem* damaging = weapon != nullptr ? weapon : itemByObject(game_.wornItemObject(pc, 4));
            if (damaging != nullptr) {
                game_.rules().weaponPropertyBonus(*damaging, propertyAttack, propertyDamage);
            }
            std::int32_t baseAttack = game_.rules().baseAttackBonus(sheet.classId, sheet.level);
            if (sheet.secondClassId >= 0 && sheet.secondClassLevel > 0) {
                baseAttack += game_.rules().baseAttackBonus(sheet.secondClassId, sheet.secondClassLevel);
            }
            const kotor::Vec3 normal{0.0f, 0.66f, 0.98f};
            const kotor::Vec3 raised{0.278f, 0.922f, 0.110f};
            // The bare-hand path (006b9970, the empty right hand) sets the text only: the panel colour stays.
            const bool armed = weapon != nullptr || doubleWeapon;
            gui_.setTextColor(damageLabel, armed && propertyDamage > 0 ? raised : normal);
            gui_.setTextColor(hitLabel, armed && baseAttack < hit ? raised : normal);
        };
        hand(4, false, "LBL_ATKR", "LBL_TOHITR");
        hand(5, true, "LBL_ATKL", "LBL_TOHITL");
    }

    // Two modes (the shipped layout overlaps them: LB_DESC at 62,112 lies over the slot buttons at
    // 157..321 x 132..296): the paper doll with its slots, portrait and slot name; or, once a slot is
    // clicked, the item list with the description and the Equip button. Retail 006b7680 (the equip
    // panel's mode switch, constructor 006ba980) does exactly this: with the visible bit (0x2 of the
    // control flag word, the one the hit test 004187c0 requires) it sets the nine slot buttons, the
    // nine slot labels, LBL_PORTRAIT, LBL_TXTBAR, LBL_PORT_BORD and LBL_SLOTNAME and clears LB_DESC
    // and BTN_EQUIP in the slots mode (button text strref 0x62e = 1582), and the reverse in the
    // selection mode (strref 0x62d = 1581); the item rows' alpha goes 0 -> 1.0, which is what the
    // LB_ITEMS visibility below stands for. Cancel returns.
    const bool selecting = equipSelecting_;
    for (const EquipSlot& slot : kEquipSlots) {
        gui_.setVisible(std::string("BTN_INV_") + slot.suffix, !selecting);
        gui_.setVisible(std::string("LBL_INV_") + slot.suffix, !selecting);
    }
    for (const char* tag : {"LBL_PORTRAIT", "LBL_PORT_BORD", "LBL_SLOTNAME", "LBL_TXTBAR"}) {
        gui_.setVisible(tag, !selecting);
    }
    for (const char* tag : {"LB_DESC", "BTN_EQUIP", "LBL_SELECTTITLE"}) {
        gui_.setVisible(tag, selecting);
    }
    // The list stays up in both modes (paired frame ui_equip); on the paper doll it only shows.
    gui_.setEnabled("LB_ITEMS", selecting);
    for (const EquipSlot& slot : kEquipSlots) {
        gui_.setMarked(std::string("BTN_INV_") + slot.suffix, !selecting && slot.index == equipSlot_);
    }
    // ENGINE CHOICE (text): the shipped LBL_SELECTTITLE has no text and retail shows it only in the
    // selection mode (0006b7680 sets the control after the nine slot labels); it names the slot.
    for (const EquipSlot& slot : kEquipSlots) {
        if (slot.index == equipSlot_) {
            gui_.setText("LBL_SELECTTITLE", tlk(slot.name));
        }
    }
    gui_.setText("BTN_BACK", tlk(selecting ? 1581 : 1582)); // Cancel / Close

    // The list (retail 006b9470): None (strref 363), what is worn marked Equipped (006b6710, strref
    // 32346), then the carried items that fit the slot.
    equipRows_.clear();
    std::vector<GuiListItem> rows;
    equipWornRow_ = false;
    {
        GuiListItem none;
        none.text = tlk(363);
        none.icon = "inone"; // 006b6710: the frame is lbl_hex_3, the fill inone (0x006b6d71)
        none.itemEntry = true;
        rows.push_back(std::move(none));
        equipRows_.push_back(SIZE_MAX);
    }
    const auto addRow = [&](const InventoryItem& item, std::size_t index) {
        const ItemInfo& info = itemInfo(item);
        GuiListItem row;
        row.text = equipRows_.size() == 1 && equipWornRow_ ? info.name + " (" + tlk(32346) + ")" : info.name; // 006b6710: name + " (" + strref 0x7e5a + ")"
        row.icon = info.icon;
        row.itemEntry = true;
        row.count = item.stack;
        row.value = static_cast<int>(index);
        rows.push_back(std::move(row));
        equipRows_.push_back(index);
    };
    equipWornObject_ = 0;
    if (wornInSelected != nullptr) {
        equipWornRow_ = true;
        equipWornObject_ = wornInSelected->object;
        const std::ptrdiff_t at = wornInSelected - game_.inventory().data();
        const bool shared = at >= 0 && static_cast<std::size_t>(at) < game_.inventory().size();
        addRow(*wornInSelected, shared ? static_cast<std::size_t>(at) : SIZE_MAX);
    }
    for (std::size_t i = 0; i < game_.inventory().size(); ++i) {
        const InventoryItem& item = game_.inventory()[i];
        if (&item == wornInSelected) {
            continue;
        }
        if (item.equipped || item.object == 0) {
            continue;
        }
        if ((game_.rules().equipMask(item.baseItem) >> static_cast<unsigned>(equipSlot_) & 1u) == 0u) {
            continue;
        }
        // Retail 006b9470: option bit 1 leaves out what this member cannot wear; off, it stays, red (state 2, 006b5060).
        const bool wearable = game_.rules().canEquip(sheet, item);
        if (!wearable && (game_.feedbackOptions() & 0x1u) != 0u) {
            continue;
        }
        addRow(item, i);
        if (!wearable) {
            rows.back().hasColor = true;
            rows.back().color = {0.85f, 0.05f, 0.05f};
        }
    }
    const std::size_t selection = std::min(gui_.listSelection("LB_ITEMS"), rows.size() - 1);
    const bool wornHere = equipWornRow_;
    if (!selecting) {
        for (GuiListItem& row : rows) {
            row.selectable = false;
        }
    }
    gui_.setRowHeight("LB_ITEMS", kItemEntryHeight);
    gui_.setItems("LB_ITEMS", std::move(rows));
    gui_.setListSelection("LB_ITEMS", selection);
    shownRow_ = selection;

    GuiListItem description;
    description.selectable = false;
    if (const InventoryItem* shown = rowItem(selection)) {
        description.text = describe(*shown);
    }
    gui_.setItems("LB_DESC", {description});

    // None takes the worn item off, as does the worn row itself.
    const bool onWorn = wornHere && selection <= 1;
    gui_.setText("BTN_EQUIP", tlk(onWorn ? 36840 : 31387));
    gui_.setEnabled("BTN_EQUIP", onWorn || selection >= 1);

    if (sheet.portraitRow >= 0) {
        gui_.setFill("LBL_PORTRAIT", game_.rules().portraitResrefFor(game_.rules().portraitRow(sheet.portraitRow),
                                                                      sheet.alignment));
    }
    gui_.setText("LBL_DEF", std::to_string(game_.rules().armorClass(sheet)));
    // Current over most, as the inventory screen shows it (walk 0032: "6/6").
    const bool controlled = viewedNpc_ == game_.partyTable().controlled;
    gui_.setText("LBL_VITALITY", std::to_string(controlled ? game_.controlledHp() : sheet.hp) + "/"
                                     + std::to_string(controlled ? game_.controlledMaxHp() : sheet.maxHp));
    gui_.setVisible("LBL_CANTEQUIP", false);
}

void GameScreens::hoverEquipSlot()
{
    const std::string& underKey = gui_.hovered();
    const std::string under = underKey.substr(underKey.rfind('/') + 1); // the full key to its tag
    if (under.rfind("BTN_INV_", 0) != 0) {
        return; // the last slot stays (retail keeps the focused control)
    }
    for (const EquipSlot& slot : kEquipSlots) {
        if (under == std::string("BTN_INV_") + slot.suffix) {
            equipSlot_ = slot.index;
        }
    }
}

bool GameScreens::actionEquipment(const std::string& tag)
{
    if (tag.rfind("BTN_INV_", 0) == 0) {
        for (const EquipSlot& slot : kEquipSlots) {
            if (tag == std::string("BTN_INV_") + slot.suffix) {
                equipSlot_ = slot.index;
                equipSelecting_ = true;
                // ENGINE CHOICE: the cursor starts on the first thing the slot can hold, not on None.
                gui_.setListSelection("LB_ITEMS", 1);
                return true;
            }
        }
        return false;
    }
    const auto commit = [&](std::size_t row) {
        if (row >= equipRows_.size()) {
            return;
        }
        const InventoryItem* picked = row == 0 && equipWornRow_ ? itemByObject(equipWornObject_) : rowItem(row);
        if (picked == nullptr) {
            return;
        }
        const InventoryItem item = *picked;
        const std::uint32_t pc = viewedObject();
        if (equipWornRow_ && row <= 1) {
            const bool done = game_.unequipItemObject(pc, item.object);
            logInfo(concat("equipment screen: unequip ", item.resref, done ? " ok" : " refused"));
        } else {
            const bool done = game_.equipItemObject(pc, item.object, equipSlot_);
            logInfo(concat("equipment screen: equip ", item.resref, " slot ", std::to_string(equipSlot_),
                           done ? " ok" : " refused"));
        }
        equipSelecting_ = false;
    };
    if (tag == "BTN_EQUIP") {
        commit(gui_.listSelection("LB_ITEMS"));
        return true;
    }
    if (tag == "LB_ITEMS") {
        const int row = gui_.lastItem();
        if (row >= 0) {
            const bool again = clickedRow_ == static_cast<std::size_t>(row);
            clickedRow_ = static_cast<std::size_t>(row);
            gui_.setListSelection("LB_ITEMS", static_cast<std::size_t>(row));
            if (again) {
                commit(static_cast<std::size_t>(row));
            }
        }
        return true;
    }
    return false;
}

// ---- abilities ------------------------------------------------------------------

void GameScreens::refreshAbilities()
{
    refreshMemberSwitch();
    const CharacterSheet& sheet = viewedSheet();
    kotor::Install& install = game_.install();
    const std::optional<kotor::TwoDa> skills = table(install, "skills");
    const std::optional<kotor::TwoDa> feats = table(install, "feat");
    const std::optional<kotor::TwoDa> spells = table(install, "spells");

    abilityRows_.clear();
    std::vector<GuiListItem> rows;
    const auto add = [&](const std::optional<kotor::TwoDa>& data, std::size_t row, bool withIcon, const char* icon) {
        GuiListItem item;
        const std::uint32_t strref = strrefOf(cellOf(data, row, "name"));
        item.text = strref != UINT32_MAX ? tlk(strref) : cellOf(data, row, "label");
        if (withIcon) {
            item.icon = lowerOfText(cellOf(data, row, icon));
        }
        item.value = static_cast<int>(row);
        rows.push_back(std::move(item));
        abilityRows_.push_back(static_cast<std::int32_t>(row));
    };
    if (abilityTab_ == 0) {
        for (int i = 0; i < game_.rules().skillCount(); ++i) {
            add(skills, static_cast<std::size_t>(i), true, "icon"); // paired frame ui_abilities: the skill's icon
        }
    } else if (abilityTab_ == 1) {
        for (const std::int32_t id : sheet.powers) {
            add(spells, static_cast<std::size_t>(id), true, "iconresref");
        }
    }
    // Retail 006abce0, 006ce0f0: the feat page is the chargen grid, of what the member has. Fails on the old names.
    const bool featGrid = abilityTab_ == 2;
    int featPicked = -1;
    if (featGrid) {
        bindFeatGrid(gui_, game_, sheet.classId, sheet.feats, [](std::int32_t) { return 1; }, "LB_ABILITY", true);
        featPicked = gui_.listCellValue("LB_ABILITY");
        if (featPicked >= 0) {
            abilityRows_.push_back(featPicked);
        }
    } else {
        gui_.setRowHeight("LB_ABILITY", 0.0f);
    }
    const std::size_t selection = featGrid ? 0 : std::min(gui_.listSelection("LB_ABILITY"), rows.empty() ? std::size_t{0} : rows.size() - 1);
    const bool empty = featGrid ? featPicked < 0 : rows.empty();
    std::string selectedName;
    if (featGrid && !empty) {
        const std::uint32_t nameRef = strrefOf(cellOf(feats, static_cast<std::size_t>(featPicked), "name"));
        selectedName = nameRef != UINT32_MAX ? tlk(nameRef) : game_.rules().featName(featPicked);
    } else if (!empty) {
        selectedName = rows[selection].text;
    }
    if (!featGrid) {
        gui_.setItems("LB_ABILITY", std::move(rows));
        gui_.setListSelection("LB_ABILITY", selection);
    }
    shownRow_ = selection;

    GuiListItem description;
    description.selectable = false;
    gui_.setText("LBL_NAME", selectedName);
    const bool skillTab = abilityTab_ == 0;
    // Retail 006ad560 for the skill page: Skill Rank (1579), Bonus (32129), Total Rank (41904). The
    // file's own strrefs (Base Cost, Dark Side) belong to the power page.
    if (skillTab) {
        gui_.setText("LBL_SKILLRANK", tlk(1579));
        gui_.setText("LBL_BONUS", tlk(32129));
        gui_.setText("LBL_TOTAL", tlk(41904));
    }
    // Paired frame ui_abilities: a class with no Force points has no Powers tab.
    gui_.setVisible("BTN_POWERS", sheet.maxFp > 0 || !sheet.powers.empty());
    for (const char* tag : {"LBL_SKILLRANK", "LBL_RANKVAL", "LBL_BONUS", "LBL_BONUSVAL", "LBL_TOTAL", "LBL_TOTALVAL"}) {
        gui_.setVisible(tag, skillTab);
    }
    if (!empty) {
        const std::size_t row = static_cast<std::size_t>(abilityRows_[selection]);
        const std::optional<kotor::TwoDa>& source = abilityTab_ == 0 ? skills : (abilityTab_ == 1 ? spells : feats);
        const std::uint32_t strref = strrefOf(cellOf(source, row, abilityTab_ == 1 ? "spelldesc" : "description"));
        description.text = strref != UINT32_MAX ? tlk(strref) : std::string{};
        if (skillTab) {
            // Rank from the sheet; the bonus is the key ability's modifier plus the feats'.
            const std::string key = game_.rules().skillName(static_cast<std::int32_t>(row));
            const std::string label = cellOf(skills, row, "label");
            int rank = 0;
            for (const std::string* candidate : {&key, &label}) {
                const auto found = sheet.skills.find(*candidate);
                if (found != sheet.skills.end()) {
                    rank = found->second;
                    break;
                }
            }
            const std::string ability = alnumLower(cellOf(skills, row, "keyability"));
            int score = 10;
            if (ability == "str") score = sheet.str;
            else if (ability == "dex") score = sheet.dex;
            else if (ability == "con") score = sheet.con;
            else if (ability == "int") score = sheet.intel;
            else if (ability == "wis") score = sheet.wis;
            else if (ability == "cha") score = sheet.cha;
            const int modifier = score >= 10 ? (score - 10) / 2 : -((11 - score) / 2);
            const int bonus = modifier + game_.rules().skillBonus(sheet, key);
            gui_.setText("LBL_RANKVAL", std::to_string(rank));
            gui_.setText("LBL_BONUSVAL", std::to_string(bonus));
            gui_.setText("LBL_TOTALVAL", std::to_string(rank + bonus));
        }
    }
    gui_.setItems("LB_DESC", {description});
    if (sheet.portraitRow >= 0) {
        gui_.setFill("LBL_PORTRAIT", game_.rules().portraitResrefFor(game_.rules().portraitRow(sheet.portraitRow),
                                                                      sheet.alignment));
    }
    gui_.setSelected("BTN_SKILLS", abilityTab_ == 0);
    gui_.setSelected("BTN_POWERS", abilityTab_ == 1);
    gui_.setSelected("BTN_FEATS", abilityTab_ == 2);
    // Paired frame ui_abilities: the page's own tab wears the highlight art.
    gui_.setMarked("BTN_SKILLS", abilityTab_ == 0);
    gui_.setMarked("BTN_POWERS", abilityTab_ == 1);
    gui_.setMarked("BTN_FEATS", abilityTab_ == 2);
}

bool GameScreens::actionAbilities(const std::string& tag)
{
    int tab = -1;
    if (tag == "BTN_SKILLS") tab = 0;
    else if (tag == "BTN_POWERS") tab = 1;
    else if (tag == "BTN_FEATS") tab = 2;
    if (tab >= 0) {
        abilityTab_ = tab;
        gui_.setListSelection("LB_ABILITY", 0);
        return true;
    }
    return tag == "LB_ABILITY";
}

// ---- messages -------------------------------------------------------------------

void GameScreens::refreshMessages()
{
    // The conversation history (Dialog) and the feedback the session told the player (Feedback).
    std::vector<GuiListItem> lines;
    for (const std::string& line : dialogueLog_) {
        GuiListItem item;
        item.text = line;
        item.selectable = false;
        lines.push_back(std::move(item));
    }
    const bool dialogChanged = gui_.state("LB_DIALOG").items.size() != lines.size();
    const bool feedbackChanged = gui_.state("LB_MESSAGES").items.size() != game_.feedbackLog().size();
    gui_.setItems("LB_DIALOG", std::move(lines));
    std::vector<GuiListItem> feedback;
    for (const std::string& line : game_.feedbackLog()) {
        GuiListItem item;
        item.text = line;
        item.selectable = false;
        feedback.push_back(std::move(item));
    }
    gui_.setItems("LB_MESSAGES", std::move(feedback));
    // AddDialogMessages 00626b10 and AddMessages 00626920 hand the list boxes entry controls, then pick the
    // last (SetSelectedControl(count - 1)): the lists open scrolled to their end with that entry framed.
    for (const char* tag : {"LB_DIALOG", "LB_MESSAGES"}) {
        const bool changed = std::string(tag) == "LB_DIALOG" ? dialogChanged : feedbackChanged;
        const std::size_t count = gui_.state(tag).items.size();
        const bool fresh = changed || !gui_.state(tag).entryList;
        gui_.setEntryList(tag, true, fresh);
        if (count > 0 && fresh) {
            gui_.setListSelection(tag, count - 1);
        }
    }
    gui_.setVisible("LB_DIALOG", showDialogLog_);
    gui_.setVisible("LB_MESSAGES", !showDialogLog_);
    // Retail 00624df0 and 00624f70: "Show Feedback" over the dialog log, "Show Dialog" over the feedback,
    // and the title names the list (strrefs 1563, 371, 42167; paired frame ui_messages).
    gui_.setText("BTN_SHOW", tlk(showDialogLog_ ? 42142 : 42143));
    gui_.setText("LBL_MESSAGES", tlk(1563) + " - " + tlk(showDialogLog_ ? 371 : 42167));
    shownRow_ = gui_.listSelection(listTag());
}

bool GameScreens::actionMessages(const std::string& tag)
{
    if (tag == "BTN_SHOW") {
        showDialogLog_ = !showDialogLog_;
        return true;
    }
    return false;
}

// ---- galaxy map -----------------------------------------------------------------

std::string GameScreens::previewPlanetModel() const
{
    if (bound_ != "galaxymap" || planet_ < 0) {
        return {};
    }
    const std::optional<kotor::TwoDa> planetary = table(game_.install(), "planetary");
    return cellOf(planetary, static_cast<std::size_t>(planet_), "model");
}

void GameScreens::refreshGalaxyMap()
{
    const GalaxyMapState& map = game_.galaxyMap();
    const auto planets = game_.galaxyPlanetNames();
    const std::optional<kotor::TwoDa> planetary = table(game_.install(), "planetary");
    galaxyTags_.clear();
    if (planet_ < 0 && map.highlighted >= 0) {
        planet_ = map.highlighted;
    }
    // Retail 00694b40: nothing picked yet means the first planet that is available and selectable,
    // and 006933a0 then fills the name and LBL_DESC, so the description is never blank on open.
    if (planet_ < 0) {
        for (std::size_t row = 0; row < kPlanetSlots; ++row) {
            if (map.available[row] && map.selectable[row]) {
                planet_ = static_cast<int>(row);
                break;
            }
        }
    }
    // planetary.2da guitag names each row's button; 00694b40 shows a button when its row is available.
    for (const kotor::GuiControl& control : gui_.screen()->controls) {
        if (control.tag.rfind("LBL_Planet_", 0) != 0 && control.tag.rfind("LBL_Live", 0) != 0) {
            continue;
        }
        int row = -1;
        for (std::size_t index = 0; index < kPlanetSlots; ++index) {
            if (cellOf(planetary, index, "guitag") == control.tag) {
                row = static_cast<int>(index);
                break;
            }
        }
        const bool shown = row >= 0 && map.available[static_cast<std::size_t>(row)];
        gui_.setVisible(control.key, shown);
        if (shown) {
            gui_.setEnabled(control.key, map.selectable[static_cast<std::size_t>(row)]);
            gui_.setSelected(control.key, row == planet_);
            galaxyTags_[control.tag] = row;
        }
    }
    std::string name;
    std::string description;
    if (planet_ >= 0) {
        for (const auto& [index, text] : planets) {
            if (index == planet_) {
                name = text;
            }
        }
        const std::uint32_t strref = strrefOf(cellOf(planetary, static_cast<std::size_t>(planet_), "description"));
        description = strref != UINT32_MAX ? tlk(strref) : std::string{};
    }
    gui_.setText("LBL_PLANETNAME", name);
    gui_.setText("LBL_DESC", description);
    gui_.setEnabled("BTN_ACCEPT", planet_ >= 0 && static_cast<std::size_t>(planet_) < kPlanetSlots
                                      && map.available[static_cast<std::size_t>(planet_)]
                                      && map.selectable[static_cast<std::size_t>(planet_)]);
}

bool GameScreens::actionGalaxyMap(const std::string& tag)
{
    const auto found = galaxyTags_.find(tag);
    if (found != galaxyTags_.end()) {
        planet_ = found->second;
        return true;
    }
    if (tag == "BTN_ACCEPT") {
        if (planet_ >= 0) {
            const bool done = game_.selectPlanet(planet_);
            logInfo(concat("galaxy map screen: planet ", std::to_string(planet_), done ? " selected" : " refused"));
        }
        return true;
    }
    return false;
}

} // namespace ke

namespace ke {

// ---- map ------------------------------------------------------------------------

// The area map panel (retail 00693650). The area drawing is painted by the application
// (the lbl_map<area> art fitted into LBL_Map with the leader's arrow); this binds the rest:
// the area name, the Party Selection button and its refusal, and the script-set Return button.
void GameScreens::refreshMap()
{
    gui_.setText("LBL_Area", game_.areaDisplayName());
    // Retail 006929b0: "Map Note" (strref 349), ": " and the selected pin's note, empty without one.
    // ENGINE CHOICE: a refusal shows on this line too; retail raises a message box with the same text.
    std::string note;
    const std::vector<std::size_t> pins = game_.mapPins();
    if (!pins.empty()) { // paired frame map_refused: the pin's note stays under the refusal box
        mapPin_ = std::clamp(mapPin_, 0, static_cast<int>(pins.size()) - 1);
        const kotor::Entity& pin = game_.scene().entities[pins[static_cast<std::size_t>(mapPin_)]];
        const std::string text = pin.mapNote.empty() && pin.mapNoteStrref != 0xFFFFFFFFu ? tlk(pin.mapNoteStrref) : pin.mapNote;
        note = text.empty() ? std::string{} : tlk(349) + ": " + text;
    }
    gui_.setText("LBL_MapNote", note);
    // Retail colours the button as disabled and keeps the reason; here a click on it says why.
    const bool refused = game_.partySelectionRefusal() != 0;
    gui_.setTextColor("BTN_PRTYSLCT", refused ? kotor::Vec3{0.5f, 0.5f, 0.5f} : kotor::Vec3{1.0f, 1.0f, 1.0f});
    // Paired frame ui_map: without a script-set return the button stays up, dim, with the file's text.
    // Option bit 0x100's only reader outside the movie code: 00693650 centres these two (alignment 18; the files say 10).
    const int mapButtonAlign = (game_.feedbackOptions() & 0x100u) != 0u ? 18 : 0;
    gui_.setTextAlign("BTN_PRTYSLCT", mapButtonAlign);
    gui_.setTextAlign("BTN_RETURN", mapButtonAlign);
    const bool returnShown = game_.presentation().returnOption;
    gui_.setEnabled("BTN_RETURN", true); // dim, not dead: 00692bc0 answers with the reason
    gui_.setTextColor("BTN_RETURN", returnShown ? kotor::Vec3{1.0f, 1.0f, 1.0f} : kotor::Vec3{0.5f, 0.5f, 0.5f});
    if (returnShown) {
        gui_.setText("BTN_RETURN", tlk(static_cast<std::uint32_t>(game_.presentation().returnStrref)));
    } else {
        gui_.setText("BTN_RETURN", tlk(38550)); // "Transit Disabled", paired frame ui_map
    }
}

void GameScreens::selectMapPin(int index)
{
    mapNote_ = 0;
    mapPin_ = index;
}

bool GameScreens::actionMap(const std::string& tag)
{
    // Retail 006927b0/006927c0 (00693090, 00692e80): BTN_UP and BTN_DOWN walk the pins and wrap.
    if (tag == "BTN_UP" || tag == "BTN_DOWN") {
        const int count = static_cast<int>(game_.mapPins().size());
        if (count > 0) {
            selectMapPin((mapPin_ + (tag == "BTN_UP" ? count - 1 : 1)) % count);
        }
        return true;
    }
    if (tag == "BTN_PRTYSLCT") {
        const std::uint32_t refusal = game_.partySelectionRefusal();
        if (refusal != 0) {
            // Retail 00692bc0: a message box with the reason (+0x10d4) and OK alone (00627130(0)).
            mapNote_ = refusal;
            openPazaakPopup(5);
            return true;
        }
        // The refusals passed: the map closes and the party selection takes its place.
        if (game_.openPartySelectionFromMap()) {
            game_.setUiPanel(UiPanel::None);
            gui_.close();
            bound_.clear();
            key_.clear();
        }
        return true;
    }
    if (tag == "BTN_RETURN") {
        // Retail 00694d50 -> 00693bc0 (event 0x29) -> 00692bc0: a question box with the strref the script
        // gave (SetReturnStrref's third argument); its OK runs k_sup_gohawk (00692b00).
        if (game_.presentation().returnOption) {
            openPazaakPopup(4);
        } else {
            // The same handler with bit 2 clear: the box with the reason (+0x10d4), 38451 when none was stored.
            mapNote_ = game_.partySelectionRefusal() != 0 ? game_.partySelectionRefusal() : 38451;
            openPazaakPopup(5);
        }
        return true;
    }
    return false;
}

// ---- level up -------------------------------------------------------------------
//
// Retail opens the same ability, skill and feat screens it uses for character generation
// (ABCHRGEN reads a mode flag, 006f7600: title strref 0x42f "Level Up" when set), with the
// points the character has waiting, behind the LEVELUPPNL step list (006ee7d0). A point is spent
// the moment it is placed; lighting a row again puts that row's picks back (resetLevelRow, 006ee0e0).

namespace {
constexpr const char* kAbilityTags[6] = {"STR", "DEX", "CON", "INT", "WIS", "CHA"};
constexpr std::uint32_t kAbilityNames[6] = {719, 720, 721, 722, 723, 724};
constexpr const char* kSkillBase[8] = {"COMPUTER_USE", "DEMOLITIONS", "STEALTH", "AWARENESS",
                                       "PERSUADE",     "REPAIR",      "SECURITY", "TREAT_INJURY"};
constexpr const char* kSkillShort[8] = {"COM", "DEM", "STE", "AWA", "PER", "REP", "SEC", "TRE"};
// The help of each ability row, by sheet order: retail 006f7600 sets control+0x58 to 0xde..0xe3 down its
// slots, which run STR DEX CON WIS INT CHA, and 006f70e0 shows the focused slot's text in LB_DESC.
constexpr std::uint32_t kAbilityHelp[6] = {222, 223, 224, 226, 225, 227};
} // namespace

bool GameScreens::levelStepAvailable(int step) const
{
    const CharacterSheet& sheet = game_.character();
    if (step == 0) {
        return sheet.attributePoints > 0;
    }
    if (step == 1) {
        return sheet.skillPoints > 0;
    }
    if (step == 2) {
        if (sheet.featPoints <= 0) {
            return false;
        }
        for (std::int32_t id = 0; id < game_.rules().featCount(); ++id) {
            if (!sheet.feats.contains(id) && game_.rules().featIsAvailable(sheet.classId, id)
                && game_.rules().canTakeFeat(sheet, id)) {
                return true;
            }
        }
    }
    if (step == 3) {
        if (sheet.powerPoints <= 0) {
            return false;
        }
        const std::int32_t count = static_cast<std::int32_t>(game_.rules().powerCount());
        for (std::int32_t id = 0; id < count; ++id) {
            if (game_.rules().canLearnPowerAtLevelUp(sheet, id)) {
                return true;
            }
        }
    }
    return false;
}

int GameScreens::levelHover() const
{
    if (levelStep_ == 0) {
        for (int i = 0; i < 6; ++i) {
            for (const char* part : {"_LBL", "_POINTS_BTN", "_PLUS_BTN", "_MINUS_BTN"}) {
                if (gui_.state(std::string(kAbilityTags[i]) + part).hovered) {
                    return i;
                }
            }
        }
    } else if (levelStep_ == 1) {
        for (int i = 0; i < 8; ++i) {
            for (const char* part : {"_LBL", "_POINTS_BTN"}) {
                if (gui_.state(std::string(kSkillBase[i]) + part).hovered) {
                    return i;
                }
            }
            for (const char* part : {"_PLUS_BTN", "_MINUS_BTN"}) {
                if (gui_.state(std::string(kSkillShort[i]) + part).hovered) {
                    return i;
                }
            }
        }
    }
    return -1;
}

int GameScreens::levelHubNext(int from, int direction) const
{
    // 006ee0e0: from the lit row to the next shown one that way (rows past the end count as shown).
    for (int row = from + direction; row >= 0 && row < 5; row += direction) {
        if (levelHubRow(row)) {
            return row;
        }
    }
    return -1;
}

void GameScreens::refreshLevelHub()
{
    bindLevelUpMaincg(gui_, game_, game_.character());
    bindLevelUpSteps(gui_, levelRows_, levelLit_, levelLit_ == levelFirst_);
}

void GameScreens::refreshLevelUp()
{
    if (levelHub_) {
        refreshLevelHub();
        return;
    }
    const CharacterSheet& sheet = game_.character();
    const auto set = [&](const std::string& tag, const std::string& text) { gui_.setText(tag, text); };
    set("MAIN_TITLE_LBL", tlk(1071)); // retail's own title in level-up mode (006f7600, 006f51d0: 0x42f)
    // The files' own sub-titles (paired frames levelup_attrs, levelup_skills): Attributes, Skills, Feats.
    static const std::uint32_t stepNames[3] = {209, 233, 232};
    if (levelStep_ < 3) {
        set("SUB_TITLE_LBL", tlk(stepNames[levelStep_]));
    }
    const int hover = levelHover();
    // Retail's help follows the focus and stays when the pointer leaves (006f70e0, 006f4bf0); the first
    // row is selected when the panel opens (FUN_0040a630 at the end of 006f7600 and 006f51d0).
    if (hover >= 0) {
        levelFocus_ = hover;
    }
    const auto showHelp = [&](const std::string& title, const std::string& text) {
        set("DESC_LBL", title);
        gui_.setVisible("DESC_LBL", true);
        GuiListItem item;
        item.text = text;
        item.selectable = false;
        gui_.setItems("LB_DESC", {item});
    };

    if (levelStep_ == 0) {
        set("REMAINING_SELECTIONS_LBL", std::to_string(sheet.attributePoints));
        // 006f7600's level-up branch writes its own English label here, not a dialog.tlk line.
        set("SELECTIONS_REMAINING_LBL", "Points Remaining");
        const std::array<std::int32_t, 6> scores{sheet.str, sheet.dex, sheet.con, sheet.intel, sheet.wis, sheet.cha};
        for (int i = 0; i < 6; ++i) {
            const std::string base = kAbilityTags[i];
            set(base + "_LBL", tlk(kAbilityNames[i]));
            set(base + "_POINTS_BTN", std::to_string(scores[static_cast<std::size_t>(i)]));
            gui_.setVisible(base + "_PLUS_BTN", sheet.attributePoints > 0);
            gui_.setVisible(base + "_MINUS_BTN", false);
        }
        bindAbilityFocus(gui_, game_, scores, levelFocus_ % 6, true);
        gui_.setVisible("BTN_RECOMMENDED", sheet.attributePoints > 0); // 006f8880 acts only with points left
        return;
    }

    if (levelStep_ == 1) {
        const auto skills = table(game_.install(), "skills");
        set("REMAINING_SELECTIONS_LBL", std::to_string(sheet.skillPoints));
        set("SELECTIONS_REMAINING_LBL", tlk(210));
        for (int i = 0; i < 8; ++i) {
            const std::string name = game_.rules().skillName(i);
            const auto rank = sheet.skills.find(name);
            const int ranks = rank == sheet.skills.end() ? 0 : rank->second;
            const std::uint32_t nameRef = strrefOf(cellOf(skills, static_cast<std::size_t>(i), "name"));
            set(std::string(kSkillBase[i]) + "_LBL", nameRef != UINT32_MAX ? tlk(nameRef) : name);
            set(std::string(kSkillBase[i]) + "_POINTS_BTN", std::to_string(ranks));
            const int cost = game_.rules().skillRankCost(sheet.classId, i);
            // 006f51d0: the plus goes only at the row's cap (paired frame levelup_skills shows it on the cross-class
            // rows a single point cannot buy); spendSkillRank refuses what the points do not cover.
            const bool can = cost > 0 && ranks < game_.rules().maxSkillRank(sheet.classId, i, sheet.level);
            gui_.setVisible(std::string(kSkillShort[i]) + "_PLUS_BTN", can);
            gui_.setVisible(std::string(kSkillShort[i]) + "_MINUS_BTN", false);
        }
        gui_.setVisible("BTN_RECOMMENDED", true);
        bindSkillFocus(gui_, game_, sheet.classId, levelFocus_ % 8);
        return;
    }

    if (levelStep_ == 3) {
        // Powers (pwrlvlup, retail 006f2180): what the class may learn now, and the one picked.
        const auto spells = table(game_.install(), "spells");
        abilityRows_.clear();
        std::vector<GuiListItem> rows;
        const std::int32_t count = static_cast<std::int32_t>(game_.rules().powerCount());
        for (std::int32_t id = 0; id < count; ++id) {
            if (!game_.rules().canLearnPowerAtLevelUp(sheet, id)) {
                continue;
            }
            const std::uint32_t nameRef = strrefOf(cellOf(spells, static_cast<std::size_t>(id), "name"));
            GuiListItem item;
            item.text = nameRef != UINT32_MAX ? tlk(nameRef) : game_.rules().powerName(id);
            item.icon = cellOf(spells, static_cast<std::size_t>(id), "iconresref");
            item.value = id;
            rows.push_back(std::move(item));
            abilityRows_.push_back(id);
        }
        gui_.setItems("LB_POWERS", std::move(rows));
        set("REMAINING_SELECTIONS_LBL", std::to_string(sheet.powerPoints));
        const std::size_t row = gui_.listSelection("LB_POWERS");
        const bool picked = row < abilityRows_.size();
        gui_.setVisible("SELECT_BTN", picked && sheet.powerPoints > 0);
        gui_.setVisible("DESC_LBL", picked);
        if (picked) {
            const std::size_t id = static_cast<std::size_t>(abilityRows_[row]);
            const std::uint32_t nameRef = strrefOf(cellOf(spells, id, "name"));
            const std::uint32_t descRef = strrefOf(cellOf(spells, id, "spelldesc"));
            showHelp(nameRef != UINT32_MAX ? tlk(nameRef) : std::string{}, descRef != UINT32_MAX ? tlk(descRef) : std::string{});
        } else {
            gui_.setItems("LB_DESC", {});
        }
        return;
    }

    // Feats: the list of what the character can take now.
    const auto feats = table(game_.install(), "feat");
    abilityRows_.clear();
    // Retail 006f30f0: what the sheet has, what can be taken now, the rest out of reach.
    const auto status = [&](std::int32_t id) {
        if (sheet.feats.contains(id)) {
            return 1;
        }
        return game_.rules().featIsAvailable(sheet.classId, id) && game_.rules().canTakeFeat(sheet, id) ? 0 : 3;
    };
    bindFeatGrid(gui_, game_, sheet.classId, sheet.feats, status);
    set("STD_REMAINING_SELECTIONS_LBL", std::to_string(sheet.featPoints));
    set("STD_SELECTIONS_REMAINING_LBL", tlk(32062)); // Remaining Feats
    const std::int32_t pickedId = gui_.listCellValue("LB_FEATS");
    const bool picked = pickedId >= 0;
    gui_.setVisible("BTN_SELECT", picked && sheet.featPoints > 0);
    gui_.setEnabled("BTN_SELECT", picked && status(pickedId) == 0);
    gui_.setVisible("BTN_RECOMMENDED", sheet.featPoints > 0);
    // The same screen as chargen (retail 006f3d60): the name on LBL_NAME, "Description" over the text.
    gui_.setVisible("LBL_NAME", picked);
    gui_.setVisible("DESC_LBL", true);
    set("DESC_LBL", tlk(217));
    if (picked) {
        const std::int32_t id = pickedId;
        const std::uint32_t nameRef = strrefOf(cellOf(feats, static_cast<std::size_t>(id), "name"));
        set("LBL_NAME", nameRef != UINT32_MAX ? tlk(nameRef) : game_.rules().featName(id));
        set("BTN_SELECT", tlk(status(id) == 1 ? 38456 : 38455) + " " + tlk(42487)); // retail 006f3d60, 006f2fb0
        const std::uint32_t descRef = strrefOf(cellOf(feats, static_cast<std::size_t>(id), "description"));
        GuiListItem item;
        item.text = descRef != UINT32_MAX ? tlk(descRef) : std::string{};
        item.selectable = false;
        gui_.setItems("LB_DESC", {item});
    } else {
        gui_.setItems("LB_DESC", {});
    }
}

bool GameScreens::actionLevelUp(const std::string& tag)
{
    const auto finish = [&] {
        closeScreen();
        return true;
    };
    if (levelNotice_) {
        if (tag == "BTN_OK") {
            gui_.popModal();
            levelNotice_ = false;
            key_.clear();
            return true;
        }
        return tag == "LB_SKILLS";
    }
    if (levelHub_) {
        // LEVELUPPNL (006ee7d0): each shown row opens its screen with no gating (006ee500 attributes,
        // 006ee480 skills, 006ee3d0 feats, 006ee350 powers); Accept (006ee780) ends the level-up (006e88a0).
        if (tag.rfind("BTN_STEPNAME", 0) == 0 && tag.size() == 13) {
            const int row = tag[12] - '1';
            if (!levelHubRow(row) || row != levelLit_) {
                return true; // 006ee0e0 leaves only the lit row's button enabled
            }
            if (row == 4) {
                // Accept (LAB_006ee780): 00649950(0) gives the creature every feat its class grants at the new class
                // level (005bd0f0 keeps <class>_list 3 as the granted kind, 005be310 reads <class>_granted, which must
                // equal the level), then 006e88a0 applies the level-up (00678280) and the panel goes. Retail then shows
                // skillinfo with 0xa512 over the character sheet it returns to, listing the new feats (paired
                // req_levelup_granted: a Scoundrel reaching level 3, "Sneak Attack II"); the hub opened with no box
                // (req_levelup_l3_hub_open). Which call raises the box after the apply is not traced.
                levelGranted_ = grantLevelFeats();
                if (!levelGranted_.empty()) {
                    closeScreen();
                    game_.setUiPanel(UiPanel::Character); // the sheet the level-up was opened from, under the box
                    return true;
                }
                return finish();
            }
            levelHub_ = false;
            levelStep_ = row;
            levelFocus_ = 0;
            return true;
        }
        if (tag == "BTN_BACK") {
            // 006ee0e0(1): the row before the lit one; nothing on the first (Back is off there).
            if (levelLit_ != levelFirst_) {
                if (const int previous = levelHubNext(levelLit_, -1); previous >= 0) {
                    levelLit_ = previous;
                    resetLevelRow(levelLit_);
                }
            }
            return true;
        }
        return tag.rfind("LBL_", 0) == 0;
    }
    if (tag == "BTN_RECOMMENDED" || tag == "RECOMMENDED_BTN") {
        applyLevelRecommended();
        return true;
    }
    if (tag == "BTN_ACCEPT" || tag == "ACCEPT_BTN") {
        // A step's Accept (006ee5b0 / 006ee5d0): back on the hub with the row after the lit one lit.
        if (const int next = levelHubNext(levelLit_, 1); next >= 0) {
            levelLit_ = next;
            resetLevelRow(levelLit_);
        }
        levelHub_ = true;
        return true;
    }
    if (tag == "BTN_BACK" || tag == "BTN_CANCEL" || tag == "BACK_BTN") {
        // 006ee580 (CancelledByChild): the lit row steps back one and 006ee0e0(0) lights it again, which puts its
        // picks back (the step's work is dropped).
        resetLevelRow(levelLit_);
        levelHub_ = true;
        return true;
    }
    if (levelStep_ == 0) {
        for (int i = 0; i < 6; ++i) {
            if (tag == std::string(kAbilityTags[i]) + "_PLUS_BTN") {
                game_.spendAttributePoint(i);
                return true;
            }
        }
    } else if (levelStep_ == 1) {
        for (int i = 0; i < 8; ++i) {
            if (tag == std::string(kSkillShort[i]) + "_PLUS_BTN") {
                game_.spendSkillRank(i);
                return true;
            }
        }
    } else if (levelStep_ == 2) {
        if (tag == "BTN_SELECT") {
            if (const int feat = gui_.listCellValue("LB_FEATS"); feat >= 0) {
                game_.learnFeat(feat);
            }
            return true;
        }
    }
    if (levelStep_ == 3 && tag == "SELECT_BTN") {
        const std::size_t row = gui_.listSelection("LB_POWERS");
        if (row < abilityRows_.size()) {
            game_.learnLevelUpPower(abilityRows_[row]);
        }
        return true;
    }
    return tag == "LB_FEATS" || tag == "LB_POWERS" || tag.find("_MINUS_BTN") != std::string::npos
        || tag.find("_POINTS_BTN") != std::string::npos;
}

void GameScreens::applyLevelRecommended()
{
    // Retail's Recommended is event 0x2a of each panel's vtable+0x3c: 006f7390 (abilities), 006f4e20
    // (skills), 006f3a80 (feats), 006f1f30 (powers). Retail first restores the rows, as the step's reset does
    // here (resetLevelRow), then spends the whole pool.
    resetLevelRow(levelStep_);
    const Rules& rules = game_.rules();
    const CharacterSheet& sheet = game_.character();
    if (levelStep_ == 0) {
        // Level-up branch of 006f7390: the point goes on classes.2da primaryabil and none remain.
        if (sheet.attributePoints > 0) {
            const std::int32_t primary = rules.primaryAbility(sheet.classId);
            while (primary >= 0 && game_.spendAttributePoint(primary)) {
            }
        }
        return;
    }
    if (levelStep_ == 1) {
        // 006491a0: rank 1 upward, as many ranks as the row takes (00648d50 applies cost and cap).
        for (std::int32_t rank = 1; rank <= 8; ++rank) {
            const std::int32_t skill = rules.skillForRecommendRank(sheet.classId, rank);
            while (skill >= 0 && game_.spendSkillRank(skill)) {
            }
        }
        return;
    }
    std::vector<GuiListItem> taken;
    const auto note = [&](const std::optional<kotor::TwoDa>& data, std::int32_t id, const char* nameColumn, const char* iconColumn) {
        GuiListItem item;
        const std::uint32_t nameRef = strrefOf(cellOf(data, static_cast<std::size_t>(id), nameColumn));
        item.text = nameRef != UINT32_MAX ? tlk(nameRef) : std::string{};
        item.icon = cellOf(data, static_cast<std::size_t>(id), iconColumn);
        item.selectable = false;
        taken.push_back(std::move(item));
    };
    std::uint32_t message = 0;
    if (levelStep_ == 2) {
        // 0064a770: rank 1 upward until the points are gone, each row checked like a pick by hand.
        const auto feats = table(game_.install(), "feat");
        const std::int32_t count = rules.featCount();
        for (std::int32_t rank = 1; rank <= count && game_.character().featPoints > 0; ++rank) {
            const std::int32_t id = rules.featForRecommendRank(sheet.classId, rank);
            if (id >= 0 && rules.featIsAvailable(sheet.classId, id) && game_.learnFeat(id)) {
                note(feats, id, "name", "icon");
            }
        }
        message = 42256; // "The following feat(s) have been recommended."
    } else if (levelStep_ == 3) {
        // 00649c30: light_recom, or dark_recom below alignment 41; rank walk bounded by the row count.
        const auto spells = table(game_.install(), "spells");
        const bool dark = sheet.alignment < 41;
        const std::int32_t count = static_cast<std::int32_t>(rules.powerCount());
        for (std::int32_t rank = 1; rank <= count && game_.character().powerPoints > 0; ++rank) {
            const std::int32_t id = rules.powerForRecommendRank(dark, rank);
            if (id >= 0 && game_.learnLevelUpPower(id)) {
                note(spells, id, "name", "iconresref");
            }
        }
        message = 42257; // "The following power(s) have been recommended."
    }
    if (!taken.empty()) {
        showLevelNotice(std::move(taken), message);
    }
}

std::vector<std::int32_t> GameScreens::grantLevelFeats()
{
    std::vector<std::int32_t> granted;
    CharacterSheet sheet = game_.character();
    const auto feats = table(game_.install(), "feat");
    const std::string prefix = game_.rules().classColumnPrefix(sheet.classId);
    if (!feats || prefix.empty()) {
        return granted;
    }
    const int listColumn = feats->columnIndex(prefix + "_list");
    const int levelColumn = feats->columnIndex(prefix + "_granted");
    const std::string level = std::to_string(sheet.level); // our level rose with the experience: the new level
    for (std::size_t row = 0; listColumn >= 0 && levelColumn >= 0 && row < feats->rowCount(); ++row) {
        const auto id = static_cast<std::int32_t>(row);
        if (feats->cell(row, static_cast<std::size_t>(listColumn)) == "3"
            && feats->cell(row, static_cast<std::size_t>(levelColumn)) == level && !sheet.feats.contains(id)) {
            sheet.feats.insert(id);
            granted.push_back(id);
        }
    }
    if (!granted.empty()) {
        game_.applyCharacter(sheet);
    }
    return granted;
}

void GameScreens::showLevelGranted()
{
    const auto feats = table(game_.install(), "feat");
    std::vector<GuiListItem> rows;
    for (const std::int32_t id : levelGranted_) {
        GuiListItem row;
        const std::uint32_t nameRef = strrefOf(cellOf(feats, static_cast<std::size_t>(id), "name"));
        row.text = nameRef != UINT32_MAX ? game_.localization().tlk().text(nameRef) : std::string{};
        row.icon = cellOf(feats, static_cast<std::size_t>(id), "icon");
        row.selectable = false;
        rows.push_back(std::move(row));
    }
    levelGranted_.clear();
    showLevelNotice(std::move(rows), 0xa512);
}

void GameScreens::showLevelNotice(std::vector<GuiListItem> rows, std::uint32_t message)
{
    // FUN_006cdfc0 fills the skillinfo popup (006ce7c0: LBL_MESSAGE, LB_SKILLS, BTN_OK) and 0040bc70 opens it. Each
    // row is a skillinfo entry (006acb40, vtable 0x755ed0): 42 tall (006ac930), the icon in a lbl_hex_3 frame.
    if (!gui_.pushModal("skillinfo")) {
        return;
    }
    for (GuiListItem& row : rows) {
        row.itemEntry = true;
        row.count = 1;
    }
    const auto& text = game_.localization().tlk();
    gui_.setText("LBL_MESSAGE", text.text(message));
    gui_.setRowHeight("LB_SKILLS", 42.0f); // 006ac930: each entry is 42 tall
    gui_.setItems("LB_SKILLS", std::move(rows));
    gui_.setText("BTN_OK", text.text(1580));
    levelNotice_ = true;
}

void GameScreens::updateLevelUp()
{
    if (levelNotice_) {
        return; // the Recommended popup holds the panel until OK
    }
    if (game_.activePanel() != UiPanel::LevelUp) {
        levelOpen_ = false;
        levelHub_ = false;
        return;
    }
    if (!levelOpen_) {
        // 006ee7d0 decides the rows once, when the panel is built: attributes every fourth level
        // (here: points to place), skills with points, feats with a pick to make, powers with one to
        // learn, and Accept always.
        for (int row = 0; row < 4; ++row) {
            levelRows_[static_cast<std::size_t>(row)] = levelStepAvailable(row);
        }
        levelRows_[4] = true;
        levelHubScreen_ = maincgComposite(gui_, "leveluppnl");
        if (levelHubScreen_.empty()) {
            game_.setUiPanel(UiPanel::None);
            return;
        }
        levelOpen_ = true;
        levelHub_ = true;
        levelStep_ = 0;
        levelFocus_ = 0;
        // 006ee6c0: the lit row starts before the first and steps onto the first shown one.
        levelLit_ = levelHubNext(-1, 1);
        levelFirst_ = levelLit_;
        levelBase_ = game_.character();
    }
}

void GameScreens::resetLevelRow(int row)
{
    // 006e85a0 (scores and points), 006e8730 (skill ranks and points: 006488e0 per row), 006e8db0 (feats),
    // 006e87e0 (powers): the creature's row is set back from the copy taken when the level-up opened.
    CharacterSheet sheet = game_.character();
    const CharacterSheet& base = levelBase_;
    switch (row) {
    case 0:
        sheet.str = base.str;
        sheet.dex = base.dex;
        sheet.con = base.con;
        sheet.intel = base.intel;
        sheet.wis = base.wis;
        sheet.cha = base.cha;
        sheet.attributePoints = base.attributePoints;
        sheet.maxHp = base.maxHp;
        break;
    case 1:
        sheet.skills = base.skills;
        sheet.skillPoints = base.skillPoints;
        break;
    case 2:
        sheet.feats = base.feats;
        sheet.featPoints = base.featPoints;
        break;
    case 3:
        sheet.powers = base.powers;
        sheet.powerPoints = base.powerPoints;
        break;
    default:
        return;
    }
    sheet.hp = std::min(game_.playerHp(), sheet.maxHp);
    game_.applyCharacter(sheet);
    key_.clear();
}

} // namespace ke
