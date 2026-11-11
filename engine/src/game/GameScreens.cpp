#include "game/GameScreens.h"

#include "core/Log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>

namespace ke {

namespace {

std::string lowerOf(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::optional<kotor::TwoDa> loadTable(kotor::Install& install, const char* name)
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

std::string signedText(int value)
{
    // 006ae990: "+" only above zero, so a zero modifier reads "0" (req_levelup_l3_after_ok).
    return (value > 0 ? "+" : "") + std::to_string(value);
}

std::string threeDigits(int value)
{
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%03d", value);
    return buffer;
}

} // namespace

GameScreens::GameScreens(GuiSystem& gui, GameSession& game, GuiPainter& painter)
    : gui_(gui), game_(game), painter_(painter)
{
}

bool GameScreens::owns(const std::string& screenName)
{
    return screenName == "confirm_box" || screenName == "statussummary_fit" || screenName == "pazaakwager" || screenName == "pazaaksetup"
        || screenName == "pazaakgame" || screenName == "upgradesel" || screenName == "upgradeitems"
        || screenName == "upgrade_saber" || screenName == "upgrade_ranged"
        || screenName == "upgrade_melee" || screenName == "upgrade_armor" || screenName == "inventory" || screenName == "journal" || screenName == "character"
        || screenName == "container" || screenName == "store" || screenName == "partyselection"
        || screenName == "equip" || screenName == "abilities" || screenName == "messages"
        || screenName == "galaxymap" || screenName == "map" || screenName == "abchrgen"
        || screenName == "skchrgen" || screenName == "ftchrgen" || screenName == "pwrlvlup";
}

// The class's shown name: classes.2da's name column; its label is a table key ("JediSentinel").
std::string GameScreens::classText(std::int32_t classId) const
{
    if (const std::shared_ptr<const kotor::TwoDa> table = game_.install().table("classes")) {
        const std::string ref = table->cell(std::to_string(classId), "name");
        if (!ref.empty() && ref != "****" && std::isdigit(static_cast<unsigned char>(ref[0])) != 0) {
            return tlk(static_cast<std::uint32_t>(std::stoul(ref)));
        }
    }
    return game_.rules().className(classId);
}

std::string GameScreens::tlk(std::uint32_t strref) const
{
    return game_.localization().tlk().text(strref);
}

std::string GameScreens::wanted() const
{
    // The status summary queues its tutorial (row 14) as it opens (00625c60) and stays the panel on
    // top: retail's frames show the summary with no tutorial over it until OK.
    if (!game_.gameOverMessage() && game_.statusSummaryOpen()) {
        return "statussummary_fit"; // the shipped pop-up, rows closed up (GameScreensExtra.cpp)
    }
    if (messageBoxWanted(game_)) {
        return "confirm_box"; // the shipped message box, grown to fit (GameScreensExtra.cpp)
    }
    if (game_.pazaakActive()) {
        return pazaakScreenName(game_);
    }
    if (game_.upgradeScreenOpen()) {
        return upgradeScreenName();
    }
    if (game_.partySelection().active) {
        return "partyselection";
    }
    if (game_.galaxyMapOpen()) {
        return "galaxymap";
    }
    switch (game_.activePanel()) {
    case UiPanel::Equipment: return "equip";
    case UiPanel::Abilities: return "abilities";
    case UiPanel::Messages: return "messages";
    case UiPanel::Map: return "map";
    case UiPanel::LevelUp: {
        static const char* const names[4] = {"abchrgen", "skchrgen", "ftchrgen", "pwrlvlup"};
        if (!levelOpen_) {
            return "";
        }
        return levelHub_ ? levelHubScreen_ : std::string(names[levelStep_]);
    }
    case UiPanel::Inventory: return "inventory";
    case UiPanel::Journal: return "journal";
    case UiPanel::Character: return "character";
    case UiPanel::Loot: return "container";
    case UiPanel::Merchant: return game_.activeMerchant() >= 0 ? "store" : "";
    case UiPanel::None: break;
    }
    return {};
}

void GameScreens::closeScreen()
{
    if (bound_ == "inventory") {
        for (const std::uint32_t object : seenNewItems_) {
            game_.clearNewItem(object); // 006b4430 -> 00556050: only the items looked at
        }
        seenNewItems_.clear();
    }
    if (upgradeActive_) {
        game_.closeUpgradeScreen();
        upgradeActive_ = false;
    }
    if (game_.activeMerchant() >= 0) {
        game_.closeStore();
    }
    if (game_.activePanel() == UiPanel::Loot) {
        game_.closeLoot();
    }
    if (game_.partySelection().active) {
        game_.cancelPartySelection();
    }
    if (game_.galaxyMapOpen()) {
        game_.closeGalaxyMap();
    }
    game_.setUiPanel(UiPanel::None);
    if (!bound_.empty() && gui_.screenName() == bound_) {
        gui_.close();
    }
    bound_.clear();
    key_.clear();
    equipSelecting_ = false;
    mapNote_ = 0;
    mapPin_ = 0;
    levelOpen_ = false;
}

bool GameScreens::back()
{
    if (bound_.empty()) {
        return false;
    }
    if (bound_.rfind("pazaak", 0) == 0) {
        return pazaakBack();
    }
    if (bound_ == "statussummary_fit") {
        return actionStatusSummary("BTN_OK");
    }
    if (bound_ == "confirm_box" || tutorialModal_) {
        // Escape is the box's OK for a tutorial; the game over message has to be answered.
        return game_.gameOverMessage() ? true : actionMessageBox("BTN_OK");
    }
    if (upgradeActive_ && bound_.rfind("upgrade", 0) == 0) {
        upgradeBack(); // Escape steps back one screen, the first one closes
        return true;
    }
    if (game_.activePanel() == UiPanel::LevelUp && levelOpen_) {
        actionLevelUp("BTN_BACK"); // the panels' key 0x28 is their Back (006ee720 on LEVELUPPNL)
        return true;
    }
    closeScreen();
    return true;
}

void GameScreens::update()
{
    if (bound_ == "partyselection") {
        // Retail 006bfa40 wires 006bf5b0 to each BTN_NPCn's focus event and the lost-focus handler does not
        // clear the boxes: the last member the pointer was on stays shown.
        const std::string hovered = gui_.hovered();
        const std::size_t at = hovered.find("BTN_NPC");
        if (at != std::string::npos) {
            hoverNpc_ = std::atoi(hovered.c_str() + at + 7);
        }
    }
    if (gui_.active() && gui_.screenName() == "optionsingame") {
        updateTabs(); // the pause menu is the tabs' eighth page, not a screen bound here
    }
    recordDialogue();
    updateLevelUp();
    updateUpgrade();
    // The party and merchant panels the keyboard toggles are the shipped screens, not a list: Party
    // asks for the party selection by the UI path (refused while the area is Unescapable, retail
    // 0062dd20) and Merchant opens the nearest store.
    if (game_.activePanel() == UiPanel::Party) {
        game_.setUiPanel(UiPanel::None);
        if (!game_.partySelection().active) {
            game_.requestPartySelectionFromUi();
        }
    } else if (game_.activePanel() == UiPanel::Merchant && game_.activeMerchant() < 0) {
        if (!game_.openNearestStore()) {
            game_.setUiPanel(UiPanel::None);
        }
    }
    if (updatePazaak()) {
        return; // a result popup holds the screen
    }
    if (levelNotice_) {
        return; // the Recommended popup is up over the level-up panel
    }
    if (updateTutorialModal()) {
        return; // a tutorial is up over its panel
    }
    const std::string want = wanted();
    if (!bound_.empty()) {
        if (!gui_.active() || gui_.screenName() != bound_) {
            // Closed from outside (Escape, a script): the panel goes with it.
            closeScreen();
            return;
        }
        if (want != bound_) {
            gui_.close();
            bound_.clear();
            key_.clear();
        }
    }
    if (bound_.empty() && !want.empty() && !gui_.active()) {
        if (want == "confirm_box") {
            bindMessageBox();
        } else if (want == "statussummary_fit") {
            bindStatusSummary();
        } else if (want.rfind("upgrade_", 0) == 0) {
            bindUpgradeScreen(want);
        } else if (gui_.open(want)) {
            bind(want);
        } else {
            game_.setUiPanel(UiPanel::None);
        }
    }
    if (!bound_.empty()) {
        const std::string now = stateKey();
        const std::size_t selected = gui_.listSelection(listTag());
        if (now != key_ || selected != shownRow_) {
            key_ = now;
            shownRow_ = selected;
            refresh();
        }
        // Retail 006b9470: the slot under the pointer (or focused) fills the list on the right.
        if (bound_ == "equip" && !equipSelecting_) {
            hoverEquipSlot();
        }
        if (gui_.control("BTN_EQU") != nullptr) {
            updateTabs();
        }
        if (!levelGranted_.empty() && bound_ == "character" && !levelNotice_) {
            showLevelGranted(); // the granted-feats box over the sheet the level-up returned to (req_levelup_granted)
        }
    }
}

std::string GameScreens::stateKey() const
{
    // Everything the bound screen shows: a change rebuilds the lists.
    std::string key = bound_ + "|" + std::to_string(game_.gold()) + "|"
        + std::to_string(game_.inventory().size()) + "|" + std::to_string(game_.loot().size())
        + "|" + std::to_string(game_.journalSize()) + "|" + std::to_string(game_.activeMerchant())
        + "|" + std::to_string(inventoryMode_) + std::to_string(sellMode_) + std::to_string(completedJournal_);
    int equipped = 0;
    for (const InventoryItem& item : game_.inventory()) {
        equipped += item.equipped ? 1 : 0;
    }
    key += "|" + std::to_string(equipped);
    if (bound_ == "partyselection") {
        for (const int chosen : game_.partySelection().chosen) {
            key += "," + std::to_string(chosen);
        }
        key += "|h" + std::to_string(hoverNpc_);
    }
    if (bound_ == "abilities") {
        key += "|c" + std::to_string(gui_.listCellValue("LB_ABILITY")); // a feat row's icon is picked inside its row
    }
    if (bound_ == "character") {
        const CharacterSheet& points = game_.character();
        key += "|p" + std::to_string(points.attributePoints) + "." + std::to_string(points.skillPoints) + "." + std::to_string(points.featPoints);
    }
    if (bound_ == "equip") {
        key += "|f" + std::to_string(game_.feedbackOptions()) + "." + std::to_string(game_.character().feats.size());
    }
    if (bound_ == "equip" || bound_ == "abilities" || bound_ == "character") {
        // The member shown and what that member wears (a companion's gear is not in the PC's flags).
        key += "|v" + std::to_string(viewedNpc_);
        const std::uint32_t who = viewedObject();
        for (std::int32_t slot = 0; slot < 18; ++slot) {
            key += "," + std::to_string(game_.wornItemObject(who, slot));
        }
    }
    if (bound_ == "character") {
        const CharacterSheet& sheet = viewedSheet();
        key += "|" + std::to_string(sheet.xp) + "," + std::to_string(sheet.hp) + "," + std::to_string(sheet.fp) + ","
            + std::to_string(sheet.alignment);
    }
    return key + (bound_.rfind("pazaak", 0) == 0 ? pazaakStateKey() : std::string{}) + moreStateKey();
}

void GameScreens::bind(const std::string& screen)
{
    // The first time a screen opens its tutorial popup follows (rows of tutorial.2da: messages 12,
    // map 13, inventory 20; the callers are retail 00626d90, 00693650 and 006b4c30).
    if (screen == "messages") {
        game_.showTutorial(12);
    } else if (screen == "map") {
        game_.showTutorial(13);
    } else if (screen == "inventory") {
        game_.showTutorial(20);
    }
    bound_ = screen;
    key_.clear();
    shownRow_ = SIZE_MAX;
    clickedRow_ = SIZE_MAX;
    inventoryMode_ = 0;
    sellMode_ = false;
    completedJournal_ = false;
    hoverNpc_ = -1;
    // The focused slot stays across closing and reopening the panel: 006bb530 OnPanelAdded calls
    // OnEnterSlot on the current focus (+0x1c) and 006b8e10 OnPanelRemoved leaves it; the first
    // opening is on the Body slot (the header default; paired frame ui_equip, retail req_eq_*).
    abilityTab_ = 0;
    viewedNpc_ = kNpcPlayer;
    showDialogLog_ = true; // paired frame ui_messages: the screen opens on the dialog log
    planet_ = -1;
}

void GameScreens::refresh()
{
    if (bound_ == "inventory") {
        refreshInventory();
    } else if (bound_ == "journal") {
        refreshJournal();
    } else if (bound_ == "character") {
        refreshCharacter();
    } else if (bound_ == "container") {
        refreshContainer();
    } else if (bound_ == "store") {
        refreshStore();
    } else if (bound_ == "partyselection") {
        refreshPartySelection();
    } else if (bound_ == "equip") {
        refreshEquipment();
    } else if (bound_ == "abilities") {
        refreshAbilities();
    } else if (bound_ == "messages") {
        refreshMessages();
    } else if (bound_ == "galaxymap") {
        refreshGalaxyMap();
    } else if (bound_ == "map") {
        refreshMap();
    } else if (bound_ == "abchrgen" || bound_ == "skchrgen" || bound_ == "ftchrgen" || bound_ == "pwrlvlup"
               || (!levelHubScreen_.empty() && bound_ == levelHubScreen_)) {
        refreshLevelUp();
    } else if (bound_.rfind("pazaak", 0) == 0) {
        refreshPazaak();
    } else if (bound_ == "confirm_box") {
        refreshMessageBox();
    } else if (bound_.rfind("upgrade", 0) == 0) {
        refreshUpgrade();
    }
}

std::string GameScreens::iconFor(const std::string& itemClass, int variation)
{
    if (!iconsLoaded_) {
        iconsLoaded_ = true;
        for (const std::string& name : game_.install().resourceNames(kotor::resType(kotor::ResType::Tpc))) {
            if (name.size() > 2 && (name[0] == 'i' || name[0] == 'I')) {
                iconNames_.insert(lowerOf(name));
            }
        }
    }
    if (itemClass.empty()) {
        return {};
    }
    const std::string base = "i" + lowerOf(itemClass) + "_";
    for (const int index : {variation, 1}) {
        const std::string candidate = base + threeDigits(index);
        if (iconNames_.empty() || iconNames_.count(candidate) != 0) {
            return candidate;
        }
    }
    return {};
}

const GameScreens::ItemInfo& GameScreens::itemInfo(const InventoryItem& item)
{
    const auto found = items_.find(item.resref);
    if (found != items_.end()) {
        return found->second;
    }
    ItemInfo info;
    info.name = item.name;
    int modelVariation = 1;
    int textureVar = 0;
    if (auto data = game_.install().find(item.resref, 2025)) { // UTI
        try {
            kotor::Gff uti = kotor::Gff::parse(std::move(*data), item.resref + ".uti");
            const auto text = [&](const char* label) {
                const kotor::GffField* field = uti.field(uti.rootIndex(), label);
                if (field == nullptr) {
                    return std::string{};
                }
                if (field->type == kotor::GffType::LocString) {
                    const std::uint32_t strref = uti.locStringStrref(*field);
                    if (strref != UINT32_MAX) {
                        const std::string resolved = tlk(strref);
                        if (!resolved.empty()) {
                            return resolved;
                        }
                    }
                    return uti.locString(*field);
                }
                return uti.text(*field);
            };
            const auto number = [&](const char* label, int fallback) {
                const kotor::GffField* field = uti.field(uti.rootIndex(), label);
                return field != nullptr ? static_cast<int>(uti.integer(*field)) : fallback;
            };
            if (info.name.empty()) {
                info.name = text("LocalizedName");
            }
            info.description = text("DescIdentified");
            if (info.description.empty()) {
                info.description = text("Description");
            }
            modelVariation = number("ModelVariation", 1);
            textureVar = number("TextureVar", 0);
            info.plot = number("Plot", 0) != 0;
        } catch (const kotor::FormatError&) {
        }
    }
    if (info.name.empty()) {
        info.name = item.resref;
    }
    std::string itemClass;
    if (const auto baseitems = loadTable(game_.install(), "baseitems")) {
        const int column = baseitems->columnIndex("itemclass");
        if (column >= 0 && item.baseItem >= 0 && static_cast<std::size_t>(item.baseItem) < baseitems->rowCount()) {
            itemClass = baseitems->cell(static_cast<std::size_t>(item.baseItem), static_cast<std::size_t>(column));
        }
    }
    info.icon = iconFor(itemClass, textureVar > 0 && textureVar != modelVariation && itemClass.rfind("a_", 0) == 0
                                       ? textureVar
                                       : modelVariation);
    return items_.emplace(item.resref, std::move(info)).first->second;
}

std::string GameScreens::describe(const InventoryItem& item)
{
    const ItemInfo& info = itemInfo(item);
    std::string text = info.name;
    if (item.stack > 1) {
        text += " (" + std::to_string(item.stack) + ")";
    }
    if (!info.description.empty()) {
        text += "\n\n" + info.description;
    }
    return text;
}

void GameScreens::refreshInventory()
{
    gui_.setText("LBL_CREDITS_VALUE", std::to_string(game_.gold()));
    // CSWGuiInGameInventory::UpdatePortraits 006b2dc0: BTN_CHANGE1 shows party member 1's portrait
    // when the party has two or more, BTN_CHANGE2 member 2's with three; otherwise the flag that
    // draws the art is cleared and the square stays empty (paired/walk/out_ig5/0033_menus_inventory_pair.png,
    // no party). The panel has no member arrows in retail's frame (they show on equip, not here).
    {
        const std::vector<int> party = memberList();
        for (std::size_t i = 0; i < 2; ++i) {
            const std::string tag = "BTN_CHANGE" + std::to_string(i + 1);
            std::string art;
            if (party.size() > i + 1) {
                const int npc = party[i + 1];
                const CharacterSheet* sheet = npc == kNpcPlayer ? &game_.character() : game_.partySheet(npc);
                if (sheet != nullptr && sheet->portraitRow >= 0) {
                    art = game_.rules().portraitResrefFor(game_.rules().portraitRow(sheet->portraitRow),
                                                          sheet->alignment);
                }
            }
            gui_.setFill(tag, art);
            // With the art flag cleared nothing of the button draws, its BLACKFILL frame included
            // (paired walk out_ig6 0033_menus_inventory: two plain dark squares in retail).
            gui_.setVisible(tag, !art.empty());
        }
        gui_.setVisible("BTN_CHARLEFT", false);
        gui_.setVisible("BTN_CHARRIGHT", false);
    }
    rows_.clear();
    // Retail table 00756444: All, New, Quest, Equippable, Utility, Useable (41822, 42165, 41818, 41821,
    // 41819, 41820). The title names this list and the button offers the next (006b2a80, 006b34c0).
    static constexpr std::uint32_t kModeNames[6] = {41822, 42165, 41818, 41821, 41819, 41820};
    gui_.setText("LBL_INV", tlk(32171) + " - " + tlk(kModeNames[inventoryMode_]));
    gui_.setText("BTN_QUESTITEMS", tlk(42359) + " " + tlk(kModeNames[(inventoryMode_ + 1) % 6]));
    // The filter is retail 006b2c90. Plot is its +0x108; equippable its base item test (inferred from
    // the mode's name to equipableslots, the field offset was not traced to the 2DA loader); useable
    // adds 0051b440, the creature's requirements, as Rules::canEquip. New tests the item's arrival mark
    // (+0x288 bit 7, GameSession::isNewItem).
    const auto listed = [&](const InventoryItem& item, const ItemInfo& info) {
        const bool equippable = game_.rules().equipMask(item.baseItem) != 0;
        switch (inventoryMode_) {
        case 1: return game_.isNewItem(item.object);
        case 2: return info.plot;
        case 3: return equippable;
        case 4: return !info.plot && !equippable;
        case 5: return equippable && game_.rules().canEquip(game_.character(), item);
        default: return true;
        }
    };
    // Paired frame ui_inventory: what is worn comes first, marked (Equipped, strref 32346).
    std::vector<GuiListItem> rows;
    for (const bool worn : {true, false}) {
        for (std::size_t i = 0; i < game_.inventory().size(); ++i) {
            const InventoryItem& item = game_.inventory()[i];
            if (item.equipped != worn) {
                continue;
            }
            const ItemInfo& info = itemInfo(item);
            if (!listed(item, info)) {
                continue;
            }
            GuiListItem row;
            row.text = info.name; // the stack size is on the icon frame (006b6710)
            if (worn) {
                row.text += " (" + tlk(32346) + ")";
            }
            row.icon = info.icon;
            row.itemEntry = true;
            row.count = item.stack;
            row.value = static_cast<int>(i);
            rows.push_back(std::move(row));
            rows_.push_back(i);
        }
    }
    const std::size_t selection = std::min(gui_.listSelection("LB_ITEMS"), rows.empty() ? std::size_t{0} : rows.size() - 1);
    const bool empty = rows.empty();
    gui_.setRowHeight("LB_ITEMS", kItemEntryHeight);
    gui_.setItems("LB_ITEMS", std::move(rows));
    gui_.setListSelection("LB_ITEMS", selection);
    shownRow_ = selection;

    GuiListItem description;
    description.selectable = false;
    if (!empty) {
        const InventoryItem& shown = game_.inventory()[rows_[selection]];
        description.text = describe(shown);
        // 006b3d10: the row whose description shows joins the screen's seen list (+0x1dcc) when it
        // carries the NewItem bit; closing the screen (006b4430 -> 00556050) clears only those.
        if (shown.object != 0 && game_.isNewItem(shown.object) && seenNewItems_.insert(shown.object).second) {
            logInfo(concat("inventory screen: new item seen ", shown.resref, " object ", shown.object, " row ", selection));
        }
    }
    gui_.setItems("LB_DESCRIPTION", {description});

    // The party leader beside the list: portrait, vitality, defence.
    const CharacterSheet& sheet = game_.character();
    if (sheet.portraitRow >= 0) {
        gui_.setFill("LBL_PORT", game_.rules().portraitResrefFor(game_.rules().portraitRow(sheet.portraitRow),
                                                                 sheet.alignment));
    }
    gui_.setText("LBL_VIT", std::to_string(game_.controlledHp()) + "/" + std::to_string(game_.controlledMaxHp())); // paired frame
    gui_.setText("LBL_DEF", std::to_string(game_.rules().armorClass(sheet)));
    gui_.setEnabled("BTN_USEITEM", !empty);
}

void GameScreens::refreshJournal()
{
    // Active quests: the plot ids the session holds, titled by global.jrl.
    std::vector<GuiListItem> rows;
    rows_.clear();
    std::vector<std::string> plots;
    for (const auto& entryPair : game_.journal()) {
        const std::string& plot = entryPair.first;
        if (game_.journalQuestFinished(plot) != completedJournal_) {
            continue;
        }
        GuiListItem row;
        const std::string name = game_.journalCategoryName(plot);
        row.text = name.empty() ? plot : name;
        row.tag = plot;
        rows.push_back(std::move(row));
        plots.push_back(plot);
    }
    const std::size_t selection = std::min(gui_.listSelection("LB_ITEMS"), rows.empty() ? std::size_t{0} : rows.size() - 1);
    gui_.setItems("LB_ITEMS", std::move(rows));
    gui_.setListSelection("LB_ITEMS", selection);
    shownRow_ = selection;
    GuiListItem entry;
    entry.selectable = false;
    if (!plots.empty()) {
        const std::string& plot = plots[selection];
        entry.text = game_.journalEntryText(plot, game_.journalState(plot));
    }
    gui_.setItems("LBL_ITEM_DESCRIPTION", {entry});
    gui_.setText("LBL_TITLE", tlk(completedJournal_ ? 32177 : 32178));
    gui_.setText("BTN_SWAPTEXT", tlk(completedJournal_ ? 32178 : 32177));
    gui_.setEnabled("BTN_QUESTITEMS", false); // the quest-item list is not modelled
    gui_.setEnabled("BTN_SORT", false);       // the journal keeps one order
}

// ---- the party member the equip / abilities / character screens show ---------------------------
// The shipped files carry four controls for it (BTN_CHARLEFT and BTN_CHARRIGHT cycle, BTN_CHANGE1
// and BTN_CHANGE2 hold the portraits of the other members: equip.gui, abilities.gui, character.gui).
// Retail's panel constructors (006ba980 for equip) look the four up by name; their click handlers
// were not read, so the behaviour here is the one the names and art give: the PC first, then the
// members in join order, the two portraits are the two members not shown, the arrows step through
// the list. The arrows show with more than two available NPCs (006b5b60, see refreshMemberSwitch).

std::vector<int> GameScreens::memberList() const
{
    std::vector<int> list{kNpcPlayer};
    for (const int member : game_.partyTable().members) {
        if (member >= 0 && member < kNpcCount && std::find(list.begin(), list.end(), member) == list.end()
            && game_.partySheet(member) != nullptr) {
            list.push_back(member);
        }
    }
    return list;
}

const CharacterSheet& GameScreens::viewedSheet() const
{
    if (viewedNpc_ != kNpcPlayer) {
        if (const CharacterSheet* sheet = game_.partySheet(viewedNpc_)) {
            return *sheet;
        }
    }
    return game_.character();
}

std::uint32_t GameScreens::viewedObject() const
{
    if (viewedNpc_ != kNpcPlayer) {
        for (const std::uint32_t object : game_.partyObjects()) {
            if (game_.npcForObject(object) == viewedNpc_) {
                return object;
            }
        }
    }
    return game_.scriptWorld().pc;
}

// An available NPC whose creature exists in the world: what 006b5b60 lists as a switch target (the loop
// asks 005636b0 for the slot's availability, then looks the stored object up with 005ed5b0).
bool GameScreens::npcShowable(int npc) const
{
    if (npc < 0 || npc >= kNpcCount) {
        return false;
    }
    const PartyNpc& entry = game_.partyTable().npcs[static_cast<std::size_t>(npc)];
    if (!entry.available) {
        return false;
    }
    if (entry.object != script::kObjectInvalid) {
        return true;
    }
    const std::string& tag = !entry.tag.empty() ? entry.tag : game_.partyTable().npcTag[static_cast<std::size_t>(npc)];
    return !tag.empty() && game_.findEntity(tag) != static_cast<std::size_t>(-1);
}

// 006b5b60 (the equip panel's own copy; 006abec0 and 006aee30 are the abilities and character panels'),
// with the mode bit at +0x104 of the client object set: BTN_CHANGE1 and BTN_CHANGE2 hold the next two
// switch targets after the member shown, scanning the nine available-NPC slots from the one after the
// shown member and wrapping; when the shown member is an NPC the player character fills a square that
// the NPCs left free (005ed540 is the PC's portrait). The arrows draw when more than two NPCs are
// available (005636d0, `2 < count`). Paired frames req44_* (every state, retail run 2026-11-08):
// Carth spawned and available with only the PC active fills square 1; Mission available but not
// spawned does not show; viewing Carth gives (PC, empty), with Mission spawned (Mission, PC).
void GameScreens::refreshMemberSwitch()
{
    if (viewedNpc_ != kNpcPlayer && !npcShowable(viewedNpc_)) {
        viewedNpc_ = kNpcPlayer;
    }
    std::array<int, 2> shown{-2, -2};
    int found = 0;
    const int start = viewedNpc_ + 1;
    const auto scan = [&](int from, int to) {
        for (int npc = from; npc < to && found < 2; ++npc) {
            if (npc != viewedNpc_ && npcShowable(npc)) {
                shown[static_cast<std::size_t>(found++)] = npc;
            }
        }
    };
    scan(start, kNpcCount);
    if (found < 2) {
        if (viewedNpc_ != kNpcPlayer) {
            // The player character takes the next free square, after the NPCs found from the start index.
            shown[static_cast<std::size_t>(found++)] = kNpcPlayer;
        }
        scan(0, start);
    }
    changeNpc_ = shown;
    for (std::size_t i = 0; i < 2; ++i) {
        const std::string tag = "BTN_CHANGE" + std::to_string(i + 1);
        const bool present = shown[i] != -2;
        gui_.setVisible(tag, present);
        if (!present) {
            continue;
        }
        const CharacterSheet* sheet = shown[i] == kNpcPlayer ? &game_.character() : game_.partySheet(shown[i]);
        if (sheet != nullptr && sheet->portraitRow >= 0) {
            gui_.setFill(tag, game_.rules().portraitResrefFor(game_.rules().portraitRow(sheet->portraitRow),
                                                              sheet->alignment));
        }
    }
    int availableNpcs = 0;
    for (const PartyNpc& npc : game_.partyTable().npcs) {
        availableNpcs += npc.available ? 1 : 0;
    }
    const bool arrows = availableNpcs > 2;
    gui_.setVisible("BTN_CHARLEFT", arrows);
    gui_.setVisible("BTN_CHARRIGHT", arrows);
}

bool GameScreens::actionMemberSwitch(const std::string& tag)
{
    std::vector<int> list{kNpcPlayer};
    for (int npc = 0; npc < kNpcCount; ++npc) {
        if (npcShowable(npc)) {
            list.push_back(npc);
        }
    }
    const auto position = std::find(list.begin(), list.end(), viewedNpc_);
    const int at = position == list.end() ? 0 : static_cast<int>(position - list.begin());
    const int count = static_cast<int>(list.size());
    int next = -1;
    if (tag == "BTN_CHARLEFT" && count > 1) {
        next = (at + count - 1) % count;
    } else if (tag == "BTN_CHARRIGHT" && count > 1) {
        next = (at + 1) % count;
    } else if ((tag == "BTN_CHANGE1" || tag == "BTN_CHANGE2")) {
        const int target = changeNpc_[tag == "BTN_CHANGE1" ? 0 : 1];
        const auto found = std::find(list.begin(), list.end(), target);
        if (found != list.end()) {
            next = static_cast<int>(found - list.begin());
        }
    } else {
        return false;
    }
    if (next < 0) {
        return true; // a hidden or empty button: nothing to switch to
    }
    viewedNpc_ = list[static_cast<std::size_t>(next)];
    equipSelecting_ = false;
    equipSlot_ = 1; // Retail opens on the Body slot (paired frame ui_equip)
    logInfo(concat("party screen: now showing ", viewedNpc_ == kNpcPlayer ? std::string("the player character")
                                                                          : std::string("npc ") + std::to_string(viewedNpc_)));
    return true;
}

void GameScreens::refreshCharacter()
{
    const CharacterSheet& sheet = viewedSheet();
    refreshMemberSwitch();
    const Rules& rules = game_.rules();
    gui_.setText("LBL_NAME", sheet.name);
    gui_.setText("LBL_CLASS1", classText(sheet.classId));
    gui_.setText("LBL_LEVEL1", std::to_string(sheet.level));
    const bool second = sheet.secondClassId >= 0;
    gui_.setVisible("LBL_CLASS2", second);
    gui_.setVisible("LBL_LEVEL2", second);
    if (second) {
        gui_.setText("LBL_CLASS2", classText(sheet.secondClassId));
        gui_.setText("LBL_LEVEL2", std::to_string(sheet.secondClassLevel));
    }
    const struct {
        const char* tag;
        int value;
    } abilities[] = {{"STR", sheet.str}, {"DEX", sheet.dex}, {"CON", sheet.con},
                     {"INT", sheet.intel}, {"WIS", sheet.wis}, {"CHA", sheet.cha}};
    for (const auto& ability : abilities) {
        gui_.setText(std::string("LBL_") + ability.tag, std::to_string(ability.value));
        const int modifier = (ability.value - 10) >= 0 ? (ability.value - 10) / 2 : -((11 - ability.value) / 2);
        gui_.setText(std::string("LBL_") + ability.tag + "_MOD", signedText(modifier));
    }
    const bool controlledShown = viewedNpc_ == game_.partyTable().controlled;
    gui_.setText("LBL_VITALITY_STAT", std::to_string(controlledShown ? game_.controlledHp() : sheet.hp) + "/"
                                          + std::to_string(controlledShown ? game_.controlledMaxHp() : sheet.maxHp));
    // Retail 006afda0: the Force row only for a class that has Force points.
    gui_.setVisible("LBL_FORCE", sheet.maxFp > 0);
    gui_.setVisible("LBL_FORCE_STAT", sheet.maxFp > 0);
    gui_.setText("LBL_FORCE_STAT", std::to_string(sheet.fp) + "/" + std::to_string(sheet.maxFp));
    // LBL_GOOD1..10 are the active effects' icons (006afda0, up to ten, LBL_MORE past that); the
    // constructor 006b0e40 starts them hidden. The sheet carries no effect list, so none show.
    for (int icon = 1; icon <= 10; ++icon) {
        gui_.setVisible("LBL_GOOD" + std::to_string(icon), false);
    }
    gui_.setVisible("LBL_MORE", false);
    gui_.setText("LBL_DEFENSE_STAT", std::to_string(rules.armorClass(sheet)));
    // 006afda0 shows the whole save (005ab810/005ab8f0/005ab880): class base plus the CON, DEX and WIS
    // modifiers (req_levelup_l3_after_ok: a level 3 Scoundrel with DEX 16 reads Reflex 6, WIS 12 Will 2).
    const auto abilityMod = [](int value) { return (value - 10) >= 0 ? (value - 10) / 2 : -((11 - value) / 2); };
    gui_.setText("LBL_FORTITUDE_STAT", std::to_string(rules.savingThrow(sheet, 0) + abilityMod(sheet.con)));
    gui_.setText("LBL_REFLEX_STAT", std::to_string(rules.savingThrow(sheet, 1) + abilityMod(sheet.dex)));
    gui_.setText("LBL_WILL_STAT", std::to_string(rules.savingThrow(sheet, 2) + abilityMod(sheet.wis)));
    gui_.setText("LBL_EXPERIENCE_STAT", std::to_string(sheet.xp));
    gui_.setText("LBL_NEEDED_XP", std::to_string(rules.xpForLevel(sheet.level + 1)));
    if (const kotor::GuiControl* slider = gui_.control("SLD_ALIGN")) {
        // 006afda0: 00417ee0(100 - alignment), so light (100) sits at the top of the bar and dark at the bottom.
        gui_.setValue("SLD_ALIGN", slider->maxValue * static_cast<float>(100 - std::clamp(sheet.alignment, 0, 100)) / 100.0f);
        gui_.setEnabled("SLD_ALIGN", false); // the alignment is shown, not set, here
    }
    const bool levelUp = sheet.attributePoints > 0 || sheet.skillPoints > 0 || sheet.featPoints > 0;
    gui_.setVisible("BTN_LEVELUP", levelUp);
    gui_.setVisible("BTN_AUTO", levelUp); // paired frame ui_character: neither shows with nothing to spend
    // LBL_3DCHAR is painted by ChargenPreview::showSheet.
}

void GameScreens::refreshContainer()
{
    rows_.clear();
    std::vector<GuiListItem> rows;
    for (std::size_t i = 0; i < game_.loot().size(); ++i) {
        const InventoryItem& item = game_.loot()[i];
        const ItemInfo& info = itemInfo(item);
        GuiListItem row;
        row.text = info.name; // the stack size is on the icon frame (006b6710)
        row.icon = info.icon;
        row.itemEntry = true;
        row.count = item.stack;
        row.value = static_cast<int>(i);
        rows.push_back(std::move(row));
        rows_.push_back(i);
    }
    const std::size_t selection = std::min(gui_.listSelection("LB_ITEMS"), rows.empty() ? std::size_t{0} : rows.size() - 1);
    gui_.setRowHeight("LB_ITEMS", kItemEntryHeight);
    gui_.setItems("LB_ITEMS", std::move(rows));
    gui_.setListSelection("LB_ITEMS", selection);
    shownRow_ = selection;
    // LBL_MESSAGE carries strref 392 ("Items Available to Place in Container"), the caption of the
    // give-items list, which is not modelled; it stays hidden with BTN_GIVEITEMS.
    gui_.setVisible("LBL_MESSAGE", false);
    gui_.setVisible("BTN_GIVEITEMS", false); // putting items in a container is not modelled
}

void GameScreens::refreshStore()
{
    if (game_.activeMerchant() < 0 || static_cast<std::size_t>(game_.activeMerchant()) >= game_.merchants().size()) {
        return;
    }
    const MerchantState& merchant = game_.merchants()[static_cast<std::size_t>(game_.activeMerchant())];
    gui_.setText("LBL_CREDITS_VALUE", std::to_string(game_.gold()));
    gui_.setVisible("LB_SHOPITEMS", !sellMode_);
    gui_.setVisible("LB_INVITEMS", sellMode_);
    gui_.setText("LBL_BUYSELL", tlk(sellMode_ ? 32130 : 32132));
    gui_.setText("BTN_Accept", tlk(sellMode_ ? 32130 : 32132));
    gui_.setText("BTN_Examine", tlk(sellMode_ ? 41937 : 41938));
    gui_.setText("LBL_COST", tlk(sellMode_ ? 41945 : 41943));

    rows_.clear();
    std::vector<GuiListItem> rows;
    const std::string listKey = sellMode_ ? "LB_INVITEMS" : "LB_SHOPITEMS";
    if (sellMode_) {
        for (std::size_t i = 0; i < game_.inventory().size(); ++i) {
            const InventoryItem& item = game_.inventory()[i];
            if (item.equipped) {
                continue;
            }
            const ItemInfo& info = itemInfo(item);
            GuiListItem row;
            row.text = info.name; // the stack size is on the icon frame (006b6710)
            row.icon = info.icon;
            row.itemEntry = true;
            row.count = item.stack;
            row.value = static_cast<int>(i);
            rows.push_back(std::move(row));
            rows_.push_back(i);
        }
    } else {
        for (std::size_t i = 0; i < merchant.stock.size(); ++i) {
            const MerchantStockItem& stock = merchant.stock[i];
            if (!stock.infinite && stock.remaining <= 0) {
                continue;
            }
            const ItemInfo& info = itemInfo(stock.item);
            GuiListItem row;
            row.text = info.name;
            row.icon = info.icon;
            row.itemEntry = true;
            row.count = stock.item.stack;
            row.value = static_cast<int>(i);
            rows.push_back(std::move(row));
            rows_.push_back(i);
        }
    }
    const std::size_t selection = std::min(gui_.listSelection(listKey), rows.empty() ? std::size_t{0} : rows.size() - 1);
    const bool empty = rows.empty();
    gui_.setRowHeight(listKey, kItemEntryHeight);
    gui_.setItems(listKey, std::move(rows));
    gui_.setListSelection(listKey, selection);
    shownRow_ = selection;

    GuiListItem description;
    description.selectable = false;
    std::string cost;
    std::string stock;
    if (!empty) {
        const std::size_t index = rows_[selection];
        if (sellMode_) {
            description.text = describe(game_.inventory()[index]);
            cost = std::to_string(game_.sellPrice(index));
            stock = std::to_string(game_.inventory()[index].stack);
        } else {
            description.text = describe(merchant.stock[index].item);
            cost = std::to_string(game_.buyPrice(index));
            stock = merchant.stock[index].infinite ? "-" : std::to_string(merchant.stock[index].remaining);
        }
    }
    gui_.setItems("LB_DESCRIPTION", {description});
    gui_.setText("LBL_COST_VALUE", cost);
    gui_.setText("LBL_STOCK_VALUE", stock);
    gui_.setEnabled("BTN_Accept", !empty);
}

void GameScreens::refreshPartySelection()
{
    const PartySelectionState& selection = game_.partySelection();
    const PartyTable& table = game_.partyTable();
    // "Available Slots": the places still free, so it falls as members are picked (ENGINE CHOICE
    // from the label's text; the file only carries the caption).
    gui_.setText("LBL_COUNT", std::to_string(std::max(0, kMaxPartyNpcs - static_cast<int>(selection.chosen.size()))));
    for (int npc = 0; npc < kNpcCount; ++npc) {
        const std::string number = std::to_string(npc);
        const PartyNpc& slot = table.npcs[static_cast<std::size_t>(npc)];
        const bool selectable = slot.available && slot.selectable();
        gui_.setVisible("LBL_NA" + number, !selectable);
        gui_.setEnabled("BTN_NPC" + number, selectable);
        gui_.setSelected("BTN_NPC" + number,
                         std::find(selection.chosen.begin(), selection.chosen.end(), npc) != selection.chosen.end());
        if (slot.available && slot.hasSheet && slot.sheet.portraitRow >= 0) {
            gui_.setFill("LBL_CHAR" + number,
                         game_.rules().portraitResrefFor(game_.rules().portraitRow(slot.sheet.portraitRow),
                                                         slot.sheet.alignment));
        }
    }
    // The hovered member, else the last one picked, else the first one that can be picked (so the
    // name, level and preview boxes are never left blank while members are on offer).
    int firstOffered = -1;
    for (int npc = 0; npc < kNpcCount && firstOffered < 0; ++npc) {
        const PartyNpc& slot = table.npcs[static_cast<std::size_t>(npc)];
        firstOffered = slot.available && slot.selectable() ? npc : -1;
    }
    const int shown = hoverNpc_ >= 0 ? hoverNpc_ : (selection.chosen.empty() ? firstOffered : selection.chosen.back());
    // Retail 006bf5b0, the focus handler of each BTN_NPCn: an empty member hides the three boxes. A member's
    // LBL_3D is not a model: its fill (+0x70 of the control) becomes the portrait's name plus "3", a shipped
    // full body picture, or plus "3e" below alignment 50 when that art exists. LBL_NPC_LEVEL is
    // "%s %d %s" of strref 32154, the level and the class's name.
    const PartyNpc* member = shown >= 0 && shown < kNpcCount && table.npcs[static_cast<std::size_t>(shown)].available
        ? &table.npcs[static_cast<std::size_t>(shown)]
        : nullptr;
    // Retail 006becc0, called from the focus (006bf5b0) and click (006bf2a0) handlers: BTN_ACCEPT reads Remove
    // (38456) for a member already picked, else Add (38455). Its text is 007a23b4 (0, 0.66, 0.98), or 007a2408
    // (0, 0.33, 0.49) when it says Add and both places are taken.
    {
        const bool picked = shown >= 0
            && std::find(selection.chosen.begin(), selection.chosen.end(), shown) != selection.chosen.end();
        const bool full = static_cast<int>(selection.chosen.size()) >= kMaxPartyNpcs;
        gui_.setText("BTN_ACCEPT", tlk(picked ? 38456 : 38455));
        gui_.setTextColor("BTN_ACCEPT", !picked && full ? kotor::Vec3{0.0f, 0.33f, 0.49f} : kotor::Vec3{0.0f, 0.66f, 0.98f});
    }
    gui_.setVisible("LBL_3D", member != nullptr);
    gui_.setVisible("LBL_NPC_NAME", member != nullptr);
    gui_.setVisible("LBL_NPC_LEVEL", member != nullptr);
    if (member != nullptr) {
        gui_.setText("LBL_NPC_NAME", member->sheet.name.empty() ? member->name : member->sheet.name);
        std::string line;
        if (member->hasSheet) {
            const std::int32_t cls = member->sheet.secondClassId >= 0 ? member->sheet.secondClassId : member->sheet.classId;
            line = tlk(32154) + " " + std::to_string(member->sheet.level) + " " + classText(cls);
        }
        gui_.setText("LBL_NPC_LEVEL", line);
        std::int32_t row = member->hasSheet ? member->sheet.portraitRow : -1;
        std::int32_t alignment = member->hasSheet ? member->sheet.alignment : 50;
        if (row < 0) {
            if (auto data = game_.install().find(member->templateResref, 2027)) { // UTC
                try {
                    const kotor::Gff utc = kotor::Gff::parse(std::move(*data), member->templateResref + ".utc");
                    if (const kotor::GffField* field = utc.field(utc.rootIndex(), "PortraitId")) {
                        row = static_cast<std::int32_t>(utc.integer(*field));
                    }
                    if (const kotor::GffField* field = utc.field(utc.rootIndex(), "GoodEvil")) {
                        alignment = static_cast<std::int32_t>(utc.integer(*field));
                    }
                } catch (const kotor::FormatError&) {
                }
            }
        }
        std::string picture;
        if (row >= 0) {
            const std::string base = game_.rules().portraitRow(row).resref;
            if (!base.empty()) {
                picture = base + "3";
                if (alignment < 50 && (game_.install().find(picture + "e", 3).has_value()
                                       || game_.install().find(picture + "e", 3007).has_value())) {
                    picture += "e";
                }
            }
        }
        gui_.setFill("LBL_3D", picture);
    }
}

UiPanel GameScreens::tabPanel(const std::string& tag)
{
    if (tag == "BTN_EQU") return UiPanel::Equipment;
    if (tag == "BTN_INV") return UiPanel::Inventory;
    if (tag == "BTN_CHAR") return UiPanel::Character;
    if (tag == "BTN_ABI") return UiPanel::Abilities;
    if (tag == "BTN_MSG") return UiPanel::Messages;
    if (tag == "BTN_JOU") return UiPanel::Journal;
    if (tag == "BTN_MAP") return UiPanel::Map;
    return UiPanel::None;
}

void GameScreens::requestOptions()
{
    if (tutorialModal_) {
        return; // the tutorial box over a panel holds the keys
    }
    game_.setUiPanel(UiPanel::None);
    optionsRequested_ = true;
}

bool GameScreens::takeOptionsRequest()
{
    if (!optionsRequested_ || gui_.active()) {
        return false;
    }
    optionsRequested_ = false;
    return true;
}

// Retail 00627980: LBLH_x draws lbl_icn_x, its HILIGHT lbl_icn_x2 for the page on show (0062cf10 picks
// it) and for the tab under the pointer (events 0x35/0x36, 00624c00/00624c30).
void GameScreens::updateTabs()
{
    struct Tab {
        const char* button;
        const char* label;
        const char* fill;
        const char* page;
    };
    static constexpr Tab kTabs[] = {
        {"BTN_EQU", "LBLH_EQU", "lbl_icn_equ", "equip"},       {"BTN_INV", "LBLH_INV", "lbl_icn_inv", "inventory"},
        {"BTN_CHAR", "LBLH_CHA", "lbl_icn_char", "character"}, {"BTN_ABI", "LBLH_ABI", "lbl_icn_abi", "abilities"},
        {"BTN_MSG", "LBLH_MSG", "lbl_icn_msg", "messages"},    {"BTN_JOU", "LBLH_JOU", "lbl_icn_que", "journal"},
        {"BTN_MAP", "LBLH_MAP", "lbl_icn_map", "map"},         {"BTN_OPT", "LBLH_OPT", "lbl_icn_opt", "optionsingame"},
    };
    for (const Tab& tab : kTabs) {
        const bool lit = gui_.screenName() == tab.page || gui_.state(tab.button).hovered;
        gui_.setFill(tab.label, std::string(tab.fill) + (lit ? "2" : ""));
    }
}

bool GameScreens::action(const std::string& tag)
{
    if (bound_.empty()) {
        return false;
    }
    if (tutorialModal_) {
        return actionMessageBox(tag); // the panel below is inert while its tutorial is up
    }
    if (levelNotice_ && bound_ == "character") {
        // The granted-feats box over the sheet: OK closes it and leaves the sheet (req_levelup_l3_after_ok).
        if (tag == "BTN_OK") {
            gui_.popModal();
            levelNotice_ = false;
            key_.clear();
            return true;
        }
        return tag == "LB_SKILLS";
    }
    if (const UiPanel page = tabPanel(tag); page != UiPanel::None) {
        if (page != game_.activePanel()) {
            game_.setUiPanel(page); // Retail 00624cf0: 0062cf10 goes to that page; its own tab does nothing
        }
        return true;
    }
    if (tag == "BTN_OPT") {
        requestOptions();
        return true;
    }
    const auto doubleClicked = [&](const std::string& list) {
        const int row = gui_.lastItem();
        if (row < 0) {
            return false;
        }
        const bool again = clickedRow_ == static_cast<std::size_t>(row);
        clickedRow_ = static_cast<std::size_t>(row);
        gui_.setListSelection(list, static_cast<std::size_t>(row));
        return again;
    };
    const std::size_t selected = gui_.listSelection(listTag());
    const bool hasRow = selected < rows_.size();

    if (pazaakPopup_ != 0) {
        return actionPazaakPopup(tag);
    }
    if (bound_.rfind("pazaak", 0) == 0) {
        return actionPazaak(tag);
    }
    if (bound_ == "confirm_box") {
        return actionMessageBox(tag);
    }
    if (bound_ == "statussummary_fit") {
        return actionStatusSummary(tag);
    }
    if (upgradeActive_ && bound_.rfind("upgrade", 0) == 0) {
        return actionUpgrade(tag);
    }
    if (bound_ == "equip" && equipSelecting_ && (tag == "BTN_BACK" || tag == "BTN_CANCEL")) {
        equipSelecting_ = false; // Cancel in the item list returns to the slots
        return true;
    }
    if (game_.activePanel() == UiPanel::LevelUp) {
        return actionLevelUp(tag);
    }
    if ((bound_ == "equip" || bound_ == "abilities" || bound_ == "character") && actionMemberSwitch(tag)) {
        return true;
    }
    if (tag == "BTN_EXIT" || tag == "BTN_CANCEL" || tag == "BTN_Cancel" || tag == "BTN_BACK") {
        closeScreen();
        return true;
    }
    if (bound_ == "inventory") {
        if (tag == "LB_ITEMS") {
            if (doubleClicked("LB_ITEMS") && static_cast<std::size_t>(gui_.lastItem()) < rows_.size()) {
                const InventoryItem& picked = game_.inventory()[rows_[static_cast<std::size_t>(gui_.lastItem())]];
                if (picked.equipped) {
                    return true; // a worn row stays worn; taking it off is the equipment screen's
                }
                const std::string& resref = picked.resref;
                const bool equipped = game_.equipItem(resref);
                logInfo(concat("inventory screen: equip ", resref, equipped ? " ok" : " refused"));
            }
            return true;
        }
        if (tag == "BTN_QUESTITEMS") {
            inventoryMode_ = (inventoryMode_ + 1) % 6; // retail 006b2a80
            return true;
        }
        if (tag == "BTN_USEITEM") {
            if (hasRow) {
                const InventoryItem& item = game_.inventory()[rows_[selected]];
                if (item.object != 0) {
                    game_.useItem(item.object, game_.scriptWorld().pc);
                }
            }
            return true;
        }
        return false;
    }
    if (bound_ == "journal") {
        if (tag == "BTN_SWAPTEXT") {
            completedJournal_ = !completedJournal_;
            gui_.setListSelection("LB_ITEMS", 0);
            return true;
        }
        return tag == "LB_ITEMS";
    }
    if (bound_ == "container") {
        if (tag == "BTN_OK") {
            game_.takeAllLoot();
            if (game_.loot().empty()) {
                closeScreen();
            }
            return true;
        }
        if (tag == "LB_ITEMS") {
            if (doubleClicked("LB_ITEMS") && static_cast<std::size_t>(gui_.lastItem()) < rows_.size()) {
                game_.takeLootItem(rows_[static_cast<std::size_t>(gui_.lastItem())]);
            }
            return true;
        }
        return false;
    }
    if (bound_ == "store") {
        if (tag == "BTN_Examine") {
            sellMode_ = !sellMode_;
            gui_.setListSelection(sellMode_ ? "LB_INVITEMS" : "LB_SHOPITEMS", 0);
            return true;
        }
        if (tag == "BTN_Accept" || tag == "LB_SHOPITEMS" || tag == "LB_INVITEMS") {
            const bool commit = tag == "BTN_Accept"
                || (doubleClicked(tag) && static_cast<std::size_t>(gui_.lastItem()) < rows_.size());
            if (commit && hasRow) {
                const std::size_t index = tag == "BTN_Accept" ? rows_[selected]
                                                              : rows_[static_cast<std::size_t>(gui_.lastItem())];
                if (sellMode_) {
                    game_.sellInventoryItem(index);
                } else {
                    game_.buyMerchantItem(index);
                }
            }
            return true;
        }
        return false;
    }
    if (bound_ == "partyselection") {
        if (tag.rfind("BTN_NPC", 0) == 0) {
            const int npc = std::atoi(tag.c_str() + 7);
            hoverNpc_ = npc;
            game_.partySelectionToggle(npc);
            return true;
        }
        // Retail 006bfa40 binds BTN_ACCEPT to the same handler as the portraits (006bf2a0), which toggles the
        // focused member (+0x3a7c); BTN_DONE (006bf3b0) is the one that confirms the party.
        if (tag == "BTN_ACCEPT") {
            const PartySelectionState& selection = game_.partySelection();
            const int focused = hoverNpc_ >= 0 ? hoverNpc_ : (selection.chosen.empty() ? -1 : selection.chosen.back());
            if (focused >= 0) {
                hoverNpc_ = focused;
                game_.partySelectionToggle(focused);
            }
            return true;
        }
        if (tag == "BTN_DONE") {
            if (game_.acceptPartySelection()) {
                game_.setUiPanel(UiPanel::None);
                gui_.close();
                bound_.clear();
                key_.clear();
            }
            return true;
        }
        return false;
    }
    if (bound_ == "equip") {
        return actionEquipment(tag);
    }
    if (bound_ == "abilities") {
        return actionAbilities(tag);
    }
    if (bound_ == "messages") {
        return actionMessages(tag);
    }
    if (bound_ == "galaxymap") {
        return actionGalaxyMap(tag);
    }
    if (bound_ == "map") {
        return actionMap(tag);
    }
    if (bound_ == "character" && tag == "BTN_LEVELUP") {
        game_.setUiPanel(UiPanel::LevelUp); // the shipped level-up screens take the character screen's place
        return true;
    }
    return bound_ == "character" && (tag == "BTN_SCRIPTS" || tag == "BTN_AUTO");
}

} // namespace ke
