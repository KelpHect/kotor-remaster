#include "kotor/Formats.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>

namespace ke::kotor {

namespace {

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

const std::vector<std::string>& scriptHookLabels()
{
    static const std::vector<std::string> labels = {
        // Creature/UTC
        "ScriptHeartbeat", "ScriptOnNotice", "ScriptSpellAt", "ScriptAttacked",
        "ScriptDamaged", "ScriptDisturbed", "ScriptEndRound", "ScriptEndDialogu",
        "ScriptDialogue", "ScriptSpawn", "ScriptRested", "ScriptDeath",
        "ScriptUserDefine", "ScriptOnBlocked",
        // Door/placeable/trigger/store
        "OnClosed", "OnDamaged", "OnDeath", "OnDisarm", "OnHeartbeat", "OnLock",
        "OnMeleeAttacked", "OnOpen", "OnSpellCastAt", "OnTrapTriggered", "OnUnlock",
        "OnUserDefined", "OnClick", "OnFailToOpen", "OnUsed", "OnInvDisturbed",
        "ScriptOnEnter", "ScriptOnExit", "ScriptUserDefine",
        "OnOpenStore", "OnOpen", "OnClose",
        "OnEntered", "OnExhausted",
        // Area (ARE)
        "OnEnter", "OnExit", "OnUserDefined",
        // Module (IFO)
        "Mod_OnHeartbeat", "Mod_OnModLoad", "Mod_OnModStart", "Mod_OnClientEntr",
        "Mod_OnClientLeav", "Mod_OnActvtItem", "Mod_OnAcquirItem", "Mod_OnUsrDefined",
        "Mod_OnUnAqreItem", "Mod_OnPlrDeath", "Mod_OnPlrDying", "Mod_OnPlrLvlUp",
        "Mod_OnSpawnBtnDn", "Mod_OnPlrRest",
    };
    return labels;
}

std::string textField(const Gff& gff, std::uint32_t structure, const char* label)
{
    const GffField* field = gff.field(structure, label);
    return field != nullptr ? gff.text(*field) : std::string{};
}

float numberField(const Gff& gff, std::uint32_t structure, const char* label, float fallback = 0.0f)
{
    const GffField* field = gff.field(structure, label);
    return field != nullptr ? gff.number(*field) : fallback;
}

std::int32_t intField(const Gff& gff, std::uint32_t structure, const char* label, std::int32_t fallback = 0)
{
    const GffField* field = gff.field(structure, label);
    return field != nullptr ? gff.integer(*field) : fallback;
}

std::string locField(const Gff& gff, std::uint32_t structure, const char* label, const Tlk* tlk)
{
    const GffField* field = gff.field(structure, label);
    if (field == nullptr) {
        return {};
    }
    if (field->type != GffType::LocString) {
        return gff.text(*field);
    }
    const std::uint32_t strref = gff.locStringStrref(*field);
    if (tlk != nullptr && strref != UINT32_MAX) {
        const std::string resolved = tlk->text(strref);
        if (!resolved.empty()) {
            return resolved;
        }
    }
    return gff.locString(*field);
}

std::optional<Tlk> loadTlk(Install& install)
{
    const std::filesystem::path path = install.root() / "dialog.tlk";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return std::nullopt;
    }
    try {
        return Tlk::parse(std::move(bytes), "dialog.tlk");
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

std::optional<TwoDa> loadTable(Install& install, const char* name)
{
    auto data = install.find(name, resType(ResType::TwoDa));
    if (!data.has_value()) {
        return std::nullopt;
    }
    try {
        return TwoDa::parse(std::move(*data), name);
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

std::uint32_t ensureTexture(Scene& scene, Install& install, const std::string& rawName)
{
    const std::string name = lower(rawName);
    if (name.empty()) {
        return 0;
    }
    for (std::size_t i = 0; i < scene.textures.size(); ++i) {
        if (lower(scene.textures[i].name) == name) {
            return static_cast<std::uint32_t>(i);
        }
    }
    std::uint32_t type = 0;
    auto data = install.findTexture(name, &type);
    if (!data.has_value()) {
        return 0;
    }
    try {
        Texture texture;
        texture.name = name;
        texture.image = decodeTexture(*data, type, name);
        scene.textures.push_back(std::move(texture));
        return static_cast<std::uint32_t>(scene.textures.size() - 1);
    } catch (const FormatError&) {
        return 0;
    }
}

bool loadModel(Install& install, const std::string& modelName, std::vector<MdlTriangle>& triangles,
               std::string& error)
{
    if (modelName.empty()) {
        error = "empty model name";
        return false;
    }
    auto mdl = install.find(modelName, resType(ResType::Mdl));
    auto mdx = install.find(modelName, resType(ResType::Mdx));
    if (!mdl.has_value() || !mdx.has_value()) {
        error = "missing MDL/MDX for " + modelName;
        return false;
    }
    try {
        Mdl model = Mdl::parse(std::move(*mdl), std::move(*mdx), lower(modelName) + ".mdl");
        triangles = model.triangles({0.0f, 0.0f, 0.0f});
        // A parsed model may legitimately have no trimesh geometry (smoke,
        // steam and other emitter placeables). That is not a load failure.
        return true;
    } catch (const FormatError& exception) {
        error = exception.what();
        return false;
    }
}

void buildMesh(Scene& scene, Install& install, std::uint32_t entityIndex,
               const std::vector<MdlTriangle>& triangles)
{
    std::map<std::string, std::vector<MdlTriangle>> byTexture;
    for (const MdlTriangle& triangle : triangles) {
        std::string texture = lower(triangle.texture);
        if (texture.empty()) {
            texture = "__default";
        }
        byTexture[texture].push_back(triangle);
    }
    EntityMesh mesh;
    mesh.entity = entityIndex;
    for (const auto& [textureName, list] : byTexture) {
        const std::uint32_t texture = ensureTexture(scene, install, textureName);
        TextureBatch batch;
        batch.texture = texture;
        batch.firstIndex = static_cast<std::uint32_t>(mesh.indices.size());
        for (const MdlTriangle& triangle : list) {
            for (int i = 0; i < 3; ++i) {
                SceneVertex vertex;
                vertex.position = triangle.p[i];
                vertex.normal = triangle.normal;
                vertex.uv = triangle.uv[i];
                mesh.vertices.push_back(vertex);
                mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size() - 1));
            }
        }
        batch.indexCount = static_cast<std::uint32_t>(mesh.indices.size()) - batch.firstIndex;
        if (batch.indexCount != 0) {
            mesh.batches.push_back(batch);
        }
    }
    if (!mesh.vertices.empty()) {
        scene.entityMeshes.push_back(std::move(mesh));
    }
}

void applyEquippedWeapon(Entity& entity, Install& install, const Gff& utc,
                         const TwoDa& baseitems)
{
    const GffField* list = utc.field(utc.rootIndex(), "Equip_ItemList");
    if (list == nullptr) {
        return;
    }
    for (const std::uint32_t item : utc.list(*list)) {
        std::int32_t baseItem = intField(utc, item, "BaseItem", -1);
        if (baseItem < 0) {
            const std::string resref = textField(utc, item, "EquippedRes");
            if (!resref.empty()) {
                if (const std::optional<ItemTemplate> templ = loadItemTemplate(install, resref);
                    templ.has_value()) {
                    baseItem = templ->baseItem;
                }
            }
        }
        if (baseItem < 0) {
            continue;
        }
        const std::string row = std::to_string(baseItem);
        if (baseitems.rowIndex(row) < 0) {
            continue;
        }
        if (baseitems.cell(row, "weapontype") == "4") {
            entity.ranged = true;
            entity.attackRange = 15.0f;
            return;
        }
    }
}

std::string creatureModel(const TwoDa& appearances, std::uint32_t appearance, std::string& texture)
{
    const std::string row = std::to_string(appearance);
    if (appearances.rowIndex(row) < 0) {
        return {};
    }
    texture = appearances.cell(row, "texa");
    if (texture.empty()) {
        texture = appearances.cell(row, "racetex");
    }
    std::string model = appearances.cell(row, "modela");
    if (model.empty()) {
        model = appearances.cell(row, "race");
    }
    return model;
}

void addTriggerGeometry(Scene& scene, const Gff& git)
{    std::size_t triggerOrdinal = 0;
    for (const std::uint32_t structure : [&]() {
             const GffField* field = git.field(git.rootIndex(), "TriggerList");
             return field != nullptr ? git.list(*field) : std::vector<std::uint32_t>{};
         }()) {
        std::size_t entityIndex = scene.entities.size();
        std::size_t seen = 0;
        for (std::size_t i = 0; i < scene.entities.size(); ++i) {
            if (scene.entities[i].kind != EntityKind::Trigger) {
                continue;
            }
            if (seen++ == triggerOrdinal) {
                entityIndex = i;
                break;
            }
        }
        ++triggerOrdinal;
        if (entityIndex >= scene.entities.size()) {
            continue;
        }
        const Entity& entity = scene.entities[entityIndex];
        TriggerVolume volume;
        volume.entity = static_cast<std::uint32_t>(entityIndex);
        const GffField* geometryField = git.field(structure, "Geometry");
        if (geometryField == nullptr) {
            continue;
        }
        for (const std::uint32_t point : git.list(*geometryField)) {
            volume.vertices.push_back({entity.position.x + numberField(git, point, "PointX"),
                                       entity.position.y + numberField(git, point, "PointY"),
                                       entity.position.z + numberField(git, point, "PointZ")});
        }
        if (volume.vertices.size() >= 3) {
            for (std::size_t i = 1; i + 1 < volume.vertices.size(); ++i) {
                volume.indices.push_back(0);
                volume.indices.push_back(static_cast<std::uint32_t>(i));
                volume.indices.push_back(static_cast<std::uint32_t>(i + 1));
            }
            scene.triggers.push_back(std::move(volume));
        }
    }
}

void addEncounterGeometry(Scene& scene, Install& install, const Gff& git)
{
    std::size_t encounterOrdinal = 0;
    for (const std::uint32_t structure : [&]() {
             const GffField* field = git.field(git.rootIndex(), "Encounter List");
             return field != nullptr ? git.list(*field) : std::vector<std::uint32_t>{};
         }()) {
        std::size_t entityIndex = scene.entities.size();
        std::size_t seen = 0;
        for (std::size_t i = 0; i < scene.entities.size(); ++i) {
            if (scene.entities[i].kind != EntityKind::Encounter) {
                continue;
            }
            if (seen++ == encounterOrdinal) {
                entityIndex = i;
                break;
            }
        }
        ++encounterOrdinal;
        if (entityIndex >= scene.entities.size()) {
            continue;
        }
        const Entity& entity = scene.entities[entityIndex];
        EncounterVolume volume;
        volume.entity = static_cast<std::uint32_t>(entityIndex);
        volume.templateResref = entity.templateResref;

        auto data = install.find(entity.templateResref, 2040); // UTE
        if (!data.has_value()) {
            continue;
        }
        try {
            Gff ute = Gff::parse(std::move(*data), entity.templateResref + ".ute");
            EncounterTemplate& encounter = volume.templateData;
            encounter.resref = entity.templateResref;
            encounter.tag = textField(ute, ute.rootIndex(), "Tag");
            encounter.active = intField(ute, ute.rootIndex(), "Active", 1) != 0;
            encounter.difficulty = intField(ute, ute.rootIndex(), "Difficulty", 0);
            encounter.maxCreatures = intField(ute, ute.rootIndex(), "MaxCreatures", 0);
            encounter.recCreatures = intField(ute, ute.rootIndex(), "RecCreatures", 0);
            encounter.resetTime = intField(ute, ute.rootIndex(), "ResetTime", 0);
            encounter.respawns = intField(ute, ute.rootIndex(), "Respawns", 0);
            encounter.spawnOption = intField(ute, ute.rootIndex(), "SpawnOption", 0);
            if (const GffField* list = ute.field(ute.rootIndex(), "CreatureList"); list != nullptr) {
                for (const std::uint32_t creatureStruct : ute.list(*list)) {
                    EncounterCreature creature;
                    creature.resref = textField(ute, creatureStruct, "ResRef");
                    creature.appearance = intField(ute, creatureStruct, "Appearance", 0);
                    creature.challengeRating = numberField(ute, creatureStruct, "CR", 0.0f);
                    creature.singleSpawn = intField(ute, creatureStruct, "SingleSpawn", 0) != 0;
                    if (!creature.resref.empty()) {
                        encounter.creatures.push_back(std::move(creature));
                    }
                }
            }
        } catch (const FormatError&) {
            continue;
        }

        if (const GffField* geometryField = git.field(structure, "Geometry");
            geometryField != nullptr) {
            for (const std::uint32_t point : git.list(*geometryField)) {
                volume.vertices.push_back({entity.position.x + numberField(git, point, "X"),
                                           entity.position.y + numberField(git, point, "Y"),
                                           entity.position.z + numberField(git, point, "Z")});
            }
            if (volume.vertices.size() >= 3) {
                for (std::size_t i = 1; i + 1 < volume.vertices.size(); ++i) {
                    volume.indices.push_back(0);
                    volume.indices.push_back(static_cast<std::uint32_t>(i));
                    volume.indices.push_back(static_cast<std::uint32_t>(i + 1));
                }
            }
        }
        if (const GffField* spawnField = git.field(structure, "SpawnPointList");
            spawnField != nullptr) {
            for (const std::uint32_t point : git.list(*spawnField)) {
                volume.spawnPoints.push_back({
                    {numberField(git, point, "X"), numberField(git, point, "Y"),
                     numberField(git, point, "Z")},
                    numberField(git, point, "Orientation", 0.0f)});
            }
        }
        scene.encounters.push_back(std::move(volume));
    }
}

} // namespace

void collectScriptHooks(const Gff& gff, std::uint32_t structure,
                        std::map<std::string, std::string>& scripts)
{
    if (structure >= gff.structureCount()) {
        return;
    }
    for (const std::string& label : scriptHookLabels()) {
        const GffField* field = gff.field(structure, label);
        if (field == nullptr) {
            continue;
        }
        const std::string resref = gff.text(*field);
        if (!resref.empty()) {
            scripts[label] = resref;
        }
    }
}

std::optional<ItemTemplate> loadItemTemplate(Install& install, const std::string& resref)
{
    auto data = install.find(resref, 2025); // UTI
    if (!data.has_value()) {
        return std::nullopt;
    }
    try {
        Gff uti = Gff::parse(std::move(*data), resref + ".uti");
        ItemTemplate item;
        item.resref = resref;
        item.tag = textField(uti, uti.rootIndex(), "Tag");
        item.baseItem = intField(uti, uti.rootIndex(), "BaseItem", 0);
        item.cost = intField(uti, uti.rootIndex(), "Cost", 0);
        item.stackSize = std::max(1, intField(uti, uti.rootIndex(), "StackSize", 1));
        item.identified = intField(uti, uti.rootIndex(), "Identified", 1) != 0;
        const std::optional<Tlk> tlk = loadTlk(install);
        item.name = locField(uti, uti.rootIndex(), "LocalizedName", tlk ? &*tlk : nullptr);
        return item;
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

std::size_t spawnCreature(Scene& scene, Install& install, const std::string& utcResref,
                          Vec3 position, float bearing)
{
    auto data = install.find(utcResref, 2027); // UTC
    if (!data.has_value()) {
        return static_cast<std::size_t>(-1);
    }
    try {
        Gff utc = Gff::parse(std::move(*data), utcResref + ".utc");
        const std::uint32_t root = utc.rootIndex();
        const std::optional<Tlk> tlk = loadTlk(install);
        const std::optional<TwoDa> appearances = loadTable(install, "appearance");
        const std::optional<TwoDa> baseitems = loadTable(install, "baseitems");

        Entity entity;
        entity.kind = EntityKind::Creature;
        entity.sourceStructure = root;
        entity.templateResref = utcResref;
        entity.position = position;
        entity.bearing = bearing;
        entity.tag = textField(utc, root, "Tag");
        if (entity.tag.empty()) {
            entity.tag = utcResref;
        }
        entity.name = locField(utc, root, "FirstName", tlk ? &*tlk : nullptr);
        entity.conversation = textField(utc, root, "Conversation");
        entity.appearance = static_cast<std::uint32_t>(
            intField(utc, root, "Appearance_Type", 0));
        entity.faction = static_cast<std::uint32_t>(intField(utc, root, "FactionID", 0));
        entity.hitPoints = intField(utc, root, "HitPoints", 1);
        entity.maxHitPoints = intField(utc, root, "MaxHitPoints", entity.hitPoints);
        if (entity.maxHitPoints <= 0) {
            entity.maxHitPoints = 1;
        }
        entity.hitPoints = std::max(1, std::min(entity.hitPoints, entity.maxHitPoints));
        const std::int32_t dexterity = intField(utc, root, "Dex", 10);
        entity.defense = 10 + (dexterity - 10) / 2 + intField(utc, root, "NaturalAC", 0);
        entity.fortSave = intField(utc, root, "FortSaveThrow", 0);
        entity.willSave = intField(utc, root, "WillSaveThrow", 0);
        entity.refSave = intField(utc, root, "RefSaveThrow", 0);
        entity.attackBonus = 2 + std::max(0, static_cast<int>(numberField(utc, root,
                                                                        "ChallengeRating", 0.0f)));
        entity.damageMin = 1;
        entity.damageMax = 6;
        entity.scriptDialogue = textField(utc, root, "ScriptDialogue");
        entity.scriptOnEnter = textField(utc, root, "ScriptOnNotice");
        entity.walkRate = 3.0f + numberField(utc, root, "WalkRate", 7.0f) * 0.15f;
        entity.hostile = entity.faction == 4;
        if (baseitems.has_value()) {
            applyEquippedWeapon(entity, install, utc, *baseitems);
        }
        collectScriptHooks(utc, root, entity.scripts);
        if (appearances.has_value()) {
            std::string texture;
            entity.model = creatureModel(*appearances, entity.appearance, texture);
            entity.texture = texture;
        }
        scene.entities.push_back(std::move(entity));
        const std::size_t index = scene.entities.size() - 1;
        if (!scene.entities[index].model.empty()) {
            std::vector<MdlTriangle> triangles;
            std::string error;
            if (loadModel(install, scene.entities[index].model, triangles, error)) {
                buildMesh(scene, install, static_cast<std::uint32_t>(index), triangles);
            } else {
                scene.warnings.push_back(scene.entities[index].model + ": " + error);
            }
        }
        return index;
    } catch (const FormatError&) {
        return static_cast<std::size_t>(-1);
    }
}

void loadEntityAssets(Scene& scene, Install& install, const Gff& git)
{
    const std::optional<Tlk> tlk = loadTlk(install);
    const std::optional<TwoDa> appearances = loadTable(install, "appearance");
    const std::optional<TwoDa> genericDoors = loadTable(install, "genericdoors");
    const std::optional<TwoDa> placeables = loadTable(install, "placeables");
    const std::optional<TwoDa> baseitems = loadTable(install, "baseitems");

    for (std::size_t index = 0; index < scene.entities.size(); ++index) {
        Entity& entity = scene.entities[index];
        const std::uint32_t structure = entity.sourceStructure;
        if (entity.kind == EntityKind::Creature) {
            auto data = install.find(entity.templateResref, 2027); // UTC
            if (data.has_value()) {
                try {
                    Gff utc = Gff::parse(std::move(*data), entity.templateResref + ".utc");
                    collectScriptHooks(utc, utc.rootIndex(), entity.scripts);
                    entity.tag = textField(utc, utc.rootIndex(), "Tag");
                    entity.name = locField(utc, utc.rootIndex(), "FirstName", tlk ? &*tlk : nullptr);
                    entity.conversation = textField(utc, utc.rootIndex(), "Conversation");
                    entity.appearance = static_cast<std::uint32_t>(
                        intField(utc, utc.rootIndex(), "Appearance_Type", 0));
                    entity.faction = static_cast<std::uint32_t>(intField(utc, utc.rootIndex(), "FactionID", 0));
                    entity.hitPoints = intField(utc, utc.rootIndex(), "HitPoints", 1);
                    entity.maxHitPoints = intField(utc, utc.rootIndex(), "MaxHitPoints", entity.hitPoints);
                    if (entity.maxHitPoints <= 0) {
                        entity.maxHitPoints = 1;
                    }
                    entity.hitPoints = std::max(1, std::min(entity.hitPoints, entity.maxHitPoints));
                    const std::int32_t dexterity = intField(utc, utc.rootIndex(), "Dex", 10);
                    entity.defense = 10 + (dexterity - 10) / 2 + intField(utc, utc.rootIndex(), "NaturalAC", 0);
                    entity.fortSave = intField(utc, utc.rootIndex(), "FortSaveThrow", 0);
                    entity.willSave = intField(utc, utc.rootIndex(), "WillSaveThrow", 0);
                    entity.refSave = intField(utc, utc.rootIndex(), "RefSaveThrow", 0);
                    entity.attackBonus = 2 + std::max(0, static_cast<int>(numberField(utc, utc.rootIndex(),
                                                                                    "ChallengeRating", 0.0f)));
                    entity.damageMin = 1;
                    entity.damageMax = 6;
                    entity.scriptDialogue = textField(utc, utc.rootIndex(), "ScriptDialogue");
                    entity.scriptOnEnter = textField(utc, utc.rootIndex(), "ScriptOnNotice");
                    entity.walkRate = 3.0f + numberField(utc, utc.rootIndex(), "WalkRate", 7.0f) * 0.15f;
                    entity.hostile = entity.faction == 4; // Sith faction in the Endar Spire slice
                    if (baseitems.has_value()) {
                        applyEquippedWeapon(entity, install, utc, *baseitems);
                    }
                    if (appearances.has_value()) {
                        std::string texture;
                        entity.model = creatureModel(*appearances, entity.appearance, texture);
                        entity.texture = texture;
                    }
                } catch (const FormatError&) {
                }
            }
        } else if (entity.kind == EntityKind::Door) {
            auto data = install.find(entity.templateResref, 2042); // UTD
            if (data.has_value()) {
                try {
                    Gff utd = Gff::parse(std::move(*data), entity.templateResref + ".utd");
                    collectScriptHooks(utd, utd.rootIndex(), entity.scripts);
                    entity.tag = textField(utd, utd.rootIndex(), "Tag");
                    entity.faction = static_cast<std::uint32_t>(intField(utd, utd.rootIndex(), "Faction", 0));
                    const std::uint32_t genericType = static_cast<std::uint32_t>(
                        intField(utd, utd.rootIndex(), "GenericType", 0));
                    entity.appearance = genericType;
                    if (genericDoors.has_value()) {
                        entity.model = genericDoors->cell(std::to_string(genericType), "modelname");
                    }
                } catch (const FormatError&) {
                }
            }
        } else if (entity.kind == EntityKind::Placeable) {
            auto data = install.find(entity.templateResref, 2044); // UTP
            if (data.has_value()) {
                try {
                    Gff utp = Gff::parse(std::move(*data), entity.templateResref + ".utp");
                    collectScriptHooks(utp, utp.rootIndex(), entity.scripts);
                    entity.tag = textField(utp, utp.rootIndex(), "Tag");
                    entity.conversation = textField(utp, utp.rootIndex(), "Conversation");
                    entity.appearance = static_cast<std::uint32_t>(
                        intField(utp, utp.rootIndex(), "Appearance", 0));
                    entity.faction = static_cast<std::uint32_t>(intField(utp, utp.rootIndex(), "Faction", 0));
                    entity.useable = intField(utp, utp.rootIndex(), "Useable", 0) != 0
                        || !entity.conversation.empty();
                    if (const GffField* list = utp.field(utp.rootIndex(), "ItemList"); list != nullptr) {
                        for (const std::uint32_t itemStruct : utp.list(*list)) {
                            const std::string resref = textField(utp, itemStruct, "InventoryRes");
                            if (!resref.empty()) {
                                entity.lootItems.push_back(resref);
                            }
                        }
                    }
                    entity.useable = entity.useable || !entity.lootItems.empty();
                    if (placeables.has_value()) {
                        entity.model = placeables->cell(std::to_string(entity.appearance), "modelname");
                    }
                } catch (const FormatError&) {
                }
            }
        } else if (entity.kind == EntityKind::Trigger) {
            auto data = install.find(entity.templateResref, 2032); // UTT
            if (data.has_value()) {
                try {
                    Gff utt = Gff::parse(std::move(*data), entity.templateResref + ".utt");
                    collectScriptHooks(utt, utt.rootIndex(), entity.scripts);
                    entity.tag = textField(utt, utt.rootIndex(), "Tag");
                    entity.scriptOnEnter = textField(utt, utt.rootIndex(), "ScriptOnEnter");
                    entity.scriptOnExit = textField(utt, utt.rootIndex(), "ScriptOnExit");
                } catch (const FormatError&) {
                }
            }
        } else if (entity.kind == EntityKind::Store) {
            auto data = install.find(entity.templateResref, 2051); // UTM
            if (data.has_value()) {
                try {
                    Gff utm = Gff::parse(std::move(*data), entity.templateResref + ".utm");
                    collectScriptHooks(utm, utm.rootIndex(), entity.scripts);
                    entity.tag = textField(utm, utm.rootIndex(), "Tag");
                    entity.name = locField(utm, utm.rootIndex(), "LocName", tlk ? &*tlk : nullptr);
                    entity.storeMarkUp = intField(utm, utm.rootIndex(), "MarkUp", 100);
                    entity.storeMarkDown = intField(utm, utm.rootIndex(), "MarkDown", 40);
                    entity.storeBuySell = intField(utm, utm.rootIndex(), "BuySellFlag", 3);
                    entity.useable = true;
                    if (const GffField* list = utm.field(utm.rootIndex(), "ItemList"); list != nullptr) {
                        for (const std::uint32_t itemStruct : utm.list(*list)) {
                            const std::string resref = textField(utm, itemStruct, "InventoryRes");
                            if (!resref.empty()) {
                                entity.storeItems.emplace_back(
                                    resref, intField(utm, itemStruct, "Infinite", 0) != 0);
                            }
                        }
                    }
                } catch (const FormatError&) {
                }
            }
        } else if (entity.kind == EntityKind::Encounter) {
            auto data = install.find(entity.templateResref, 2040); // UTE
            if (data.has_value()) {
                try {
                    Gff ute = Gff::parse(std::move(*data), entity.templateResref + ".ute");
                    collectScriptHooks(ute, ute.rootIndex(), entity.scripts);
                    entity.tag = textField(ute, ute.rootIndex(), "Tag");
                    entity.name = locField(ute, ute.rootIndex(), "LocalizedName",
                                           tlk ? &*tlk : nullptr);
                } catch (const FormatError&) {
                }
            }
        }
        collectScriptHooks(git, structure, entity.scripts);
        if (!entity.model.empty()
            && (entity.kind == EntityKind::Creature || entity.kind == EntityKind::Door
                || entity.kind == EntityKind::Placeable)) {
            std::vector<MdlTriangle> triangles;
            std::string error;
            if (loadModel(install, entity.model, triangles, error)) {
                buildMesh(scene, install, static_cast<std::uint32_t>(index), triangles);
            } else {
                scene.warnings.push_back(entity.model + ": " + error);
            }
        }
    }
    addTriggerGeometry(scene, git);
    addEncounterGeometry(scene, install, git);
}

} // namespace ke::kotor
