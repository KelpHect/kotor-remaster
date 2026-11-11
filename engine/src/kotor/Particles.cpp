// MDL emitter nodes. Every value comes from the node's header or its controllers; nothing here
// draws without shipped data. Retail's update loop has not been traced, so the integration
// (below) follows the controller names: birth rate per second, life in seconds, velocity and
// sizes in engine units, spread as a cone half angle about the node's Z axis.

#include "kotor/Formats.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace ke::kotor {

namespace {

// Controller ids (emitter family).
constexpr std::int32_t kAlphaEnd = 80, kAlphaStart = 84, kBirthRate = 88, kBounceCo = 92, kDrag = 100,
                       kFps = 104, kFrameEnd = 108, kFrameStart = 112, kGravity = 116, kLife = 120,
                       kMass = 124, kParticleRot = 136, kRandVel = 140, kSizeStart = 144,
                       kSizeEnd = 148, kSizeStartY = 152, kSizeEndY = 156, kSpread = 160,
                       kThreshold = 164, kVelocity = 168, kXSize = 172, kYSize = 176,
                       kAlphaMid = 216, kPercentStart = 220, kPercentMid = 224, kPercentEnd = 228,
                       kSizeMid = 232, kSizeMidY = 236, kColorMid = 284, kColorEnd = 380,
                       kColorStart = 392, kDetonate = 502, kLightningDelay = 184, kLightningRadius = 188,
                       kLightningScale = 192, kLightningSubDiv = 196, kCombineTime = 96, kP2PBezier2 = 128,
                       kP2PBezier3 = 132, kLightningZigzag = 200, kTargetSize = 252, kNumControlPoints = 256,
                       kControlPointRadius = 260, kControlPointDelay = 264, kTangentSpread = 268,
                       kTangentLength = 272, kBlurLength = 180;

constexpr std::uint32_t kFlagBounce = 0x10u;
constexpr std::uint32_t kFlagRandom = 0x20u;

constexpr float kNoMiddleKey = 255.0f;
constexpr std::size_t kMaxParticlesPerEmitter = 512;
constexpr std::int32_t kRandomBirthRate = 240; // instance +0xf0 (00496e40)

float nextRandom(std::uint32_t& state)
{
    // xorshift32: reproducible captures from one seed per emitter.
    state ^= state << 13u;
    state ^= state >> 17u;
    state ^= state << 5u;
    return static_cast<float>(state & 0xFFFFFFu) / 16777216.0f;
}

Vec3 mulRotation(const Mat3& matrix, Vec3 value)
{
    return {matrix.m[0][0] * value.x + matrix.m[0][1] * value.y + matrix.m[0][2] * value.z,
            matrix.m[1][0] * value.x + matrix.m[1][1] * value.y + matrix.m[1][2] * value.z,
            matrix.m[2][0] * value.x + matrix.m[2][1] * value.y + matrix.m[2][2] * value.z};
}

Vec3 column(const Mat3& m, int c)
{
    return {m.m[0][c], m.m[1][c], m.m[2][c]};
}

std::string lowerName(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

const EntityMesh* ownerMesh(const Scene& scene, std::uint32_t entity)
{
    for (const EntityMesh& mesh : scene.entityMeshes) {
        if (mesh.entity == entity && !mesh.attachModel) {
            return &mesh;
        }
    }
    return nullptr;
}

// Refreshes the node's model-space pose and controller values when the owner's clip time moved.
void updatePlacement(const Scene& scene, EmitterInstance& emitter)
{
    std::string clip;
    float time = 0.0f;
    if (emitter.entity != UINT32_MAX) {
        if (const EntityMesh* mesh = ownerMesh(scene, emitter.entity)) {
            clip = mesh->animation;
            time = mesh->animationTime;
        } else {
            clip = emitter.ownerClip;
        }
    } else if (emitter.ownerDriven) {
        clip = emitter.ownerClip;
        time = emitter.ownerTime;
    } else {
        // Room emitters follow the scriptloop clip a script started on the room (PlayRoomAnimation;
        // RETAIL row 34, m12ab scriptloop04 moves AuroraLight02 and its OmenEmitter02 sun core),
        // else the room's looping clip, when it has one.
        for (const EntityMesh& mesh : scene.entityMeshes) {
            if (mesh.scriptLoop && !mesh.animation.empty() && mesh.model.get() == emitter.model.get()) {
                clip = mesh.animation;
                time = mesh.animationTime;
                break;
            }
        }
        for (const char* name : {"animloop1", "animloop2", "animloop3"}) {
            if (!clip.empty()) {
                break;
            }
            const float duration = emitter.model->animationDuration(name);
            if (duration > 0.0f) {
                clip = name;
                time = std::fmod(scene.clock, duration);
                break;
            }
        }
    }
    if (emitter.poseValid && clip == emitter.sampledClip && time == emitter.sampledTime) {
        return;
    }
    noteDetonateEvents(emitter, clip, emitter.poseValid && clip == emitter.sampledClip ? emitter.lastClipTime : -1.0f, time);
    if (const std::optional<Mdl::Pose> pose = emitter.model->nodePose(emitter.node, clip, time)) {
        emitter.worldRotation = pose->rotation;
        emitter.worldPosition = pose->translation;
    }
    emitter.params = emitter.model->fxNodeParameters(emitter.ordinal, clip, time);
    // A clip that wrapped re-arms an explosion for the next detonate key.
    if (clip == emitter.sampledClip && time < emitter.lastClipTime) {
        emitter.fired = false;
    }
    emitter.lastClipTime = time;
    emitter.sampledClip = clip;
    emitter.sampledTime = time;
    emitter.poseValid = true;
}

struct Placement {
    Vec3 position;
    Mat3 rotation;
};

// Where the emitter's model sits: its owner entity, or the fixed placement it was given.
Placement ownerPlacement(const Scene& scene, const EmitterInstance& emitter)
{
    Placement owner{emitter.origin, emitter.orientation};
    if (emitter.entity != UINT32_MAX && emitter.entity < scene.entities.size()) {
        const Entity& entity = scene.entities[emitter.entity];
        owner.position = entity.position;
        const float c = std::cos(entity.drawBearing());
        const float s = std::sin(entity.drawBearing());
        owner.rotation = Mat3{{{c, -s, 0.0f}, {s, c, 0.0f}, {0.0f, 0.0f, 1.0f}}};
    }
    return owner;
}

Placement worldPlacement(const Scene& scene, const EmitterInstance& emitter)
{
    const Placement owner = ownerPlacement(scene, emitter);
    const Vec3 origin = owner.position;
    const Mat3& orientation = owner.rotation;
    Placement out;
    out.position = origin + mulRotation(orientation, emitter.worldPosition);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out.rotation.m[r][c] = 0.0f;
            for (int k = 0; k < 3; ++k) {
                out.rotation.m[r][c] += orientation.m[r][k] * emitter.worldRotation.m[k][c];
            }
        }
    }
    return out;
}

constexpr float kFlareFadeRate = 3.0f; // 0078e3e8, per second
constexpr float kFlareScale = 0.1f;    // 0078e3e0
constexpr float kFlarePlane = 2.0f;    // 00457ec0 draws in view space on the plane z = -2

Vec3 flareWorldPosition(const Scene& scene, const FlareLight& flare)
{
    if (flare.entity != UINT32_MAX && flare.entity < scene.entities.size()) {
        const Entity& entity = scene.entities[flare.entity];
        const float c = std::cos(entity.drawBearing());
        const float s = std::sin(entity.drawBearing());
        return entity.position + Vec3{c * flare.local.x - s * flare.local.y, s * flare.local.x + c * flare.local.y, flare.local.z};
    }
    return flare.origin + mulRotation(flare.orientation, flare.local);
}

// Whether a walkmesh face that stops sight lies on the segment. Inferred: retail's cast goes through
// the area's own query (a virtual call in 00457ec0), which has not been traced to its geometry.
bool segmentBlocked(const Scene& scene, Vec3 from, Vec3 to)
{
    const Vec3 ray = to - from;
    for (const CollisionFacet& facet : scene.collision) {
        if (!facet.blocksSight) {
            continue;
        }
        const Vec3& a = scene.collisionVertices[facet.a];
        const Vec3 e1 = scene.collisionVertices[facet.b] - a;
        const Vec3 e2 = scene.collisionVertices[facet.c] - a;
        const Vec3 p = cross(ray, e2);
        const float det = dot(e1, p);
        if (std::abs(det) < 1e-7f) {
            continue;
        }
        const Vec3 t = from - a;
        const float u = dot(t, p) / det;
        const Vec3 q = cross(t, e1);
        const float v = dot(ray, q) / det;
        const float hit = dot(e2, q) / det;
        if (u >= 0.0f && v >= 0.0f && u + v <= 1.0f && hit > 0.0f && hit < 1.0f) {
            return true;
        }
    }
    return false;
}

// Whether a shown world-placed model (minigame ships, guns, hulls: entity meshes with no scene entity,
// posed in world space) lies on the segment `from` -> `to`. Scene::ObjectHitCheck 0044fe00 with the
// flare's CHitInfo(0, 0xbfffffff, 0) (004578f0: +0x5c, +0x5e, +0x60 clear, +0x5d set): a gob is
// skipped when its model's classification has bit 0, 1 or 7 (0x80) set, then Gob::HitCheck 0043eb80
// (gob fade +0xd8 below 0.25 skips it; minigame gobs are opaque) walks its parts (004429d0); a
// mesh part (00440ff0) tests each face with 004ab750, which is one-sided: `from` must be on the
// face's front (plane distance >= 0) and `to` on its back (<= 0), the crossing inside every edge
// within 1e-5 (007440f8). Skinned parts (vfunc +0x50 non-zero, 00641db0) and AABB nodes (0x821)
// are not tested; the node's render flag is not read.
bool segmentHitsPlacedModel(const Scene& scene, Vec3 from, Vec3 to)
{
    const Vec3 ray = to - from;
    const auto hits = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
        const Vec3 normal = normalize(cross(b - a, c - a));
        if (dot(normal, normal) < 0.5f) {
            return false;
        }
        const float fromSide = dot(normal, from - a);
        const float toSide = dot(normal, to - a);
        if (!(fromSide >= 0.0f && toSide <= 0.0f && fromSide != toSide)) {
            return false;
        }
        const Vec3 at = from + ray * (fromSide / (fromSide - toSide));
        const Vec3 corners[3] = {a, b, c};
        for (int k = 0; k < 3; ++k) {
            const Vec3& p0 = corners[k];
            const Vec3& p1 = corners[(k + 1) % 3];
            if (dot(cross(p1 - p0, at - p0), normal) < -1.0e-5f) {
                return false;
            }
        }
        return true;
    };
    for (const EntityMesh& mesh : scene.entityMeshes) {
        if (!mesh.visible || mesh.entity != UINT32_MAX || mesh.roomAnimated || mesh.fx) {
            continue;
        }
        if (mesh.model && (mesh.model->classification() & 0x83u) != 0u) {
            continue;
        }
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            if (hits(mesh.vertices[mesh.indices[i]].position, mesh.vertices[mesh.indices[i + 1]].position,
                     mesh.vertices[mesh.indices[i + 2]].position)) {
                return true;
            }
        }
        for (const std::array<Vec3, 3>& face : mesh.hiddenFaces) {
            if (hits(face[0], face[1], face[2])) {
                return true;
            }
        }
    }
    return false;
}

int crtRand(std::uint32_t& state);

void spawn(EmitterInstance& emitter, const Placement& place, float dt)
{
    if (emitter.particles.size() >= kMaxParticlesPerEmitter) {
        return;
    }
    const FxNodeParameters& p = emitter.params;
    Particle particle;
    // Retail 00494b30 keeps a particle forever when the emitter's life is below 0 (6,501 Single
    // emitters ship life -1: crowds, bird flocks, star-map points), so a negative life is kept.
    const float life = p.scalar(kLife, 1.0f);
    particle.life = life < 0.0f ? life : std::max(life, 0.01f);
    particle.seed = emitter.random;
    // Particle init 004921d0, rand() calls in retail's order (RETAIL row 42, smoke_births.json: 10 per
    // birth on smoke01: 2 speed, 6 place, 2 angles). ftol truncates toward zero.
    // Speed: when randvel > 0.01 (double 00743310), k = rand % (int)(randvel x 100), negated when the next
    // rand is odd; speed = velocity + 0.01 k.
    int speedSteps = 0;
    const float randVel = p.scalar(kRandVel, 0.0f);
    if (randVel > 0.01f) {
        const int range = static_cast<int>(randVel * 100.0f);
        speedSteps = range > 0 ? crtRand(emitter.random) % range : 0;
        if ((crtRand(emitter.random) & 1) != 0) {
            speedSteps = -speedSteps;
        }
    }
    const float speed = p.scalar(kVelocity, 0.0f) + static_cast<float>(speedSteps) * 0.01f;
    // Place 0048d870: each axis (int)(size x 50) wide (the size is the whole width in cm, the offset
    // runs half of it either way), offset (rand x rand) % width, then one sign rand per axis, times
    // 0.0001 along the emitter's X and Y. Skipped when both widths are 0. RETAIL row 57: 150 of 150
    // smoke01 births (xsize 30, ysize 58) land within 3e-6 of this with widths 1500 and 2900.
    const int widthX = static_cast<int>(p.scalar(kXSize, 0.0f) * 50.0f);
    const int widthY = static_cast<int>(p.scalar(kYSize, 0.0f) * 50.0f);
    Vec3 birthLocal{};
    if (widthX != 0 || widthY != 0) {
        int offX = 0;
        int offY = 0;
        if (widthX >= 1) {
            const int a = crtRand(emitter.random);
            offX = (a * crtRand(emitter.random)) % widthX;
        }
        if (widthY >= 1) {
            const int a = crtRand(emitter.random);
            offY = (a * crtRand(emitter.random)) % widthY;
        }
        if ((crtRand(emitter.random) & 1) != 0) {
            offX = -offX;
        }
        if ((crtRand(emitter.random) & 1) != 0) {
            offY = -offY;
        }
        birthLocal = {static_cast<float>(offX) * 0.0001f, static_cast<float>(offY) * 0.0001f, 0.0f};
    }
    particle.position = place.position + mulRotation(place.rotation, birthLocal);
    // Direction 0048de60 (spread != 0): turn the emitter's frame about its X axis by
    // 0.01 x (rand % (1 - (int)(spread x 0.5 x -100))), then about its Z axis by 0.01 x (rand % 628);
    // the particle flies along the turned Z.
    const float spread = p.scalar(kSpread, 0.0f);
    Vec3 directionLocal{0.0f, 0.0f, 1.0f};
    if (spread != 0.0f) {
        const int polarRange = 1 - static_cast<int>(spread * 0.5f * -100.0f);
        const int polarSteps = crtRand(emitter.random) % std::max(polarRange, 1);
        const float polar = static_cast<float>(polarSteps) * 0.01f;
        const float azimuth = static_cast<float>(crtRand(emitter.random) % 628) * 0.01f;
        // R_z(azimuth) R_x(polar) applied to +Z.
        directionLocal = {std::sin(polar) * std::sin(azimuth), -std::sin(polar) * std::cos(azimuth), std::cos(polar)};
    }
    particle.velocity = mulRotation(place.rotation, directionLocal) * speed;
    particle.rotation = nextRandom(emitter.random) * 6.2831853f;
    particle.trail = particle.position; // 004921d0: the tail starts at the particle
    if ((emitter.header.flags & 0x80u) != 0u && emitter.previousValid) {
        // 0048d9e0 (flag 0x80): a second velocity from how far the emitter moved since the last
        // frame, plus how its Z axis swung times the particle's distance from it, both per second.
        const float perSecond = dt > 0.0f ? 1.0f / dt : 0.01f;
        const float reach = length(place.position - particle.position);
        const Vec3 swing = column(place.rotation, 2) - column(emitter.previousRotation, 2);
        const Vec3 v2 = (place.position - emitter.previousPosition) * perSecond + swing * (reach * perSecond);
        particle.speed2 = length(v2);
        particle.direction2 = particle.speed2 == 0.0f ? Vec3{} : v2 * (1.0f / particle.speed2);
    }
    emitter.particles.push_back(particle);
}

// The C runtime's rand() range (0..32767), drawn from the emitter's own seeded generator so captures
// repeat. Retail calls rand() (006fc447) at each place below; the distributions are the same.
int crtRand(std::uint32_t& state)
{
    nextRandom(state);
    return static_cast<int>((state >> 9u) & 0x7FFFu);
}

int randMod(std::uint32_t& state, int modulus)
{
    const int r = crtRand(state);
    return modulus > 0 ? r % modulus : 0;
}

// _ftol: truncation toward zero.
int ftol(float value)
{
    return static_cast<int>(value);
}

constexpr float kDegToRad = 0.017453292f; // 007415f8

// Vector::Normalize: a zero vector becomes (1,0,0).
Vec3 unitOr(Vec3 value)
{
    const float len = length(value);
    return len > 0.0f ? value * (1.0f / len) : Vec3{1.0f, 0.0f, 0.0f};
}

struct Quat {
    float w = 1.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// 004ac960: (cos(a/2), sin(a/2) * the unit axis).
Quat axisAngle(Vec3 axis, float radians)
{
    const Vec3 n = unitOr(axis);
    const float s = std::sin(radians * 0.5f);
    return {std::cos(radians * 0.5f), n.x * s, n.y * s, n.z * s};
}

// 004a9ac0
Quat mul(const Quat& a, const Quat& b)
{
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

// 004a9bb0
Vec3 rotate(const Quat& q, Vec3 v)
{
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

// 004acac0(out, z, x, y), degrees: Qz * Qx * Qy.
Quat yawPitchRoll(float zDeg, float xDeg, float yDeg)
{
    return mul(mul(axisAngle({0.0f, 0.0f, 1.0f}, zDeg * kDegToRad), axisAngle({1.0f, 0.0f, 0.0f}, xDeg * kDegToRad)),
               axisAngle({0.0f, 1.0f, 0.0f}, yDeg * kDegToRad));
}

// 004ab630: the shortest arc from one direction to another; opposite directions turn about X.
Quat rotationArc(Vec3 from, Vec3 to)
{
    const Vec3 a = unitOr(from);
    const Vec3 b = unitOr(to);
    const Vec3 c = cross(a, b);
    const float d = dot(a, b);
    if (d <= -1.0f) {
        return {0.0f, 1.0f, 0.0f, 0.0f};
    }
    const float s = std::sqrt(2.0f + 2.0f * d);
    return {s * 0.5f, c.x / s, c.y / s, c.z / s};
}

Vec3 bezier(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float t)
{
    const float u = 1.0f - t;
    return p0 * (u * u * u) + p1 * (3.0f * u * u * t) + p2 * (3.0f * u * t * t) + p3 * (t * t * t);
}

Vec3 bezierSlope(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float t)
{
    const float u = 1.0f - t;
    return (p1 - p0) * (3.0f * u * u) + (p2 - p1) * (6.0f * u * t) + (p3 - p2) * (3.0f * t * t);
}

// 004ab360: the point of segment [a, b] closest to p.
Vec3 segmentProject(Vec3 a, Vec3 b, Vec3 p)
{
    const Vec3 ab = b - a;
    const float len2 = dot(ab, ab);
    if (len2 <= 0.0f) {
        return a;
    }
    return a + ab * std::clamp(dot(p - a, ab) / len2, 0.0f, 1.0f);
}

// The target-size jitter of 00493ae0 / 00492ea0 / 00498b80: per axis r % ftol(size * 50), negated
// when r is even, in hundredths.
Vec3 targetJitter(float targetSize, std::uint32_t& state)
{
    const int modulus = ftol(targetSize * 50.0f);
    float out[3] = {};
    for (float& axis : out) {
        const int r = crtRand(state);
        int v = modulus > 0 ? r % modulus : 0;
        if ((r & 1) == 0) {
            v = -v;
        }
        axis = static_cast<float>(v) * 0.01f;
    }
    return {out[0], out[1], out[2]};
}

// The emitter's lightning controllers (PartEmitter +0xb8..+0xc8, +0x100..+0x110).
struct BoltParams {
    float subDiv = 0.0f;
    float numControlPoints = 0.0f;
    float controlPointRadius = 0.0f;
    float controlPointDelay = 0.0f;
    float tangentSpread = 0.0f;
    float tangentLength = 0.0f;
    float delay = 0.0f;
    float radius = 0.0f;
    float scale = 0.0f;
    float zigzag = 0.0f;
    float targetSize = 0.0f;
    std::size_t branchCount = 0;
    bool smoothing = false;
    // What the points' frames and tails need (00498b80 head, 0048e460).
    float frameStart = 0.0f;
    float frameEnd = 0.0f;
    float fps = 0.0f;
    float life = 0.0f;
    float blurLength = 0.0f;
    std::uint32_t flags = 0;
    bool motionBlur = false;
};

// The three Z axes a bolt bends its curve with: the node's local Z (+0x14), its world Z (+0x1a4),
// and the target's Z (+0x68 of the target). A branch has none of these: all three are +Z.
struct BoltAxes {
    Vec3 local{0.0f, 0.0f, 1.0f};
    Vec3 world{0.0f, 0.0f, 1.0f};
    Vec3 target{0.0f, 0.0f, 1.0f};
};

void resizeLists(LightningBolt& b, std::size_t n)
{
    for (std::vector<Vec3>* list : {&b.base, &b.oldPoints, &b.newPoints, &b.baseTangents, &b.oldTangents, &b.newTangents}) {
        list->resize(n);
    }
}

// Places bolt points [from, to) on the curve through the control points, morphed from the old set
// (u = 0) to the new (u = 1): each segment is a cubic from Q[k-1] along T[k-1] to Q[k] against T[k]
// (00498b80 / 00497c60). Retail walks the segments with step 1/(m-1); the bound check is ours.
void placeOnCurve(LightningBolt& b, float u, std::size_t from, std::size_t to)
{
    const std::size_t m = b.oldPoints.size();
    const std::size_t count = b.points.size();
    if (m < 2u || count < 2u) {
        return;
    }
    const float step = 1.0f / static_cast<float>(m - 1u);
    for (std::size_t j = from; j < to && j < count; ++j) {
        const float f = static_cast<float>(j) / static_cast<float>(count - 1u);
        std::size_t k = std::clamp<std::size_t>(static_cast<std::size_t>(f / step) + 1u, 1u, m - 1u);
        const float lo = static_cast<float>(k - 1u) * step;
        const float v = std::clamp((f - lo) / step, 0.0f, 1.0f);
        const Vec3 q0 = b.oldPoints[k - 1u] + (b.newPoints[k - 1u] - b.oldPoints[k - 1u]) * u;
        const Vec3 q1 = b.oldPoints[k] + (b.newPoints[k] - b.oldPoints[k]) * u;
        const Vec3 t0 = b.oldTangents[k - 1u] + (b.newTangents[k - 1u] - b.oldTangents[k - 1u]) * u;
        const Vec3 t1 = b.oldTangents[k] + (b.newTangents[k] - b.oldTangents[k]) * u;
        b.points[j] = bezier(q0, q0 + t0, q1 - t1, q1, v);
    }
}

// 00497c60 (Lightning vtable +0x88): re-fits the base control points to the bolt's length, then
// scatters a new displaced set about them, keeping the last set as the one to morph from.
void updateControlPoints(LightningBolt& b, const BoltParams& bp, const BoltAxes& axes, std::uint32_t& rng)
{
    const int radius = ftol(bp.controlPointRadius * 100.0f);
    const int spread = ftol(bp.tangentSpread * 100.0f);
    const float len = bp.tangentLength;
    const Vec3 span = b.target - b.position;
    const std::size_t n = static_cast<std::size_t>(std::max(ftol(length(span) * bp.numControlPoints + 0.5f), 0)) + 2u;
    const std::size_t old = b.base.size();
    if (n != old && old >= 1u) {
        const Vec3 last = b.base.back();
        resizeLists(b, n);
        b.base[n - 1u] = last;
        b.baseTangents[0] = axes.local * len;
        b.baseTangents[n - 1u] = unitOr(span) * len;
        const Vec3 a0 = b.base[0];
        const Vec3 a1 = b.base[n - 1u];
        for (std::size_t i = 1; i + 1u < n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n - 1u);
            b.base[i] = bezier(a0, a0 + axes.local * len, a1 + axes.target * len, a1, t);
            b.baseTangents[i] = unitOr(bezierSlope(a0, a0 + axes.local * len, a1 + axes.target * len, a1, t)) * len;
        }
    }
    for (std::size_t i = 0; i < b.base.size(); ++i) {
        b.oldPoints[i] = b.newPoints[i];
        b.oldTangents[i] = b.newTangents[i];
        if (i == 0u || i + 1u == b.base.size()) {
            b.newPoints[i] = b.base[i];
            b.newTangents[i] = i == 0u ? axes.world * len : unitOr(span) * len;
        } else {
            if (radius != 0) {
                float r = static_cast<float>(randMod(rng, radius)) * 0.01f;
                if ((crtRand(rng) & 1) != 0) {
                    r = -r;
                }
                const float turn = static_cast<float>(randMod(rng, 360));
                b.newPoints[i] = b.base[i] + rotate(yawPitchRoll(0.0f, turn, 0.0f), b.axis * r);
            } else {
                b.newPoints[i] = b.base[i];
            }
            if (spread != 0) {
                float f1 = static_cast<float>(randMod(rng, spread)) * 0.01f;
                if ((crtRand(rng) & 1) != 0) {
                    f1 = -f1;
                }
                float f2 = static_cast<float>(randMod(rng, spread)) * 0.01f;
                if ((crtRand(rng) & 1) != 0) {
                    f2 = -f2;
                }
                b.newTangents[i] = rotate(yawPitchRoll(f1, f2, 0.0f), b.baseTangents[i]);
            } else {
                b.newTangents[i] = b.baseTangents[i];
            }
        }
        if (b.branch) {
            b.oldPoints[i] = b.newPoints[i];
        }
    }
    if (b.points.size() >= 3u) {
        placeOnCurve(b, 0.0f, 1u, b.points.size() - 1u);
    }
}

// 00491a50 (Lightning vtable +0x8c): picks how many branches show and where each leaves the bolt.
void updateBranches(EmitterInstance& emitter, const BoltParams& bp, std::uint32_t& rng)
{
    LightningBolt& main = emitter.lightning;
    const std::size_t count = main.points.size();
    emitter.activeBranches = static_cast<std::size_t>(randMod(rng, static_cast<int>(bp.branchCount) + 1));
    emitter.activeBranches = std::min(emitter.activeBranches, emitter.branches.size());
    if (count == 0u) {
        emitter.activeBranches = 0;
        return;
    }
    for (std::size_t i = 0; i < emitter.activeBranches; ++i) {
        LightningBolt& br = emitter.branches[i];
        const float s = static_cast<float>(randMod(rng, 80)) * 0.01f + 0.1f;
        br.sizeScale = s;
        br.attach = std::min(static_cast<std::size_t>(std::max(ftol(static_cast<float>(count) * s), 0)), count - 1u);
        const Vec3 from = main.points[br.attach];
        br.base[0] = from;
        br.position = from;
        const float y = static_cast<float>(randMod(rng, 360));
        const float z = -static_cast<float>(randMod(rng, 45));
        br.baseTangents[0] = rotate(yawPitchRoll(z, 0.0f, y), main.axis * bp.tangentLength);
        if (s <= 0.5f) {
            const Vec3 d = main.points.back() - main.points.front();
            const float reach = (static_cast<float>(randMod(rng, 75)) * 0.01f + 0.25f) * length(d) * 0.5f;
            if (reach == 0.0f) {
                br.birthrate = 0.0f;
                br.target = from;
            } else {
                const float turn = static_cast<float>(randMod(rng, 360));
                const Vec3 w = rotate(yawPitchRoll(0.0f, turn, 0.0f), main.axis * reach);
                br.target = br.base[0] + w + d * (static_cast<float>(randMod(rng, 25)) * 0.01f + 0.25f);
                br.birthrate = length(br.target - from) * bp.subDiv + 2.0f;
                br.baseTangents[1] = br.baseTangents[0];
            }
            br.targetSize = 0.0f;
            br.base[1] = br.target;
            const Vec3 n = unitOr(br.base[1] - br.base[0]);
            br.axis = {-n.y, n.z, -n.x};
        } else {
            br.target = main.target;
            br.base[1] = main.target;
            br.targetSize = bp.targetSize;
            br.birthrate = length(main.target - from) * bp.subDiv + 2.0f;
            br.baseTangents[1] = main.baseTangents.back();
        }
        br.prevDir = br.target - br.position;
        br.prevPos = br.position;
    }
}

// 00494da0, first frame with a target: the bolt's point count, control points and tangents.
// Then 0049d480 (Initialize) arms both timers and 0049ce30 makes each branch.
void initLightning(EmitterInstance& emitter, const BoltParams& bp, const BoltAxes& axes, Vec3 from, Vec3 to)
{
    LightningBolt& b = emitter.lightning;
    b = LightningBolt{};
    const float dist = length(to - from);
    b.birthrate = bp.subDiv * dist + 2.0f;
    const int n = std::max(ftol(bp.numControlPoints * dist + 0.5f), 0);
    const std::size_t m = static_cast<std::size_t>(n) + 2u;
    resizeLists(b, m);
    const float len = bp.tangentLength;
    b.base[0] = b.newPoints[0] = from;
    b.base[m - 1u] = b.newPoints[m - 1u] = to;
    b.baseTangents[0] = b.newTangents[0] = axes.local * len;
    b.baseTangents[m - 1u] = b.newTangents[m - 1u] = unitOr(to - from) * len;
    for (int i = 1; i <= n; ++i) {
        const float t = std::min(static_cast<float>(i) / static_cast<float>(n + 1), 1.0f);
        const Vec3 p1 = from + axes.local * len;
        const Vec3 p2 = to + axes.target * len;
        b.base[i] = b.newPoints[i] = bezier(from, p1, p2, to, t);
        b.baseTangents[i] = b.newTangents[i] = unitOr(bezierSlope(from, p1, p2, to, t)) * len;
    }
    const Vec3 u = unitOr(to - from);
    b.axis = {-u.y, u.z, -u.x};
    b.prevDir = to - from;
    b.prevPos = from;
    b.position = from;
    b.target = to;
    emitter.branches.assign(bp.branchCount, LightningBolt{});
    for (LightningBolt& br : emitter.branches) {
        br.branch = true;
        resizeLists(br, 2u);
        br.cpTimer = bp.controlPointDelay;
        br.boltTimer = bp.delay;
        br.targetSize = 0.0f;
    }
    emitter.activeBranches = 0;
}

// 00498b80 (Lightning vtable +0x80), for the main bolt or one of its branches.
void updateBolt(EmitterInstance& emitter, LightningBolt& b, const BoltParams& bp, const BoltAxes& axes, float dt)
{
    std::uint32_t& rng = emitter.random;
    b.boltTimer += dt;
    const std::size_t count = static_cast<std::size_t>(std::max(ftol(b.birthrate), 0));
    if (count == 0u) {
        b.points.clear();
        b.state.clear();
        return;
    }
    const Vec3 from = b.position;
    const Vec3 to = b.target;
    // The point list grows at its end from the pool of points it dropped (+0x1d4), or from new
    // ones born at the emitter (00494c40 / 004921d0: frame ftol(frameStart), tail at the point);
    // each one added takes a random frame when the emitter's flag 0x20 is set. It shrinks from
    // its end into the pool.
    while (b.points.size() < count) {
        BoltPoint point;
        Vec3 at = from;
        if (!b.pool.empty()) {
            point = b.pool.back();
            at = point.parked;
            b.pool.pop_back();
        } else {
            point.frame = ftol(bp.frameStart);
            point.trail = at;
        }
        if ((bp.flags & kFlagRandom) != 0u) {
            const int span = std::abs(ftol(bp.frameEnd - bp.frameStart) + 1);
            if (span != 0) {
                point.frame = ftol(static_cast<float>(randMod(rng, span)) + bp.frameStart);
            }
        }
        b.points.push_back(at);
        b.state.push_back(point);
    }
    while (b.points.size() > count) {
        BoltPoint parked = b.state.back();
        parked.parked = b.points.back();
        b.pool.push_back(parked);
        b.points.pop_back();
        b.state.pop_back();
    }
    if (b.offsets.size() < count) {
        b.offsets.resize(count);
    }
    // Each point ages; one whose frame just stepped (age == +0x40) starts again from 0. A
    // Motion_Blur emitter (+0x114) draws its tail toward the point by min(dt, blurlength) / blurlength.
    for (std::size_t i = 0; i < count; ++i) {
        BoltPoint& point = b.state[i];
        if (point.age == point.stepped) {
            point.age = 0.0f;
            point.stepped = 0.0f;
            point.shown = 0.0f;
        }
        point.age += dt;
        if (bp.motionBlur && bp.blurLength != 0.0f) {
            const float w = std::min(dt, bp.blurLength) / bp.blurLength;
            point.trail = point.trail * (1.0f - w) + b.points[i] * w;
        }
    }
    b.cpTimer += dt;
    bool refreshed = false;
    if (b.cpTimer <= bp.controlPointDelay) {
        if (bp.smoothing && !b.branch && count >= 3u) {
            const float u = bp.controlPointDelay != 0.0f ? b.cpTimer / bp.controlPointDelay : 0.0f;
            placeOnCurve(b, u, 1u, count - 1u);
        }
    } else {
        b.cpTimer = 0.0f;
        b.base.front() = from;
        b.base.back() = to;
        updateControlPoints(b, bp, axes, rng);
        if (bp.branchCount > 0u && !b.branch) {
            updateBranches(emitter, bp, rng);
            for (LightningBolt& br : emitter.branches) {
                br.cpTimer = bp.controlPointDelay;
            }
            refreshed = true;
        }
        b.boltTimer = bp.delay + 0.01f;
    }
    if (b.boltTimer <= bp.delay) {
        // Between redraws the bolt follows its two ends rigidly.
        const Quat turn = rotationArc(b.prevDir, to - from);
        for (Vec3& p : b.points) {
            p = rotate(turn, p - b.prevPos) + from;
        }
        for (Vec3& p : b.base) {
            p = rotate(turn, p - b.prevPos) + from;
        }
        for (Vec3& d : b.baseTangents) {
            d = rotate(turn, d);
        }
        if (!b.branch) {
            for (std::size_t i = 0; i < emitter.activeBranches; ++i) {
                LightningBolt& br = emitter.branches[i];
                const std::size_t at = std::min(br.attach, count - 1u);
                const Vec3 q = b.points[at] + b.offsets[at];
                br.newPoints[0] = br.oldPoints[0] = q;
                br.position = q;
                br.target = from + rotate(turn, br.target - b.prevPos);
            }
        }
        b.prevDir = to - from;
        b.prevPos = from;
    } else {
        // Redraw: new end points, then a fresh scatter of offsets about the curve.
        b.points.front() = from;
        b.points.back() = to;
        if (b.targetSize != 0.0f) {
            b.points.back() = b.points.back() + targetJitter(b.targetSize, rng);
            if (b.base.size() == 2u) {
                b.base.back() = b.points.back();
                updateControlPoints(b, bp, axes, rng);
            } else {
                b.newPoints.back() = b.base.back() = b.points.back();
                const std::size_t m = b.base.size();
                const float lo = static_cast<float>(m - 2u) / static_cast<float>(m - 1u);
                const std::size_t first = static_cast<std::size_t>(std::max(ftol(static_cast<float>(count) * lo), 0)) + 1u;
                const float u = bp.controlPointDelay != 0.0f ? b.cpTimer / bp.controlPointDelay : 0.0f;
                if (count >= 2u) {
                    placeOnCurve(b, u, first, count - 1u);
                }
            }
        }
        const Vec3 d = b.points.front() - b.points.back();
        if (length(d) != 0.0f) {
            b.boltTimer = 0.0f;
            const float s = (length(d) / static_cast<float>(count)) * 0.5f * bp.scale;
            const Vec3 along = unitOr(d) * s;
            Vec3 axis = b.axis;
            const int spin = ftol(bp.zigzag);
            int turned = 0;
            for (std::size_t i = 1; i + 1u < count; ++i) {
                if (spin != 0) {
                    int a = randMod(rng, spin);
                    if ((crtRand(rng) & 1) != 0) {
                        a = 360 - a;
                    }
                    turned += a;
                    axis = rotate(axisAngle(to - from, static_cast<float>(turned) * kDegToRad), axis);
                }
                float f = static_cast<float>(i) / static_cast<float>(count);
                if (f > 0.5f) {
                    f = 1.0f - f;
                }
                const float amp = 2.0f * f * bp.radius * static_cast<float>(randMod(rng, 100)) * 0.01f;
                Vec3 shift = along * (static_cast<float>(randMod(rng, 100)) * 0.01f);
                if ((crtRand(rng) & 1) != 0) {
                    shift = shift * -1.0f;
                }
                b.offsets[i] = axis * amp + shift;
            }
            if (!b.branch) {
                for (std::size_t i = 0; i < emitter.activeBranches; ++i) {
                    LightningBolt& br = emitter.branches[i];
                    const std::size_t at = std::min(br.attach, count - 1u);
                    const Vec3 q = b.points[at] + b.offsets[at];
                    br.newPoints[0] = br.oldPoints[0] = q;
                    br.position = q;
                    br.prevDir = br.target - q;
                    br.prevPos = q;
                }
            }
        }
    }
    if (!b.branch) {
        const BoltAxes flat;
        for (std::size_t i = 0; i < emitter.activeBranches; ++i) {
            updateBolt(emitter, emitter.branches[i], bp, flat, refreshed ? bp.delay + 0.01f : dt);
        }
    }
}

// Where a point-to-point emitter aims (+0x1e4): a beam's end when a projectile or beam effect set
// one, otherwise the emitter's first reference child (0049b240, Part slot +0x38).
void resolveTarget(const Scene& scene, EmitterInstance& emitter)
{
    emitter.targetValid = false;
    emitter.targetZ = {0.0f, 0.0f, 1.0f};
    if (emitter.boltTargetSet) {
        // Inferred: the beam's target object orientation is not tracked; its Z is taken as world up.
        emitter.targetValid = true;
        emitter.targetPosition = emitter.boltTarget;
        return;
    }
    if (emitter.header.targetNode.empty()) {
        return;
    }
    const std::optional<Mdl::Pose> reference =
        emitter.model->nodePose(emitter.header.targetNode, emitter.sampledClip, emitter.lastClipTime);
    if (!reference) {
        return;
    }
    const Placement owner = ownerPlacement(scene, emitter);
    emitter.targetValid = true;
    emitter.targetPosition = owner.position + mulRotation(owner.rotation, reference->translation);
    emitter.targetZ = mulRotation(owner.rotation, column(reference->rotation, 2));
}

// updateParticleFrame 0048e460, which RenderLinked 00495b20 runs on the points each draw with
// step = 1 / fps (0 without an fps). Flag 0x20 measures from +0x40 and wraps; otherwise frames
// count up from +0x44 and stop on the last. Flag 0x1000 picks a random frame at each step.
void stepBoltFrames(LightningBolt& b, const BoltParams& bp, std::uint32_t& rng)
{
    const float step = bp.fps > 0.0f ? 1.0f / bp.fps : 0.0f;
    const bool random = (bp.flags & kFlagRandom) != 0u;
    const int span = std::abs(ftol(bp.frameEnd - bp.frameStart));
    const int first = ftol(bp.frameStart);
    const int last = ftol(bp.frameEnd);
    for (BoltPoint& point : b.state) {
        const float elapsed = point.age - (random ? point.stepped : point.shown);
        if (step == 0.0f || elapsed < step || bp.frameStart == bp.frameEnd) {
            continue;
        }
        if ((bp.flags & 0x1000u) != 0u) {
            point.frame = randMod(rng, span + 1);
            if (!random) {
                point.shown = static_cast<float>(point.frame) * step;
            }
        } else if (bp.frameStart <= bp.frameEnd) {
            const int n = static_cast<int>(std::floor(elapsed / step));
            point.frame += n;
            if (random) {
                point.shown += static_cast<float>(n) * step;
                while (last < point.frame) {
                    point.frame += (first - last) - 1;
                }
            } else {
                point.frame = std::min(point.frame, last);
                point.shown = static_cast<float>(point.frame) * step;
            }
        } else {
            const int n = static_cast<int>(std::floor(elapsed / step));
            point.frame -= n;
            if (random) {
                point.shown -= static_cast<float>(n) * step;
                while (static_cast<float>(point.frame) < bp.frameEnd) {
                    point.frame += (first - last) + 1;
                }
            } else {
                // As retail: a frame that ran below the start goes back to the start.
                if (point.frame < first) {
                    point.frame = first;
                }
                point.shown = static_cast<float>(point.frame) * step;
            }
        }
        if (bp.life < 0.0f) {
            point.stepped = 0.0f;
            point.age = 0.0f;
        } else {
            point.stepped = point.age;
        }
    }
}

void stepLightning(EmitterInstance& emitter, const Placement& place, float dt)
{
    const FxNodeParameters& p = emitter.params;
    BoltParams bp;
    bp.subDiv = p.scalar(kLightningSubDiv, 0.0f);
    bp.numControlPoints = p.scalar(kNumControlPoints, 0.0f);
    bp.controlPointRadius = p.scalar(kControlPointRadius, 0.0f);
    bp.controlPointDelay = p.scalar(kControlPointDelay, 0.0f);
    bp.tangentSpread = p.scalar(kTangentSpread, 0.0f);
    bp.tangentLength = p.scalar(kTangentLength, 0.0f);
    bp.delay = p.scalar(kLightningDelay, 0.0f);
    bp.radius = p.scalar(kLightningRadius, 0.0f);
    bp.scale = p.scalar(kLightningScale, 0.0f);
    bp.zigzag = p.scalar(kLightningZigzag, 0.0f);
    bp.targetSize = p.scalar(kTargetSize, 0.0f);
    bp.branchCount = emitter.header.branchCount;
    bp.smoothing = std::bit_cast<std::int32_t>(emitter.header.smoothing) != 0;
    bp.frameStart = p.scalar(kFrameStart, 0.0f);
    bp.frameEnd = p.scalar(kFrameEnd, bp.frameStart);
    bp.fps = p.scalar(kFps, 0.0f);
    bp.life = p.scalar(kLife, 0.0f);
    bp.blurLength = p.scalar(kBlurLength, 0.0f);
    bp.flags = emitter.header.flags;
    bp.motionBlur = lowerName(emitter.header.render) == "motion_blur";
    BoltAxes axes;
    axes.local = column(emitter.worldRotation, 2);
    axes.world = column(place.rotation, 2);
    axes.target = emitter.targetZ;
    if (!emitter.targetValid) {
        // 00498b80 with no target: the bolt has no points.
        emitter.lightning.points.clear();
        emitter.activeBranches = 0;
        emitter.bolt.clear();
        return;
    }
    if (!emitter.lightningReady) {
        // Retail sets up on the first frame (+0x1f7) and only when a target exists then; here the
        // first frame that has a target does it, so a beam whose end arrives a frame late still draws.
        initLightning(emitter, bp, axes, place.position, emitter.targetPosition);
        emitter.lightningReady = true;
    }
    LightningBolt& b = emitter.lightning;
    b.position = place.position;
    b.target = emitter.targetPosition;
    b.targetSize = bp.targetSize;
    updateBolt(emitter, b, bp, axes, dt);
    // RenderFunc 0049b680 draws (and so frame-steps) each active branch, then the main bolt.
    for (std::size_t i = 0; i < emitter.activeBranches && i < emitter.branches.size(); ++i) {
        stepBoltFrames(emitter.branches[i], bp, emitter.random);
    }
    stepBoltFrames(b, bp, emitter.random);
    emitter.bolt.resize(b.points.size());
    for (std::size_t i = 0; i < b.points.size(); ++i) {
        emitter.bolt[i] = b.points[i] + (i < b.offsets.size() ? b.offsets[i] : Vec3{});
    }
}

constexpr std::uint32_t kFlagP2P = 0x1u;
constexpr std::uint32_t kFlagP2PBezier = 0x2u;

} // namespace

float emitterCurve(float start, float mid, float end, float percentStart, float percentMid, float percentEnd, float t)
{
    if (percentStart == kNoMiddleKey) {
        return start + (end - start) * t;
    }
    if (t >= percentEnd) {
        return end;
    }
    if (t >= percentMid) {
        return mid + (end - mid) * ((t - percentMid) / (percentEnd - percentMid));
    }
    if (t >= percentStart) {
        return start + (mid - start) * ((t - percentStart) / (percentMid - percentStart));
    }
    return start;
}

Vec3 bouncedVelocity(Vec3 velocity, float bounce, float dt)
{
    // Retail 00492450, off a level floor: reflect, keep 0.75 of it, then scale the vertical part by
    // 0.033 / dt (held to 0.5 .. 1) and the bounce coefficient. A coefficient of zero stops it dead.
    if (bounce == 0.0f) {
        return {};
    }
    const float step = dt > 0.0f ? std::clamp(0.033f / dt, 0.5f, 1.0f) : 1.0f;
    return {velocity.x * 0.75f, velocity.y * 0.75f, -velocity.z * 0.75f * step * bounce};
}

int particleFrame(int first, int last, float fps, float age, bool random, std::uint32_t seed)
{
    // Retail 0048e460: without an fps a particle keeps its first frame. With one it steps up and
    // stops on the last, unless the emitter's random flag (0x20) gave it a random start, which wraps.
    const int span = std::abs(last - first);
    const int start = random && span > 0 ? first + static_cast<int>(seed % static_cast<std::uint32_t>(span + 1)) : first;
    if (fps <= 0.0f || span == 0) {
        return start;
    }
    const int stepped = start + static_cast<int>(age * fps);
    if (random) {
        return first + (stepped - first) % (span + 1);
    }
    return std::min(stepped, last);
}

// A white texel stands in for GL texture name 0, which retail binds for a missing emitter texture
// (see the emitter build below): the fragment keeps the particle's colour and alpha.
static std::uint32_t untexturedParticleTexture(Scene& scene)
{
    static const std::string kName = "__untextured";
    if (const auto it = scene.textureIndex.find(kName); it != scene.textureIndex.end()) {
        return it->second;
    }
    Texture texture;
    texture.name = kName;
    texture.image.width = 1;
    texture.image.height = 1;
    texture.image.rgba.assign(4u, 255u);
    const std::uint32_t index = static_cast<std::uint32_t>(scene.textures.size());
    scene.textures.push_back(std::move(texture));
    scene.textureIndex[kName] = index;
    return index;
}

TextureBatch makeEmitterBatch(const Scene& scene, const MdlEmitter& header, std::uint32_t texture)
{
    TextureBatch batch;
    batch.texture = texture;
    batch.unlit = true;
    // Retail's emitter draws (0048f040, 00490820, 00495b20) all clear the depth mask and lighting.
    batch.writeDepth = false;
    batch.twoSided = true;
    const std::string blend = lowerName(header.blend);
    // 0049caa0 sorts the name, ignoring case: Normal 0, PunchThrough or Punch-Through 1, anything
    // else 2, so a name it does not know is additive.
    if (blend == "normal") {
        // SRC_ALPHA / ONE_MINUS_SRC_ALPHA, alpha test off.
        batch.blend = BlendMode::Alpha;
    } else if (blend == "punchthrough" || blend == "punch-through") {
        // Blending off and the alpha test on, at the reference the sprite's own TXI selected in
        // 0047af70: the punch-through constant for a punchthrough texture, otherwise above zero.
        batch.blend = BlendMode::Opaque;
        const bool punch = texture < scene.textures.size()
            && scene.textures[texture].txi.blending == TxiInfo::Blend::PunchThrough;
        batch.alphaRef = punch ? kPunchThroughAlpha : 0.0f;
    } else {
        // SRC_ALPHA / ONE, with fog off for the draw.
        batch.blend = BlendMode::Additive;
        batch.noFog = true;
    }
    return batch;
}

void addModelEmitters(Scene& scene, Install& install, const std::shared_ptr<const Mdl>& model,
                      std::uint32_t entity, Vec3 origin, const Mat3& orientation)
{
    if (!model) {
        return;
    }
    for (const Mdl::EmitterNode& node : model->emitters()) {
        const std::string chunkName = lowerName(node.header.chunk);
        const bool hasChunk = !chunkName.empty() && chunkName != "null";
        if (!hasChunk && (node.header.texture.empty() || lowerName(node.header.texture) == "null")) {
            continue; // a lightning emitter with no sprite
        }
        EmitterInstance emitter;
        emitter.model = model;
        emitter.ordinal = node.ordinal;
        emitter.node = node.name;
        emitter.header = node.header;
        emitter.entity = entity;
        emitter.origin = origin;
        emitter.orientation = orientation;
        if (hasChunk) {
            const std::shared_ptr<Mdl> piece = loadWorldModel(install, chunkName);
            if (!piece) {
                continue;
            }
            auto parts = std::make_shared<std::vector<EmitterInstance::ChunkPart>>();
            for (MdlMeshTriangle& triangle : piece->meshTriangles()) {
                const std::uint32_t pieceTexture = ensureOptionalTexture(scene, install, triangle.texture);
                if (pieceTexture == kNoTexture) {
                    continue;
                }
                auto part = std::find_if(parts->begin(), parts->end(), [&](const EmitterInstance::ChunkPart& candidate) {
                    return candidate.texture == pieceTexture;
                });
                if (part == parts->end()) {
                    parts->push_back({pieceTexture, {}});
                    part = parts->end() - 1;
                }
                part->triangles.push_back(std::move(triangle));
            }
            if (parts->empty()) {
                continue;
            }
            emitter.texture = parts->front().texture;
            emitter.chunk = std::move(parts);
            emitter.random = 0x9E3779B9u ^ (static_cast<std::uint32_t>(scene.emitters.size()) * 2654435761u + 1u);
            scene.emitters.push_back(std::move(emitter));
            continue;
        }
        std::uint32_t texture = ensureOptionalTexture(scene, install, node.header.texture);
        if (texture == kNoTexture) {
            const std::string missing = lowerName(node.header.texture);
            if (missing.empty() || missing == "null") {
                emitter.textureMissing = true; // not established for a NULL name; kept as before
                continue;
            }
            // Retail still draws it, untextured: a texture whose resource is missing keeps GL name 0
            // (00420970 returns +0x58 = 0 without data), and its bind 004214b0 then binds name 0
            // (004204f0 at 0042154f). Name 0 has no image, so the unit is incomplete and each
            // particle shows its own colour and alpha. Paired: mgf_turret OmenEmitter01 (FXP_aurabesh01
            // is not shipped) draws solid squares of 0,102,12 in retail.
            ++scene.missingTextures[missing];
            texture = untexturedParticleTexture(scene);
        }
        emitter.texture = texture;
        emitter.random = 0x9E3779B9u ^ (static_cast<std::uint32_t>(scene.emitters.size()) * 2654435761u + 1u);
        scene.emitters.push_back(std::move(emitter));
    }
    for (const MdlLight& light : model->lights()) {
        if (light.flares.empty() || light.flareRadius <= 0.0f) {
            continue;
        }
        FlareLight flare;
        flare.local = light.position;
        flare.node = light.node;
        flare.origin = origin;
        flare.orientation = orientation;
        flare.entity = entity;
        flare.colour = light.colour * light.multiplier;
        flare.radius = light.flareRadius;
        for (const MdlLight::Flare& source : light.flares) {
            // A name the install does not carry draws nothing; shipped lights do have these.
            const std::uint32_t texture = ensureOptionalTexture(scene, install, source.texture);
            if (texture != kNoTexture) {
                flare.sprites.push_back({texture, source.size, source.position, source.colourShift});
            }
        }
        if (!flare.sprites.empty()) {
            scene.flareLights.push_back(std::move(flare));
        }
    }
}

void noteDetonateEvents(EmitterInstance& emitter, const std::string& clip, float from, float to)
{
    if (clip.empty() || lowerName(emitter.header.update) != "explosion") {
        return;
    }
    const float start = to < from ? -1.0f : from;
    for (const MdlAnimEvent& event : emitter.model->animationEvents(clip)) {
        if (event.time > start && event.time <= to && lowerName(event.name) == "detonate") {
            emitter.detonatePending = true;
        }
    }
}

namespace {

Mat3 matMul(const Mat3& a, const Mat3& b)
{
    Mat3 out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c];
        }
    }
    return out;
}

Mat3 transposed(const Mat3& a)
{
    Mat3 out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out.m[r][c] = a.m[c][r];
        }
    }
    return out;
}

// The movers' segment test through the area (the scene object at 007b92a0, vtable +0x38): the
// first face the step crosses, where, and that face's normal. Inferred: the area query has not
// been traced to its geometry; the walkmesh faces stand in for it, with each face's stored normal.
bool areaRaycast(const Scene& scene, Vec3 from, Vec3 to, Vec3& hit, Vec3& normal)
{
    const Vec3 ray = to - from;
    float best = 2.0f;
    for (const CollisionFacet& facet : scene.collision) {
        const Vec3& a = scene.collisionVertices[facet.a];
        const Vec3 e1 = scene.collisionVertices[facet.b] - a;
        const Vec3 e2 = scene.collisionVertices[facet.c] - a;
        const Vec3 p = cross(ray, e2);
        const float det = dot(e1, p);
        if (std::abs(det) < 1e-9f) {
            continue;
        }
        const Vec3 t = from - a;
        const float u = dot(t, p) / det;
        const Vec3 q = cross(t, e1);
        const float v = dot(ray, q) / det;
        const float at = dot(e2, q) / det;
        if (u >= 0.0f && v >= 0.0f && u + v <= 1.0f && at >= 0.0f && at <= 1.0f && at < best) {
            best = at;
            normal = facet.normal;
        }
    }
    if (best > 1.0f) {
        return false;
    }
    hit = from + ray * best;
    return true;
}

// Where a mover starts a particle's step after the inherit flags carried it with what moved since
// the last frame: 0x40 with the emitter (+0x170/+0x1b4 to +0x164/+0x1a4), 0x100 (without 0x40)
// with the owning model (+0x40, its +0x78/+0x84), 0x400 with the emitter's position and the
// model's turn. The P2P gravity mover 00493ae0 also turns the particle about the model for 0x100
// and 0x400; the standard mover 00492450 only moves it.
struct InheritStep {
    bool local = false;
    bool owner = false;
    bool part = false;
    bool turn = false;
    Mat3 localTurn;
    Vec3 localFrom;
    Vec3 emitterAt;
    Mat3 ownerTurn;
    Vec3 inheritFrom;
    Vec3 ownerAt;
};

InheritStep inheritStep(const EmitterInstance& emitter, const Placement& place, const Placement& owner, bool turn,
                        bool localOnly)
{
    InheritStep s;
    const std::uint32_t flags = emitter.header.flags;
    s.turn = turn;
    s.local = (flags & 0x40u) != 0u && emitter.previousValid;
    s.owner = !localOnly && (flags & 0x100u) != 0u && (flags & 0x40u) == 0u && emitter.inheritValid;
    s.part = !localOnly && (flags & 0x400u) != 0u && emitter.inheritValid;
    s.localTurn = matMul(place.rotation, transposed(emitter.previousRotation));
    s.localFrom = emitter.previousPosition;
    s.emitterAt = place.position;
    s.ownerTurn = matMul(owner.rotation, transposed(emitter.inheritRotation));
    s.inheritFrom = emitter.inheritPosition;
    s.ownerAt = owner.position;
    return s;
}

Vec3 inheritStart(const InheritStep& s, Particle& particle)
{
    Vec3 start = particle.position;
    if (s.local) {
        start = mulRotation(s.localTurn, start - s.localFrom) + s.emitterAt;
        particle.velocity = mulRotation(s.localTurn, particle.velocity);
    }
    if (s.owner) {
        if (s.turn) {
            start = mulRotation(s.ownerTurn, start - s.inheritFrom) + s.ownerAt;
            particle.velocity = mulRotation(s.ownerTurn, particle.velocity);
        } else {
            start = start + (s.ownerAt - s.inheritFrom);
        }
        particle.trail = particle.trail + (s.ownerAt - s.inheritFrom);
    }
    if (s.part) {
        const Vec3 moved = s.emitterAt - s.inheritFrom;
        if (s.turn) {
            start = mulRotation(s.ownerTurn, start - s.inheritFrom) + s.emitterAt;
            particle.trail = mulRotation(s.ownerTurn, particle.trail + moved - s.ownerAt) + s.ownerAt;
            particle.velocity = mulRotation(s.ownerTurn, particle.velocity);
        } else {
            start = start + moved;
            particle.trail = particle.trail + moved;
        }
    }
    return start;
}

// What +0x158/+0x194 keep for the next frame (the ends of 00492450 and 00493ae0).
void keepInherit(EmitterInstance& emitter, const Placement& place, const Placement& owner)
{
    const std::uint32_t flags = emitter.header.flags;
    if ((flags & 0x100u) != 0u) {
        emitter.inheritPosition = owner.position;
    } else if ((flags & 0x400u) != 0u) {
        emitter.inheritPosition = place.position;
    } else {
        return;
    }
    emitter.inheritRotation = owner.rotation;
    emitter.inheritValid = true;
}

// The second velocity (+0x18 along +0x1c) the movers add, which then fades by (1 - dt).
void secondVelocity(Particle& particle, Vec3& at, float dt)
{
    at = at + particle.direction2 * (particle.speed2 * dt);
    particle.speed2 *= 1.0f - dt;
    if (particle.speed2 < 0.0f) {
        particle.speed2 = -particle.speed2;
    }
}

// The P2P movers' bounce (00493ae0 / 00492ea0, flag 0x10): the step from where the particle was to
// where it now is (past it by sizeStart for a chunk emitter) through the area; on a hit it moves to
// the hit, reflects, keeps bounce of it, steps 0.005 off the face, and comes to rest (0.02 off it)
// when slower than sqrt(0.5).
void bounceP2P(const Scene& scene, const EmitterInstance& emitter, Particle& particle, Vec3 from, float bounce,
               float sizeStart)
{
    const bool chunk = static_cast<bool>(emitter.chunk);
    Vec3 to = particle.position;
    if (chunk) {
        to = particle.position + (particle.position - from) * sizeStart;
    }
    Vec3 hit;
    Vec3 n;
    if (!areaRaycast(scene, from, to, hit, n)) {
        return;
    }
    particle.position = hit;
    const float k = dot(particle.velocity, n);
    particle.velocity = (particle.velocity + n * (-2.0f * k)) * bounce;
    particle.position = particle.position + n * 0.005f;
    if (dot(particle.velocity, particle.velocity) < 0.5f) {
        particle.landed = true;
        particle.position = particle.position + n * 0.02f;
    }
    if (chunk) {
        particle.position = particle.position + n * sizeStart;
    }
}

// The standard mover's bounce (00492450, bounce mode 1 at 00798ba0): the same cast; a zero bounce
// stops the particle, otherwise it reflects, keeps 0.75, and its vertical part takes 0.033 / dt
// (held to 0.5 .. 1) times the bounce. Slower than sqrt(0.5) and it comes to rest.
void bounceStandard(const Scene& scene, const EmitterInstance& emitter, Particle& particle, Vec3 from, float bounce,
                    float sizeStart, float dt)
{
    const bool chunk = static_cast<bool>(emitter.chunk);
    Vec3 hit;
    Vec3 n;
    if (!areaRaycast(scene, from, particle.position, hit, n)) {
        return;
    }
    particle.position = hit;
    float speed2 = 0.0f;
    if (bounce != 0.0f) {
        float step = 1.0f;
        const float ratio = dt > 0.0f ? 0.033f / dt : 2.0f;
        if (ratio <= 1.0f) {
            step = std::max(ratio, 0.5f);
        }
        const float k = dot(n, particle.velocity);
        particle.velocity = (particle.velocity + n * (-2.0f * k)) * 0.75f;
        particle.velocity.z = step * particle.velocity.z * bounce;
        speed2 = dot(particle.velocity, particle.velocity);
    } else {
        particle.velocity = {};
    }
    if (speed2 < 0.5f) {
        particle.landed = true;
        particle.position = particle.position + n * 0.02f;
        if (chunk) {
            particle.position = particle.position + n * sizeStart;
        }
    }
}

} // namespace

void stepParticles(Scene& scene, float dt, Vec3 focus)
{
    for (EmitterInstance& emitter : scene.emitters) {
        updatePlacement(scene, emitter);
        const Placement place = worldPlacement(scene, emitter);
        // Retail has no distance limit: AddEmitterToRooms 0046cd70 files each emitter in the room
        // buckets its box falls in (every room when none), and nothing in the fountain update
        // (00496e40) or the camera fade (0048dc90) tests the distance. m12ab's sun core
        // (OmenEmitter02, fx_flare02, under AuroraLight02) runs 624 units out (RETAIL row 34).
        const bool inRange = !emitter.hidden;
        const std::string update = lowerName(emitter.header.update);
        const FxNodeParameters& p = emitter.params;
        const float birthRate = p.scalar(kBirthRate, 0.0f);
        const bool pointToPoint = (emitter.header.flags & kFlagP2P) != 0u;
        if (pointToPoint || update == "lightning") {
            resolveTarget(scene, emitter);
        }
        // FountainEmitter::Update 00496e40 calls PartEmitter::chkParticleLife, then PartEmitter::MoveFunc
        // (the carry of flag 0x40 included), and only then births, so a newborn is neither moved nor carried in
        // its first frame. Births here run after the move below; carried first, a particle born while
        // its emitter turns kept the frame's swing as an offset (mgf_turret OmenEmitter01 lamps).
        const auto birthFountain = [&]() {
            if (update == "fountain" && emitter.header.spawnType == 0) {
                // 00496e40 (00496fdc, 00497402..00497610): nothing while the whole part of the rate is 0.
                // The clock runs to 1/rate; then the rate is re-rolled by +-(rand % round(random birth,
                // controller 240 at +0xf0)), stopped at 0, and ftol(rate * clock) % (ftol(rate) + 1)
                // particles are born, even none, and the clock goes back to 0. So a roll below the rate
                // usually births nothing (charrec_light Mist03, paired cg_main).
                if (static_cast<int>(birthRate) > 0) {
                    emitter.birthDebt += dt;
                    if (emitter.birthDebt >= 1.0f / birthRate) {
                        float rolled = birthRate;
                        const int spread = static_cast<int>(std::lround(p.scalar(kRandomBirthRate, 0.0f)));
                        if (spread > 0) {
                            const bool up = static_cast<int>(nextRandom(emitter.random) * 32768.0f) % 2 != 0;
                            const int roll = static_cast<int>(nextRandom(emitter.random) * 32768.0f) % spread;
                            rolled = std::max(rolled + static_cast<float>(up ? roll : -roll), 0.0f);
                        }
                        const int births = static_cast<int>(rolled * emitter.birthDebt) % (static_cast<int>(rolled) + 1);
                        for (int birth = 0; birth < births; ++birth) {
                            spawn(emitter, place, dt);
                        }
                        emitter.birthDebt = 0.0f;
                    }
                }
            } else if (update == "fountain") {
                emitter.birthDebt += birthRate * dt;
                int births = 0;
                while (emitter.birthDebt >= 1.0f && births < 64) {
                    emitter.birthDebt -= 1.0f;
                    spawn(emitter, place, dt);
                    ++births;
                }
                emitter.birthDebt = std::min(emitter.birthDebt, 8.0f);
            }
        };
        if (inRange) {
            if (update == "fountain") {
                // Born after the move, below.
            } else if (update == "single") {
                if (emitter.particles.empty() && (!emitter.fired || emitter.header.loop)) {
                    spawn(emitter, place, dt);
                    emitter.fired = true;
                }
            } else if (update == "lightning") {
                stepLightning(emitter, place, dt);
            } else if (update == "explosion") {
                // Fires on a detonate key of the owner's clip, or on a clip event named Detonate
                // (noteDetonateEvents). A model with neither has nothing to fire it, so it stays idle.
                const FxParameter* detonate = p.find(kDetonate);
                const bool keyed = detonate != nullptr && detonate->scalar() > 0.0f && !emitter.fired;
                if (keyed || emitter.detonatePending) {
                    // 00497640: the burst also starts a grass wind at the emitter when the node's
                    // blast radius (+0x54) is set: AddPointSourceWind 004a6510 with the blast
                    // length (+0x58) as its life and strength 1. Retail does it only when the burst
                    // count (_ftol 006fae8c of instance +0x58, read here as the birthrate) is not 0.
                    if (emitter.header.blastRadius != 0.0f && static_cast<int>(birthRate) != 0) {
                        addPointSourceWind(scene, place.position, emitter.header.blastRadius,
                                           emitter.header.blastLength, 1.0f);
                    }
                    const int burst = std::clamp(static_cast<int>(birthRate), 1,
                                                 static_cast<int>(kMaxParticlesPerEmitter));
                    for (int i = 0; i < burst; ++i) {
                        spawn(emitter, place, dt);
                    }
                    emitter.fired = emitter.fired || keyed;
                }
            }
        }
        emitter.detonatePending = false;
        const float mass = p.scalar(kMass, 0.0f);
        const float drag = p.scalar(kDrag, 0.0f);
        const float bounce = p.scalar(kBounceCo, 0.0f);
        const float sizeStart = p.scalar(kSizeStart, 0.0f);
        const bool bounces = (emitter.header.flags & kFlagBounce) != 0u;
        const Placement owner = ownerPlacement(scene, emitter);
        for (Particle& particle : emitter.particles) {
            particle.age += dt;
        }
        if (pointToPoint) {
            // MoveFunc 00494d40: a point-to-point emitter with no target leaves its particles where
            // they are; with one, flag 0x2 picks the bezier mover, otherwise the gravity mover.
            if (emitter.targetValid) {
                const Vec3 aim = emitter.targetPosition + targetJitter(p.scalar(kTargetSize, 0.0f), emitter.random);
                const float threshold = p.scalar(kThreshold, 0.0f);
                const float threshold2 = threshold * threshold;
                if ((emitter.header.flags & kFlagP2PBezier) != 0u) {
                    // 00492ea0: a cubic from the emitter out along its world Z by p2p_bezier2, into the
                    // target along the target's Z by p2p_bezier3; the free path blends into it by
                    // (life fraction ^ 3) ^ combinetime. Only flag 0x40 carries particles here.
                    const InheritStep carry = inheritStep(emitter, place, owner, false, true);
                    const Vec3 c0 = place.position;
                    const Vec3 c1 = c0 + column(place.rotation, 2) * p.scalar(kP2PBezier2, 0.0f);
                    const Vec3 c2 = aim + emitter.targetZ * p.scalar(kP2PBezier3, 0.0f);
                    const float combine = p.scalar(kCombineTime, 0.0f);
                    for (Particle& particle : emitter.particles) {
                        const Vec3 from = particle.position;
                        const Vec3 start = inheritStart(carry, particle);
                        const Vec3 next = start + particle.velocity * dt;
                        const float u = particle.life != 0.0f ? std::min(particle.age / particle.life, 1.0f) : 1.0f;
                        const Vec3 onCurve = bezier(c0, c1, c2, aim, u);
                        const float w = std::pow(u * u * u, combine);
                        Vec3 at = onCurve * w + next * (1.0f - w);
                        secondVelocity(particle, at, dt);
                        particle.position = at;
                        particle.velocity.z -= dt * mass * 9.81f;
                        if (bounces) {
                            bounceP2P(scene, emitter, particle, from, bounce, sizeStart);
                        }
                    }
                } else {
                    // 00493ae0: pulled toward the target by grav, slowed by drag ^ dt; dies within
                    // threshold of it. A particle already inside the threshold ends this frame's
                    // pass, as in retail, and then +0x158 is not kept either.
                    const InheritStep carry = inheritStep(emitter, place, owner, true, false);
                    const float pull = p.scalar(kGravity, 0.0f);
                    const float keep = std::pow(drag, dt);
                    bool ended = false;
                    for (Particle& particle : emitter.particles) {
                        const Vec3 from = particle.position;
                        const Vec3 start = inheritStart(carry, particle);
                        Vec3 next = start + particle.velocity * dt;
                        const Vec3 d = aim - start;
                        if (dot(d, d) <= threshold2) {
                            particle.age = particle.life;
                            ended = true;
                            break;
                        }
                        particle.velocity = particle.velocity * keep;
                        const Vec3 g = unitOr(d) * pull;
                        next = next + g * (dt * dt);
                        particle.velocity = particle.velocity + g * dt;
                        const Vec3 nearest = segmentProject(start, next, aim);
                        const Vec3 miss = aim - nearest;
                        if (dot(miss, miss) <= threshold2) {
                            particle.age = particle.life;
                        }
                        secondVelocity(particle, next, dt);
                        particle.position = next;
                        particle.velocity.z -= dt * mass * 9.81f;
                        if (bounces) {
                            bounceP2P(scene, emitter, particle, from, bounce, sizeStart);
                        }
                    }
                    if (!ended) {
                        keepInherit(emitter, place, owner);
                    }
                }
            }
        } else {
            // 00492450: a particle at rest (+0x6c) stays; the rest move by their velocity and the
            // second velocity, then fall by mass * 9.81. The gravity and drag controllers are not
            // read here.
            const InheritStep carry = inheritStep(emitter, place, owner, false, false);
            for (Particle& particle : emitter.particles) {
                if (particle.landed) {
                    continue;
                }
                const Vec3 from = particle.position;
                // Carry first: inheritStart turns the velocity too, and the step must use the turned
                // one. In one expression the two operands are unsequenced, and MSVC read the old
                // velocity, so a turning emitter's particles slid sideways each frame (mgf_turret lamps).
                const Vec3 carried = inheritStart(carry, particle);
                Vec3 at = carried + particle.velocity * dt;
                secondVelocity(particle, at, dt);
                particle.position = at;
                particle.velocity.z -= dt * mass * 9.81f;
                if (bounces) {
                    bounceStandard(scene, emitter, particle, from, bounce, sizeStart, dt);
                }
            }
            keepInherit(emitter, place, owner);
        }
        if (inRange) {
            birthFountain();
        }
        emitter.previousPosition = place.position;
        emitter.previousRotation = place.rotation;
        emitter.previousValid = true;
        emitter.particles.erase(std::remove_if(emitter.particles.begin(), emitter.particles.end(),
                                               [](const Particle& particle) {
                                                   return particle.life >= 0.0f && particle.age >= particle.life;
                                               }),
                                emitter.particles.end());
    }
    for (FlareLight& flare : scene.flareLights) {
        const Vec3 world = flareWorldPosition(scene, flare);
        if (flare.hidden || length(world - focus) > flare.radius) {
            continue;
        }
        // 00457ec0 casts through the scene (00456b60: the area, then the objects 0044fe00) and fades
        // by 3 a second (0078e3e8) either way. With 0078e3ec set, as RETAIL row 34 logs it, the ray
        // runs from the light to the camera, which matters to the one-sided object faces.
        const float change = kFlareFadeRate * dt;
        flare.visibility = segmentBlocked(scene, world, focus) || segmentHitsPlacedModel(scene, world, focus)
            ? std::max(flare.visibility - change, 0.0f)
            : std::min(flare.visibility + change, 1.0f);
    }
}

void buildParticleMesh(Scene& scene, Vec3 eye, Vec3 forward, Vec3 up)
{
    ParticleMesh& out = scene.particleMesh;
    out.vertices.clear();
    out.batches.clear();
    const Vec3 viewForward = normalize(forward);
    const Vec3 viewRight = normalize(cross(viewForward, up));
    const Vec3 viewUp = cross(viewRight, viewForward);
    // qsort comparator 0044f5a0 (Scene::DoEmitterBucket 004509b0 draws the list in order): emitters of one model
    // instance by the node's render order (+0x108), lowest first; otherwise additive ones after the rest, then by
    // the owner's distance from the camera, deepest first. Stable because this comparison is not transitive.
    // KE_EMITTER_TRACE=<node>: every 60th build, that emitter's live particles and the first one's place.
    static const std::string traced = [] {
        std::string name;
#if defined(_WIN32)
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, "KE_EMITTER_TRACE") == 0 && value != nullptr) {
            name = value;
            std::free(value);
        }
#else
        if (const char* value = std::getenv("KE_EMITTER_TRACE"); value != nullptr) {
            name = value;
        }
#endif
        return lowerName(name);
    }();
    static unsigned traceFrame = 0;
    if (!traced.empty() && (traceFrame++ % 60u) == 0u) {
        for (const EmitterInstance& emitter : scene.emitters) {
            if (lowerName(emitter.node) == traced) {
                const Vec3 at = emitter.particles.empty() ? Vec3{} : emitter.particles.front().position;
                const Vec3 from = worldPlacement(scene, emitter).position;
                const Vec3 axis = column(worldPlacement(scene, emitter).rotation, 2);
                std::fprintf(stderr, "emitter trace %s: particles %zu first %.3f,%.3f,%.3f emitter %.3f,%.3f,%.3f z %.4f,%.4f,%.4f\n",
                             emitter.node.c_str(), emitter.particles.size(), at.x, at.y, at.z, from.x, from.y, from.z,
                             axis.x, axis.y, axis.z);
                // Every live particle, in the fields RETAIL row 35 logs (age +0x70, place, velocity).
                std::vector<const Particle*> oldest;
                for (const Particle& particle : emitter.particles) {
                    oldest.push_back(&particle);
                }
                std::sort(oldest.begin(), oldest.end(), [](const Particle* a, const Particle* b) { return a->age > b->age; });
                for (std::size_t i = 0; i < oldest.size(); ++i) {
                    const Particle& o = *oldest[i];
                    std::fprintf(stderr, "emitter trace %s: age %.3f life %.3f at %.3f,%.3f,%.3f v %.3f,%.3f,%.3f landed %d\n",
                                 emitter.node.c_str(), o.age, o.life, o.position.x, o.position.y, o.position.z,
                                 o.velocity.x, o.velocity.y, o.velocity.z, o.landed ? 1 : 0);
                }
            }
        }
    }
    std::vector<std::pair<float, const EmitterInstance*>> order;
    for (const EmitterInstance& emitter : scene.emitters) {
        // RenderFunc 0049b680: a point-to-point emitter with no target draws nothing.
        if ((emitter.header.flags & kFlagP2P) != 0u && !emitter.targetValid) {
            continue;
        }
        if (!emitter.particles.empty() || emitter.lightning.points.size() >= 2u) {
            order.emplace_back(length(ownerPlacement(scene, emitter).position - eye), &emitter);
        }
    }
    const auto additive = [](const EmitterInstance& emitter) {
        const std::string blend = lowerName(emitter.header.blend);
        return blend != "normal" && blend != "punchthrough" && blend != "punch-through";
    };
    std::stable_sort(order.begin(), order.end(), [&](const auto& a, const auto& b) {
        const EmitterInstance& first = *a.second;
        const EmitterInstance& second = *b.second;
        if (first.entity == second.entity && first.model == second.model && a.first == b.first) {
            return first.header.renderOrder < second.header.renderOrder;
        }
        if (additive(first) != additive(second)) {
            return additive(second);
        }
        return a.first > b.first;
    });
    for (const auto& [distance, emitterPointer] : order) {
        (void)distance;
        const EmitterInstance& emitter = *emitterPointer;
        const FxNodeParameters& p = emitter.params;
        if (emitter.chunk) {
            // Each particle draws the chunk model, solid, tumbling about an axis of its own.
            // Inferred: retail's chunk path was not read; the look comes from frames of its droid
            // explosion (v_drdexpld_01), which throw black solid pieces.
            const float spinRate = p.scalar(kParticleRot, 0.0f);
            const Vec3 tint{0.0f, 0.0f, 0.0f}; // Retail frames show them nearly black: unlit, not colorStart.
            for (const EmitterInstance::ChunkPart& part : *emitter.chunk) {
                TextureBatch solid = makeEmitterBatch(scene, emitter.header, part.texture);
                solid.blend = BlendMode::Opaque;
                solid.writeDepth = true;
                solid.alphaRef = 0.0f;
                solid.firstIndex = static_cast<std::uint32_t>(out.vertices.size());
                for (const Particle& particle : emitter.particles) {
                    std::uint32_t seed = particle.seed;
                    const Vec3 raw{nextRandom(seed) - 0.5f, nextRandom(seed) - 0.5f, nextRandom(seed) - 0.5f};
                    const Vec3 axis = length(raw) > 1e-3f ? normalize(raw) : Vec3{0.0f, 0.0f, 1.0f};
                    const float angle = particle.rotation + spinRate * particle.age;
                    const float c = std::cos(angle);
                    const float s = std::sin(angle);
                    const auto turned = [&](Vec3 v) {
                        return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0f - c));
                    };
                    for (const MdlMeshTriangle& triangle : part.triangles) {
                        for (const MdlMeshVertex& corner : triangle.v) {
                            SceneVertex vertex;
                            vertex.position = particle.position + turned(corner.position);
                            vertex.normal = turned(corner.normal);
                            vertex.uv = corner.uv;
                            vertex.selfIllum = tint;
                            vertex.alpha = 1.0f;
                            out.vertices.push_back(vertex);
                        }
                    }
                }
                solid.indexCount = static_cast<std::uint32_t>(out.vertices.size()) - solid.firstIndex;
                if (solid.indexCount != 0) {
                    out.batches.push_back(solid);
                }
            }
            continue;
        }
        const std::string render = lowerName(emitter.header.render);
        const Vec3 colourStart = p.colour(kColorStart, {1.0f, 1.0f, 1.0f});
        const Vec3 colourEnd = p.colour(kColorEnd, colourStart);
        const Vec3 colourMid = p.colour(kColorMid, colourStart);
        const float alphaStart = p.scalar(kAlphaStart, 1.0f);
        const float alphaEnd = p.scalar(kAlphaEnd, alphaStart);
        const float alphaMid = p.scalar(kAlphaMid, alphaStart);
        const float sizeStart = p.scalar(kSizeStart, 1.0f);
        const float sizeEnd = p.scalar(kSizeEnd, sizeStart);
        const float sizeMid = p.scalar(kSizeMid, sizeStart);
        const float sizeStartY = p.scalar(kSizeStartY, 0.0f);
        const float sizeEndY = p.scalar(kSizeEndY, 0.0f);
        const float sizeMidY = p.scalar(kSizeMidY, 0.0f);
        const float frameStart = p.scalar(kFrameStart, 0.0f);
        const float frameEnd = p.scalar(kFrameEnd, frameStart);
        const float fps = p.scalar(kFps, 0.0f);
        const float spin = p.scalar(kParticleRot, 0.0f);
        const int gridX = std::max(emitter.header.gridX, 1);
        const int gridY = std::max(emitter.header.gridY, 1);
        // Where in a particle's life the three keys sit (0048f040); 255 means no middle key.
        const float percentStart = p.scalar(kPercentStart, kNoMiddleKey);
        const float percentMid = p.scalar(kPercentMid, 0.5f);
        const float percentEnd = p.scalar(kPercentEnd, 1.0f);
        const auto curve = [&](float start, float mid, float end, float t) {
            return emitterCurve(start, mid, end, percentStart, percentMid, percentEnd, t);
        };

        const bool lightning = lowerName(emitter.header.update) == "lightning";
        if (render == "linked" || lightning) {
            // Retail 0049b680 sends every "Linked" emitter, whatever its update, to 00495b20: one
            // strip through the particle list in its stored order (oldest first). Segment i takes
            // particle i's size, colour, alpha and frame; its corners sit at +-size (2 x 0.5 x size)
            // across the segment as seen by the camera and sizeY/4 (x 0.5 x 0.5) past each end along
            // it, and a segment's first corners reuse the previous segment's last ones. Each segment
            // maps one whole grid cell, u across, v along. Depth writes off; the batch's blend
            // follows the emitter's blend as for sprites.
            struct StripPoint {
                Vec3 position;
                float t = 0.0f;
                float age = 0.0f;
                std::uint32_t seed = 0;
                int frame = -1; // a bolt point's own frame (+0x3c, stepped by 0048e460)
            };
            const auto drawStrip = [&](const std::vector<StripPoint>& points, float sizeScale) {
                if (points.size() < 2u) {
                    return;
                }
                TextureBatch strip = makeEmitterBatch(scene, emitter.header, emitter.texture);
                strip.firstIndex = static_cast<std::uint32_t>(out.vertices.size());
                Vec3 carryPlus;
                Vec3 carryMinus;
                for (std::size_t i = 0; i + 1u < points.size(); ++i) {
                    const StripPoint& particle = points[i];
                    const StripPoint& next = points[i + 1u];
                    const float t = particle.t;
                    const float alpha = curve(alphaStart, alphaMid, alphaEnd, t);
                    const float size = curve(sizeStart, sizeMid, sizeEnd, t) * sizeScale;
                    const float sizeY = curve(sizeStartY, sizeMidY, sizeEndY, t) * sizeScale;
                    const Vec3 colour{curve(colourStart.x, colourMid.x, colourEnd.x, t),
                                      curve(colourStart.y, colourMid.y, colourEnd.y, t),
                                      curve(colourStart.z, colourMid.z, colourEnd.z, t)};
                    // The segment's direction projected onto the camera's right/up plane.
                    const Vec3 d = next.position - particle.position;
                    const float sx = dot(d, viewRight);
                    const float sy = dot(d, viewUp);
                    const float len = std::sqrt(sx * sx + sy * sy);
                    Vec3 along = viewRight;
                    Vec3 across = viewUp;
                    if (len > 1e-6f) {
                        along = viewRight * (sx / len) + viewUp * (sy / len);
                        across = viewUp * (sx / len) - viewRight * (sy / len);
                    }
                    const Vec3 side = across * size;
                    const Vec3 extend = along * (sizeY * 0.25f);
                    if (i == 0u) {
                        carryPlus = particle.position + side - extend;
                        carryMinus = particle.position - side - extend;
                    }
                    const Vec3 nextPlus = next.position + side + extend;
                    const Vec3 nextMinus = next.position - side + extend;
                    const int frame = particle.frame >= 0
                        ? particle.frame
                        : particleFrame(static_cast<int>(frameStart), static_cast<int>(frameEnd), fps, particle.age,
                                        (emitter.header.flags & kFlagRandom) != 0u, particle.seed);
                    const float u0 = static_cast<float>(frame % gridX) / static_cast<float>(gridX);
                    const float v0 = static_cast<float>((frame / gridX) % gridY) / static_cast<float>(gridY);
                    const float du = 1.0f / static_cast<float>(gridX);
                    const float dv = 1.0f / static_cast<float>(gridY);
                    const Vec3 positions[4] = {carryPlus, nextPlus, nextMinus, carryMinus};
                    const Vec2 uvs[4] = {{u0 + du, v0 + dv}, {u0 + du, v0}, {u0, v0}, {u0, v0 + dv}};
                    for (const int index : {0, 1, 2, 0, 2, 3}) {
                        SceneVertex vertex;
                        vertex.position = positions[index];
                        vertex.normal = {0.0f, 0.0f, 1.0f};
                        vertex.uv = uvs[index];
                        vertex.selfIllum = colour;
                        vertex.alpha = alpha;
                        out.vertices.push_back(vertex);
                    }
                    carryPlus = nextPlus;
                    carryMinus = nextMinus;
                }
                strip.twoSided = true;
                strip.writeDepth = false;
                strip.indexCount = static_cast<std::uint32_t>(out.vertices.size()) - strip.firstIndex;
                if (strip.indexCount != 0) {
                    out.batches.push_back(strip);
                }
            };
            std::vector<StripPoint> points;
            if (lightning) {
                // 0049b680 draws each active branch, then the main bolt. A Lightning emitter sets
                // +0x1f4 so 00495b20 takes t = index / count and adds the +0x270 offsets.
                const auto boltPoints = [&](const LightningBolt& bolt) {
                    points.clear();
                    const float count = static_cast<float>(bolt.points.size());
                    for (std::size_t i = 0; i < bolt.points.size(); ++i) {
                        const Vec3 offset = i < bolt.offsets.size() ? bolt.offsets[i] : Vec3{};
                        const int frame = i < bolt.state.size() ? std::max(bolt.state[i].frame, 0) : 0;
                        points.push_back({bolt.points[i] + offset, static_cast<float>(i) / count, 0.0f, 0u, frame});
                    }
                };
                for (std::size_t i = 0; i < emitter.activeBranches && i < emitter.branches.size(); ++i) {
                    boltPoints(emitter.branches[i]);
                    drawStrip(points, emitter.branches[i].sizeScale);
                }
                boltPoints(emitter.lightning);
                drawStrip(points, 1.0f);
                continue;
            }
            for (const Particle& particle : emitter.particles) {
                const float t = particle.life < 0.0f
                    ? std::clamp(particle.age * 0.01f, 0.0f, 1.0f)
                    : std::clamp(particle.age / std::max(particle.life, 0.001f), 0.0f, 1.0f);
                points.push_back({particle.position, t, particle.age, particle.seed});
            }
            drawStrip(points, 1.0f);
            continue;
        }
        TextureBatch batch = makeEmitterBatch(scene, emitter.header, emitter.texture);
        batch.firstIndex = static_cast<std::uint32_t>(out.vertices.size());
        // 0048f040 fills its vertex array from the end (list[0] in the last slot), so the newest particle draws first
        // and the oldest on top; the motion-blur path 00490820 fills in list order.
        const bool newestFirst = render != "motion_blur";
        const std::size_t particleCount = emitter.particles.size();
        for (std::size_t particleIndex = 0; particleIndex < particleCount; ++particleIndex) {
            const Particle& particle = emitter.particles[newestFirst ? particleCount - 1u - particleIndex : particleIndex];
            // 0048f040 scales age by 1/life, or by 0.01 when life <= 0; 0048e460 sets an undying
            // particle's age back to 0 at each frame step, so with frames playing it stays within one.
            float lifeAge = particle.age;
            if (particle.life < 0.0f && fps > 0.0f) {
                lifeAge = std::fmod(particle.age, 1.0f / fps);
            }
            const float t = particle.life < 0.0f ? std::clamp(lifeAge * 0.01f, 0.0f, 1.0f)
                                                 : std::clamp(particle.age / std::max(particle.life, 0.001f), 0.0f, 1.0f);
            // 0048f040 writes a quad for every particle in the list (0x60 bytes each); nothing compares
            // the alpha with the threshold controller (164), which only the point-to-point movers read.
            // MGB_sithfighter's explode emitters (threshold 1) hid every particle when it did.
            const float alpha = curve(alphaStart, alphaMid, alphaEnd, t);
            const float size = curve(sizeStart, sizeMid, sizeEnd, t);
            float sizeY = curve(sizeStartY, sizeMidY, sizeEndY, t);
            if (sizeY <= 0.0f) {
                sizeY = size;
            }
            const Vec3 colour{curve(colourStart.x, colourMid.x, colourEnd.x, t),
                              curve(colourStart.y, colourMid.y, colourEnd.y, t),
                              curve(colourStart.z, colourMid.z, colourEnd.z, t)};
            const int frame = particleFrame(static_cast<int>(frameStart), static_cast<int>(frameEnd), fps, particle.age,
                                            (emitter.header.flags & kFlagRandom) != 0u, particle.seed);
            const float u0 = static_cast<float>(frame % gridX) / static_cast<float>(gridX);
            const float v0 = static_cast<float>((frame / gridX) % gridY) / static_cast<float>(gridY);
            const float du = 1.0f / static_cast<float>(gridX);
            const float dv = 1.0f / static_cast<float>(gridY);

            // Axes by render mode.
            Vec3 axisX = viewRight;
            Vec3 axisY = viewUp;
            if (render == "billboard_to_local_z") {
                // 0048f040: the quad takes the emitter node's own world orientation (its quaternion
                // at +0x1a4) instead of the camera's, so particles lie in the node's XY plane (the
                // galaxy map's tilted galaxy, galaxy.mdl Galaxy_01..03; paired frame galaxy_settled).
                const Mat3 rotation = worldPlacement(scene, emitter).rotation;
                axisX = normalize(mulRotation(rotation, {1.0f, 0.0f, 0.0f}));
                axisY = normalize(mulRotation(rotation, {0.0f, 1.0f, 0.0f}));
            } else if (render == "billboard_to_world_z") {
                // 0048f040, mode 00798b74 (0049caa0 maps "Billboard_to_World_Z" to it): the facing
                // normal is world (0,0,1) under an identity base (004ac960 with angle 0), X is the
                // particle's turn about Z applied to (1,0,0) and Y = cross(normal, X) (004ab210), so
                // the quad lies flat in world XY (main menu Mist/Mist01; paired 0001_main).
                axisX = {1.0f, 0.0f, 0.0f};
                axisY = {0.0f, 1.0f, 0.0f};
            } else if (render == "aligned_to_world_z") {
                // 0048f040, mode 00798b7c: the camera branch (base = camera quaternion) with Y forced
                // to world (0,0,+-1); the sign follows the camera's up (0044f420).
                axisY = {0.0f, 0.0f, viewUp.z < 0.0f ? -1.0f : 1.0f};
                axisX = normalize(cross(viewForward, axisY));
            } else if (render == "aligned_to_particle_dir" || render == "motion_blur") {
                const Vec3 direction = normalize(particle.velocity);
                if (length(direction) > 0.5f) {
                    axisY = direction;
                    axisX = normalize(cross(viewForward, axisY));
                }
            }
            const float angle = particle.rotation + spin * particle.age;
            if (spin != 0.0f) {
                const float c = std::cos(angle);
                const float s = std::sin(angle);
                const Vec3 x = axisX * c + axisY * s;
                const Vec3 y = axisY * c - axisX * s;
                axisX = x;
                axisY = y;
            }
            const Vec3 hx = axisX * (size * 0.5f);
            const Vec3 hy = axisY * (sizeY * 0.5f);
            SceneVertex corners[4];
            const Vec3 positions[4] = {particle.position - hx - hy, particle.position + hx - hy,
                                       particle.position + hx + hy, particle.position - hx + hy};
            const Vec2 uvs[4] = {{u0, v0 + dv}, {u0 + du, v0 + dv}, {u0 + du, v0}, {u0, v0}};
            for (int i = 0; i < 4; ++i) {
                corners[i].position = positions[i];
                corners[i].normal = {0.0f, 0.0f, 1.0f};
                corners[i].uv = uvs[i];
                corners[i].selfIllum = colour;
                corners[i].alpha = alpha;
            }
            for (const int index : {0, 1, 2, 0, 2, 3}) {
                out.vertices.push_back(corners[index]);
            }
        }
        batch.indexCount = static_cast<std::uint32_t>(out.vertices.size()) - batch.firstIndex;
        if (batch.indexCount != 0) {
            out.batches.push_back(batch);
        }
    }
    // Lens flares, after retail 00457ec0: ONE / ONE with depth test, lighting and fog off, on a
    // plane two units ahead of the camera under the scene's own projection.
    for (const FlareLight& flare : scene.flareLights) {
        if (flare.visibility <= 0.0f || flare.hidden) {
            continue;
        }
        const Vec3 toLight = flareWorldPosition(scene, flare) - eye;
        const float distance = length(toLight);
        if (distance > flare.radius || distance < 1e-4f) {
            continue;
        }
        const Vec3 view{dot(toLight, viewRight) / distance, dot(toLight, viewUp) / distance,
                        dot(toLight, viewForward) / distance};
        if (view.z <= 0.0f) {
            continue; // Behind the camera: the facing term below is zero.
        }
        const float facing = view.z * view.z;
        const float across = kFlarePlane / view.z;
        const float fade = (flare.radius - distance) / flare.radius;
        for (const FlareLight::Sprite& sprite : flare.sprites) {
            const float half = sprite.size * fade * kFlareScale;
            const float scale = flare.visibility * facing * fade;
            const Vec3 colour{std::clamp((sprite.colourShift.x + flare.colour.x) * scale, 0.0f, 1.0f),
                              std::clamp((sprite.colourShift.y + flare.colour.y) * scale, 0.0f, 1.0f),
                              std::clamp((sprite.colourShift.z + flare.colour.z) * scale, 0.0f, 1.0f)};
            const Vec3 centre = eye + viewForward * kFlarePlane
                + viewRight * (view.x * across * sprite.position) + viewUp * (view.y * across * sprite.position);
            TextureBatch batch;
            batch.texture = sprite.texture;
            batch.unlit = true;
            batch.twoSided = true;
            batch.writeDepth = false;
            batch.depthTest = false;
            batch.noFog = true;
            batch.opaqueOutput = true;
            batch.blend = BlendMode::Additive;
            batch.firstIndex = static_cast<std::uint32_t>(out.vertices.size());
            const Vec3 hx = viewRight * half;
            const Vec3 hy = viewUp * half;
            const Vec3 positions[4] = {centre - hx - hy, centre + hx - hy, centre + hx + hy, centre - hx + hy};
            const Vec2 uvs[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
            for (const int index : {0, 1, 2, 0, 2, 3}) {
                SceneVertex vertex;
                vertex.position = positions[index];
                vertex.normal = {0.0f, 0.0f, 1.0f};
                vertex.uv = uvs[index];
                vertex.selfIllum = colour;
                vertex.alpha = 1.0f;
                out.vertices.push_back(vertex);
            }
            batch.indexCount = 6u;
            out.batches.push_back(batch);
        }
    }
}

} // namespace ke::kotor
