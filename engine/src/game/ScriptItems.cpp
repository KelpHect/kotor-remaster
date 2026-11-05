// Item objects, inventories, equipment, gold, stores and containers.
//
// One model. An item anywhere is a script object (tag, resref, base item, stack,
// identified, plot, stolen, charges, loaded from its UTI) and every creature,
// placeable and store holds a list of them. The party shares the PC's inventory,
// which is character_.inventory: each row links to its item object, so the UI,
// the rules (equipped rows drive AC and attack) and the scripts see one thing.
// Retail routes a party member's inventory to the party's (00563340 behind
// 004ef770), so every handler resolves "a party member's inventory" to the PC's
// while equipment slots stay per creature.
#include "game/ScriptHost.h"

#include "game/GameSession.h"

#include "core/Log.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace ke {

namespace {

using Cells = std::optional<std::vector<script::Value>>;

constexpr std::int64_t kObjectTypeItem = 2;
constexpr std::int32_t kSlotCount = 18; // NUM_INVENTORY_SLOTS
constexpr std::int32_t kDisturbAdded = 0;   // INVENTORY_DISTURB_TYPE_ADDED
constexpr std::int32_t kDisturbRemoved = 1; // INVENTORY_DISTURB_TYPE_REMOVED
constexpr std::int64_t kBaseItemInvalid = 256;
constexpr std::size_t kNone = static_cast<std::size_t>(-1);

std::string lowerStr(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::int64_t attrInt(const script::GameObject& object, const char* key, std::int64_t fallback = 0)
{
    const auto it = object.attrs.find(key);
    if (it == object.attrs.end()) {
        return fallback;
    }
    if (const auto* integer = std::get_if<std::int64_t>(&it->second)) {
        return *integer;
    }
    if (const auto* flag = std::get_if<bool>(&it->second)) {
        return *flag ? 1 : 0;
    }
    return fallback;
}

std::string attrStr(const script::GameObject& object, const char* key)
{
    const auto it = object.attrs.find(key);
    if (it == object.attrs.end()) {
        return {};
    }
    const auto* text = std::get_if<std::string>(&it->second);
    return text != nullptr ? *text : std::string{};
}

void setInt(script::GameObject& object, const char* key, std::int64_t value)
{
    object.attrs[key] = value;
}

bool hasItemShape(const script::GameObject& object)
{
    return object.attrs.contains("item_resref");
}

} // namespace

// ---- the model -------------------------------------------------------------

struct ItemOps {
    enum class Kind { None, Party, Creature, Placeable, Store };

    GameSession& s;
    explicit ItemOps(GameSession& session) : s(session) {}

    script::World& world() const { return s.scriptWorld_; }
    ScriptApi api() const { return ScriptApi{s}; }
    std::uint32_t pc() const { return s.scriptWorld_.pc; }

    // ---- identity -----------------------------------------------------------

    script::GameObject* object(std::uint32_t id) const { return world().get(id); }

    bool isItem(std::uint32_t id) const
    {
        const script::GameObject* found = world().get(id);
        return found != nullptr && found->valid && hasItemShape(*found);
    }

    Kind kindOf(std::uint32_t id) const
    {
        if (id == 0 || id == script::kObjectInvalid || world().get(id) == nullptr) {
            return Kind::None;
        }
        if (id == pc()
            || std::find(world().party.begin(), world().party.end(), id) != world().party.end()) {
            return Kind::Party;
        }
        const std::size_t index = api().entityFor(id);
        if (index == kNone) {
            return Kind::None;
        }
        switch (s.scene_.entities[index].kind) {
        case kotor::EntityKind::Creature: return Kind::Creature;
        case kotor::EntityKind::Placeable: return Kind::Placeable;
        case kotor::EntityKind::Store: return Kind::Store;
        default: return Kind::None;
        }
    }

    bool canHold(std::uint32_t id) const { return kindOf(id) != Kind::None; }

    // Retail: a party member's inventory is the party's.
    std::uint32_t inventoryHolder(std::uint32_t id) const
    {
        return kindOf(id) == Kind::Party ? pc() : id;
    }

    CompanionState* companionOf(std::uint32_t id) const
    {
        if (id == pc()) {
            return nullptr;
        }
        const script::GameObject* found = world().get(id);
        if (found == nullptr) {
            return nullptr;
        }
        for (CompanionState& companion : s.companions_) {
            if (lowerStr(companion.tag) == lowerStr(found->tag)) {
                return &companion;
            }
        }
        return nullptr;
    }

    CharacterSheet* sheetOf(std::uint32_t id) const
    {
        if (id == pc()) {
            return &s.character_;
        }
        CompanionState* companion = companionOf(id);
        return companion != nullptr ? &companion->sheet : nullptr;
    }

    std::uint32_t ownerOf(std::uint32_t item) const
    {
        const script::GameObject* found = world().get(item);
        return found != nullptr ? static_cast<std::uint32_t>(attrInt(*found, "item_owner")) : 0;
    }

    std::int32_t slotOf(std::uint32_t item) const
    {
        const script::GameObject* found = world().get(item);
        return found != nullptr ? static_cast<std::int32_t>(attrInt(*found, "item_slot", -1)) : -1;
    }

    const kotor::ItemTemplate* templateFor(const std::string& resref) const
    {
        static std::set<std::string> missing;
        const std::string key = lowerStr(resref);
        const auto it = s.itemTemplates_.find(key);
        if (it != s.itemTemplates_.end()) {
            return &it->second;
        }
        if (key.empty() || missing.contains(key)) {
            return nullptr;
        }
        if (auto loaded = kotor::loadItemTemplate(s.install_, key); loaded.has_value()) {
            return &s.itemTemplates_.emplace(key, std::move(*loaded)).first->second;
        }
        missing.insert(key);
        return nullptr;
    }

    std::int32_t maxStackOf(std::int32_t baseItem) const
    {
        return s.rules_.loaded() ? s.rules_.maxStack(baseItem) : 1;
    }

    // ChangeItemCost scales every item of that base type (the multiplier sits on
    // the base-item row in retail, 00547970).
    std::int32_t unitCost(const script::GameObject& item) const
    {
        const std::int32_t cost = static_cast<std::int32_t>(attrInt(item, "item_value"));
        const auto it = s.itemCostScale_.find(static_cast<std::int32_t>(item.baseItem));
        return it == s.itemCostScale_.end()
            ? cost
            : static_cast<std::int32_t>(static_cast<float>(cost) * it->second);
    }

    // ---- creating items -----------------------------------------------------

    // A new, unheld item object. `allowMissing` keeps the legacy behaviour of
    // items with no UTI; scripts get retail's refusal (invalid) instead.
    std::uint32_t makeItem(const std::string& resref, std::int32_t stack, bool allowMissing,
                           std::int32_t baseHint = 0, std::int32_t costOverride = 0) const
    {
        const kotor::ItemTemplate* templ = templateFor(resref);
        if (templ == nullptr && !allowMissing) {
            return script::kObjectInvalid;
        }
        const std::int32_t base = templ != nullptr ? templ->baseItem : baseHint;
        if (s.rules_.loaded()) {
            // Retail clamps to the base item's stack limit, then refuses stacks
            // below one (0052f570).
            stack = std::min(stack, maxStackOf(base));
        }
        if (stack < 1) {
            return script::kObjectInvalid;
        }
        // The UTI Tag names the item; the resref is the fallback for an empty tag.
        const std::string tag = templ != nullptr && !templ->tag.empty() ? templ->tag : resref;
        script::GameObject& item = world().addObject(tag, 100, base, true);
        std::int32_t cost = costOverride;
        if (cost <= 0 && templ != nullptr) {
            cost = templ->cost;
        }
        if (cost <= 0 && s.rules_.loaded()) {
            cost = s.rules_.itemCost(base);
        }
        setInt(item, "object_type", kObjectTypeItem);
        item.attrs["item_resref"] = resref;
        item.attrs["item_name"] = templ != nullptr ? templ->name : std::string{};
        setInt(item, "item_stack", stack);
        setInt(item, "item_identified", templ != nullptr ? (templ->identified ? 1 : 0) : 1);
        setInt(item, "item_plot", templ != nullptr && templ->plot ? 1 : 0);
        setInt(item, "item_stolen", 0);
        setInt(item, "item_charges", templ != nullptr ? templ->charges : 0);
        setInt(item, "item_value", cost);
        setInt(item, "item_owner", 0);
        setInt(item, "item_slot", -1);
        setInt(item, "item_floor", 0);
        setInt(item, "item_nonequip", 0);
        return item.id;
    }

    void setCostScale(std::int32_t baseItem, float scale) const { s.itemCostScale_[baseItem] = scale; }

    InventoryItem toInventory(const script::GameObject& item) const
    {
        InventoryItem row;
        row.resref = attrStr(item, "item_resref");
        row.name = attrStr(item, "item_name");
        row.baseItem = static_cast<std::int32_t>(item.baseItem);
        row.cost = static_cast<std::int32_t>(attrInt(item, "item_value"));
        row.stack = static_cast<std::int32_t>(attrInt(item, "item_stack", 1));
        row.slot = s.rules_.loaded() ? s.rules_.equipSlotFor(row.baseItem) : -1;
        row.equipped = false;
        row.identified = attrInt(item, "item_identified", 1) != 0;
        row.object = item.id;
        return row;
    }

    // ---- the PC's rows <-> item objects ------------------------------------

    // Every row in character_.inventory has a live item object, and the PC's
    // object lists exactly those. Rows are the truth for the fields they hold
    // (stack, identified, cost, name, slot); a row some other path produced
    // (a loaded save, the chargen sheet, a test) gets its object here.
    void syncPc() const
    {
        script::GameObject* holder = object(pc());
        if (holder == nullptr) {
            return;
        }
        std::set<std::uint32_t> kept;
        for (InventoryItem& row : s.character_.inventory) {
            script::GameObject* item = object(row.object);
            const bool linked = item != nullptr && item->valid && hasItemShape(*item)
                && lowerStr(attrStr(*item, "item_resref")) == lowerStr(row.resref)
                && !kept.contains(row.object);
            if (!linked) {
                const std::uint32_t id = makeItem(row.resref, std::max(1, row.stack), true,
                                                  row.baseItem, row.cost);
                if (id == script::kObjectInvalid) {
                    continue;
                }
                row.object = id;
                item = object(id);
                if (item == nullptr) {
                    continue;
                }
            }
            item->baseItem = row.baseItem;
            setInt(*item, "item_stack", row.stack);
            setInt(*item, "item_identified", row.identified ? 1 : 0);
            setInt(*item, "item_value", row.cost);
            if (!row.name.empty()) {
                item->attrs["item_name"] = row.name;
            }
            setInt(*item, "item_owner", pc());
            setInt(*item, "item_slot", row.equipped ? row.slot : -1);
            kept.insert(row.object);
        }
        const std::vector<std::uint32_t> before = holder->inventory;
        holder->inventory.clear();
        for (const InventoryItem& row : s.character_.inventory) {
            if (kept.contains(row.object)) {
                holder->inventory.push_back(row.object);
            }
        }
        // Objects the PC lists with no row behind them are leftovers of a path
        // that bypassed the model: they stop existing.
        for (const std::uint32_t id : before) {
            if (kept.contains(id)) {
                continue;
            }
            script::GameObject* orphan = object(id);
            if (orphan != nullptr && hasItemShape(*orphan) && attrInt(*orphan, "item_owner") == pc()) {
                orphan->valid = false;
                setInt(*orphan, "item_owner", 0);
            }
        }
    }

    // A companion's worn gear: its native Equip_ItemList on first look, then the
    // object list. Rules reads the sheet, so worn rows are mirrored into it.
    void syncCompanion(std::uint32_t id) const
    {
        CompanionState* companion = companionOf(id);
        script::GameObject* holder = object(id);
        if (companion == nullptr || holder == nullptr) {
            return;
        }
        if (attrInt(*holder, "inv_loaded") == 0) {
            setInt(*holder, "inv_loaded", 1);
            const std::size_t index = api().entityFor(id);
            if (index != kNone) {
                for (const kotor::CreatureItem& entry : s.scene_.entities[index].creatureItems) {
                    if (entry.slot < 0) {
                        continue; // carried items of a recruit are not the party's
                    }
                    const std::uint32_t made = makeItem(entry.resref, 1, false);
                    if (made == script::kObjectInvalid) {
                        continue;
                    }
                    script::GameObject* item = object(made);
                    setInt(*item, "item_owner", id);
                    setInt(*item, "item_slot", entry.slot);
                    holder->inventory.push_back(made);
                }
            }
        }
        companion->sheet.inventory.clear();
        for (const std::uint32_t itemId : holder->inventory) {
            const script::GameObject* item = object(itemId);
            if (item == nullptr || !item->valid || attrInt(*item, "item_slot", -1) < 0) {
                continue;
            }
            InventoryItem row = toInventory(*item);
            row.equipped = true;
            row.slot = static_cast<std::int32_t>(attrInt(*item, "item_slot"));
            companion->sheet.inventory.push_back(std::move(row));
        }
    }

    // A creature or container's own contents from its template, once. The marker
    // persists with the object, so a module snapshot never loads them twice.
    void ensureLoaded(std::uint32_t id) const
    {
        script::GameObject* holder = object(id);
        const Kind kind = kindOf(id);
        if (holder == nullptr || attrInt(*holder, "inv_loaded") != 0
            || (kind != Kind::Creature && kind != Kind::Placeable)) {
            return;
        }
        setInt(*holder, "inv_loaded", 1);
        const std::size_t index = api().entityFor(id);
        if (index == kNone) {
            return;
        }
        const kotor::Entity& entity = s.scene_.entities[index];
        if (kind == Kind::Placeable) {
            for (const std::string& resref : entity.lootItems) {
                const std::uint32_t made = makeItem(resref, 1, true);
                if (made != script::kObjectInvalid) {
                    setInt(*object(made), "item_owner", id);
                    holder->inventory.push_back(made);
                }
            }
            return;
        }
        for (const kotor::CreatureItem& entry : entity.creatureItems) {
            const std::uint32_t made = makeItem(entry.resref, 1, true);
            if (made == script::kObjectInvalid) {
                continue;
            }
            script::GameObject* item = object(made);
            setInt(*item, "item_owner", id);
            setInt(*item, "item_slot", entry.slot);
            setInt(*item, "item_droppable", entry.droppable ? 1 : 0);
            holder->inventory.push_back(made);
        }
    }

    // ---- stores -------------------------------------------------------------

    std::size_t merchantFor(std::uint32_t storeObject) const
    {
        const std::size_t entity = api().entityFor(storeObject);
        if (entity == kNone) {
            return kNone;
        }
        for (std::size_t i = 0; i < s.merchants_.size(); ++i) {
            if (s.merchants_[i].entity == entity) {
                return i;
            }
        }
        return kNone;
    }

    std::vector<std::uint32_t> storeItems(std::uint32_t storeObject) const
    {
        std::vector<std::uint32_t> out;
        const std::size_t merchant = merchantFor(storeObject);
        script::GameObject* holder = object(storeObject);
        if (merchant == kNone || holder == nullptr) {
            return out;
        }
        holder->inventory.clear();
        for (MerchantStockItem& stock : s.merchants_[merchant].stock) {
            if (!stock.infinite && stock.remaining <= 0) {
                continue;
            }
            script::GameObject* item = object(stock.item.object);
            if (item == nullptr || !item->valid || !hasItemShape(*item)
                || lowerStr(attrStr(*item, "item_resref")) != lowerStr(stock.item.resref)) {
                const std::uint32_t made = makeItem(stock.item.resref, std::max(1, stock.item.stack),
                                                    true, stock.item.baseItem, stock.item.cost);
                if (made == script::kObjectInvalid) {
                    continue;
                }
                stock.item.object = made;
                item = object(made);
            }
            setInt(*item, "item_owner", storeObject);
            setInt(*item, "item_stack", stock.item.stack);
            holder->inventory.push_back(item->id);
            out.push_back(item->id);
        }
        return out;
    }

    // ---- reading inventories ------------------------------------------------

    // What GetFirstItemInInventory walks: carried items only, in order.
    std::vector<std::uint32_t> carried(std::uint32_t holderArg) const
    {
        std::vector<std::uint32_t> out;
        const Kind kind = kindOf(holderArg);
        if (kind == Kind::None) {
            return out;
        }
        if (kind == Kind::Party) {
            syncPc();
            for (const InventoryItem& row : s.character_.inventory) {
                if (!row.equipped) {
                    out.push_back(row.object);
                }
            }
            return out;
        }
        if (kind == Kind::Store) {
            return storeItems(holderArg);
        }
        ensureLoaded(holderArg);
        const script::GameObject* holder = object(holderArg);
        if (holder == nullptr) {
            return out;
        }
        for (const std::uint32_t id : holder->inventory) {
            const script::GameObject* item = object(id);
            if (item != nullptr && item->valid && attrInt(*item, "item_slot", -1) < 0) {
                out.push_back(id);
            }
        }
        return out;
    }

    // The item worn in a slot by one creature (not the shared inventory).
    std::uint32_t wornIn(std::uint32_t creature, std::int32_t slot) const
    {
        const Kind kind = kindOf(creature);
        if (kind == Kind::None || slot < 0 || slot >= kSlotCount) {
            return script::kObjectInvalid;
        }
        if (kind == Kind::Party) {
            if (creature == pc()) {
                syncPc();
                for (const InventoryItem& row : s.character_.inventory) {
                    if (row.equipped && row.slot == slot) {
                        return row.object;
                    }
                }
                return script::kObjectInvalid;
            }
            syncCompanion(creature);
        } else if (kind == Kind::Creature) {
            ensureLoaded(creature);
        } else {
            return script::kObjectInvalid;
        }
        const script::GameObject* holder = object(creature);
        if (holder == nullptr) {
            return script::kObjectInvalid;
        }
        for (const std::uint32_t id : holder->inventory) {
            const script::GameObject* item = object(id);
            if (item != nullptr && item->valid && attrInt(*item, "item_slot", -1) == slot) {
                return id;
            }
        }
        return script::kObjectInvalid;
    }

    // GetItemPossessedBy (0053a390): the holder's inventory by tag, then each of
    // its 18 slots. A party member's inventory is the party's. Retail compares
    // the tag as bytes; shipped scripts spell tags as authored, so a
    // case-insensitive match is the safe superset (evidence gap, see report).
    std::uint32_t possessedBy(std::uint32_t holderArg, const std::string& tag) const
    {
        if (tag.empty()) {
            return script::kObjectInvalid;
        }
        const std::string wanted = lowerStr(tag);
        for (const std::uint32_t id : carried(holderArg)) {
            const script::GameObject* item = object(id);
            if (item != nullptr && lowerStr(item->tag) == wanted) {
                return id;
            }
        }
        for (std::int32_t slot = 0; slot < kSlotCount; ++slot) {
            const std::uint32_t id = wornIn(holderArg, slot);
            const script::GameObject* item = object(id);
            if (item != nullptr && lowerStr(item->tag) == wanted) {
                return id;
            }
        }
        return script::kObjectInvalid;
    }

    // Does this holder (or its party) hold the item, carried or worn?
    bool holds(std::uint32_t holder, std::uint32_t item) const
    {
        const std::uint32_t owner = ownerOf(item);
        if (owner == 0 || !canHold(holder)) {
            return false;
        }
        if (owner == holder) {
            return true;
        }
        return kindOf(holder) == Kind::Party && owner == pc();
    }

    // ---- moving items -------------------------------------------------------

    void fireAcquired(std::uint32_t item, std::uint32_t from) const
    {
        api().raiseHook(s.sim_->moduleObject, "Mod_OnAcquirItem",
                        {{"moduleItemAcquired", item},
                         {"moduleItemAcquiredFrom", from},
                         {"caller", pc()}});
    }

    void fireLost(std::uint32_t item, std::uint32_t by) const
    {
        api().raiseHook(s.sim_->moduleObject, "Mod_OnUnAqreItem",
                        {{"moduleItemLost", item}, {"moduleItemLostBy", by}});
    }

    // OnInventoryDisturbed on a creature or container whose contents changed.
    void disturb(std::uint32_t holder, std::int32_t type, std::uint32_t item,
                 std::uint32_t by) const
    {
        const Kind kind = kindOf(holder);
        if (kind != Kind::Creature && kind != Kind::Placeable) {
            return;
        }
        api().raiseHook(holder, "Disturbed",
                        {{"lastDisturbed", by},
                         {"disturbType", static_cast<std::uint32_t>(type)},
                         {"disturbItem", item}});
    }

    // A container's UTP list mirrors what it holds, so the module snapshot and
    // the loot UI keep working from one source.
    void syncLootList(std::uint32_t placeable) const
    {
        const std::size_t index = api().entityFor(placeable);
        const script::GameObject* holder = object(placeable);
        if (index == kNone || holder == nullptr
            || s.scene_.entities[index].kind != kotor::EntityKind::Placeable) {
            return;
        }
        std::vector<std::string>& list = s.scene_.entities[index].lootItems;
        list.clear();
        for (const std::uint32_t id : holder->inventory) {
            const script::GameObject* item = object(id);
            if (item != nullptr && item->valid && attrInt(*item, "item_slot", -1) < 0) {
                list.push_back(attrStr(*item, "item_resref"));
            }
        }
    }

    // Takes the item out of whatever holds it. No events: callers decide.
    void detach(std::uint32_t item) const
    {
        script::GameObject* held = object(item);
        if (held == nullptr) {
            return;
        }
        const std::uint32_t owner = static_cast<std::uint32_t>(attrInt(*held, "item_owner"));
        if (owner != 0) {
            if (script::GameObject* holder = object(owner); holder != nullptr) {
                std::erase(holder->inventory, item);
            }
            if (owner == pc()) {
                std::erase_if(s.character_.inventory,
                              [&](const InventoryItem& row) { return row.object == item; });
            }
            if (CompanionState* companion = companionOf(owner); companion != nullptr) {
                std::erase_if(companion->sheet.inventory,
                              [&](const InventoryItem& row) { return row.object == item; });
            }
            const Kind kind = kindOf(owner);
            if (kind == Kind::Store) {
                const std::size_t merchant = merchantFor(owner);
                if (merchant != kNone) {
                    for (MerchantStockItem& stock : s.merchants_[merchant].stock) {
                        if (stock.item.object == item && !stock.infinite) {
                            stock.remaining = 0;
                        }
                    }
                }
            } else if (kind == Kind::Placeable) {
                syncLootList(owner);
            }
        }
        setInt(*held, "item_owner", 0);
        setInt(*held, "item_slot", -1);
    }

    // Puts an item in a holder's inventory, merging into a partial stack of the
    // same item first. Returns the item that now holds the count (the target
    // stack when everything merged) or invalid. Fires the module acquire/lose
    // events when the item crosses the party boundary.
    std::uint32_t addTo(std::uint32_t item, std::uint32_t holderArg, std::uint32_t from) const
    {
        script::GameObject* held = object(item);
        const Kind kind = kindOf(holderArg);
        if (held == nullptr || !held->valid || !hasItemShape(*held) || kind == Kind::None) {
            return script::kObjectInvalid;
        }
        const std::uint32_t holder = inventoryHolder(holderArg);
        const std::uint32_t previous = ownerOf(item);
        const bool wasParty = previous != 0 && kindOf(previous) == Kind::Party;
        const bool nowParty = kind == Kind::Party;
        if (previous == holder && slotOf(item) < 0) {
            return item; // already there
        }
        if (from == script::kObjectInvalid && previous != 0) {
            from = previous;
        }
        if (nowParty) {
            syncPc();
        } else {
            ensureLoaded(holder);
        }
        detach(item);
        std::uint32_t result = item;
        std::int32_t count = static_cast<std::int32_t>(attrInt(*held, "item_stack", 1));
        const std::int32_t maxStack = maxStackOf(static_cast<std::int32_t>(held->baseItem));
        if (maxStack > 1) {
            for (const std::uint32_t existing : carried(holder)) {
                script::GameObject* target = object(existing);
                if (target == nullptr || existing == item
                    || lowerStr(attrStr(*target, "item_resref"))
                        != lowerStr(attrStr(*held, "item_resref"))) {
                    continue;
                }
                const std::int32_t have = static_cast<std::int32_t>(attrInt(*target, "item_stack", 1));
                const std::int32_t moved = std::min(maxStack - have, count);
                if (moved <= 0) {
                    continue;
                }
                setStack(existing, have + moved);
                count -= moved;
                result = existing;
                if (count == 0) {
                    break;
                }
            }
        }
        if (count == 0) {
            held->valid = false;
            setInt(*held, "destroyed", 1);
        } else {
            setInt(*held, "item_stack", count);
            setInt(*held, "item_owner", holder);
            setInt(*held, "item_slot", -1);
            setInt(*held, "item_floor", 0);
            result = item;
            script::GameObject* container = object(holder);
            if (container != nullptr) {
                container->inventory.push_back(item);
            }
            if (nowParty) {
                s.character_.inventory.push_back(toInventory(*held));
            } else if (kind == Kind::Store) {
                const std::size_t merchant = merchantFor(holder);
                if (merchant != kNone) {
                    MerchantStockItem stock;
                    stock.item = toInventory(*held);
                    stock.infinite = false;
                    stock.remaining = 1;
                    s.merchants_[merchant].stock.push_back(std::move(stock));
                }
            } else if (kind == Kind::Placeable) {
                syncLootList(holder);
            }
        }
        if (!wasParty && nowParty) {
            fireAcquired(result, from);
        } else if (wasParty && !nowParty) {
            fireLost(item, previous == 0 ? pc() : previous);
        }
        return result;
    }

    void setStack(std::uint32_t item, std::int32_t count) const
    {
        script::GameObject* found = object(item);
        if (found == nullptr) {
            return;
        }
        setInt(*found, "item_stack", count);
        for (InventoryItem& row : s.character_.inventory) {
            if (row.object == item) {
                row.stack = count;
            }
        }
        for (CompanionState& companion : s.companions_) {
            for (InventoryItem& row : companion.sheet.inventory) {
                if (row.object == item) {
                    row.stack = count;
                }
            }
        }
        const std::uint32_t owner = ownerOf(item);
        if (kindOf(owner) == Kind::Store) {
            const std::size_t merchant = merchantFor(owner);
            if (merchant != kNone) {
                for (MerchantStockItem& stock : s.merchants_[merchant].stock) {
                    if (stock.item.object == item) {
                        stock.item.stack = count;
                    }
                }
            }
        }
    }

    // DestroyObject on an item: out of its holder, no longer an object.
    bool destroy(std::uint32_t item) const
    {
        script::GameObject* found = object(item);
        if (found == nullptr || !hasItemShape(*found)) {
            return false;
        }
        const std::uint32_t previous = ownerOf(item);
        const bool wasParty = previous != 0 && kindOf(previous) == Kind::Party;
        if (wasParty) {
            syncPc();
        }
        detach(item);
        found->valid = false;
        setInt(*found, "destroyed", 1);
        if (wasParty) {
            fireLost(item, pc());
        }
        return true;
    }

    // ---- equipment ----------------------------------------------------------

    // Puts a worn item back into carried inventory.
    void unequipTo(std::uint32_t creature, std::uint32_t item) const
    {
        script::GameObject* worn = object(item);
        if (worn == nullptr) {
            return;
        }
        if (creature == pc()) {
            for (InventoryItem& row : s.character_.inventory) {
                if (row.object == item) {
                    row.equipped = false;
                    row.slot = s.rules_.loaded() ? s.rules_.equipSlotFor(row.baseItem) : -1;
                }
            }
            setInt(*worn, "item_slot", -1);
            return;
        }
        if (kindOf(creature) == Kind::Party) {
            // A companion's gear goes back to the party's shared inventory.
            detach(item);
            setInt(*worn, "item_owner", pc());
            if (script::GameObject* holder = object(pc()); holder != nullptr) {
                holder->inventory.push_back(item);
            }
            s.character_.inventory.push_back(toInventory(*worn));
            syncCompanion(creature);
            return;
        }
        setInt(*worn, "item_slot", -1);
    }

    // ActionEquipItem / ActionUnequipItem. Retail queues these on the actor; the
    // host runs them when issued, which orders them correctly for the scripts
    // that follow with an attack.
    bool equip(std::uint32_t creature, std::uint32_t item, std::int32_t slotArg) const
    {
        const Kind kind = kindOf(creature);
        script::GameObject* gear = object(item);
        if ((kind != Kind::Party && kind != Kind::Creature) || gear == nullptr || !gear->valid
            || !hasItemShape(*gear)) {
            return false;
        }
        if (kind == Kind::Party) {
            syncPc();
            if (creature != pc()) {
                syncCompanion(creature);
            }
        } else {
            ensureLoaded(creature);
        }
        if (!holds(creature, item) || attrInt(*gear, "item_nonequip") != 0) {
            return false;
        }
        const std::int32_t base = static_cast<std::int32_t>(gear->baseItem);
        const std::uint32_t mask = s.rules_.loaded() ? s.rules_.equipMask(base) : 0;
        if (mask == 0) {
            return false;
        }
        std::int32_t slot = slotArg;
        if (slot < 0) {
            for (std::int32_t bit = 0; bit < kSlotCount; ++bit) {
                if ((mask & (1u << bit)) != 0) {
                    slot = bit;
                    break;
                }
            }
        }
        if (slot < 0 || slot >= kSlotCount || (mask & (1u << slot)) == 0) {
            return false;
        }
        if (const CharacterSheet* sheet = sheetOf(creature);
            sheet != nullptr && !s.rules_.canEquip(*sheet, toInventory(*gear))) {
            return false;
        }
        const std::uint32_t occupant = wornIn(creature, slot);
        if (occupant == item) {
            return true;
        }
        if (occupant != script::kObjectInvalid) {
            unequipTo(creature, occupant);
        }
        if (slotOf(item) >= 0 && ownerOf(item) == (creature == pc() ? pc() : creature)) {
            // Moving between slots: just relabel.
        }
        if (creature == pc()) {
            for (InventoryItem& row : s.character_.inventory) {
                if (row.object == item) {
                    row.equipped = true;
                    row.slot = slot;
                }
            }
        } else if (kind == Kind::Party) {
            // A companion wears it: it leaves the shared inventory.
            detach(item);
            setInt(*gear, "item_owner", creature);
            if (script::GameObject* holder = object(creature); holder != nullptr) {
                holder->inventory.push_back(item);
            }
        }
        setInt(*gear, "item_slot", slot);
        if (kind == Kind::Party && creature != pc()) {
            syncCompanion(creature);
        }
        api().actor(creature).last["lastItemEquipped"] = item;
        refreshModel(creature);
        s.logEvent("equip " + attrStr(*gear, "item_resref") + " slot " + std::to_string(slot));
        return true;
    }

    bool unequip(std::uint32_t creature, std::uint32_t item) const
    {
        const Kind kind = kindOf(creature);
        if ((kind != Kind::Party && kind != Kind::Creature) || !isItem(item)
            || slotOf(item) < 0) {
            return false;
        }
        const std::uint32_t owner = ownerOf(item);
        if (owner != creature) {
            return false;
        }
        unequipTo(creature, item);
        refreshModel(creature);
        return true;
    }

    // Weapons in the right and left hand slots show on a creature entity's model
    // (the loader's weapon attach is rebuilt). Armour and other slots have no
    // runtime model in the existing pipeline.
    void refreshModel(std::uint32_t creature) const
    {
        const std::size_t index = api().entityFor(creature);
        if (index == kNone || index >= s.scene_.entities.size()) {
            return;
        }
        const auto modelOf = [&](std::uint32_t item) -> std::string {
            const script::GameObject* gear = object(item);
            if (gear == nullptr || !s.rules_.loaded()) {
                return {};
            }
            return s.rules_.baseItemString(static_cast<std::int32_t>(gear->baseItem), "defaultmodel");
        };
        kotor::Entity& entity = s.scene_.entities[index];
        const std::string right = modelOf(wornIn(creature, 4));
        const std::string left = modelOf(wornIn(creature, 5));
        entity.weaponModel = right == "I_Null" ? std::string{} : right;
        entity.offhandModel = left == "I_Null" ? std::string{} : left;
    }
};

// ---- GameSession: the item API ---------------------------------------------

std::uint32_t GameSession::giveItem(const std::string& resref, std::uint32_t holder,
                                    std::int32_t stack, std::uint32_t from)
{
    ItemOps ops(*this);
    const std::uint32_t made = ops.makeItem(resref, stack, false);
    if (made == script::kObjectInvalid) {
        return script::kObjectInvalid;
    }
    const std::uint32_t result = ops.addTo(made, holder, from);
    if (result == script::kObjectInvalid) {
        // Retail destroys an item that has nowhere to go (0052f570).
        if (script::GameObject* orphan = ops.object(made); orphan != nullptr) {
            orphan->valid = false;
        }
    }
    return result;
}

bool GameSession::moveItem(std::uint32_t item, std::uint32_t holder, std::uint32_t from)
{
    ItemOps ops(*this);
    return ops.isItem(item) && ops.addTo(item, holder, from) != script::kObjectInvalid;
}

bool GameSession::destroyItem(std::uint32_t item)
{
    return ItemOps(*this).destroy(item);
}

std::uint32_t GameSession::itemPossessedBy(std::uint32_t holder, const std::string& tag)
{
    return ItemOps(*this).possessedBy(holder, tag);
}

std::uint32_t GameSession::itemPossessor(std::uint32_t item)
{
    ItemOps ops(*this);
    if (!ops.isItem(item)) {
        return script::kObjectInvalid;
    }
    ops.syncPc();
    const std::uint32_t owner = ops.ownerOf(item);
    return owner != 0 && ops.canHold(owner) ? owner : script::kObjectInvalid;
}

std::vector<std::uint32_t> GameSession::itemsOf(std::uint32_t holder)
{
    return ItemOps(*this).carried(holder);
}

std::uint32_t GameSession::inventoryObject(std::size_t index)
{
    ItemOps ops(*this);
    ops.syncPc();
    return index < character_.inventory.size() ? character_.inventory[index].object
                                                : script::kObjectInvalid;
}

bool GameSession::equipItemObject(std::uint32_t creature, std::uint32_t item, std::int32_t slot)
{
    return ItemOps(*this).equip(creature, item, slot);
}

bool GameSession::unequipItemObject(std::uint32_t creature, std::uint32_t item)
{
    return ItemOps(*this).unequip(creature, item);
}

std::int32_t GameSession::creatureGold(std::uint32_t creature) const
{
    ItemOps ops(const_cast<GameSession&>(*this));
    switch (ops.kindOf(creature)) {
    case ItemOps::Kind::Party: return gold_;
    case ItemOps::Kind::Creature: {
        const script::GameObject* found = ops.object(creature);
        return found != nullptr ? static_cast<std::int32_t>(attrInt(*found, "gold")) : 0;
    }
    default: return 0;
    }
}

void GameSession::addCreatureGold(std::uint32_t creature, std::int32_t amount)
{
    ItemOps ops(*this);
    switch (ops.kindOf(creature)) {
    case ItemOps::Kind::Party:
        gold_ = std::max<std::int32_t>(0, gold_ + amount);
        break;
    case ItemOps::Kind::Creature:
        if (script::GameObject* found = ops.object(creature); found != nullptr) {
            setInt(*found, "gold", std::max<std::int64_t>(0, attrInt(*found, "gold") + amount));
        }
        break;
    default:
        break;
    }
}

// ---- legacy API, now views of the model -------------------------------------

bool GameSession::addInventoryItem(const std::string& resref, std::int32_t baseItem,
                                   std::int32_t stack, std::int32_t cost)
{
    if (resref.empty() || stack <= 0) {
        return false;
    }
    ItemOps ops(*this);
    ops.syncPc();
    const std::uint32_t made = ops.makeItem(resref, stack, true, baseItem, cost);
    if (made == script::kObjectInvalid) {
        return false;
    }
    if (ops.addTo(made, scriptWorld_.pc, script::kObjectInvalid) == script::kObjectInvalid) {
        return false;
    }
    logEvent("item " + resref + " x" + std::to_string(stack));
    return true;
}

bool GameSession::removeInventoryItem(const std::string& resref, std::int32_t stack)
{
    ItemOps ops(*this);
    ops.syncPc();
    for (const InventoryItem& row : character_.inventory) {
        if (lowerStr(row.resref) != lowerStr(resref)) {
            continue;
        }
        const std::uint32_t object = row.object;
        if (stack > 0 && row.stack > stack) {
            ops.setStack(object, row.stack - stack);
            return true;
        }
        return ops.destroy(object);
    }
    return false;
}

bool GameSession::equipItem(const std::string& resref)
{
    ItemOps ops(*this);
    ops.syncPc();
    for (const InventoryItem& row : character_.inventory) {
        if (lowerStr(row.resref) != lowerStr(resref)) {
            continue;
        }
        // The default slot: the lowest bit of the base item's equipable mask.
        return ops.equip(scriptWorld_.pc, row.object,
                         rules_.loaded() ? rules_.equipSlotFor(row.baseItem) : -1);
    }
    return false;
}

// ---- stores ------------------------------------------------------------------

void GameSession::closeStore()
{
    activeMerchant_ = -1;
    storeBonusMarkUp_ = 0;
    storeBonusMarkDown_ = 0;
}

// ACT_378 (00540300): both bonuses clamp to [-100, 100], the store records who
// opened it and the store panel opens. The arithmetic the panel applies is not
// recovered; the bonuses add to the template's MarkUp and MarkDown percentages.
bool GameSession::openStoreFor(std::uint32_t store, std::uint32_t pc, std::int32_t bonusMarkUp,
                               std::int32_t bonusMarkDown)
{
    (void)pc;
    ItemOps ops(*this);
    if (ops.kindOf(store) != ItemOps::Kind::Store) {
        return false;
    }
    const std::size_t entity = ScriptApi{*this}.entityFor(store);
    if (entity == kNone || !openStore(entity)) {
        return false;
    }
    storeBonusMarkUp_ = std::clamp<std::int32_t>(bonusMarkUp, -100, 100);
    storeBonusMarkDown_ = std::clamp<std::int32_t>(bonusMarkDown, -100, 100);
    setUiPanel(UiPanel::Merchant);
    return true;
}

// ---- containers ----------------------------------------------------------------

bool GameSession::openLoot(std::size_t entityIndex)
{
    if (entityIndex >= scene_.entities.size()
        || scene_.entities[entityIndex].kind != kotor::EntityKind::Placeable) {
        return false;
    }
    // Retail opens a placeable only when its HasInventory flag is set (00587420);
    // items listed on a template without the flag stay unreachable (00587710
    // counts none).
    if (!scene_.entities[entityIndex].hasInventory) {
        return false;
    }
    ItemOps ops(*this);
    const std::uint32_t container = objectForEntity(entityIndex);
    const std::vector<std::uint32_t> items = ops.carried(container);
    if (items.empty()) {
        return false;
    }
    lootItems_.clear();
    lootEntity_ = static_cast<int>(entityIndex);
    for (const std::uint32_t id : items) {
        lootItems_.push_back(ops.toInventory(*ops.object(id)));
    }
    scene_.entities[entityIndex].open = true;
    setUiPanel(UiPanel::Loot);
    ops.api().raiseHook(container, "OnOpen", {{"lastOpenedBy", scriptWorld_.pc}});
    logEvent("loot " + scene_.entities[entityIndex].tag + " x" + std::to_string(lootItems_.size()));
    return true;
}

bool GameSession::takeLootItem(std::size_t index)
{
    if (lootEntity_ < 0 || index >= lootItems_.size()
        || static_cast<std::size_t>(lootEntity_) >= scene_.entities.size()) {
        return false;
    }
    ItemOps ops(*this);
    const std::uint32_t container = objectForEntity(static_cast<std::size_t>(lootEntity_));
    const std::uint32_t id = lootItems_[index].object;
    if (!ops.isItem(id) || ops.ownerOf(id) != container) {
        lootItems_.erase(lootItems_.begin() + static_cast<std::ptrdiff_t>(index));
        return false;
    }
    const std::string resref = lootItems_[index].resref;
    const std::uint32_t moved = ops.addTo(id, scriptWorld_.pc, container);
    if (moved == script::kObjectInvalid) {
        return false;
    }
    ops.disturb(container, kDisturbRemoved, moved, scriptWorld_.pc);
    lootItems_.erase(lootItems_.begin() + static_cast<std::ptrdiff_t>(index));
    logEvent("take " + resref);
    if (lootItems_.empty()) {
        closeLoot();
        setUiPanel(UiPanel::None);
    }
    return true;
}

void GameSession::closeLoot()
{
    if (lootEntity_ >= 0 && static_cast<std::size_t>(lootEntity_) < scene_.entities.size()) {
        scene_.entities[static_cast<std::size_t>(lootEntity_)].open = false;
        ScriptApi{*this}.raiseHook(objectForEntity(static_cast<std::size_t>(lootEntity_)),
                                   "OnClosed", {{"lastClosedBy", scriptWorld_.pc}});
    }
    lootItems_.clear();
    lootEntity_ = -1;
}

// ---- item use --------------------------------------------------------------------

namespace {

constexpr std::int32_t kItemPropertyActivateItem = 10; // ITEM_PROPERTY_ACTIVATE_ITEM

const kotor::TwoDa* tableFor(kotor::Install& install, const std::string& name)
{
    static std::map<std::string, std::optional<kotor::TwoDa>> cache;
    auto it = cache.find(name);
    if (it == cache.end()) {
        std::optional<kotor::TwoDa> table;
        if (auto data = install.find(name, 2017); data.has_value()) {
            try {
                table = kotor::TwoDa::parse(std::move(*data), name);
            } catch (const kotor::FormatError&) {
            }
        }
        it = cache.emplace(name, std::move(table)).first;
    }
    return it->second.has_value() ? &*it->second : nullptr;
}

std::int32_t cellValue(const kotor::TwoDa& table, const std::string& row, const char* column,
                       std::int32_t fallback)
{
    const std::string text = table.cell(row, column);
    if (text.empty() || text == "****") {
        return fallback;
    }
    try {
        return static_cast<std::int32_t>(std::stol(text, nullptr, 0));
    } catch (const std::exception&) {
        return fallback;
    }
}

} // namespace

// An item's ACTIVATE_ITEM property names a spell (iprp_spells.2da -> spells.2da),
// and using the item runs that spell's impact script with the user as caster, the
// target and the item in context, as a cast does. The module's activation event
// is raised too (EventActivateItem, ACT_424), which is how shipped scripts see
// unique items. Charges, then the stack, are spent. The spell's effects only
// land once the effects family applies them; this call is the only entry the
// quest bot and the UI need.
bool GameSession::useItem(std::uint32_t item, std::uint32_t user, std::uint32_t target)
{
    ItemOps ops(*this);
    if (user == 0 || user == script::kObjectInvalid) {
        user = scriptWorld_.pc;
    }
    if (!ops.isItem(item)) {
        return false;
    }
    ops.syncPc();
    if (!ops.holds(user, item)) {
        return false;
    }
    const script::GameObject& gear = *ops.object(item);
    const kotor::ItemTemplate* templ = ops.templateFor(attrStr(gear, "item_resref"));
    if (templ == nullptr) {
        return false;
    }
    if (target == 0 || target == script::kObjectInvalid) {
        target = user;
    }
    const kotor::TwoDa* spellRefs = tableFor(install_, "iprp_spells");
    const kotor::TwoDa* spells = tableFor(install_, "spells");
    bool usable = false;
    std::vector<std::string> scripts;
    for (const kotor::ItemTemplate::Property& property : templ->properties) {
        if (property.name != kItemPropertyActivateItem || !property.useable) {
            continue;
        }
        usable = true;
        if (spellRefs == nullptr || spells == nullptr) {
            continue;
        }
        const std::int32_t spell = cellValue(*spellRefs, std::to_string(property.subtype),
                                             "spellindex", -1);
        if (spell < 0) {
            continue;
        }
        const std::string script = spells->cell(std::to_string(spell), "impactscript");
        if (!script.empty() && script != "****") {
            scripts.push_back(script);
        }
    }
    if (!usable) {
        return false;
    }
    ScriptApi api{*this};
    api.raiseHook(sim_->moduleObject, "Mod_OnActvtItem",
                  {{"itemActivated", item},
                   {"itemActivator", user},
                   {"itemTarget", target},
                   {"caller", scriptWorld_.pc}});
    for (const std::string& script : scripts) {
        ScriptContext context;
        context["lastSpellCaster"] = user;
        context["spellTarget"] = target;
        context["attemptedTarget"] = target;
        context["spellCastItem"] = item;
        context["itemActivated"] = item;
        context["itemActivator"] = user;
        context["itemTarget"] = target;
        api.runScript(script, user, context);
    }
    // Spend a charge; when none are left the use consumes one of the stack. An
    // item with no charges (equipment with an activation) is not consumed.
    const std::int64_t charges = attrInt(gear, "item_charges");
    if (charges > 0) {
        setInt(*ops.object(item), "item_charges", charges - 1);
        if (charges - 1 == 0) {
            const std::int32_t stack = static_cast<std::int32_t>(attrInt(gear, "item_stack", 1));
            if (stack > 1) {
                ops.setStack(item, stack - 1);
                setInt(*ops.object(item), "item_charges", templ->charges);
            } else {
                ops.destroy(item);
            }
        }
    }
    logEvent("use item " + templ->resref);
    return true;
}

// ---- handlers ------------------------------------------------------------------

namespace {

Cells invalidObject()
{
    return ActionCall::retObject(script::kObjectInvalid);
}

// ACT_29 GetItemPossessor
Cells getItemPossessor(ActionCall& call)
{
    return ActionCall::retObject(call.session.itemPossessor(call.objectArg(0)));
}

// ACT_30 GetItemPossessedBy (0053a390)
Cells getItemPossessedBy(ActionCall& call)
{
    return ActionCall::retObject(call.session.itemPossessedBy(call.objectArg(0), call.stringArg(1)));
}

// ACT_31 CreateItemOnObject (0052f570)
Cells createItemOnObject(ActionCall& call)
{
    return ActionCall::retObject(call.session.giveItem(call.stringArg(0), call.objectArg(1),
                                                       static_cast<std::int32_t>(call.intArg(2, 1)),
                                                       script::kObjectInvalid));
}

// ACT_32 ActionEquipItem: queued in retail; run when issued here.
Cells actionEquipItem(ActionCall& call)
{
    if (!call.session.equipItemObject(call.self(), call.objectArgOrInvalid(0),
                                      static_cast<std::int32_t>(call.intArg(1, -1)))) {
        call.api().log("ActionEquipItem failed.");
    }
    return ActionCall::none();
}

// ACT_33 ActionUnequipItem (00545050)
Cells actionUnequipItem(ActionCall& call)
{
    call.session.unequipItemObject(call.self(), call.objectArgOrInvalid(0));
    return ActionCall::none();
}

// ACT_34 ActionPickUpItem: the actor takes an item off the ground. Walking to it
// first is not modelled; the item must be on the floor.
Cells actionPickUpItem(ActionCall& call)
{
    ItemOps ops(call.session);
    const std::uint32_t item = call.objectArgOrInvalid(0);
    script::GameObject* found = ops.object(item);
    if (!ops.isItem(item) || attrInt(*found, "item_floor") == 0
        || !ops.canHold(call.self())
        || call.session.moveItem(item, call.self()) == false) {
        call.api().log("ActionPickUpItem failed.");
    }
    return ActionCall::none();
}

// ACT_35 ActionPutDownItem: the item leaves the actor's inventory for the ground.
Cells actionPutDownItem(ActionCall& call)
{
    ItemOps ops(call.session);
    const std::uint32_t item = call.objectArgOrInvalid(0);
    const std::uint32_t self = call.self();
    if (!ops.isItem(item)) {
        call.api().log("ActionPutDownItem failed.");
        return ActionCall::none();
    }
    ops.syncPc();
    if (!ops.holds(self, item)) {
        call.api().log("ActionPutDownItem failed.");
        return ActionCall::none();
    }
    const std::uint32_t previous = ops.ownerOf(item);
    const bool wasParty = ops.kindOf(previous) == ItemOps::Kind::Party;
    ops.detach(item);
    script::GameObject* found = ops.object(item);
    const kotor::Vec3 at = call.api().position(self);
    found->position = {at.x, at.y, at.z};
    setInt(*found, "item_floor", 1);
    if (wasParty) {
        ops.fireLost(item, previous);
    }
    return ActionCall::none();
}

// ACT_52 GetLastItemEquipped (0053f800): a field of the running object.
Cells getLastItemEquipped(ActionCall& call)
{
    const ScriptContext& last = call.api().actor(call.self()).last;
    const auto it = last.find("lastItemEquipped");
    return ActionCall::retObject(it != last.end() ? it->second : script::kObjectInvalid);
}

// ACT_135 ActionGiveItem(oItem, oGiveTo): the actor hands over an item it holds.
Cells actionGiveItem(ActionCall& call)
{
    ItemOps ops(call.session);
    const std::uint32_t item = call.objectArgOrInvalid(0);
    const std::uint32_t to = call.objectArgOrInvalid(1);
    const std::uint32_t self = call.self();
    ops.syncPc();
    if (!ops.isItem(item) || !ops.holds(self, item) || !ops.canHold(to)) {
        return ActionCall::none();
    }
    if (call.session.moveItem(item, to, self)) {
        ops.disturb(to, kDisturbAdded, item, self);
        ops.disturb(self, kDisturbRemoved, item, self);
    }
    return ActionCall::none();
}

// ACT_271 GiveItem(oItem, oGiveTo): from whoever holds it.
Cells giveItemAction(ActionCall& call)
{
    ItemOps ops(call.session);
    const std::uint32_t item = call.objectArgOrInvalid(0);
    const std::uint32_t to = call.objectArgOrInvalid(1);
    if (ops.isItem(item) && ops.canHold(to)) {
        const std::uint32_t from = ops.ownerOf(item);
        if (call.session.moveItem(item, to, from)) {
            ops.disturb(to, kDisturbAdded, item, from);
        }
    }
    return ActionCall::none();
}

// ACT_136 ActionTakeItem(oItem, oTakeFrom): the actor takes it from oTakeFrom.
// A caller that cannot hold items does nothing.
Cells actionTakeItem(ActionCall& call)
{
    ItemOps ops(call.session);
    const std::uint32_t item = call.objectArgOrInvalid(0);
    const std::uint32_t from = call.objectArgOrInvalid(1);
    const std::uint32_t self = call.self();
    ops.syncPc();
    if (!ops.isItem(item) || !ops.canHold(self) || !ops.holds(from, item)) {
        return ActionCall::none();
    }
    if (call.session.moveItem(item, self, from)) {
        ops.disturb(from, kDisturbRemoved, item, self);
    }
    return ActionCall::none();
}

// ACT_138 GetItemStackSize / ACT_475 GetNumStackedItems
Cells getItemStackSize(ActionCall& call)
{
    ItemOps ops(call.session);
    ops.syncPc();
    const std::uint32_t item = call.objectArgOrInvalid(0);
    return ActionCall::retInt(ops.isItem(item) ? attrInt(*ops.object(item), "item_stack", 1) : 0);
}

// ACT_150 SetItemStackSize (00546630): clamped to [1, base item maximum].
Cells setItemStackSize(ActionCall& call)
{
    ItemOps ops(call.session);
    const std::uint32_t item = call.objectArgOrInvalid(0);
    if (!ops.isItem(item)) {
        return ActionCall::none();
    }
    ops.syncPc();
    const std::int32_t limit = ops.maxStackOf(static_cast<std::int32_t>(ops.object(item)->baseItem));
    ops.setStack(item, std::clamp<std::int32_t>(static_cast<std::int32_t>(call.intArg(1)), 1,
                                                 std::max(1, limit)));
    return ActionCall::none();
}

// ACT_155 GetItemInSlot
Cells getItemInSlot(ActionCall& call)
{
    return ActionCall::retObject(ItemOps(call.session).wornIn(
        call.objectArg(1), static_cast<std::int32_t>(call.intArg(0))));
}

// ACT_266 SetItemNonEquippable
Cells setItemNonEquippable(ActionCall& call)
{
    ItemOps ops(call.session);
    const std::uint32_t item = call.objectArgOrInvalid(0);
    if (ops.isItem(item)) {
        setInt(*ops.object(item), "item_nonequip", call.intArg(1) != 0 ? 1 : 0);
    }
    return ActionCall::none();
}

// ACT_332 GetIdentified / ACT_333 SetIdentified
Cells getIdentified(ActionCall& call)
{
    ItemOps ops(call.session);
    ops.syncPc();
    const std::uint32_t item = call.objectArgOrInvalid(0);
    return ActionCall::retInt(ops.isItem(item) ? attrInt(*ops.object(item), "item_identified", 1) : 0);
}

Cells setIdentified(ActionCall& call)
{
    ItemOps ops(call.session);
    const std::uint32_t item = call.objectArgOrInvalid(0);
    if (!ops.isItem(item)) {
        return ActionCall::none();
    }
    ops.syncPc();
    const bool identified = call.intArg(1) != 0;
    setInt(*ops.object(item), "item_identified", identified ? 1 : 0);
    for (InventoryItem& row : call.session.character().inventory) {
        if (row.object == item) {
            row.identified = identified;
        }
    }
    return ActionCall::none();
}

Cells walkInventory(ActionCall& call, bool first)
{
    ItemOps ops(call.session);
    const std::uint32_t holder = call.objectArg(0);
    const std::string key = "inv:" + std::to_string(holder);
    const std::vector<std::uint32_t> items = ops.carried(holder);
    std::int64_t position = first ? 0 : call.world.cursor(key);
    if (position < 0 || position >= static_cast<std::int64_t>(items.size())) {
        call.world.setCursor(key, static_cast<std::int64_t>(items.size()));
        return invalidObject();
    }
    call.world.setCursor(key, position + 1);
    return ActionCall::retObject(items[static_cast<std::size_t>(position)]);
}

// ACT_339 / ACT_340 (00549b50): carried items of a creature, container or store.
Cells getFirstItemInInventory(ActionCall& call)
{
    return walkInventory(call, true);
}

Cells getNextItemInInventory(ActionCall& call)
{
    return walkInventory(call, false);
}

// ACT_570 GetHasInventory (005392d0): creatures and stores always; a placeable
// answers its HasInventory flag; items and everything else never.
Cells getHasInventory(ActionCall& call)
{
    ItemOps ops(call.session);
    const std::uint32_t object = call.objectArg(0);
    switch (ops.kindOf(object)) {
    case ItemOps::Kind::Party:
    case ItemOps::Kind::Creature:
    case ItemOps::Kind::Store:
        return ActionCall::retInt(1);
    case ItemOps::Kind::Placeable: {
        const std::size_t index = call.api().entityFor(object);
        const kotor::Entity& entity = call.api().scene().entities[index];
        return ActionCall::retInt(entity.hasInventory ? 1 : 0); // reads the flag, 005392d0
    }
    default:
        return ActionCall::retInt(0);
    }
}

// ACT_397 GetBaseItemType: BASE_ITEM_INVALID for a non-item.
Cells getBaseItemType(ActionCall& call)
{
    ItemOps ops(call.session);
    const std::uint32_t item = call.objectArgOrInvalid(0);
    return ActionCall::retInt(ops.isItem(item) ? ops.object(item)->baseItem : kBaseItemInvalid);
}

// ACT_401 GetItemACValue: the base item's armour value (property bonuses are
// not folded in).
Cells getItemACValue(ActionCall& call)
{
    ItemOps ops(call.session);
    const std::uint32_t item = call.objectArgOrInvalid(0);
    if (!ops.isItem(item) || !call.session.rules().loaded()) {
        return ActionCall::retInt(0);
    }
    return ActionCall::retInt(call.session.rules().baseItemInt(
        static_cast<std::int32_t>(ops.object(item)->baseItem), "baseac", 0));
}

// ACT_398 GetItemHasItemProperty
Cells getItemHasItemProperty(ActionCall& call)
{
    ItemOps ops(call.session);
    const std::uint32_t item = call.objectArgOrInvalid(0);
    if (!ops.isItem(item)) {
        return ActionCall::retInt(0);
    }
    const kotor::ItemTemplate* templ = ops.templateFor(attrStr(*ops.object(item), "item_resref"));
    if (templ == nullptr) {
        return ActionCall::retInt(0);
    }
    const std::int64_t wanted = call.intArg(1);
    for (const kotor::ItemTemplate::Property& property : templ->properties) {
        if (property.name == wanted) {
            return ActionCall::retInt(1);
        }
    }
    return ActionCall::retInt(0);
}

// ACT_418 GetGold: the party's for the PC and party members; a creature's own
// otherwise. A caller with no gold (the module, an area) falls to the legacy path.
Cells getGold(ActionCall& call)
{
    ItemOps ops(call.session);
    const std::uint32_t who = call.objectArg(0);
    if (ops.kindOf(who) == ItemOps::Kind::None) {
        return std::nullopt;
    }
    return ActionCall::retInt(call.session.creatureGold(who));
}

// ACT_322 GiveGoldToCreature
Cells giveGoldToCreature(ActionCall& call)
{
    const std::int64_t amount = call.intArg(1);
    if (amount > 0) {
        call.session.addCreatureGold(call.objectArg(0), static_cast<std::int32_t>(amount));
    }
    return ActionCall::none();
}

// ACT_444 TakeGoldFromCreature(nAmount, oCreatureToTakeFrom, bDestroy): what the
// creature has moves to the caller unless bDestroy is set. Taking more than is
// held takes what there is.
Cells takeGoldFromCreature(ActionCall& call)
{
    ItemOps ops(call.session);
    const std::uint32_t from = call.objectArg(1);
    const ItemOps::Kind kind = ops.kindOf(from);
    if (kind != ItemOps::Kind::Party && kind != ItemOps::Kind::Creature) {
        return ActionCall::none();
    }
    const std::int32_t taken = std::min<std::int32_t>(
        static_cast<std::int32_t>(std::max<std::int64_t>(0, call.intArg(0))),
        call.session.creatureGold(from));
    call.session.addCreatureGold(from, -taken);
    if (call.intArg(2) == 0) {
        call.session.addCreatureGold(call.self(), taken);
    }
    return ActionCall::none();
}

// ACT_378 OpenStore (00540300)
Cells openStore(ActionCall& call)
{
    call.session.openStoreFor(call.objectArgOrInvalid(0), call.objectArgOrInvalid(1),
                              static_cast<std::int32_t>(call.intArg(2)),
                              static_cast<std::int32_t>(call.intArg(3)));
    return ActionCall::none();
}

// ACT_747 ChangeItemCost (00547970): the multiplier is stored per base item.
Cells changeItemCost(ActionCall& call)
{
    ItemOps ops(call.session);
    if (const kotor::ItemTemplate* templ = ops.templateFor(call.stringArg(0)); templ != nullptr) {
        ops.setCostScale(templ->baseItem, static_cast<float>(call.floatArg(1, 1.0)));
    }
    return ActionCall::none();
}

// ACT_766 CreateItemOnFloor (00547f60)
Cells createItemOnFloor(ActionCall& call)
{
    ItemOps ops(call.session);
    ScriptApi api = call.api();
    const LocationData* location = api.sim().engine.get<LocationData>(call.engineArg(1));
    if (location == nullptr) {
        return invalidObject();
    }
    const std::uint32_t made = ops.makeItem(call.stringArg(0), 1, false);
    if (made == script::kObjectInvalid) {
        return invalidObject();
    }
    script::GameObject* item = ops.object(made);
    item->position = {location->position.x, location->position.y, location->position.z};
    setInt(*item, "item_floor", 1);
    return ActionCall::retObject(made);
}

// ACT_241 DestroyObject for items; everything else stays with the legacy path.
Cells destroyObject(ActionCall& call)
{
    ItemOps ops(call.session);
    const std::uint32_t object = call.objectArg(0);
    if (!ops.isItem(object)) {
        return std::nullopt;
    }
    ops.destroy(object);
    return ActionCall::none();
}

// ACT_438 GetSpellCastItem: the item a spell script was started by.
Cells getSpellCastItem(ActionCall& call)
{
    return ActionCall::retObject(call.api().contextObject("spellCastItem"));
}

} // namespace

void registerItemActions(HandlerTable& table)
{
    table.add(29, getItemPossessor);
    table.add(30, getItemPossessedBy);
    table.add(31, createItemOnObject);
    table.add(32, actionEquipItem);
    table.add(33, actionUnequipItem);
    table.add(34, actionPickUpItem);
    table.add(35, actionPutDownItem);
    table.add(52, getLastItemEquipped);
    table.add(135, actionGiveItem);
    table.add(136, actionTakeItem);
    table.add(138, getItemStackSize);
    table.add(150, setItemStackSize);
    table.add(155, getItemInSlot);
    table.add(241, destroyObject);
    table.add(266, setItemNonEquippable);
    table.add(271, giveItemAction);
    table.add(322, giveGoldToCreature);
    table.add(332, getIdentified);
    table.add(333, setIdentified);
    table.add(339, getFirstItemInInventory);
    table.add(340, getNextItemInInventory);
    table.add(378, openStore);
    table.add(397, getBaseItemType);
    table.add(398, getItemHasItemProperty);
    table.add(401, getItemACValue);
    table.add(418, getGold);
    table.add(438, getSpellCastItem);
    table.add(444, takeGoldFromCreature);
    table.add(475, getItemStackSize);
    table.add(570, getHasInventory);
    table.add(747, changeItemCost);
    table.add(766, createItemOnFloor);
}

// ---- the family gate: --family-test items --------------------------------------

namespace {

// A small NCS assembler so the gate runs real bytecode through the real VM.
class GateScript {
public:
    GateScript& constInt(std::int32_t value)
    {
        op(0x04, 0x03);
        be32(static_cast<std::uint32_t>(value));
        return *this;
    }
    GateScript& constString(const std::string& value)
    {
        op(0x04, 0x05);
        code_.push_back(static_cast<std::uint8_t>(value.size() >> 8));
        code_.push_back(static_cast<std::uint8_t>(value.size() & 0xFF));
        code_.insert(code_.end(), value.begin(), value.end());
        return *this;
    }
    GateScript& action(std::uint16_t id, std::uint8_t argc)
    {
        op(0x05, 0x00);
        code_.push_back(static_cast<std::uint8_t>(id >> 8));
        code_.push_back(static_cast<std::uint8_t>(id & 0xFF));
        code_.push_back(argc);
        return *this;
    }
    // GetObjectByTag(tag, 0)
    GateScript& byTag(const std::string& tag) { return constInt(0).constString(tag).action(200, 2); }
    std::vector<std::uint8_t> bytes()
    {
        op(0x20, 0x00);
        const std::string magic = "NCS V1.0";
        std::vector<std::uint8_t> out(magic.begin(), magic.end());
        out.push_back(0x42);
        const std::uint32_t size = static_cast<std::uint32_t>(13 + code_.size());
        for (const int shift : {24, 16, 8, 0}) {
            out.push_back(static_cast<std::uint8_t>(size >> shift));
        }
        out.insert(out.end(), code_.begin(), code_.end());
        return out;
    }

private:
    void op(std::uint8_t code, std::uint8_t qualifier)
    {
        code_.push_back(code);
        code_.push_back(qualifier);
    }
    void be32(std::uint32_t value)
    {
        for (const int shift : {24, 16, 8, 0}) {
            code_.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }
    std::vector<std::uint8_t> code_;
};

} // namespace

int runItemsFamilyTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("items family: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    script::Vm dummy({});
    const auto call = [&](std::uint16_t id, std::vector<std::vector<script::Value>> args) {
        const auto result = game.action(id, args, game.scriptWorld(), dummy);
        return result.has_value() && !result->empty() ? (*result)[0].integer : std::int64_t{0};
    };
    const auto objectOf = [](std::uint32_t id) {
        return std::vector<script::Value>{script::Value::makeObject(id)};
    };
    const auto hookCount = [&](const char* hook) {
        std::size_t count = 0;
        const std::string needle = std::string("hook ") + hook + " ->";
        for (const std::string& line : game.scriptLog()) {
            count += line.rfind(needle, 0) == 0 ? 1u : 0u;
        }
        return count;
    };
    const auto countIn = [&](const std::string& item) {
        std::size_t count = 0;
        for (const InventoryItem& row : game.inventory()) {
            count += lowerStr(row.resref) == lowerStr(item) ? 1u : 0u;
        }
        return count;
    };
    const std::uint32_t pc = game.scriptWorld().pc;
    const std::string startModule = game.module();
    constexpr std::size_t kMissing = static_cast<std::size_t>(-1);

    // ---- fixtures: a named NPC, a container with loot, and a plot item from the
    // shipped templates (a medpac when the module carries none).
    const std::size_t npcIndex = game.findEntity("end_trask");
    const std::size_t locker = game.findEntity("end_locker01");
    check(npcIndex != kMissing, "fixture_npc");
    check(locker != kMissing, "fixture_container");
    if (npcIndex == kMissing || locker == kMissing) {
        return 40;
    }
    const std::uint32_t npc = game.objectForEntityIndex(npcIndex);
    std::string resref = "g_i_medeqpmnt01";
    {
        ItemOps ops(game);
        bool found = false;
        for (const kotor::Entity& entity : game.scene().entities) {
            std::vector<std::string> candidates = entity.lootItems;
            for (const kotor::CreatureItem& carried : entity.creatureItems) {
                candidates.push_back(carried.resref);
            }
            for (const std::string& candidate : candidates) {
                const kotor::ItemTemplate* templ = ops.templateFor(candidate);
                if (templ != nullptr && templ->plot && !templ->tag.empty()) {
                    resref = candidate;
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        logInfo(concat("items family: fixture item ", resref,
                       found ? " (plot)" : " (no plot item in module)"));
    }
    std::string tag;
    {
        const kotor::ItemTemplate* templ = ItemOps(game).templateFor(resref);
        check(templ != nullptr && !templ->tag.empty(), "fixture_item_template");
        if (templ == nullptr) {
            return 41;
        }
        tag = templ->tag;
    }

    // ---- creation on an NPC leaves the PC's inventory alone.
    check(game.itemPossessedBy(npc, tag) == script::kObjectInvalid
              && game.itemPossessedBy(pc, tag) == script::kObjectInvalid,
          "not_possessed_before");
    const std::size_t pcRowsBefore = game.inventory().size();
    const std::size_t pcCountBefore = countIn(resref);
    game.scene().scripts["Mod_OnAcquirItem"] = "items_acquired";
    game.scene().scripts["Mod_OnUnAqreItem"] = "items_lost";
    game.addScriptForTest("items_acquired",
                          GateScript().constInt(1).constString("ITEMS_ACQ").action(581, 2).bytes());
    game.addScriptForTest("items_lost",
                          GateScript().constInt(1).constString("ITEMS_LOST").action(581, 2).bytes());
    const std::size_t acquiredBefore = hookCount("Mod_OnAcquirItem");
    const std::size_t lostBefore = hookCount("Mod_OnUnAqreItem");
    {
        // CreateItemOnObject(resref, GetObjectByTag("end_trask"), 1)
        const bool ran = game.runScriptNcsForTest(
            "items_create",
            GateScript().constInt(1).byTag("end_trask").constString(resref).action(31, 3).bytes(),
            pc);
        check(ran, "create_script_ran");
    }
    game.flushScriptEvents();
    check(game.inventory().size() == pcRowsBefore && countIn(resref) == pcCountBefore,
          "create_on_npc_leaves_pc_inventory");
    check(game.itemPossessedBy(npc, tag) != script::kObjectInvalid, "npc_holds_item");
    check(game.itemPossessedBy(pc, tag) == script::kObjectInvalid, "pc_does_not_hold_item");
    check(hookCount("Mod_OnAcquirItem") == acquiredBefore, "npc_gain_is_not_a_party_acquire");
    {
        // The VM-visible queries: GetItemPossessedBy and GetFirstItemInInventory.
        const std::int64_t found = call(30, {objectOf(npc), {script::Value::makeString(tag)}});
        check(found != 0 && found != script::kObjectInvalid
                  && static_cast<std::uint32_t>(found) == game.itemPossessedBy(npc, tag),
              "action_30_finds_tag");
        const std::int64_t first = call(339, {objectOf(npc)});
        check(first != 0 && first != script::kObjectInvalid, "action_339_lists_npc_inventory");
        check(call(30, {objectOf(pc), {script::Value::makeString(tag)}}) == script::kObjectInvalid,
              "action_30_misses_on_pc");
    }

    // ---- the PC takes it from the NPC: the acquire event fires, the row appears.
    {
        // ActionTakeItem(GetItemPossessedBy(npc, tag), npc) run by the PC.
        GateScript take;
        take.byTag("end_trask").constString(tag).byTag("end_trask").action(30, 2).action(136, 2);
        game.runScriptNcsForTest("items_take_pc", take.bytes(), pc);
        game.flushScriptEvents();
    }
    check(countIn(resref) == pcCountBefore + 1, "pc_inventory_gained_row");
    check(game.itemPossessedBy(pc, tag) != script::kObjectInvalid
              && game.itemPossessedBy(npc, tag) == script::kObjectInvalid,
          "possession_moved_to_pc");
    check(hookCount("Mod_OnAcquirItem") == acquiredBefore + 1, "acquire_hook_fired_once");
    check(game.scriptWorld().getGlobal("ITEMS_ACQ", script::ValueType::Int).asInt() == 1,
          "acquire_script_body_ran");

    // ---- the NPC takes it back from the PC: character_.inventory loses the row.
    {
        // ActionTakeItem(GetItemPossessedBy(GetFirstPC(), tag), GetFirstPC()) run by the NPC.
        GateScript take;
        take.action(548, 0).constString(tag).action(548, 0).action(30, 2).action(136, 2);
        game.runScriptNcsForTest("items_take_npc", take.bytes(), npc);
        game.flushScriptEvents();
    }
    check(countIn(resref) == pcCountBefore && game.inventory().size() == pcRowsBefore,
          "take_item_removes_pc_row");
    check(game.itemPossessedBy(pc, tag) == script::kObjectInvalid
              && game.itemPossessedBy(npc, tag) != script::kObjectInvalid,
          "possession_moved_to_npc");
    check(hookCount("Mod_OnUnAqreItem") == lostBefore + 1, "unacquire_hook_fired_once");
    check(game.scriptWorld().getGlobal("ITEMS_LOST", script::ValueType::Int).asInt() == 1,
          "unacquire_script_body_ran");

    // ---- container loot stays gone across leaving and returning to the module.
    const std::size_t lootBefore = game.scene().entities[locker].lootItems.size();
    check(lootBefore > 0, "container_has_loot");
    const std::uint32_t lockerObject = game.objectForEntityIndex(locker);
    if (lootBefore > 0) {
        game.setPositionForTest(game.scene().entities[locker].position);
        check(game.openLoot(locker), "container_opened");
        check(game.itemsOf(lockerObject).size() == lootBefore, "container_holds_item_objects");
        const std::string takenResref =
            game.loot().empty() ? std::string() : game.loot().front().resref;
        const std::size_t takenBefore = countIn(takenResref);
        check(game.takeLootItem(0), "container_item_taken");
        game.flushScriptEvents();
        check(countIn(takenResref) == takenBefore + 1, "taken_item_in_pc_inventory");
        check(game.scene().entities[locker].lootItems.size() == lootBefore - 1
                  && game.itemsOf(lockerObject).size() == lootBefore - 1,
              "container_lost_the_item");
        game.closeLoot();
    }
    const auto travel = [&](const std::string& module) {
        game.runScriptNcsForTest("items_travel",
                                 GateScript()
                                     .constString("").constString("").constString("")
                                     .constString("").constString("").constString("")
                                     .constString("").constString(module)
                                     .action(509, 8)
                                     .bytes(),
                                 pc);
        game.update(1.0f / 30.0f, InputState{});
    };

    // ---- a store opened by a real script call, in the Taris module that has stores.
    travel("tar_m02ac");
    check(game.module() == "tar_m02ac", "store_module_loaded");
    if (game.module() == "tar_m02ac" && !game.merchants().empty()) {
        std::size_t first = kMissing;
        std::size_t second = kMissing;
        for (std::size_t i = 0; i < game.merchants().size(); ++i) {
            const MerchantState& merchant = game.merchants()[i];
            if (merchant.entity >= game.scene().entities.size()
                || game.scene().entities[merchant.entity].tag.empty()) {
                continue;
            }
            bool priced = false;
            for (const MerchantStockItem& stock : merchant.stock) {
                priced = priced || stock.item.cost >= 100;
            }
            if (!priced) {
                continue;
            }
            (first == kMissing ? first : second) = i;
            if (second != kMissing) {
                break;
            }
        }
        check(first != kMissing, "fixture_store");
        const auto openByScript = [&](std::size_t merchantIndex, int markUp, int markDown) {
            const std::string storeTag =
                game.scene().entities[game.merchants()[merchantIndex].entity].tag;
            // OpenStore(GetObjectByTag(store), GetFirstPC(), markUp, markDown)
            GateScript open;
            open.constInt(markDown).constInt(markUp).action(548, 0).byTag(storeTag).action(378, 4);
            return game.runScriptNcsForTest("items_open_store", open.bytes(), pc);
        };
        if (first != kMissing) {
            const MerchantState merchant = game.merchants()[first];
            std::size_t stockIndex = 0;
            while (merchant.stock[stockIndex].item.cost < 100) {
                ++stockIndex;
            }
            const std::int32_t cost = merchant.stock[stockIndex].item.cost;
            const auto percent = [](std::int32_t base, std::int32_t mark) {
                return std::max<std::int32_t>(1, base * mark / 100);
            };
            check(game.activeMerchant() < 0, "no_store_open_before");
            check(openByScript(first, 0, 0), "open_store_script_ran");
            check(game.activeMerchant() == static_cast<int>(first), "script_opened_the_right_store");
            check(game.activePanel() == UiPanel::Merchant, "store_panel_open");
            const std::int32_t plain = game.buyPrice(stockIndex);
            check(plain == percent(cost, merchant.markUp), "plain_buy_price");
            game.closeStore();
            check(openByScript(first, 25, 0) && game.activeMerchant() == static_cast<int>(first),
                  "reopened_with_bonus");
            check(game.buyPrice(stockIndex) == percent(cost, merchant.markUp + 25)
                      && game.buyPrice(stockIndex) > plain,
                  "markup_bonus_applied");
            game.closeStore();
            check(openByScript(first, 5000, 0)
                      && game.buyPrice(stockIndex) == percent(cost, merchant.markUp + 100),
                  "markup_bonus_clamped_to_100");
            game.closeStore();
            // The sell side takes the markdown bonus.
            check(game.giveItem(resref, pc) != script::kObjectInvalid, "item_given_to_sell");
            const std::size_t row = game.inventory().size() - 1;
            const std::int32_t rowCost = game.inventory()[row].cost;
            check(openByScript(first, 0, 0), "reopened_for_sell");
            const std::int32_t plainSell = game.sellPrice(row);
            game.closeStore();
            check(openByScript(first, 0, 20), "reopened_with_markdown");
            check(game.sellPrice(row) == percent(rowCost, merchant.markDown + 20)
                      && game.sellPrice(row) >= plainSell,
                  "markdown_bonus_applied");
            game.closeStore();
            if (second != kMissing) {
                check(openByScript(second, 0, 0) && game.activeMerchant() == static_cast<int>(second),
                      "script_opens_a_different_store");
                game.closeStore();
            }
        }
    }

    // ---- back in the first module the looted item is still gone.
    travel(startModule);
    check(game.module() == startModule, "returned_to_module");
    {
        const std::size_t again = game.findEntity("end_locker01");
        check(again != kMissing, "container_present_after_return");
        if (again != kMissing && lootBefore > 0) {
            check(game.scene().entities[again].lootItems.size() == lootBefore - 1
                      && game.itemsOf(game.objectForEntityIndex(again)).size() == lootBefore - 1,
                  "container_loot_stays_gone");
        }
    }

    logInfo(ok ? "items family: PASS" : "items family: FAIL");
    return ok ? 0 : 42;
}

} // namespace ke
