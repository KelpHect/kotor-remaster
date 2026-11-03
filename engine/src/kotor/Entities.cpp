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
        return !triangles.empty();
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
{
    std::size_t triggerOrdinal = 0;
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

} // namespace

void loadEntityAssets(Scene& scene, Install& install, const Gff& git)
{
    const std::optional<Tlk> tlk = loadTlk(install);
    const std::optional<TwoDa> appearances = loadTable(install, "appearance");
    const std::optional<TwoDa> genericDoors = loadTable(install, "genericdoors");
    const std::optional<TwoDa> placeables = loadTable(install, "placeables");

    for (std::size_t index = 0; index < scene.entities.size(); ++index) {
        Entity& entity = scene.entities[index];
        const std::uint32_t structure = entity.sourceStructure;
        if (entity.kind == EntityKind::Creature) {
            auto data = install.find(entity.templateResref, 2027); // UTC
            if (data.has_value()) {
                try {
                    Gff utc = Gff::parse(std::move(*data), entity.templateResref + ".utc");
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
                    entity.attackBonus = 2 + std::max(0, static_cast<int>(numberField(utc, utc.rootIndex(),
                                                                                    "ChallengeRating", 0.0f)));
                    entity.damageMin = 1;
                    entity.damageMax = 6;
                    entity.scriptDialogue = textField(utc, utc.rootIndex(), "ScriptDialogue");
                    entity.scriptOnEnter = textField(utc, utc.rootIndex(), "ScriptOnNotice");
                    entity.walkRate = 3.0f + numberField(utc, utc.rootIndex(), "WalkRate", 7.0f) * 0.15f;
                    entity.hostile = entity.faction == 4; // Sith faction in the Endar Spire slice
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
                    entity.tag = textField(utp, utp.rootIndex(), "Tag");
                    entity.conversation = textField(utp, utp.rootIndex(), "Conversation");
                    entity.appearance = static_cast<std::uint32_t>(
                        intField(utp, utp.rootIndex(), "Appearance", 0));
                    entity.faction = static_cast<std::uint32_t>(intField(utp, utp.rootIndex(), "Faction", 0));
                    entity.useable = !entity.conversation.empty();
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
                    entity.tag = textField(utt, utt.rootIndex(), "Tag");
                    entity.scriptOnEnter = textField(utt, utt.rootIndex(), "ScriptOnEnter");
                    entity.scriptOnExit = textField(utt, utt.rootIndex(), "ScriptOnExit");
                } catch (const FormatError&) {
                }
            }
        }
        (void)structure;
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
}

} // namespace ke::kotor
