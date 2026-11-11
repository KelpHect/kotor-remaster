#pragma once

#include <chrono>

// The in-game shipped screens that open over the world: inventory, journal, character
// sheet, container, store and party selection. Each is the shipped .gui bound to the live
// session (items with their icons and descriptions, journal.jrl text, the character
// sheet, the store's prices), driven by the same GuiSystem events as the front end. The
// session keeps the model (UiPanel, the open store/container, the party selection); this
// class only opens the screen that stands for it and turns control actions into calls.

#include "game/GameSession.h"
#include "game/GuiPaint.h"
#include "game/GuiSystem.h"

#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ke {

class GameScreens {
public:
    GameScreens(GuiSystem& gui, GameSession& game, GuiPainter& painter);

    // Once per frame before painting: opens or closes the shipped screen that stands for
    // the session's open panel, and refreshes its content when the session changed.
    void update();
    // A control action (tag). True when this class consumed it.
    bool action(const std::string& tag);
    // Escape / Back: closes the open screen and the session panel behind it.
    bool back();
    // True while one of these screens is on the GuiSystem (the world is paused).
    bool open() const { return !bound_.empty(); }
    const std::string& screen() const { return bound_; }
    static bool owns(const std::string& screenName);

    struct ItemInfo {
        std::string name;
        std::string description;
        std::string icon;
        bool plot = false;
    };
    // Name, description and icon of an item as the inventory shows them (the HUD's item slot).
    const ItemInfo& itemDetails(const InventoryItem& item) { return itemInfo(item); }

    // Audit 5: every icon the screens can ask for, resolved through the same code and the painter's
    // texture loader. `failures` lists "key icon" strings (first 24); rows with no name are not asked.
    struct IconGroup {
        std::string name;
        int total = 0;
        int missing = 0;
        std::vector<std::string> failures;
    };
    std::vector<IconGroup> iconAudit();

    // The galaxy map's planet model.
    std::string previewPlanetModel() const;

    // The page tabs (top.gui): the panel a BTN_EQU .. BTN_MAP button shows, None for others.
    static UiPanel tabPanel(const std::string& tag);
    // A page closed and the pause menu asked for (BTN_OPT, the O key): true once, the caller opens it.
    void requestOptions();
    bool takeOptionsRequest();

    // The map's selected pin; a click on one (the application hit tests the art) selects it.
    int mapPinSelected() const { return mapPin_; }
    void selectMapPin(int index);

    int inventoryMode() const { return inventoryMode_; }

    // Frames the opponent waits between Pazaak decisions (the gate shortens it).
    void setPazaakOpponentDelay(int frames);

private:

    std::string wanted() const;
    void bind(const std::string& screen);
    void refresh();
    void refreshInventory();
    void refreshJournal();
    void refreshCharacter();
    void refreshContainer();
    void refreshStore();
    void refreshPartySelection();
    void refreshEquipment();
    void refreshAbilities();
    void refreshMessages();
    void refreshGalaxyMap();
    void refreshMap();
    bool actionMap(const std::string& tag);
    void refreshLevelUp();
    bool actionLevelUp(const std::string& tag);
    void applyLevelRecommended();
    void updateLevelUp();
    void updateTabs();
    void hoverEquipSlot();
    bool optionsRequested_ = false;
    int levelFocus_ = 0;        // the ability or skill row whose help is shown
    bool levelNotice_ = false;  // the Recommended popup (skillinfo) is up
    // The class feats granted at the new level on Accept (LAB_006ee780 -> 00649950), shown over the sheet after.
    std::vector<std::int32_t> levelGranted_;
    std::vector<std::int32_t> grantLevelFeats();
    void showLevelNotice(std::vector<GuiListItem> rows, std::uint32_t message);
    void showLevelGranted();
    bool levelStepAvailable(int step) const;
    int levelHover() const;
    bool actionEquipment(const std::string& tag);
    bool actionAbilities(const std::string& tag);
    // The party member the equip, abilities and character screens show (the four shipped controls
    // BTN_CHARLEFT/BTN_CHARRIGHT/BTN_CHANGE1/BTN_CHANGE2 switch it; the PC when nothing else is chosen).
    std::vector<int> memberList() const;
    const CharacterSheet& viewedSheet() const;
    std::uint32_t viewedObject() const;
    void refreshMemberSwitch();
    bool npcShowable(int npc) const;
    bool actionMemberSwitch(const std::string& tag);
    bool actionMessages(const std::string& tag);
    bool actionGalaxyMap(const std::string& tag);
    std::string listTag() const;
    std::string moreStateKey() const;
    void recordDialogue();
    const InventoryItem* itemByObject(std::uint32_t object) const;
    const InventoryItem* rowItem(std::size_t row) const;
    void closeScreen();
    const ItemInfo& itemInfo(const InventoryItem& item);
    std::string iconFor(const std::string& itemClass, int variation);
    std::string tlk(std::uint32_t strref) const;
    std::string classText(std::int32_t classId) const;
    std::string describe(const InventoryItem& item);
    std::string stateKey() const;

    // Message boxes: tutorial popups and the game over message (GameScreensExtra.cpp).
    void bindMessageBox();
    void bindStatusSummary();
    bool actionStatusSummary(const std::string& tag);
    void refreshMessageBox();
    bool actionMessageBox(const std::string& tag);
    static bool messageBoxWanted(const GameSession& game);
    std::int32_t tutorialRow_ = -1;
    int tutorialPage_ = 0;
    // A tutorial raised by opening a panel (inventory 20, map 13, messages 12) sits over that panel,
    // which stays drawn and inert below it, rather than replacing it.
    bool prepareMessageBox();
    bool updateTutorialModal();
    bool tutorialModal_ = false;
    std::string tutorialKey_;

    // The upgrade bench (GameScreensExtra.cpp): upgradesel -> upgradeitems -> upgrade, the three
    // shipped screens ShowUpgradeScreen opens (retail 00543990 -> 0062e760).
    void updateUpgrade();
    std::string upgradeScreenName() const;
    void bindUpgradeScreen(const std::string& screen);
    void refreshUpgrade();
    bool actionUpgrade(const std::string& tag);
    std::string upgradeStateKey() const;
    void upgradeBack();
    const InventoryItem* upgradeTargetItem(std::size_t* index = nullptr) const;
    std::string upgradedDescription(const InventoryItem& item);
    InventoryItem upgradeRowItem(int row);
    bool upgradeActive_ = false;
    int upgradeStage_ = 0;                   // 0 upgradesel, 1 upgradeitems, 2 upgrade
    int upgradeCategory_ = -1;               // 0 lightsaber, 1 ranged, 2 melee, 3 armor
    std::uint32_t upgradeObject_ = 0;        // the item being upgraded (its script object) ...
    std::string upgradeResref_;              // ... and template
    int upgradeSlot_ = 0;                    // the slot type being filled
    std::map<int, int> upgradePending_;      // slot type -> upgrade.2da row chosen (-1 = empty)
    std::vector<int> upgradeListRows_;       // upgrade screen list row -> upgrade.2da row (-1 = none)

    // Pazaak (GameScreensPazaak.cpp): pazaakwager -> pazaaksetup -> pazaakgame, with a popup for the
    // set and match results and for the side deck question.
    static std::string pazaakScreenName(const GameSession& game);
    bool updatePazaak();
    void refreshPazaak();
    bool actionPazaak(const std::string& tag);
    bool pazaakBack();
    void openPazaakPopup(int kind);
    std::string pazaakPopupText(int kind);
    bool actionPazaakPopup(const std::string& tag);
    void refreshPazaakWager();
    std::string pazaakStateKey() const;
    PazaakHelp* pazaakHelp(); // the help box of the panel on top (setup or table), or null
    void pazaakHelpClosed();
    int pazaakPopup_ = 0;                    // 0 none, 1 set result, 2 side deck question, 3 match result, 9 help
    int pazaakWait_ = 0;
    int pazaakDelay_ = 45;
    float pazaakFixedStep_ = 0.0f;           // the checks' fixed frame time; 0 runs on the clock
    std::chrono::steady_clock::time_point pazaakClock_ = std::chrono::steady_clock::now();
    int pazaakPick_ = -1;                    // setup: the card kind picked on the left

    GuiSystem& gui_;
    GameSession& game_;
    GuiPainter& painter_;
    std::string bound_;
    std::string key_;                 // what the content was built from
    std::size_t shownRow_ = SIZE_MAX; // the list row the description shows
    std::size_t clickedRow_ = SIZE_MAX;
    int inventoryMode_ = 0;           // inventory: which of the six lists (retail party byte +0xbc1)
    bool sellMode_ = false;           // store: the player's list
    bool completedJournal_ = false;
    std::set<std::uint32_t> seenNewItems_; // retail +0x1dcc: new items whose description showed
    std::vector<std::size_t> rows_;   // list row -> session index
    std::map<std::string, ItemInfo> items_;
    std::set<std::string> iconNames_;
    bool iconsLoaded_ = false;
    int hoverNpc_ = -1;
    std::uint32_t mapNote_ = 0; // strref of the reason the map panel last refused something
    int mapPin_ = 0;            // the selected pin among GameSession::mapPins
    bool equipSelecting_ = false;            // equipment: a slot was picked, its items are listed
    int equipSlot_ = 1;                      // equipment: the selected inventory slot
    std::uint32_t equipWornObject_ = 0;      // equipment: the item object of the worn row
    bool equipWornRow_ = false;              // equipment: row 0 is what is worn in the slot
    std::vector<std::size_t> equipRows_;     // equipment: list row -> inventory index
    int abilityTab_ = 0;                     // abilities: skills, powers, feats
    int viewedNpc_ = -1;                     // equip/abilities/character: kNpcPlayer or an NPC id
    std::array<int, 2> changeNpc_{-1, -1};   // the NPC each of BTN_CHANGE1/2 switches to (-2 = none)
    std::vector<std::int32_t> abilityRows_;  // abilities: list row -> 2da row
    bool showDialogLog_ = true;             // messages: the conversation history
    std::vector<std::string> dialogueLog_;
    std::uint32_t lastDialogueEntry_ = UINT32_MAX;
    int levelStep_ = 0;                      // level up: ability, skill, feat screen
    bool levelOpen_ = false;
    // Level up opens on MAINCG with LEVELUPPNL laid in (retail 006e8ef0 / 006ee7d0); each row opens its step
    // screen, whose Accept or Back comes back here.
    bool levelHub_ = false;
    std::string levelHubScreen_;             // "maincg_leveluppnl"
    std::array<bool, 5> levelRows_{};        // 006ee7d0 at open: attributes, skills, feats, powers, accept
    int levelLit_ = 0;                       // +0x1ccc: the lit row
    int levelFirst_ = 0;                     // +0x1cd0: the first shown row (Back is off there)
    bool levelHubRow(int row) const { return row >= 0 && row < 5 && levelRows_[static_cast<std::size_t>(row)]; }
    int levelHubNext(int from, int direction) const;
    void refreshLevelHub();
    // The sheet as the hub opened; lighting a row (006ee0e0 -> 006e85a0 / 006e8730 / 006e8db0 / 006e87e0) puts
    // that row's picks back to it, so a row reached again by Back, or a step left by its own Back, starts over.
    CharacterSheet levelBase_;
    void resetLevelRow(int row);
    int planet_ = -1;                        // galaxy map: the planet picked
    std::map<std::string, int> galaxyTags_;  // galaxy map: planet button -> planetary.2da row
};

} // namespace ke
