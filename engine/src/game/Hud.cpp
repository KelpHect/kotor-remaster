#include "game/Hud.h"

#include "game/GuiPaint.h"
#include "game/AudioSystem.h"
#include "audio/Audio.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>

namespace ke {

namespace {

bool startsWith(const std::string& text, const char* prefix)
{
    return text.rfind(prefix, 0) == 0;
}

// Controls that exist for events or states the HUD only shows while they are true; the
// session turns them on when the event is on.
bool eventControl(const std::string& tag)
{
    for (const char* prefix : {"LBL_CMBTEFCT", "LBL_DEBILATATED", "LBL_DISABLE", "LBL_LEVELUP", "LBL_LVLUPBG",
                               "LBL_INDICATE", "LBL_SOLOMODE", "LBL_JOURNAL", "LBL_CASH", "LBL_PLOTXP",
                               "LBL_STEALTHXP", "LBL_DARKSHIFT", "LBL_LIGHTSHIFT", "LBL_ITEMRCVD",
                               "LBL_ITEMLOST", "LBL_CMBTMSGBG", "LBL_CMBTMODEMSG", "LBL_COMBATBG",
                               "LBL_ACTIONTYPE", "LB_ACTIONS", "BTN_CLOSEMENU", "LBL_HIGHLIGHT", "LBH_",
                               "BTN_CLEARONE2", "LBL_ACTIONDESC"}) {
        if (startsWith(tag, prefix)) {
            return true;
        }
    }
    return false;
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

std::string cell(const std::optional<kotor::TwoDa>& data, std::size_t row, const char* column)
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

std::string lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// CSWGuiMainInterface::UpdatePortraits (00687860), outside the leader-swap animation: party member i
// sits in portrait slot i, except that a party of two puts its second member in the third slot
// (LBL_CHAR3) and leaves the second empty. -1 for an empty slot.
int memberInPortraitSlot(int slot, int members)
{
    if (members == 2) {
        return slot == 0 ? 0 : (slot == 2 ? 1 : -1);
    }
    return slot < members ? slot : -1;
}

// The feats GameSession::useFeat turns into a combat mode.
// "BTN_ACTION2" -> 2 with the prefix "BTN_ACTION"; -1 when the tag is not that control.
int slotOf(const std::string& tag, const char* prefix)
{
    if (!startsWith(tag, prefix)) {
        return -1;
    }
    const std::string rest = tag.substr(std::char_traits<char>::length(prefix));
    if (rest.size() != 1 || rest[0] < '0' || rest[0] > '9') {
        return -1;
    }
    return rest[0] - '0';
}

constexpr std::uint32_t kStrrefAttack = 375; // "Attack"

} // namespace

Hud::Hud(GuiSystem& gui, GameSession& game, GameScreens* screens)
    : gui_(gui), game_(game), screens_(screens)
{
}

namespace {

// Retail 0068c4e2..0068c56c: the file for the window, drawn 1:1. Its sizes are the ones 0040a3e0 lists.
const char* retailHudFile(int width, int height)
{
    if (width == 1024 && height == 768) {
        return "mipc210x7";
    }
    if (width == 1280 && height == 960) {
        return "mipc212x9";
    }
    if (width == 1280 && height == 1024) {
        return "mipc212x10";
    }
    if (width == 1600 && height == 1200) {
        return "mipc216x12";
    }
    return nullptr;
}

} // namespace

std::string Hud::layoutFor(float windowWidth, float windowHeight, float& designWidth, float& designHeight)
{
    const int width = static_cast<int>(windowWidth);
    const int height = static_cast<int>(windowHeight);
    if (const char* file = retailHudFile(width, height)) {
        designWidth = windowWidth;
        designHeight = windowHeight;
        return file;
    }
    // ENGINE CHOICE at every size retail never ran: its files keep each icon and bar at one pixel size
    // and only spread them out, so the compact file with each control anchored to its window edge
    // (GuiView::anchored) is the same HUD, scaled by layoutScale so it stays readable on large windows.
    designWidth = 800.0f;
    designHeight = 600.0f;
    return "mipc28x6";
}

float Hud::layoutScale(float windowWidth, float windowHeight)
{
    return retailHudFile(static_cast<int>(windowWidth), static_cast<int>(windowHeight)) != nullptr
        ? 1.0f
        : std::max(1.0f, windowHeight / 720.0f);
}

void Hud::hideEventControls()
{
    const std::function<void(const kotor::GuiControl&)> walk = [&](const kotor::GuiControl& control) {
        if (eventControl(control.tag)) {
            gui_.setVisible(control.key, false);
        }
        for (const kotor::GuiControl& child : control.children) {
            walk(child);
        }
    };
    if (gui_.screen() != nullptr) {
        for (const kotor::GuiControl& control : gui_.screen()->controls) {
            walk(control);
        }
    }
}

std::string Hud::itemIcon(const InventoryItem& item) const
{
    return screens_ != nullptr ? screens_->itemDetails(item).icon : std::string{};
}

void Hud::rebuildOptions()
{
    for (std::vector<Option>& slot : options_) {
        slot.clear();
    }
    if (!tablesLoaded_) {
        tablesLoaded_ = true;
        feats_ = loadTable(game_.install(), "feat");
        spells_ = loadTable(game_.install(), "spells");
        baseItems_ = loadTable(game_.install(), "baseitems");
        skills_ = loadTable(game_.install(), "skills");
    }
    const std::optional<kotor::TwoDa>& feats = feats_;
    const std::optional<kotor::TwoDa>& spells = spells_;
    const auto tlk = [&](const std::string& strref) {
        return strref.empty() ? std::string{}
                              : game_.localization().tlk().text(static_cast<std::uint32_t>(std::strtoul(strref.c_str(), nullptr, 10)));
    };

    // The slots as retail's CSWGuiMainInterfaceAction::Initialize (0068b9d0) numbers them, with
    // their keymap.2da rows: 0 personal Force powers (row 32, key 4), 1 medical items (row 34,
    // key 5), 2 other usable items (row 38, key 6), 3 mines (row 36, key 7). 006197d0 sorts the
    // items with CSWCCreature::IsLeaderUsableItem 00616520 by baseitems.2da itemtype: 45, 47 and 26
    // medical, 28 mines, 25 the other slot. The target's three slots come from GetTargetActions
    // 00619c20: the left one Attack and the combat modes (00619950 / 00619b10), the middle one the
    // hostile powers (006191f0), the right one the grenades, itemtype 6 (006198e0 -> 006196a0).
    //
    // Powers (0064a870 over the codes the spells.2da loader 0059ba20 builds): FORCEFRIENDLY gives
    // the code 3000 + 10 * value + FORCEPRIORITY, FORCEHOSTILE 1000 + 10 * value + FORCEPRIORITY.
    // Slot 0 (00616230) takes category 3, the target's middle slot category 1. Per value only the
    // power with the highest priority stays (the first one found on a tie), ordered by value; a
    // power with neither column set is on no slot. Usable when the cost fits the Force points.
    // NOT YET: the middle slot's range test (squared distance against the spell's range) and the
    // droid / organic filter (006191f0 hands 0064a870 a mask tested against the spell's +0x188).
    // NOT YET: 00616520's other slot also takes items whose activation spell has ItemTargeting 1
    // and droid gear (itemtype 12, 13, 20) after 005a4950; here every other activatable item lands
    // there, grenades excepted.
    for (std::vector<Option>& list : targetOptions_) {
        list.clear();
    }
    // The target's left slot, 00619950 then 00619b10: three combat-mode feats first, then Attack.
    // The right-hand weapon (slot 0x10, INVENTORY_SLOT_RIGHTWEAPON) picks the set: ranged
    // (baseitems +0x1a) takes power blast 82/18/29, rapid shot 26/92/30 and sniper shot 77/20/31;
    // melee or bare hands critical strike 81/19/8, flurry 53/91/11 and power attack 83/17/28.
    // 00616140 keeps the first of each master/improved/basic triple the creature has.
    const CharacterSheet& sheet = game_.character();
    const auto addMode = [&](std::int32_t id) {
        Option mode;
        mode.kind = Option::Kind::Mode;
        mode.id = id;
        mode.name = tlk(cell(feats, static_cast<std::size_t>(id), "name"));
        if (mode.name.empty()) {
            mode.name = game_.rules().featName(id);
        }
        mode.icon = lowered(cell(feats, static_cast<std::size_t>(id), "icon"));
        targetOptions_[0].push_back(std::move(mode));
    };
    if (!game_.companionControlled()) {
        bool ranged = false;
        for (const InventoryItem& item : sheet.inventory) {
            if (item.equipped && item.slot == 4 && item.baseItem >= 0) {
                ranged = std::strtol(cell(baseItems_, static_cast<std::size_t>(item.baseItem), "rangedweapon").c_str(), nullptr, 10) != 0;
            }
        }
        static constexpr std::int32_t kRanged[3][3] = {{82, 18, 29}, {26, 92, 30}, {77, 20, 31}};
        static constexpr std::int32_t kMelee[3][3] = {{81, 19, 8}, {53, 91, 11}, {83, 17, 28}};
        for (const auto& triple : ranged ? kRanged : kMelee) {
            for (const std::int32_t id : triple) {
                if (sheet.feats.count(id) != 0) {
                    addMode(id);
                    break;
                }
            }
        }
    }
    Option attack;
    attack.kind = Option::Kind::Attack;
    attack.name = game_.localization().tlk().text(kStrrefAttack);
    attack.icon = "i_attack";
    targetOptions_[0].push_back(attack);
    if (!game_.companionControlled()) {
        struct Pick {
            int group = 0;
            int priority = 0;
            std::int32_t id = 0;
        };
        std::vector<Pick> friendly;
        std::vector<Pick> hostile;
        const auto consider = [](std::vector<Pick>& picks, int group, int priority, std::int32_t id) {
            for (Pick& pick : picks) {
                if (pick.group == group) {
                    if (pick.priority < priority) {
                        pick.priority = priority;
                        pick.id = id;
                    }
                    return;
                }
            }
            const auto at = std::find_if(picks.begin(), picks.end(), [&](const Pick& pick) { return pick.group > group; });
            picks.insert(at, Pick{group, priority, id});
        };
        const auto number = [&](std::int32_t id, const char* column, int& out) {
            const std::string text = cell(spells, static_cast<std::size_t>(id), column);
            if (text.empty() || text == "****") {
                return false;
            }
            out = std::atoi(text.c_str());
            return true;
        };
        for (const std::int32_t id : sheet.powers) {
            int priority = 0;
            number(id, "forcepriority", priority);
            int group = 0;
            if (number(id, "forcefriendly", group)) {
                consider(friendly, group, priority, id);
            }
            if (number(id, "forcehostile", group)) {
                consider(hostile, group, priority, id);
            }
        }
        const auto powerOption = [&](std::int32_t id) {
            Option power;
            power.kind = Option::Kind::Power;
            power.id = id;
            const std::optional<ForcePowerInfo> info = game_.rules().forcePower(id);
            power.name = tlk(cell(spells, static_cast<std::size_t>(id), "name"));
            if (power.name.empty() && info.has_value()) {
                power.name = info->label;
            }
            power.icon = lowered(cell(spells, static_cast<std::size_t>(id), "iconresref"));
            if (info.has_value()) {
                // retail 0059b8a0: the party pays the forceadjust.2da-scaled cost.
                const std::int32_t cost = game_.rules().adjustedForceCost(id, sheet.alignment, true);
                power.detail = std::to_string(cost);
                power.usable = game_.forcePoints() >= cost;
            }
            return power;
        };
        for (const Pick& pick : friendly) {
            options_[0].push_back(powerOption(pick.id));
        }
        for (const Pick& pick : hostile) {
            targetOptions_[1].push_back(powerOption(pick.id));
        }
        for (const InventoryItem& item : sheet.inventory) {
            if (item.equipped || item.object == 0 || !game_.itemUsable(item.object)) {
                continue;
            }
            Option use;
            use.kind = Option::Kind::Item;
            use.item = item.object;
            use.name = screens_ != nullptr ? screens_->itemDetails(item).name : item.name;
            use.icon = itemIcon(item);
            if (item.stack > 1) {
                use.detail = std::to_string(item.stack);
            }
            const std::string itemType = item.baseItem >= 0
                ? cell(baseItems_, static_cast<std::size_t>(item.baseItem), "itemtype") : std::string{};
            if (itemType == "6") {
                targetOptions_[2].push_back(std::move(use));
                continue;
            }
            std::size_t slot = 2;
            if (itemType == "45" || itemType == "47" || itemType == "26") {
                slot = 1;
            } else if (itemType == "28") {
                slot = 3;
            }
            options_[slot].push_back(std::move(use));
        }
    }
    // A door or a placeable as the target (CSWCDoor::GetTargetActions 00684410, CSWCPlaceable::GetTargetActions
    // 006837d0): the slots hold only Bash on the left (GUI string 0x170, icon i_attack) while it is locked,
    // can be hurt (not Plot: the server's flag at +0xf8 / the client's +0x104) and the area allows it, and the
    // Security skill's Unlock (string 0x149, icon the skill's own) in the middle while it is locked, the
    // controlled creature can use Security (GetCanUseSkill 6; trained only) and the lock can be picked (a door
    // without KeyRequired, +0x2d8; a container, HasInventory +0x324). NOT YET: the area's +0x2b0 flag.
    optionsTarget_ = game_.hudTarget();
    if (optionsTarget_ < game_.scene().entities.size()) {
        const kotor::Entity& target = game_.scene().entities[optionsTarget_];
        if (target.kind == kotor::EntityKind::Creature && !game_.hudTargetHostile()) {
            // 00619c20 case 3: a friendly creature offers nothing.
            for (std::vector<Option>& list : targetOptions_) {
                list.clear();
            }
        }
        if (target.kind == kotor::EntityKind::Door || target.kind == kotor::EntityKind::Placeable) {
            for (std::vector<Option>& list : targetOptions_) {
                list.clear();
            }
            if (target.locked && !target.plot && !target.isStatic) {
                Option bash;
                bash.kind = Option::Kind::Bash;
                bash.name = tlk(std::to_string(368));
                bash.icon = "i_attack";
                targetOptions_[0].push_back(std::move(bash));
            }
            const bool pickable = target.kind == kotor::EntityKind::Door ? !target.keyRequired : target.hasInventory;
            if (target.locked && pickable && game_.canUseSecurity()) {
                Option unlock;
                unlock.kind = Option::Kind::Unlock;
                unlock.name = tlk(std::to_string(329));
                unlock.icon = lowered(cell(skills_, 6, "icon"));
                targetOptions_[1].push_back(std::move(unlock));
            }
        }
    }
    for (std::size_t slot = 0; slot < targetOptions_.size(); ++slot) {
        const int size = static_cast<int>(targetOptions_[slot].size());
        targetIndex_[slot] = size > 0 ? std::clamp(targetIndex_[slot], 0, size - 1) : 0;
    }
    for (std::size_t slot = 0; slot < options_.size(); ++slot) {
        const int size = static_cast<int>(options_[slot].size());
        index_[slot] = size > 0 ? std::clamp(index_[slot], 0, size - 1) : 0;
    }
}

const Hud::Option* Hud::current(int slot) const
{
    if (slot < 0 || slot >= static_cast<int>(options_.size())) {
        return nullptr;
    }
    const std::vector<Option>& list = options_[static_cast<std::size_t>(slot)];
    if (list.empty()) {
        return nullptr;
    }
    return &list[static_cast<std::size_t>(std::clamp(index_[static_cast<std::size_t>(slot)], 0,
                                                       static_cast<int>(list.size()) - 1))];
}

void Hud::bindActionSlots()
{
    for (int slot = 0; slot < 4; ++slot) {
        const std::string number = std::to_string(slot);
        const Option* option = current(slot);
        const bool present = option != nullptr;
        const bool cycle = present && options_[static_cast<std::size_t>(slot)].size() > 1;
        // Retail keeps an empty slot's frame on screen (retail frame paired/hud_sword_retail.png: the
        // first and fourth slots are bordered dark squares); only the icon goes.
        gui_.setVisible("BTN_ACTION" + number, true);
        gui_.setVisible("LBL_ACTION" + number, present);
        // The empty slot's frame and fill are translucent (paired frame req_swoop_finish_045014: the first and
        // fourth slots about half as bright as the two that hold an action). Retail PopulateMenus 00689d80 calls
        // SetFrame 00689280 with the empty flag for a slot with no personal action, which writes the frame and
        // fill alphas (+0x8c, +0x100) from the float at 0073e9ac = 0.5; a slot that holds an action keeps 1.0.
        gui_.setAlpha("BTN_ACTION" + number, present ? 1.0f : 0.5f);
        if (!present) {
            gui_.setEnabled("BTN_ACTION" + number, true);
        }
        gui_.setVisible("BTN_ACTIONUP" + number, cycle);
        gui_.setVisible("BTN_ACTIONDOWN" + number, cycle);
        if (present) {
            gui_.setFill("LBL_ACTION" + number, option->icon);
            gui_.setEnabled("BTN_ACTION" + number, option->usable);
            gui_.setEnabled("LBL_ACTION" + number, option->usable);
        }
    }

    // The description line under the bar names the slot the pointer is over.
    std::string hovered;
    if (const kotor::GuiControl* control = gui_.control(gui_.hovered())) {
        hovered = control->tag;
    }
    int slot = -1;
    for (const char* prefix : {"BTN_ACTION", "LBL_ACTION", "BTN_ACTIONUP", "BTN_ACTIONDOWN"}) {
        // The longest prefixes first would be needed if the digit check did not reject them.
        slot = std::max(slot, slotOf(hovered, prefix));
    }
    // The pointer on a slot's arrow lights the slot too (paired frame req_hud_slot_down_hover: the
    // capsule turns yellow under the hovered DOWN arrow; 0068bc63..0068bced hook the arrows' enter and
    // leave events, 0x1f4 / 0x1f5, to the slot).
    for (int lit = 0; lit < 4; ++lit) {
        gui_.setMarked("BTN_ACTION" + std::to_string(lit), lit == slot);
    }
    const Option* option = slot >= 0 ? current(slot) : nullptr;
    gui_.setVisible("LBL_ACTIONDESC", option != nullptr);
    gui_.setVisible("LBL_ACTIONDESCBG", option != nullptr);
    if (option != nullptr) {
        gui_.setText("LBL_ACTIONDESC", option->detail.empty() ? option->name : option->name + " (" + option->detail + ")");
    }
}

const Hud::Option* Hud::currentTarget(int slot) const
{
    if (slot < 0 || slot >= static_cast<int>(targetOptions_.size())) {
        return nullptr;
    }
    const std::vector<Option>& list = targetOptions_[static_cast<std::size_t>(slot)];
    if (list.empty()) {
        return nullptr;
    }
    return &list[static_cast<std::size_t>(std::clamp(targetIndex_[static_cast<std::size_t>(slot)], 0,
                                                       static_cast<int>(list.size()) - 1))];
}

void Hud::bindTarget(double timeSeconds)
{
    // The block is on the combat target, else on the client's selected object (W46: the conversation
    // owner stays selected, 004cf490), a friend's block with the friend art (006859e0 SetFriend).
    targetShown_ = game_.hudTarget() != UINT32_MAX;
    const bool friendly = targetShown_ && !game_.hudTargetHostile();
    reticle_.visible = false;
    const std::array<const char*, 4> group = {"LBL_NAME", "LBL_NAMEBG", "PB_HEALTH", "LBL_HEALTHBG"};
    for (const char* tag : group) {
        gui_.setVisible(tag, targetShown_);
    }
    bool anyAction = false;
    for (int slot = 0; slot < 3; ++slot) {
        const std::string number = std::to_string(slot);
        // Every slot keeps its frame (paired/hud_sword_retail.png: three frames under the name).
        // A friend's block shows the name and the bar only (paired/walk/out_ig5/0031_hud_start_pair.png).
        const Option* option = targetShown_ ? currentTarget(slot) : nullptr;
        anyAction = anyAction || option != nullptr;
        // A hostile target keeps its three frames; a friendly one (a door, a container) only those that hold an action.
        gui_.setVisible("BTN_TARGET" + number, targetShown_ && (!friendly || option != nullptr));
        gui_.setVisible("LBL_TARGET" + number, option != nullptr);
        const bool arrows = option != nullptr && targetOptions_[static_cast<std::size_t>(slot)].size() > 1;
        gui_.setVisible("BTN_TARGETUP" + number, arrows);
        gui_.setVisible("BTN_TARGETDOWN" + number, arrows);
        if (option != nullptr) {
            gui_.setFill("LBL_TARGET" + number, option->icon);
            gui_.setEnabled("LBL_TARGET" + number, option->usable);
        }
        // 006859e0: lbl_miscroll_h on a hostile target's three slots, lbl_miscroll_f on a friend's.
        // The engine's combat target is always an enemy (attackNearestHostile, cycleCombatTarget).
        gui_.setFill("BTN_TARGET" + number, friendly ? "lbl_miscroll_f" : "lbl_miscroll_h");
    }
    if (!targetShown_) {
        return;
    }
    // 006859e0: ENEMY_BAR for a hostile target, POISON_BAR while it is poisoned, FRIEND_BAR for a
    // friend. NOT YET: the poisoned case.
    gui_.setFill("PB_HEALTH", friendly ? "friend_bar" : "enemy_bar");

    // CSWGuiMainInterface 00686090 places the block on the target each frame: its height reaches
    // the slots' bottom while any slot holds an action, else the health bar's; centred on the
    // target's projected point (the creature's vtable +0x13c) and 32 pixels above it; within 32
    // pixels of the top it sits at the top, within 32 of the bottom it sits 32 above the bottom;
    // a point behind the camera puts it on the side away from the point; then it is kept inside
    // its area. NOT TRACED: that area (+0x15bc..+0x15c8, not written in the code read); the
    // window is used. The anchor is the floating-text point (GameSession::floatingAnchor).
    const kotor::GuiControl* nameBg = gui_.control("LBL_NAMEBG");
    const kotor::GuiControl* health = gui_.control("PB_HEALTH");
    const kotor::GuiControl* slotFrame = gui_.control("BTN_TARGET0");
    const std::array<const char*, 16> tags = {"LBL_NAME", "LBL_NAMEBG", "PB_HEALTH", "LBL_HEALTHBG",
                                              "BTN_TARGET0", "LBL_TARGET0", "BTN_TARGETUP0", "BTN_TARGETDOWN0",
                                              "BTN_TARGET1", "LBL_TARGET1", "BTN_TARGETUP1", "BTN_TARGETDOWN1",
                                              "BTN_TARGET2", "LBL_TARGET2", "BTN_TARGETUP2", "BTN_TARGETDOWN2"};
    if (nameBg == nullptr || health == nullptr || slotFrame == nullptr || viewScale_ <= 0.0f || viewWidth_ <= 0.0f) {
        return;
    }
    const float left0 = static_cast<float>(nameBg->extent.left);
    const float top0 = static_cast<float>(nameBg->extent.top);
    const float width = static_cast<float>(nameBg->extent.width);
    const float bottom = anyAction ? static_cast<float>(slotFrame->extent.top + slotFrame->extent.height)
                                   : static_cast<float>(health->extent.top + health->extent.height);
    const float height = bottom - top0;
    // Worked in the HUD's design units (window pixels over the HUD scale).
    const float areaW = viewWidth_ / viewScale_;
    const float areaH = viewHeight_ / viewScale_;
    float x = blockPlaced_ ? blockX_ : 0.5f * (areaW - width);
    float y = blockPlaced_ ? blockY_ : 0.0f;
    // 00686090 leaves the block where it is while the pointer is over it, and for 0.5 s after the
    // pointer leaves (the hit test at 00686000..0068607e sets +0x1aec bit 2 and, on leaving, +0x1ae0
    // = 0.5; 00684ea2 counts it down), so its arrows and slots can be clicked.
    const float holdDt = blockTime_ < 0.0 ? 0.0f : static_cast<float>(std::max(0.0, timeSeconds - blockTime_));
    blockTime_ = timeSeconds;
    bindReticle(holdDt);
    std::string hoveredTag;
    if (const kotor::GuiControl* hoveredControl = gui_.control(gui_.hovered())) {
        hoveredTag = hoveredControl->tag;
    }
    const bool overBlock = std::find(tags.begin(), tags.end(), hoveredTag) != tags.end();
    if (pointerOnBlock_ && !overBlock) {
        blockHold_ = 0.5f;
    } else if (blockHold_ > 0.0f) {
        blockHold_ = std::max(0.0f, blockHold_ - holdDt);
    }
    pointerOnBlock_ = overBlock;
    const bool hold = blockPlaced_ && (overBlock || blockHold_ > 0.0f);
    kotor::Vec3 at;
    const std::uint32_t object = game_.objectForEntityIndex(game_.hudTarget());
    if (!hold && game_.floatingAnchor(object, at)) {
        // 0060ff30 (the creature's vtable +0x13c point): the creature's position plus 1.75 (007415c4), or plus
        // 2.0 (00741be0) when its server object's string at +0x18 is "zaalbar" or "hk47" (0060ffe3..0060fffa).
        // floatingAnchor stands 2.0 over the position. NOT TRACED: which field +0x18 is; the tag is used here.
        std::string tag;
        if (const std::uint32_t shown = game_.hudTarget(); shown < game_.scene().entities.size()) {
            tag = game_.scene().entities[shown].tag;
        }
        std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        at.z += tag == "zaalbar" || tag == "hk47" ? 0.0f : -0.25f;
        const SceneCamera camera = game_.camera();
        const auto dot = [](const kotor::Vec3& a, const kotor::Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
        const auto unit = [&](kotor::Vec3 v) {
            const float size = std::sqrt(dot(v, v));
            return size > 0.0f ? kotor::Vec3{v.x / size, v.y / size, v.z / size} : v;
        };
        const kotor::Vec3 forward = unit(camera.forward);
        const kotor::Vec3 right = unit({forward.y * camera.up.z - forward.z * camera.up.y,
                                        forward.z * camera.up.x - forward.x * camera.up.z,
                                        forward.x * camera.up.y - forward.y * camera.up.x});
        const kotor::Vec3 up{right.y * forward.z - right.z * forward.y, right.z * forward.x - right.x * forward.z,
                             right.x * forward.y - right.y * forward.x};
        const float focal = (areaH * 0.5f) / std::tan(camera.fovYDegrees * 0.5f * 3.14159265f / 180.0f);
        const kotor::Vec3 d{at.x - camera.eye.x, at.y - camera.eye.y, at.z - camera.eye.z};
        const float depth = dot(d, forward);
        const bool behind = depth <= camera.nearClip;
        const float safeDepth = behind ? std::max(std::fabs(depth), camera.nearClip) : depth;
        const float px = areaW * 0.5f + dot(d, right) / safeDepth * focal;
        const float py = areaH * 0.5f - dot(d, up) / safeDepth * focal;
        if (behind) {
            x = px >= areaW * 0.5f ? 0.0f : areaW - width;
        } else {
            x = px - std::floor(width / 2.0f);
        }
        if (py < 32.0f) {
            y = 0.0f;
        } else if (py > areaH - 32.0f) {
            y = areaH - height - 32.0f;
        } else {
            y = std::floor(py) - height - 32.0f;
        }
    }
    x = std::clamp(x, 0.0f, std::max(0.0f, areaW - width));
    y = std::clamp(y, 0.0f, std::max(0.0f, areaH - height));
    blockX_ = x;
    blockY_ = y;
    blockPlaced_ = true;
    // The HUD view moves each control with its nearest window edge (guiAnchorShift); the offset
    // takes that move back out so the block lands where it was placed.
    GuiView view;
    view.scale = viewScale_;
    view.baseWidth = gui_.screen() != nullptr ? static_cast<float>(gui_.screen()->extent.width) : 800.0f;
    view.baseHeight = gui_.screen() != nullptr ? static_cast<float>(gui_.screen()->extent.height) : 600.0f;
    view.windowWidth = viewWidth_;
    view.windowHeight = viewHeight_;
    view.anchored = viewAnchored_;
    for (const char* tag : tags) {
        const kotor::GuiControl* control = gui_.control(tag);
        if (control == nullptr) {
            continue;
        }
        float shiftX = 0.0f;
        float shiftY = 0.0f;
        guiAnchorShift(view, *control, shiftX, shiftY);
        gui_.setOffset(tag, x - left0 - shiftX, y - top0 - shiftY);
    }
    gui_.setText("LBL_NAME", game_.hudTargetName());
    gui_.setValue(health->key, health->maxValue * game_.hudTargetHealthRatio());
}

void Hud::bindReticle(float dt)
{
    // CSWGuiMainInterface 0068a310 for a creature target (case 5). The art: friendlyreticle2 for a
    // friend; for a hostile, combatreticle while the client's +0x320 flag (005ede70) is set, else
    // hostilereticle2. NOT TRACED: what sets +0x320; the controlled creature's action queue stands in
    // for it here (the same test that shows the Disengage bar). The engine's combat target is always
    // an enemy (Hud::bindTarget), so the friendly art is not reached.
    const std::uint32_t target = game_.hudTarget();
    const bool friendly = target != UINT32_MAX && !game_.hudTargetHostile();
    if (target != reticleTarget_) {
        reticleTarget_ = target;
        reticlePulse_ = -1.0f; // NOT TRACED: where retail puts +0x5cb0 back to -1
    }
    const std::uint32_t object = game_.objectForEntityIndex(target);
    kotor::Vec3 at;
    if (target == UINT32_MAX || !game_.floatingAnchor(object, at)) {
        return;
    }
    // The edge: 64 inside 5 m of the controlled creature, 16 beyond 30 m, and between them
    // 64 - round((distance - 5) x 0.04 x 48) (constants at 00745e5c, 007455bc, 00753d88..98).
    const kotor::Vec3 from = game_.controlledPosition();
    const kotor::Vec3 feet{at.x, at.y, at.z - 2.0f}; // floatingAnchor stands 2 m over the position
    const float dx = feet.x - from.x;
    const float dy = feet.y - from.y;
    const float dz = feet.z - from.z;
    const float squared = dx * dx + dy * dy + dz * dz;
    float size = 64.0f;
    if (squared >= 900.0f) {
        size = 16.0f;
    } else if (squared > 25.0f) {
        size = 64.0f - std::round((std::sqrt(squared) - 5.0f) * 0.04f * 48.0f);
    }
    std::string art = friendly ? "friendlyreticle2" : "hostilereticle2";
    if (!friendly && game_.controlledQueueSize() > 0) {
        art = "combatreticle";
        // +0x5cb0: the first frame adds 64 and starts 0.5 s, then the edge carries an extra
        // round((1 - (0.5 - left) x 2) x 64) until the time runs out (0.5 at 0073e9ac, 64 at 007455cc).
        if (reticlePulse_ <= 0.0f) {
            if (reticlePulse_ == -1.0f) {
                size += 64.0f;
                reticlePulse_ = 0.5f;
            }
        } else {
            reticlePulse_ -= dt;
            if (reticlePulse_ > 0.0f) {
                size += std::round((1.0f - (0.5f - reticlePulse_) * 2.0f) * 64.0f);
            } else {
                reticlePulse_ = 0.0f;
            }
        }
    }
    // The point: the creature's vtable +0x13c projection, the same one the target block is placed on.
    const float areaW = viewWidth_ / viewScale_;
    const float areaH = viewHeight_ / viewScale_;
    const SceneCamera camera = game_.camera();
    const auto dot = [](const kotor::Vec3& a, const kotor::Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const auto unit = [&](kotor::Vec3 v) {
        const float length = std::sqrt(dot(v, v));
        return length > 0.0f ? kotor::Vec3{v.x / length, v.y / length, v.z / length} : v;
    };
    const kotor::Vec3 forward = unit(camera.forward);
    const kotor::Vec3 right = unit({forward.y * camera.up.z - forward.z * camera.up.y, forward.z * camera.up.x - forward.x * camera.up.z,
                                    forward.x * camera.up.y - forward.y * camera.up.x});
    const kotor::Vec3 up{right.y * forward.z - right.z * forward.y, right.z * forward.x - right.x * forward.z,
                         right.x * forward.y - right.y * forward.x};
    const float focal = (areaH * 0.5f) / std::tan(camera.fovYDegrees * 0.5f * 3.14159265f / 180.0f);
    const kotor::Vec3 d{at.x - camera.eye.x, at.y - camera.eye.y, at.z - camera.eye.z};
    const float depth = dot(d, forward);
    const bool behind = depth <= camera.nearClip;
    const float safeDepth = behind ? std::max(std::fabs(depth), camera.nearClip) : depth;
    const float px = areaW * 0.5f + dot(d, right) / safeDepth * focal;
    const float py = areaH * 0.5f - dot(d, up) / safeDepth * focal;

    // Placement (0068a6f0..0068aa61) in the view rectangle (+0xbf70, here the whole window). On screen,
    // 32 or more inside every edge: the art centred on the point. Otherwise the arrow (hostilearrow /
    // friendlyarrow), 32 square, on the edge the point is past, turned 0 (left), 180 (right), 90
    // (bottom) or 270 (top), its free coordinate round(point - 16) or the corner when past two edges.
    // Behind the camera the arrow goes to the side away from the point and the height is mirrored.
    const std::string arrow = friendly ? "friendlyarrow" : "hostilearrow";
    const float left = 0.0f;
    const float top = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float angle = 0.0f;
    const auto alongY = [&](float value) {
        if (value < top + 32.0f) {
            return top;
        }
        if (value > top + areaH - 32.0f) {
            return top + areaH - 32.0f;
        }
        return std::round(value - 16.0f);
    };
    if (behind) {
        art = arrow;
        size = 32.0f;
        if (std::floor(viewWidth_ / 2.0f) / viewScale_ <= px) {
            x = left;
        } else {
            angle = 180.0f;
            x = left + areaW - 32.0f;
        }
        if (py < top + 32.0f) {
            y = top + areaH - 32.0f;
        } else if (py > top + areaH - 32.0f) {
            y = top;
        } else if (std::floor(viewHeight_ / 2.0f) / viewScale_ <= py) {
            y = std::round(py - 16.0f);
        } else {
            y = areaH - std::round(py + 16.0f);
        }
    } else if (px < left + 32.0f) {
        art = arrow;
        size = 32.0f;
        x = left;
        y = alongY(py);
    } else if (px > left + areaW - 32.0f) {
        art = arrow;
        size = 32.0f;
        angle = 180.0f;
        x = left + areaW - 32.0f;
        y = alongY(py);
    } else if (py < top + 32.0f) {
        art = arrow;
        size = 32.0f;
        angle = 270.0f;
        x = std::round(px - 16.0f);
        y = top;
    } else if (py > top + areaH - 32.0f) {
        art = arrow;
        size = 32.0f;
        angle = 90.0f;
        x = std::round(px - 16.0f);
        y = top + areaH - 32.0f;
    } else {
        const float half = static_cast<float>(static_cast<int>(size) / 2);
        x = std::round(px - half);
        y = std::round(py - half);
    }
    reticle_.visible = true;
    reticle_.art = art;
    reticle_.x = x;
    reticle_.y = y;
    reticle_.size = size;
    reticle_.angleDegrees = angle;
}

float Hud::mapArrowAngle() const
{
    return game_.yaw();
}

void Hud::update(double timeSeconds, bool userPaused)
{
    if (gui_.screen() == nullptr || gui_.screenName().rfind("mipc2", 0) != 0) {
        return;
    }
    hideEventControls();
    portraits_.clear();
    // The event icons: 00686920 binds them in this order, 00687fe0 raises one for four seconds.
    static constexpr const char* kIcons[9] = {"LBL_JOURNAL", "LBL_CASH", "LBL_PLOTXP", "LBL_STEALTHXP", "LBL_DARKSHIFT",
                                              "LBL_LIGHTSHIFT", nullptr, "LBL_ITEMRCVD", "LBL_ITEMLOST"};
    for (std::size_t i = 0; i < 9; ++i) {
        if (kIcons[i] != nullptr && game_.feedbackIconSeconds(i) > 0.0f) {
            gui_.setVisible(kIcons[i], true);
        }
    }
    // Feedback options, retail bits: 0x8 the minimap (00687e40), 0x200 the quick menu buttons (00686ba0).
    const std::uint32_t feedback = game_.feedbackOptions();
    for (const char* tag : {"LBL_MAP", "LBL_MAPVIEW", "LBL_ARROW", "LBL_MAPBORDER", "BTN_MINIMAP"}) {
        gui_.setVisible(tag, (feedback & 0x8u) != 0u);
    }
    for (const char* tag : {"BTN_EQU", "BTN_INV", "BTN_CHAR", "BTN_ABI", "BTN_MSG", "BTN_JOU", "BTN_MAP", "BTN_OPT", "LBL_MENUBG"}) {
        gui_.setVisible(tag, (feedback & 0x200u) == 0u);
    }

    // Leader first; the screen has three party slots.
    const std::vector<std::uint32_t> objects = game_.partyObjects();
    members_ = static_cast<int>(std::min<std::size_t>(objects.size(), 3));
    for (int slot = 0; slot < 3; ++slot) {
        const std::string number = std::to_string(slot + 1);
        const int member = memberInPortraitSlot(slot, members_);
        const bool present = member >= 0;
        gui_.setVisible("LBL_CHAR" + number, present);
        gui_.setVisible("BTN_CHAR" + number, present);
        gui_.setVisible("PB_VIT" + number, present);
        gui_.setVisible("PB_FORCE" + number, present);
        gui_.setVisible("LBL_BACK" + number, present);
        if (!present) {
            continue;
        }
        const int npc = game_.npcForObject(objects[static_cast<std::size_t>(member)]);
        const CharacterSheet* sheet = nullptr;
        if (npc == kNpcPlayer) {
            sheet = &game_.character();
        } else if (npc >= 0 && npc < kNpcCount) {
            sheet = &game_.partyTable().npcs[static_cast<std::size_t>(npc)].sheet;
        }
        if (sheet == nullptr) {
            continue;
        }
        // The level-up badge sits on the portrait of a creature with points waiting. Only the
        // player character is tracked for that (the companions' points are not).
        const bool levelUp = npc == kNpcPlayer && game_.canLevelUp();
        gui_.setVisible("LBL_LEVELUP" + number, levelUp);
        gui_.setVisible("LBL_LVLUPBG" + number, levelUp);
        std::int32_t hp = sheet->hp;
        std::int32_t maxHp = sheet->maxHp;
        if (npc == game_.partyTable().controlled) {
            hp = game_.controlledHp();
            maxHp = game_.controlledMaxHp();
        }
        const kotor::GuiControl* vit = gui_.control("PB_VIT" + number);
        const kotor::GuiControl* force = gui_.control("PB_FORCE" + number);
        if (vit != nullptr) {
            gui_.setValue(vit->key, vit->maxValue * static_cast<float>(std::clamp(hp, 0, std::max(1, maxHp)))
                                        / static_cast<float>(std::max(1, maxHp)));
        }
        if (force != nullptr) {
            gui_.setValue(force->key, sheet->maxFp > 0
                                          ? force->maxValue * static_cast<float>(std::clamp(sheet->fp, 0, sheet->maxFp))
                                              / static_cast<float>(sheet->maxFp)
                                          : 0.0f);
        }
        std::int32_t portraitRow = sheet->portraitRow;
        if (portraitRow < 0 && npc == kNpcPlayer) {
            // A PC with no portraits.2da row (a module started without chargen or a save that names
            // one) shows the PC row whose appearance its body uses, not the file's own LBL_CHAR1 art.
            // ENGINE CHOICE: retail always has the row from chargen or the save.
            portraitRow = game_.rules().portraitRowForAppearance(static_cast<std::int32_t>(game_.playerAppearance()));
        }
        if (portraitRow >= 0) {
            const PortraitInfo info = game_.rules().portraitRow(portraitRow);
            portraits_["LBL_CHAR" + number] = game_.rules().portraitResrefFor(info, sheet->alignment);
        }
    }

    // The toggles: pause, solo mode and stealth (stealth only where the class has it).
    gui_.setSelected("TB_PAUSE", userPaused);
    // 00686ba0 sets the solo toggle's visibility from the party table's member count (> 0; PT_NUM_MEMBERS,
    // the PC not counted): with nobody in the party it is hidden (paired/walk/out_ig5/0031_hud_start_pair.png).
    gui_.setVisible("TB_SOLO", !game_.partyTable().members.empty());
    gui_.setSelected("TB_SOLO", game_.soloMode());
    gui_.setVisible("TB_STEALTH", game_.canStealth());
    gui_.setSelected("TB_STEALTH", game_.stealthActive());

    if (timeSeconds - lastRebuild_ > 0.25 || game_.hudTarget() != optionsTarget_) {
        lastRebuild_ = timeSeconds;
        rebuildOptions();
    }
    bindActionSlots();
    bindTarget(timeSeconds);

    // The action queue: ENGINE CHOICE, the first cell shows the attack while the controlled
    // creature has actions queued; the other three cells stay empty (the queue does not name
    // its entries to the HUD) and the Clear buttons appear with it.
    const std::size_t queued = game_.controlledQueueSize();
    for (int cell = 0; cell < 4; ++cell) {
        gui_.setVisible("LBL_QUEUE" + std::to_string(cell), cell == 0 && queued > 0);
    }
    gui_.setFill("LBL_QUEUE0", "i_attack");
    // ShowActionQueue 006874c0 toggles all three backgrounds together with the queue cells and
    // the three Clear buttons: LBL_COMBATBG%d (bound at 0028c701, +0x675c/+0x689c/+0x69dc),
    // BTN_CLEARONE +0x6cd0, BTN_CLEARONE2 +0x6e94, BTN_CLEARALL +0x7058, LBL_QUEUE%d +0x625c.
    // BG1 is the red ring round the first cell, BG2/BG3 the dark fill and blue outline of the
    // queue bar and the Disengage box.
    gui_.setVisible("BTN_CLEARALL", queued > 0);
    gui_.setVisible("BTN_CLEARONE", queued > 0);
    gui_.setVisible("BTN_CLEARONE2", queued > 0);
    for (const char* bg : {"LBL_COMBATBG1", "LBL_COMBATBG2", "LBL_COMBATBG3"}) {
        gui_.setVisible(bg, queued > 0);
    }

    // The combat line. Retail binds LBL_CMBTMODEMSG (+0x735c) and LBL_CMBTMSGBG (+0x749c) in
    // 0068b9d0 and shows both with the action queue (ShowActionQueue 006874c0). The line holds the
    // controlled member's combat message (CSWPartyMemberData +0x84, SetCombatMessage 006345e0),
    // 48208 "COMBAT MODE engaged..." by default, drawn by 00687700 in the colour that message
    // picks (0x7a23d8 for 48208). While T is held (HideActionDescription 006856e0) the message is
    // hidden and a code-built label at +0x75dc showing 48458 takes its place over the same
    // background; release (00685710) puts the message back.
    // 00687700 ignores any message while a transient one runs. 48208 (red), 42476 (green), 42477
    // (blue) and 47915 (orange) stay; 47859 (magenta), 42478 (green) and any other strref (blue)
    // are transient: 2.5 s (0x7720 / 0x7724), drawn at alpha min(1, 2 * left / 2.5) by Update
    // 00686ba0, which then puts 48208 back. UpdatePortraits 00687860 raises 47915 while the
    // controlled creature is debilitated and puts 48208 back once it is not. Raised by the game:
    // GameSession::raiseCombatMessage (the CSWPartyMemberData setter 006345e0's callers: 48208 from
    // 0050f170 / 00514af0 / 005bbbf0, 42476 from 006227e0, 42477 from 004f9xxx / 005bc9xx).
    // NOT TRACED: 005edef0 (client flag +900 bit 0) that drops 42476, 42477 and 47859; taken as clear.
    constexpr std::uint32_t kCombatEngaged = 48208;
    constexpr std::uint32_t kDebilitated = 47915;
    const float dt = lastUpdate_ < 0.0 ? 0.0f : static_cast<float>(std::max(0.0, timeSeconds - lastUpdate_));
    lastUpdate_ = timeSeconds;
    float alpha = 1.0f;
    if (combatTransient_ >= 0.0f && !userPaused) {
        combatTransient_ -= dt;
        if (combatTransient_ >= 0.0f) {
            alpha = std::min(1.0f, 2.0f * combatTransient_ / 2.5f);
        } else {
            combatTransient_ = -1.0f;
            combatStrref_ = kCombatEngaged;
        }
    }
    std::uint32_t raised = game_.takeCombatMessage();
    if (raised == 0u) {
        if (game_.controlledDebilitated()) {
            raised = kDebilitated;
        } else if (combatStrref_ == kDebilitated && combatTransient_ < 0.0f) {
            raised = kCombatEngaged;
        }
    }
    if (raised != 0u && combatTransient_ < 0.0f && raised != combatStrref_) {
        combatStrref_ = raised;
        alpha = 1.0f;
        if (raised == 47859u || raised == 42478u
            || (raised != kCombatEngaged && raised != 42476u && raised != 42477u && raised != kDebilitated)) {
            combatTransient_ = 2.5f;
        }
    }
    kotor::Vec3 color{0.0f, 0.66f, 0.98f}; // 0x7a23b4
    switch (combatStrref_) {
    case kCombatEngaged: color = {0.74f, 0.11f, 0.0f}; break;  // 0x7a23d8
    case 42476u: case 42478u: color = {0.28f, 0.92f, 0.11f}; break; // 0x7a23e4
    case 47859u: color = {0.95f, 0.0f, 0.85f}; break;            // 0x7a23fc
    case kDebilitated: color = {0.98f, 0.45f, 0.0f}; break;      // 0x7a23f0
    default: break;
    }
    // 006856e0 sets the background and the 48458 label visible whether or not the queue shows.
    const bool lineShown = queued > 0 || instantToolTips_;
    gui_.setVisible("LBL_CMBTMSGBG", lineShown);
    gui_.setVisible("LBL_CMBTMODEMSG", lineShown);
    if (lineShown) {
        constexpr std::uint32_t kHoverForHotKey = 48458;
        gui_.setText("LBL_CMBTMODEMSG", game_.localization().tlk().text(instantToolTips_ ? kHoverForHotKey : combatStrref_));
        // NOT TRACED: the 48458 label's own colour; it is drawn in the message's.
        gui_.setTextColor("LBL_CMBTMODEMSG", color);
        gui_.setAlpha("LBL_CMBTMODEMSG", alpha);
    }
}

std::string Hud::tooltip() const
{
    if ((game_.feedbackOptions() & 0x400u) == 0u) {
        return {};
    }
    const kotor::GuiControl* control = gui_.control(gui_.hovered());
    // A hidden control (the target block's arrows with no target) has no tip; the HUD's picker
    // reports the control under the pointer whether or not it is drawn.
    if (control == nullptr || !gui_.state(control->key).visible) {
        return {};
    }
    // Strrefs: 00627980 and 0068c100 for the bar, 0068b9d0 for the slots with their keymap.2da row (+0x30), whose
    // key name 00418a90 appends after " : ".
    struct Tip {
        const char* tag;
        std::uint32_t strref;
        int keyRow;
    };
    static constexpr Tip kTips[] = {
        {"BTN_EQU", 48219, -1}, {"BTN_INV", 48220, -1}, {"BTN_CHAR", 48225, -1}, {"BTN_MAP", 48221, -1},
        {"BTN_ABI", 48224, -1}, {"BTN_JOU", 48218, -1}, {"BTN_OPT", 48222, -1}, {"BTN_MSG", 48223, -1},
        {"BTN_MINIMAP", 48221, -1}, {"BTN_CLEARONE", 48456, -1}, {"BTN_CLEARONE2", 48456, -1},
        {"BTN_CLEARALL", 48518, -1}, {"BTN_ACTION0", 48486, 232}, {"BTN_ACTION1", 48291, 234},
        {"BTN_ACTION2", 48299, 238}, {"BTN_ACTION3", 48295, 236}, {"BTN_TARGET0", 48303, 226},
        {"BTN_TARGET1", 48307, 228}, {"BTN_TARGET2", 48311, 230},
        // 0068bd14 / 0068bd24: every slot's arrows, "Action Menu Up" / "Action Menu Down". They also
        // set keymap rows 248 / 249 (+0x30), but retail shows no key (paired frame
        // req_hud_slot_down_hover: "Action Menu Down" alone; ours read " : 2" from row 249).
        // NOT TRACED: why 00418a90 gives no name for these rows.
        {"BTN_ACTIONUP0", 48465, -1}, {"BTN_ACTIONUP1", 48465, -1}, {"BTN_ACTIONUP2", 48465, -1},
        {"BTN_ACTIONUP3", 48465, -1}, {"BTN_ACTIONDOWN0", 48466, -1}, {"BTN_ACTIONDOWN1", 48466, -1},
        {"BTN_ACTIONDOWN2", 48466, -1}, {"BTN_ACTIONDOWN3", 48466, -1}, {"BTN_TARGETUP0", 48465, -1},
        {"BTN_TARGETUP1", 48465, -1}, {"BTN_TARGETUP2", 48465, -1}, {"BTN_TARGETDOWN0", 48466, -1},
        {"BTN_TARGETDOWN1", 48466, -1}, {"BTN_TARGETDOWN2", 48466, -1},
    };
    std::string hoveredTag = control->tag;
    if (hoveredTag.rfind("LBL_ACTION", 0) == 0 || hoveredTag.rfind("LBL_TARGET", 0) == 0) {
        hoveredTag.replace(0, 3, "BTN"); // the slot's art lies over its button
    }
    for (const Tip& tip : kTips) {
        if (hoveredTag != tip.tag) {
            continue;
        }
        std::string text = game_.localization().tlk().text(tip.strref);
        if (tip.keyRow >= 0) {
            if (auto table = game_.install().table("keymap")) {
                const std::string key = table->cell("action" + std::to_string(tip.keyRow), "character");
                if (!key.empty() && key != "****") {
                    text += " : " + key;
                }
            }
        }
        return text;
    }
    return {};
}

Hud::Request Hud::activate(const std::string& tag)
{
    if (tag == "BTN_EQU") {
        game_.toggleUiPanel(UiPanel::Equipment);
    } else if (tag == "BTN_INV") {
        game_.toggleUiPanel(UiPanel::Inventory);
    } else if (tag == "BTN_CHAR") {
        game_.toggleUiPanel(UiPanel::Character);
    } else if (tag == "BTN_ABI") {
        game_.toggleUiPanel(UiPanel::Abilities);
    } else if (tag == "BTN_MSG") {
        game_.toggleUiPanel(UiPanel::Messages);
    } else if (tag == "BTN_JOU") {
        game_.toggleUiPanel(UiPanel::Journal);
    } else if (tag == "BTN_MAP" || tag == "BTN_MINIMAP") {
        return Request::Map;
    } else if (tag == "BTN_OPT") {
        return Request::Pause;
    } else if (tag == "TB_PAUSE") {
        return Request::TogglePause;
    } else if (tag == "TB_SOLO") {
        game_.setSoloMode(!game_.soloMode());
    } else if (tag == "TB_STEALTH") {
        if (game_.stealthActive()) {
            game_.leaveStealth();
        } else {
            game_.enterStealth();
        }
    } else if (tag == "BTN_CLEARALL" || tag == "BTN_CLEARONE" || tag == "BTN_CLEARONE2") {
        game_.cancelCombat();
    } else if (const int tup = slotOf(tag, "BTN_TARGETUP"); tup >= 0 && tup < 3) {
        const int size = static_cast<int>(targetOptions_[static_cast<std::size_t>(tup)].size());
        if (size > 0) {
            int& index = targetIndex_[static_cast<std::size_t>(tup)];
            index = (index + size - 1) % size;
        }
    } else if (const int tdown = slotOf(tag, "BTN_TARGETDOWN"); tdown >= 0 && tdown < 3) {
        const int size = static_cast<int>(targetOptions_[static_cast<std::size_t>(tdown)].size());
        if (size > 0) {
            int& index = targetIndex_[static_cast<std::size_t>(tdown)];
            index = (index + 1) % size;
        }
    } else if (const int slot = slotOf(tag, "BTN_CHAR") ; slot >= 1 && slot <= 3) {
        const std::vector<std::uint32_t> objects = game_.partyObjects();
        const int member = memberInPortraitSlot(slot - 1, static_cast<int>(std::min<std::size_t>(objects.size(), 3)));
        if (member >= 0) {
            game_.selectLeaderNpc(game_.npcForObject(objects[static_cast<std::size_t>(member)]));
        }
    } else if (const int up = slotOf(tag, "BTN_ACTIONUP"); up >= 0 && up < 4) {
        const int size = static_cast<int>(options_[static_cast<std::size_t>(up)].size());
        if (size > 0) {
            index_[static_cast<std::size_t>(up)] = (index_[static_cast<std::size_t>(up)] + size - 1) % size;
        }
    } else if (const int down = slotOf(tag, "BTN_ACTIONDOWN"); down >= 0 && down < 4) {
        const int size = static_cast<int>(options_[static_cast<std::size_t>(down)].size());
        if (size > 0) {
            index_[static_cast<std::size_t>(down)] = (index_[static_cast<std::size_t>(down)] + 1) % size;
        }
    } else if (const int action = std::max(slotOf(tag, "BTN_ACTION"), slotOf(tag, "LBL_ACTION")); action >= 0 && action < 4) {
        const Option* option = current(action);
        if (option == nullptr || !option->usable) {
            return Request::None;
        }
        switch (option->kind) {
        case Option::Kind::Attack: game_.attackNearestHostile(); break;
        case Option::Kind::Mode: game_.useFeat(option->id); break;
        case Option::Kind::Power: {
            const std::optional<ForcePowerInfo> info = game_.rules().forcePower(option->id);
            std::uint32_t target = 0;
            if (info.has_value() && info->range > 0 && game_.combatTarget() != UINT32_MAX) {
                target = game_.objectForEntityIndex(game_.combatTarget());
            }
            game_.useForcePower(option->id, target);
            break;
        }
        case Option::Kind::Item: game_.useItem(option->item, 0); break;
        case Option::Kind::Bash:
        case Option::Kind::Unlock: break;
        }
        lastRebuild_ = -1.0e9; // the use changed charges, stacks and Force points
    } else if (const int target = std::max(slotOf(tag, "BTN_TARGET"), slotOf(tag, "LBL_TARGET")); target >= 0 && target < 3) {
        // The target's slots act on the target (00619c20's lists); key 1 with no target attacks
        // the nearest enemy.
        const Option* option = currentTarget(target);
        if (option != nullptr && (option->kind == Option::Kind::Bash || option->kind == Option::Kind::Unlock)) {
            // A door or a container as the target (00684410 / 006837d0 page 0 and 1).
            const std::uint32_t shown = game_.hudTarget();
            if (option->kind == Option::Kind::Bash) {
                game_.bashDoor(shown);
            } else {
                game_.unlockDoor(shown);
            }
            lastRebuild_ = -1.0e9;
            return Request::None;
        }
        const std::uint32_t on = game_.combatTarget() != UINT32_MAX ? game_.objectForEntityIndex(game_.combatTarget())
                                                                     : script::kObjectInvalid;
        if (option == nullptr || option->kind == Option::Kind::Attack) {
            if (target == 0) {
                game_.attackNearestHostile();
            }
        } else if (!option->usable || on == script::kObjectInvalid) {
            return Request::None;
        } else if (option->kind == Option::Kind::Mode) {
            game_.useFeat(option->id);
        } else if (option->kind == Option::Kind::Power) {
            game_.useForcePower(option->id, on);
            lastRebuild_ = -1.0e9;
        } else if (option->kind == Option::Kind::Item) {
            game_.useItem(option->item, 0, on);
            lastRebuild_ = -1.0e9;
        }
    }
    return Request::None;
}

std::string Hud::portraitFor(const std::string& tag) const
{
    const auto found = portraits_.find(tag);
    return found != portraits_.end() ? found->second : std::string{};
}

void Hud::updateBark(double timeSeconds)
{
    const float dt = barkClock_ < 0.0 ? 0.0f : static_cast<float>(std::max(0.0, timeSeconds - barkClock_));
    barkClock_ = timeSeconds;
    // A new bark (BarkString 00548900 -> 0062ab30 -> 006a9920; a speaker's own bark goes through its
    // client object, 005f2c90 -> 0060f560 -> 0062ab30): the line from dialog.tlk, its time
    // 1 + 0.11 s per character (00755ccc, 0073d764), and the entry's sound through priority group 26
    // at volume 127 (0074c5e8).
    const PresentationState& shown = game_.presentation();
    if (shown.barkSerial != barkSerial_) {
        barkSerial_ = shown.barkSerial;
        if (shown.lastBarkStrref >= 0) {
            const auto strref = static_cast<std::uint32_t>(shown.lastBarkStrref);
            bark_.text = game_.localization().tlk().text(strref);
            barkSpeaker_ = shown.lastBarkSpeaker;
            barkLeft_ = static_cast<float>(bark_.text.size()) * 0.11f + 1.0f;
            barkSound_ = 0.0f;
            barkSoundPlayed_ = false;
            const std::string sound = game_.localization().tlk().sound(strref);
            if (!sound.empty()) {
                AudioSystem::GroupSound request;
                request.group = AudioSystem::kGroupBark;
                request.stream = true;
                if (game_.audio().playGroupSound(sound, request) >= 0) {
                    barkSoundPlayed_ = true;
                    audio::SoundData data;
                    std::string error;
                    if (game_.audio().decodeProbe(sound, data, error)) {
                        barkSound_ = static_cast<float>(data.durationSeconds());
                    }
                }
            }
            barkActive_ = true;
        }
    }
    bark_.visible = false;
    if (!barkActive_) {
        return;
    }
    // 006a9ce0: a bark with no speaker always shows; a speaker's shows while it is within 6 m of the
    // player (distance squared under 36, 0074f348).
    bool show = true;
    if (barkSpeaker_ != script::kObjectInvalid) {
        kotor::Vec3 at;
        show = false;
        if (game_.floatingAnchor(barkSpeaker_, at)) {
            const kotor::Vec3 from = game_.controlledPosition();
            const float dx = at.x - from.x;
            const float dy = at.y - from.y;
            const float dz = at.z - 2.0f - from.z; // floatingAnchor stands 2 m over the position
            show = dx * dx + dy * dy + dz * dz < 36.0f;
        }
    }
    // While the sound plays the box stays (hidden, not dropped, when the speaker is too far); after
    // it, the box stays for what is left of its time and is then dropped. The time runs only while shown.
    const bool soundPlaying = barkSound_ > 0.0f;
    barkSound_ = std::max(0.0f, barkSound_ - dt);
    if (soundPlaying) {
        if (show) {
            bark_.visible = true;
            barkLeft_ -= dt;
        }
        return;
    }
    // 006a9e74..006a9e8f: once a sound that did play (+0x1c4) has ended, the bubble is dropped
    // (006a9efa) whatever is left of its text time. RETAIL row 38: "Incoming fighters!" goes with its
    // 1.15 s line, not after 1 + 0.11 s per character.
    if (barkSoundPlayed_) {
        barkActive_ = false;
        return;
    }
    if (barkLeft_ > 0.0f && show) {
        bark_.visible = true;
        barkLeft_ -= dt;
        return;
    }
    barkActive_ = false;
}

} // namespace ke
