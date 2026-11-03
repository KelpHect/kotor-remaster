#include "kotor/Formats.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace ke::kotor {

namespace {

const GffField* field(const Gff& gff, std::uint32_t structure, const char* label)
{
    return gff.field(structure, label);
}

std::string text(const Gff& gff, std::uint32_t structure, const char* label, const std::string& fallback = {})
{
    const GffField* f = field(gff, structure, label);
    return f != nullptr ? gff.text(*f) : fallback;
}

float number(const Gff& gff, std::uint32_t structure, const char* label, float fallback = 0.0f)
{
    const GffField* f = field(gff, structure, label);
    return f != nullptr ? gff.number(*f) : fallback;
}

Vec3 packedColor(const Gff& gff, std::uint32_t structure, const char* label, Vec3 fallback)
{
    const GffField* f = field(gff, structure, label);
    if (f == nullptr || (f->type != GffType::UInt32 && f->type != GffType::Int32)) {
        return fallback;
    }
    const std::uint32_t value = f->data;
    if (value == 0u) {
        return fallback;
    }
    return {static_cast<float>(value & 0xFFu) / 255.0f,
            static_cast<float>((value >> 8) & 0xFFu) / 255.0f,
            static_cast<float>((value >> 16) & 0xFFu) / 255.0f};
}

std::vector<std::uint32_t> list(const Gff& gff, std::uint32_t structure, const char* label)
{
    const GffField* f = field(gff, structure, label);
    return f != nullptr ? gff.list(*f) : std::vector<std::uint32_t>{};
}

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

Image checkerboard()
{
    Image image;
    image.width = 64;
    image.height = 64;
    image.rgba.assign(64u * 64u * 4u, 255u);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const bool a = ((x / 8) + (y / 8)) % 2 == 0;
            const std::size_t o = (static_cast<std::size_t>(y) * 64u + x) * 4u;
            image.rgba[o] = static_cast<std::uint8_t>(a ? 190 : 80);
            image.rgba[o + 1] = static_cast<std::uint8_t>(a ? 190 : 80);
            image.rgba[o + 2] = static_cast<std::uint8_t>(a ? 200 : 90);
            image.rgba[o + 3] = 255;
        }
    }
    return image;
}

void addEntities(const Gff& git, std::uint32_t root, const char* listLabel, EntityKind kind, Scene& scene)
{
    for (const std::uint32_t structure : list(git, root, listLabel)) {
        Entity entity;
        entity.kind = kind;
        entity.sourceStructure = structure;
        entity.templateResref = text(git, structure, "TemplateResRef");
        entity.tag = text(git, structure, "Tag");
        entity.linkedModule = text(git, structure, "LinkedToModule");
        entity.linkedTo = text(git, structure, "LinkedTo");
        entity.linkedFlags = static_cast<std::uint32_t>(number(git, structure, "LinkedToFlags", 0.0f));
        entity.transitionDestination = text(git, structure, "TransitionDestin");
        const bool hasXYZ = field(git, structure, "X") != nullptr;
        const char* x = hasXYZ ? "X" : "XPosition";
        const char* y = hasXYZ ? "Y" : "YPosition";
        const char* z = hasXYZ ? "Z" : "ZPosition";
        entity.position = {number(git, structure, x, 0.0f), number(git, structure, y, 0.0f),
                           number(git, structure, z, 0.0f)};
        float bearing = number(git, structure, "Bearing", 0.0f);
        if (field(git, structure, "Bearing") == nullptr && field(git, structure, "XOrientation") != nullptr) {
            bearing = std::atan2(number(git, structure, "YOrientation", 0.0f),
                                 number(git, structure, "XOrientation", 1.0f));
        }
        entity.bearing = bearing;
        entity.model = entity.templateResref;
        scene.entities.push_back(std::move(entity));
    }
}

} // namespace

Scene loadModule(Install& install, const std::string& moduleStem)
{
    install.bindModule(moduleStem);
    install.addPacks();

    auto ifoData = install.find("module", resType(ResType::Ifo));
    if (!ifoData.has_value()) {
        throw FormatError(moduleStem + ": no IFO");
    }
    Gff ifo = Gff::parse(std::move(*ifoData), moduleStem + ".ifo");
    const std::vector<std::uint32_t> areas = list(ifo, ifo.rootIndex(), "Mod_Area_list");
    if (areas.empty()) {
        throw FormatError(moduleStem + ": IFO has no Mod_Area_list");
    }
    const std::string area = text(ifo, areas.front(), "Area_Name");
    if (area.empty()) {
        throw FormatError(moduleStem + ": IFO area has no Area_Name");
    }
    auto areData = install.find(area, resType(ResType::Are));
    auto gitData = install.find(area, resType(ResType::Git));
    auto lytData = install.find(area, resType(ResType::Lyt));
    if (!areData.has_value() || !gitData.has_value() || !lytData.has_value()) {
        throw FormatError(moduleStem + ": missing ARE/GIT/LYT for " + area);
    }
    Gff are = Gff::parse(std::move(*areData), area + ".are");
    Gff git = Gff::parse(std::move(*gitData), area + ".git");
    Lyt lyt = parseLyt(std::move(*lytData), area + ".lyt");

    Scene scene;
    scene.module = moduleStem;
    scene.area = area;
    scene.installPath = install.root().string();
    scene.entry = {number(ifo, ifo.rootIndex(), "Mod_Entry_X", 0.0f),
                   number(ifo, ifo.rootIndex(), "Mod_Entry_Y", 0.0f),
                   number(ifo, ifo.rootIndex(), "Mod_Entry_Z", 0.0f)};
    scene.entryDirection = {number(ifo, ifo.rootIndex(), "Mod_Entry_Dir_X", 0.0f),
                            number(ifo, ifo.rootIndex(), "Mod_Entry_Dir_Y", 1.0f), 0.0f};
    scene.ambient = packedColor(are, are.rootIndex(), "SunAmbientColor", scene.ambient);
    scene.diffuse = packedColor(are, are.rootIndex(), "SunDiffuseColor", scene.diffuse);
    scene.fogColor = packedColor(are, are.rootIndex(), "SunFogColor", scene.fogColor);
    scene.fogNear = number(are, are.rootIndex(), "FogNear", scene.fogNear);
    scene.fogFar = number(are, are.rootIndex(), "FogFar", scene.fogFar);
    scene.rooms = lyt.rooms;

    std::map<std::string, std::vector<MdlTriangle>> byTexture;
    std::uint32_t sourceBytes = 0;
    for (const LytRoom& room : lyt.rooms) {
        auto wokData = install.find(room.room, resType(ResType::Wok));
        auto mdlData = install.find(room.room, resType(ResType::Mdl));
        auto mdxData = install.find(room.room, resType(ResType::Mdx));
        if (wokData.has_value()) {
            Wok wok = parseWok(std::move(*wokData), room.room + ".wok");
            sourceBytes += static_cast<std::uint32_t>(wok.vertices.size() * 12u);
            const std::uint32_t base = static_cast<std::uint32_t>(scene.collisionVertices.size());
            scene.collisionVertices.insert(scene.collisionVertices.end(), wok.vertices.begin(), wok.vertices.end());
            for (const WokFace& face : wok.faces) {
                CollisionFacet facet;
                facet.a = base + face.a;
                facet.b = base + face.b;
                facet.c = base + face.c;
                facet.surface = face.surface;
                facet.normal = face.normal;
                facet.floor = face.floor;
                scene.collision.push_back(facet);
            }
        }
        if (!mdlData.has_value() || !mdxData.has_value()) {
            continue;
        }
        Mdl mdl = Mdl::parse(std::move(*mdlData), std::move(*mdxData), room.room + ".mdl");
        const std::vector<MdlTriangle> triangles = mdl.triangles(room.origin);
        sourceBytes += static_cast<std::uint32_t>(triangles.size() * 32u);
        for (const MdlTriangle& triangle : triangles) {
            std::string texture = lower(triangle.texture);
            if (texture.empty()) {
                texture = "__missing";
            }
            byTexture[texture].push_back(triangle);
        }
    }
    scene.sourceBytes = sourceBytes;

    scene.textures.push_back({"__default", checkerboard()});
    std::unordered_map<std::string, std::uint32_t> textureIndex;
    textureIndex["__default"] = 0;
    for (const auto& [name, triangles] : byTexture) {
        if (textureIndex.find(name) != textureIndex.end()) {
            continue;
        }
        std::uint32_t type = 0;
        std::string source;
        auto data = install.findTexture(name, &type, &source);
        if (!data.has_value()) {
            textureIndex[name] = 0;
            continue;
        }
        try {
            Texture texture;
            texture.name = name;
            texture.image = decodeTexture(*data, type, name);
            textureIndex[name] = static_cast<std::uint32_t>(scene.textures.size());
            scene.textures.push_back(std::move(texture));
        } catch (const FormatError&) {
            textureIndex[name] = 0;
        }
    }

    for (const auto& [name, triangles] : byTexture) {
        const std::uint32_t texture = textureIndex[name];
        TextureBatch batch;
        batch.texture = texture;
        batch.firstIndex = static_cast<std::uint32_t>(scene.indices.size());
        for (const MdlTriangle& triangle : triangles) {
            for (int i = 0; i < 3; ++i) {
                SceneVertex vertex;
                vertex.position = triangle.p[i];
                vertex.normal = triangle.normal;
                vertex.uv = triangle.uv[i];
                scene.vertices.push_back(vertex);
                scene.indices.push_back(static_cast<std::uint32_t>(scene.vertices.size() - 1));
            }
        }
        batch.indexCount = static_cast<std::uint32_t>(scene.indices.size()) - batch.firstIndex;
        if (batch.indexCount != 0) {
            scene.batches.push_back(batch);
        }
    }

    addEntities(git, git.rootIndex(), "Door List", EntityKind::Door, scene);
    addEntities(git, git.rootIndex(), "Placeable List", EntityKind::Placeable, scene);
    addEntities(git, git.rootIndex(), "Creature List", EntityKind::Creature, scene);
    addEntities(git, git.rootIndex(), "WaypointList", EntityKind::Waypoint, scene);
    addEntities(git, git.rootIndex(), "TriggerList", EntityKind::Trigger, scene);
    addEntities(git, git.rootIndex(), "Encounter List", EntityKind::Encounter, scene);
    addEntities(git, git.rootIndex(), "SoundList", EntityKind::Sound, scene);
    addEntities(git, git.rootIndex(), "StoreList", EntityKind::Store, scene);

    loadEntityAssets(scene, install, git);
    return scene;
}

} // namespace ke::kotor
