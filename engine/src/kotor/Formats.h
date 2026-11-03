#pragma once

// KotOR 1.0.3.0 data formats, implemented inside the engine so the playable
// runtime never depends on a Python tool. The layouts are the ones verified by
// the read-only research tools in tools/resources and tools/formats; those
// tools remain oracles only.

#include "kotor/Profile.h"

#include <cstdint>
#include <cmath>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace ke::kotor {

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Mat3 {
    float m[3][3]{};
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a)
{
    const float n = length(a);
    return n > 1e-8f ? a * (1.0f / n) : Vec3{0.0f, 0.0f, 0.0f};
}

// 8-bit RGBA, top-left origin. The GPU uploader converts to the backend's
// preferred channel order; the decoder always produces RGBA.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct Texture {
    std::string name;
    Image image;
};

// Resource type ids used by the engine (tools/inventory/kotor_types.py).
enum class ResType : std::uint32_t {
    Tga = 3,
    Mdl = 2002,
    Tpc = 3007,
    Wok = 2016,
    Are = 2012,
    Ifo = 2014,
    Git = 2023,
    Lyt = 3000,
    Vis = 3001,
    Pth = 3003,
    Mdx = 3008,
    TwoDa = 2017,
};

inline std::uint32_t resType(ResType type) { return static_cast<std::uint32_t>(type); }

// ---- byte access ---------------------------------------------------------------------------------------------------------

std::uint16_t readU16(const std::uint8_t* data, std::size_t size, std::size_t offset, const char* what);
std::uint32_t readU32(const std::uint8_t* data, std::size_t size, std::size_t offset, const char* what);
std::int32_t readI32(const std::uint8_t* data, std::size_t size, std::size_t offset, const char* what);
float readF32(const std::uint8_t* data, std::size_t size, std::size_t offset, const char* what);
std::string readCString(const std::uint8_t* data, std::size_t size, std::size_t offset, std::size_t maxLength, const char* what);
std::string normalizeResref(const std::uint8_t* data, std::size_t size, std::size_t offset, std::size_t maxLength, const char* what);
std::string normalizeResref(std::string text);
std::vector<std::uint8_t> readBytes(const std::uint8_t* data, std::size_t size, std::size_t offset, std::size_t length, const char* what);
void requireRange(std::size_t size, std::size_t offset, std::size_t length, const char* what);

// ---- hashes --------------------------------------------------------------------------------------------------------------

std::string sha256File(const std::filesystem::path& path);
std::string sha256Bytes(const std::uint8_t* data, std::size_t size);

// ---- archives ------------------------------------------------------------------------------------------------------------

struct ArchiveEntry {
    std::string resref;
    std::uint32_t type = 0;
    std::uint32_t id = 0;
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t index = 0;
};

// ERF/MOD/SAV and RIM reader. The archive file is opened on demand; texture
// packs are hundreds of MB and are not copied into memory.
class Archive {
public:
    Archive() = default;
    static Archive open(const std::filesystem::path& path);

    const std::filesystem::path& path() const { return path_; }
    const std::string& format() const { return format_; }
    const std::vector<ArchiveEntry>& entries() const { return entries_; }
    const ArchiveEntry* find(const std::string& resref, std::uint32_t type) const;
    std::vector<std::uint8_t> read(const ArchiveEntry& entry) const;

private:
    std::filesystem::path path_;
    std::string format_;
    std::uint64_t fileSize_ = 0;
    std::vector<ArchiveEntry> entries_;
};

struct BifRecord {
    std::uint32_t id = 0;
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t type = 0;
    std::uint32_t index = 0;
};

class BifFile {
public:
    BifFile() = default;
    static BifFile open(const std::filesystem::path& path);

    const std::filesystem::path& path() const { return path_; }
    const BifRecord* find(std::uint32_t id) const;
    std::vector<std::uint8_t> read(const BifRecord& record) const;

private:
    std::filesystem::path path_;
    std::uint64_t fileSize_ = 0;
    std::vector<BifRecord> records_;
    std::unordered_map<std::uint32_t, std::size_t> byId_;
};

struct KeyBif {
    std::uint32_t index = 0;
    std::string name;
    std::uint32_t declaredSize = 0;
};

struct KeyEntry {
    std::string resref;
    std::uint32_t type = 0;
    std::uint32_t resid = 0;
    std::uint32_t bifIndex = 0;
    std::uint32_t tableIndex = 0;
};

class KeyFile {
public:
    KeyFile() = default;
    static KeyFile open(const std::filesystem::path& path);

    const std::vector<KeyBif>& bifs() const { return bifs_; }
    const std::vector<KeyEntry>& entries() const { return entries_; }
    const KeyEntry* find(const std::string& resref, std::uint32_t type) const;

private:
    std::vector<KeyBif> bifs_;
    std::vector<KeyEntry> entries_;
    std::unordered_map<std::string, std::vector<std::size_t>> byKey_;
};

// ---- GFF -----------------------------------------------------------------------------------------------------------------

enum class GffType : std::uint32_t {
    UInt8 = 0, Int8 = 1, UInt16 = 2, Int16 = 3, UInt32 = 4, Int32 = 5,
    UInt64 = 6, Int64 = 7, Float = 8, Double = 9, String = 10, ResRef = 11,
    LocString = 12, Binary = 13, Struct = 14, List = 15, Vector4 = 16,
    Vector3 = 17, StrRef = 18,
};

struct GffField {
    GffType type = GffType::UInt32;
    std::uint32_t labelIndex = 0;
    std::uint32_t data = 0;
    std::string label;
};

struct GffStruct {
    std::uint32_t typeId = 0;
    std::vector<std::uint32_t> fieldRows;
};

class Gff {
public:
    static Gff parse(std::vector<std::uint8_t> bytes, std::string name);

    const std::string& name() const { return name_; }
    std::uint32_t rootIndex() const { return rootIndex_; }
    std::size_t structureCount() const { return structures_.size(); }
    const GffStruct& structure(std::uint32_t index) const;
    const GffField& fieldAt(std::size_t row) const { return fields_.at(row); }
    const std::vector<std::uint8_t>& fieldData() const { return fieldData_; }
    const GffField* field(const GffStruct& structure, const std::string& label) const;
    const GffField* field(std::uint32_t structureIndex, const std::string& label) const;

    std::string text(const GffField& field) const;
    std::uint32_t locStringStrref(const GffField& field) const;
    std::vector<std::pair<std::uint32_t, std::string>> locStringParts(const GffField& field) const;
    float number(const GffField& field) const;
    std::int32_t integer(const GffField& field) const;
    Vec3 vector3(const GffField& field) const;
    std::vector<std::uint32_t> list(const GffField& field) const;
    std::uint32_t structIndex(const GffField& field) const;
    std::string locString(const GffField& field) const;

private:
    std::string name_;
    std::vector<std::uint8_t> bytes_;
    std::uint32_t rootIndex_ = 0;
    std::vector<GffField> fields_;
    std::vector<GffStruct> structures_;
    std::vector<std::uint8_t> fieldData_;
    std::vector<std::uint32_t> fieldIndices_;
    std::vector<std::uint32_t> listIndices_;
    std::vector<std::string> labels_;
};

// ---- area geometry -------------------------------------------------------------------------------------------------------

struct WokFace {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
    std::uint32_t surface = 0;
    Vec3 normal;
    bool floor = false;
};

struct Wok {
    std::vector<Vec3> vertices;
    std::vector<WokFace> faces;
};

struct LytRoom {
    std::string room;
    Vec3 origin;
};

struct LytHook {
    std::string room;
    std::string node;
    Vec3 position;
};

struct Lyt {
    std::vector<LytRoom> rooms;
    std::vector<LytHook> hooks;
};

Lyt parseLyt(std::vector<std::uint8_t> bytes, const std::string& name);
Wok parseWok(std::vector<std::uint8_t> bytes, const std::string& name);

struct MdlTriangle {
    Vec3 p[3];
    Vec2 uv[3];
    Vec3 normal;
    std::string texture;
};

struct MdlMeshVertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    std::int32_t node = -1;
    std::array<float, 4> weights{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<std::int32_t, 4> bones{-1, -1, -1, -1};
};

struct MdlMeshTriangle {
    MdlMeshVertex v[3];
    std::string texture;
};

struct MdlAnimationInfo {
    std::string name;
    float duration = 0.0f;
    float transition = 0.0f;
};

// Minimal MDL/MDX parser: node hierarchy, trimesh/skin nodes, world-space
// triangles and animation clips. Skinning is evaluated on the CPU.
class Mdl {
public:
    static Mdl parse(std::vector<std::uint8_t> mdl, std::vector<std::uint8_t> mdx, std::string name);

    std::vector<MdlTriangle> triangles(Vec3 roomOrigin) const;
    std::vector<MdlMeshTriangle> meshTriangles() const;
    std::vector<MdlMeshTriangle> posedTriangles(const std::string& animation, float time) const;

    const std::string& name() const { return name_; }
    const std::vector<MdlAnimationInfo>& animations() const { return animations_; }
    int animationIndex(const std::string& name) const;
    float animationDuration(const std::string& name) const;
    std::size_t skinnedNodeCount() const;
    float skinBindError() const;

private:
    struct ControllerKey {
        float time = 0.0f;
        std::vector<float> data;
    };

    struct Controller {
        std::int32_t type = 0;
        bool bezier = false;
        std::vector<ControllerKey> keys;
    };

    struct Skin {
        std::vector<std::int32_t> boneMap;
        std::vector<std::array<float, 4>> qbones;
        std::vector<Vec3> tbones;
        std::uint32_t weightsOffset = 0;
        std::uint32_t bonesOffset = 0;
    };

    struct Node {
        std::uint32_t offset = 0;
        std::uint16_t type = 0;
        std::uint16_t supernode = 0;
        std::uint16_t nodeId = 0;
        std::uint32_t parent = 0;
        Vec3 position;
        float qw = 1.0f;
        float qx = 0.0f;
        float qy = 0.0f;
        float qz = 0.0f;
        std::vector<std::uint32_t> children;
        std::string name;
        std::uint32_t faceOffset = 0;
        std::uint32_t faceCount = 0;
        std::uint32_t stride = 0;
        std::uint32_t vertexCount = 0;
        std::uint32_t mdxOffset = 0;
        std::string texture0;
        std::string texture1;
        bool trimesh = false;
        std::optional<Skin> skin;
        std::vector<Controller> controllers;
    };

    struct Animation {
        std::string name;
        float duration = 0.0f;
        float transition = 0.0f;
        std::vector<Node> nodes;
    };

    struct Pose {
        Mat3 rotation;
        Vec3 translation;
    };

    std::vector<Node> parseNodes(std::uint32_t rootOffset) const;
    std::vector<Controller> parseControllers(std::uint32_t nodeHeader) const;
    Animation parseAnimation(std::uint32_t offset) const;
    std::map<std::uint16_t, const Node*> animationNodes(const Animation& animation) const;
    std::vector<Pose> poseWorld(const Animation* animation, float time) const;

    std::string name_;
    std::vector<std::uint8_t> mdl_;
    std::vector<std::uint8_t> mdx_;
    std::vector<Node> nodes_;
    std::vector<Animation> animationData_;
    std::vector<MdlAnimationInfo> animations_;
    std::vector<Pose> bindWorld_;
    std::unordered_map<std::uint32_t, std::size_t> nodeByOffset_;
    std::unordered_map<std::uint16_t, std::size_t> nodeById_;
};

// ---- 2DA -----------------------------------------------------------------------------------------------------------------

// KotOR 2DA table (binary V2.b and ASCII V2.0). Column and row lookups are
// case-insensitive because the shipped tables mix cases between releases.
class TwoDa {
public:
    static TwoDa parse(std::vector<std::uint8_t> bytes, std::string name);

    std::size_t rowCount() const { return rows_.size(); }
    std::size_t columnCount() const { return columns_.size(); }
    const std::string& column(std::size_t index) const { return columns_.at(index); }
    const std::string& rowLabel(std::size_t index) const { return rows_.at(index).label; }

    int columnIndex(const std::string& name) const;
    int rowIndex(const std::string& label) const;
    std::string cell(std::size_t row, std::size_t column) const;
    std::string cell(const std::string& row, const std::string& column) const;

private:
    struct Row {
        std::string label;
        std::vector<std::string> cells;
    };
    std::string name_;
    std::vector<std::string> columns_;
    std::vector<Row> rows_;
};

// ---- TLK -----------------------------------------------------------------------------------------------------------------

class Tlk {
public:
    static Tlk parse(std::vector<std::uint8_t> bytes, std::string name);

    std::size_t size() const { return entries_.size(); }
    std::string text(std::uint32_t strref) const;

private:
    struct Entry {
        std::uint32_t offset = 0;
        std::uint32_t length = 0;
    };
    std::string name_;
    std::vector<Entry> entries_;
    std::vector<std::uint8_t> text_;
};

// ---- dialogue ------------------------------------------------------------------------------------------------------------

struct DlgEntry {
    std::string speaker;
    std::string text;
    std::string script;
    std::string active;
    std::string quest;
    std::int32_t action[4]{};
    std::string comment;
    std::string sound;
    std::vector<std::uint32_t> replies;
};

struct DlgReply {
    std::string text;
    std::string script;
    std::string active;
    std::string quest;
    std::int32_t action[4]{};
    std::string comment;
    std::string sound;
    std::vector<std::uint32_t> entries;
};

struct Dlg {
    std::vector<DlgEntry> entries;
    std::vector<DlgReply> replies;
    std::vector<std::uint32_t> starting;
    std::vector<std::string> startingActive;
    std::string endConversation;
    std::string endConversationAbort;
    bool skippable = true;

    static Dlg parse(const Gff& gff, const Tlk* tlk, std::string name);
};

// ---- entity templates ---------------------------------------------------------------------------------------------------

struct CreatureTemplate {
    std::string resref;
    std::string tag;
    std::string name;
    std::string conversation;
    std::string model;
    std::string texture;
    std::string scriptDialogue;
    std::string scriptDeath;
    std::uint32_t appearance = 0;
    std::uint32_t faction = 0;
    std::int32_t hitPoints = 1;
    std::int32_t maxHitPoints = 1;
    std::int32_t attackBonus = 0;
    std::int32_t defense = 10;
    std::int32_t damageMin = 1;
    std::int32_t damageMax = 4;
    float walkRate = 1.0f;
};

struct DoorTemplate {
    std::string resref;
    std::string tag;
    std::string model;
    std::uint32_t genericType = 0;
    std::uint32_t faction = 0;
};

struct PlaceableTemplate {
    std::string resref;
    std::string tag;
    std::string conversation;
    std::string model;
    std::uint32_t appearance = 0;
    bool useable = false;
};

struct TriggerTemplate {
    std::string resref;
    std::string tag;
    std::string scriptOnEnter;
    std::string scriptOnExit;
};

struct ItemTemplate {
    std::string resref;
    std::string tag;
    std::string name;
    std::int32_t baseItem = 0;
    std::int32_t cost = 0;
    std::int32_t stackSize = 1;
    bool identified = true;
};

struct StoreTemplate {
    std::string resref;
    std::string tag;
    std::string name;
    std::int32_t markUp = 100;
    std::int32_t markDown = 40;
    std::int32_t buySellFlag = 3;
    std::vector<std::pair<std::string, bool>> items; // resref, infinite
};

struct EncounterCreature {
    std::string resref;
    std::int32_t appearance = 0;
    float challengeRating = 0.0f;
    bool singleSpawn = false;
};

struct EncounterTemplate {
    std::string resref;
    std::string tag;
    std::int32_t maxCreatures = 0;
    std::int32_t recCreatures = 0;
    std::int32_t resetTime = 0;
    std::int32_t respawns = 0;
    std::int32_t spawnOption = 0;
    std::int32_t difficulty = 0;
    bool active = true;
    std::vector<EncounterCreature> creatures;
};

struct EncounterSpawnPoint {
    Vec3 position;
    float bearing = 0.0f;
};

struct EncounterVolume {
    std::uint32_t entity = 0;
    std::string templateResref;
    EncounterTemplate templateData;
    std::vector<Vec3> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<EncounterSpawnPoint> spawnPoints;
};

// ---- textures ------------------------------------------------------------------------------------------------------------

Image decodeTpc(const std::vector<std::uint8_t>& data, const std::string& name);
Image decodeTga(const std::vector<std::uint8_t>& data, const std::string& name);
Image decodeTexture(const std::vector<std::uint8_t>& data, std::uint32_t type, const std::string& name);

// ---- module scene --------------------------------------------------------------------------------------------------------

struct CollisionFacet {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
    std::uint32_t surface = 0;
    Vec3 normal;
    bool floor = false;
};

// One active Force/spell effect. The stack is shared by the player sheet and
// scene entities so Breach/Suppress/dispel can remove effects uniformly.
struct ActiveEffect {
    std::int32_t spellId = -1;
    std::string label;
    float remaining = 0.0f;
    std::int32_t armorBonus = 0;
    std::int32_t attackBonus = 0;
    std::int32_t damageBonus = 0;
    std::int32_t shield = 0;
    bool force = false;
    bool dispellable = true;
};

enum class EntityKind { Door, Placeable, Creature, Waypoint, Trigger, Encounter, Sound, Store, Unknown };

struct Entity {
    EntityKind kind = EntityKind::Unknown;
    std::uint32_t sourceStructure = 0; // GIT struct for trigger geometry and diagnostics
    std::string templateResref;
    std::string tag;
    std::string name;
    std::string conversation;
    Vec3 position;
    float bearing = 0.0f;
    std::string linkedModule;
    std::string linkedTo;
    std::uint32_t linkedFlags = 0;
    std::string transitionDestination;
    std::string model;
    std::string texture;
    std::string scriptDialogue;
    std::string scriptOnEnter;
    std::string scriptOnExit;
    std::uint32_t appearance = 0;
    std::uint32_t faction = 0;
    std::int32_t hitPoints = 1;
    std::int32_t maxHitPoints = 1;
    std::int32_t attackBonus = 0;
    std::int32_t defense = 10;
    std::int32_t fortSave = 0;
    std::int32_t willSave = 0;
    std::int32_t refSave = 0;
    std::int32_t damageMin = 1;
    std::int32_t damageMax = 4;
    float walkRate = 1.0f;
    bool ranged = false;
    float attackRange = 2.4f;
    float stunTimer = 0.0f;
    float fearTimer = 0.0f;
    float forceSuppressedTimer = 0.0f;
    std::vector<ActiveEffect> effects;
    bool hostile = false;
    bool dead = false;
    bool useable = false;
    bool open = false;
    bool locked = false;
    // Merchant template data for Store entities.
    std::int32_t storeMarkUp = 100;
    std::int32_t storeMarkDown = 40;
    std::int32_t storeBuySell = 3;
    std::vector<std::pair<std::string, bool>> storeItems;
    std::vector<std::string> lootItems;
    // Hook name (ScriptSpawn, OnOpen, ...) to script resref. Template values
    // are loaded first; non-empty GIT instance values override them.
    std::map<std::string, std::string> scripts;
};

struct TextureBatch {
    std::uint32_t texture = 0; // index into Scene::textures
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
};

struct SceneVertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
};

// Geometry for one GIT instance. Vertices are in model space; the entity's
// position/bearing is applied by the renderer. Animated meshes keep their MDL
// and bind triangles so a pose can refresh the vertex buffer without rebuilding
// topology.
struct EntityMesh {
    std::uint32_t entity = 0;
    std::shared_ptr<Mdl> model;
    std::vector<MdlMeshTriangle> triangles;
    std::vector<SceneVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<TextureBatch> batches;
    std::string animation;
    float animationTime = 0.0f;
    bool animationLoop = true;
    std::uint32_t revision = 0;
};

// One walkable trigger polygon in world space (GIT TriggerList geometry).
struct TriggerVolume {
    std::uint32_t entity = 0;
    std::vector<Vec3> vertices;
    std::vector<std::uint32_t> indices;
};

// Read-only view of one KotOR install. Build identity is checked before any
// resource is used, and the install is never opened for writing.
class Install {
public:
    explicit Install(std::filesystem::path gameDir, bool verifyBuild = true);

    const std::filesystem::path& root() const { return root_; }
    const std::string& buildSha256() const { return buildSha256_; }
    const GameProfile& profile() const { return profile_; }
    const KeyFile& key() const { return key_; }

    void bindModule(const std::string& stem);
    void addPacks();
    std::optional<std::vector<std::uint8_t>> find(const std::string& resref, std::uint32_t type,
                                                  std::string* source = nullptr);
    std::optional<std::vector<std::uint8_t>> findTexture(const std::string& resref, std::uint32_t* type,
                                                         std::string* source = nullptr);

private:
    const BifFile& bif(std::uint32_t index);
    std::filesystem::path bifPath(const std::string& keyName) const;

    std::filesystem::path root_;
    GameProfile profile_;
    std::string buildSha256_;
    KeyFile key_;
    std::unordered_map<std::uint32_t, BifFile> bifs_;
    std::vector<Archive> archives_;
    std::vector<std::string> archiveNames_;
};

struct Scene {
    std::string module;
    std::string area;
    std::string areaTag;
    std::string installPath;
    Vec3 entry;
    Vec3 entryDirection;
    std::vector<SceneVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<TextureBatch> batches;
    std::vector<Texture> textures;
    std::vector<Vec3> collisionVertices;
    std::vector<CollisionFacet> collision;
    std::vector<Entity> entities;
    std::vector<EntityMesh> entityMeshes;
    std::vector<TriggerVolume> triggers;
    std::vector<EncounterVolume> encounters;
    std::vector<LytRoom> rooms;
    std::vector<std::string> warnings;
    std::map<std::string, std::string> scripts;
    Vec3 ambient{0.35f, 0.35f, 0.38f};
    Vec3 diffuse{0.85f, 0.82f, 0.75f};
    Vec3 fogColor{0.06f, 0.07f, 0.10f};
    float fogNear = 40.0f;
    float fogFar = 160.0f;
    std::uint32_t sourceBytes = 0;
};

Scene loadModule(Install& install, const std::string& moduleStem);
void refreshEntityMesh(EntityMesh& mesh);

// Collect non-empty script hook fields from one GFF struct into a hook map.
// Used for ARE/IFO module hooks and GIT instance overrides.
void collectScriptHooks(const Gff& gff, std::uint32_t structure,
                        std::map<std::string, std::string>& scripts);

// Load one UTI item template (type 2025) from the install.
std::optional<ItemTemplate> loadItemTemplate(Install& install, const std::string& resref);

// Append one creature from a UTC template at a world position. Builds its mesh
// and hook map; returns the new entity index or -1 on failure. Used by the
// encounter spawner.
std::size_t spawnCreature(Scene& scene, Install& install, const std::string& utcResref,
                          Vec3 position, float bearing);

// Resolves GIT templates, model geometry and trigger polygons after the base
// module has been loaded. Kept separate from loadModule so the room pass stays
// independent of template/model failures.
void loadEntityAssets(Scene& scene, Install& install, const Gff& git);

} // namespace ke::kotor
