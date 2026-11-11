#include "kotor/Formats.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <sstream>

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

std::string tableResource(const std::shared_ptr<const TwoDa>& table, std::int32_t id)
{
    if (table == nullptr || id < 0) {
        return {};
    }
    const std::string row = std::to_string(id);
    if (table->rowIndex(row) < 0) {
        return {};
    }
    return table->cell(row, "resource");
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
        if (entity.templateResref.empty()) {
            entity.templateResref = text(git, structure, "ResRef");
        }
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
        if (kind == EntityKind::Waypoint) {
            entity.hasMapNote = number(git, structure, "HasMapNote", 0.0f) != 0.0f;
            entity.mapNoteEnabled = number(git, structure, "MapNoteEnabled", 0.0f) != 0.0f;
            if (const GffField* note = git.field(structure, "MapNote")) {
                entity.mapNoteStrref = git.locStringStrref(*note);
                entity.mapNote = git.locString(*note);
            }
        }
        scene.entities.push_back(std::move(entity));
    }
}

void addCameras(const Gff& git, std::uint32_t root, Scene& scene)
{
    const GffField* listField = git.field(root, "CameraList");
    if (listField == nullptr || listField->type != GffType::List) {
        return;
    }
    for (const std::uint32_t structure : git.list(*listField)) {
        CameraDef camera;
        if (const GffField* field = git.field(structure, "CameraID")) {
            camera.id = static_cast<std::uint32_t>(std::max(0, git.integer(*field)));
        }
        if (const GffField* field = git.field(structure, "Position")) {
            camera.position = git.vector3(*field);
        }
        if (const GffField* field = git.field(structure, "Orientation")) {
            const std::array<float, 4> orientation = git.vector4(*field);
            // The GFF stores W X Y Z (yaw only: X and Y are 0 in all 1,099 shipped cameras).
            // Reading it as X Y Z W turned every camera into a roll about X; measured against
            // 1,744 static-camera dialogue lines, W-first puts the speaker in view in 83% of them.
            camera.qw = orientation[0];
            camera.qx = orientation[1];
            camera.qy = orientation[2];
            camera.qz = orientation[3];
        }
        if (const GffField* field = git.field(structure, "Pitch")) {
            camera.pitch = git.number(*field);
        }
        if (const GffField* field = git.field(structure, "Height")) {
            camera.height = git.number(*field);
        }
        if (const GffField* field = git.field(structure, "FieldOfView")) {
            camera.fov = git.number(*field);
        }
        scene.cameras.push_back(camera);
    }
}

void addAreaAudio(Install& install, const Gff& git, std::uint32_t root, Scene& scene)
{
    const GffField* field = git.field(root, "AreaProperties");
    if (field == nullptr || field->type != GffType::Struct) {
        return;
    }
    const std::uint32_t structure = git.structIndex(*field);
    AreaAudio& audio = scene.areaAudio;
    const auto integer = [&](const char* label, std::int32_t fallback) {
        const GffField* entry = git.field(structure, label);
        return entry != nullptr ? git.integer(*entry) : fallback;
    };
    audio.musicDayId = integer("MusicDay", -1);
    audio.musicNightId = integer("MusicNight", -1);
    audio.musicBattleId = integer("MusicBattle", -1);
    audio.ambientDayId = integer("AmbientSndDay", -1);
    audio.ambientNightId = integer("AmbientSndNight", -1);
    audio.ambientDayVolume = integer("AmbientSndDayVol", 0);
    audio.ambientNightVolume = integer("AmbientSndNitVol", 0);
    audio.musicDelay = integer("MusicDelay", 0);
    const std::int32_t eax = integer("EnvAudio", -1);

    const std::shared_ptr<const TwoDa> music = install.table("ambientmusic");
    const std::shared_ptr<const TwoDa> ambient = install.table("ambientsound");
    const std::shared_ptr<const TwoDa> eaxTable = install.table("soundeax");
    audio.musicDay = tableResource(music, audio.musicDayId);
    audio.musicNight = tableResource(music, audio.musicNightId);
    audio.musicBattle = tableResource(music, audio.musicBattleId);
    audio.ambientDay = tableResource(ambient, audio.ambientDayId);
    audio.ambientNight = tableResource(ambient, audio.ambientNightId);
    if (eaxTable != nullptr && eax >= 0) {
        const std::string row = std::to_string(eax);
        if (eaxTable->rowIndex(row) >= 0) {
            audio.eaxPreset = eaxTable->cell(row, "label");
        }
    }
}

// ARE "Rooms": the per-room EAX environment (EnvAudio) and AmbientScale.
void addRoomAudio(const Gff& are, std::uint32_t root, Scene& scene)
{
    for (const std::uint32_t structure : list(are, root, "Rooms")) {
        AreaRoomAudio room;
        room.room = text(are, structure, "RoomName");
        if (const GffField* env = field(are, structure, "EnvAudio")) {
            room.envAudio = are.integer(*env);
        }
        room.ambientScale = number(are, structure, "AmbientScale", 1.0f);
        if (!room.room.empty()) {
            scene.areaAudio.rooms.push_back(std::move(room));
        }
    }
}

void addSoundEmitters(Install& install, const Gff& git, std::uint32_t root, Scene& scene)
{
    const GffField* listField = git.field(root, "SoundList");
    if (listField == nullptr || listField->type != GffType::List) {
        return;
    }
    std::vector<std::uint32_t> soundEntities;
    for (std::uint32_t entity = 0; entity < scene.entities.size(); ++entity) {
        if (scene.entities[entity].kind == EntityKind::Sound) {
            soundEntities.push_back(entity);
        }
    }
    std::size_t soundEntityOrdinal = 0;
    for (const std::uint32_t structure : git.list(*listField)) {
        SoundEmitter emitter;
        emitter.templateResref = text(git, structure, "TemplateResRef");
        emitter.position = {number(git, structure, "XPosition", 0.0f),
                            number(git, structure, "YPosition", 0.0f),
                            number(git, structure, "ZPosition", 0.0f)};
        std::uint32_t soundEntity = 0;
        if (soundEntityOrdinal < soundEntities.size()) {
            soundEntity = soundEntities[soundEntityOrdinal++];
        }
        emitter.entity = soundEntity;

        // A saved sound struct holds the whole template (no TemplateResRef; retail Sound Load 005c9040).
        std::optional<Gff> ownedUts;
        const Gff* utsSource = nullptr;
        std::uint32_t utsRoot = 0;
        if (emitter.templateResref.empty()) {
            utsSource = &git;
            utsRoot = structure;
        } else if (auto utsData = install.find(emitter.templateResref, resType(ResType::Uts)); utsData.has_value()) {
            try {
                ownedUts.emplace(Gff::parse(std::move(*utsData), emitter.templateResref + ".uts"));
                utsSource = &*ownedUts;
                utsRoot = ownedUts->rootIndex();
            } catch (const FormatError&) {
            }
        }
        if (utsSource != nullptr) {
            try {
                const Gff& uts = *utsSource;
                emitter.elevation = number(uts, utsRoot, "Elevation", 0.0f);
                emitter.maxDistance = number(uts, utsRoot, "MaxDistance", 20.0f);
                emitter.minDistance = number(uts, utsRoot, "MinDistance", 10.0f);
                // Volume is 0..127 (retail 005c8df0 clamps to 0x7f; 395 shipped templates
                // reach exactly 127), the same scale as SoundObjectSetVolume.
                emitter.volume127 = std::clamp(
                    static_cast<int>(number(uts, utsRoot, "Volume", 127.0f)), 0, 127);
                emitter.volume = static_cast<float>(emitter.volume127) / 127.0f;
                emitter.volumeVariation = std::clamp(
                    static_cast<int>(number(uts, utsRoot, "VolumeVrtn", 0.0f)), 0, 127);
                emitter.times = static_cast<int>(number(uts, utsRoot, "Times", 3.0f));
                emitter.hours = static_cast<std::uint32_t>(
                    static_cast<std::int64_t>(number(uts, utsRoot, "Hours", 0.0f)));
                emitter.pitchVariation = number(uts, utsRoot, "PitchVariation", 0.0f);
                emitter.fixedVariance = number(uts, utsRoot, "FixedVariance", 1.0f);
                emitter.randomPosition = number(uts, utsRoot, "RandomPosition", 0.0f) != 0.0f;
                emitter.randomRangeX = number(uts, utsRoot, "RandomRangeX", 0.0f);
                emitter.randomRangeY = number(uts, utsRoot, "RandomRangeY", 0.0f);
                emitter.priority = static_cast<int>(number(uts, utsRoot, "Priority", 0.0f));
                emitter.active = number(uts, utsRoot, "Active", 1.0f) != 0.0f;
                emitter.continuous = number(uts, utsRoot, "Continuous", 0.0f) != 0.0f;
                emitter.looping = number(uts, utsRoot, "Looping", 0.0f) != 0.0f;
                emitter.positional = number(uts, utsRoot, "Positional", 1.0f) != 0.0f;
                emitter.random = number(uts, utsRoot, "Random", 0.0f) != 0.0f;
                emitter.interval = static_cast<int>(number(uts, utsRoot, "Interval", 0.0f));
                emitter.intervalVariation = static_cast<int>(
                    number(uts, utsRoot, "IntervalVrtn", 0.0f));
                if (const GffField* list = uts.field(utsRoot, "Sounds")) {
                    for (const std::uint32_t sound : uts.list(*list)) {
                        const std::string resref = text(uts, sound, "Sound");
                        if (!resref.empty()) {
                            emitter.sounds.push_back(resref);
                        }
                    }
                }
            } catch (const FormatError&) {
            }
        }
        scene.soundEmitters.push_back(std::move(emitter));
    }
}

// VIS: "<room> <count>" followed by that many indented room names.
std::vector<std::vector<std::uint32_t>> parseVis(const std::vector<std::uint8_t>& bytes,
                                                 const std::vector<LytRoom>& rooms)
{
    std::unordered_map<std::string, std::uint32_t> roomIndex;
    for (std::uint32_t i = 0; i < rooms.size(); ++i) {
        roomIndex[lower(rooms[i].room)] = i;
    }
    std::vector<std::vector<std::uint32_t>> visibility(rooms.size());
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::istringstream input(text);
    std::string line;
    std::int64_t current = -1;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string name;
        std::string count;
        if (!(row >> name)) {
            continue;
        }
        const auto it = roomIndex.find(lower(name));
        if (row >> count) {
            current = it != roomIndex.end() ? static_cast<std::int64_t>(it->second) : -1;
        } else if (current >= 0 && it != roomIndex.end()) {
            visibility[static_cast<std::size_t>(current)].push_back(it->second);
        }
    }
    return visibility;
}

} // namespace

std::int32_t readLoadScreenId(Install& install, const std::string& moduleStem)
{
    try {
        install.bindModule(moduleStem);
        install.addPacks();
        auto ifoData = install.find("module", resType(ResType::Ifo));
        if (!ifoData.has_value()) {
            return -1;
        }
        Gff ifo = Gff::parse(std::move(*ifoData), moduleStem + ".ifo");
        const std::vector<std::uint32_t> areas = list(ifo, ifo.rootIndex(), "Mod_Area_list");
        if (areas.empty()) {
            return -1;
        }
        const std::string area = text(ifo, areas.front(), "Area_Name");
        auto areData = install.find(area, resType(ResType::Are));
        if (area.empty() || !areData.has_value()) {
            return -1;
        }
        Gff are = Gff::parse(std::move(*areData), area + ".are");
        const GffField* field = are.field(are.rootIndex(), "LoadScreenID");
        return field != nullptr ? static_cast<std::int32_t>(are.integer(*field)) : 0;
    } catch (const FormatError&) {
        return -1;
    }
}

Scene loadModule(Install& install, const std::string& moduleStem, const std::vector<std::uint8_t>* savedGit)
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
    Gff git = savedGit != nullptr && !savedGit->empty() ? Gff::parse(*savedGit, area + ".git")
                                                        : Gff::parse(std::move(*gitData), area + ".git");
    Lyt lyt = parseLyt(std::move(*lytData), area + ".lyt");

    Scene scene;
    scene.module = moduleStem;
    scene.area = area;
    scene.areaTag = text(are, are.rootIndex(), "Tag");
    scene.unescapable = number(are, are.rootIndex(), "Unescapable", 0.0f) != 0.0f;
    if (const std::shared_ptr<const TwoDa> styles = install.table("camerastyle")) {
        const std::string row = std::to_string(static_cast<int>(number(are, are.rootIndex(), "CameraStyle", 0.0f)));
        const auto read = [&](const char* column, float& value) {
            const std::string cell = styles->rowIndex(row) >= 0 ? styles->cell(row, column) : std::string{};
            if (!cell.empty() && cell != "****") {
                value = std::strtof(cell.c_str(), nullptr);
            }
        };
        read("distance", scene.cameraStyle.distance);
        read("pitch", scene.cameraStyle.pitch);
        read("height", scene.cameraStyle.height);
        read("viewangle", scene.cameraStyle.viewAngle);
        read("fl_lookup", scene.cameraStyle.lookUp);
        read("fl_lookdown", scene.cameraStyle.lookDown);
    }
    if (const GffField* mapField = are.field(are.rootIndex(), "Map"); mapField != nullptr) {
        const std::uint32_t map = are.structIndex(*mapField);
        Scene::Minimap& minimap = scene.minimap;
        minimap.present = true;
        minimap.world1[0] = number(are, map, "WorldPt1X", 0.0f);
        minimap.world1[1] = number(are, map, "WorldPt1Y", 0.0f);
        minimap.world2[0] = number(are, map, "WorldPt2X", 0.0f);
        minimap.world2[1] = number(are, map, "WorldPt2Y", 0.0f);
        minimap.map1[0] = number(are, map, "MapPt1X", 0.0f);
        minimap.map1[1] = number(are, map, "MapPt1Y", 0.0f);
        minimap.map2[0] = number(are, map, "MapPt2X", 0.0f);
        minimap.map2[1] = number(are, map, "MapPt2Y", 0.0f);
        minimap.northAxis = static_cast<int>(number(are, map, "NorthAxis", 0.0f));
        minimap.resX = static_cast<int>(number(are, map, "MapResX", 0.0f));
        minimap.zoom = number(are, map, "MapZoom", 1.0f);
    }
    scene.installPath = install.root().string();
    collectScriptHooks(are, are.rootIndex(), scene.scripts);
    collectScriptHooks(ifo, ifo.rootIndex(), scene.scripts);
    scene.entry = {number(ifo, ifo.rootIndex(), "Mod_Entry_X", 0.0f),
                   number(ifo, ifo.rootIndex(), "Mod_Entry_Y", 0.0f),
                   number(ifo, ifo.rootIndex(), "Mod_Entry_Z", 0.0f)};
    scene.entryDirection = {number(ifo, ifo.rootIndex(), "Mod_Entry_Dir_X", 0.0f),
                            number(ifo, ifo.rootIndex(), "Mod_Entry_Dir_Y", 1.0f), 0.0f};
    // ARE sun and moon blocks (retail 00508c50 reads both); IsNight picks the active one.
    const bool night = number(are, are.rootIndex(), "IsNight", 0.0f) != 0.0f;
    const auto block = [&](const char* sun, const char* moon) { return night ? moon : sun; };
    const std::uint32_t areRoot = are.rootIndex();
    scene.ambient = packedColor(are, areRoot, block("SunAmbientColor", "MoonAmbientColor"), scene.ambient);
    scene.diffuse = packedColor(are, areRoot, block("SunDiffuseColor", "MoonDiffuseColor"), scene.diffuse);
    scene.dynamicAmbient = packedColor(are, areRoot, "DynAmbientColor", Vec3{});
    scene.fogColor = packedColor(are, areRoot, block("SunFogColor", "MoonFogColor"), scene.fogColor);
    scene.fogNear = number(are, areRoot, block("SunFogNear", "MoonFogNear"), scene.fogNear);
    scene.fogFar = number(are, areRoot, block("SunFogFar", "MoonFogFar"), scene.fogFar);
    scene.fogOn = number(are, areRoot, block("SunFogOn", "MoonFogOn"), 0.0f) != 0.0f;
    scene.alphaTest = number(are, areRoot, "AlphaTest", scene.alphaTest);
    scene.defaultEnvMap = text(are, areRoot, "DefaultEnvMap");
    scene.rooms = lyt.rooms;
    if (auto visData = install.find(area, resType(ResType::Vis)); visData.has_value()) {
        scene.visibility = parseVis(*visData, scene.rooms);
    }

    // Which surface materials can be walked on and which stop a line of sight. A
    // missing table falls back to the face normal alone.
    std::vector<char> surfaceWalk;
    std::vector<char> surfaceSight;
    std::vector<char> surfaceGrass;
    if (auto table = install.find("surfacemat", 2017); table.has_value()) {
        const TwoDa surfaces = TwoDa::parse(std::move(*table), "surfacemat.2da");
        const int walk = surfaces.columnIndex("walk");
        const int sight = surfaces.columnIndex("lineofsight");
        const int grass = surfaces.columnIndex("grass");
        for (std::size_t row = 0; row < surfaces.rowCount() && grass >= 0; ++row) {
            surfaceGrass.push_back(surfaces.cell(row, static_cast<std::size_t>(grass)) == "1" ? 1 : 0);
        }
        for (std::size_t row = 0; row < surfaces.rowCount() && walk >= 0 && sight >= 0; ++row) {
            surfaceWalk.push_back(surfaces.cell(row, static_cast<std::size_t>(walk)) == "1" ? 1 : 0);
            surfaceSight.push_back(surfaces.cell(row, static_cast<std::size_t>(sight)) == "1" ? 1 : 0);
        }
    }

    std::map<std::string, std::vector<MdlMeshTriangle>> byMaterial;
    std::vector<std::shared_ptr<Mdl>> roomModels;
    std::vector<std::size_t> roomModelRoom;
    std::vector<std::vector<std::string>> roomLoopClip;
    std::vector<bool> roomScript;
    std::uint32_t sourceBytes = 0;
    std::map<std::string, std::shared_ptr<const Mdl>> referenceModels;
    std::map<std::string, std::vector<MdlMeshTriangle>> byReference;
    struct ReferenceEmitters {
        std::shared_ptr<const Mdl> model;
        Vec3 origin;
        Mat3 rotation;
    };
    std::vector<ReferenceEmitters> referenceEmitters;
    std::size_t referencePlaced = 0;
    struct SkinnedReference {
        std::shared_ptr<const Mdl> model;
        std::size_t node = 0;
        std::size_t holder = 0;
        std::size_t room = 0;
    };
    std::vector<SkinnedReference> skinnedReferences;
    std::map<std::uint32_t, std::vector<MdlMeshTriangle>> grassWalkmesh; // room -> AABB faces
    for (std::size_t roomIndex = 0; roomIndex < lyt.rooms.size(); ++roomIndex) {
        const LytRoom& room = lyt.rooms[roomIndex];
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
                facet.room = static_cast<std::uint32_t>(roomIndex);
                facet.seamEdges = face.seamEdges;
                facet.walkable = face.surface < surfaceWalk.size() ? surfaceWalk[face.surface] != 0
                                                                   : face.floor;
                facet.blocksSight = face.surface < surfaceSight.size()
                    ? surfaceSight[face.surface] != 0
                    : true;
                scene.collision.push_back(facet);
            }
        }
        if (!mdlData.has_value() || !mdxData.has_value()) {
            continue;
        }
        Mdl mdl = Mdl::parse(std::move(*mdlData), std::move(*mdxData), room.room + ".mdl");
        roomScript.push_back(mdl.markRoomScriptMeshes());
        roomLoopClip.push_back(mdl.markRoomLoopMeshes());
        const std::string roomKey = std::to_string(roomIndex) + "#";
        std::vector<MdlMeshTriangle> triangles = mdl.roomTriangles(room.origin);
        for (MdlMeshTriangle& triangle : mdl.roomTriangles(room.origin, true)) {
            byReference[roomKey + "a#" + materialKey(triangle)].push_back(std::move(triangle));
        }
        grassWalkmesh[static_cast<std::uint32_t>(roomIndex)] = mdl.walkmeshTriangles(room.origin);
        sourceBytes += static_cast<std::uint32_t>(triangles.size() * 32u);
        for (MdlMeshTriangle& triangle : triangles) {
            byMaterial[roomKey + materialKey(triangle)].push_back(std::move(triangle));
        }
        for (const MdlLight& light : mdl.lights()) {
            Scene::WorldLight world;
            world.position = light.position + room.origin;
            world.colour = light.colour;
            world.multiplier = light.multiplier;
            world.radius = light.radius;
            world.ambientOnly = light.ambientOnly;
            world.priority = light.priority;
            world.dynamicType = light.dynamicType;
            world.shadow = light.shadow;
            world.fading = light.fading;
            world.shadowRadius = light.shadowRadius;
            world.verticalDisplacement = light.verticalDisplacement;
            world.room = static_cast<int>(roomIndex);
            world.node = light.node;
            // Shipped room lights carry 1 or 2 in the dynamic-type field; the next field is 0 on them all.
            if (light.dynamicType != 0 || light.ambientOnly) {
                scene.lights.push_back(world);
            }
        }
        // A reference node shows another whole model there, as its own lit object. Paired frame
        // w_unk_m41aa_b: every fern and most palms on the cliffs are or_* models placed this way.
        // Skinned references (c_bantha, c_brith, or_tbat) are posed each frame; the stunt crowds are
        // rigid and go through the path below. A referenced model's own lights count as the room's.
        for (const Mdl::ReferenceNode& reference : mdl.references()) {
            const std::string name = lower(reference.model);
            auto known = referenceModels.find(name);
            if (known == referenceModels.end()) {
                std::shared_ptr<const Mdl> loaded;
                auto refMdl = install.find(name, resType(ResType::Mdl));
                auto refMdx = install.find(name, resType(ResType::Mdx));
                if (refMdl.has_value() && refMdx.has_value()) {
                    loaded = std::make_shared<Mdl>(Mdl::parse(std::move(*refMdl), std::move(*refMdx), name + ".mdl"));
                }
                known = referenceModels.emplace(name, std::move(loaded)).first;
            }
            if (!known->second) {
                continue;
            }
            const Vec3 at = reference.position + room.origin;
            for (const MdlLight& light : known->second->lights()) {
                Scene::WorldLight world;
                const Mat3& turn = reference.rotation;
                world.position = at
                    + Vec3{turn.m[0][0] * light.position.x + turn.m[0][1] * light.position.y + turn.m[0][2] * light.position.z,
                           turn.m[1][0] * light.position.x + turn.m[1][1] * light.position.y + turn.m[1][2] * light.position.z,
                           turn.m[2][0] * light.position.x + turn.m[2][1] * light.position.y + turn.m[2][2] * light.position.z};
                world.colour = light.colour;
                world.multiplier = light.multiplier;
                world.radius = light.radius;
                world.ambientOnly = light.ambientOnly;
                world.priority = light.priority;
                world.dynamicType = light.dynamicType;
                world.shadow = light.shadow;
                world.fading = light.fading;
                world.shadowRadius = light.shadowRadius;
                world.verticalDisplacement = light.verticalDisplacement;
                if (light.dynamicType != 0 || light.ambientOnly) {
                    scene.lights.push_back(world);
                }
            }
            if (known->second->hasSkinnedMesh()) {
                skinnedReferences.push_back({known->second, reference.node, roomModels.size(), roomIndex});
                continue;
            }
            const std::string instanceKey = roomKey + std::to_string(referencePlaced++) + "#";
            for (MdlMeshTriangle& triangle : known->second->placedTriangles(at, reference.rotation)) {
                byReference[instanceKey + materialKey(triangle)].push_back(std::move(triangle));
            }
            referenceEmitters.push_back({known->second, at, reference.rotation});
        }
        roomModels.push_back(std::make_shared<Mdl>(std::move(mdl)));
        roomModelRoom.push_back(roomIndex);
    }
    scene.sourceBytes = sourceBytes;

    scene.textures.push_back({"__default", checkerboard()});
    scene.textures.back().clamp = 0;
    scene.textureIndex["__default"] = 0;

    const auto addBatches = [&](const std::map<std::string, std::vector<MdlMeshTriangle>>& groups, bool asObject) {
    for (const auto& [key, triangles] : groups) {
        TextureBatch batch = makeMaterialBatch(scene, install, triangles.front());
        batch.room = std::stoi(key.substr(0, key.find('#')));
        batch.roomObject = asObject;
        batch.firstIndex = static_cast<std::uint32_t>(scene.indices.size());
        Vec3 sum{};
        for (const MdlMeshTriangle& triangle : triangles) {
            for (int i = 0; i < 3; ++i) {
                sum = sum + triangle.v[i].position;
                scene.vertices.push_back(toSceneVertex(triangle.v[i], triangle));
                scene.indices.push_back(static_cast<std::uint32_t>(scene.vertices.size() - 1));
            }
        }
        batch.indexCount = static_cast<std::uint32_t>(scene.indices.size()) - batch.firstIndex;
        if (batch.indexCount == 0) {
            continue;
        }
        batch.center = sum * (1.0f / static_cast<float>(batch.indexCount));
        if (batch.lightmap == kNoTexture) {
            batch.bakedLight = true;
            batch.baked = asObject ? sampleSceneLight(scene, batch.center) : roomMeshLight(scene, batch.center, batch.blend != BlendMode::Opaque);
        }
        scene.batches.push_back(batch);
    }
    };
    addBatches(byMaterial, false);
    addBatches(byReference, true);
    GrassParams grass;
    grass.texture = text(are, areRoot, "Grass_TexName");
    grass.density = number(are, areRoot, "Grass_Density", 0.0f);
    grass.quadSize = number(are, areRoot, "Grass_QuadSize", 0.0f);
    grass.probLL = number(are, areRoot, "Grass_Prob_LL", 0.0f);
    grass.probLR = number(are, areRoot, "Grass_Prob_LR", 0.0f);
    grass.probUL = number(are, areRoot, "Grass_Prob_UL", 0.0f);
    grass.probUR = number(are, areRoot, "Grass_Prob_UR", 0.0f);
    grass.windPower = static_cast<int>(number(are, areRoot, "WindPower", 0.0f));
    grass.ambient = packedColor(are, areRoot, "Grass_Ambient", Vec3{});
    grass.diffuse = packedColor(are, areRoot, "Grass_Diffuse", Vec3{});
    addGrass(scene, install, grass, grassWalkmesh, surfaceGrass);

    const Mat3 identity{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
    for (std::size_t i = 0; i < roomModels.size(); ++i) {
        const std::size_t flaresBefore = scene.flareLights.size();
        addModelEmitters(scene, install, roomModels[i], UINT32_MAX,
                         lyt.rooms[roomModelRoom[i]].origin, identity);
        for (std::size_t f = flaresBefore; f < scene.flareLights.size(); ++f) {
            scene.flareLights[f].room = static_cast<int>(roomModelRoom[i]);
        }
    }
    for (const ReferenceEmitters& placed : referenceEmitters) {
        addModelEmitters(scene, install, placed.model, UINT32_MAX, placed.origin, placed.rotation);
    }

    for (const SkinnedReference& placed : skinnedReferences) {
        // The room's loop clip moves the node, when it has one.
        std::string holderClip;
        for (const char* name : {"animloop1", "animloop2", "animloop3"}) {
            if (holderClip.empty() && roomModels[placed.holder]->animationDuration(name) > 0.0f) {
                holderClip = name;
            }
        }
        addSkinnedReferenceMesh(scene, install, placed.model, roomModels[placed.holder], placed.node, holderClip,
                                lyt.rooms[placed.room].origin, static_cast<int>(placed.room));
    }
    for (std::size_t i = 0; i < roomModels.size() && i < roomScript.size(); ++i) {
        if (roomScript[i]) {
            addRoomScriptMesh(scene, install, roomModels[i], lyt.rooms[roomModelRoom[i]].origin,
                              static_cast<int>(roomModelRoom[i]));
        }
    }
    for (std::size_t i = 0; i < roomModels.size(); ++i) {
        for (const std::string& clip : roomLoopClip[i]) {
            addRoomLoopMesh(scene, install, roomModels[i], clip, lyt.rooms[roomModelRoom[i]].origin,
                            static_cast<int>(roomModelRoom[i]));
        }
    }

    if (!scene.vertices.empty()) {
        Vec3 low = scene.vertices.front().position;
        Vec3 high = low;
        for (const SceneVertex& vertex : scene.vertices) {
            low.x = std::min(low.x, vertex.position.x);
            low.y = std::min(low.y, vertex.position.y);
            low.z = std::min(low.z, vertex.position.z);
            high.x = std::max(high.x, vertex.position.x);
            high.y = std::max(high.y, vertex.position.y);
            high.z = std::max(high.z, vertex.position.z);
        }
        scene.viewExtent = length(high - low);
    }

    addEntities(git, git.rootIndex(), "Door List", EntityKind::Door, scene);
    addEntities(git, git.rootIndex(), "Placeable List", EntityKind::Placeable, scene);
    addEntities(git, git.rootIndex(), "Creature List", EntityKind::Creature, scene);
    addEntities(git, git.rootIndex(), "WaypointList", EntityKind::Waypoint, scene);
    addEntities(git, git.rootIndex(), "TriggerList", EntityKind::Trigger, scene);
    addEntities(git, git.rootIndex(), "Encounter List", EntityKind::Encounter, scene);
    addEntities(git, git.rootIndex(), "SoundList", EntityKind::Sound, scene);
    addEntities(git, git.rootIndex(), "StoreList", EntityKind::Store, scene);
    addCameras(git, git.rootIndex(), scene);
    addAreaAudio(install, git, git.rootIndex(), scene);
    addRoomAudio(are, are.rootIndex(), scene);
    addSoundEmitters(install, git, git.rootIndex(), scene);

    loadEntityAssets(scene, install, git);
    return scene;
}

namespace {

// planelineintersection as ClipProjection 0046f650 and ClipProjectionCaps 0046f580 use it: where
// the line through `from` and `to` meets the plane.
Vec3 planeLineIntersection(Vec3 normal, float distance, Vec3 from, Vec3 to)
{
    const Vec3 along = to - from;
    const float denominator = dot(normal, along);
    if (denominator == 0.0f) {
        return to;
    }
    const float t = -(dot(normal, from) + distance) / denominator;
    return from + along * t;
}

float planeSide(Vec3 normal, float distance, Vec3 point)
{
    return dot(normal, point) + distance;
}

Vec3 turnBy(const Mat3& m, Vec3 v)
{
    return {m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
            m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
            m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z};
}

Vec3 turnBackBy(const Mat3& m, Vec3 v)
{
    return {m.m[0][0] * v.x + m.m[1][0] * v.y + m.m[2][0] * v.z,
            m.m[0][1] * v.x + m.m[1][1] * v.y + m.m[2][1] * v.z,
            m.m[0][2] * v.x + m.m[1][2] * v.y + m.m[2][2] * v.z};
}

// Scene::RenderShadows 00451a50 reads its light and intensity from scene +0x270 -> +0x1c / +0x20:
// the light list of the scene's own gob (gob at scene +0xf8, list at gob +0x178). The object binder
// 004a3f70 fills them for each gob it binds (004a424d..004a4352): the shadow light is the first light
// in the light manager's sorted candidates (+0x44, count +0x48) with the shadow flag (+0x5c); the
// intensity fades in by 3 x frame time (007a1944 x 0078e574) up to 1; a different candidate first fades
// the old one out to 0 and only then takes its place; with no candidate the light fades out and the slot
// clears at 0. The candidates are 004a3cf0's list in 004a28c0's order (lower priority, type 1, 2, 0,
// then nearer over the radius in the XY plane, nothing beyond the radius), as sampleLights sorts them.
// GetShadowLights 004a4b10 is not this: it is only called with modes 0 and 1 (00453982, 00453c1b,
// 00456278), and its mode-2 rule (radius >= 1000 or type 0) has no caller.
// Inferred: the point the candidates are sorted from is the scene focus (the scene gob's place is not
// decoded); RETAIL row 47 asks for the live light. The sun's two lights never have the shadow flag in
// K1 (no ARE sets SunShadows or MoonShadows), so only scene.lights can win.
std::size_t shadowCandidate(const Scene& scene, Vec3 focus)
{
    std::size_t chosen = SIZE_MAX;
    int bestPriority = 0;
    int bestRank = 0;
    float bestReach = 0.0f;
    const auto typeRank = [](int type) { return type == 1 ? 0 : (type == 2 ? 1 : 2); };
    for (std::size_t i = 0; i < scene.lights.size(); ++i) {
        const Scene::WorldLight& light = scene.lights[i];
        if (!light.shadow || light.radius <= 0.0f
            || (light.colour.x <= 0.0f && light.colour.y <= 0.0f && light.colour.z <= 0.0f)) {
            continue;
        }
        const float dx = light.position.x - focus.x;
        const float dy = light.position.y - focus.y;
        const float reach = std::sqrt(dx * dx + dy * dy) / light.radius;
        if (reach > 1.0f) {
            continue;
        }
        const int rank = typeRank(light.dynamicType);
        if (chosen == SIZE_MAX || light.priority < bestPriority
            || (light.priority == bestPriority && (rank < bestRank || (rank == bestRank && reach < bestReach)))) {
            chosen = i;
            bestPriority = light.priority;
            bestRank = rank;
            bestReach = reach;
        }
    }
    return chosen;
}

// The scene gob's shadow slot, advanced once per light frame as 004a3f70 does (see shadowCandidate).
// Outside a game session (no light fades) the candidate holds at full strength.
std::size_t shadowLight(const Scene& scene, Vec3 focus, float& intensity)
{
    const std::size_t candidate = shadowCandidate(scene, focus);
    Scene::ShadowSlot& slot = scene.shadowSlot;
    if (!scene.lightFades) {
        slot.light = candidate;
        slot.intensity = candidate == SIZE_MAX ? 0.0f : 1.0f;
    } else if (slot.frame != scene.lightFrame) {
        slot.frame = scene.lightFrame;
        const float step = scene.lightFadeStep;
        if (slot.light >= scene.lights.size()) {
            slot.light = SIZE_MAX;
        }
        if (candidate == SIZE_MAX) {
            if (slot.light != SIZE_MAX) {
                slot.intensity = std::max(slot.intensity - step, 0.0f);
                if (slot.intensity <= 0.0f) {
                    slot.light = SIZE_MAX;
                }
            }
        } else if (slot.light == SIZE_MAX || slot.light == candidate) {
            slot.light = candidate;
            slot.intensity = std::min(slot.intensity + step, 1.0f);
        } else {
            slot.intensity = std::max(slot.intensity - step, 0.0f);
            if (slot.intensity <= 0.0f) {
                slot.light = candidate;
                slot.intensity = std::min(step, 1.0f);
            }
        }
    }
    intensity = slot.intensity;
    return slot.light;
}

} // namespace

ShadowFrame buildShadowFrame(const Scene& scene, const ShadowView& view)
{
    ShadowFrame frame;
    if (!view.enabled) {
        return frame;
    }
    float intensity = 0.0f;
    const std::size_t lightIndex = shadowLight(scene, view.focus, intensity);
    const Scene::WorldLight* light = lightIndex < scene.lights.size() ? &scene.lights[lightIndex] : nullptr;
    // 00451a50: the shadow light is used while scene +0xb0 + 1.75 (007415c4) is below its z (+0x9c).
    if (light == nullptr || !(view.focus.z + 1.75f < light->position.z)) {
        return frame;
    }
    // Light +0x60 when above 0, else its radius +0x58 (0044ff50, 00450550, 00450730).
    const float reach = light->shadowRadius > 0.0f ? light->shadowRadius : light->radius;
    // 0044ff50: the projection plane is the camera's far plane (camera +0x1f8 plane 1, written by
    // 0045c688 as (0, 0, -1, -far) in view space) moved in by 0.05 (007415bc) when the light's radius
    // is at most 1000 (0073d6fc), else (0, 0, -1, -0.1). Both clip what lies on their positive side.
    Vec3 planeNormal{0.0f, 0.0f, -1.0f};
    float planeDistance = -0.1f;
    if (light->radius <= 1000.0f) {
        planeNormal = normalize(view.forward);
        planeDistance = -view.farClip - dot(planeNormal, view.eye) + 0.05f;
    }
    // 00474710 gets the light at z + light +0x64.
    const Vec3 lightAt{light->position.x, light->position.y, light->position.z + light->verticalDisplacement};

    // Which entities are gobs that cast (00450550): gob +0xc2 (shadows on, 1 from the gob
    // constructor 0044982a) and +0x166 (TreeHasGeometry 004440d0), the model's word +0x50 with bit 2
    // (character) or bit 5 (placeable), within 35 (0078e3c8 under 0078e3cc) of the scene focus, and
    // nearer to the light (+0x94..+0x9c) than its reach.
    std::vector<std::uint8_t> casts(scene.entities.size(), 0u);
    for (const EntityMesh& mesh : scene.entityMeshes) {
        if (mesh.entity >= scene.entities.size() || mesh.fx || !mesh.model) {
            continue;
        }
        const Mdl& gobModel = mesh.attachModel ? *mesh.attachModel : *mesh.model;
        if (!mesh.attachModel && (gobModel.classification() & 0x24u) != 0u) {
            casts[mesh.entity] = 1u;
        }
    }
    std::vector<std::uint16_t> marks;
    std::vector<std::uint16_t> slots;
    std::vector<std::array<std::uint16_t, 2>> silhouette;
    std::vector<float> facing;
    std::vector<Vec3> points;
    std::vector<std::uint32_t> strip;
    for (const EntityMesh& mesh : scene.entityMeshes) {
        if (mesh.shadowParts.empty() || !mesh.visible || mesh.entity >= scene.entities.size()
            || casts[mesh.entity] == 0u) {
            continue;
        }
        const Entity& entity = scene.entities[mesh.entity];
        if (entity.dead && !entity.corpse()) {
            continue;
        }
        if (length(view.focus - entity.position) > 35.0f) {
            continue;
        }
        if (!(length(light->position - entity.position) < reach)) {
            continue;
        }
        const float bearing = entity.drawBearing();
        const float c = std::cos(bearing);
        const float s = std::sin(bearing);
        const Mat3 turn{{{c, -s, 0.0f}, {s, c, 0.0f}, {0.0f, 0.0f, 1.0f}}};
        for (const Mdl::ShadowPart& part : mesh.shadowParts) {
            const Mdl::ShadowMesh& caster = *part.mesh;
            // The part's place in the world: the entity's, then the posed node's.
            Mat3 rotation;
            for (int r = 0; r < 3; ++r) {
                for (int k = 0; k < 3; ++k) {
                    rotation.m[r][k] = turn.m[r][0] * part.pose.rotation.m[0][k]
                        + turn.m[r][1] * part.pose.rotation.m[1][k] + turn.m[r][2] * part.pose.rotation.m[2][k];
                }
            }
            const Vec3 origin = entity.position + turnBy(turn, part.pose.translation);
            const float scale = part.pose.scale;
            // 00474710: the light and the plane in the part's space (Plane::Transform 004ab4c0);
            // the part's scale goes on with glScalef and is not taken out of either.
            const Vec3 local = turnBackBy(rotation, lightAt - origin);
            if (local.x == 0.0f && local.y == 0.0f && local.z == 0.0f) {
                continue;
            }
            const Vec3 normal = turnBackBy(rotation, planeNormal);
            const float distance = planeDistance + dot(planeNormal, origin);
            // 00474220: the axis from the light towards the part's origin, its near point scaled by
            // 0082804c (zero: no code writes it) and its far point at the light's reach.
            const Vec3 axis = normalize(local * -1.0f);
            Vec3 nearCap{0.0f, 0.0f, 0.0f};
            Vec3 farCap = local + axis * reach;
            if (!caster.beaming) {
                // ClipProjectionCaps 0046f580.
                if (planeSide(normal, distance, farCap) > 0.0f) {
                    if (distance > 0.0f) {
                        continue;
                    }
                    farCap = planeLineIntersection(normal, distance, nearCap, farCap);
                }
                if (planeSide(normal, distance, nearCap) > 0.0f) {
                    nearCap = planeLineIntersection(normal, distance, nearCap, farCap);
                }
            }
            // PartTriMesh::Project 004741a0: each face's side of the light.
            const std::size_t faceCount = caster.faces.size();
            facing.resize(faceCount);
            for (std::size_t f = 0; f < faceCount; ++f) {
                facing[f] = dot(caster.faces[f].normal, local) + caster.faces[f].distance;
            }
            // Silhouette edges: from every face not turned away, each edge without a neighbour or
            // whose neighbour's side times this face's is not above 0.
            const std::size_t vertexCount = caster.vertices.size();
            marks.assign(vertexCount, 0u);
            silhouette.clear();
            bool valid = true;
            for (std::size_t f = 0; f < faceCount && valid; ++f) {
                if (facing[f] < 0.0f) {
                    continue;
                }
                const Mdl::ShadowMesh::Face& face = caster.faces[f];
                for (std::size_t e = 0; e < 3u; ++e) {
                    const std::int16_t neighbour = face.adjacent[e];
                    if (neighbour != -1) {
                        if (neighbour < 0 || static_cast<std::size_t>(neighbour) >= faceCount) {
                            continue;
                        }
                        if (facing[static_cast<std::size_t>(neighbour)] * facing[f] > 0.0f) {
                            continue;
                        }
                    }
                    const std::uint16_t a = face.vertex[e];
                    const std::uint16_t b = face.vertex[(e + 1u) % 3u];
                    if (a >= vertexCount || b >= vertexCount) {
                        valid = false;
                        break;
                    }
                    silhouette.push_back({a, b});
                    marks[a] = 1u;
                    marks[b] = 1u;
                }
            }
            if (!valid || silhouette.empty()) {
                continue;
            }
            // GLRender::BuildShadowArray 004269f0: each used vertex, in vertex order, gives its near
            // point (clipped by ClipProjection 0046f650) and its far point on the line from the
            // light, at the reach; a far point behind its near one (0078d994) is the near one.
            slots.assign(vertexCount, 0u);
            points.clear();
            std::uint16_t used = 0;
            for (std::size_t v = 0; v < vertexCount; ++v) {
                if (marks[v] == 0u) {
                    continue;
                }
                slots[v] = ++used;
                Vec3 nearPoint = caster.vertices[v];
                const Vec3 direction = normalize(nearPoint - local);
                Vec3 farPoint = local + direction * reach;
                if (planeSide(normal, distance, nearPoint) > 0.0f) {
                    nearPoint = planeLineIntersection(normal, distance, local, nearPoint);
                }
                if (planeSide(normal, distance, farPoint) > 0.0f) {
                    farPoint = planeLineIntersection(normal, distance, local, farPoint);
                }
                if (dot(nearPoint - farPoint, direction) > 0.0f) {
                    farPoint = nearPoint;
                }
                points.push_back(nearPoint);
                points.push_back(farPoint);
            }
            points.push_back(nearCap);
            points.push_back(farCap);
            // RenderShadowArray 00426660: six indices an edge, alternating, drawn as one strip.
            const std::uint32_t capNear = static_cast<std::uint32_t>(used) * 2u;
            const std::uint32_t capFar = capNear + 1u;
            strip.clear();
            for (std::size_t e = 0; e < silhouette.size(); ++e) {
                const std::uint32_t a = static_cast<std::uint32_t>(slots[silhouette[e][0]]) * 2u;
                const std::uint32_t b = static_cast<std::uint32_t>(slots[silhouette[e][1]]) * 2u;
                if (e % 2u == 0u) {
                    strip.insert(strip.end(), {capNear, a - 2u, b - 2u, a - 1u, b - 1u, capFar});
                } else {
                    strip.insert(strip.end(), {capFar, b - 1u, a - 1u, b - 2u, a - 2u, capNear});
                }
            }
            ShadowFrame::Run run;
            run.first = static_cast<std::uint32_t>(frame.volume.size());
            const auto place = [&](std::uint32_t index) {
                return origin + turnBy(rotation, points[index] * scale);
            };
            for (std::size_t j = 0; j + 2u < strip.size(); ++j) {
                std::uint32_t a = strip[j];
                std::uint32_t b = strip[j + 1u];
                const std::uint32_t cIndex = strip[j + 2u];
                if (j % 2u == 1u) {
                    std::swap(a, b);
                }
                if (a == b || b == cIndex || a == cIndex) {
                    continue;
                }
                frame.volume.push_back(place(a));
                frame.volume.push_back(place(b));
                frame.volume.push_back(place(cIndex));
            }
            run.count = static_cast<std::uint32_t>(frame.volume.size()) - run.first;
            if (run.count != 0u) {
                frame.runs.push_back(run);
                ++frame.parts;
                frame.edges += silhouette.size();
            }
        }
    }
    if (frame.runs.empty()) {
        return frame;
    }
    // 00450730: the fan.
    Vec3 centre = view.focus;
    float radius = 50.0f; // 00740cc0
    if (!(reach > 50.0f)) {
        const float height = std::fabs(light->position.z - view.focus.z);
        const float squared = reach * reach - height * height;
        radius = std::sqrt(squared < 0.0f ? 0.0f : squared);
        centre = {light->position.x, light->position.y, view.focus.z};
    }
    frame.disk[0] = centre;
    for (int i = 0; i < 10; ++i) {
        const float angle = static_cast<float>(i) * 0.6981316804885864f; // 007415c0, 40 degrees
        frame.disk[static_cast<std::size_t>(i) + 1u] = {std::cos(angle) * radius + centre.x,
                                                        std::sin(angle) * radius + centre.y, view.focus.z};
    }
    // The centre's alpha byte: _ftol(intensity * 0.5 * 255).
    const int alphaByte = static_cast<int>(static_cast<double>(intensity) * 0.5 * 255.0);
    frame.diskAlpha = static_cast<float>(alphaByte) / 255.0f;
    return frame;
}

} // namespace ke::kotor
