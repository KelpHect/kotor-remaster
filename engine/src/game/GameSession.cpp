#include "game/GameSession.h"

#include "core/Log.h"
#include "kotor/SaveGame.h"
#include "script/Ncs.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace ke {

const char* uiPanelName(UiPanel panel)
{
    switch (panel) {
    case UiPanel::Character: return "character";
    case UiPanel::Inventory: return "inventory";
    case UiPanel::Party: return "party";
    case UiPanel::Journal: return "journal";
    case UiPanel::Merchant: return "merchant";
    case UiPanel::Loot: return "loot";
    case UiPanel::None: break;
    }
    return "none";
}

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEyeHeight = 1.55f;
constexpr float kPlayerRadius = 0.28f;
constexpr float kStepHeight = 0.55f;
constexpr float kGravity = 18.0f;

float clamp(float value, float low, float high)
{
    return std::max(low, std::min(high, value));
}

float distance2D(kotor::Vec3 a, kotor::Vec3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

// Barycentric floor height at (x, y); the highest triangle below maxZ wins.
bool floorHeight(const kotor::Scene& scene, float x, float y, float maxZ, float& out)
{
    bool found = false;
    float best = -1.0e30f;
    for (const kotor::CollisionFacet& facet : scene.collision) {
        if (!facet.floor) {
            continue;
        }
        const kotor::Vec3& a = scene.collisionVertices[facet.a];
        const kotor::Vec3& b = scene.collisionVertices[facet.b];
        const kotor::Vec3& c = scene.collisionVertices[facet.c];
        const float den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::fabs(den) < 1.0e-8f) {
            continue;
        }
        const float u = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / den;
        const float v = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / den;
        const float w = 1.0f - u - v;
        if (u < -0.001f || v < -0.001f || w < -0.001f) {
            continue;
        }
        const float z = u * a.z + v * b.z + w * c.z;
        if (z <= maxZ + kStepHeight && z > best) {
            best = z;
            found = true;
        }
    }
    if (found) {
        out = best;
    }
    return found;
}

bool segmentIntersects(const kotor::Vec3& a, const kotor::Vec3& b,
                       const kotor::Vec3& c, const kotor::Vec3& d)
{
    const float rX = b.x - a.x;
    const float rY = b.y - a.y;
    const float sX = d.x - c.x;
    const float sY = d.y - c.y;
    const float den = rX * sY - rY * sX;
    if (std::fabs(den) < 1.0e-8f) {
        return false;
    }
    const float qpx = c.x - a.x;
    const float qpy = c.y - a.y;
    const float t = (qpx * sY - qpy * sX) / den;
    const float u = (qpx * rY - qpy * rX) / den;
    return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
}

bool wallBlocked(const kotor::Scene& scene, kotor::Vec3 from, kotor::Vec3 to, float radius)
{
    // A small square around the path approximates the player's radius. It is
    // deliberately conservative: the first slice must not tunnel through a
    // wall, and sliding is handled by trying the two axes separately.
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    const float nx = len > 1.0e-6f ? -dy / len : 0.0f;
    const float ny = len > 1.0e-6f ? dx / len : 0.0f;
    const kotor::Vec3 offsets[2] = {{nx * radius, ny * radius, 0.0f}, {-nx * radius, -ny * radius, 0.0f}};
    for (const kotor::CollisionFacet& facet : scene.collision) {
        if (facet.floor) {
            continue;
        }
        const kotor::Vec3& a = scene.collisionVertices[facet.a];
        const kotor::Vec3& b = scene.collisionVertices[facet.b];
        const kotor::Vec3& c = scene.collisionVertices[facet.c];
        for (const kotor::Vec3& offset : offsets) {
            const kotor::Vec3 p0{from.x + offset.x, from.y + offset.y, from.z};
            const kotor::Vec3 p1{to.x + offset.x, to.y + offset.y, to.z};
            if (segmentIntersects(p0, p1, a, b) || segmentIntersects(p0, p1, b, c)
                || segmentIntersects(p0, p1, c, a)) {
                return true;
            }
        }
    }
    return false;
}

bool pointInTriangle2D(float x, float y, const kotor::Vec3& a, const kotor::Vec3& b, const kotor::Vec3& c)
{
    const float d1 = (x - b.x) * (a.y - b.y) - (a.x - b.x) * (y - b.y);
    const float d2 = (x - c.x) * (b.y - c.y) - (b.x - c.x) * (y - c.y);
    const float d3 = (x - a.x) * (c.y - a.y) - (c.x - a.x) * (y - a.y);
    const bool hasNegative = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
    const bool hasPositive = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
    return !(hasNegative && hasPositive);
}

std::vector<script::Value> intCells(std::int64_t value)
{
    return {script::Value::makeInt(value)};
}

std::vector<script::Value> objectCells(std::uint32_t value)
{
    return {script::Value::makeObject(value)};
}

std::vector<script::Value> floatCells(double value)
{
    return {script::Value::makeFloat(value)};
}

} // namespace

GameSession::GameSession(std::string installPath, std::string module)
    : installPath_(std::move(installPath))
    , install_(installPath_)
    , rules_(install_)
{
    const std::filesystem::path tlkPath = std::filesystem::path(installPath_) / "dialog.tlk";
    std::ifstream tlkFile(tlkPath, std::ios::binary | std::ios::ate);
    if (tlkFile) {
        const std::streamoff size = tlkFile.tellg();
        tlkFile.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (bytes.empty() || tlkFile.read(reinterpret_cast<char*>(bytes.data()), size)) {
            try {
                tlk_ = kotor::Tlk::parse(std::move(bytes), "dialog.tlk");
            } catch (const kotor::FormatError&) {
                tlk_.reset();
            }
        }
    }
    if (rules_.loaded()) {
        character_ = rules_.createCharacter(0, 1);
    } else {
        character_.str = 14;
        character_.dex = 12;
        character_.con = 12;
        character_.intel = 10;
        character_.wis = 10;
        character_.cha = 10;
        character_.maxHp = 30;
        character_.hp = 30;
    }
    playerMaxHp_ = character_.maxHp;
    playerHp_ = character_.hp;
    gold_ = character_.gold;
    if (!loadModule(module, nullptr, {})) {
        throw kotor::FormatError("cannot load module " + module);
    }
    runtime_ = std::make_unique<script::ActionRuntime>(&scriptWorld_, this);
    initScriptWorld();
    syncPlayerToScriptWorld();
}

GameSession::~GameSession() = default;

bool GameSession::loadModule(const std::string& module, const kotor::Vec3* spawn, const std::string& linkedDoor)
{
    kotor::Scene loaded = kotor::loadModule(install_, module);
    module_ = module;
    scene_ = std::move(loaded);
    if (spawn != nullptr) {
        position_ = *spawn;
        yaw_ = std::atan2(scene_.entryDirection.y, scene_.entryDirection.x);
    } else {
        position_ = scene_.entry;
        yaw_ = std::atan2(scene_.entryDirection.y, scene_.entryDirection.x);
    }
    if (!linkedDoor.empty()) {
        for (const kotor::Entity& entity : scene_.entities) {
            if (entity.kind == kotor::EntityKind::Door && entity.tag == linkedDoor) {
                position_ = entity.position;
                position_.x += std::cos(entity.bearing) * 0.9f;
                position_.y += std::sin(entity.bearing) * 0.9f;
                yaw_ = entity.bearing + kPi;
                break;
            }
        }
    }
    float ground = 0.0f;
    if (floorHeight(scene_, position_.x, position_.y, position_.z + 2.0f, ground)) {
        position_.z = ground;
    }
    verticalVelocity_ = 0.0f;
    grounded_ = true;
    activeDialogue_.reset();
    dialogueView_ = {};
    combatTarget_ = UINT32_MAX;
    playerAttackTimer_ = 0.0f;
    npcAttackTimers_.assign(scene_.entities.size(), 0.0f);
    triggerInside_.assign(scene_.triggers.size(), false);
    if (playerHp_ <= 0) {
        playerHp_ = playerMaxHp_;
        playerDead_ = false;
    }
    if (runtime_) {
        initScriptWorld();
        syncPlayerToScriptWorld();
    }
    return true;
}

float GameSession::floorHeightAt(float x, float y) const
{
    float ground = 0.0f;
    if (floorHeight(scene_, x, y, position_.z + 2.0f, ground)) {
        return ground;
    }
    return position_.z;
}

std::size_t GameSession::findEntity(const std::string& tag) const
{
    const std::string wanted = lower(tag);
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        if (lower(scene_.entities[i].tag) == wanted) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

std::uint32_t GameSession::random()
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

std::uint32_t GameSession::objectForEntity(std::size_t index) const
{
    if (index >= entityObjects_.size()) {
        return script::kObjectInvalid;
    }
    return entityObjects_[index];
}

std::uint32_t GameSession::objectForTag(const std::string& tag) const
{
    return scriptWorld_.byTag(tag, 0);
}

bool GameSession::runHookForTest(const std::string& hook, const std::string& tag,
                                 std::uint32_t entering)
{
    const std::size_t index = findEntity(tag);
    if (index == static_cast<std::size_t>(-1)) {
        return false;
    }
    const auto it = scene_.entities[index].scripts.find(hook);
    if (it == scene_.entities[index].scripts.end() || it->second.empty()) {
        return false;
    }
    runHook(hook, index, entering == 0 ? scriptWorld_.pc : entering);
    return true;
}

bool GameSession::scriptValueTruthy(const std::optional<script::Value>& value) const
{
    return value.has_value() && value->truthy();
}

std::string GameSession::scriptSource(const std::string& resref)
{
    if (resref.empty()) {
        return {};
    }
    const auto cached = ncsCache_.find(resref);
    if (cached != ncsCache_.end()) {
        return std::string(cached->second.begin(), cached->second.end());
    }
    auto data = install_.find(resref, 2010);
    if (!data.has_value()) {
        if (missingScripts_.insert(resref).second) {
            scriptLog_.push_back("missing script " + resref);
        }
        return {};
    }
    ncsCache_.emplace(resref, *data);
    return std::string(data->begin(), data->end());
}

std::optional<script::Value> GameSession::runScript(const std::string& resref,
                                                    std::uint32_t self,
                                                    std::uint32_t entering,
                                                    std::uint32_t caller,
                                                    std::uint32_t trigger)
{
    if (runtime_ == nullptr || resref.empty()) {
        return std::nullopt;
    }
    const std::string bytes = scriptSource(resref);
    if (bytes.empty()) {
        return std::nullopt;
    }
    std::vector<script::Instruction> instructions;
    try {
        instructions = script::decodeNcs(
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    } catch (const script::NcsError& error) {
        scriptLog_.push_back("decode " + resref + ": " + error.what());
        return std::nullopt;
    }
    runtime_->setContextObject("self", self);
    runtime_->setContextObject("entering", entering);
    runtime_->setContextObject("exiting", entering);
    runtime_->setContextObject("caller", caller);
    runtime_->setContextObject("trigger", trigger);
    runtime_->setContextObject("speaker", self);
    runtime_->setContextObject("lastSpeaker", self);
    runtime_->setContextObject("lastUsedBy", caller);
    runtime_->setContextObject("lastOpenedBy", caller);
    runtime_->setContextObject("lastAttacker", trigger);
    runtime_->setContextObject("lastDamager", trigger);
    runtime_->setContextObject("lastKiller", trigger);
    script::Vm vm(std::move(instructions), runtime_.get(), 200000);
    const script::VmResult result = vm.run();
    std::ostringstream line;
    line << "script " << resref << " status=" << script::statusName(result.status)
         << " steps=" << result.steps << " actions=";
    std::size_t count = 0;
    for (const auto& [id, hits] : result.actions) {
        if (count++ != 0) {
            line << ",";
        }
        line << id << ":" << hits;
    }
    if (result.top.has_value()) {
        line << " top=" << result.top->asInt();
    }
    if (!result.error.empty()) {
        line << " error=" << result.error;
    }
    scriptLog_.push_back(line.str());
    if (scriptLog_.size() > 8192) {
        scriptLog_.erase(scriptLog_.begin(), scriptLog_.begin() + 4096);
    }
    syncScriptWorld();
    if (result.top.has_value()) {
        return result.top;
    }
    return std::nullopt;
}

void GameSession::syncPlayerToScriptWorld()
{
    script::GameObject* pc = scriptWorld_.get(scriptWorld_.pc);
    if (pc == nullptr) {
        return;
    }
    pc->position = {position_.x, position_.y, position_.z};
    pc->hp = playerHp_;
    pc->attrs["level"] = static_cast<std::int64_t>(character_.level);
    pc->attrs["xp"] = static_cast<std::int64_t>(character_.xp);
    pc->attrs["gold"] = static_cast<std::int64_t>(gold_);
    pc->attrs["class_id"] = static_cast<std::int64_t>(character_.classId);
    pc->attrs["ac"] = static_cast<std::int64_t>(rules_.armorClass(character_));
    pc->attrs["good_evil"] = static_cast<std::int64_t>(50);
}

void GameSession::syncEntityToScriptWorld(std::size_t index)
{
    if (index >= scene_.entities.size() || index >= entityObjects_.size()) {
        return;
    }
    script::GameObject* object = scriptWorld_.get(entityObjects_[index]);
    if (object == nullptr) {
        return;
    }
    const kotor::Entity& entity = scene_.entities[index];
    object->position = {entity.position.x, entity.position.y, entity.position.z};
    object->hp = entity.hitPoints;
    object->valid = !entity.dead;
    object->tag = entity.tag.empty() ? object->tag : entity.tag;
}

void GameSession::syncScriptWorld()
{
    syncPlayerToScriptWorld();
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        syncEntityToScriptWorld(i);
    }
}

void GameSession::initMerchants()
{
    merchants_.clear();
    activeMerchant_ = -1;
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        if (entity.kind != kotor::EntityKind::Store) {
            continue;
        }
        MerchantState merchant;
        merchant.entity = i;
        merchant.resref = entity.templateResref;
        merchant.name = entity.name.empty() ? entity.tag : entity.name;
        merchant.markUp = entity.storeMarkUp;
        merchant.markDown = entity.storeMarkDown;
        merchant.buySellFlag = entity.storeBuySell;
        for (const auto& [resref, infinite] : entity.storeItems) {
            MerchantStockItem stock;
            stock.item.resref = resref;
            stock.infinite = infinite;
            stock.remaining = infinite ? -1 : 1;
            auto cached = itemTemplates_.find(resref);
            if (cached == itemTemplates_.end()) {
                if (auto item = kotor::loadItemTemplate(install_, resref); item.has_value()) {
                    cached = itemTemplates_.emplace(resref, *item).first;
                }
            }
            if (cached != itemTemplates_.end()) {
                stock.item.baseItem = cached->second.baseItem;
                stock.item.cost = cached->second.cost;
                stock.item.stack = cached->second.stackSize;
                stock.item.identified = cached->second.identified;
            }
            if (stock.item.cost <= 0 && rules_.loaded()) {
                stock.item.cost = rules_.itemCost(stock.item.baseItem);
            }
            merchant.stock.push_back(std::move(stock));
        }
        std::int64_t stockValue = 0;
        for (const MerchantStockItem& stock : merchant.stock) {
            stockValue += static_cast<std::int64_t>(stock.item.cost) * std::max(1, stock.item.stack);
        }
        merchant.gold = static_cast<std::int32_t>(std::max<std::int64_t>(1000, stockValue / 2));
        merchants_.push_back(std::move(merchant));
    }
}

bool GameSession::openStore(std::size_t entityIndex)
{
    if (entityIndex >= scene_.entities.size()
        || scene_.entities[entityIndex].kind != kotor::EntityKind::Store) {
        return false;
    }
    for (std::size_t i = 0; i < merchants_.size(); ++i) {
        if (merchants_[i].entity == entityIndex) {
            activeMerchant_ = static_cast<int>(i);
            runHook("OnOpenStore", entityIndex, scriptWorld_.pc);
            logEvent("store " + merchants_[i].resref + " opened");
            return true;
        }
    }
    return false;
}

bool GameSession::openStoreByTag(const std::string& tag)
{
    const std::size_t index = findEntity(tag);
    return index != static_cast<std::size_t>(-1) && openStore(index);
}

std::int32_t GameSession::buyPrice(std::size_t stockIndex) const
{
    if (activeMerchant_ < 0 || static_cast<std::size_t>(activeMerchant_) >= merchants_.size()) {
        return 0;
    }
    const MerchantState& merchant = merchants_[static_cast<std::size_t>(activeMerchant_)];
    if (stockIndex >= merchant.stock.size()) {
        return 0;
    }
    return std::max<std::int32_t>(1, merchant.stock[stockIndex].item.cost
                                        * std::max<std::int32_t>(1, merchant.markUp) / 100);
}

std::int32_t GameSession::sellPrice(std::size_t inventoryIndex) const
{
    if (activeMerchant_ < 0 || static_cast<std::size_t>(activeMerchant_) >= merchants_.size()
        || inventoryIndex >= character_.inventory.size()) {
        return 0;
    }
    const MerchantState& merchant = merchants_[static_cast<std::size_t>(activeMerchant_)];
    return std::max<std::int32_t>(1, character_.inventory[inventoryIndex].cost
                                        * std::max<std::int32_t>(1, merchant.markDown) / 100);
}

bool GameSession::buyMerchantItem(std::size_t stockIndex)
{
    if (activeMerchant_ < 0) {
        return false;
    }
    MerchantState& merchant = merchants_[static_cast<std::size_t>(activeMerchant_)];
    if (stockIndex >= merchant.stock.size() || (merchant.buySellFlag & 1) == 0) {
        return false;
    }
    MerchantStockItem& stock = merchant.stock[stockIndex];
    if (!stock.infinite && stock.remaining <= 0) {
        return false;
    }
    const std::int32_t price = buyPrice(stockIndex);
    if (price <= 0 || gold_ < price) {
        return false;
    }
    if (!addInventoryItem(stock.item.resref, stock.item.baseItem, stock.item.stack,
                          stock.item.cost)) {
        return false;
    }
    gold_ -= price;
    merchant.gold += price;
    if (!stock.infinite) {
        --stock.remaining;
    }
    logEvent("buy " + stock.item.resref + " for " + std::to_string(price));
    return true;
}

bool GameSession::sellInventoryItem(std::size_t inventoryIndex)
{
    if (activeMerchant_ < 0 || inventoryIndex >= character_.inventory.size()) {
        return false;
    }
    MerchantState& merchant = merchants_[static_cast<std::size_t>(activeMerchant_)];
    if ((merchant.buySellFlag & 2) == 0) {
        return false;
    }
    InventoryItem item = character_.inventory[inventoryIndex];
    const std::int32_t price = sellPrice(inventoryIndex);
    if (merchant.gold < price) {
        return false;
    }
    if (!removeInventoryItem(item.resref, item.stack)) {
        return false;
    }
    gold_ += price;
    merchant.gold -= price;
    logEvent("sell " + item.resref + " for " + std::to_string(price));
    return true;
}

bool GameSession::identifyInventoryItem(std::size_t inventoryIndex)
{
    if (inventoryIndex >= character_.inventory.size()) {
        return false;
    }
    InventoryItem& item = character_.inventory[inventoryIndex];
    if (item.identified) {
        return false;
    }
    const std::int32_t price = std::max<std::int32_t>(1, item.cost / 2);
    if (gold_ < price) {
        return false;
    }
    gold_ -= price;
    item.identified = true;
    logEvent("identify " + item.resref + " for " + std::to_string(price));
    return true;
}

bool GameSession::openLoot(std::size_t entityIndex)
{
    if (entityIndex >= scene_.entities.size()
        || scene_.entities[entityIndex].kind != kotor::EntityKind::Placeable
        || scene_.entities[entityIndex].lootItems.empty()) {
        return false;
    }
    lootItems_.clear();
    lootEntity_ = static_cast<int>(entityIndex);
    for (const std::string& resref : scene_.entities[entityIndex].lootItems) {
        InventoryItem item;
        item.resref = resref;
        auto cached = itemTemplates_.find(resref);
        if (cached == itemTemplates_.end()) {
            if (auto templ = kotor::loadItemTemplate(install_, resref); templ.has_value()) {
                cached = itemTemplates_.emplace(resref, *templ).first;
            }
        }
        if (cached != itemTemplates_.end()) {
            item.name = cached->second.name;
            item.baseItem = cached->second.baseItem;
            item.cost = cached->second.cost;
            item.stack = cached->second.stackSize;
            item.identified = cached->second.identified;
        }
        lootItems_.push_back(std::move(item));
    }
    if (lootItems_.empty()) {
        lootEntity_ = -1;
        return false;
    }
    setUiPanel(UiPanel::Loot);
    logEvent("loot " + scene_.entities[entityIndex].tag + " x"
             + std::to_string(lootItems_.size()));
    return true;
}

bool GameSession::takeLootItem(std::size_t index)
{
    if (lootEntity_ < 0 || index >= lootItems_.size()
        || static_cast<std::size_t>(lootEntity_) >= scene_.entities.size()) {
        return false;
    }
    const InventoryItem item = lootItems_[index];
    if (!addInventoryItem(item.resref, item.baseItem, item.stack, item.cost)) {
        return false;
    }
    kotor::Entity& entity = scene_.entities[static_cast<std::size_t>(lootEntity_)];
    for (auto it = entity.lootItems.begin(); it != entity.lootItems.end(); ++it) {
        if (lower(*it) == lower(item.resref)) {
            entity.lootItems.erase(it);
            break;
        }
    }
    lootItems_.erase(lootItems_.begin() + static_cast<std::ptrdiff_t>(index));
    logEvent("take " + item.resref);
    if (lootItems_.empty()) {
        lootEntity_ = -1;
        setUiPanel(UiPanel::None);
    }
    return true;
}

void GameSession::takeAllLoot()
{
    while (!lootItems_.empty()) {
        if (!takeLootItem(0)) {
            break;
        }
    }
}

void GameSession::spawnEncounters()
{
    encounterStates_.assign(scene_.encounters.size(), EncounterState{});
    for (std::size_t encounterIndex = 0; encounterIndex < scene_.encounters.size();
         ++encounterIndex) {
        const kotor::EncounterVolume& volume = scene_.encounters[encounterIndex];
        EncounterState& state = encounterStates_[encounterIndex];
        state.respawnsRemaining = volume.templateData.respawns < 0
            ? -1
            : volume.templateData.respawns;
        state.resetTime = static_cast<float>(std::max(0, volume.templateData.resetTime));
        const std::int32_t spawned = spawnEncounterCreatures(encounterIndex);
        if (spawned > 0) {
            runHook("OnEntered", volume.entity, scriptWorld_.pc);
            logEvent("encounter " + volume.templateData.resref + " spawned "
                     + std::to_string(spawned));
        }
    }
}

std::int32_t GameSession::spawnEncounterCreatures(std::size_t encounterIndex)
{
    if (encounterIndex >= scene_.encounters.size()
        || encounterIndex >= encounterStates_.size()) {
        return 0;
    }
    const kotor::EncounterVolume& volume = scene_.encounters[encounterIndex];
    const kotor::EncounterTemplate& encounter = volume.templateData;
    EncounterState& state = encounterStates_[encounterIndex];
    if (!encounter.active || encounter.creatures.empty() || volume.spawnPoints.empty()) {
        return 0;
    }
    std::int32_t count = encounter.recCreatures > 0 ? encounter.recCreatures
                                                    : encounter.maxCreatures;
    if (count <= 0) {
        count = static_cast<std::int32_t>(encounter.creatures.size());
    }
    if (encounter.maxCreatures > 0) {
        count = std::min(count, encounter.maxCreatures);
    }
    std::set<std::string> singleSpawnUsed;
    std::int32_t spawnedCount = 0;
    for (std::int32_t i = 0; i < count; ++i) {
        const kotor::EncounterCreature& creature =
            encounter.creatures[static_cast<std::size_t>(i) % encounter.creatures.size()];
        if (creature.singleSpawn && singleSpawnUsed.contains(creature.resref)) {
            continue;
        }
        const kotor::EncounterSpawnPoint& point =
            volume.spawnPoints[static_cast<std::size_t>(i) % volume.spawnPoints.size()];
        const std::size_t entityIndex = kotor::spawnCreature(
            scene_, install_, creature.resref, point.position, point.bearing);
        if (entityIndex == static_cast<std::size_t>(-1)) {
            continue;
        }
        const kotor::Entity& spawned = scene_.entities[entityIndex];
        scene_.entities[entityIndex].hostile = true;
        script::GameObject& object =
            scriptWorld_.addObject(spawned.tag, 100, 0, true);
        object.position = {spawned.position.x, spawned.position.y, spawned.position.z};
        object.hp = spawned.hitPoints;
        object.attrs["ac"] = static_cast<std::int64_t>(spawned.defense);
        object.attrs["fortitude"] = static_cast<std::int64_t>(0);
        object.attrs["will"] = static_cast<std::int64_t>(0);
        object.attrs["reflex"] = static_cast<std::int64_t>(0);
        object.attrs["challenge_rating"] =
            static_cast<std::int64_t>(creature.challengeRating);
        object.attrs["movement_rate"] = static_cast<std::int64_t>(1);
        object.attrs["good_evil"] = static_cast<std::int64_t>(50);
        object.attrs["level"] = static_cast<std::int64_t>(
            std::max(1, static_cast<int>(creature.challengeRating)));
        object.attrs["class_id"] = static_cast<std::int64_t>(0);
        object.attrs["xp"] = static_cast<std::int64_t>(0);
        entityObjects_.push_back(object.id);
        if (creature.singleSpawn) {
            singleSpawnUsed.insert(creature.resref);
        }
        state.entities.push_back(entityIndex);
        ++spawnedCount;
        runHook("ScriptSpawn", entityIndex, scriptWorld_.pc);
    }
    state.spawnedCount += spawnedCount;
    state.spawned = state.spawnedCount > 0;
    state.resetTimer = 0.0f;
    if (spawnedCount > 0) {
        state.exhausted = false;
    }
    return spawnedCount;
}

void GameSession::updateEncounters(float dt)
{
    for (std::size_t i = 0; i < encounterStates_.size(); ++i) {
        EncounterState& state = encounterStates_[i];
        if (!state.spawned || state.exhausted || state.resetTime <= 0.0f
            || state.respawnsRemaining == 0) {
            continue;
        }
        bool anyAlive = false;
        for (const std::size_t entity : state.entities) {
            if (entity < scene_.entities.size() && !scene_.entities[entity].dead) {
                anyAlive = true;
                break;
            }
        }
        if (anyAlive) {
            state.resetTimer = 0.0f;
            continue;
        }
        state.resetTimer += dt;
        if (state.resetTimer < state.resetTime) {
            continue;
        }
        if (state.respawnsRemaining > 0) {
            --state.respawnsRemaining;
        }
        const std::int32_t spawned = spawnEncounterCreatures(i);
        if (spawned == 0) {
            state.exhausted = true;
        } else {
            runHook("OnEntered", scene_.encounters[i].entity, scriptWorld_.pc);
            logEvent("encounter " + scene_.encounters[i].templateData.resref
                     + " respawned " + std::to_string(spawned));
        }
    }
}

void GameSession::updateParty(float dt)
{
    if (companions_.empty() || playerDead_) {
        return;
    }
    kotor::Vec3 anchor = position_;
    float anchorYaw = yaw_;
    if (leaderTag_ != "PC") {
        const std::size_t leader = findEntity(leaderTag_);
        if (leader != static_cast<std::size_t>(-1)) {
            anchor = scene_.entities[leader].position;
            anchorYaw = scene_.entities[leader].bearing;
        }
    }
    const kotor::Vec3 forward{std::cos(anchorYaw), std::sin(anchorYaw), 0.0f};
    const kotor::Vec3 right{-forward.y, forward.x, 0.0f};
    int slot = 0;
    for (CompanionState& companion : companions_) {
        const std::size_t index = findEntity(companion.tag);
        if (index == static_cast<std::size_t>(-1) || scene_.entities[index].dead) {
            continue;
        }
        if (companion.tag == leaderTag_) {
            continue;
        }
        kotor::Entity& entity = scene_.entities[index];
        companion.attackTimer = std::max(0.0f, companion.attackTimer - dt);
        const bool hasTarget = combatTarget_ != UINT32_MAX
            && combatTarget_ < scene_.entities.size()
            && scene_.entities[combatTarget_].hostile && !scene_.entities[combatTarget_].dead;
        if (hasTarget) {
            kotor::Entity& target = scene_.entities[combatTarget_];
            const float tdx = target.position.x - entity.position.x;
            const float tdy = target.position.y - entity.position.y;
            const float targetDistance = std::sqrt(tdx * tdx + tdy * tdy);
            if (targetDistance > 2.2f) {
                const float step = std::min(targetDistance - 2.0f, 3.2f * dt);
                kotor::Vec3 next{entity.position.x + tdx / targetDistance * step,
                                 entity.position.y + tdy / targetDistance * step,
                                 entity.position.z};
                if (!wallBlocked(scene_, entity.position, next, 0.25f)) {
                    entity.position.x = next.x;
                    entity.position.y = next.y;
                    entity.bearing = std::atan2(tdy, tdx);
                    float ground = entity.position.z;
                    if (floorHeight(scene_, entity.position.x, entity.position.y,
                                    entity.position.z + 2.0f, ground)) {
                        entity.position.z = ground;
                    }
                    syncEntityToScriptWorld(index);
                }
            } else if (companion.attackTimer <= 0.0f) {
                const AttackResult result = rules_.resolveAttack(
                    companion.sheet, target.defense, rng_, 1, 8,
                    rules_.damageBonus(companion.sheet));
                companion.attackTimer = 2.0f;
                if (result.hit) {
                    target.hitPoints -= result.damage;
                    logEvent(companion.tag + " hits "
                             + (target.name.empty() ? target.tag : target.name)
                             + " for " + std::to_string(result.damage));
                    if (target.hitPoints <= 0) {
                        target.dead = true;
                        logEvent(companion.tag + " defeats "
                                 + (target.name.empty() ? target.tag : target.name));
                        combatTarget_ = UINT32_MAX;
                    }
                }
            }
            ++slot;
            continue;
        }
        const float row = static_cast<float>(slot / 2);
        const float side = (slot % 2 == 0) ? -1.2f : 1.2f;
        const kotor::Vec3 target = anchor - forward * (1.6f + row * 1.1f) + right * side;
        const float dx = target.x - entity.position.x;
        const float dy = target.y - entity.position.y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance > 0.35f) {
            const float step = std::min(distance, 3.2f * dt);
            kotor::Vec3 next{entity.position.x + dx / distance * step,
                             entity.position.y + dy / distance * step, entity.position.z};
            if (!wallBlocked(scene_, entity.position, next, 0.25f)) {
                entity.position.x = next.x;
                entity.position.y = next.y;
                entity.bearing = std::atan2(dy, dx);
                float ground = entity.position.z;
                if (floorHeight(scene_, entity.position.x, entity.position.y,
                                entity.position.z + 2.0f, ground)) {
                    entity.position.z = ground;
                }
                syncEntityToScriptWorld(index);
            }
        }
        ++slot;
    }
}

void GameSession::initScriptWorld()
{
    scriptWorld_.clearObjects();
    scriptWorld_.attrs["module"] = module_;
    scriptWorld_.attrs["difficulty"] = static_cast<std::int64_t>(1);
    entityObjects_.assign(scene_.entities.size(), 0);
    spawnRan_.clear();
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        std::string tag = entity.tag;
        if (tag.empty()) {
            tag = "entity_" + std::to_string(i);
        }
        script::GameObject& object = scriptWorld_.addObject(tag, 100, 0, !entity.dead);
        object.position = {entity.position.x, entity.position.y, entity.position.z};
        object.hp = entity.hitPoints;
        object.attrs["ac"] = static_cast<std::int64_t>(
            entity.defense > 0 ? entity.defense : 10);
        object.attrs["fortitude"] = static_cast<std::int64_t>(0);
        object.attrs["will"] = static_cast<std::int64_t>(0);
        object.attrs["reflex"] = static_cast<std::int64_t>(0);
        object.attrs["challenge_rating"] = static_cast<std::int64_t>(
            std::max(1, entity.maxHitPoints / 4));
        object.attrs["movement_rate"] = static_cast<std::int64_t>(1);
        object.attrs["good_evil"] = static_cast<std::int64_t>(50);
        object.attrs["level"] = static_cast<std::int64_t>(1);
        object.attrs["class_id"] = static_cast<std::int64_t>(0);
        object.attrs["xp"] = static_cast<std::int64_t>(0);
        entityObjects_[i] = object.id;
    }
    scriptWorld_.party.clear();
    scriptWorld_.party.push_back(scriptWorld_.pc);
    for (const std::string& tag : partyTags_) {
        const std::uint32_t id = scriptWorld_.byTag(tag, 0);
        if (id != script::kObjectInvalid) {
            std::erase(scriptWorld_.party, id);
            scriptWorld_.party.push_back(id);
        }
    }
    syncScriptWorld();
    initMerchants();
    runSpawnAndEnterScripts();
    spawnEncounters();
}

void GameSession::runHook(const std::string& hook, std::size_t entityIndex,
                          std::uint32_t entering)
{
    if (entityIndex >= scene_.entities.size()) {
        return;
    }
    const kotor::Entity& entity = scene_.entities[entityIndex];
    const auto it = entity.scripts.find(hook);
    if (it == entity.scripts.end() || it->second.empty()) {
        return;
    }
    scriptLog_.push_back("hook " + hook + " " + entity.tag + " -> " + it->second);
    runScript(it->second, objectForEntity(entityIndex), entering, scriptWorld_.pc,
              scriptWorld_.pc);
}

void GameSession::runSceneHook(const std::string& hook, std::uint32_t entering)
{
    const auto it = scene_.scripts.find(hook);
    if (it == scene_.scripts.end() || it->second.empty()) {
        return;
    }
    runScript(it->second, scriptWorld_.pc, entering, scriptWorld_.pc, scriptWorld_.pc);
}

void GameSession::runSpawnAndEnterScripts()
{
    runSceneHook("Mod_OnModLoad", scriptWorld_.pc);
    runSceneHook("Mod_OnModStart", scriptWorld_.pc);
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        const auto spawn = entity.scripts.find("ScriptSpawn");
        if (spawn != entity.scripts.end() && !spawn->second.empty()) {
            if (spawnRan_.insert(objectForEntity(i)).second) {
                runScript(spawn->second, objectForEntity(i), scriptWorld_.pc,
                          scriptWorld_.pc, scriptWorld_.pc);
            }
        }
    }
    runSceneHook("OnEnter", scriptWorld_.pc);
}

void GameSession::updateScripts(float dt)
{
    heartbeatTimer_ += dt;
    if (heartbeatTimer_ < 6.0) {
        return;
    }
    heartbeatTimer_ = 0.0;
    runSceneHook("Mod_OnHeartbeat", scriptWorld_.pc);
    runSceneHook("OnHeartbeat", scriptWorld_.pc);
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        if (scene_.entities[i].dead) {
            continue;
        }
        runHook("ScriptHeartbeat", i, scriptWorld_.pc);
        runHook("OnHeartbeat", i, scriptWorld_.pc);
    }
}

void GameSession::logEvent(std::string line)
{
    combatLog_.push_back(concat("[t=", elapsed_, "] ", line));
    if (combatLog_.size() > 32) {
        combatLog_.erase(combatLog_.begin());
    }
}

void GameSession::update(float dt, const InputState& input)
{
    if (dt <= 0.0f) {
        return;
    }
    dt = std::min(dt, 0.08f);
    elapsed_ += dt;
    yaw_ += input.mouseDx * 0.0026f;
    pitch_ = clamp(pitch_ - input.mouseDy * 0.0022f, -1.35f, 1.35f);

    float forwardAxis = 0.0f;
    float rightAxis = 0.0f;
    if (!playerDead_ && !dialogueView_.active) {
        if (input.forward) forwardAxis += 1.0f;
        if (input.back) forwardAxis -= 1.0f;
        if (input.right) rightAxis += 1.0f;
        if (input.left) rightAxis -= 1.0f;
    }
    const float speed = input.run ? 7.5f : 3.8f;
    const float cosYaw = std::cos(yaw_);
    const float sinYaw = std::sin(yaw_);
    kotor::Vec3 desired{cosYaw * forwardAxis + sinYaw * rightAxis,
                        sinYaw * forwardAxis - cosYaw * rightAxis, 0.0f};
    const float magnitude = std::sqrt(desired.x * desired.x + desired.y * desired.y);
    if (magnitude > 1.0e-5f) {
        desired = desired * (1.0f / magnitude);
    }
    const kotor::Vec3 delta = desired * (speed * dt);

    const kotor::Vec3 start = position_;
    kotor::Vec3 candidate{position_.x + delta.x, position_.y + delta.y, position_.z};
    if (wallBlocked(scene_, start, candidate, kPlayerRadius)) {
        kotor::Vec3 xOnly{start.x + delta.x, start.y, start.z};
        kotor::Vec3 yOnly{start.x, start.y + delta.y, start.z};
        if (!wallBlocked(scene_, start, xOnly, kPlayerRadius)) {
            candidate = xOnly;
        } else if (!wallBlocked(scene_, start, yOnly, kPlayerRadius)) {
            candidate = yOnly;
        } else {
            candidate = start;
        }
    }
    position_.x = candidate.x;
    position_.y = candidate.y;

    float ground = position_.z;
    const bool hasFloor = floorHeight(scene_, position_.x, position_.y, position_.z + 2.0f, ground);
    if (input.jump && grounded_) {
        verticalVelocity_ = 5.2f;
        grounded_ = false;
    }
    verticalVelocity_ -= kGravity * dt;
    position_.z += verticalVelocity_ * dt;
    if (hasFloor && position_.z <= ground) {
        position_.z = ground;
        verticalVelocity_ = 0.0f;
        grounded_ = true;
    } else if (hasFloor && position_.z - ground < kStepHeight && verticalVelocity_ <= 0.0f) {
        position_.z = ground;
        verticalVelocity_ = 0.0f;
        grounded_ = true;
    } else {
        grounded_ = false;
    }

    syncPlayerToScriptWorld();
    updateTriggers();
    updateCombat(dt);
    updateParty(dt);
    updateScripts(dt);
    updateEncounters(dt);
    syncPlayerToScriptWorld();
    if (!pendingModule_.empty()) {
        const std::string next = pendingModule_;
        pendingModule_.clear();
        loadModule(next, nullptr, {});
    }
}

SceneCamera GameSession::camera() const
{
    SceneCamera camera;
    camera.eye = {position_.x, position_.y, position_.z + kEyeHeight};
    camera.forward = {std::cos(pitch_) * std::cos(yaw_), std::cos(pitch_) * std::sin(yaw_),
                      std::sin(pitch_)};
    camera.up = {0.0f, 0.0f, 1.0f};
    return camera;
}

const kotor::Entity* GameSession::nearestDoor(float maxDistance) const
{
    const kotor::Entity* best = nullptr;
    float bestDistance = maxDistance;
    for (const kotor::Entity& entity : scene_.entities) {
        if (entity.kind != kotor::EntityKind::Door) {
            continue;
        }
        const float distance = distance2D(position_, entity.position);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = &entity;
        }
    }
    return best;
}

const kotor::Entity* GameSession::nearestConversation(float maxDistance) const
{
    const kotor::Entity* best = nullptr;
    float bestDistance = maxDistance;
    for (const kotor::Entity& entity : scene_.entities) {
        if ((entity.kind != kotor::EntityKind::Creature && entity.kind != kotor::EntityKind::Placeable)
            || entity.conversation.empty() || entity.dead) {
            continue;
        }
        const float distance = distance2D(position_, entity.position);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = &entity;
        }
    }
    return best;
}

std::string GameSession::interactionPrompt() const
{
    if (dialogueView_.active) {
        return "Dialogue: 1-9 choose, Esc end";
    }
    if (const kotor::Entity* talk = nearestConversation(3.0f); talk != nullptr) {
        return "E: talk to " + (talk->name.empty() ? talk->tag : talk->name);
    }
    const kotor::Entity* door = nearestDoor(2.8f);
    if (door != nullptr) {
        if (door->linkedModule.empty()) {
            return "E: " + door->tag + " (no transition)";
        }
        return "E: " + door->tag + " -> " + door->linkedModule;
    }
    for (const kotor::Entity& entity : scene_.entities) {
        if (entity.kind != kotor::EntityKind::Placeable || !entity.useable || entity.dead) {
            continue;
        }
        if (distance2D(position_, entity.position) < 2.5f) {
            return "E: use " + (entity.name.empty() ? entity.tag : entity.name);
        }
    }
    return {};
}

bool GameSession::interact()
{
    if (dialogueView_.active) {
        chooseDialogueOption(0);
        return true;
    }
    if (const kotor::Entity* talk = nearestConversation(3.0f); talk != nullptr) {
        return startDialogue(static_cast<std::size_t>(talk - scene_.entities.data()));
    }
    const kotor::Entity* door = nearestDoor(2.8f);
    if (door != nullptr) {
        const std::size_t index = static_cast<std::size_t>(door - scene_.entities.data());
        const auto open = door->scripts.find("OnOpen");
        if (open != door->scripts.end() && !open->second.empty()) {
            runHook("OnOpen", index, scriptWorld_.pc);
        } else {
            runHook("OnClick", index, scriptWorld_.pc);
        }
        if (!door->linkedModule.empty()) {
            return loadModule(door->linkedModule, nullptr, door->linkedTo);
        }
        logEvent("door " + door->tag + ": no transition");
        return true;
    }
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        if (entity.kind != kotor::EntityKind::Store) {
            continue;
        }
        if (distance2D(position_, entity.position) < 3.0f && openStore(i)) {
            setUiPanel(UiPanel::Merchant);
            return true;
        }
    }
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        if (entity.kind != kotor::EntityKind::Placeable || !entity.useable || entity.dead) {
            continue;
        }
        if (distance2D(position_, entity.position) < 2.5f) {
            return usePlaceable(i);
        }
    }
    return false;
}

bool GameSession::usePlaceable(std::size_t entityIndex)
{
    if (entityIndex >= scene_.entities.size()) {
        return false;
    }
    kotor::Entity& entity = scene_.entities[entityIndex];
    if (entity.kind != kotor::EntityKind::Placeable || !entity.useable || entity.dead) {
        return false;
    }
    if (distance2D(position_, entity.position) > 2.5f) {
        return false;
    }
    logEvent("use " + entity.tag);
    runHook("OnUsed", entityIndex, scriptWorld_.pc);
    openLoot(entityIndex);
    return true;
}

bool GameSession::testTransition(std::string& from, std::string& to)
{
    for (const kotor::Entity& entity : scene_.entities) {
        if (entity.kind != kotor::EntityKind::Door || entity.linkedModule.empty()) {
            continue;
        }
        const std::string targetModule = entity.linkedModule;
        const std::string targetDoor = entity.linkedTo;
        from = module_;
        position_ = entity.position;
        yaw_ = entity.bearing;
        const bool ok = loadModule(targetModule, nullptr, targetDoor);
        to = module_;
        return ok;
    }
    return false;
}

void GameSession::faceTowards(kotor::Vec3 target)
{
    yaw_ = std::atan2(target.y - position_.y, target.x - position_.x);
    pitch_ = 0.0f;
}

bool GameSession::findClearView(kotor::Vec3 target, float radius, kotor::Vec3& out) const
{
    constexpr int kAngles = 16;
    const float radii[3] = {radius, radius + 1.5f, radius + 3.0f};
    for (const float r : radii) {
        for (int i = 0; i < kAngles; ++i) {
            const float angle = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(kAngles);
            kotor::Vec3 candidate{target.x + std::cos(angle) * r,
                                  target.y + std::sin(angle) * r, target.z};
            float ground = candidate.z;
            if (!floorHeight(scene_, candidate.x, candidate.y, candidate.z + 2.0f, ground)) {
                continue;
            }
            candidate.z = ground;
            const kotor::Vec3 from{candidate.x, candidate.y, ground + 1.2f};
            const kotor::Vec3 to{target.x, target.y, target.z + 1.0f};
            if (!wallBlocked(scene_, from, to, 0.15f)) {
                out = candidate;
                return true;
            }
        }
    }
    return false;
}

bool GameSession::walkTo(kotor::Vec3 target, float maxSeconds)
{
    const int steps = std::max(1, static_cast<int>(maxSeconds * 60.0f));
    for (int i = 0; i < steps; ++i) {
        if (distance2D(position_, target) < 0.6f) {
            return true;
        }
        yaw_ = std::atan2(target.y - position_.y, target.x - position_.x);
        InputState input;
        input.forward = true;
        input.run = distance2D(position_, target) > 4.0f;
        update(1.0f / 60.0f, input);
    }
    return distance2D(position_, target) < 1.2f;
}

bool GameSession::loadDialogue(const std::string& resref, kotor::Dlg& dlg)
{
    if (resref.empty()) {
        return false;
    }
    auto data = install_.find(resref, 2029);
    if (!data.has_value()) {
        return false;
    }
    try {
        kotor::Gff gff = kotor::Gff::parse(std::move(*data), resref + ".dlg");
        dlg = kotor::Dlg::parse(gff, tlk_ ? &*tlk_ : nullptr, resref);
        return true;
    } catch (const kotor::FormatError&) {
        return false;
    }
}

bool GameSession::startDialogue(std::size_t entityIndex)
{
    if (entityIndex >= scene_.entities.size()) {
        return false;
    }
    kotor::Entity& entity = scene_.entities[entityIndex];
    if (entity.conversation.empty()) {
        logEvent((entity.name.empty() ? entity.tag : entity.name) + " has nothing to say");
        return false;
    }
    kotor::Dlg dlg;
    if (!loadDialogue(entity.conversation, dlg)) {
        logEvent("missing dialogue " + entity.conversation);
        return false;
    }
    ActiveDialogue active;
    active.npcTag = entity.tag;
    active.speaker = entity.name.empty() ? entity.tag : entity.name;
    active.resref = entity.conversation;
    active.npcObject = objectForEntity(entityIndex);
    active.dlg = std::move(dlg);
    activeDialogue_ = std::move(active);
    std::uint32_t entry = UINT32_MAX;
    // KotOR 1 carries conditions on StartingList.Active. Run them in order and
    // take the first truthy one; an empty Active is unconditional.
    for (std::size_t i = 0; i < activeDialogue_->dlg.starting.size(); ++i) {
        const std::uint32_t candidate = activeDialogue_->dlg.starting[i];
        if (candidate >= activeDialogue_->dlg.entries.size()) {
            continue;
        }
        const std::string activeScript = i < activeDialogue_->dlg.startingActive.size()
            ? activeDialogue_->dlg.startingActive[i]
            : std::string{};
        bool condition = true;
        if (!activeScript.empty()) {
            condition = scriptValueTruthy(runScript(activeScript, activeDialogue_->npcObject,
                                                    scriptWorld_.pc, scriptWorld_.pc,
                                                    scriptWorld_.pc));
            scriptLog_.push_back("dialogue condition " + activeScript + " entry="
                                 + std::to_string(candidate) + " = "
                                 + (condition ? "1" : "0"));
        }
        if (!condition) {
            continue;
        }
        entry = candidate;
        break;
    }
    if (entry == UINT32_MAX && !activeDialogue_->dlg.entries.empty()) {
        entry = 0;
    }
    if (entry == UINT32_MAX) {
        endDialogue();
        return false;
    }
    scriptLog_.push_back("dialogue start " + activeDialogue_->resref + " entry="
                         + std::to_string(entry));
    logEvent("dialogue started with " + activeDialogue_->speaker);
    showDialogueEntry(entry);
    return true;
}

bool GameSession::startDialogue(const std::string& tag)
{
    const std::size_t index = findEntity(tag);
    if (index == static_cast<std::size_t>(-1)) {
        return false;
    }
    return startDialogue(index);
}

void GameSession::showDialogueEntry(std::uint32_t entryIndex)
{
    if (!activeDialogue_.has_value() || entryIndex >= activeDialogue_->dlg.entries.size()) {
        endDialogue();
        return;
    }
    // Control entries carry scripts but no spoken text and exactly one
    // continuation. Run them, then show the first real line (the shipped
    // conversation starts on such an entry).
    for (int guard = 0; guard < 8; ++guard) {
        const kotor::DlgEntry& control = activeDialogue_->dlg.entries[entryIndex];
        if (!control.text.empty() || control.replies.size() != 1) {
            break;
        }
        if (!control.script.empty() && activeDialogue_->firedEntries.insert(entryIndex).second) {
            runScript(control.script, activeDialogue_->npcObject, scriptWorld_.pc,
                      scriptWorld_.pc, scriptWorld_.pc);
        }
        const std::uint32_t replyIndex = control.replies.front();
        if (replyIndex >= activeDialogue_->dlg.replies.size()) {
            break;
        }
        const kotor::DlgReply& reply = activeDialogue_->dlg.replies[replyIndex];
        if (!reply.script.empty()) {
            runScript(reply.script, activeDialogue_->npcObject, scriptWorld_.pc,
                      scriptWorld_.pc, scriptWorld_.pc);
        }
        if (reply.entries.empty() || reply.entries.front() == entryIndex
            || reply.entries.front() >= activeDialogue_->dlg.entries.size()) {
            break;
        }
        entryIndex = reply.entries.front();
    }
    const kotor::DlgEntry& entry = activeDialogue_->dlg.entries[entryIndex];
    dialogueView_.active = true;
    dialogueView_.npcTag = activeDialogue_->npcTag;
    dialogueView_.speaker = activeDialogue_->speaker;
    dialogueView_.text = entry.text.empty() ? "(No text)" : entry.text;
    dialogueView_.entry = entryIndex;
    dialogueView_.options.clear();
    if (!entry.script.empty() && activeDialogue_->firedEntries.insert(entryIndex).second) {
        runScript(entry.script, activeDialogue_->npcObject, scriptWorld_.pc,
                  scriptWorld_.pc, scriptWorld_.pc);
    }
    for (const std::uint32_t replyIndex : entry.replies) {
        if (replyIndex >= activeDialogue_->dlg.replies.size()) {
            continue;
        }
        const kotor::DlgReply& reply = activeDialogue_->dlg.replies[replyIndex];
        if (!reply.active.empty()
            && !scriptValueTruthy(runScript(reply.active, activeDialogue_->npcObject,
                                            scriptWorld_.pc, scriptWorld_.pc,
                                            scriptWorld_.pc))) {
            continue;
        }
        DialogueOption option;
        option.text = reply.text.empty() ? "(Continue)" : reply.text;
        option.reply = replyIndex;
        dialogueView_.options.push_back(std::move(option));
    }
    if (dialogueView_.options.empty()) {
        dialogueView_.options.push_back({"(End conversation)", UINT32_MAX});
    }
    logEvent("dialog " + dialogueView_.speaker + ": " + dialogueView_.text);
}

void GameSession::chooseDialogueOption(std::size_t optionIndex)
{
    if (!activeDialogue_.has_value() || optionIndex >= dialogueView_.options.size()) {
        return;
    }
    const std::uint32_t replyIndex = dialogueView_.options[optionIndex].reply;
    if (replyIndex == UINT32_MAX || replyIndex >= activeDialogue_->dlg.replies.size()) {
        endDialogue();
        return;
    }
    const kotor::DlgReply& reply = activeDialogue_->dlg.replies[replyIndex];
    if (!reply.script.empty()) {
        runScript(reply.script, activeDialogue_->npcObject, scriptWorld_.pc,
                  scriptWorld_.pc, scriptWorld_.pc);
        logEvent("dialog script " + reply.script);
    }
    if (!reply.quest.empty()) {
        logEvent("dialog quest " + reply.quest);
    }
    const bool startsCombat = lower(reply.quest) == "end_attack"
        || lower(reply.script).find("attack") != std::string::npos;
    if (startsCombat) {
        logEvent("dialogue triggers combat");
    }
    if (!reply.entries.empty() && reply.entries.front() < activeDialogue_->dlg.entries.size()) {
        showDialogueEntry(reply.entries.front());
    } else {
        endDialogue();
    }
    if (startsCombat) {
        attackNearestHostile();
    }
}

void GameSession::endDialogue()
{
    activeDialogue_.reset();
    dialogueView_ = {};
}

void GameSession::attackNearestHostile()
{
    std::size_t best = static_cast<std::size_t>(-1);
    float bestDistance = 20.0f;
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        if (!entity.hostile || entity.dead) {
            continue;
        }
        const float distance = distance2D(position_, entity.position);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    if (best == static_cast<std::size_t>(-1)) {
        logEvent("no hostile target");
        return;
    }
    combatTarget_ = static_cast<std::uint32_t>(best);
    logEvent("combat target " + combatTargetName());
}

std::string GameSession::combatTargetName() const
{
    if (combatTarget_ >= scene_.entities.size()) {
        return {};
    }
    const kotor::Entity& entity = scene_.entities[combatTarget_];
    return entity.name.empty() ? entity.tag : entity.name;
}

void GameSession::playerAttack(std::size_t target)
{
    kotor::Entity& entity = scene_.entities[target];
    std::int32_t dice = 1;
    std::int32_t die = 8;
    std::int32_t bonus = 0;
    rules_.weaponDamage(character_, dice, die, bonus);
    const AttackResult result = rules_.resolveAttack(character_, entity.defense, rng_, dice, die,
                                                     bonus);
    if (!result.hit) {
        logEvent("player misses " + (entity.name.empty() ? entity.tag : entity.name)
                 + " (" + std::to_string(result.total) + " vs " + std::to_string(entity.defense) + ")");
        return;
    }
    entity.hitPoints -= result.damage;
    logEvent("player hits " + (entity.name.empty() ? entity.tag : entity.name)
             + " for " + std::to_string(result.damage)
             + (result.critical ? " (critical)" : "") + " ("
             + std::to_string(std::max(0, entity.hitPoints)) + "/" + std::to_string(entity.maxHitPoints) + ")");
    if (entity.hitPoints <= 0) {
        entity.dead = true;
        logEvent("player defeats " + (entity.name.empty() ? entity.tag : entity.name));
        const std::size_t index = &entity - scene_.entities.data();
        const std::int32_t reward = std::max<std::int32_t>(1, entity.maxHitPoints / 2);
        combatTarget_ = UINT32_MAX;
        giveXp(reward);
        runHook("ScriptDeath", index, scriptWorld_.pc);
    }
}

void GameSession::npcAttack(std::size_t attacker)
{
    kotor::Entity& entity = scene_.entities[attacker];
    const std::int32_t roll = 1 + static_cast<std::int32_t>(random() % 20u);
    const std::int32_t targetAc = rules_.armorClass(character_);
    const std::int32_t total = roll + entity.attackBonus;
    if (roll != 20 && (roll == 1 || total < targetAc)) {
        logEvent((entity.name.empty() ? entity.tag : entity.name) + " misses player ("
                 + std::to_string(total) + " vs " + std::to_string(targetAc) + ")");
        return;
    }
    const std::int32_t span = std::max(1, entity.damageMax - entity.damageMin + 1);
    std::int32_t damage = entity.damageMin
        + static_cast<std::int32_t>(random() % static_cast<std::uint32_t>(span));
    if (roll == 20) {
        damage *= 2;
    }
    playerHp_ -= damage;
    logEvent((entity.name.empty() ? entity.tag : entity.name) + " hits player for "
             + std::to_string(damage) + (roll == 20 ? " (critical)" : "") + " ("
             + std::to_string(std::max(0, playerHp_)) + "/"
             + std::to_string(playerMaxHp_) + ")");
    if (playerHp_ <= 0) {
        playerHp_ = 0;
        playerDead_ = true;
        logEvent("player defeated");
        runSceneHook("Mod_OnPlrDying", scriptWorld_.pc);
        runSceneHook("Mod_OnPlrDeath", scriptWorld_.pc);
    }
}

void GameSession::updateCombat(float dt)
{
    if (playerDead_ || dialogueView_.active) {
        return;
    }
    if (npcAttackTimers_.size() != scene_.entities.size()) {
        npcAttackTimers_.resize(scene_.entities.size(), 0.0f);
    }
    if (combatTarget_ != UINT32_MAX
        && (combatTarget_ >= scene_.entities.size() || scene_.entities[combatTarget_].dead)) {
        combatTarget_ = UINT32_MAX;
    }
    if (combatTarget_ == UINT32_MAX) {
        std::size_t best = static_cast<std::size_t>(-1);
        float bestDistance = 14.0f;
        for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
            const kotor::Entity& entity = scene_.entities[i];
            if (!entity.hostile || entity.dead) {
                continue;
            }
            const float distance = distance2D(position_, entity.position);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = i;
            }
        }
        if (best != static_cast<std::size_t>(-1)) {
            combatTarget_ = static_cast<std::uint32_t>(best);
            combatRoundTimer_ = 0.0f;
            combatRound_ = 0;
            logEvent("combat started: " + combatTargetName());
        }
    }

    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        kotor::Entity& entity = scene_.entities[i];
        if (!entity.hostile || entity.dead) {
            continue;
        }
        const float distance = distance2D(position_, entity.position);
        if (distance < 14.0f && distance > 1.5f) {
            const float step = std::min(2.5f * dt, distance - 1.4f);
            const float dx = (position_.x - entity.position.x) / distance;
            const float dy = (position_.y - entity.position.y) / distance;
            entity.position.x += dx * step;
            entity.position.y += dy * step;
            entity.bearing = std::atan2(dy, dx);
            syncEntityToScriptWorld(i);
        }
    }

    combatRoundTimer_ = std::max(0.0f, combatRoundTimer_ - dt);
    if (combatTarget_ == UINT32_MAX || combatRoundTimer_ > 0.0f) {
        return;
    }
    combatRoundTimer_ = 6.0f;
    ++combatRound_;

    struct Actor {
        int initiative;
        std::size_t index;
        bool player;
    };
    std::vector<Actor> order;
    order.push_back({1 + static_cast<int>(random() % 20u) + rules_.initiativeBonus(character_),
                     0, true});
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        if (!entity.hostile || entity.dead) {
            continue;
        }
        if (distance2D(position_, entity.position) <= 2.4f) {
            order.push_back({1 + static_cast<int>(random() % 20u), i, false});
        }
    }
    std::sort(order.begin(), order.end(), [](const Actor& a, const Actor& b) {
        return a.initiative > b.initiative;
    });
    logEvent("round " + std::to_string(combatRound_) + ": " + std::to_string(order.size())
             + " actors");
    for (const Actor& actor : order) {
        if (playerDead_ || dialogueView_.active) {
            break;
        }
        if (actor.player) {
            if (combatTarget_ != UINT32_MAX
                && distance2D(position_, scene_.entities[combatTarget_].position) <= 2.4f) {
                playerAttack(combatTarget_);
            }
        } else if (actor.index < scene_.entities.size()
                   && !scene_.entities[actor.index].dead) {
            npcAttack(actor.index);
        }
    }
}

bool GameSession::pointInTrigger(const kotor::TriggerVolume& trigger, float x, float y) const
{
    for (std::size_t i = 0; i + 2 < trigger.indices.size(); i += 3) {
        if (trigger.indices[i] >= trigger.vertices.size()
            || trigger.indices[i + 1] >= trigger.vertices.size()
            || trigger.indices[i + 2] >= trigger.vertices.size()) {
            continue;
        }
        if (pointInTriangle2D(x, y, trigger.vertices[trigger.indices[i]],
                              trigger.vertices[trigger.indices[i + 1]],
                              trigger.vertices[trigger.indices[i + 2]])) {
            return true;
        }
    }
    return false;
}

void GameSession::updateTriggers()
{
    if (triggerInside_.size() != scene_.triggers.size()) {
        triggerInside_.assign(scene_.triggers.size(), false);
    }
    for (std::size_t i = 0; i < scene_.triggers.size(); ++i) {
        const kotor::TriggerVolume& trigger = scene_.triggers[i];
        if (trigger.entity >= scene_.entities.size()) {
            continue;
        }
        const kotor::Entity& entity = scene_.entities[trigger.entity];
        const bool inside = pointInTrigger(trigger, position_.x, position_.y);
        if (inside == triggerInside_[i]) {
            continue;
        }
        triggerInside_[i] = inside;
        if (inside) {
            runHook("ScriptOnEnter", trigger.entity, scriptWorld_.pc);
            logEvent("trigger " + entity.tag + ": enter " + entity.scriptOnEnter);
        } else {
            runHook("ScriptOnExit", trigger.entity, scriptWorld_.pc);
            logEvent("trigger " + entity.tag + ": exit " + entity.scriptOnExit);
        }
    }
}

std::int32_t GameSession::journalState(const std::string& plotId) const
{
    const auto it = journal_.find(plotId);
    return it == journal_.end() ? 0 : it->second;
}

bool GameSession::addJournalEntry(const std::string& plotId, std::int32_t state)
{
    if (plotId.empty()) {
        return false;
    }
    journal_[plotId] = state;
    logEvent("journal " + plotId + " = " + std::to_string(state));
    return true;
}

bool GameSession::removeJournalEntry(const std::string& plotId)
{
    return journal_.erase(plotId) != 0;
}

bool GameSession::addInventoryItem(const std::string& resref, std::int32_t baseItem,
                                   std::int32_t stack, std::int32_t cost)
{
    if (resref.empty() || stack <= 0) {
        return false;
    }
    InventoryItem item = rules_.loaded() ? rules_.makeItem(resref, baseItem) : InventoryItem{};
    item.resref = resref;
    item.baseItem = baseItem;
    item.stack = stack;
    auto cached = itemTemplates_.find(resref);
    if (cached == itemTemplates_.end()) {
        if (auto templ = kotor::loadItemTemplate(install_, resref); templ.has_value()) {
            cached = itemTemplates_.emplace(resref, *templ).first;
        }
    }
    if (cached != itemTemplates_.end()) {
        item.name = cached->second.name;
        item.identified = cached->second.identified;
        if (item.baseItem == 0) {
            item.baseItem = cached->second.baseItem;
        }
        if (item.cost <= 0) {
            item.cost = cached->second.cost;
        }
    }
    if (cost > 0) {
        item.cost = cost;
    }
    character_.inventory.push_back(item);
    script::GameObject& object = scriptWorld_.addObject(resref, 100, baseItem, true);
    object.attrs["item_value"] = static_cast<std::int64_t>(item.cost);
    script::GameObject* pc = scriptWorld_.get(scriptWorld_.pc);
    if (pc != nullptr) {
        pc->inventory.push_back(object.id);
    }
    logEvent("item " + resref + " x" + std::to_string(stack));
    return true;
}

bool GameSession::removeInventoryItem(const std::string& resref, std::int32_t stack)
{
    for (auto it = character_.inventory.begin(); it != character_.inventory.end(); ++it) {
        if (lower(it->resref) != lower(resref)) {
            continue;
        }
        if (stack > 0 && it->stack > stack) {
            it->stack -= stack;
        } else {
            character_.inventory.erase(it);
        }
        script::GameObject* pc = scriptWorld_.get(scriptWorld_.pc);
        if (pc != nullptr) {
            for (auto id = pc->inventory.begin(); id != pc->inventory.end(); ++id) {
                const script::GameObject* item = scriptWorld_.get(*id);
                if (item != nullptr && lower(item->tag) == lower(resref)) {
                    auto* mutableItem = scriptWorld_.get(*id);
                    mutableItem->valid = false;
                    pc->inventory.erase(id);
                    break;
                }
            }
        }
        return true;
    }
    return false;
}

bool GameSession::equipItem(const std::string& resref)
{
    for (InventoryItem& item : character_.inventory) {
        if (lower(item.resref) != lower(resref)) {
            continue;
        }
        if (rules_.loaded() && !rules_.canEquip(character_, item)) {
            return false;
        }
        for (InventoryItem& other : character_.inventory) {
            if (other.equipped && other.slot == item.slot) {
                other.equipped = false;
            }
        }
        item.equipped = true;
        item.slot = rules_.loaded() ? rules_.equipSlotFor(item.baseItem) : item.slot;
        logEvent("equip " + resref + " slot " + std::to_string(item.slot));
        return true;
    }
    return false;
}

bool GameSession::addPartyMember(const std::string& tag)
{
    if (tag.empty()) {
        return false;
    }
    for (const std::string& existing : partyTags_) {
        if (lower(existing) == lower(tag)) {
            return false;
        }
    }
    partyTags_.push_back(tag);
    const std::size_t entityIndex = findEntity(tag);
    CompanionState companion;
    companion.tag = tag;
    if (rules_.loaded()) {
        companion.sheet = rules_.createCharacter(0, std::max<std::int32_t>(1, character_.level),
                                                 character_.xp);
    }
    companion.sheet.name = tag;
    if (entityIndex != static_cast<std::size_t>(-1)) {
        const kotor::Entity& entity = scene_.entities[entityIndex];
        if (!entity.name.empty()) {
            companion.sheet.name = entity.name;
        }
        companion.sheet.maxHp = std::max<std::int32_t>(1, entity.maxHitPoints);
        companion.sheet.hp = std::max<std::int32_t>(1, entity.hitPoints);
    }
    companions_.push_back(std::move(companion));
    if (selectedCompanion_ < 0) {
        selectedCompanion_ = 0;
    }
    const std::uint32_t object = objectForTag(tag);
    if (object != script::kObjectInvalid) {
        std::erase(scriptWorld_.party, object);
        scriptWorld_.party.push_back(object);
    }
    logEvent("party join " + tag);
    return true;
}

bool GameSession::removePartyMember(const std::string& tag)
{
    const auto old = partyTags_.size();
    std::erase_if(partyTags_, [&](const std::string& existing) {
        return lower(existing) == lower(tag);
    });
    std::erase_if(companions_, [&](const CompanionState& companion) {
        return lower(companion.tag) == lower(tag);
    });
    if (companions_.empty()) {
        selectedCompanion_ = -1;
    } else if (selectedCompanion_ >= static_cast<int>(companions_.size())) {
        selectedCompanion_ = 0;
    }
    const std::uint32_t object = objectForTag(tag);
    if (object != script::kObjectInvalid) {
        std::erase(scriptWorld_.party, object);
    }
    return partyTags_.size() != old;
}

bool GameSession::learnFeat(std::int32_t featId)
{
    if (character_.featPoints <= 0 || character_.feats.contains(featId)
        || !rules_.canTakeFeat(character_, featId)) {
        return false;
    }
    character_.feats.insert(featId);
    --character_.featPoints;
    logEvent("learn feat " + rules_.featName(featId));
    return true;
}

bool GameSession::learnPower(std::int32_t powerId)
{
    if (character_.powers.contains(powerId) || !rules_.canTakePower(character_, powerId)) {
        return false;
    }
    character_.powers.insert(powerId);
    logEvent("learn power " + rules_.powerName(powerId));
    return true;
}

void GameSession::giveXp(std::int32_t amount){
    if (amount <= 0) {
        return;
    }
    character_.xp += amount;
    const std::int32_t level = rules_.loaded() ? rules_.levelForXp(character_.xp)
                                               : character_.level;
    if (level > character_.level) {
        character_.level = level;
        if (rules_.loaded()) {
            character_.maxHp = rules_.maxHitPoints(character_.classId, level,
                                                   character_.con);
            character_.hp = character_.maxHp;
        }
        playerMaxHp_ = character_.maxHp;
        playerHp_ = playerMaxHp_;
        logEvent("level up " + std::to_string(level));
    }
    for (CompanionState& companion : companions_) {
        const std::int32_t share = amount * 80 / 100;
        companion.sheet.xp += share;
        if (rules_.loaded()) {
            const std::int32_t companionLevel = rules_.levelForXp(companion.sheet.xp);
            if (companionLevel > companion.sheet.level) {
                companion.sheet.level = companionLevel;
                companion.sheet.maxHp = rules_.maxHitPoints(companion.sheet.classId,
                                                            companionLevel, companion.sheet.con);
                companion.sheet.hp = companion.sheet.maxHp;
            }
        }
    }
    logEvent("xp +" + std::to_string(amount) + " = " + std::to_string(character_.xp));
}

std::optional<std::vector<script::Value>> GameSession::action(
    std::uint16_t actionId, const std::vector<std::vector<script::Value>>& args,
    script::World& world, script::Vm& vm)
{
    (void)vm;
    const auto arg = [&](std::size_t index, script::ParamKind kind) -> script::Value {
        (void)kind;
        return index < args.size() && !args[index].empty()
            ? args[index].front()
            : script::Value{};
    };
    const auto objectArg = [&](std::size_t index) -> std::uint32_t {
        const script::Value value = arg(index, script::ParamKind::Object);
        return value.type == script::ValueType::Object
            ? static_cast<std::uint32_t>(value.integer)
            : script::kObjectInvalid;
    };
    const auto intArg = [&](std::size_t index) -> std::int64_t {
        return arg(index, script::ParamKind::Int).asInt();
    };
    const auto stringArg = [&](std::size_t index) -> std::string {
        return arg(index, script::ParamKind::String).asString();
    };
    const auto isPlayer = [&](std::uint32_t object) {
        return object == world.pc || object == script::kObjectInvalid;
    };

    switch (actionId) {
    case 418: // GetGold
        return intCells(gold_);
    case 393: // GiveXPToCreature
        if (isPlayer(objectArg(0))) {
            giveXp(static_cast<std::int32_t>(intArg(1)));
        }
        return std::vector<script::Value>{};
    case 394: // SetXP
        character_.xp = static_cast<std::int32_t>(std::max<std::int64_t>(0, intArg(1)));
        character_.level = rules_.loaded() ? rules_.levelForXp(character_.xp) : 1;
        return std::vector<script::Value>{};
    case 395: // GetXP
        if (isPlayer(objectArg(0))) {
            return intCells(character_.xp);
        }
        if (const script::GameObject* object = world.get(objectArg(0)); object != nullptr) {
            const auto it = object->attrs.find("xp");
            if (it != object->attrs.end() && std::holds_alternative<std::int64_t>(it->second)) {
                return intCells(std::get<std::int64_t>(it->second));
            }
        }
        return intCells(0);
    case 367: // AddJournalQuestEntry
        scriptLog_.push_back("journal action " + stringArg(0) + " = "
                             + std::to_string(intArg(1)));
        addJournalEntry(stringArg(0), static_cast<std::int32_t>(intArg(1)));
        return std::vector<script::Value>{};
    case 368: // RemoveJournalQuestEntry
        removeJournalEntry(stringArg(0));
        return std::vector<script::Value>{};
    case 369: // GetJournalEntry
        return intCells(journalState(stringArg(0)));
    case 322: // GiveGoldToCreature
        gold_ += static_cast<std::int32_t>(intArg(1));
        return std::vector<script::Value>{};
    case 444: // TakeGoldFromCreature
        gold_ = std::max<std::int32_t>(0, gold_ - static_cast<std::int32_t>(intArg(0)));
        return std::vector<script::Value>{};
    case 31: { // CreateItemOnObject
        const std::string resref = stringArg(0);
        const std::uint32_t target = objectArg(1);
        const std::int32_t stack = std::max<std::int32_t>(1, static_cast<std::int32_t>(intArg(2)));
        if (!addInventoryItem(resref, 0, stack)) {
            return objectCells(script::kObjectInvalid);
        }
        const auto created = scriptWorld_.byTag(resref, 0);
        script::GameObject* receiver = world.get(target == script::kObjectInvalid ? world.pc : target);
        script::GameObject* item = world.get(created);
        if (receiver != nullptr && item != nullptr) {
            if (receiver != world.get(world.pc)) {
                script::GameObject* pc = world.get(world.pc);
                if (pc != nullptr) {
                    std::erase(pc->inventory, created);
                }
            }
            receiver->inventory.push_back(created);
        }
        return objectCells(created);
    }
    case 135: // ActionGiveItem
    case 271: { // GiveItem
        const std::uint32_t item = objectArg(0);
        const std::uint32_t target = objectArg(1);
        script::GameObject* receiver = world.get(target == script::kObjectInvalid ? world.pc : target);
        if (receiver != nullptr && world.get(item) != nullptr) {
            receiver->inventory.push_back(item);
        }
        return std::vector<script::Value>{};
    }
    case 136: { // ActionTakeItem
        script::GameObject* holder = world.get(objectArg(1));
        if (holder != nullptr) {
            std::erase(holder->inventory, objectArg(0));
        }
        return std::vector<script::Value>{};
    }
    case 574: { // AddPartyMember
        const std::uint32_t creature = objectArg(1);
        const script::GameObject* object = world.get(creature);
        if (object != nullptr) {
            addPartyMember(object->tag);
        }
        return intCells(1);
    }
    case 575: // RemovePartyMember
        return intCells(1);
    case 572: { // AddToParty
        const script::GameObject* leader = world.get(objectArg(1));
        if (leader != nullptr) {
            addPartyMember(leader->tag);
        }
        return std::vector<script::Value>{};
    }
    case 573: { // RemoveFromParty
        const script::GameObject* pc = world.get(objectArg(0));
        if (pc != nullptr) {
            removePartyMember(pc->tag);
        }
        return std::vector<script::Value>{};
    }
    case 576: { // IsObjectPartyMember
        const std::uint32_t who = objectArg(0);
        if (who == world.pc) {
            return intCells(1);
        }
        if (who == script::kObjectInvalid) {
            return intCells(0);
        }
        const script::GameObject* object = world.get(who);
        if (object == nullptr) {
            return intCells(0);
        }
        for (const std::string& tag : partyTags_) {
            if (lower(tag) == lower(object->tag)) {
                return intCells(1);
            }
        }
        return intCells(0);
    }
    case 126: // GetPartyMemberCount
        return intCells(static_cast<std::int64_t>(partyTags_.size() + 1));
    case 577: { // GetPartyMemberByIndex
        const std::int64_t index = intArg(0);
        if (index <= 0) {
            return objectCells(world.pc);
        }
        const std::size_t slot = static_cast<std::size_t>(index - 1);
        if (slot < partyTags_.size()) {
            return objectCells(objectForTag(partyTags_[slot]));
        }
        return objectCells(script::kObjectInvalid);
    }
    case 255: { // BeginConversation
        const std::string resref = stringArg(0);
        const std::uint32_t target = objectArg(1);
        if (target != script::kObjectInvalid) {
            const script::GameObject* object = world.get(target);
            if (object != nullptr) {
                startDialogue(object->tag);
            }
        } else if (!resref.empty()) {
            startDialogue(resref);
        }
        return intCells(1);
    }
    case 204: { // ActionStartConversation
        const std::uint32_t target = objectArg(0);
        const script::GameObject* object = world.get(target);
        if (object != nullptr) {
            startDialogue(object->tag);
        }
        return std::vector<script::Value>{};
    }
    case 509: { // StartNewModule
        const std::string next = stringArg(0);
        if (!next.empty()) {
            pendingModule_ = next;
        }
        return std::vector<script::Value>{};
    }
    case 241: { // DestroyObject
        script::GameObject* object = world.get(objectArg(0));
        if (object != nullptr) {
            object->valid = false;
            for (std::size_t i = 0; i < entityObjects_.size(); ++i) {
                if (entityObjects_[i] == object->id && i < scene_.entities.size()) {
                    scene_.entities[i].dead = true;
                    break;
                }
            }
        }
        return std::vector<script::Value>{};
    }
    case 166: // GetHitDice
        return intCells(character_.level);
    case 341: // GetClassByPosition
        return intCells(character_.classId);
    case 342: // GetLevelByPosition
        return intCells(character_.level);
    case 343: // GetLevelByClass
        return intCells(static_cast<std::int64_t>(intArg(0)) == character_.classId
                            ? character_.level
                            : 0);
    case 701: // GetIsConversationActive
        return intCells(dialogueView_.active ? 1 : 0);
    case 217: // GetIsPC
        return intCells(objectArg(0) == world.pc ? 1 : 0);
    case 711: // GetLastConversation
        return std::vector<script::Value>{script::Value::makeString(
            activeDialogue_.has_value() ? activeDialogue_->npcTag : std::string{})};
    case 25: // GetEnteringObject
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("entering")
                                               : script::kObjectInvalid);
    case 26: // GetExitingObject
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("exiting")
                                               : script::kObjectInvalid);
    case 36: // GetLastAttacker
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastAttacker")
                                               : script::kObjectInvalid);
    case 47: // GetSpellTargetObject
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("spellTarget")
                                               : script::kObjectInvalid);
    case 238: // GetPCSpeaker
        return objectCells(world.pc);
    case 245: // GetLastSpellCaster
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastSpellCaster")
                                               : script::kObjectInvalid);
    case 254: // GetLastSpeaker
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastSpeaker")
                                               : script::kObjectInvalid);
    case 256: // GetLastPerceived
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastPerceived")
                                               : script::kObjectInvalid);
    case 260: // GetLastClosedBy
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastClosedBy")
                                               : script::kObjectInvalid);
    case 291: // GetLastPlayerDied
        return objectCells(playerDead_ ? world.pc : script::kObjectInvalid);
    case 326: // GetClickingObject
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("clicking")
                                               : script::kObjectInvalid);
    case 330: // GetLastUsedBy
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastUsedBy")
                                               : script::kObjectInvalid);
    case 346: // GetLastDamager
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastDamager")
                                               : script::kObjectInvalid);
    case 375: // GetAttemptedSpellTarget
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("attemptedTarget")
                                               : script::kObjectInvalid);
    case 376: // GetLastOpenedBy
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastOpenedBy")
                                               : script::kObjectInvalid);
    case 410: // GetLastPlayerDying
        return objectCells(playerDead_ ? world.pc : script::kObjectInvalid);
    case 419: // GetLastRespawnButtonPresser
        return objectCells(script::kObjectInvalid);
    case 437: // GetLastKiller
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("lastKiller")
                                               : script::kObjectInvalid);
    case 439: // GetItemActivated
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("itemActivated")
                                               : script::kObjectInvalid);
    case 440: // GetItemActivator
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("itemActivator")
                                               : script::kObjectInvalid);
    case 442: // GetItemActivatedTarget
        return objectCells(runtime_ != nullptr ? runtime_->contextObject("itemTarget")
                                               : script::kObjectInvalid);
    case 378: // OpenStore
        scriptLog_.push_back("store " + stringArg(0));
        return std::vector<script::Value>{};
    default:
        break;
    }
    return std::nullopt;
}

bool GameSession::save(const std::filesystem::path& path) const
{
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file << "schema=kotor.engine.player/3\n";
    file << "module=" << module_ << "\n";
    file << "x=" << position_.x << "\ny=" << position_.y << "\nz=" << position_.z << "\n";
    file << "yaw=" << yaw_ << "\npitch=" << pitch_ << "\n";
    file << "hp=" << playerHp_ << "\n";
    file << "name=" << character_.name << "\n";
    file << "class=" << character_.classId << "\n";
    file << "level=" << character_.level << "\n";
    file << "xp=" << character_.xp << "\n";
    file << "gold=" << gold_ << "\n";
    file << "str=" << character_.str << "\ndex=" << character_.dex
         << "\ncon=" << character_.con << "\nint=" << character_.intel
         << "\nwis=" << character_.wis << "\ncha=" << character_.cha << "\n";
    file << "feat_points=" << character_.featPoints << "\n";
    file << "skill_points=" << character_.skillPoints << "\n";
    for (const std::int32_t feat : character_.feats) {
        file << "feat=" << feat << "\n";
    }
    for (const std::int32_t power : character_.powers) {
        file << "power=" << power << "\n";
    }
    for (const auto& [plot, state] : journal_) {
        file << "journal=" << plot << ":" << state << "\n";
    }
    for (const std::string& tag : partyTags_) {
        file << "party=" << tag << "\n";
    }
    for (const CompanionState& companion : companions_) {
        file << "companion=" << companion.tag << "|" << companion.sheet.name << "|"
             << companion.sheet.classId << "|" << companion.sheet.level << "|"
             << companion.sheet.xp << "|" << companion.sheet.hp << "|"
             << companion.sheet.maxHp << "\n";
    }
    file << "leader=" << leaderTag_ << "\n";
    for (const MerchantState& merchant : merchants_) {
        file << "merchant_gold=" << merchant.resref << "|" << merchant.gold << "\n";
        for (std::size_t i = 0; i < merchant.stock.size(); ++i) {
            const MerchantStockItem& stock = merchant.stock[i];
            if (!stock.infinite) {
                file << "merchant=" << merchant.resref << "|" << i << "|"
                     << stock.remaining << "\n";
            }
        }
    }
    for (const InventoryItem& item : character_.inventory) {
        file << "item=" << item.resref << "|" << item.baseItem << "|" << item.cost << "|"
             << item.stack << "|" << item.slot << "|" << (item.equipped ? 1 : 0)
             << "|" << (item.identified ? 1 : 0) << "\n";
    }
    for (const auto& [name, value] : scriptWorld_.globals) {
        if (value.type == script::ValueType::Int) {
            file << "global_int=" << name << ":" << value.integer << "\n";
        } else if (value.type == script::ValueType::String) {
            file << "global_string=" << name << ":" << value.text << "\n";
        }
    }
    for (const auto& [id, object] : scriptWorld_.objects) {
        if (object.locals.empty()) {
            continue;
        }
        for (const auto& [name, value] : object.locals) {
            const char type = value.type == script::ValueType::String ? 's'
                : value.type == script::ValueType::Float                ? 'f'
                : value.type == script::ValueType::Object               ? 'o'
                                                                        : 'i';
            file << "local=" << object.tag << "|" << name << "|" << type << "|"
                 << (value.type == script::ValueType::String
                         ? value.text
                         : value.type == script::ValueType::Float
                         ? std::to_string(value.real)
                         : std::to_string(value.integer))
                 << "\n";
        }
    }
    return static_cast<bool>(file);
}

bool GameSession::load(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::string line;
    std::string module = module_;
    kotor::Vec3 position = position_;
    float yaw = yaw_;
    float pitch = pitch_;
    std::int32_t hp = playerHp_;
    CharacterSheet sheet = character_;
    std::int32_t gold = gold_;
    std::map<std::string, std::int32_t> journal;
    std::vector<std::string> party;
    std::vector<InventoryItem> inventory;
    std::map<std::string, script::Value> globals;
    struct MerchantDelta {
        std::string store;
        std::size_t index = 0;
        std::int32_t remaining = 0;
    };
    std::vector<MerchantDelta> merchantDeltas;
    std::vector<std::pair<std::string, std::int32_t>> merchantGolds;
    struct CompanionSave {
        std::string tag;
        std::string name;
        std::int32_t classId = 0;
        std::int32_t level = 1;
        std::int32_t xp = 0;
        std::int32_t hp = 1;
        std::int32_t maxHp = 1;
    };
    std::vector<CompanionSave> companionSaves;
    std::string leaderTag = "PC";
    struct LocalRecord {
        std::string tag;
        std::string name;
        char type = 'i';
        std::string value;
    };
    std::vector<LocalRecord> locals;
    bool schema2 = false;
    while (std::getline(file, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        try {
            if (key == "schema") {
                schema2 = value == "kotor.engine.player/2" || value == "kotor.engine.player/3";
            } else if (key == "module") {
                module = value;
            } else if (key == "x") {
                position.x = std::stof(value);
            } else if (key == "y") {
                position.y = std::stof(value);
            } else if (key == "z") {
                position.z = std::stof(value);
            } else if (key == "yaw") {
                yaw = std::stof(value);
            } else if (key == "pitch") {
                pitch = std::stof(value);
            } else if (key == "hp") {
                hp = std::stoi(value);
            } else if (key == "name") {
                sheet.name = value;
            } else if (key == "class") {
                sheet.classId = std::stoi(value);
            } else if (key == "level") {
                sheet.level = std::stoi(value);
            } else if (key == "xp") {
                sheet.xp = std::stoi(value);
            } else if (key == "gold") {
                gold = std::stoi(value);
            } else if (key == "str") {
                sheet.str = std::stoi(value);
            } else if (key == "dex") {
                sheet.dex = std::stoi(value);
            } else if (key == "con") {
                sheet.con = std::stoi(value);
            } else if (key == "int") {
                sheet.intel = std::stoi(value);
            } else if (key == "wis") {
                sheet.wis = std::stoi(value);
            } else if (key == "cha") {
                sheet.cha = std::stoi(value);
            } else if (key == "feat_points") {
                sheet.featPoints = std::stoi(value);
            } else if (key == "skill_points") {
                sheet.skillPoints = std::stoi(value);
            } else if (key == "feat") {
                sheet.feats.insert(std::stoi(value));
            } else if (key == "power") {
                sheet.powers.insert(std::stoi(value));
            } else if (key == "journal") {
                const std::size_t colon = value.rfind(':');
                if (colon != std::string::npos) {
                    journal[value.substr(0, colon)] = std::stoi(value.substr(colon + 1));
                }
            } else if (key == "party") {
                party.push_back(value);
            } else if (key == "leader") {
                leaderTag = value;
            } else if (key == "companion") {
                std::vector<std::string> parts;
                std::size_t start = 0;
                while (start <= value.size()) {
                    const std::size_t bar = value.find('|', start);
                    parts.push_back(value.substr(start, bar == std::string::npos
                                                            ? std::string::npos
                                                            : bar - start));
                    if (bar == std::string::npos) {
                        break;
                    }
                    start = bar + 1;
                }
                if (parts.size() >= 7) {
                    companionSaves.push_back({parts[0], parts[1], std::stoi(parts[2]),
                                              std::stoi(parts[3]), std::stoi(parts[4]),
                                              std::stoi(parts[5]), std::stoi(parts[6])});
                }
            } else if (key == "merchant") {
                const std::size_t first = value.find('|');
                const std::size_t second = value.find('|', first == std::string::npos
                                                              ? first
                                                              : first + 1);
                if (first != std::string::npos && second != std::string::npos) {
                    merchantDeltas.push_back({value.substr(0, first),
                                              static_cast<std::size_t>(
                                                  std::stoul(value.substr(first + 1,
                                                                          second - first - 1))),
                                              std::stoi(value.substr(second + 1))});
                }
            } else if (key == "merchant_gold") {
                const std::size_t bar = value.find('|');
                if (bar != std::string::npos) {
                    merchantGolds.emplace_back(value.substr(0, bar),
                                               std::stoi(value.substr(bar + 1)));
                }
            } else if (key == "item") {
                std::vector<std::string> parts;
                std::size_t start = 0;
                while (start <= value.size()) {
                    const std::size_t bar = value.find('|', start);
                    parts.push_back(value.substr(start, bar == std::string::npos
                                                            ? std::string::npos
                                                            : bar - start));
                    if (bar == std::string::npos) {
                        break;
                    }
                    start = bar + 1;
                }
                if (parts.size() >= 6) {
                    InventoryItem item;
                    item.resref = parts[0];
                    item.baseItem = std::stoi(parts[1]);
                    item.cost = std::stoi(parts[2]);
                    item.stack = std::stoi(parts[3]);
                    item.slot = std::stoi(parts[4]);
                    item.equipped = std::stoi(parts[5]) != 0;
                    item.identified = parts.size() < 7 || std::stoi(parts[6]) != 0;
                    inventory.push_back(std::move(item));
                }
            } else if (key == "global_int") {
                const std::size_t colon = value.rfind(':');
                if (colon != std::string::npos) {
                    globals[value.substr(0, colon)] =
                        script::Value::makeInt(std::stoll(value.substr(colon + 1)));
                }
            } else if (key == "global_string") {
                const std::size_t colon = value.find(':');
                if (colon != std::string::npos) {
                    globals[value.substr(0, colon)] =
                        script::Value::makeString(value.substr(colon + 1));
                }
            } else if (key == "local") {
                std::vector<std::string> parts;
                std::size_t start = 0;
                while (start <= value.size()) {
                    const std::size_t bar = value.find('|', start);
                    parts.push_back(value.substr(start, bar == std::string::npos
                                                            ? std::string::npos
                                                            : bar - start));
                    if (bar == std::string::npos) {
                        break;
                    }
                    start = bar + 1;
                }
                if (parts.size() >= 4) {
                    locals.push_back({parts[0], parts[1], parts[2][0], parts[3]});
                }
            }
        } catch (const std::exception&) {
            return false;
        }
    }
    if (module != module_) {
        if (!loadModule(module, &position, {})) {
            return false;
        }
    }
    position_ = position;
    yaw_ = yaw;
    pitch_ = pitch;
    playerHp_ = std::max(1, hp);
    playerDead_ = false;
    if (schema2) {
        character_ = std::move(sheet);
        playerMaxHp_ = std::max<std::int32_t>(1, character_.maxHp);
        if (character_.maxHp <= 1 && rules_.loaded()) {
            character_.maxHp = rules_.maxHitPoints(character_.classId, character_.level,
                                                   character_.con);
            playerMaxHp_ = character_.maxHp;
        }
        playerHp_ = std::min(playerHp_, playerMaxHp_);
        gold_ = gold;
        journal_ = std::move(journal);
        partyTags_.clear();
        companions_.clear();
        selectedCompanion_ = -1;
        for (const std::string& tag : party) {
            addPartyMember(tag);
        }
        for (const CompanionSave& saved : companionSaves) {
            for (CompanionState& companion : companions_) {
                if (lower(companion.tag) != lower(saved.tag)) {
                    continue;
                }
                companion.sheet.name = saved.name;
                companion.sheet.classId = saved.classId;
                companion.sheet.level = saved.level;
                companion.sheet.xp = saved.xp;
                companion.sheet.hp = saved.hp;
                companion.sheet.maxHp = saved.maxHp;
            }
        }
        setPartyLeader(leaderTag);
        scriptWorld_.party.clear();
        scriptWorld_.party.push_back(scriptWorld_.pc);
        for (const std::string& tag : partyTags_) {
            const std::uint32_t id = scriptWorld_.byTag(tag, 0);
            if (id != script::kObjectInvalid) {
                std::erase(scriptWorld_.party, id);
                scriptWorld_.party.push_back(id);
            }
        }
        for (const MerchantDelta& delta : merchantDeltas) {
            for (MerchantState& merchant : merchants_) {
                if (lower(merchant.resref) != lower(delta.store)) {
                    continue;
                }
                if (delta.index < merchant.stock.size()) {
                    merchant.stock[delta.index].infinite = false;
                    merchant.stock[delta.index].remaining = delta.remaining;
                }
            }
        }
        for (const auto& [store, value] : merchantGolds) {
            for (MerchantState& merchant : merchants_) {
                if (lower(merchant.resref) == lower(store)) {
                    merchant.gold = value;
                }
            }
        }
        character_.inventory = std::move(inventory);
        for (InventoryItem& item : character_.inventory) {
            if (!item.name.empty()) {
                continue;
            }
            auto cached = itemTemplates_.find(item.resref);
            if (cached == itemTemplates_.end()) {
                if (auto templ = kotor::loadItemTemplate(install_, item.resref);
                    templ.has_value()) {
                    cached = itemTemplates_.emplace(item.resref, *templ).first;
                }
            }
            if (cached != itemTemplates_.end()) {
                item.name = cached->second.name;
            }
        }
        for (const auto& [name, value] : globals) {
            scriptWorld_.globals[name] = value;
        }
        for (const LocalRecord& record : locals) {
            const std::uint32_t object = scriptWorld_.byTag(record.tag, 0);
            if (object == script::kObjectInvalid) {
                continue;
            }
            script::Value value;
            if (record.type == 's') {
                value = script::Value::makeString(record.value);
            } else if (record.type == 'f') {
                value = script::Value::makeFloat(std::stod(record.value));
            } else if (record.type == 'o') {
                value = script::Value::makeObject(
                    static_cast<std::uint32_t>(std::stoul(record.value)));
            } else {
                value = script::Value::makeInt(std::stoll(record.value));
            }
            scriptWorld_.setLocal(object, record.name, value);
        }
        syncPlayerToScriptWorld();
    }
    return true;
}

bool GameSession::loadOriginalSave(const std::filesystem::path& folder)
{
    kotor::OriginalSave saved;
    try {
        saved = kotor::readOriginalSave(folder);
    } catch (const std::exception& exception) {
        logEvent(std::string("original save read failed: ") + exception.what());
        return false;
    }
    if (saved.module.empty()) {
        logEvent("original save has no module");
        return false;
    }
    try {
        if (!loadModule(saved.module, nullptr, {})) {
            return false;
        }
    } catch (const std::exception& exception) {
        logEvent(std::string("original save module failed: ") + exception.what());
        return false;
    }
    if (!saved.area.empty() && lower(saved.area) != lower(scene_.area)) {
        logEvent("original save area differs from module entry: " + saved.area);
    }
    character_.name = saved.pcName.empty() ? "Player" : saved.pcName;
    character_.classId = saved.classId;
    character_.level = std::max(1, saved.level);
    character_.xp = saved.xp;
    character_.str = saved.str;
    character_.dex = saved.dex;
    character_.con = saved.con;
    character_.intel = saved.intel;
    character_.wis = saved.wis;
    character_.cha = saved.cha;
    character_.hp = std::max(1, saved.hp);
    character_.maxHp = std::max(character_.hp, saved.maxHp);
    character_.feats.clear();
    for (const std::int32_t feat : saved.feats) {
        character_.feats.insert(feat);
    }
    character_.inventory.clear();
    for (const kotor::SavedInventoryItem& source : saved.inventory) {
        InventoryItem item;
        item.resref = source.resref;
        item.name = source.name;
        item.baseItem = source.baseItem;
        item.cost = source.cost;
        item.stack = std::max(1, source.stack);
        item.slot = source.slot;
        item.equipped = source.equipped;
        item.identified = source.identified;
        character_.inventory.push_back(std::move(item));
    }
    gold_ = saved.gold;
    journal_ = saved.journal;
    playerHp_ = character_.hp;
    playerMaxHp_ = character_.maxHp;
    playerDead_ = false;

    scriptWorld_.globals.clear();
    globalKinds_.clear();
    locationGlobals_.clear();
    for (const kotor::SavedGlobal& value : saved.globals) {
        if (value.type == kotor::SavedGlobal::Type::String) {
            scriptWorld_.globals[value.name] = script::Value::makeString(value.text);
            globalKinds_[value.name] = "string";
        } else if (value.type == kotor::SavedGlobal::Type::Boolean) {
            scriptWorld_.globals[value.name] = script::Value::makeInt(value.boolean ? 1 : 0);
            globalKinds_[value.name] = "boolean";
        } else if (value.type == kotor::SavedGlobal::Type::Location) {
            locationGlobals_[value.name] = value.location;
            globalKinds_[value.name] = "location";
        } else {
            scriptWorld_.globals[value.name] = script::Value::makeInt(value.number);
            globalKinds_[value.name] = "number";
        }
    }

    partyTags_.clear();
    companions_.clear();
    selectedCompanion_ = -1;
    leaderTag_ = "PC";
    for (const std::string& tag : saved.partyTags) {
        if (tag != "PC") {
            addPartyMember(tag);
        }
    }
    for (const auto& [memberId, leader] : saved.party) {
        if (leader && memberId != 0) {
            const std::string tag = memberId == 1 ? "Carth"
                : memberId == 2 ? "Bastila"
                : memberId == 3 ? "Zaalbar"
                : memberId == 4 ? "Mission"
                : memberId == 5 ? "Canderous"
                : memberId == 6 ? "Jolee"
                : memberId == 7 ? "Juhani"
                : memberId == 8 ? "HK-47"
                : memberId == 9 ? "T3-M4" : "";
            if (!tag.empty()) {
                setPartyLeader(tag);
            }
        }
    }

    for (const kotor::SavedObjectState& state : saved.objects) {
        const std::size_t index = findEntity(state.tag);
        if (index == static_cast<std::size_t>(-1)) {
            continue;
        }
        kotor::Entity& entity = scene_.entities[index];
        entity.position = state.position;
        entity.bearing = state.bearing;
        entity.hitPoints = state.hitPoints;
        entity.maxHitPoints = std::max(1, state.maxHitPoints);
        entity.dead = state.dead;
        entity.open = state.open;
        entity.locked = state.locked;
        if (entity.kind == kotor::EntityKind::Placeable) {
            entity.lootItems = state.loot;
        }
    }

    originalAreaName_ = saved.areaName;
    originalPortrait_ = saved.portrait;
    originalTimePlayed_ = saved.timePlayed;
    originalRepute_ = saved.repute;
    originalOuterExtras_ = saved.preservedOuter;
    originalPreservedModules_ = saved.preservedModules;
    position_ = saved.position;
    yaw_ = saved.yaw;
    float ground = position_.z;
    if (floorHeight(scene_, position_.x, position_.y, position_.z + 2.0f, ground)) {
        position_.z = ground;
    }
    activeDialogue_.reset();
    dialogueView_ = {};
    combatTarget_ = UINT32_MAX;
    syncScriptWorld();
    logEvent("original save loaded: " + saved.module + " gold=" + std::to_string(gold_)
             + " inventory=" + std::to_string(character_.inventory.size())
             + " journal=" + std::to_string(journal_.size())
             + " globals=" + std::to_string(scriptWorld_.globals.size()));
    return true;
}

bool GameSession::saveOriginalSave(const std::filesystem::path& folder,
                                   const std::string& saveName)
{
    kotor::OriginalSave save;
    save.folder = folder;
    save.module = module_;
    save.area = scene_.area;
    save.areaName = originalAreaName_.empty()
        ? (scene_.areaTag.empty() ? scene_.area : scene_.areaTag)
        : originalAreaName_;
    save.saveName = saveName;
    save.pcName = character_.name;
    save.portrait = originalPortrait_.empty() ? "po_pmhc4" : originalPortrait_;
    save.position = position_;
    save.yaw = yaw_;
    save.gold = gold_;
    save.xp = character_.xp;
    save.hp = playerHp_;
    save.maxHp = std::max(playerHp_, playerMaxHp_);
    save.level = character_.level;
    save.classId = character_.classId;
    save.str = character_.str;
    save.dex = character_.dex;
    save.con = character_.con;
    save.intel = character_.intel;
    save.wis = character_.wis;
    save.cha = character_.cha;
    save.playedSeconds = originalTimePlayed_ + static_cast<std::int32_t>(elapsed_);
    save.timePlayed = save.playedSeconds;
    save.repute = originalRepute_;
    save.preservedOuter = originalOuterExtras_;
    save.preservedModules = originalPreservedModules_;
    for (const std::int32_t feat : character_.feats) {
        save.feats.push_back(feat);
    }
    for (const InventoryItem& item : character_.inventory) {
        kotor::SavedInventoryItem copy;
        copy.resref = item.resref;
        copy.name = item.name;
        copy.baseItem = item.baseItem;
        copy.cost = item.cost;
        copy.stack = item.stack;
        copy.slot = item.slot;
        copy.equipped = item.equipped;
        copy.identified = item.identified;
        save.inventory.push_back(std::move(copy));
    }
    save.party.emplace_back(0, leaderTag_ == "PC");
    for (const CompanionState& companion : companions_) {
        const std::int32_t id = companion.tag == "Carth" ? 1
            : companion.tag == "Bastila" ? 2
            : companion.tag == "Zaalbar" ? 3
            : companion.tag == "Mission" ? 4
            : companion.tag == "Canderous" ? 5
            : companion.tag == "Jolee" ? 6
            : companion.tag == "Juhani" ? 7
            : companion.tag == "HK-47" ? 8
            : companion.tag == "T3-M4" ? 9 : -1;
        if (id >= 0) {
            save.party.emplace_back(id, leaderTag_ == companion.tag);
            save.partyTags.push_back(companion.tag);
        }
    }
    save.journal = journal_;
    for (const auto& [name, value] : scriptWorld_.globals) {
        kotor::SavedGlobal global;
        global.name = name;
        const auto kind = globalKinds_.find(name);
        if (value.type == script::ValueType::String) {
            global.type = kotor::SavedGlobal::Type::String;
            global.text = value.text;
        } else if (kind != globalKinds_.end() && kind->second == "boolean") {
            global.type = kotor::SavedGlobal::Type::Boolean;
            global.boolean = value.integer != 0;
        } else {
            global.type = kotor::SavedGlobal::Type::Number;
            global.number = static_cast<std::int32_t>(value.integer);
        }
        save.globals.push_back(std::move(global));
    }
    for (const auto& [name, location] : locationGlobals_) {
        kotor::SavedGlobal global;
        global.type = kotor::SavedGlobal::Type::Location;
        global.name = name;
        global.location = location;
        save.globals.push_back(std::move(global));
    }
    for (const kotor::Entity& entity : scene_.entities) {
        if (entity.tag.empty()) {
            continue;
        }
        kotor::SavedObjectState state;
        switch (entity.kind) {
        case kotor::EntityKind::Creature: state.kind = kotor::SavedObjectState::Kind::Creature; break;
        case kotor::EntityKind::Door: state.kind = kotor::SavedObjectState::Kind::Door; break;
        case kotor::EntityKind::Placeable: state.kind = kotor::SavedObjectState::Kind::Placeable; break;
        case kotor::EntityKind::Trigger: state.kind = kotor::SavedObjectState::Kind::Trigger; break;
        case kotor::EntityKind::Store: state.kind = kotor::SavedObjectState::Kind::Store; break;
        case kotor::EntityKind::Encounter: state.kind = kotor::SavedObjectState::Kind::Encounter; break;
        case kotor::EntityKind::Waypoint: state.kind = kotor::SavedObjectState::Kind::Waypoint; break;
        case kotor::EntityKind::Sound: state.kind = kotor::SavedObjectState::Kind::Sound; break;
        default: state.kind = kotor::SavedObjectState::Kind::Other; break;
        }
        state.tag = entity.tag;
        state.templateResref = entity.templateResref;
        state.position = entity.position;
        state.bearing = entity.bearing;
        state.hitPoints = entity.hitPoints;
        state.maxHitPoints = entity.maxHitPoints;
        state.dead = entity.dead;
        state.open = entity.open;
        state.locked = entity.locked;
        state.loot = entity.lootItems;
        save.objects.push_back(std::move(state));
    }
    try {
        if (!kotor::writeOriginalSave(folder, save, install_, true)) {
            logEvent("original save write refused: " + folder.string());
            return false;
        }
    } catch (const std::exception& exception) {
        logEvent(std::string("original save write failed: ") + exception.what());
        return false;
    }
    logEvent("original save written: " + folder.string());
    return true;
}

} // namespace ke
