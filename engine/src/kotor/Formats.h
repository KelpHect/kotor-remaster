#pragma once

// KotOR 1.0.3.0 data formats, implemented inside the engine so the playable
// runtime never depends on a Python tool. The layouts are the ones verified by
// the read-only research tools in tools/resources and tools/formats; those
// tools remain oracles only.

#include "kotor/Profile.h"

#include <atomic>
#include <cstdint>
#include <cmath>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
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

// The TXI directives that change how a texture draws (retail parsers 0047abd0, 00422390).
struct TxiInfo {
    enum class Blend { Default, Additive, PunchThrough };
    enum class Procedure { None, Cycle, Arturo, Water, Other };
    Blend blending = Blend::Default;
    Procedure procedure = Procedure::None;
    bool decal = false;
    bool cube = false;
    std::string envMap;      // envmaptexture
    std::string bumpyShiny;  // bumpyshinytexture (its environment map)
    std::string bumpMap;     // bumpmaptexture
    bool isBumpMap = false;  // isbumpmap: this texture is itself a bump map
    float bumpScale = 1.0f;  // bumpmapscaling, for a height map
    // Retail ctor 00423150: isspecularbumpmap 1 and specularcolor 1,1,1 unless the TXI says otherwise.
    bool isSpecularBump = true;
    float specular = 1.0f;   // specularcolor; every shipped value is grey
    int numX = 1;
    int numY = 1;
    float fps = 0.0f;
    float waterAlpha = -1.0f;
    // Procedure parameters (retail base parser 00462b00, ctor defaults 00462a70).
    bool distort = false;           // distort
    bool distortAngle = true;       // distortangle
    float distortAmplitude = 5.0f;  // distortionamplitude
    float speed = 1.0f;             // speed
    // arturowidth/arturoheight (00463400, default 15) or waterwidth/waterheight (004631a0, 31).
    int gridWidth = 0;
    int gridHeight = 0;
};

TxiInfo parseTxi(const std::string& text);

struct Texture {
    std::string name;
    Image image;
    TxiInfo txi;
    bool hasAlpha = false; // some texel is not fully opaque
    // Retail wrap state (glTexParameteri at 00424980/00424dd0): both axes repeat unless the
    // texture's TXI `clamp` value says otherwise; bit 0 clamps S (U), bit 1 clamps T (V).
    // A texture with no TXI repeats. Set by textureClampFlags() from the shipped TPC/TXI.
    std::uint8_t clamp = 0;
    // Counts rewrites of `image` after the first upload, by a procedure: a backend whose copy
    // has another count uploads the image again.
    std::uint32_t revision = 0;
};

// The running state of an animated (arturo or water) texture.
struct ProceduralTexture {
    std::string name;
    std::size_t texture = 0;        // index into Scene::textures
    int gridWidth = 0;
    int gridHeight = 0;
    std::vector<float> next;        // retail +0x40
    std::vector<float> current;     // retail +0x44
    std::vector<float> previous;    // retail +0x48 (arturo), velocity (water)
    std::vector<std::uint8_t> heights;  // retail +0x50 (arturo), +0x5c (water)
    std::vector<std::uint8_t> original; // retail +0x34: the texels the procedure displaces
    float blend = 0.0f;             // retail +0x4c (arturo)
    float angleA = 0.0f;            // retail +0x4c (water)
    float angleB = 0.0f;            // retail +0x50 (water)
    float accumulator = 0.0f;
    int phase = 0;
    std::uint32_t random = 0;
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
    Uts = 2035,
    Bik = 2063,
    Lip = 3004,
    Gui = 2047,
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
    std::array<float, 4> vector4(const GffField& field) const;
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
    std::uint8_t seamEdges = 0; // Bit i: edge i is a perimeter edge with a room transition.
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

// Blend state for a draw batch. FX lights are additive, emitters are
// alpha-blended, and all static geometry stays opaque.
enum class BlendMode { Opaque, Alpha, Additive };

struct MdlTriangle {
    Vec3 p[3];
    Vec2 uv[3];
    Vec3 normal;
    std::string texture;
    BlendMode blend = BlendMode::Opaque;
};

struct MdlMeshVertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    Vec2 uv1; // second UV set: the lightmap's
    std::int32_t node = -1;
    std::array<float, 4> weights{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<std::int32_t, 4> bones{-1, -1, -1, -1};
    float dangly = 0.0f;
};

// Fixed per-node render data from the trimesh header (see Mdl::parse).
struct MdlNodeMaterial {
    std::string lightmap;        // bitmap2, when the header's lightmap flag is set
    float transparencyHint = 0.0f;
    Vec3 diffuse{1.0f, 1.0f, 1.0f}; // material colours of the mesh header
    Vec3 ambient{1.0f, 1.0f, 1.0f};
    bool fades = false;          // an alpha controller exists on this node, in any clip
    bool noFog = false;          // the model's header says fog does not touch it (skies)
    bool saber = false;          // a lightsaber blade node: retail 00471860 draws it with culling off
    bool animateUv = false;
    bool hasTangents = false;    // the MDX carries a tangent frame (retail 0043e410, header +288)
    float uvDirX = 0.0f;
    float uvDirY = 0.0f;
    float uvJitter = 0.0f;
    float uvJitterSpeed = 0.0f;
    bool beaming = false;
    bool shadow = false;
};

struct MdlMeshTriangle {
    MdlMeshVertex v[3];
    std::string texture;
    BlendMode blend = BlendMode::Opaque;
    const MdlNodeMaterial* material = nullptr; // owned by the Mdl, null for generated quads
    std::uint32_t surface = UINT32_MAX; // MDL face +0x10 (surfacemat.2da row); none for generated faces
    Vec3 selfIllum;      // controller 100 at the sampled time
    float alpha = 1.0f;  // controller 132 at the sampled time
};

// One MDL light node, in model space, at its header/controller values.
struct MdlLight {
    Vec3 position;
    Vec3 colour{1.0f, 1.0f, 1.0f};
    float radius = 1.0f;
    float multiplier = 1.0f;
    int priority = 0;
    bool ambientOnly = false;
    bool affectDynamic = false;
    int dynamicType = 0;
    // Light header +80 (node +0xa0): retail copies it to the light's +0x5c (00447bd5), the flag
    // LightManager::GetShadowLights 004a4b10 picks shadow lights by. Controllers 96 and 100 are the
    // light's +0x60 (shadow reach when above 0) and +0x64 (raised onto the light's z by 0044ff50).
    bool shadow = false;
    float shadowRadius = 0.0f;
    float verticalDisplacement = 0.0f;
    float flareRadius = 0.0f;
    // Light header +88 (node +0xa8, fadinglight): the object binder 004a3f70 fades such a light in and
    // out by 3 x frame time; any other light is bound at full strength from the next frame.
    bool fading = false;
    std::size_t node = SIZE_MAX; // The light's node index (Mdl::lights), to pose it by a clip.
    struct Flare {
        std::string texture;
        float size = 0.0f;
        float position = 0.0f;
        Vec3 colourShift;
    };
    std::vector<Flare> flares;
};

// One MDL emitter node's fixed header (retail node type 0x04).
struct MdlEmitter {
    std::string update;   // Fountain, Single, Explosion, Lightning
    std::string render;   // Normal, Linked, Billboard_to_Local_Z, ...
    std::string blend;    // Normal, Punch-Through, Lighten
    std::string texture;
    std::string chunk;
    float deadSpace = 0.0f;
    float blastRadius = 0.0f;
    float blastLength = 0.0f;
    std::uint32_t branchCount = 0;
    float smoothing = 0.0f;
    std::int32_t gridX = 1;
    std::int32_t gridY = 1;
    std::int32_t spawnType = 0;
    bool twoSided = false;
    bool loop = false;
    std::uint16_t renderOrder = 0;
    bool frameBlend = false;
    std::uint32_t flags = 0;
    std::string targetNode; // A Lightning emitter's reference child: where its bolt ends.
};

// One entry of an animation's event table (name fired at `time` into the clip).
struct MdlAnimEvent {
    float time = 0.0f;
    std::string name;
};

// A clip being sampled at a time. Later layers replace the channels they animate.
struct MdlAnimLayer {
    std::string animation;
    float time = 0.0f;
};

struct MdlAnimationInfo {
    std::string name;
    float duration = 0.0f;
    float transition = 0.0f;
};

// One sampled MDL controller. `values` keeps the controller's native width so
// scalar, colour and vector families share a single deterministic model.
struct FxParameter {
    std::int32_t type = -1;
    std::vector<float> values;

    float scalar(std::size_t component = 0, float fallback = 0.0f) const;
    Vec3 colour(Vec3 fallback) const;
};

// All controller families that affect one light/emitter node at a given time.
struct FxNodeParameters {
    std::size_t nodeIndex = 0;
    std::uint16_t nodeId = 0;
    std::uint16_t type = 0;
    std::string name;
    std::vector<FxParameter> parameters;

    const FxParameter* find(std::int32_t family) const;
    bool has(std::int32_t family) const { return find(family) != nullptr; }
    float scalar(std::int32_t family, float fallback, std::size_t component = 0) const;
    Vec3 colour(std::int32_t family, Vec3 fallback) const;
};

// The lip track's pose: retail 00486670 samples a `talk` clip at two times and blends them. The
// fractions are of the clip's length, so one track fits any model's clip.
struct MdlFacePose {
    float fractionA = 0.0f;
    float fractionB = 0.0f;
    float blend = 0.0f;
    float weight = 0.0f; // 0: no lip track. Fades out when the track ends.
};

// What to pose a model with: a clip (perhaps still blending out of an earlier one) and the lip
// track over them. Clips always stay at the model's animation root and below (retail 00483d80;
// see Mdl::headRootName), which is how a separate head follows its body's clips; headRootOnly
// is kept for callers and no longer changes the pose.
// Retail PartLightsaber swing trail state, one per blade node: ages at +0x98, world positions at
// +0xfc, world orientations (w, x, y, z) at +0x228, all 25 slots wide (ctor 004478f0), used as a
// ring of 20 (00740a4c); head +0x3b8, tail +0x3ba, count +0x3bc, last position +0x3c0.
struct SaberTrail {
    std::array<float, 25> age{};
    std::array<Vec3, 25> position{};
    std::array<std::array<float, 4>, 25> rotation{};
    std::int16_t head = 0;
    std::int16_t tail = 0;
    std::int16_t count = 0;
    Vec3 last;
};

// What posing needs to run the trails this frame: the item model's place in the world (a point p
// of the model is at origin + rotation * (scale * p)) and the frame time for the ages (00481ae0).
struct SaberTrailFeed {
    std::vector<SaberTrail>* trails = nullptr; // one per blade node, in node order
    Mat3 rotation{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
    Vec3 origin;
    float scale = 1.0f;
    float dt = 0.0f;
    bool step = true; // run Update (00481ae0) on this pose; retail runs it once a frame (0046be00)
};

struct MdlPoseRequest {
    std::string animation;
    float time = 0.0f;
    std::string previous;
    float previousTime = 0.0f;
    float blend = 1.0f;
    float danglyTime = -1.0f;
    const MdlFacePose* face = nullptr;
    bool headRootOnly = false;
    // A lit saber's swing trail (00471860 / 00481ae0); null draws the still blade.
    SaberTrailFeed* saberTrail = nullptr;
    // Head follow (retail 00487190): turns the model's hturn_g by this rotation in model space.
    // Children follow.
    const Mat3* headTurn = nullptr;
};

// Minimal MDL/MDX parser: node hierarchy, trimesh/skin nodes, world-space
// triangles and animation clips. Skinning is evaluated on the CPU.
class Mdl {
public:
    struct Pose {
        Mat3 rotation;
        Vec3 translation;
        float scale = 1.0f; // The node's own scale controller; a rigid mesh is drawn with it.
    };

    static Mdl parse(std::vector<std::uint8_t> mdl, std::vector<std::uint8_t> mdx, std::string name);

    bool isKotOR2() const { return k2_; }

    std::vector<MdlTriangle> triangles(Vec3 roomOrigin) const;
    std::vector<MdlMeshTriangle> meshTriangles() const;
    std::vector<MdlMeshTriangle> posedTriangles(const std::string& animation, float time,
                                                float danglyTime = -1.0f) const;
    std::vector<MdlMeshTriangle> posedTrianglesFor(const MdlPoseRequest& request) const;
    std::optional<Pose> nodePoseFor(const std::string& node, const MdlPoseRequest& request) const;
    // The model header's animation root (+168, retail 004a1030): neck_g on the shipped head
    // models, the root node on every other shipped model.
    const std::string& headRootName() const { return headRootName_; }
    // The clip's own spelling on this model's chain; empty when it has none.
    std::string clipNamed(const std::string& name) const;
    std::vector<MdlMeshTriangle> blendedTriangles(const std::string& animationA, float timeA,
                                                  const std::string& animationB, float timeB,
                                                  float blend, float danglyTime = -1.0f) const;
    bool hasFxNodes() const;
    // Renderable triangles in world space for a room placed at `origin` (Render flag set).
    // gobParts false: the room's static parts. true: the parts under the room's "<model>a" node, which
    // retail draws as their own object after the static geometry, lit by the object binder 004a3f70
    // (RETAIL row 41, palm_order_m41aa: every M41aa_*a palm part draws at 0047409f after a 004a3f70
    // ShadowAurora bind; every other part at 00455824 in the static pass).
    std::vector<MdlMeshTriangle> roomTriangles(Vec3 origin, bool gobParts = false) const;
    // The AABB (walkmesh) node's faces of a room placed at `origin`, with their surfaces, lightmap and
    // lightmap uvs: what retail grows grass on (00456f30 bins the part answering vtable +0x48).
    std::vector<MdlMeshTriangle> walkmeshTriangles(Vec3 origin) const;
    // The faces a ray test meets in a room placed at `origin` (0046b4c0 -> Part::HitCheck 004429d0 on
    // every part): every mesh node at its bind pose whatever its render flag, except skinned parts and
    // the AABB node, whose faces Scene::collision tests with the hit mask.
    std::vector<MdlMeshTriangle> roomHitTriangles(Vec3 origin) const;
    // Every rendered mesh at its bind pose, turned and moved as one object.
    std::vector<MdlMeshTriangle> placedTriangles(Vec3 origin, const Mat3& rotation) const;
    // Nodes of type 0x11 name another model to show at that node (refmodel, parsed in 00468070).
    struct ReferenceNode {
        std::string model;
        Vec3 position;
        Mat3 rotation;
        std::size_t node = 0;
        std::string nodeName;
    };
    // Where a reference node stands at `time` of `clip` (its bind place without that clip). Rooms
    // move some of them this way: the or_tbat bats of m37aa_07 on animloop2.
    void nodeWorldAt(std::size_t node, const std::string& clip, float time, Vec3& position, Mat3& rotation) const;
    // Every rendered mesh, skinned ones too, posed by `clip` at `time`, then turned and moved as one.
    std::vector<MdlMeshTriangle> placedPosedTriangles(const std::string& clip, float time, Vec3 origin,
                                                      const Mat3& rotation) const;
    std::vector<ReferenceNode> references() const;
    bool hasSkinnedMesh() const;
    // A room's looping clips (retail 00607610 starts animloop1, 2 and 3 on every room), and the
    // meshes each moves or tints; a mesh belongs to the first clip that moves it. Those
    // leave roomTriangles and are posed per frame through loopMeshTriangles. Returns the clip.
    std::vector<std::string> markRoomLoopMeshes();
    std::vector<MdlMeshTriangle> loopMeshTriangles(const std::string& clip, float time, Vec3 origin) const;
    // Room geometry that a "scriptloop" clip moves or tints (ACT_738 PlayRoomAnimation through
    // CSWCRoom::PlayAnimation 00646180): kept out of roomTriangles and the animloop meshes, and
    // posed through scriptLoopTriangles with the clip the script picked (rest pose before one).
    // Returns true when the model has any.
    bool markRoomScriptMeshes();
    std::vector<MdlMeshTriangle> scriptLoopTriangles(const std::string& clip, float time, Vec3 origin) const;
    // Light nodes and emitter nodes, in model space / fxNodeParameters ordinal order.
    std::vector<MdlLight> lights() const;
    struct EmitterNode {
        std::size_t ordinal = 0;
        std::string name;
        MdlEmitter header;
    };
    std::vector<EmitterNode> emitters() const;
    Vec3 rootMotion(const std::string& animation, float time) const;
    bool hasNode(const std::string& name) const;

    const std::string& name() const { return name_; }
    const std::string& supermodel() const { return supermodel_; }
    void mergeAnimations(const Mdl& supermodel);
    const std::vector<MdlAnimationInfo>& animations() const { return animations_; }
    int animationIndex(const std::string& name) const;
    float animationDuration(const std::string& name) const;
    float animationTransition(const std::string& name) const;
    // A cutscene model whose body is a skeleton of hidden "_g" proxies: it carries the clips,
    // while the participant's own body is what can be seen.
    bool hasProxyBody() const;
    std::size_t skinnedNodeCount() const;
    std::size_t danglyNodeCount() const;
    bool hasNonPoseControllers() const;
    bool hasControllerType(std::int32_t type) const;
    std::set<std::int32_t> fxControllerTypes() const;
    std::size_t fxNodeCount() const;
    FxNodeParameters fxNodeParameters(std::size_t ordinal, const std::string& animation,
                                      float time) const;
    float skinBindError() const;
    std::optional<Pose> nodePose(const std::string& node, const std::string& animation,
                                 float time) const;
    // Animation names in shipped models differ in case from the scripts naming them
    // ("Gear0" and "gear0"), so these match without regard to case.
    bool hasAnimationAnyCase(const std::string& name) const;
    float animationDurationAnyCase(const std::string& name) const;
    std::vector<MdlAnimEvent> animationEvents(const std::string& name) const;
    std::vector<MdlMeshTriangle> posedTrianglesLayered(const std::vector<MdlAnimLayer>& layers,
                                                       bool renderableOnly) const;
    // The posed faces a line-of-sight cast also tests but the renderer does not draw: mesh nodes
    // with the render flag off (AABB hit nodes such as hitbullet), skinned ones excepted (00440ff0
    // skips a part whose vfunc +0x50 is non-zero, 00641db0).
    std::vector<MdlMeshTriangle> posedHiddenTrianglesLayered(const std::vector<MdlAnimLayer>& layers) const;
    std::optional<Pose> nodePoseLayered(const std::string& node,
                                        const std::vector<MdlAnimLayer>& layers) const;
    // Faces of one mesh node in model space at the bind pose (AABB hit nodes).
    std::vector<std::array<Vec3, 3>> nodeFaces(const std::string& node) const;

    // Retail stencil shadows. A mesh node whose header has the shadow byte (node +0x187) or the
    // beaming byte (+0x188) answers PartTriMesh vtable +0x5c (00474220, also the dangly and
    // lightsaber part tables); lightsaber nodes (type 0x821, 0043e3a0) return there at once, and a
    // skin's +0x5c (PartSkin::Project 00477b90) returns at once while 00828058 is zero, which no
    // code writes. The data is what 00474220 reads: the node's MDL vertex coordinates (+0x198) and
    // its faces (+0x58: plane normal and distance, adjacent faces +0x14, vertices +0x1a).
    struct ShadowMesh {
        struct Face {
            Vec3 normal;
            float distance = 0.0f;
            std::array<std::int16_t, 3> adjacent{};
            std::array<std::uint16_t, 3> vertex{};
        };
        std::vector<Vec3> vertices;
        std::vector<Face> faces;
        bool beaming = false;
    };
    struct ShadowPart {
        const ShadowMesh* mesh = nullptr;
        Pose pose; // the node's place in model space
    };
    // Every caster node of the model at the pose the request gives.
    std::vector<ShadowPart> shadowPartsFor(const MdlPoseRequest& request) const;
    bool hasShadowMeshes() const;
    // Model header +0x50 (classification; retail reads it as a word at model +0x50): 0x04 character,
    // 0x20 placeable. Scene::DoGobShadowsForThisLight 00450550 takes gobs with either bit.
    std::uint8_t classification() const { return classification_; }
    // The next header byte (+0x51, the word's high byte): Scene::ObjectHitCheck 0044fe00 tests its 0x04 bit.
    std::uint8_t subclassification() const { return subclassification_; }

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
        std::vector<std::int32_t> slotNode;
        std::vector<std::array<float, 4>> qbones;
        std::vector<Vec3> tbones;
        std::uint32_t weightsOffset = 0;
        std::uint32_t bonesOffset = 0;
    };

    struct Dangly {
        std::vector<float> constraints;
        float displacement = 0.0f;
        float tightness = 0.0f;
        float period = 0.0f;
    };

    struct Saber {
        std::vector<Vec3> positions;
        std::vector<Vec2> uvs;
        std::vector<Vec3> normals;
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
        std::uint32_t uv0Offset = 24u;
        std::uint32_t uv1Offset = 0xFFFFFFFFu;
        MdlNodeMaterial material;
        std::optional<MdlEmitter> emitter;
        std::optional<MdlLight> light;
        bool trimesh = false;
        bool render = true; // Trimesh header flag; false on AABB hit nodes.
        bool loopAnimated = false; // Moved by one of the room's looping clips.
        int loopClip = -1; // Which one.
        bool scriptLoop = false; // Moved or tinted by a scriptloop clip (markRoomScriptMeshes).
        std::optional<Skin> skin;
        std::optional<Dangly> dangly;
        std::optional<Saber> saber;
        std::shared_ptr<const ShadowMesh> shadowMesh;
        std::vector<Controller> controllers;
    };

    struct Animation {
        std::string name;
        float duration = 0.0f;
        float transition = 0.0f;
        std::vector<Node> nodes;
        std::vector<MdlAnimEvent> events;
    };

    std::vector<Node> parseNodes(std::uint32_t rootOffset) const;
    std::vector<Controller> parseControllers(std::uint32_t nodeHeader) const;
    void resolveSkins();
    static std::vector<float> sampleController(const Controller& controller, float time);
    Animation parseAnimation(std::uint32_t offset) const;
    std::map<std::uint16_t, const Node*> animationNodes(const Animation& animation) const;
    const Animation* findAnimationAnyCase(const std::string& name) const;
    std::vector<Pose> layeredWorld(const std::vector<MdlAnimLayer>& layers) const;
    std::vector<Pose> poseWorld(const Animation* animation, float time,
                                const MdlFacePose* face = nullptr, bool headRootOnly = false,
                                const std::vector<std::pair<const Animation*, float>>* overlays = nullptr,
                                const Mat3* headTurn = nullptr) const;
    std::vector<MdlMeshTriangle> trianglesFromPose(const std::vector<Pose>& world,
                                                   float danglyTime,
                                                   const std::function<bool(const Node&)>& filter = {},
                                                   const Animation* tintClip = nullptr,
                                                   float tintTime = 0.0f,
                                                   SaberTrailFeed* saberTrail = nullptr,
                                                   // Several clips, lowest priority first; each
                                                   // replaces the tint channels it animates.
                                                   const std::vector<std::pair<const Animation*, float>>* tintLayers
                                                   = nullptr) const;

    std::string name_;
    std::string supermodel_;
    bool k2_ = false;
    bool affectedByFog_ = true;
    std::uint8_t classification_ = 0;
    std::uint8_t subclassification_ = 0;
    std::vector<std::uint8_t> mdl_;
    std::vector<std::uint8_t> mdx_;
    std::vector<Node> nodes_;
    // Immutable once parsed, so inheriting a supermodel's clips shares rather than copies them.
    std::vector<std::shared_ptr<const Animation>> animationData_;
    std::vector<MdlAnimationInfo> animations_;
    std::vector<Pose> bindWorld_;
    std::vector<Pose> worldFor(const MdlPoseRequest& request) const;
    std::string headRootName_;
    std::vector<bool> underHeadRoot_;
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

    // TSLPatcher-style merge: patch rows overwrite non-empty cells, new columns
    // are appended and new rows are appended after the base rows.
    static TwoDa merge(const TwoDa& base, const TwoDa& patch, std::string name);

private:
    struct Row {
        std::string label;
        std::vector<std::string> cells;
    };
    static TwoDa parseTable(std::vector<std::uint8_t> bytes, std::string name);
    void buildIndex();

    std::string name_;
    std::vector<std::string> columns_;
    std::vector<Row> rows_;
    std::unordered_map<std::string, int> columnIndex_;
    std::unordered_map<std::string, int> rowIndex_;
};

// ---- TLK -----------------------------------------------------------------------------------------------------------------

class Tlk {
public:
    static Tlk parse(std::vector<std::uint8_t> bytes, std::string name);
    static Tlk fromEntries(std::uint32_t language,
                           const std::vector<std::pair<std::string, std::string>>& entries,
                           std::string name);
    // TSLPatcher-style merge: the first `replaceCount` patch entries overwrite
    // (empty fields keep the base value); remaining entries append. The
    // default replaces all patch entries.
    static Tlk merge(const Tlk& base, const Tlk& patch, std::string name,
                     std::size_t replaceCount = static_cast<std::size_t>(-1));

    std::uint32_t language() const { return language_; }
    std::size_t size() const { return entries_.size(); }
    std::string text(std::uint32_t strref) const;
    // Gendered lookup. gender 1 reads the feminine table (dialogf.tlk, attached with
    // attachFemale) when the language is not English, and falls back to the masculine
    // entry when the feminine one is missing or empty. English is always masculine
    // (retail forces gender 0 for language 0).
    std::string text(std::uint32_t strref, int gender) const;
    void attachFemale(Tlk female);
    bool hasFemale() const { return female_ != nullptr; }
    std::string sound(std::uint32_t strref) const;
    // TLK V3.0 serializer for patch fixtures and mod tooling.
    std::vector<std::uint8_t> toBytes() const;

private:
    struct Entry {
        std::uint32_t offset = 0;
        std::uint32_t length = 0;
        std::string sound;
    };
    std::string name_;
    std::uint32_t language_ = 0;
    std::vector<Entry> entries_;
    std::vector<std::uint8_t> text_;
    std::shared_ptr<const Tlk> female_;
};

// KotOR SSF V1.1 (type 2060): a fixed table of StrRefs into dialog.tlk.
// The TLK entry's sound resref is the playable stream.
class Ssf {
public:
    static Ssf parse(std::vector<std::uint8_t> bytes, std::string name);

    std::size_t slotCount() const { return strrefs_.size(); }
    std::int32_t strref(std::size_t slot) const;
    std::size_t filledSlots() const;

private:
    std::string name_;
    std::vector<std::int32_t> strrefs_;
};

// ---- dialogue ------------------------------------------------------------------------------------------------------------

// One AnimList item of a node: the participant tag (PLAYER for the PC) and the animation id
// (>= 10000: dialoganimations.2da row + 10000; 1200..: the cutscene clip CUT0nnW).
struct DlgAnim {
    std::string participant;
    std::int32_t animation = 0;
};

// Presentation fields every node carries (retail reader 0059f5f0): the voice-over, the
// screen fade and the participants' animations.
struct DlgNodePresentation {
    std::string voResRef;           // VO_ResRef: the voice. `sound` is a separate effect sound.
    std::uint32_t soundExists = 0;  // SoundExists bit 0 voice file, bit 1 Sound
    std::int32_t fadeType = 0;      // 0 none, 1/2 instant cut, 3 fade in, 4 fade out
    float fadeColor[3]{};
    float fadeDelay = 0.0f;
    float fadeLength = 0.0f;
    std::vector<DlgAnim> animList;
    // CamFieldOfView (LoadDialogCamera 0059eaa0): -1 when absent or not above 0.
    float camFieldOfView = -1.0f;
};

struct DlgEntry : DlgNodePresentation {
    std::string speaker;
    std::string listener;
    std::string text;
    std::string script;
    std::string active;
    std::string quest;
    std::int32_t questEntry = 0;
    bool hasQuestEntry = false;
    std::int32_t plotIndex = -1;
    float plotXpPercentage = 1.0f;
    std::uint32_t delay = 0xFFFFFFFFu;
    std::uint32_t waitFlags = 0;
    std::int32_t action[4]{};
    std::string comment;
    std::string sound;
    std::int32_t cameraId = -1;
    std::int32_t cameraAnimation = -1;
    std::int32_t cameraAngle = -1;
    std::int32_t camVidEffect = -1;
    float camHeightOffset = 0.0f;
    float tarHeightOffset = 0.0f;
    std::vector<std::uint32_t> replies;
    // RepliesList[i].Active, parallel to `replies`. The condition lives on the
    // link, not the node; a shorter vector means unconditional.
    std::vector<std::string> repliesActive;
};

struct DlgReply : DlgNodePresentation {
    std::string speaker;
    std::string listener;
    std::string text;
    std::string script;
    std::string active;
    std::string quest;
    std::int32_t questEntry = 0;
    bool hasQuestEntry = false;
    std::int32_t plotIndex = -1;
    float plotXpPercentage = 1.0f;
    std::uint32_t delay = 0xFFFFFFFFu;
    std::uint32_t waitFlags = 0;
    std::int32_t action[4]{};
    std::string comment;
    std::string sound;
    std::int32_t cameraId = -1;
    std::int32_t cameraAnimation = -1;
    std::int32_t cameraAngle = -1;
    std::int32_t camVidEffect = -1;
    float camHeightOffset = 0.0f;
    float tarHeightOffset = 0.0f;
    std::vector<std::uint32_t> entries;
    // EntriesList[i].Active, parallel to `entries`.
    std::vector<std::string> entriesActive;
};

struct Dlg {
    std::vector<DlgEntry> entries;
    std::vector<DlgReply> replies;
    std::vector<std::uint32_t> starting;
    std::vector<std::string> startingActive;
    std::string cameraModel;
    // Root fields of the cutscene dialogues (retail reader 005a2ae0): the track that plays
    // during the conversation, the stunt models that stand in for the participants
    // (StuntList), and the conversation type (0 cinematic, 1 computer, 2 other).
    std::string ambientTrack;
    std::int32_t conversationType = 0;
    bool animatedCut = false;
    struct Stunt {
        std::string participant;
        std::string model;
    };
    std::vector<Stunt> stunts;
    std::string endConversation;
    std::string endConversationAbort;
    bool skippable = true;

    // entryGender/replyGender pick the feminine TLK text (when attached) for NPC
    // lines (the speaker's gender) and player lines (the player's gender).
    static Dlg parse(const Gff& gff, const Tlk* tlk, std::string name,
                     int entryGender = 0, int replyGender = 0);
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
    bool plot = false;
    std::int32_t charges = 0;
    std::int32_t addCost = 0;
    std::int32_t modelVariation = 0;
    // UTI PropertiesList, in file order (ScriptItems.cpp reads these for item use).
    struct Property {
        std::int32_t name = 0;
        std::int32_t subtype = 0;
        std::int32_t costTable = 0;
        std::int32_t costValue = 0;
        std::int32_t param1 = 0;
        std::int32_t param1Value = 0;
        std::int32_t chanceAppear = 100; // carried and saved (retail 00555790); every shipped UTI has 100
        std::int32_t usesPerDay = 255;
        bool useable = true;
        std::int32_t upgradeType = 255; // the upgrade.2da row that turns this property on; 255 = always
    };
    std::vector<Property> properties;
};

// One UTC ItemList (carried) or Equip_ItemList (worn) entry.
struct CreatureItem {
    std::string resref;
    bool droppable = true;
    std::int32_t slot = -1; // INVENTORY_SLOT_* for worn items, -1 for carried
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
    std::int32_t difficultyIndex = -1; // encdifficulty.2da row
    std::int32_t faction = 0;
    bool playerOnly = false;
    bool reset = false;
    bool active = true;
    // Runtime counters a saved encounter carries (retail ReadEncounterFromGff 00592430); a UTE has none.
    bool started = false;                // Started (+0x278): the wave was spawned and not yet wiped
    std::int32_t numberSpawned = 0;      // NumberSpawned (+0x264)
    std::int32_t currentSpawns = 0;      // CurrentSpawns (+0x2c8): respawns already used
    std::uint32_t lastSpawnDay = 0;      // LastSpawnDay / LastSpawnTime (+0x270 / +0x274)
    std::uint32_t lastSpawnTime = 0;
    std::vector<std::uint32_t> areaObjects; // AreaList.AreaObject: object ids of the creatures it spawned
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

struct CameraDef {
    std::uint32_t id = 0;
    Vec3 position;
    float qw = 1.0f;
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
    float pitch = 0.0f;
    float height = 0.0f;
    float fov = 60.0f;
};

// ---- textures ------------------------------------------------------------------------------------------------------------

Image decodeTpc(const std::vector<std::uint8_t>& data, const std::string& name);
Image decodeTga(const std::vector<std::uint8_t>& data, const std::string& name);
Image decodeTexture(const std::vector<std::uint8_t>& data, std::uint32_t type, const std::string& name);

// The TXI text appended after a TPC's pixel data (empty when there is none).
std::string tpcEmbeddedTxi(const std::vector<std::uint8_t>& data);
// Wrap flags from a TXI `clamp <int>` line (retail 00422390 reads it with "%d" into a 16-bit
// field): bit 0 clamps S, bit 1 clamps T. No such line is 0, which repeats on both axes.
std::uint8_t txiClampFlags(const std::string& txi);
// Wrap flags for one shipped texture: the TXI appended to a TPC, else the separate TXI
// resource (`separateTxi`, may be null) that accompanies TGA art.
std::uint8_t textureClampFlags(const std::vector<std::uint8_t>& data, std::uint32_t type,
                               const std::vector<std::uint8_t>* separateTxi);

// ---- module scene --------------------------------------------------------------------------------------------------------

struct CollisionFacet {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
    std::uint32_t surface = 0;
    Vec3 normal;
    bool floor = false;
    std::uint32_t room = 0;  // Index into Scene::rooms.
    bool walkable = true;    // surfacemat.2da "walk" for this surface.
    bool blocksSight = true; // surfacemat.2da "lineofsight" for this surface.
    std::uint8_t seamEdges = 0; // Bit i: the edge from vertex i leads into another room.
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

// UTC sheet fields that script queries read (GetRacialType, GetGender,
// GetAbilityScore, GetLevelByPosition, GetSkillRank, ...). `parsed` is false for
// entities that did not come from a UTC.
struct CreatureStats {
    bool parsed = false;
    std::int32_t race = 0;
    std::int32_t subRace = 0;
    std::int32_t gender = 0;
    float challengeRating = 0.0f;
    float perceptionRange = 11.0f;
    std::int32_t naturalAC = 0;
    std::int32_t goodEvil = 50;
    std::array<std::int32_t, 6> abilities{10, 10, 10, 10, 10, 10}; // Str Dex Con Int Wis Cha
    std::vector<std::pair<std::int32_t, std::int32_t>> classes;    // ClassList: class, level
    std::vector<std::int32_t> skills;                              // SkillList ranks, row order
    std::vector<std::int32_t> feats;                               // FeatList
    std::vector<std::int32_t> powers;                              // ClassList[].KnownList0
    std::int32_t forcePoints = 0;                                  // CurrentForce
    std::int32_t maxForcePoints = 0;                               // ForcePoints
    // Filled by Rules::deriveCreatureCombat from the fields above and the items.
    bool combatDerived = false;
    std::int32_t baseAttack = 0;
    struct Weapon {
        bool present = false;
        std::int32_t baseItem = -1;
        std::int32_t dice = 1;
        std::int32_t die = 2;
        std::int32_t flatDamage = 0;
        std::int32_t critThreat = 1;
        std::int32_t critMult = 2;
        std::int32_t attackBonus = 0; // Everything but the d20, including penalties.
        std::int32_t maxDamage = 2;
        bool ranged = false;
        bool keen = false;
    };
    Weapon mainHand;
    Weapon offHand;
};

enum class EntityKind { Door, Placeable, Creature, Waypoint, Trigger, Encounter, Sound, Store, Unknown };

// What the armour in the Body slot does to a creature (retail 00697610): BodyVariation picks the
// appearance.2da model<letter>/tex<letter> pair (1 = a .. 10 = j), TextureVar the "%s%02d" suffix.
// Zero means no armour, the row's modela.
struct BodyVariant {
    std::int32_t variation = 0;
    std::int32_t texture = 0;
    bool operator==(const BodyVariant&) const = default;
};

struct Entity {
    // Where its light is read, from the entity's position.
    Vec3 lightOffset{0.0f, 0.0f, 1.0f};
    EntityKind kind = EntityKind::Unknown;
    std::uint32_t sourceStructure = 0; // GIT struct for trigger geometry and diagnostics
    std::string templateResref;
    std::string tag;
    std::string name;
    std::string conversation;
    Vec3 position;
    float bearing = 0.0f;
    std::uint32_t savedObjectId = 0; // ObjectId of a struct read from a save (0 for a module template)
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
    std::string soundSet;
    // appearance.2da footsteptype: the footstepsounds.2da row, -1 when the cell is blank (Hutt,
    // Gizka, SeaBeast, Tach: retail 0061a2d0 plays no step for those).
    int footstepType = 0;
    // appearance.2da sizecategory: retail 0061a2d0 / 00611470 pick the medium-and-small voice and
    // footstep priority groups for 1..3 and the large ones otherwise.
    int sizeCategory = 3;
    int soundAppType = 0;
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
    std::int32_t walkRateRow = 7; // UTC WalkRate: creaturespeed.2da row; 7 = from appearance.
    bool ranged = false;
    float attackRange = 2.4f;
    std::string weaponModel;
    std::string offhandModel;
    // The head slot's item model (baseitems 44 Mask, I_Mask_nnn): retail 006964c0 hangs it on
    // the head model's GoggleHook.
    std::string headItemModel;
    // retail 00610f40 hangs the right and left hand items on these hooks. The hook is a child
    // of the hand bone, placed and turned to be the grip.
    std::string weaponAttach = "rhand";
    std::string offhandAttach = "lhand";
    // baseitems.2da weaponwield of what each hand holds, the input of wieldClass.
    std::int32_t rightWield = 0;
    std::int32_t leftWield = 0;
    float stunTimer = 0.0f;
    float fearTimer = 0.0f;
    float forceSuppressedTimer = 0.0f;
    std::vector<ActiveEffect> effects;
    bool hostile = false;
    bool hostileOverride = false;
    bool dead = false;
    bool dying = false;   // Killed this moment: the body has its death clip to play once.
    // A killed creature's body stays in the world; a destroyed object leaves it, and DestroyObject
    // keeps its hit points above zero so that survives a save.
    bool corpse() const { return dead && kind == EntityKind::Creature && hitPoints <= 0; }
    // The turn that takes this entity's model into the world. A creature's bearing is the way it
    // faces, as in GetFacing and the GIT orientation vector, and its model faces +Y, a quarter turn
    // on from that (same-camera retail frames, tar_m02aa). Doors and placeables turn by Bearing itself.
    float drawBearing() const { return kind == EntityKind::Creature ? bearing - 1.5707963f : bearing; }
    bool partyAvatar = false;
    bool useable = false;
    bool open = false;
    // UTD OpenState (0 closed, 1 and 2 the two open sides, 3 destroyed) and UTP AnimationState.
    std::int32_t openState = 0;
    std::int32_t animationState = 0;
    bool locked = false;
    // UTD/UTP/UTC gameplay flags. Data only here; the world-state family gives
    // them effect.
    bool lockable = false;
    bool keyRequired = false;
    std::string keyName;
    std::int32_t openLockDC = 0;
    std::int32_t closeLockDC = 0;
    bool autoRemoveKey = false;
    bool plot = false;
    bool min1Hp = false;
    bool trapFlag = false;
    bool trapDetectable = false;
    bool trapDisarmable = false;
    bool trapOneShot = false;
    std::int32_t trapDetectDC = 0;
    std::int32_t trapDisarmDC = 0;
    std::int32_t trapType = 0;
    bool hasInventory = false;
    bool isStatic = false;
    std::int32_t hardness = 0;
    // UTT Type: 0 generic, 1 area transition, 2 trap. Trap state is runtime.
    std::int32_t triggerType = 0;
    bool trapDetected = false;
    bool trapDisarmed = false;
    bool trapSprung = false;
    // Merchant template data for Store entities.
    std::int32_t storeMarkUp = 100;
    std::int32_t storeMarkDown = 40;
    std::int32_t storeBuySell = 3;
    std::vector<std::pair<std::string, bool>> storeItems;
    std::vector<std::string> lootItems;
    // UTC ItemList and Equip_ItemList (creatures only).
    std::vector<CreatureItem> creatureItems;
    BodyVariant bodyVariant; // from the Body slot's item when spawned
    // A waypoint's map note (GIT HasMapNote, MapNoteEnabled, MapNote; retail 005c7f30).
    bool hasMapNote = false;
    bool mapNoteEnabled = false;
    std::string mapNote;
    std::uint32_t mapNoteStrref = 0xFFFFFFFFu;
    CreatureStats stats;
    // Hook name (ScriptSpawn, OnOpen, ...) to script resref. Template values
    // are loaded first; non-empty GIT instance values override them.
    std::map<std::string, std::string> scripts;
};

constexpr std::uint32_t kNoTexture = UINT32_MAX;

// The lights one object is drawn with: retail enables at most three GL lights per object
// (004a3f70) and sets the rest of the area's light through ambient-only lights.
struct SampledLight {
    Vec3 ambient;
    struct Light {
        Vec3 position;       // Or a direction when quadratic < 0.
        Vec3 colour;
        float quadratic = -1.0f; // GL_QUADRATIC_ATTENUATION; the constant term is GL's default 1.
    };
    std::array<Light, 3> lights{};
};

struct TextureBatch {
    std::uint32_t texture = 0; // index into Scene::textures
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    BlendMode blend = BlendMode::Opaque;
    std::uint32_t lightmap = kNoTexture;
    std::uint32_t envmap = kNoTexture;
    std::uint32_t bumpmap = kNoTexture; // normal map; needs the mesh's tangents (retail 0043e410)
    int bumpCycleX = 1;     // the bump map's own TXI cycle
    int bumpCycleY = 1;
    float bumpCycleFps = 0.0f;
    float bumpSpecular = 0.0f; // 830250: the map's specular colour, on the route that has the term
    // Alpha below or at this discards the fragment; negative means no test.
    float alphaRef = -1.0f;
    bool twoSided = false;
    bool decal = false;     // TXI decal (material +0x40): vertex light starts at 1 (0046a9e0)
    bool unlit = false;     // particles: the vertex colour is the whole light
    bool envMask = false;   // texture alpha selects the environment map, the surface stays opaque
    bool envReflectionUnlit = false; // skinned env surfaces: fragment program 830248, the reflection is not lit (see 0046ff70)
    bool writeDepth = true;
    bool noFog = false;     // retail's additive emitter draws turn fog off
    bool depthTest = true;  // lens flares draw over everything
    bool opaqueOutput = false; // output alpha 1 under SRC_ALPHA / ONE: retail's ONE / ONE for flares
    float envOnlyAlpha = -1.0f; // >= 0: only the environment map, at this alpha (water)
    bool showsBehind = false;   // Its pixels can let what is behind them show (texture alpha, water).
    std::uint32_t shell = 0;    // A force shell's pass over its creature (EffectDirector), or 0.
    Vec3 materialDiffuse{1.0f, 1.0f, 1.0f}; // GL material for the lit path (retail 00473b55).
    Vec3 materialAmbient{1.0f, 1.0f, 1.0f};
    float transparencyHint = 0.0f;
    // UV animation: a scroll from the trimesh header, or a TXI cycle over an atlas.
    float scrollU = 0.0f;
    float scrollV = 0.0f;
    int cycleX = 1;
    int cycleY = 1;
    float cycleFps = 0.0f;
    float distort = 0.0f;   // TXI arturo procedure amplitude
    // Index into Scene::rooms for room geometry, for VIS culling. -1 is always drawn.
    std::int32_t room = -1;
    Vec3 center;            // back-to-front sort, grass chunk range
    float maxDistance = 0.0f; // > 0: not drawn when the camera is farther than this (grass)
    // A room batch without a lightmap has no per-entity light to read: filled at load.
    bool bakedLight = false;
    // Part of a model retail draws as its own object after RenderShadows (a reference node's model, or
    // a room's "<model>a" parts): not drawn with the static geometry, so the shadow fan stays off it.
    bool roomObject = false;
    SampledLight baked;
};

struct SceneVertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    Vec2 uv1;
    Vec3 selfIllum;      // emissive colour, or the particle colour on an unlit batch
    float alpha = 1.0f;
};

// Per-draw shader values for a batch at scene time `clock` (both backends use this).
struct BatchUv {
    float scaleU = 1.0f;
    float scaleV = 1.0f;
    float offsetU = 0.0f;
    float offsetV = 0.0f;
};
BatchUv batchUvAt(const TextureBatch& batch, float clock);

// Geometry for one GIT instance. Vertices are in model space; the entity's
// position/bearing is applied by the renderer. Animated meshes keep their MDL
// and bind triangles so a pose can refresh the vertex buffer without rebuilding
// topology.
inline std::uint64_t nextEntityMeshUid()
{
    static std::atomic<std::uint64_t> next{1u};
    return next.fetch_add(1u, std::memory_order_relaxed);
}

struct EntityMesh {
    // Follows the mesh through list changes, so a renderer keeps what it uploaded.
    std::uint64_t uid = nextEntityMeshUid();
    std::uint32_t entity = 0;
    std::shared_ptr<Mdl> model;
    std::vector<SceneVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<TextureBatch> batches;
    std::string animation;
    float animationTime = 0.0f;
    float animationRate = 1.0f; // Clip seconds per second: a walking body's clip is cut to its speed (CSWCCreature::DriveAnimationUpdate 00615960).
    bool animationLoop = true;
    std::string previousAnimation;
    float previousAnimationTime = 0.0f;
    bool previousAnimationLoop = true; // The blended-from clip's own looping, not the new clip's.
    float blendDuration = 0.0f;
    float blendRemaining = 0.0f;
    float danglyTime = 0.0f;
    bool fx = false;
    std::shared_ptr<Mdl> attachModel;
    std::string attachNode;
    // A head slot item (006964c0): it hangs on attachHead's "gogglehook", and the head hangs on
    // the body's attachNode (headhook), turned by attachHeadTurn (the body mesh's headTurn, copied
    // each frame like attachAnimation).
    std::shared_ptr<Mdl> attachHead;
    Mat3 attachHeadTurn{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
    std::string attachAnimation;
    float attachTime = 0.0f;
    // Separate head geometry merged into this body mesh. The head follows the
    // body node `headNode` (normally `headhook`) through body animations.
    std::shared_ptr<Mdl> headModel;
    std::string headNode;
    MdlFacePose face;
    // Where the head looks in a conversation, in model space, radians (see GameSession::
    // tickHeadFollow): headTurn is that as a rotation, identity when it looks ahead.
    float headYaw = 0.0f;
    float headPitch = 0.0f;
    Mat3 headTurn;
    // An item in the hand: the parent's blend (attachAnimation/attachTime are its current clip),
    // appearance.2da weaponscale, and hidden by a clip whose hideequippeditems is set.
    std::string attachPrevious;
    float attachPreviousTime = 0.0f;
    float attachBlend = 1.0f;
    float attachScale = 1.0f;
    bool attachHidden = false;
    // The stance tick's state: the clip it last chose (anything else is someone else's clip),
    // where the creature was, and how it was moving.
    std::string stanceClip;
    Vec3 stancePosition;
    bool stancePlaced = false;
    float stanceStill = 1.0f;
    bool stanceRunning = false;
    float stanceSpeed = 0.0f;  // Measured forward speed, and the distance one cycle of the walk or run clip covers (appearance DRIVEANIMWALK/RUN).
    float stanceStride = 1.0f;
    std::int32_t stanceAttack = 0;
    bool visible = true;
    // Placed by its clip rather than by its entity: `center` is where the posed mesh is.
    bool clipPlaced = false;
    Vec3 center;
    std::uint32_t revision = 0;
    // A room's scriptloop group (Mdl::markRoomScriptMeshes): posed through scriptLoopTriangles
    // with `animation`, which setRoomClip changes; an empty clip is the rest pose.
    bool scriptLoop = false;
    // Animated room geometry: posed through loopMeshTriangles, in world space.
    bool roomAnimated = false;
    Vec3 origin;
    int room = -1;
    // A skinned model shown by a reference node: `model` is posed by its own clip, at the place that
    // node of `referenceHolder` has in `referenceClip`.
    std::shared_ptr<const Mdl> referenceHolder;
    std::size_t referenceNode = 0;
    std::string referenceClip;
    float referenceTime = 0.0f;
    // A hand item's saber trails (SaberTrail). The game loop sets trailPlaced with the entity's
    // place and the frame time before it refreshes the mesh; without it the blade draws still.
    std::vector<SaberTrail> saberTrails;
    bool trailPlaced = false;
    // A world-placed model's non-render faces in world space (bakeWorldEntityMesh), for the flare's
    // line-of-sight cast, which tests them as it tests the drawn ones.
    std::vector<std::array<Vec3, 3>> hiddenFaces;
    Vec3 trailPosition;
    float trailBearing = 0.0f;
    float trailDt = 0.0f;
    bool trailStep = false; // set once a frame by the game loop; other refreshes only draw
    // The stencil shadow casters of this mesh in its model space, set with each pose. Their meshes
    // belong to `model` or `headModel`, which this mesh keeps alive.
    std::vector<Mdl::ShadowPart> shadowParts;
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
    void addLips();
    // Mounts rims/<stem>.rim and rims/<stem>dx.rim (the front end's own archives, e.g. the
    // main menu's 3D model); retail mounts them by name as RIMS:<STEM>.
    void addRim(const std::string& stem);
    void setOverlayRoot(std::filesystem::path root);
    const std::filesystem::path& overlayRoot() const { return overlayRoot_; }
    std::optional<std::vector<std::uint8_t>> find(const std::string& resref, std::uint32_t type,
                                                  std::string* source = nullptr);
    std::optional<std::vector<std::uint8_t>> findTexture(const std::string& resref, std::uint32_t* type,
                                                         std::string* source = nullptr);
    std::vector<std::string> resourceNames(std::uint32_t type) const;

    // dialog.tlk of this install, read once. Null when missing or malformed.
    std::shared_ptr<const Tlk> dialogTlk();
    // A 2DA by name as the current layers resolve it. Null when missing or malformed.
    std::shared_ptr<const TwoDa> table(const std::string& name);
    // Models parsed under the current layers. Models are immutable once merged, so meshes
    // can share one. Everything here is dropped whenever the layers change.
    struct CachedModel {
        std::shared_ptr<Mdl> model;
        std::string error;
    };
    std::unordered_map<std::string, CachedModel>& modelCache() { return models_; }
    std::unordered_map<std::string, std::shared_ptr<const Mdl>>& supermodelCache() { return supermodels_; }

private:
    struct Layer {
        int pass = 2;
        int order = 0;
        int sequence = 0;
        std::string name;
        Archive archive;
    };
    struct LooseFile {
        std::uint32_t type = 0;
        std::filesystem::path path;
    };

    const BifFile& bif(std::uint32_t index);
    std::filesystem::path bifPath(const std::string& keyName) const;
    std::filesystem::path overlayPath(const std::string& relative) const;
    void addLayer(int pass, int order, const std::filesystem::path& path,
                  const std::string& name);
    void sortLayers();
    void indexLooseDirectory(const std::filesystem::path& directory);
    void rebuildLooseIndex();

    std::filesystem::path root_;
    std::filesystem::path overlayRoot_;
    GameProfile profile_;
    std::string buildSha256_;
    KeyFile key_;
    std::unordered_map<std::uint32_t, BifFile> bifs_;
    std::vector<Layer> layers_;
    std::unordered_map<std::string, std::vector<LooseFile>> loose_;
    int nextSequence_ = 0;
    void dropResolvedCaches();

    std::shared_ptr<const Tlk> dialogTlk_;
    bool dialogTlkRead_ = false;
    std::unordered_map<std::string, std::shared_ptr<const TwoDa>> tables_;
    std::unordered_map<std::string, CachedModel> models_;
    std::unordered_map<std::string, std::shared_ptr<const Mdl>> supermodels_;
};

// One ARE "Rooms" entry. Retail (swkotor.exe 00509780) reads EnvAudio and
// AmbientScale only from here, per room; the GIT AreaProperties EnvAudio is not
// read. `room` is the room name that matches the LYT room model.
struct AreaRoomAudio {
    std::string room;
    std::int32_t envAudio = -1;  // soundeax.2da row id, -1 when absent
    float ambientScale = 1.0f;
};

// Area audio routing resolved from GIT AreaProperties through
// ambientmusic.2da, ambientsound.2da and soundeax.2da.
struct AreaAudio {
    std::int32_t musicDayId = -1;
    std::int32_t musicNightId = -1;
    std::int32_t musicBattleId = -1;
    std::int32_t ambientDayId = -1;
    std::int32_t ambientNightId = -1;
    std::string musicDay;
    std::string musicNight;
    std::string musicBattle;
    std::string ambientDay;
    std::string ambientNight;
    std::string eaxPreset = "GENERIC";
    int ambientDayVolume = 0;
    int ambientNightVolume = 0;
    int musicDelay = 0;
    // Per-room environments from the ARE (the retail source of the EAX room).
    std::vector<AreaRoomAudio> rooms;
};

// One GIT SoundList instance and its UTS template.
struct SoundEmitter {
    std::uint32_t entity = 0;
    std::string templateResref;
    std::vector<std::string> sounds;
    Vec3 position;
    float elevation = 0.0f;
    float minDistance = 1.0f;
    float maxDistance = 10.0f;
    float volume = 1.0f;          // Volume / 127 (retail clamps 0..127: 005c8df0)
    bool active = true;
    bool continuous = true;
    bool looping = false;
    bool positional = false;
    bool random = false;
    int interval = 0;             // milliseconds
    int intervalVariation = 0;    // milliseconds
    // Further UTS fields read by the retail loader 005c9040 (defaults are its constructor's).
    int volume127 = 127;          // Volume as stored, 0..127
    int volumeVariation = 0;      // VolumeVrtn
    int times = 3;                // 1 day only, 2 night only, 3 always; 0 uses `hours`
    std::uint32_t hours = 0;      // bit per game hour, used when times == 0
    float pitchVariation = 0.0f;  // PitchVariation, 0..1
    float fixedVariance = 1.0f;   // FixedVariance (playback-rate factor, 0.5..2.0)
    bool randomPosition = false;
    float randomRangeX = 0.0f;
    float randomRangeY = 0.0f;
    int priority = 0;             // toolset field; the retail loader does not read it
};

// A live MDL emitter node (kotor/Particles.cpp).
struct Particle {
    Vec3 position;
    Vec3 velocity;
    float age = 0.0f;
    float life = 1.0f;
    float rotation = 0.0f;
    std::uint32_t seed = 0;
    Vec3 trail;                 // +0x60: where a Motion_Blur particle's tail sits
    float speed2 = 0.0f;        // +0x18: the second velocity's speed (0048d9e0, emitter flag 0x80)
    Vec3 direction2;            // +0x1c
    bool landed = false;        // +0x6c: came to rest on a bounce (00492450)
};

// One point of a Lightning bolt (a retail particle in its list +0x1c8): its frame state as
// updateParticleFrame 0048e460 keeps it, and its Motion_Blur tail.
struct BoltPoint {
    int frame = 0;      // +0x3c
    float stepped = 0.0f; // +0x40
    float shown = 0.0f;   // +0x44
    float age = 0.0f;     // +0x70
    Vec3 trail;           // +0x60
    Vec3 parked;          // its position while it waits in the pool
};

// A Lightning bolt's state (retail PartEmitter +0xf4..+0x28c, update 00498b80): the strip points, the
// per-point offsets RenderLinked 00495b20 adds, and the six control-point/tangent lists (+0x210..+0x24c).
struct LightningBolt {
    bool branch = false;            // +0x28c
    float birthrate = 0.0f;         // +0x58, set from the bolt's length (00494da0 / 00491a50)
    float boltTimer = 10000.0f;     // +0xf4
    float cpTimer = 10000.0f;       // +0xf8
    float targetSize = 0.0f;        // +0xfc
    float sizeScale = 1.0f;         // a branch's sizes are the parent's times this (00491a50)
    Vec3 position;
    Vec3 target;
    Vec3 axis;                      // +0x27c
    Vec3 prevDir;                   // +0x1f8
    Vec3 prevPos;                   // +0x204
    std::size_t attach = 0;         // the parent's +0x264 entry
    std::vector<Vec3> points;
    std::vector<Vec3> offsets;      // +0x270
    std::vector<BoltPoint> state;   // per point, beside points
    std::vector<BoltPoint> pool;    // +0x1d4: points taken off the end, reused first
    std::vector<Vec3> base;         // +0x210
    std::vector<Vec3> oldPoints;    // +0x21c
    std::vector<Vec3> newPoints;    // +0x228
    std::vector<Vec3> baseTangents; // +0x234
    std::vector<Vec3> oldTangents;  // +0x240
    std::vector<Vec3> newTangents;  // +0x24c
};

struct EmitterInstance {
    std::shared_ptr<const Mdl> model;
    std::size_t ordinal = 0;
    std::string node;
    MdlEmitter header;
    std::uint32_t texture = 0;
    bool textureMissing = false;
    // Owner: a scene entity (its mesh animates the node), or a fixed world placement.
    std::uint32_t entity = UINT32_MAX;
    Vec3 origin;
    Mat3 orientation{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
    FxNodeParameters params;
    // A model of emitters alone has no mesh to carry a clip (charrec_light); the owner names it here.
    std::string ownerClip;
    // A minigame model (no scene entity) poses its emitters by its own clip at ownerTime.
    bool ownerDriven = false;
    float ownerTime = 0.0f;
    bool detonatePending = false;
    bool hidden = false; // Its owner is not in the scene right now: nothing new is born.
    std::string sampledClip;
    float sampledTime = -1.0f;
    float lastClipTime = 0.0f;
    float birthDebt = 0.0f;
    bool fired = false;
    bool poseValid = false;
    Vec3 worldPosition;
    Mat3 worldRotation{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
    std::uint32_t random = 1;
    std::vector<Particle> particles;
    // A chunk emitter's mesh: what each particle draws instead of a sprite, one part per texture.
    struct ChunkPart {
        std::uint32_t texture = 0;
        std::vector<MdlMeshTriangle> triangles;
    };
    std::shared_ptr<const std::vector<ChunkPart>> chunk;
    // A Lightning emitter (retail vtable 00743610, update 00498b80): the points of its bolt, redrawn
    // every lightningDelay. A beam (006a0d30) sets where it ends; otherwise its reference child does.
    std::vector<Vec3> bolt;
    bool boltTargetSet = false;
    Vec3 boltTarget;
    bool lightningReady = false;    // +0x1f7: the first frame with a target set the bolt up
    LightningBolt lightning;
    std::vector<LightningBolt> branches; // +0x258
    std::size_t activeBranches = 0;      // +0x288
    // The point-to-point target this frame (+0x1e4): a beam's end, or the reference child (0049b240).
    bool targetValid = false;
    Vec3 targetPosition;
    Vec3 targetZ{0.0f, 0.0f, 1.0f};
    // Last frame's placements for the inherit flags (00492450 / 00493ae0): the emitter's own
    // (+0x170, +0x1b4) and the one +0x158/+0x194 keeps for flags 0x100 and 0x400.
    bool previousValid = false;
    Vec3 previousPosition;
    Mat3 previousRotation{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
    bool inheritValid = false;
    Vec3 inheritPosition;
    Mat3 inheritRotation{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
};

// One MDL light with lens flares, in the place its owner puts it.
struct FlareLight {
    struct Sprite {
        std::uint32_t texture = 0;
        float size = 0.0f;
        float position = 0.0f;
        Vec3 colourShift;
    };
    Vec3 local;              // in the owner's space
    Vec3 origin;             // fixed placement, when there is no owning entity
    Mat3 orientation{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
    std::uint32_t entity = UINT32_MAX;
    Vec3 colour;
    float radius = 0.0f;     // flare radius: beyond this from the camera it does not draw
    bool hidden = false;     // its owner is not in the scene right now
    float visibility = 0.0f; // fades in and out at kFlareFadeRate as the view of it opens or closes
    int room = -1;               // a layout room's light: posed by its scriptloop clip
    std::size_t node = SIZE_MAX; // that light's node in the room model
    std::vector<Sprite> sprites;
};

// Quads rebuilt every frame from Scene::emitters; six vertices per quad, no index buffer.
Vec3 bouncedVelocity(Vec3 velocity, float bounce, float dt);
int particleFrame(int first, int last, float fps, float age, bool random, std::uint32_t seed);

struct ParticleMesh {
    std::vector<SceneVertex> vertices;
    std::vector<TextureBatch> batches; // firstIndex/indexCount count vertices here
};

// Area grass after retail's CAurTriangleBin (004a9630 builds, 004a68b0 sways) and the scene's
// WindManager (004a6640). Blades are quads of four vertices in Scene::vertices: bottom left,
// bottom right, top right, top left.
struct GrassField {
    struct Bin {
        Vec3 center;                 // bin +0x2c: centre of the triangles' bounds (004a67b0)
        float height = 0.0f;         // Grass_QuadSize (grass record +0x24)
        std::uint32_t room = 0;
        std::uint32_t firstVertex = 0;
        std::uint32_t vertexCount = 0;
    };
    std::vector<Bin> bins;
    Vec3 wind{};                     // WindManager +0x18 (CSWCArea 00605260 from ARE WindPower)
    float deviation = 0.0f;          // +0x838, radians
    float deviationSecond = 0.0f;    // +0x83c, radians
    std::array<float, 256> noise{};  // +0x830
    std::array<float, 256> nextNoise{}; // +0x834
    float timer = 0.0f;              // +0x844
    float blend = 0.0f;              // +0x840
    std::uint32_t random = 1u;       // CRT rand state for FillNoiseField 004a57c0
    // Point sources (WindManager +0xc array, +0x10 count; 0x18 bytes each, 004a6510 / 004a64c0).
    struct PointWind {
        Vec3 position;               // +0x0
        float radius = 0.0f;         // +0xc
        float life = 0.0f;           // +0x10, seconds left (004a5e30 drops it below 0)
        float strength = 0.0f;       // +0x14
    };
    std::vector<PointWind> pointWinds;
};

struct Scene {
    std::string module;
    std::string area;
    std::string areaTag;
    bool unescapable = false; // ARE Unescapable (retail 00508c50, area +0x2ac).
    // ARE CameraStyle picks a row of camerastyle.2da (retail 00508c50 reads the field, 005c2540
    // loads the table). Defaults are row 0, DEFAULT. Paired frames: the eye is `distance` behind
    // the creature, `height` above its camerahook node, looking `90 - pitch` degrees below level.
    struct CameraStyle {
        float distance = 3.2f;
        float pitch = 83.0f;
        float height = 0.45f;
        float viewAngle = 55.0f;
        float lookUp = 15.0f;
        float lookDown = 20.0f;
    } cameraStyle;
    // ARE Map struct (the HUD minimap and the map screen): WorldPt1/2 are two world points and
    // MapPt1/2 the same two points as fractions of the map VIEW, 440 x 256 design pixels (retail
    // 00508c50 turns the float fields into pixels with *440 and *256, the sizes of the map panel's
    // LBL_Map; the map object the panel builds, 00694d50, spans 512 x 256). The lbl_map<area>
    // texture is drawn one texel per design pixel from the view's top left, so a point's texel is
    // (fraction x 440, fraction y 256). NorthAxis 1 to 3 turn the world first (00578c60: 1 negates
    // both axes, 2 and 3 swap them with a negation; the affine fit is the same with the swap).
    // ENGINE CHOICE, fitted: the fit was measured on the TPC's stored rows, which run bottom-up:
    // stored row = (1 - fraction y) * 256. The painter flips those rows to top-down before the
    // art is drawn, so in the drawn image the row is fraction y * 256 (project below). The area's
    // objects land on drawn map content for 90 of 90 mapped areas at 99.7 percent with 440 by 256
    // and that orientation; 512 wide gives 77 percent, 380 and 480 wide 86 and 88 percent, so the
    // peak is sharp and is not a compression effect (scratchpad tools modelscan.py). The first
    // version here applied the (1 - y) to the already flipped art, so the arrow stood mirrored top
    // to bottom for every area not symmetric about its middle row; the front end maps gate
    // (--front-end-scenario maps) reads the minimap back from the frame and caught it.
    struct Minimap {
        static constexpr float kViewWidth = 440.0f;
        static constexpr float kViewHeight = 256.0f;
        bool present = false;
        float world1[2] = {0.0f, 0.0f};
        float world2[2] = {0.0f, 0.0f};
        float map1[2] = {0.0f, 0.0f};
        float map2[2] = {0.0f, 0.0f};
        int northAxis = 0;
        int resX = 0;
        float zoom = 1.0f;
        // World x, y to texel coordinates of the map texture (design pixels from its top left).
        // False when the affine is degenerate (the areas with MapResX 0 carry a zero-extent Map).
        bool project(float x, float y, float& px, float& py) const
        {
            if (!present || resX <= 0) {
                return false;
            }
            const bool swap = northAxis >= 2;
            const float a = swap ? y : x; // drives the horizontal axis
            const float b = swap ? x : y; // drives the vertical axis
            const float a1 = swap ? world1[1] : world1[0];
            const float a2 = swap ? world2[1] : world2[0];
            const float b1 = swap ? world1[0] : world1[1];
            const float b2 = swap ? world2[0] : world2[1];
            if (a2 == a1 || b2 == b1) {
                return false;
            }
            const float u = map1[0] + (a - a1) * (map2[0] - map1[0]) / (a2 - a1);
            const float v = map1[1] + (b - b1) * (map2[1] - map1[1]) / (b2 - b1);
            px = u * kViewWidth;
            py = v * kViewHeight;
            return true;
        }
    } minimap;
    std::string installPath;
    Vec3 entry;
    Vec3 entryDirection;
    std::vector<SceneVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<TextureBatch> batches;
    std::vector<Texture> textures;
    std::vector<Vec3> collisionVertices;
    std::vector<CollisionFacet> collision;
    // Per room, the mesh faces a scene ray test meets (Mdl::roomHitTriangles) and their bounds.
    struct RoomHitFaces {
        Vec3 boxMin;
        Vec3 boxMax;
        std::vector<std::array<Vec3, 3>> faces;
    };
    std::vector<RoomHitFaces> roomHitFaces;
    std::vector<Entity> entities;
    std::vector<EntityMesh> entityMeshes;
    std::vector<FlareLight> flareLights;
    std::uint32_t meshRevision = 0; // bumped when mesh geometry was replaced at runtime
    // Across the module's room geometry, sky rooms included: a camera must reach this far to see it.
    float viewExtent = 0.0f;
    std::vector<TriggerVolume> triggers;
    std::vector<EncounterVolume> encounters;
    std::vector<CameraDef> cameras;
    std::vector<LytRoom> rooms;
    AreaAudio areaAudio;
    std::vector<SoundEmitter> soundEmitters;
    std::vector<std::string> warnings;
    std::map<std::string, std::string> scripts;
    // ARE sun/moon block, resolved for the area's IsNight flag.
    Vec3 ambient{0.35f, 0.35f, 0.38f};
    Vec3 diffuse{0.85f, 0.82f, 0.75f};
    Vec3 dynamicAmbient;
    Vec3 fogColor{0.06f, 0.07f, 0.10f};
    float fogNear = 40.0f;
    float fogFar = 160.0f;
    bool fogOn = false;
    float alphaTest = 0.2f;        // ARE AlphaTest. Retail stores it (00508c50) and its alpha func does not read it.
    std::string defaultEnvMap;     // ARE DefaultEnvMap
    std::uint32_t sourceBytes = 0;
    // World-space MDL lights of the rooms, for dynamic objects.
    struct WorldLight {
        Vec3 position;
        Vec3 colour;         // The colour controller, without the multiplier.
        float multiplier = 1.0f;
        float radius = 1.0f;
        int priority = 0;
        int dynamicType = 0;
        bool ambientOnly = false;
        bool shadow = false;              // MdlLight::shadow (light +0x5c)
        float shadowRadius = 0.0f;        // light +0x60
        float verticalDisplacement = 0.0f; // light +0x64
        bool fading = false;              // MdlLight::fading (node +0xa8)
        // A layout room's own light: its room and node, so the room's scriptloop clip moves it
        // (poseRoomScriptLights). -1 / SIZE_MAX otherwise.
        int room = -1;
        std::size_t node = SIZE_MAX;
    };
    std::vector<WorldLight> lights;
    // The object binder's per-object state (004a3f70, see objectSceneLight): bound lights (id, light) and
    // their fades, advanced once per lightFrame by lightFadeStep (3 x game frame time, 0 while paused).
    // lightFades is off outside a game session, where objects take their lights at full strength.
    struct ObjectLightFade {
        std::vector<std::pair<std::size_t, WorldLight>> lights;
        std::vector<float> fades;
        std::uint64_t frame = 0;
    };
    mutable std::unordered_map<std::uint32_t, ObjectLightFade> objectLightFades;
    // The scene gob's shadow slot (its light list +0x1c / +0x20, filled by 004a3f70): the index into
    // lights (SIZE_MAX for none) and its fading intensity, advanced once per lightFrame.
    struct ShadowSlot {
        std::size_t light = SIZE_MAX;
        float intensity = 0.0f;
        std::uint64_t frame = 0;
    };
    mutable ShadowSlot shadowSlot;
    bool lightFades = false;
    float lightFadeStep = 0.0f;
    std::uint64_t lightFrame = 0;
    std::vector<EmitterInstance> emitters;
    ParticleMesh particleMesh;
    GrassField grassField;
    // VIS: visibility[a] lists the rooms drawn while the camera is in room a. The per-frame
    // result is visibleRooms (one flag per room; empty means draw everything).
    std::vector<std::vector<std::uint32_t>> visibility;
    std::vector<std::uint8_t> visibleRooms;
    std::unordered_map<std::string, std::uint32_t> textureIndex; // lower-case name to index
    std::map<std::string, std::uint32_t> missingTextures;        // failed lookups, with uses
    float clock = 0.0f;            // seconds, drives UV animation and procedures
    std::vector<ProceduralTexture> procedurals;
    std::size_t proceduralsScanned = 0; // textures looked at for a procedure
};

// Lower-case texture name to Scene::textures index; a failed lookup returns 0 (the placeholder)
// and is counted in Scene::missingTextures.
std::uint32_t ensureSceneTexture(Scene& scene, Install& install, const std::string& name);
// A texture with no fallback: kNoTexture when absent. Used for lightmaps and environment maps.
std::uint32_t ensureOptionalTexture(Scene& scene, Install& install, const std::string& name);
// Grouping key for triangles that can share one batch; stable for a node across poses.
std::string materialKey(const MdlMeshTriangle& triangle);
// Retail's punch-through alpha reference (data at 00798a90, read by 0047af70 and 0047b000 and
// written by no code; the ARE's AlphaTest, stored by 00508c50, does not reach it).
inline constexpr float kPunchThroughAlpha = 0.35f;
// The batch state for a material, from its node header and the texture's TXI.
// dynamicPart: a part of a placed object (part +0x40 set, 00473900), which always takes the
// mesh's own ambient and diffuse; room parts go through StaticPartNeedsMat 0046f410.
TextureBatch makeMaterialBatch(Scene& scene, Install& install, const MdlMeshTriangle& triangle,
                               bool dynamicPart = false);
// Order of the blended pass: surfaces that hide what is behind them, then those that let it show,
// then additive; by transparency hint within those, then far to near.
bool blendedDrawsBefore(const TextureBatch& a, float distanceA, const TextureBatch& b, float distanceB);
// A room batch in retail's default blend state whose pixels never let what is behind them show:
// drawn with the static geometry, in place, so the room depth is complete before RenderShadows
// (Scene::Render 00451750: RenderStaticGeometry 00450c30, RenderShadows 00451a50, then the dynamic
// geometry 00450b50). Translucent and additive room parts stay in the sorted blended pass.
bool drawsWithStaticGeometry(const TextureBatch& batch);
// The batch state for an emitter's sprites, from its header and the sprite's TXI.
TextureBatch makeEmitterBatch(const Scene& scene, const MdlEmitter& header, std::uint32_t texture);
SceneVertex toSceneVertex(const MdlMeshVertex& vertex, const MdlMeshTriangle& triangle);
SampledLight sampleSceneLight(const Scene& scene, Vec3 position);
// sampleSceneLight through the object binder's per-object fades (004a3f70) when scene.lightFades is on.
SampledLight objectSceneLight(const Scene& scene, std::uint32_t entity, Vec3 position);

// Retail stencil shadows, Scene::RenderShadows 00451a50, drawn after the static geometry
// (Scene::Render: RenderStaticGeometry 00450c30, stencil cleared to 2^(bits-1)-1, RenderShadows,
// then RenderDynamicGeometry 00450b50), so only rooms receive them.
struct ShadowView {
    bool enabled = false; // options Shadows (CClientOptions::SetShadows 0061d6b0 -> 0078e3a8)
    Vec3 focus;           // scene +0xa8 (Scene::SetSceneFocus 00458ba0)
    Vec3 eye;
    Vec3 forward;
    float farClip = 500.0f;
};
struct ShadowFrame {
    // Volume triangles in world space, as 00426660's triangle strips draw them. Each run is one
    // part: drawn once with glFrontFace(GL_CW) and stencil depth-fail GL_INCR, then once with
    // GL_CCW and GL_DECR, colour and depth writes off, depth test on.
    std::vector<Vec3> volume;
    struct Run {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };
    std::vector<Run> runs;
    // Scene::RenderStencilShadowsForThisLight 00450730: a black fan (centre then ten ring points)
    // drawn where the stencil is at least 2^(bits-1), depth test off, SRC_ALPHA / ONE_MINUS_SRC_ALPHA.
    std::array<Vec3, 11> disk{};
    float diskAlpha = 0.0f; // the centre; the ring points have 0
    std::size_t parts = 0;
    std::size_t edges = 0;
    bool active() const { return !runs.empty(); }
};
ShadowFrame buildShadowFrame(const Scene& scene, const ShadowView& view);
// Light of a room mesh without a lightmap: the nearby lights but not the ARE's DynAmbientColor.
SampledLight roomMeshLight(const Scene& scene, Vec3 position, bool blended = false);
// The light a batch's material lets through (both backends draw with this).
SampledLight materialLight(const TextureBatch& batch, const SampledLight& light);
// The environment step of scene.frag and GlScene.cpp, for the checks. `lighting` is the vertex lighting the
// lerp route draws the reflection under (0046ff70, the route without fragment shaders).
Vec3 environmentComposite(Vec3 lit, Vec3 environment, float texelAlpha, bool adds, Vec3 lighting = {1.0f, 1.0f, 1.0f});
// A grey height map turned into a tangent-space normal map, each atlas cell wrapping on itself.
void heightToNormal(Image& image, float scale, int cellsX, int cellsY);
// The reflection program 83025c computes: 2 n (n.e) - e (n.n), with n a tangent-space normal
// moved into the world by the frame the program dots it with.
Vec3 bumpReflection(Vec3 toEye, Vec3 tangent, Vec3 bitangent, Vec3 normal, Vec3 mapNormal);
// The bump map's atlas cell at scene time `clock`.
struct BumpCell {
    int x = 0;
    int y = 0;
};
BumpCell bumpCellAt(const TextureBatch& batch, float clock);

// Animated textures. makeProceduralTexture is retail's init (arturo 00460560, water 0045ff40);
// the steps below are parts of the updates 00462520 and 00461820, exposed for their checks.
ProceduralTexture makeProceduralTexture(std::size_t index, const Texture& texture);
void proceduralArturoWave(ProceduralTexture& state, bool poke);
void proceduralWaterWave(ProceduralTexture& state, float step, bool poke);
void proceduralSmoothHeights(std::vector<std::uint8_t>& heights, int width, int height);
void proceduralDistort(const std::vector<std::uint8_t>& heights, const std::vector<std::uint8_t>& original,
                       int width, int height, bool byAngle, float amplitude, std::vector<std::uint8_t>& out);
bool stepProceduralTexture(ProceduralTexture& state, Texture& texture, float dt);
// One frame: finds new animated textures and advances them all.
void updateProceduralTextures(Scene& scene, float dt);
// A particle's colour, alpha or size at life fraction t (retail 0048f040): the percent values are
// where its keys sit in time, and a percentStart of 255 means there is no middle key.
float emitterCurve(float start, float mid, float end, float percentStart, float percentMid, float percentEnd, float t);
// The colour a batch fogs to (both backends draw with this).
Vec3 batchFogColour(const TextureBatch& batch, Vec3 sceneFog);
// The shader's params.w: 0 lit, 1 unlit (vertex colour), 2 unlit with output alpha 1, 3 force shell.
float batchShadeMode(const TextureBatch& batch);
// The alpha reference a batch draws with from `eye`: grass fades out by distance (retail 004a6d30).
float batchAlphaRef(const TextureBatch& batch, Vec3 eye);
// The self illumination a mesh draws with: none when it has a lightmap.
Vec3 meshEmission(const MdlMeshTriangle& triangle);
// Unpacks a TPC cube map (TXI `cube 1`, six faces stacked) into a latitude-longitude image the
// scene shaders sample with a reflection vector. Returns an empty image when `data` is not one.
Image decodeCubeLatLong(const std::vector<std::uint8_t>& data, const std::string& name);

// Registers `model`'s emitter nodes. The entity's mesh animates them; for entity == UINT32_MAX
// the emitters sit at the fixed placement (origin, orientation).
void addModelEmitters(Scene& scene, Install& install, const std::shared_ptr<const Mdl>& model,
                      std::uint32_t entity, Vec3 origin, const Mat3& orientation);
// One simulation step of every emitter near `focus`.
// Marks an explosion emitter when its owner's clip passed a Detonate event in (from, to]: retail
// 0049d688 registers that event for each Explosion emitter, 0048d770 sets +0x1f6, and 00497640
// bursts once on it. A wrapped clip starts over.
void noteDetonateEvents(EmitterInstance& emitter, const std::string& clip, float from, float to);
void stepParticles(Scene& scene, float dt, Vec3 focus);
// Rebuilds Scene::particleMesh for this camera: call once per frame after the camera is final.
void buildParticleMesh(Scene& scene, Vec3 eye, Vec3 forward, Vec3 up);
// Marks Scene::visibleRooms for a camera at `eye` from the area's VIS lists.
void updateVisibleRooms(Scene& scene, Vec3 eye);
// Resolves a --capture-camera view against the scene; false when the spec names nothing.
// ARE Grass_* fields (read by retail 00508c50).
struct GrassParams {
    std::string texture;
    float density = 0.0f;
    float quadSize = 0.0f;
    float probLL = 0.0f, probLR = 0.0f, probUL = 0.0f, probUR = 0.0f;
    int windPower = 0; // ARE WindPower: 0 none, 1 weak, 2 strong (00605260)
    Vec3 ambient{};    // ARE Grass_Ambient (00508c50 +0xcc; bytes r, g, b / 255 at 00607610)
    Vec3 diffuse{};    // ARE Grass_Diffuse (+0xd0)
};
// Grows grass over each room's AABB (walkmesh) node faces whose surfacemat.2da row sets `grass`, into
// the static batches (retail 00456f30, 00452cc0, 004a7860, 004a9630). `walkmesh` maps a room index to
// Mdl::walkmeshTriangles. Returns the number of quads.
std::size_t addGrass(Scene& scene, Install& install, const GrassParams& params,
                     const std::map<std::uint32_t, std::vector<MdlMeshTriangle>>& walkmesh,
                     const std::vector<char>& surfaceGrass);
// AddPointSourceWind 004a6510: a wind source pushing grass away from `position` for `life` seconds.
void addPointSourceWind(Scene& scene, Vec3 position, float radius, float life, float strength);
// The point sources' push at `point` (004a6440 -> 004a5c80).
Vec3 grassPointWind(const GrassField& field, Vec3 point);
// One frame of retail's grass wind (WindManager 004a5e30, then 004a68b0 for each bin closer than
// 15 m): moves the blade tops in Scene::vertices. Returns the [first, end) vertex range it changed.
std::pair<std::size_t, std::size_t> stepGrassWind(Scene& scene, float frameSeconds, Vec3 eye);
bool captureCameraPose(const Scene& scene, const std::string& spec, Vec3& eye, Vec3& target, float& fov);
// The scene cast 00456b60 (Scene vtable +0x38) with the chase camera's CHitInfo(0, 0xbfffffff, 0), +0x5c set
// and the entity `ignoreEntity` on its ignore list (0063b050): rooms, then objects. Returns 0 for no hit, 1
// when the nearest crossing is a room face, 2 when it is an object's; `hit` is that nearest crossing.
// `area`, when given, holds the room faces (gatherSceneHitFaces) of a box every tested segment lies in: a face outside
// it cannot be crossed, so the answer is the same and the room pass is shorter.
struct SceneHitFaces {
    std::vector<std::array<Vec3, 3>> faces;
};
SceneHitFaces gatherSceneHitFaces(const Scene& scene, Vec3 boxMin, Vec3 boxMax);
int sceneHitCheck(const Scene& scene, Vec3 from, Vec3 to, std::uint32_t ignoreEntity, Vec3& hit,
                  const SceneHitFaces* area = nullptr);

// `savedGit`: the GIT of a retail save's module, used in place of the area's own GIT (retail 0050dd80 reads
// the saved GIT when a game is loaded; a save writes every object whole, with no UseTemplates).
Scene loadModule(Install& install, const std::string& moduleStem,
                 const std::vector<std::uint8_t>* savedGit = nullptr);
// ARE LoadScreenID of the module's first area (loadscreens.2da row); -1 when unreadable.
// Rebinds `install` to the module, so pass a separate Install when one is in use.
std::int32_t readLoadScreenId(Install& install, const std::string& moduleStem);
void refreshEntityMesh(EntityMesh& mesh);

// A mesh whose vertices are already in world space (entity == UINT32_MAX). The batch
// layout and vertex count stay fixed for every later bake; only the renderable nodes
// of `model` are included. Used for objects that are not scene entities.
EntityMesh buildWorldEntityMesh(Scene& scene, Install& install, const std::shared_ptr<Mdl>& model);
void bakeWorldEntityMesh(EntityMesh& mesh, const std::shared_ptr<Mdl>& model,
                         const std::vector<MdlAnimLayer>& layers, const Mat3& rotation, Vec3 translation);
std::shared_ptr<Mdl> loadWorldModel(Install& install, const std::string& modelName);

// Collect non-empty script hook fields from one GFF struct into a hook map.
// Used for ARE/IFO module hooks and GIT instance overrides.
void collectScriptHooks(const Gff& gff, std::uint32_t structure,
                        std::map<std::string, std::string>& scripts);

// Load one UTI item template (type 2025) from the install.
std::optional<ItemTemplate> loadItemTemplate(Install& install, const std::string& resref);
// The model an item shows in the hand: <itemclass>_<variation> (retail 005b30e0), or the base
// item's default model when that does not exist. Empty when the base item has none.
std::string itemHandModel(Install& install, const TwoDa& baseitems, std::int32_t baseItem,
                          std::int32_t modelVariation);
// retail 00613710: what the hands hold, as the class the clip tables are keyed on.
std::int32_t wieldClass(std::int32_t rightWield, std::int32_t leftWield, bool hasRight, bool hasLeft);

// Append one creature from a UTC template at a world position. Builds its mesh
// and hook map; returns the new entity index or -1 on failure. Used by the
// encounter spawner.
std::size_t spawnCreature(Scene& scene, Install& install, const std::string& utcResref,
                          Vec3 position, float bearing);

// The clip a door or placeable rests in, from its open and animation state. Empty when the model
// has none, and the model's idle applies.
struct ObjectClip {
    std::string name;
    float time = 0.0f;
    bool loop = true;
};
ObjectClip objectStateClip(const Entity& entity, const Mdl& model);
// What ANIMATION_PLACEABLE_* asks a placeable to play. Null when `animation` is not one of them.
const char* placeableScriptClip(std::int32_t animation, bool& loop);
// Plays `clip` on the scriptloop mesh of the layout room named `room` (case-insensitive), from
// the start and looping, as retail 00540dd0 / 00646180 do for PlayRoomAnimation. An empty clip
// returns it to the rest pose. Returns false when the room has no scriptloop geometry.
bool setRoomClip(Scene& scene, const std::string& room, const std::string& clip);
// Moves the room's own lights and light flares to their nodes' places under the room's scriptloop
// clip at its time. Paired RETAIL row 33/34 (m12ab): the sun's light position (+0x94) is
// AuroraLight02 posed by scriptloop04 (its position key adds 403, -298, 0.7 to the bind place) and
// turned by spacedummy's rock, the same frame the room meshes are drawn in.
void poseRoomScriptLights(Scene& scene, const EntityMesh& mesh);
// Adds a room's scriptloop group as one dynamic mesh (see Mdl::markRoomScriptMeshes).
void addRoomScriptMesh(Scene& scene, Install& install, const std::shared_ptr<Mdl>& model, Vec3 origin, int room);
// Adds a room's loop-animated meshes (see Mdl::markRoomLoopMeshes) as one dynamic mesh per clip.
void addRoomLoopMesh(Scene& scene, Install& install, const std::shared_ptr<Mdl>& model,
                     const std::string& clip, Vec3 origin, int room);
// Adds a skinned model shown by a reference node of a room's model.
void addSkinnedReferenceMesh(Scene& scene, Install& install, const std::shared_ptr<const Mdl>& model,
                             const std::shared_ptr<const Mdl>& holder, std::size_t node,
                             const std::string& holderClip, Vec3 origin, int room);
// Replaces the hand meshes of a creature with its current weaponModel and offhandModel.
void rebuildEntityWeapons(Scene& scene, Install& install, std::size_t entityIndex);

// Create the player's rendered avatar (body + head) without a UTC template.
// The avatar is a normal scene creature tagged `PC`, so scripts and the
// renderer see the same entity the player controls.
// The variant an item template (UTI) gives; zero when it has none or cannot be read.
BodyVariant armorBodyVariant(Install& install, const std::string& itemResref);

std::size_t spawnPlayerAvatar(Scene& scene, Install& install, std::uint32_t appearance,
                              const std::string& name, Vec3 position, float bearing,
                              BodyVariant body = {});

// One non-creature entity showing the model `modelName` at a position (a GUI scene prop).
// Returns the entity index or -1 when the model cannot be loaded.
// A cutscene stand-in: `body` and `head` posed by the clips of `clips`, placed by those clips.
std::size_t spawnStuntBody(Scene& scene, Install& install, const std::string& body,
                           const std::shared_ptr<Mdl>& head, const std::string& headNode,
                           const Mdl& clips);
std::size_t spawnModelEntity(Scene& scene, Install& install, const std::string& modelName,
                             Vec3 position, float bearing);

// Resolves GIT templates, model geometry and trigger polygons after the base
// module has been loaded. Kept separate from loadModule so the room pass stays
// independent of template/model failures.
void loadEntityAssets(Scene& scene, Install& install, const Gff& git);

// A creature's hit points as retail loads them: CSWSCreatureStats::ReadStatsFromGff 005afce0 (the HitPoints,
// CurrentHitPoints and FeatList block, non-PC branch) with CSWSCreature::GetMaxHitPoints 004ed310.
// `HitPoints` is the base, the Constitution modifier and the Toughness feats add per level, and the
// loaded current value takes the same per-level share. Reads HitPoints, CurrentHitPoints, Con,
// ClassList and FeatList of a UTC or of a saved creature struct.
struct CreatureHitPoints {
    std::int32_t current = 1;
    std::int32_t max = 1;
};
CreatureHitPoints creatureHitPointsFromGff(const Gff& gff, std::uint32_t structure);


} // namespace ke::kotor
