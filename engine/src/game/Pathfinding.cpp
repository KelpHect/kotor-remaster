// Walkmesh navigation. See Pathfinding.h for how this relates to retail.
#include "game/Pathfinding.h"

#include "core/Log.h"
#include "game/GameSession.h"
#include "game/ScriptHost.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <queue>
#include <set>
#include <tuple>
#include <unordered_map>

namespace ke::nav {

using kotor::Vec3;

inline float cross2(float ax, float ay, float bx, float by)
{
    return ax * by - ay * bx;
}

inline float triCross(Vec3 a, Vec3 b, Vec3 c)
{
    return cross2(b.x - a.x, b.y - a.y, c.x - a.x, c.y - a.y);
}

inline float length2D(Vec3 a, Vec3 b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

inline float length3D(Vec3 a, Vec3 b)
{
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)
                     + (a.z - b.z) * (a.z - b.z));
}

inline Vec3 lerp(Vec3 a, Vec3 b, float t)
{
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

inline Vec3 centroid(const Tri& tri)
{
    return {(tri.v[0].x + tri.v[1].x + tri.v[2].x) / 3.0f,
            (tri.v[0].y + tri.v[1].y + tri.v[2].y) / 3.0f,
            (tri.v[0].z + tri.v[1].z + tri.v[2].z) / 3.0f};
}

inline Vec3 closestOnSegment2D(Vec3 point, Vec3 a, Vec3 b)
{
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float len = dx * dx + dy * dy;
    if (len <= 1.0e-12f) {
        return a;
    }
    const float t = std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / len, 0.0f, 1.0f);
    return lerp(a, b, t);
}

namespace {

// Most room seams in the shipped walkmeshes share exact vertices, and this only absorbs
// float noise from door cuts. Not all do: korr_m39aa's rooms meet with a chamfer on one
// side (m39aa_02 against m39aa_05), so buildLinks joins those by overlap.
constexpr float kWeld = 0.01f;
constexpr float kOnLine = 0.002f;
constexpr float kMinArea = 1.0e-5f;
constexpr float kInsideEps = 1.0e-3f;
constexpr float kPortalMatch = 0.02f;
constexpr float kDoorBackoff = 0.75f;
constexpr float kCellSize = 4.0f;

struct VertexKey {
    std::int64_t x, y, z;
    bool operator==(const VertexKey&) const = default;
};

VertexKey keyOf(Vec3 v)
{
    return {std::llround(v.x / kWeld), std::llround(v.y / kWeld), std::llround(v.z / kWeld)};
}

struct EdgeKey {
    VertexKey a, b;
    bool operator==(const EdgeKey&) const = default;
};

struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey& key) const
    {
        const auto mix = [](std::size_t seed, std::int64_t value) {
            return seed ^ (std::hash<std::int64_t>{}(value) + 0x9e3779b97f4a7c15ULL + (seed << 6)
                           + (seed >> 2));
        };
        std::size_t seed = 0;
        for (const std::int64_t value : {key.a.x, key.a.y, key.a.z, key.b.x, key.b.y, key.b.z}) {
            seed = mix(seed, value);
        }
        return seed;
    }
};

EdgeKey edgeKey(Vec3 p, Vec3 q)
{
    VertexKey a = keyOf(p);
    VertexKey b = keyOf(q);
    if (std::tie(b.x, b.y, b.z) < std::tie(a.x, a.y, a.z)) {
        std::swap(a, b);
    }
    return {a, b};
}

// Parameter on p -> q of the point where a -> b crosses its line.
float lineParam(Vec3 p, Vec3 q, Vec3 a, Vec3 b)
{
    const float sa = triCross(p, q, a);
    const float sb = triCross(p, q, b);
    return sa / (sa - sb);
}

bool closedAny(const Link& link, const std::vector<char>& closed)
{
    for (const int door : link.doors) {
        if (door >= 0 && static_cast<std::size_t>(door) < closed.size() && closed[door] != 0) {
            return true;
        }
    }
    return false;
}

} // namespace

NavMesh::NavMesh(const kotor::Scene& scene, std::vector<DoorLine> doors, std::vector<DoorLine> walls)
    : doors_(std::move(doors))
    , walls_(std::move(walls))
{
    tris_.reserve(scene.collision.size());
    for (const kotor::CollisionFacet& facet : scene.collision) {
        if (!facet.walkable) {
            continue;
        }
        Tri tri;
        tri.v[0] = scene.collisionVertices[facet.a];
        tri.v[1] = scene.collisionVertices[facet.b];
        tri.v[2] = scene.collisionVertices[facet.c];
        tri.room = static_cast<int>(facet.room);
        tri.seamEdges = facet.seamEdges;
        const float area = triCross(tri.v[0], tri.v[1], tri.v[2]);
        if (std::fabs(area) < kMinArea) {
            continue; // Vertical in plan view: nothing can stand on it.
        }
        if (area < 0.0f) {
            std::swap(tri.v[1], tri.v[2]);
            // Edges 0 and 2 trade places: 0-1 became 0-2 and 2-0 became 1-0.
            const std::uint8_t seam = tri.seamEdges;
            tri.seamEdges = static_cast<std::uint8_t>((seam & 2u) | ((seam & 1u) << 2) | ((seam & 4u) >> 2));
        }
        tris_.push_back(tri);
    }
    for (const DoorLine& door : doors_) {
        cutAlong(door);
    }
    buildLinks();
    buildGrid();
}

float NavMesh::walkableArea() const
{
    float total = 0.0f;
    for (const Tri& tri : tris_) {
        total += 0.5f * triCross(tri.v[0], tri.v[1], tri.v[2]);
    }
    return total;
}

// Splits every triangle the door's centre line crosses along that line, so the
// two sides of the door are different triangles and the door is a set of links.
void NavMesh::cutAlong(const DoorLine& door)
{
    const float span = length2D(door.a, door.b);
    if (span < 1.0e-3f) {
        return;
    }
    std::vector<Tri> added;
    for (std::size_t index = 0; index < tris_.size(); ++index) {
        const Tri tri = tris_[index];
        const float zMax = std::max({tri.v[0].z, tri.v[1].z, tri.v[2].z});
        const float zMin = std::min({tri.v[0].z, tri.v[1].z, tri.v[2].z});
        if (zMax < door.zMin || zMin > door.zMax) {
            continue;
        }
        float side[3];
        bool above = false;
        bool below = false;
        for (int i = 0; i < 3; ++i) {
            side[i] = triCross(door.a, door.b, tri.v[i]) / span;
            if (std::fabs(side[i]) < kOnLine) {
                side[i] = 0.0f;
            }
            above = above || side[i] > 0.0f;
            below = below || side[i] < 0.0f;
        }
        if (!above || !below) {
            continue;
        }
        // Where the line enters and leaves this triangle.
        std::vector<Vec3> cut;
        for (int i = 0; i < 3; ++i) {
            const int j = (i + 1) % 3;
            if (side[i] == 0.0f) {
                cut.push_back(tri.v[i]);
            } else if (side[i] * side[j] < 0.0f) {
                // Canonical edge direction, so a neighbour sharing the edge gets
                // the identical point.
                const bool flip = std::tie(tri.v[j].x, tri.v[j].y) < std::tie(tri.v[i].x, tri.v[i].y);
                const Vec3 p = flip ? tri.v[j] : tri.v[i];
                const Vec3 q = flip ? tri.v[i] : tri.v[j];
                cut.push_back(lerp(p, q, lineParam(door.a, door.b, p, q)));
            }
        }
        if (cut.size() != 2) {
            continue;
        }
        const auto along = [&](Vec3 point) {
            return ((point.x - door.a.x) * (door.b.x - door.a.x)
                    + (point.y - door.a.y) * (door.b.y - door.a.y)) / span;
        };
        float t0 = along(cut[0]);
        float t1 = along(cut[1]);
        if (t0 > t1) {
            std::swap(t0, t1);
            std::swap(cut[0], cut[1]);
        }
        if (t1 <= 0.01f || t0 >= span - 0.01f) {
            continue; // The line crosses here, but the door itself does not reach.
        }
        // The door's end points become vertices, so the part of the cut beyond the
        // door stays an ordinary open link.
        std::vector<Vec3> onLine{cut[0]};
        for (const float t : {0.0f, span}) {
            if (t > t0 + 0.01f && t < t1 - 0.01f) {
                onLine.push_back(lerp(cut[0], cut[1], (t - t0) / (t1 - t0)));
            }
        }
        onLine.push_back(cut[1]);
        // Both sides are convex. Fan each from a vertex off the line.
        const std::size_t before = added.size();
        for (const float sign : {1.0f, -1.0f}) {
            std::vector<Vec3> offLine;
            for (int i = 0; i < 3; ++i) {
                if (side[i] * sign > 0.0f) {
                    offLine.push_back(tri.v[i]);
                }
            }
            std::vector<Vec3> ring = offLine;
            ring.insert(ring.end(), onLine.begin(), onLine.end());
            // With two vertices off the line, the on-line points must continue from
            // the one the ring ends on: whichever order keeps the ring convex.
            if (offLine.size() == 2) {
                const float turn = triCross(ring[0], ring[1], ring[2]);
                const float close = triCross(ring[ring.size() - 2], ring.back(), ring[0]);
                if (turn * close < 0.0f) {
                    std::reverse(ring.begin() + 2, ring.end());
                }
            }
            for (std::size_t i = 1; i + 1 < ring.size(); ++i) {
                Tri piece;
                piece.v[0] = ring[0];
                piece.v[1] = ring[i];
                piece.v[2] = ring[i + 1];
                piece.room = tri.room;
                piece.fromCut = true;
                const float area = triCross(piece.v[0], piece.v[1], piece.v[2]);
                if (std::fabs(area) < kMinArea) {
                    continue;
                }
                if (area < 0.0f) {
                    std::swap(piece.v[1], piece.v[2]);
                }
                added.push_back(piece);
            }
        }
        if (added.size() > before) {
            tris_[index] = added.back();
            added.pop_back();
        }
    }
    tris_.insert(tris_.end(), added.begin(), added.end());
}

void NavMesh::buildLinks()
{
    struct Side {
        int tri;
        int edge;
    };
    std::unordered_map<EdgeKey, std::vector<Side>, EdgeKeyHash> edges;
    edges.reserve(tris_.size() * 3);
    for (std::size_t t = 0; t < tris_.size(); ++t) {
        for (int e = 0; e < 3; ++e) {
            edges[edgeKey(tris_[t].v[e], tris_[t].v[(e + 1) % 3])].push_back(
                {static_cast<int>(t), e});
        }
    }
    std::vector<std::vector<Link>> perTri(tris_.size());
    std::vector<Side> open;
    const auto addLink = [&](int from, int to, Vec3 p, Vec3 q) {
        Link link;
        link.to = to;
        // CCW triangles: the neighbour lies right of p -> q when `from` lies left.
        if (triCross(p, q, centroid(tris_[from])) < 0.0f) {
            std::swap(p, q);
        }
        link.p = p;
        link.q = q;
        perTri[from].push_back(link);
    };
    for (const auto& [key, sides] : edges) {
        if (sides.size() == 1) {
            open.push_back(sides.front());
            continue;
        }
        for (std::size_t i = 0; i < sides.size(); ++i) {
            for (std::size_t j = 0; j < sides.size(); ++j) {
                if (i != j) {
                    const Tri& tri = tris_[sides[i].tri];
                    addLink(sides[i].tri, sides[j].tri, tri.v[sides[i].edge],
                            tri.v[(sides[i].edge + 1) % 3]);
                }
            }
        }
    }
    // T-junctions: edges that overlap along a line but do not share both end points. A door
    // cut against an uncut neighbour makes them, and so do room seams whose two sides are
    // subdivided differently. Only edges the walkmesh marks as leading to another room
    // count there, so two rooms that merely touch back to back stay apart.
    for (std::size_t i = 0; i < open.size(); ++i) {
        const Tri& a = tris_[open[i].tri];
        const Vec3 a0 = a.v[open[i].edge];
        const Vec3 a1 = a.v[(open[i].edge + 1) % 3];
        const float lenA = length2D(a0, a1);
        if (lenA < 1.0e-4f) {
            continue;
        }
        for (std::size_t j = i + 1; j < open.size(); ++j) {
            const Tri& b = tris_[open[j].tri];
            const bool roomSeam = a.room != b.room && ((a.seamEdges >> open[i].edge) & 1u) != 0
                && ((b.seamEdges >> open[j].edge) & 1u) != 0;
            if (!a.fromCut && !b.fromCut && !roomSeam) {
                continue;
            }
            const Vec3 b0 = b.v[open[j].edge];
            const Vec3 b1 = b.v[(open[j].edge + 1) % 3];
            if (std::fabs(triCross(a0, a1, b0)) / lenA > kPortalMatch
                || std::fabs(triCross(a0, a1, b1)) / lenA > kPortalMatch) {
                continue;
            }
            float s0 = ((b0.x - a0.x) * (a1.x - a0.x) + (b0.y - a0.y) * (a1.y - a0.y)) / (lenA * lenA);
            float s1 = ((b1.x - a0.x) * (a1.x - a0.x) + (b1.y - a0.y) * (a1.y - a0.y)) / (lenA * lenA);
            if (s0 > s1) {
                std::swap(s0, s1);
            }
            const float lo = std::max(0.0f, s0);
            const float hi = std::min(1.0f, s1);
            if ((hi - lo) * lenA < kPortalMatch) {
                continue;
            }
            const Vec3 p = lerp(a0, a1, lo);
            const Vec3 q = lerp(a0, a1, hi);
            const Vec3 onB = closestOnSegment2D(p, b0, b1);
            if (std::fabs(onB.z - p.z) > 0.05f) {
                continue;
            }
            // The two triangles must lie on opposite sides of the shared line.
            if (triCross(a0, a1, centroid(a)) * triCross(a0, a1, centroid(b)) >= 0.0f) {
                continue;
            }
            addLink(open[i].tri, open[j].tri, p, q);
            addLink(open[j].tri, open[i].tri, p, q);
        }
    }
    // Mark every link that lies on a door's span.
    for (auto& list : perTri) {
        for (Link& link : list) {
            const float portal = length2D(link.p, link.q);
            for (std::size_t d = 0; d < doors_.size(); ++d) {
                const DoorLine& door = doors_[d];
                const float span = length2D(door.a, door.b);
                if (span < 1.0e-3f || portal < 1.0e-4f) {
                    continue;
                }
                if (std::fabs(triCross(door.a, door.b, link.p)) / span > kPortalMatch
                    || std::fabs(triCross(door.a, door.b, link.q)) / span > kPortalMatch) {
                    continue;
                }
                const Vec3 mid = lerp(link.p, link.q, 0.5f);
                if (mid.z < door.zMin || mid.z > door.zMax) {
                    continue;
                }
                const float along = ((mid.x - door.a.x) * (door.b.x - door.a.x)
                                     + (mid.y - door.a.y) * (door.b.y - door.a.y)) / span;
                if (along < 0.0f || along > span) {
                    continue;
                }
                if (link.doors[0] < 0) {
                    link.doors[0] = static_cast<int>(d);
                } else if (link.doors[1] < 0) {
                    link.doors[1] = static_cast<int>(d);
                }
            }
        }
    }
    links_.clear();
    for (std::size_t t = 0; t < tris_.size(); ++t) {
        tris_[t].firstLink = static_cast<int>(links_.size());
        tris_[t].linkCount = static_cast<int>(perTri[t].size());
        links_.insert(links_.end(), perTri[t].begin(), perTri[t].end());
    }
}

void NavMesh::buildGrid()
{
    grid_.clear();
    if (tris_.empty()) {
        return;
    }
    float minX = std::numeric_limits<float>::max();
    float minY = minX;
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = maxX;
    for (const Tri& tri : tris_) {
        for (const Vec3& v : tri.v) {
            minX = std::min(minX, v.x);
            minY = std::min(minY, v.y);
            maxX = std::max(maxX, v.x);
            maxY = std::max(maxY, v.y);
        }
    }
    gridMinX_ = minX;
    gridMinY_ = minY;
    gridW_ = static_cast<int>((maxX - minX) / kCellSize) + 1;
    gridH_ = static_cast<int>((maxY - minY) / kCellSize) + 1;
    grid_.assign(static_cast<std::size_t>(gridW_) * static_cast<std::size_t>(gridH_), {});
    for (std::size_t t = 0; t < tris_.size(); ++t) {
        const Tri& tri = tris_[t];
        const auto cell = [&](float value, float origin, int limit) {
            return std::clamp(static_cast<int>((value - origin) / kCellSize), 0, limit - 1);
        };
        const int x0 = cell(std::min({tri.v[0].x, tri.v[1].x, tri.v[2].x}), minX, gridW_);
        const int x1 = cell(std::max({tri.v[0].x, tri.v[1].x, tri.v[2].x}), minX, gridW_);
        const int y0 = cell(std::min({tri.v[0].y, tri.v[1].y, tri.v[2].y}), minY, gridH_);
        const int y1 = cell(std::max({tri.v[0].y, tri.v[1].y, tri.v[2].y}), minY, gridH_);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                grid_[static_cast<std::size_t>(y) * gridW_ + x].push_back(static_cast<int>(t));
            }
        }
    }
}

bool NavMesh::containsPoint(int tri, float x, float y) const
{
    const Tri& t = tris_[tri];
    const Vec3 p{x, y, 0.0f};
    for (int e = 0; e < 3; ++e) {
        const Vec3& a = t.v[e];
        const Vec3& b = t.v[(e + 1) % 3];
        const float len = length2D(a, b);
        if (triCross(a, b, p) < -kInsideEps * std::max(len, 1.0e-4f)) {
            return false;
        }
    }
    return true;
}

float NavMesh::height(int tri, float x, float y) const
{
    const Tri& t = tris_[tri];
    const Vec3 p{x, y, 0.0f};
    const float area = triCross(t.v[0], t.v[1], t.v[2]);
    if (std::fabs(area) < 1.0e-9f) {
        return t.v[0].z;
    }
    const float u = triCross(t.v[1], t.v[2], p) / area;
    const float v = triCross(t.v[2], t.v[0], p) / area;
    const float w = 1.0f - u - v;
    return u * t.v[0].z + v * t.v[1].z + w * t.v[2].z;
}

int NavMesh::locate(Vec3 point) const
{
    if (grid_.empty()) {
        return -1;
    }
    const int x = static_cast<int>(std::floor((point.x - gridMinX_) / kCellSize));
    const int y = static_cast<int>(std::floor((point.y - gridMinY_) / kCellSize));
    if (x < 0 || y < 0 || x >= gridW_ || y >= gridH_) {
        return -1;
    }
    int best = -1;
    float bestDelta = std::numeric_limits<float>::max();
    for (const int tri : grid_[static_cast<std::size_t>(y) * gridW_ + x]) {
        if (!containsPoint(tri, point.x, point.y)) {
            continue;
        }
        const float delta = std::fabs(height(tri, point.x, point.y) - point.z);
        if (delta < bestDelta) {
            bestDelta = delta;
            best = tri;
        }
    }
    return best;
}

int NavMesh::nearest(Vec3 point, Vec3& out) const
{
    int best = -1;
    float bestScore = std::numeric_limits<float>::max();
    for (std::size_t t = 0; t < tris_.size(); ++t) {
        const Tri& tri = tris_[t];
        Vec3 candidate = point;
        if (!containsPoint(static_cast<int>(t), point.x, point.y)) {
            float edgeBest = std::numeric_limits<float>::max();
            for (int e = 0; e < 3; ++e) {
                const Vec3 onEdge = closestOnSegment2D(point, tri.v[e], tri.v[(e + 1) % 3]);
                const float d = length2D(point, onEdge);
                if (d < edgeBest) {
                    edgeBest = d;
                    candidate = onEdge;
                }
            }
            // Just inside, so the result locates back onto this triangle.
            candidate = lerp(candidate, centroid(tri), 0.002f);
        }
        candidate.z = height(static_cast<int>(t), candidate.x, candidate.y);
        const float dz = (candidate.z - point.z) * 0.5f;
        const float d2 = length2D(point, candidate);
        const float score = d2 * d2 + dz * dz;
        if (score < bestScore) {
            bestScore = score;
            best = static_cast<int>(t);
            out = candidate;
        }
    }
    return best;
}

RayResult NavMesh::ray(Vec3 from, int fromTri, Vec3 to, const std::vector<char>& closed) const
{
    RayResult result;
    result.end = from;
    result.endTri = fromTri;
    if (fromTri < 0 || fromTri >= static_cast<int>(tris_.size())) {
        return result;
    }
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float len = std::hypot(dx, dy);
    int current = fromTri;
    int previous = -1;
    float reached = 0.0f;
    for (int guard = 0; guard < 2048; ++guard) {
        const Tri& tri = tris_[current];
        if (containsPoint(current, to.x, to.y)) {
            result.clear = true;
            result.end = {to.x, to.y, height(current, to.x, to.y)};
            result.endTri = current;
            return result;
        }
        if (len < 1.0e-6f) {
            result.clear = true;
            result.end = {from.x, from.y, height(current, from.x, from.y)};
            result.endTri = current;
            return result;
        }
        // Earliest edge the line leaves through, past the point already reached.
        float exitT = std::numeric_limits<float>::max();
        float edgeT[3];
        for (int e = 0; e < 3; ++e) {
            const Vec3& a = tri.v[e];
            const Vec3& b = tri.v[(e + 1) % 3];
            const float ex = b.x - a.x;
            const float ey = b.y - a.y;
            const float den = cross2(ex, ey, dx, dy);
            edgeT[e] = std::numeric_limits<float>::max();
            if (den < -1.0e-9f) {
                edgeT[e] = -cross2(ex, ey, from.x - a.x, from.y - a.y) / den;
                exitT = std::min(exitT, edgeT[e]);
            }
        }
        if (exitT == std::numeric_limits<float>::max()) {
            break;
        }
        const float t = std::clamp(exitT, reached, 1.0f);
        const Vec3 hit{from.x + dx * t, from.y + dy * t, 0.0f};
        // The neighbour across that edge. A line through a vertex leaves through
        // two edges at once, so every edge at this parameter is a candidate.
        int nextTri = -1;
        int blockedBy = -1;
        Vec3 ea = tri.v[0];
        Vec3 eb = tri.v[1];
        float edgeLen = 1.0e-5f;
        const float tieEps = 2.0e-3f / std::max(len, 1.0e-3f);
        for (int e = 0; e < 3 && nextTri < 0; ++e) {
            if (edgeT[e] > exitT + tieEps) {
                continue;
            }
            const Vec3& a = tri.v[e];
            const Vec3& b = tri.v[(e + 1) % 3];
            const float thisLen = std::max(length2D(a, b), 1.0e-5f);
            if (edgeT[e] == exitT) {
                ea = a;
                eb = b;
                edgeLen = thisLen;
            }
            for (int l = 0; l < tri.linkCount; ++l) {
                const Link& link = links_[tri.firstLink + l];
                if (link.to == previous) {
                    continue;
                }
                if (std::fabs(triCross(a, b, link.p)) / thisLen > kPortalMatch
                    || std::fabs(triCross(a, b, link.q)) / thisLen > kPortalMatch) {
                    continue;
                }
                if (length2D(closestOnSegment2D(hit, link.p, link.q), hit) > 0.003f) {
                    continue;
                }
                if (closedAny(link, closed)) {
                    blockedBy = link.doors[0] >= 0 && closed[link.doors[0]] != 0 ? link.doors[0]
                                                                                  : link.doors[1];
                    continue;
                }
                nextTri = link.to;
                break;
            }
        }
        if (nextTri < 0) {
            // Stopped by the edge. End just inside so the end point locates here.
            const float back = len > 1.0e-5f ? 0.01f / len : 0.0f;
            const float stop = std::max(0.0f, t - back);
            Vec3 end{from.x + dx * stop, from.y + dy * stop, 0.0f};
            if (!containsPoint(current, end.x, end.y)) {
                end = lerp(hit, centroid(tri), 0.01f);
            }
            end.z = height(current, end.x, end.y);
            result.end = end;
            result.endTri = current;
            result.hitDoor = blockedBy;
            result.edge = {(eb.x - ea.x) / edgeLen, (eb.y - ea.y) / edgeLen, 0.0f};
            return result;
        }
        previous = current;
        current = nextTri;
        reached = t;
    }
    result.end = {from.x + dx * reached, from.y + dy * reached, 0.0f};
    result.end.z = height(current, result.end.x, result.end.y);
    result.endTri = current;
    return result;
}

namespace {

// Distance in plan from a point to a segment.
float distanceToSegment(Vec3 point, Vec3 a, Vec3 b)
{
    return length2D(point, closestOnSegment2D(point, a, b));
}

// The first parameter t in [0, 1] along from -> to at which a body of `radius` touches the wall
// segment, or 2 when it does not. A body already inside the radius may only move parallel or away.
float wallTouch(Vec3 from, Vec3 to, Vec3 a, Vec3 b, float radius)
{
    const float span = length2D(a, b);
    if (span < 1.0e-4f) {
        return 2.0f;
    }
    const float startDistance = distanceToSegment(from, a, b);
    if (startDistance < radius) {
        return distanceToSegment(to, a, b) >= startDistance - 1.0e-4f ? 2.0f : 0.0f;
    }
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    float best = 2.0f;
    // The two lines offset by the radius, over the wall's length.
    const float ux = (b.x - a.x) / span;
    const float uy = (b.y - a.y) / span;
    const float nx = -uy;
    const float ny = ux;
    const float side0 = nx * (from.x - a.x) + ny * (from.y - a.y);
    const float sideStep = nx * dx + ny * dy;
    if (std::fabs(sideStep) > 1.0e-9f) {
        for (const float offset : {radius, -radius}) {
            const float t = (offset - side0) / sideStep;
            if (t < 0.0f || t > 1.0f) {
                continue;
            }
            const float along = ux * (from.x + dx * t - a.x) + uy * (from.y + dy * t - a.y);
            if (along >= 0.0f && along <= span) {
                best = std::min(best, t);
            }
        }
    }
    // The two end caps.
    for (const Vec3& centre : {a, b}) {
        const float fx = from.x - centre.x;
        const float fy = from.y - centre.y;
        const float qa = dx * dx + dy * dy;
        const float qb = 2.0f * (fx * dx + fy * dy);
        const float qc = fx * fx + fy * fy - radius * radius;
        const float disc = qb * qb - 4.0f * qa * qc;
        if (qa < 1.0e-12f || disc < 0.0f) {
            continue;
        }
        const float t = (-qb - std::sqrt(disc)) / (2.0f * qa);
        if (t >= 0.0f && t <= 1.0f) {
            best = std::min(best, t);
        }
    }
    return best;
}

} // namespace

RayResult NavMesh::stepAgainstWalls(Vec3 from, int fromTri, Vec3 to, float radius,
                                    const std::vector<char>& closed, bool slide) const
{
    RayResult result = ray(from, fromTri, to, closed);
    if (walls_.empty() || result.endTri < 0) {
        return result;
    }
    const Vec3 end = result.end;
    const float length = length2D(from, end);
    if (length < 1.0e-6f) {
        return result;
    }
    float stop = 2.0f;
    const DoorLine* hit = nullptr;
    for (const DoorLine& wall : walls_) {
        if (from.z < wall.zMin - 0.5f || from.z > wall.zMax + 0.1f) {
            continue; // Another floor, or a face above the head.
        }
        const float t = wallTouch(from, end, wall.a, wall.b, radius);
        if (t <= 1.0f && t < stop) {
            stop = t;
            hit = &wall;
        }
    }
    if (hit == nullptr) {
        return result;
    }
    // Stop a hair short, so the next step starts outside the radius.
    const float t = std::max(0.0f, stop - 1.0e-3f / length);
    Vec3 stopped{from.x + (end.x - from.x) * t, from.y + (end.y - from.y) * t, 0.0f};
    const int stoppedTri = locate({stopped.x, stopped.y, from.z});
    stopped.z = stoppedTri >= 0 ? height(stoppedTri, stopped.x, stopped.y) : from.z;
    const float wallSpan = std::max(length2D(hit->a, hit->b), 1.0e-5f);
    const Vec3 along{(hit->b.x - hit->a.x) / wallSpan, (hit->b.y - hit->a.y) / wallSpan, 0.0f};
    RayResult blocked;
    blocked.clear = false;
    blocked.end = stoppedTri >= 0 ? stopped : from;
    blocked.endTri = stoppedTri >= 0 ? stoppedTri : fromTri;
    blocked.edge = along;
    if (slide && stoppedTri >= 0) {
        const float rest = length2D(from, to) - length2D(from, stopped);
        const float carry = ((to.x - from.x) * along.x + (to.y - from.y) * along.y)
            / std::max(length2D(from, to), 1.0e-5f);
        if (rest > 1.0e-4f && std::fabs(carry) > 1.0e-3f) {
            const Vec3 slid{stopped.x + along.x * carry * rest, stopped.y + along.y * carry * rest, stopped.z};
            const RayResult second = stepAgainstWalls(blocked.end, blocked.endTri, slid, radius, closed, false);
            blocked.end = second.end;
            blocked.endTri = second.endTri;
        }
    }
    return blocked;
}

bool NavMesh::search(int start, int goal, Vec3 from, Vec3 to, float radius,
                     const std::vector<char>* closed, std::vector<int>& corridor,
                     std::vector<int>& viaLink, std::vector<char>* reached) const
{
    struct Node {
        float g = std::numeric_limits<float>::max();
        int parent = -1;
        int link = -1;
        Vec3 entry;
        bool done = false;
    };
    std::vector<Node> nodes(tris_.size());
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    nodes[start].g = 0.0f;
    nodes[start].entry = from;
    open.push({length3D(from, to), start});
    while (!open.empty()) {
        const int current = open.top().second;
        open.pop();
        Node& node = nodes[current];
        if (node.done) {
            continue;
        }
        node.done = true;
        if (reached != nullptr) {
            (*reached)[current] = 1;
        }
        if (current == goal) {
            corridor.clear();
            viaLink.clear();
            for (int at = goal; at != -1; at = nodes[at].parent) {
                corridor.push_back(at);
                viaLink.push_back(nodes[at].link);
            }
            std::reverse(corridor.begin(), corridor.end());
            std::reverse(viaLink.begin(), viaLink.end());
            return true;
        }
        const Tri& tri = tris_[current];
        for (int l = 0; l < tri.linkCount; ++l) {
            const int linkIndex = tri.firstLink + l;
            const Link& link = links_[linkIndex];
            if (nodes[link.to].done || (closed != nullptr && closedAny(link, *closed))) {
                continue;
            }
            // Cost through the point of the portal nearest where this triangle was
            // entered, kept off the portal's ends by the actor's radius.
            const float portal = length2D(link.p, link.q);
            const float margin = portal > 1.0e-5f ? std::min(radius, portal * 0.45f) / portal : 0.0f;
            const Vec3 entry = closestOnSegment2D(node.entry, lerp(link.p, link.q, margin),
                                                  lerp(link.p, link.q, 1.0f - margin));
            const float g = node.g + length3D(node.entry, entry);
            if (g < nodes[link.to].g) {
                Node& next = nodes[link.to];
                next.g = g;
                next.parent = current;
                next.link = linkIndex;
                next.entry = entry;
                open.push({g + length3D(entry, to), link.to});
            }
        }
    }
    return false;
}

// Simple stupid funnel over the corridor's portals, shrunk by the actor's radius.
void NavMesh::funnel(const std::vector<int>& corridor, const std::vector<int>& viaLink, Vec3 from,
                     Vec3 to, float radius, PathResult& out) const
{
    struct Portal {
        Vec3 left;
        Vec3 right;
        int tri;
    };
    std::vector<Portal> portals;
    portals.reserve(corridor.size() + 1);
    portals.push_back({from, from, corridor.front()});
    for (std::size_t i = 1; i < corridor.size(); ++i) {
        const Link& link = links_[viaLink[i]];
        const float portal = length2D(link.p, link.q);
        const float margin = portal > 1.0e-5f ? std::min(radius, portal * 0.45f) / portal : 0.0f;
        // Link::to lies right of p -> q, so heading through it q is on the left.
        portals.push_back({lerp(link.p, link.q, 1.0f - margin), lerp(link.p, link.q, margin),
                           corridor[i - 1]});
    }
    portals.push_back({to, to, corridor.back()});

    std::vector<std::pair<Vec3, int>> points;
    points.push_back({from, corridor.front()});
    Vec3 apex = from;
    Vec3 left = from;
    Vec3 right = from;
    std::size_t leftIndex = 0;
    std::size_t rightIndex = 0;
    for (std::size_t i = 1; i < portals.size(); ++i) {
        const Vec3 newLeft = portals[i].left;
        const Vec3 newRight = portals[i].right;
        if (triCross(apex, right, newRight) >= 0.0f) {
            if (length2D(apex, right) < 1.0e-6f || triCross(apex, left, newRight) < 0.0f) {
                right = newRight;
                rightIndex = i;
            } else {
                points.push_back({left, portals[leftIndex].tri});
                apex = left;
                i = leftIndex;
                left = apex;
                right = apex;
                rightIndex = i;
                continue;
            }
        }
        if (triCross(apex, left, newLeft) <= 0.0f) {
            if (length2D(apex, left) < 1.0e-6f || triCross(apex, right, newLeft) > 0.0f) {
                left = newLeft;
                leftIndex = i;
            } else {
                points.push_back({right, portals[rightIndex].tri});
                apex = right;
                i = rightIndex;
                left = apex;
                right = apex;
                leftIndex = i;
                continue;
            }
        }
    }
    points.push_back({to, corridor.back()});

    out.points.clear();
    for (const auto& [point, tri] : points) {
        Vec3 p = point;
        p.z = height(tri, p.x, p.y);
        if (out.points.empty() || length2D(out.points.back(), p) > 1.0e-3f) {
            out.points.push_back(p);
        }
    }
    if (out.points.size() == 1) {
        out.points.push_back(out.points.front());
    }
    out.corridor = corridor;
    out.doorsCrossed.clear();
    for (std::size_t i = 1; i < viaLink.size(); ++i) {
        for (const int door : links_[viaLink[i]].doors) {
            if (door >= 0) {
                out.doorsCrossed.push_back(door);
            }
        }
    }
    out.length = 0.0f;
    for (std::size_t i = 1; i < out.points.size(); ++i) {
        out.length += length3D(out.points[i - 1], out.points[i]);
    }
}

PathResult NavMesh::findPath(Vec3 from, Vec3 to, float radius,
                             const std::vector<char>& closed) const
{
    PathResult result;
    if (tris_.empty()) {
        return result;
    }
    int start = locate(from);
    if (start < 0) {
        Vec3 snapped;
        start = nearest(from, snapped);
        from = snapped;
    } else {
        from.z = height(start, from.x, from.y);
    }
    int goal = locate(to);
    bool adjusted = false;
    Vec3 target = to;
    if (goal < 0 || std::fabs(height(goal, to.x, to.y) - to.z) > 4.0f) {
        const int snappedGoal = nearest(to, target);
        adjusted = length2D(target, to) > 0.05f || goal < 0;
        goal = snappedGoal;
    } else {
        target.z = height(goal, to.x, to.y);
    }

    std::vector<int> corridor;
    std::vector<int> viaLink;
    std::vector<char> reached(tris_.size(), 0);
    if (search(start, goal, from, target, radius, &closed, corridor, viaLink, &reached)) {
        result.status = adjusted ? PathStatus::Adjusted : PathStatus::Reached;
        funnel(corridor, viaLink, from, target, radius, result);
        return result;
    }
    // Would it connect if the doors were open?
    if (search(start, goal, from, target, radius, nullptr, corridor, viaLink, nullptr)) {
        for (std::size_t i = 1; i < corridor.size(); ++i) {
            const Link& link = links_[viaLink[i]];
            if (!closedAny(link, closed)) {
                continue;
            }
            result.status = PathStatus::DoorBlocked;
            result.blockingDoor = link.doors[0] >= 0 && closed[link.doors[0]] != 0 ? link.doors[0]
                                                                                    : link.doors[1];
            const int before = corridor[i - 1];
            const Vec3 mid = lerp(link.p, link.q, 0.5f);
            const Vec3 toward = centroid(tris_[before]);
            const float gap = length2D(mid, toward);
            Vec3 stop = lerp(mid, toward, gap > 1.0e-4f ? std::min(1.0f, kDoorBackoff / gap) : 0.0f);
            stop.z = height(before, stop.x, stop.y);
            corridor.resize(i);
            viaLink.resize(i);
            // Already standing at the door: stay put rather than step back to the mark.
            if (i == 1 && length2D(from, mid) <= kDoorBackoff) {
                stop = from;
            }
            funnel(corridor, viaLink, from, stop, radius, result);
            return result;
        }
    }
    // Nothing connects: the closest point the actor can reach.
    Vec3 closest = from;
    int closestTri = start;
    float closestScore = std::numeric_limits<float>::max();
    for (std::size_t t = 0; t < tris_.size(); ++t) {
        if (reached[t] == 0) {
            continue;
        }
        const Tri& tri = tris_[t];
        Vec3 candidate = to;
        if (!containsPoint(static_cast<int>(t), to.x, to.y)) {
            float edgeBest = std::numeric_limits<float>::max();
            for (int e = 0; e < 3; ++e) {
                const Vec3 onEdge = closestOnSegment2D(to, tri.v[e], tri.v[(e + 1) % 3]);
                const float d = length2D(to, onEdge);
                if (d < edgeBest) {
                    edgeBest = d;
                    candidate = onEdge;
                }
            }
            candidate = lerp(candidate, centroid(tri), 0.002f);
        }
        candidate.z = height(static_cast<int>(t), candidate.x, candidate.y);
        const float d = length2D(to, candidate);
        const float dz = (candidate.z - to.z) * 0.5f;
        const float score = d * d + dz * dz;
        if (score < closestScore) {
            closestScore = score;
            closest = candidate;
            closestTri = static_cast<int>(t);
        }
    }
    // An off-mesh target has no walkable spot of its own, so the closest reachable
    // point is the answer. An on-mesh one is a different floor island: report it.
    result.status = adjusted ? PathStatus::Adjusted : PathStatus::Unreachable;
    if (search(start, closestTri, from, closest, radius, &closed, corridor, viaLink, nullptr)) {
        funnel(corridor, viaLink, from, closest, radius, result);
    } else {
        result.points = {from, from};
    }
    return result;
}


// ---- the detour hexagon (retail 005cf1f0, 005cfc70, 005d0160, 005cfae0, 005cf7a0, 005cf650) ------------------

Hexagon makeHexagon(Vec3 centre, float walkerFacing, float apothem)
{
    // 005cf1f0 takes the walker's facing vector negated, normalised and scaled by apothem / 0.866 (0074c454),
    // then turns it by -60 degrees (0xbf860a92) for each vertex after the first.
    const float reach = apothem / 0.8660254f;
    const float baseX = -std::cos(walkerFacing);
    const float baseY = -std::sin(walkerFacing);
    Hexagon hex;
    for (std::size_t i = 0; i < hex.vertex.size(); ++i) {
        const float turn = -static_cast<float>(i) * 1.04719755f;
        const float c = std::cos(turn);
        const float s = std::sin(turn);
        hex.vertex[i] = {centre.x + (baseX * c - baseY * s) * reach, centre.y + (baseX * s + baseY * c) * reach,
                         centre.z};
    }
    return hex;
}

namespace {

// The edge's normal into the hexagon (vertices run clockwise, so the inside lies to the right).
inline void edgeNormal(const Hexagon& hex, std::size_t edge, float& nx, float& ny)
{
    const Vec3 a = hex.vertex[edge];
    const Vec3 b = hex.vertex[(edge + 1) % 6];
    const float ex = b.x - a.x;
    const float ey = b.y - a.y;
    const float length = std::hypot(ex, ey);
    nx = length > 0.0f ? ey / length : 0.0f;
    ny = length > 0.0f ? -ex / length : 0.0f;
}

} // namespace

bool Hexagon::contains(Vec3 point) const
{
    for (std::size_t i = 0; i < 6; ++i) {
        float nx = 0.0f;
        float ny = 0.0f;
        edgeNormal(*this, i, nx, ny);
        if (nx * (point.x - vertex[i].x) + ny * (point.y - vertex[i].y) < 0.0f) {
            return false;
        }
    }
    return true;
}

std::vector<HexCrossing> Hexagon::crossings(const std::vector<Vec3>& polyline) const
{
    std::vector<HexCrossing> found;
    for (std::size_t k = 1; k < polyline.size(); ++k) {
        const Vec3 a = polyline[k - 1];
        const float dx = polyline[k].x - a.x;
        const float dy = polyline[k].y - a.y;
        float enter = 0.0f;
        float leave = 1.0f;
        int enterEdge = -1;
        int leaveEdge = -1;
        bool missed = false;
        for (std::size_t e = 0; e < 6 && !missed; ++e) {
            float nx = 0.0f;
            float ny = 0.0f;
            edgeNormal(*this, e, nx, ny);
            const float inside = nx * (a.x - vertex[e].x) + ny * (a.y - vertex[e].y);
            const float rate = nx * dx + ny * dy;
            if (std::fabs(rate) < 1.0e-9f) {
                missed = inside < 0.0f;
                continue;
            }
            const float t = -inside / rate;
            if (rate > 0.0f) {
                if (t > enter) {
                    enter = t;
                    enterEdge = static_cast<int>(e);
                }
            } else if (t < leave) {
                leave = t;
                leaveEdge = static_cast<int>(e);
            }
            missed = enter > leave;
        }
        if (missed) {
            continue;
        }
        if (enterEdge >= 0 && enter > 1.0e-6f) {
            found.push_back({k, enterEdge, {a.x + dx * enter, a.y + dy * enter, a.z}});
        }
        if (leaveEdge >= 0 && leave < 1.0f - 1.0e-6f) {
            found.push_back({k, leaveEdge, {a.x + dx * leave, a.y + dy * leave, a.z}});
        }
    }
    return found;
}

int Hexagon::nearestVertex(Vec3 point) const
{
    int best = 0;
    float bestDistance = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < 6; ++i) {
        const float d = (vertex[i].x - point.x) * (vertex[i].x - point.x)
            + (vertex[i].y - point.y) * (vertex[i].y - point.y);
        if (d < bestDistance) {
            bestDistance = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

std::vector<Vec3> Hexagon::arc(const HexCrossing& entry, const HexCrossing& exit, bool forward) const
{
    std::vector<Vec3> points;
    points.push_back(entry.point);
    if (entry.edge == exit.edge) {
        points.push_back(exit.point);
        return points;
    }
    if (forward) {
        int e = (entry.edge + 1) % 6;
        points.push_back(vertex[static_cast<std::size_t>(e)]);
        while (e != exit.edge) {
            e = (e + 1) % 6;
            points.push_back(vertex[static_cast<std::size_t>(e)]);
        }
    } else {
        int e = entry.edge;
        points.push_back(vertex[static_cast<std::size_t>(e)]);
        while (e != (exit.edge + 1) % 6) {
            e = (e + 5) % 6;
            points.push_back(vertex[static_cast<std::size_t>(e)]);
        }
    }
    points.push_back(exit.point);
    return points;
}

} // namespace ke::nav

// ---- GameSession: actors on the mesh ------------------------------------------

namespace ke {

namespace {

using kotor::Vec3;
using nav::length2D;

constexpr float kPi = 3.14159265358979323846f;
// Retail defaults 004ed6e0 when appearance.2da has no value.
constexpr float kDefaultPersonalSpace = 0.6f;
// The PC without an avatar has no appearance row here. Human rows all hold this.
constexpr float kPlayerPersonalSpace = 0.35f;
// Retail 00516630: the sixth bump ends a walk; a second bump on the same creature within 1e-5 (0074 7504) of
// where it stood stops it too. 005d0840 adds 0.2 (007a2274) to the two bodies' radii for the hexagon.
constexpr int kMaxBumps = 6;
constexpr float kSameSpot = 9.99999747e-06f;
constexpr float kDetourMargin = 0.2f;
constexpr float kRouteDriftLimit = 1.0f;
constexpr float kRepathTargetMove = 0.25f;
constexpr float kFormationArrive = 0.35f;
// A caller that starts a little off the mesh walks back on; farther than this, a
// jump's settle leaves it where the script put it.
constexpr float kSettleSnap = 3.0f;

float parseCell(const std::string& text, float fallback)
{
    try {
        return text.empty() ? fallback : std::stof(text);
    } catch (const std::exception&) {
        return fallback;
    }
}

int columnNoCase(const kotor::TwoDa& table, const std::string& wanted)
{
    for (std::size_t i = 0; i < table.columnCount(); ++i) {
        std::string name = table.column(i);
        for (char& ch : name) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (name == wanted) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<nav::DoorLine> buildDoorLines(const kotor::Scene& scene, kotor::Install& install)
{
    std::vector<nav::DoorLine> lines;
    for (std::size_t i = 0; i < scene.entities.size(); ++i) {
        const kotor::Entity& door = scene.entities[i];
        if (door.kind != kotor::EntityKind::Door) {
            continue;
        }
        float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f, maxZ = 0.0f;
        bool found = false;
        bool fromDwk = false;
        std::string model = door.model;
        for (char& ch : model) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        const auto extent = [&](float x, float y, float z) {
            if (!found) {
                minX = maxX = x;
                minY = maxY = y;
                maxZ = z;
                found = true;
            }
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
            maxZ = std::max(maxZ, z);
        };
        // <model>0.dwk is the closed state (docs/recon/12): X wide, Y thin.
        if (!model.empty()) {
            if (auto data = install.find(model + "0", 2052); data.has_value()) {
                try {
                    const kotor::Wok dwk = kotor::parseWok(std::move(*data), model + "0.dwk");
                    for (const Vec3& v : dwk.vertices) {
                        extent(v.x, v.y, v.z);
                    }
                    fromDwk = found;
                } catch (const kotor::FormatError&) {
                    found = false;
                }
            }
        }
        // Fallback, no retail evidence behind it: the rendered model's extents.
        if (!found) {
            for (const kotor::EntityMesh& mesh : scene.entityMeshes) {
                if (mesh.entity == i) {
                    for (const kotor::SceneVertex& v : mesh.vertices) {
                        extent(v.position.x, v.position.y, v.position.z);
                    }
                }
            }
        }
        if (!found) {
            continue; // No shape at all: this door cannot block.
        }
        const bool xWide = maxX - minX >= maxY - minY;
        const float midY = (minY + maxY) * 0.5f;
        const float midX = (minX + maxX) * 0.5f;
        const Vec3 localA = xWide ? Vec3{minX, midY, 0.0f} : Vec3{midX, minY, 0.0f};
        const Vec3 localB = xWide ? Vec3{maxX, midY, 0.0f} : Vec3{midX, maxY, 0.0f};
        const float c = std::cos(door.bearing);
        const float s = std::sin(door.bearing);
        nav::DoorLine line;
        line.entity = i;
        line.a = {door.position.x + c * localA.x - s * localA.y,
                  door.position.y + s * localA.x + c * localA.y, door.position.z};
        line.b = {door.position.x + c * localB.x - s * localB.y,
                  door.position.y + s * localB.x + c * localB.y, door.position.z};
        line.zMin = door.position.z - 0.5f;
        line.zMax = door.position.z + std::max(maxZ, 2.0f);
        line.fromDwk = fromDwk;
        lines.push_back(line);
    }
    return lines;
}

// The non-walkable faces of every placeable's walkmesh (<model>.pwk, resource 2053), as wall lines in
// world space. Retail 00506650 NoNonWalkPolysDetailed tests a creature's step (a segment with the
// creature's radius) against the non-walk polygons of the area's static objects; a placeable's own
// collision mesh is built from its PWK by 00585350 NoNonWalkPolys. Walkable PWK faces (surfacemat
// walk = 1, a seat or a step) do not block. A face's edges that are not vertical are the wall lines
// on the ground; vertical edges project to a point and are skipped.
std::vector<nav::DoorLine> buildPlaceableWalls(const kotor::Scene& scene, kotor::Install& install)
{
    std::vector<nav::DoorLine> walls;
    std::vector<char> surfaceWalk;
    if (auto table = install.find("surfacemat", 2017); table.has_value()) {
        const kotor::TwoDa surfaces = kotor::TwoDa::parse(std::move(*table), "surfacemat.2da");
        const int walk = surfaces.columnIndex("walk");
        for (std::size_t row = 0; row < surfaces.rowCount() && walk >= 0; ++row) {
            surfaceWalk.push_back(surfaces.cell(row, static_cast<std::size_t>(walk)) == "1" ? 1 : 0);
        }
    }
    std::unordered_map<std::string, std::shared_ptr<const kotor::Wok>> cache;
    std::set<std::tuple<int, int, int, int, int>> seen;
    for (std::size_t i = 0; i < scene.entities.size(); ++i) {
        const kotor::Entity& placeable = scene.entities[i];
        if (placeable.kind != kotor::EntityKind::Placeable || placeable.dead || placeable.model.empty()) {
            continue;
        }
        std::string model = placeable.model;
        for (char& ch : model) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        auto found = cache.find(model);
        if (found == cache.end()) {
            std::shared_ptr<const kotor::Wok> parsed;
            if (auto data = install.find(model, 2053); data.has_value()) {
                try {
                    parsed = std::make_shared<const kotor::Wok>(kotor::parseWok(std::move(*data), model + ".pwk"));
                } catch (const kotor::FormatError&) {
                }
            }
            found = cache.emplace(model, std::move(parsed)).first;
        }
        if (!found->second) {
            continue;
        }
        const kotor::Wok& pwk = *found->second;
        const float c = std::cos(placeable.bearing);
        const float s = std::sin(placeable.bearing);
        const auto world = [&](const Vec3& v) {
            return Vec3{placeable.position.x + c * v.x - s * v.y, placeable.position.y + s * v.x + c * v.y,
                        placeable.position.z + v.z};
        };
        float zLo = std::numeric_limits<float>::max();
        float zHi = std::numeric_limits<float>::lowest();
        for (const Vec3& v : pwk.vertices) {
            zLo = std::min(zLo, v.z);
            zHi = std::max(zHi, v.z);
        }
        for (const kotor::WokFace& face : pwk.faces) {
            if (face.surface < surfaceWalk.size() ? surfaceWalk[face.surface] != 0 : face.floor) {
                continue;
            }
            const std::uint32_t index[3] = {face.a, face.b, face.c};
            for (int e = 0; e < 3; ++e) {
                const std::uint32_t ia = index[e];
                const std::uint32_t ib = index[(e + 1) % 3];
                if (ia >= pwk.vertices.size() || ib >= pwk.vertices.size()) {
                    continue;
                }
                const Vec3 a = world(pwk.vertices[ia]);
                const Vec3 b = world(pwk.vertices[ib]);
                if (nav::length2D(a, b) < 0.05f) {
                    continue;
                }
                const auto key = [](const Vec3& v) {
                    return std::pair<int, int>{static_cast<int>(std::lround(v.x * 100.0f)),
                                               static_cast<int>(std::lround(v.y * 100.0f))};
                };
                auto ka = key(a);
                auto kb = key(b);
                if (kb < ka) {
                    std::swap(ka, kb);
                }
                if (!seen.emplace(ka.first, ka.second, kb.first, kb.second, static_cast<int>(i)).second) {
                    continue;
                }
                nav::DoorLine line;
                line.entity = i;
                line.a = a;
                line.b = b;
                line.zMin = placeable.position.z + zLo - 0.3f;
                line.zMax = placeable.position.z + zHi + 0.3f;
                line.fromDwk = true;
                walls.push_back(line);
            }
        }
    }
    return walls;
}

// A door ordered open whose opening clip has not finished. Its open state is committed when the clip reports
// done (client event handler 00683f00 -> CSWSDoor::MoveToNextOpenState 00589680 copies the target state at
// +0x2cd to the live one at +0x2cc), so until then it blocks the way as a closed door does.
bool doorStillOpening(const kotor::Scene& scene, std::size_t index)
{
    if (index >= scene.entities.size() || !scene.entities[index].open) {
        return false;
    }
    for (const kotor::EntityMesh& mesh : scene.entityMeshes) {
        if (mesh.entity == index && !mesh.fx && mesh.model && !mesh.animationLoop
            && mesh.animation.rfind("opening", 0) == 0) {
            return mesh.animationTime < mesh.model->animationDuration(mesh.animation);
        }
    }
    return false;
}

} // namespace

// The walkmesh triangles retail's GetObstructionFactor 0050ccf0 can meet besides the room walkmesh: the area's
// door (closed state, <model>0.dwk) and placeable (<model>.pwk) walkmeshes, 00506650 / 004bbe20, rotated by the
// object's bearing about z and moved to its position.
const std::vector<GameSession::SoundFacet>& GameSession::soundObjectFacets()
{
    const std::string key = module_ + "|" + std::to_string(scene_.entities.size());
    if (key == soundFacetKey_) {
        return soundFacets_;
    }
    soundFacetKey_ = key;
    soundFacets_.clear();
    std::unordered_map<std::string, std::shared_ptr<const kotor::Wok>> cache;
    for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
        const kotor::Entity& entity = scene_.entities[i];
        const bool door = entity.kind == kotor::EntityKind::Door;
        if ((!door && entity.kind != kotor::EntityKind::Placeable) || entity.model.empty()) {
            continue;
        }
        std::string model = entity.model;
        for (char& ch : model) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        const std::string name = door ? model + "0" : model;
        auto found = cache.find(name);
        if (found == cache.end()) {
            std::shared_ptr<const kotor::Wok> parsed;
            if (auto data = install_.find(name, door ? 2052 : 2053); data.has_value()) {
                try {
                    parsed = std::make_shared<const kotor::Wok>(
                        kotor::parseWok(std::move(*data), name + (door ? ".dwk" : ".pwk")));
                } catch (const kotor::FormatError&) {
                }
            }
            found = cache.emplace(name, std::move(parsed)).first;
        }
        if (!found->second) {
            continue;
        }
        const kotor::Wok& wok = *found->second;
        const float c = std::cos(entity.bearing);
        const float s = std::sin(entity.bearing);
        const auto world = [&](const Vec3& v) {
            return Vec3{entity.position.x + c * v.x - s * v.y, entity.position.y + s * v.x + c * v.y,
                        entity.position.z + v.z};
        };
        for (const kotor::WokFace& face : wok.faces) {
            if (face.a >= wok.vertices.size() || face.b >= wok.vertices.size() || face.c >= wok.vertices.size()) {
                continue;
            }
            SoundFacet facet;
            facet.a = world(wok.vertices[face.a]);
            facet.b = world(wok.vertices[face.b]);
            facet.c = world(wok.vertices[face.c]);
            facet.entity = static_cast<std::uint32_t>(i);
            facet.door = door;
            soundFacets_.push_back(facet);
        }
    }
    return soundFacets_;
}

nav::NavMesh* GameSession::navMesh()
{
    if (!nav_) {
        nav_ = std::make_unique<nav::Runtime>();
    }
    if (!nav_->mesh) {
        std::shared_ptr<nav::NavMesh>& cached = nav_->cache[module_];
        if (!cached) {
            cached = std::make_shared<nav::NavMesh>(scene_, buildDoorLines(scene_, install_),
                                                     buildPlaceableWalls(scene_, install_));
        }
        nav_->mesh = cached;
        nav_->closed.assign(nav_->mesh->doors().size(), 0);
        ++nav_->doorVersion;
    }
    const std::vector<nav::DoorLine>& doors = nav_->mesh->doors();
    for (std::size_t d = 0; d < doors.size(); ++d) {
        const std::size_t index = doors[d].entity;
        const bool opening = doorStillOpening(scene_, index);
        const bool closed = index < scene_.entities.size() && (!scene_.entities[index].open || opening)
            && !scene_.entities[index].dead;
        if ((nav_->closed[d] != 0) != closed) {
            if (index < scene_.entities.size() && scene_.entities[index].open) {
                // The open state commits when the opening clip is done: the times a walker waits on.
                logInfo(concat("door ", scene_.entities[index].tag, closed ? ": opening clip runs, blocks at "
                                                                          : ": open state committed at ", elapsed_));
            }
            nav_->closed[d] = closed ? 1 : 0;
            ++nav_->doorVersion;
        }
    }
    return nav_->mesh.get();
}

void GameSession::loadMovementTables()
{
    if (!nav_) {
        nav_ = std::make_unique<nav::Runtime>();
    }
    if (nav_->tablesLoaded) {
        return;
    }
    nav_->tablesLoaded = true;
    if (auto data = install_.find("creaturespeed", 2017); data.has_value()) {
        const kotor::TwoDa table = kotor::TwoDa::parse(std::move(*data), "creaturespeed.2da");
        const int walk = columnNoCase(table, "walkrate");
        const int run = columnNoCase(table, "runrate");
        const int name = columnNoCase(table, "2daname");
        for (std::size_t row = 0; row < table.rowCount() && walk >= 0 && run >= 0 && name >= 0; ++row) {
            nav_->walkRates.push_back(parseCell(table.cell(row, static_cast<std::size_t>(walk)), 0.0f));
            nav_->runRates.push_back(parseCell(table.cell(row, static_cast<std::size_t>(run)), 0.0f));
            nav_->rateNames.push_back(table.cell(row, static_cast<std::size_t>(name)));
        }
    }
    if (auto data = install_.find("appearance", 2017); data.has_value()) {
        const kotor::TwoDa table = kotor::TwoDa::parse(std::move(*data), "appearance.2da");
        const int rate = columnNoCase(table, "moverate");
        const int personal = columnNoCase(table, "perspace");
        const int creature = columnNoCase(table, "creperspace");
        const int height = columnNoCase(table, "height");
        const int driveAccel = columnNoCase(table, "driveaccl");
        const int driveMax = columnNoCase(table, "drivemaxspeed");
        const int driveWalk = columnNoCase(table, "driveanimwalk");
        const int driveRun = columnNoCase(table, "driveanimrun");
        for (std::size_t row = 0; row < table.rowCount(); ++row) {
            const auto id = static_cast<std::uint32_t>(row);
            if (driveAccel >= 0 && driveMax >= 0 && driveWalk >= 0 && driveRun >= 0) {
                nav::Runtime::DriveRow drive;
                drive.accel = parseCell(table.cell(row, static_cast<std::size_t>(driveAccel)), drive.accel);
                drive.maxSpeed = parseCell(table.cell(row, static_cast<std::size_t>(driveMax)), drive.maxSpeed);
                drive.walk = parseCell(table.cell(row, static_cast<std::size_t>(driveWalk)), drive.walk);
                drive.run = parseCell(table.cell(row, static_cast<std::size_t>(driveRun)), drive.run);
                nav_->drive[id] = drive;
            }
            if (rate >= 0) {
                const std::string name = table.cell(row, static_cast<std::size_t>(rate));
                for (std::size_t r = 0; r < nav_->rateNames.size(); ++r) {
                    if (nav_->rateNames[r] == name) {
                        nav_->appearanceRateRow[id] = static_cast<int>(r);
                        break;
                    }
                }
            }
            if (personal >= 0) {
                nav_->personalSpace[id] = parseCell(table.cell(row, static_cast<std::size_t>(personal)),
                                                    kDefaultPersonalSpace);
            }
            if (height >= 0) {
                nav_->bodyHeight[id] = parseCell(table.cell(row, static_cast<std::size_t>(height)), 1.8f);
            }
            if (creature >= 0) {
                nav_->creatureSpace[id] = parseCell(table.cell(row, static_cast<std::size_t>(creature)),
                                                    kDefaultPersonalSpace);
            }
        }
    }
}

// CSWSArea::ComputeSafeLocation 004be860 and TestSafeLocationPoint 004be5e0. A spot is safe when it lies on the walkmesh,
// no non-walkable face of the room mesh comes within the creature's personal space of it over its height (the clipped
// 0.02 m segment through the point, z - 0.1 up to z + height, is tested by NoNonWalkPolys with the creature's radius) and
// no other living creature overlaps it (NoCreaturesOnLine, creature spaces added). The search takes the point itself, then
// square rings of half-width r = 1 m growing by the personal space up to `reach`: first each ring's top and bottom rows,
// then its side columns, with the step also the personal space. With `lineTest` (module load passes 1) a candidate must also
// see the original point (TestDirectLine). Placeable walls are not part of the room mesh test.
bool GameSession::computeSafeLocation(kotor::Vec3 point, float reach, std::uint32_t appearance, bool lineTest,
                                      std::size_t before, kotor::Vec3& out)
{
    loadMovementTables();
    out = point;
    // The walkmesh is only built when the search has to leave the point (most creatures stand on a safe spot).
    nav::NavMesh* mesh = nullptr;
    const auto space = [&](const std::map<std::uint32_t, float>& table, std::uint32_t id, float fallback) {
        const auto found = table.find(id);
        return found != table.end() ? found->second : fallback;
    };
    const float step = std::max(space(nav_->personalSpace, appearance, kDefaultPersonalSpace), 0.05f);
    const float radius = step;
    const float bodyReach = space(nav_->creatureSpace, appearance, kDefaultPersonalSpace);
    const float height = space(nav_->bodyHeight, appearance, 1.8f);
    // Non-walkable faces that can matter: the search never leaves a box of `reach` around the point.
    struct Wall {
        kotor::Vec3 v[3];
        float lowX = 0.0f, highX = 0.0f, lowY = 0.0f, highY = 0.0f, lowZ = 0.0f, highZ = 0.0f;
    };
    std::vector<Wall> walls;
    const float box = reach + radius + 1.0f;
    for (const kotor::CollisionFacet& facet : scene_.collision) {
        if (facet.walkable) {
            continue;
        }
        Wall wall{{scene_.collisionVertices[facet.a], scene_.collisionVertices[facet.b], scene_.collisionVertices[facet.c]}};
        const float lowX = std::min({wall.v[0].x, wall.v[1].x, wall.v[2].x});
        const float highX = std::max({wall.v[0].x, wall.v[1].x, wall.v[2].x});
        const float lowY = std::min({wall.v[0].y, wall.v[1].y, wall.v[2].y});
        const float highY = std::max({wall.v[0].y, wall.v[1].y, wall.v[2].y});
        if (highX < point.x - box || lowX > point.x + box || highY < point.y - box || lowY > point.y + box) {
            continue;
        }
        wall.lowX = lowX;
        wall.highX = highX;
        wall.lowY = lowY;
        wall.highY = highY;
        wall.lowZ = std::min({wall.v[0].z, wall.v[1].z, wall.v[2].z});
        wall.highZ = std::max({wall.v[0].z, wall.v[1].z, wall.v[2].z});
        walls.push_back(wall);
    }
    // The 2D distance from (x, y) to a triangle's plan-view shadow.
    const auto distanceToTriangle = [](const Wall& wall, float x, float y) {
        const auto side = [&](const kotor::Vec3& a, const kotor::Vec3& b) {
            return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
        };
        const float s0 = side(wall.v[0], wall.v[1]);
        const float s1 = side(wall.v[1], wall.v[2]);
        const float s2 = side(wall.v[2], wall.v[0]);
        if ((s0 >= 0.0f && s1 >= 0.0f && s2 >= 0.0f) || (s0 <= 0.0f && s1 <= 0.0f && s2 <= 0.0f)) {
            return 0.0f;
        }
        float best = 1.0e9f;
        for (int i = 0; i < 3; ++i) {
            const kotor::Vec3& a = wall.v[i];
            const kotor::Vec3& b = wall.v[(i + 1) % 3];
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float len2 = dx * dx + dy * dy;
            float t = len2 > 0.0f ? ((x - a.x) * dx + (y - a.y) * dy) / len2 : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            best = std::min(best, std::hypot(x - (a.x + t * dx), y - (a.y + t * dy)));
        }
        return best;
    };
    const auto pointSafe = [&](kotor::Vec3& p) {
        if (mesh == nullptr) {
            float floorZ = 0.0f;
            if (!floorBelow(p.x, p.y, point.z + 2.0f, floorZ)) {
                return false;
            }
            p.z = floorZ;
        } else {
            const int tri = mesh->locate({p.x, p.y, point.z});
            if (tri < 0) {
                return false;
            }
            p.z = mesh->height(tri, p.x, p.y);
        }
        for (const Wall& wall : walls) {
            if (wall.highZ < p.z - 0.1f || wall.lowZ > p.z + height || p.x < wall.lowX - radius || p.x > wall.highX + radius
                || p.y < wall.lowY - radius || p.y > wall.highY + radius) {
                continue;
            }
            if (distanceToTriangle(wall, p.x, p.y) < radius) {
                return false;
            }
        }
        for (std::size_t i = 0; i < scene_.entities.size() && i < before; ++i) {
            const kotor::Entity& other = scene_.entities[i];
            if (other.kind != kotor::EntityKind::Creature || other.dead) {
                continue;
            }
            const float reachOther = space(nav_->creatureSpace, other.appearance, kDefaultPersonalSpace);
            if (std::hypot(other.position.x - p.x, other.position.y - p.y) <= reachOther + bodyReach + 0.01f) {
                return false;
            }
        }
        return true;
    };
    const auto sees = [&](kotor::Vec3 from) {
        if (!lineTest) {
            return true;
        }
        const int tri = mesh->locate(from);
        if (tri < 0) {
            return false;
        }
        return mesh->ray(from, tri, point, nav_->closed).clear;
    };
    kotor::Vec3 candidate = point;
    if (pointSafe(candidate)) {
        out = candidate;
        return true;
    }
    mesh = navMesh();
    if (mesh == nullptr || mesh->empty()) {
        return false;
    }
    for (float r = 1.0f; r < reach; r += step) {
        const float x0 = point.x - r;
        const float x1 = point.x + r;
        const float y0 = point.y - r;
        const float y1 = point.y + r;
        for (float x = x0; x <= x1; x += step) {
            for (const float y : {y0, y1}) {
                candidate = {x, y, point.z};
                if (pointSafe(candidate) && sees(candidate)) {
                    out = candidate;
                    return true;
                }
            }
        }
        for (const float x : {x0, x1}) {
            for (float y = y0 + step; y <= y1 - step; y += step) {
                candidate = {x, y, point.z};
                if (pointSafe(candidate) && sees(candidate)) {
                    out = candidate;
                    return true;
                }
            }
        }
    }
    return false;
}

// Retail 005a5680: UTC WalkRate picks the creaturespeed.2da row; row 7 means "from
// appearance", found by matching appearance MOVERATE against the 2DAName column.
// WALKRATE and RUNRATE are the speeds. appearance WALKDIST/RUNDIST are read by the
// model layer for stride, not by movement. The PC uses the PLAYER row.
float GameSession::creatureMoveRate(std::uint32_t object, bool run)
{
    loadMovementTables();
    const std::vector<float>& rates = run ? nav_->runRates : nav_->walkRates;
    int row = 0;
    bool pc = false;
    if (object == scriptWorld_.pc) {
        pc = true;
        for (std::size_t r = 0; r < nav_->rateNames.size(); ++r) {
            if (nav_->rateNames[r] == "PLAYER") {
                row = static_cast<int>(r);
            }
        }
    } else {
        const std::size_t index = ScriptApi{*this}.entityFor(object);
        if (index != static_cast<std::size_t>(-1)) {
            const kotor::Entity& entity = scene_.entities[index];
            row = entity.walkRateRow;
            if (row == 7 || row < 0) {
                const auto found = nav_->appearanceRateRow.find(entity.appearance);
                row = found != nav_->appearanceRateRow.end() ? found->second : -1;
            }
        } else {
            row = -1;
        }
    }
    float rate = row >= 0 && static_cast<std::size_t>(row) < rates.size()
        ? rates[static_cast<std::size_t>(row)]
        : (run ? 4.0f : 2.0f); // No table entry: the earlier engine defaults, not evidence.
    if (pc && hasEffect("SPEED")) {
        rate *= 1.5f; // The clamp at retail 004f1b20 tops out at 1.5.
    }
    return rate;
}

// Retail CSWPlayerControlCamRelative::Control (vtable 00679940, helper 00679870): the controlled creature's drive
// velocity obeys  dv/dt = A * u - (A / R) * v  with A = appearance DRIVEACCL (client record +0x58) and R = the top
// speed (record +0x5c, DRIVEMAXSPEED), u the input direction (0.5 of it while Shift holds the walk mode, 00679b7b).
// A creature slower than 1.8 accelerates 0.5556 R / 1 as fast (00752d64/68). In stealth (creature flag +0x9fc / client
// +0x2d4 bit 0) the top speed is the walk speed (record +0x60) and A is 15 (GetDriveAcceleration 00610590,
// GetMovementRate 00679510). The solution of that equation over one frame is exact below.
kotor::Vec3 GameSession::driveStep(std::uint32_t object, float dirX, float dirY, float scale, float dt)
{
    loadMovementTables();
    std::size_t index = static_cast<std::size_t>(-1);
    if (object == scriptWorld_.pc) {
        index = playerEntity_;
    } else {
        index = ScriptApi{*this}.entityFor(object);
    }
    nav::Runtime::DriveRow row;
    if (index < scene_.entities.size()) {
        if (const auto found = nav_->drive.find(scene_.entities[index].appearance); found != nav_->drive.end()) {
            row = found->second;
        }
    }
    float top = row.maxSpeed;
    float accel = row.accel;
    if (stealthActive_ && object == controlledObject()) {
        top = row.walk;
        accel = 15.0f;
    }
    if (object == scriptWorld_.pc && hasEffect("SPEED")) {
        top *= 1.5f; // The movement-rate clamp at retail 004ec3b0 tops out at 1.5.
    }
    if (top <= 0.0f || accel <= 0.0f) {
        return {0.0f, 0.0f, 0.0f};
    }
    if (top <= 1.8f) {
        accel *= top * 0.5555556f;
    }
    std::array<float, 2>& velocity = nav_->driveVelocity[object];
    const float tau = top / accel;
    const float targetX = dirX * top * scale;
    const float targetY = dirY * top * scale;
    const float decay = std::exp(-dt / tau);
    const float gain = tau * (1.0f - decay);
    const kotor::Vec3 move{targetX * dt + (velocity[0] - targetX) * gain,
                           targetY * dt + (velocity[1] - targetY) * gain, 0.0f};
    velocity[0] = targetX + (velocity[0] - targetX) * decay;
    velocity[1] = targetY + (velocity[1] - targetY) * decay;
    return move;
}

void GameSession::locomotionOf(std::uint32_t object, std::uint32_t appearance, float speed, bool& running, float& stride)
{
    loadMovementTables();
    nav::Runtime::DriveRow drive;
    if (const auto row = nav_->drive.find(appearance); row != nav_->drive.end()) {
        drive = row->second;
    }
    if (object == controlledObject() && thirdPerson_) {
        // Retail 00615960: the keyboard-driven body plays its run clip while its forward speed is over 0.6 of the top
        // speed (007a22f4), its walk clip below.
        running = speed > 0.6f * drive.maxSpeed;
    } else {
        // A walker's clip is the walk or run of its action; the ramp of 00512f10 starts it slow, so the speed is read at
        // the table speed it would make at full factor.
        float nominal = speed;
        if (const auto walking = nav_->walkFactor.find(object);
            walking != nav_->walkFactor.end() && elapsed_ - walking->second.lastStep < 0.25f
            && walking->second.scale > 0.05f) {
            nominal = speed / walking->second.scale;
        }
        const float walkRate = creatureMoveRate(object, false);
        const float runRate = creatureMoveRate(object, true);
        running = nominal >= (runRate > walkRate ? (walkRate + runRate) * 0.5f : 2.6f);
    }
    stride = running ? drive.run : drive.walk;
}

void GameSession::driveStop(std::uint32_t object)
{
    if (nav_) {
        nav_->driveVelocity.erase(object);
    }
}

void GameSession::driveSetVelocity(std::uint32_t object, float vx, float vy)
{
    std::array<float, 2>& velocity = nav_->driveVelocity[object];
    velocity[0] = vx;
    velocity[1] = vy;
}

float GameSession::creatureRadius(std::uint32_t object)
{
    loadMovementTables();
    std::size_t index = static_cast<std::size_t>(-1);
    if (object == scriptWorld_.pc) {
        index = playerEntity_;
        if (index >= scene_.entities.size()) {
            return kPlayerPersonalSpace;
        }
    } else {
        index = ScriptApi{*this}.entityFor(object);
        if (index == static_cast<std::size_t>(-1)) {
            return kDefaultPersonalSpace;
        }
    }
    const auto found = nav_->personalSpace.find(scene_.entities[index].appearance);
    return found != nav_->personalSpace.end() ? found->second : kDefaultPersonalSpace;
}

// Retail 004ee440, per target kind. It also aims at a door or placeable's nearest
// use point; here the position is the object's own and the path ends where it can.
float GameSession::creatureApproachRange(std::uint32_t self, std::uint32_t target, float range)
{
    loadMovementTables();
    ScriptApi api{*this};
    const auto creatureSpaceOf = [&](std::uint32_t object) {
        const std::size_t index = object == scriptWorld_.pc ? playerEntity_ : api.entityFor(object);
        if (index >= scene_.entities.size()) {
            return kPlayerPersonalSpace;
        }
        const auto found = nav_->creatureSpace.find(scene_.entities[index].appearance);
        return found != nav_->creatureSpace.end() ? found->second : kDefaultPersonalSpace;
    };
    float floorRange = creatureRadius(self);
    if (api.isCreature(target)) {
        floorRange = creatureSpaceOf(target) + creatureSpaceOf(self) + 0.3f;
    } else if (const std::size_t index = api.entityFor(target); index != static_cast<std::size_t>(-1)) {
        const kotor::EntityKind kind = scene_.entities[index].kind;
        if (kind == kotor::EntityKind::Door || kind == kotor::EntityKind::Placeable) {
            floorRange += 0.75f;
        }
    }
    return std::max(range, floorRange);
}

ActorStep GameSession::stepActorToward(std::uint32_t object, kotor::Vec3 target, float speed,
                                       float dt, float stopDistance, bool ramp)
{
    ScriptApi api{*this};
    const Vec3 from = api.position(object);
    const float straight = length2D(from, target);
    nav::NavMesh* mesh = navMesh();

    const auto finish = [&](ActorStep step, MoveOutcome outcome, std::size_t door) {
        if (outcome == MoveOutcome::Arrived) {
            if (const auto walking = nav_->actors.find(object); walking != nav_->actors.end()) {
                walking->second.bumps = 0;
                walking->second.hasLastBump = false;
            }
            nav_->walkFactor.erase(object);
        }
        // The outcome of the player's click walk belongs to the steered creature.
        if (object == controlledObject()) {
            moveOutcome_ = outcome;
            moveBlockingDoor_ = door;
        }
        // retail creature +0x33c; k_ai_master answers OnBlocked by opening the door. The
        // player's own creature gets the outcome above instead.
        if (scriptWorld_.get(object) != nullptr) {
            ActorState& state = api.actor(object);
            if (outcome != MoveOutcome::DoorBlocked) {
                state.blockedRaised = false;
            } else {
                state.blockingDoor = objectForEntity(door);
                if (!state.blockedRaised && object != controlledObject()) {
                    state.blockedRaised = true;
                    api.raiseHook(object, "Blocked");
                }
            }
        }
        return step;
    };
    constexpr std::size_t kNoDoor = static_cast<std::size_t>(-1);

    if (mesh->empty()) {
        // A scene with no walkmesh at all.
        if (straight <= stopDistance) {
            return finish(ActorStep::Arrived, MoveOutcome::Arrived, kNoDoor);
        }
        if (speed <= 0.0f || dt <= 0.0f || straight < 1.0e-5f) {
            return ActorStep::Blocked;
        }
        const float stride = std::min(speed * dt, straight - std::max(stopDistance, 0.0f));
        const Vec3 next{from.x + (target.x - from.x) / straight * stride,
                        from.y + (target.y - from.y) / straight * stride, from.z};
        api.setPosition(object, next, std::atan2(target.y - from.y, target.x - from.x), false);
        return ActorStep::Moving;
    }

    // A zero step only settles the actor on the floor (after a jump).
    if (speed <= 0.0f || dt <= 0.0f) {
        Vec3 settled = from;
        const int tri = mesh->locate(from);
        if (tri >= 0) {
            settled.z = mesh->height(tri, from.x, from.y);
        } else {
            Vec3 snapped;
            if (mesh->nearest(from, snapped) >= 0 && length2D(from, snapped) <= kSettleSnap) {
                settled = snapped;
            }
        }
        api.setPosition(object, settled);
        return straight <= stopDistance ? ActorStep::Arrived : ActorStep::Blocked;
    }

    nav::ActorPath& route = nav_->actors[object];
    route.abortLogLeft = std::max(0.0f, route.abortLogLeft - dt);
    route.waitLeft = std::max(0.0f, route.waitLeft - dt);
    const float radius = creatureRadius(object);
    // A walker standing out a door asks again only when its wait is over, not on the frame the door commits.
    bool replan = !route.valid || (route.doorVersion != nav_->doorVersion && route.waitLeft <= 0.0f)
        || length2D(route.target, target) > kRepathTargetMove;
    if (!replan && route.next < route.path.points.size()) {
        // Pushed or jumped off the route.
        const Vec3 near = nav::closestOnSegment2D(from, route.path.points[route.next - 1],
                                                  route.path.points[route.next]);
        replan = length2D(near, from) > kRouteDriftLimit;
    }
    if (replan) {
        // A new walk starts with a clean count of bumps.
        if (route.path.points.empty() || length2D(route.target, target) > kRepathTargetMove) {
            route.bumps = 0;
            route.hasLastBump = false;
        }
        if (route.valid && route.doorVersion != nav_->doorVersion && object != controlledObject()) {
            const std::size_t who = api.entityFor(object);
            logInfo(concat("actor ", who < scene_.entities.size() ? scene_.entities[who].tag : std::string("?"),
                           " re-plans after a door change at ", elapsed_));
        }
        route.path = mesh->findPath(from, target, radius, nav_->closed);
        route.next = 1;
        route.target = target;
        route.doorVersion = nav_->doorVersion;
        route.valid = true;
    }
    const nav::PathResult& path = route.path;
    const std::vector<Vec3>& points = path.points;
    if (points.size() < 2) {
        return finish(ActorStep::Blocked, MoveOutcome::Unreachable, kNoDoor);
    }
    float remaining = route.next < points.size() ? length2D(from, points[route.next]) : 0.0f;
    for (std::size_t i = route.next + 1; i < points.size(); ++i) {
        remaining += length2D(points[i - 1], points[i]);
    }

    const bool reachable = path.status == nav::PathStatus::Reached
        || path.status == nav::PathStatus::Adjusted;
    const std::size_t blockingEntity = path.status == nav::PathStatus::DoorBlocked
        ? mesh->doors()[static_cast<std::size_t>(path.blockingDoor)].entity
        : kNoDoor;
    // Retail stops a walk at its range before the step, not with a stride sized to end on it: 00516630 (the stop test of
    // the walk step, for a move that carries the range flag) ends the walk when the squared 2D distance to the path's last
    // point is <= range squared and the line to it is clear and walkable, so a walker ends up to one stride inside the
    // range (a script's GetDistanceToObject2D <= range then holds). A walk without a range follows the path to its end.
    if (reachable && remaining <= 0.02f) {
        return finish(ActorStep::Arrived, MoveOutcome::Arrived, kNoDoor);
    }
    if (reachable && stopDistance > 0.0f) {
        const Vec3 last = points.back();
        const float gapX = last.x - from.x;
        const float gapY = last.y - from.y;
        if (gapX * gapX + gapY * gapY <= stopDistance * stopDistance) {
            // The line to the point is clear and walkable (the walkmesh ray, closed doors stop it), not a sight test.
            const int fromTri = mesh->locate(from);
            if (fromTri >= 0 && mesh->ray(from, fromTri, last, nav_->closed).clear) {
                return finish(ActorStep::Arrived, MoveOutcome::Arrived, kNoDoor);
            }
        }
    }
    // Within range but not routed: what is wanted may stand behind the closed door
    // or on an island, and the actor has come as far as it can.
    if (!reachable && straight <= stopDistance) {
        return finish(ActorStep::Arrived, MoveOutcome::Arrived, kNoDoor);
    }
    if (!reachable && remaining <= 0.02f) {
        // At a door that is opening the walker waits for the clip to finish and then goes on (retail 0098: Trask
        // stands until the door is open); it is not blocked, so no Blocked hook and no walk failure.
        // Retail 004eb5a0 CSWSCreature::AddPathfindingWaitActionToFront queues a wait action of 0.3 s (0x3e99999a;
        // 0.1 s, 0x3dcccccd, for an object the server has a client for, i.e. the player) in front of the walk, so
        // the walker asks again at the end of each wait, not when the door commits (row 32: Trask stopped at the
        // closed door, ran 251 ms after the commit). The walk step that bumps a door is 00516630.
        if (path.status == nav::PathStatus::DoorBlocked
            && (route.waitLeft > 0.0f || doorStillOpening(scene_, blockingEntity))) {
            if (route.waitLeft <= 0.0f) {
                route.waitLeft = object == controlledObject() ? 0.1f : 0.3f;
                const std::size_t who = api.entityFor(object);
                logInfo(concat("actor ", who < scene_.entities.size() ? scene_.entities[who].tag : std::string("PC"),
                               " waits at a door that is not open at ", elapsed_));
            }
            return finish(ActorStep::Moving, MoveOutcome::Moving, kNoDoor);
        }
        return path.status == nav::PathStatus::DoorBlocked
            ? finish(ActorStep::Blocked, MoveOutcome::DoorBlocked, blockingEntity)
            : finish(ActorStep::Blocked, MoveOutcome::Unreachable, kNoDoor);
    }

    // Where this frame's stride along the route ends. Retail 0051d9c0 (WalkUpdateLocation_QuickWalk) and 0051bb10 take
    // the distance as dt * (factor + last factor) / 2 * rate, the speed factor ramping up by 1.0 per second from 0.1 and
    // down toward the end of the path (ComputeSpeedFactor 00512f10); 00516630 clamps the stride to the path's end.
    float stepSpeed = speed;
    if (ramp) {
        nav::Runtime::WalkFactor& walk = nav_->walkFactor[object];
        if (elapsed_ - walk.lastStep > 0.25f) {
            walk.factor = 0.0f;
        }
        walk.lastStep = elapsed_;
        const float previous = walk.factor;
        const float slowZone = speed * 0.5f; // 'rate * 1000 * 1000 * 5e-7': half a second of travel.
        float factor = 1.0f;
        if (slowZone > 0.0f && remaining <= slowZone) {
            const float ratio = remaining / slowZone;
            factor = (ratio < 0.01f ? 0.1f : std::sqrt(ratio)) - dt;
        }
        if (previous <= 1.0f && previous + dt <= factor) {
            factor = previous + dt;
        }
        factor = std::max(factor, 0.1f);
        walk.factor = factor;
        walk.scale = (factor + previous) * 0.5f;
        stepSpeed = speed * walk.scale;
    }
    const float stride = std::min(stepSpeed * dt, remaining);
    Vec3 desired = from;
    std::size_t nextIndex = route.next;
    float left = stride;
    while (nextIndex < points.size()) {
        const float leg = length2D(desired, points[nextIndex]);
        if (leg > left) {
            const float t = leg > 1.0e-6f ? left / leg : 0.0f;
            desired = nav::lerp(desired, points[nextIndex], t);
            break;
        }
        left -= leg;
        desired = points[nextIndex];
        ++nextIndex;
    }

    const int startTri = mesh->locate(from);
    if (startTri < 0) {
        // Standing off the mesh (a script put it there): walk back on directly.
        const Vec3 onto = points.front();
        const float gap = length2D(from, onto);
        if (gap > 1.0e-3f) {
            const float t = std::min(1.0f, speed * dt / gap);
            api.setPosition(object, nav::lerp(from, onto, t),
                            std::atan2(onto.y - from.y, onto.x - from.x), false);
            if (object == scriptWorld_.pc) {
                nav_->playerStepped = true;
            }
            return finish(ActorStep::Moving, MoveOutcome::Moving, kNoDoor);
        }
    }

    // Other creatures: retail 00516630 tests each step against them (00512fd0). A hit counts a bump (path data
    // +0x268); six bumps, or a second bump on the same creature at the same spot, end the walk; otherwise 005d0840
    // plans a detour round a hexagon about the blocker and splices it into the route.
    const std::size_t selfEntity = object == scriptWorld_.pc ? playerEntity_ : api.entityFor(object);
    struct Blocker {
        bool hit = false;
        Vec3 position;
        float space = 0.0f;
        std::uint32_t object = 0;
    };
    const auto spaceOf = [&](const kotor::Entity& other) {
        const auto space = nav_->personalSpace.find(other.appearance);
        return space != nav_->personalSpace.end() ? space->second : kDefaultPersonalSpace;
    };
    const auto blockerAt = [&](Vec3 point) {
        Blocker found;
        const auto against = [&](Vec3 other, float otherRadius) {
            const float combined = radius + otherRadius;
            if (length2D(target, other) < combined) {
                return false; // Walking up to something standing on the target.
            }
            const float there = length2D(point, other);
            const float here = length2D(from, other);
            // A walker that already stands inside the other's space (a companion that took the controls on top of the
            // PC) is let out: with the speed ramp its first strides are a few millimetres and would only ever
            // look like a step toward the other body.
            if (here < combined) {
                return false;
            }
            return there < combined && there < here - 1.0e-4f;
        };
        if (object != scriptWorld_.pc && against(position_, kPlayerPersonalSpace)) {
            found.hit = true;
            found.position = position_;
            found.space = kPlayerPersonalSpace;
            found.object = scriptWorld_.pc;
            return found;
        }
        for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
            const kotor::Entity& other = scene_.entities[i];
            if (other.kind != kotor::EntityKind::Creature || other.dead || i == selfEntity
                || i == playerEntity_) {
                continue;
            }
            if (against(other.position, spaceOf(other))) {
                found.hit = true;
                found.position = other.position;
                found.space = spaceOf(other);
                found.object = objectForEntity(i);
                return found;
            }
        }
        return found;
    };
    const auto nameOfWalker = [&]() {
        const std::size_t who = api.entityFor(object);
        return who < scene_.entities.size() ? scene_.entities[who].tag : std::string("PC");
    };
    const auto abortWalk = [&](const char* why, const Blocker& by, bool announce) {
        if (announce && route.abortLogLeft <= 0.0f) {
            route.abortLogLeft = 2.0f;
            logInfo(concat("actor ", nameOfWalker(), " aborted walking, ", why, " at ", elapsed_));
        }
        route.valid = false;
        route.bumps = 0;
        route.hasLastBump = false;
        // GetBlockingCreature 00546e00. When retail fills it was not traced; the creature a walk was aborted
        // against is this engine's moment.
        if (by.hit && by.object != 0) {
            api.actor(object).blockingCreature = by.object;
            if (object != controlledObject() && announce) {
                api.raiseHook(object, "Blocked");
            }
        }
        return finish(ActorStep::Aborted, MoveOutcome::Unreachable, kNoDoor);
    };
    // 005d0840: 0 totally blocked, -2 the goal lies inside the blocker's hexagon, 1 a detour was spliced in.
    const auto detourAround = [&](const Blocker& by) -> int {
        route.hasLastBump = true;
        route.lastBump = by.object;
        route.lastBumpPosition = by.position;
        std::vector<Vec3> q;
        q.push_back(from);
        for (std::size_t i = route.next; i < route.path.points.size(); ++i) {
            q.push_back(route.path.points[i]);
        }
        if (q.size() < 2) {
            return 0;
        }
        const nav::Hexagon hex = nav::makeHexagon(by.position, api.facing(object),
                                                  radius + by.space + kDetourMargin);
        if (hex.contains(q.back())) {
            return -2;
        }
        const std::vector<nav::HexCrossing> crossings = hex.crossings(q);
        if (crossings.empty()) {
            return 0;
        }
        const nav::HexCrossing exitAt = crossings.back();
        nav::HexCrossing entryAt;
        if (crossings.size() >= 2) {
            entryAt = crossings[crossings.size() - 2];
        } else {
            // 005d0160: one crossing only, the walker stands inside; the way round starts at the hexagon's
            // vertex nearest to where it is (005cfae0).
            const int nearest = hex.nearestVertex(q.front());
            entryAt.segment = 1;
            entryAt.edge = nearest;
            entryAt.point = hex.vertex[static_cast<std::size_t>(nearest)];
        }
        // 005cf7a0 goes forward round the hexagon, 005cf650 backward; the toggle (path data +0x244) picks which
        // is built first and flips for the second.
        const bool firstForward = !route.detourToggle;
        const std::array<std::vector<Vec3>, 2> arcs{hex.arc(entryAt, exitAt, firstForward),
                                                    hex.arc(entryAt, exitAt, !firstForward)};
        route.detourToggle = !route.detourToggle;
        // 005d0770: every leg of the arc must be walkable; a leg a creature stands on ranks below a clear one
        // and above a wall.
        std::array<int, 2> rank{0, 0};
        for (std::size_t a = 0; a < arcs.size(); ++a) {
            int result = 2;
            for (std::size_t i = 1; i < arcs[a].size() && result > 0; ++i) {
                const Vec3 legFrom = arcs[a][i - 1];
                const Vec3 legTo = arcs[a][i];
                const int tri = mesh->locate(legFrom);
                if (tri < 0 || !mesh->stepAgainstWalls(legFrom, tri, legTo, radius, nav_->closed, true).clear) {
                    result = 0;
                    break;
                }
                for (std::size_t c = 0; c < scene_.entities.size(); ++c) {
                    const kotor::Entity& other = scene_.entities[c];
                    if (other.kind != kotor::EntityKind::Creature || other.dead || c == selfEntity
                        || c == playerEntity_ || objectForEntity(c) == by.object) {
                        continue;
                    }
                    const Vec3 closest = nav::closestOnSegment2D(other.position, legFrom, legTo);
                    if (length2D(closest, other.position) < radius + spaceOf(other)) {
                        result = 1;
                    }
                }
            }
            rank[a] = result;
        }
        const std::size_t chosen = rank[0] < rank[1] ? 1 : 0;
        if (rank[chosen] == 0) {
            return 0;
        }
        // 005cecc0: the route up to the crossing, the arc, then the rest of the route. Retail skips the end of the
        // segment when the walk enters and leaves on one segment; here the rest starts at that end, or the goal
        // would be lost.
        std::vector<Vec3> spliced;
        for (std::size_t i = 0; i < entryAt.segment && i < q.size(); ++i) {
            spliced.push_back(q[i]);
        }
        for (Vec3 point : arcs[chosen]) {
            point.z = from.z;
            spliced.push_back(point);
        }
        for (std::size_t i = exitAt.segment; i < q.size(); ++i) {
            spliced.push_back(q[i]);
        }
        route.path.points = std::move(spliced);
        route.next = 1;
        // After the walker's own pick, the blocker is told to go round the other way when both face alike
        // (00423c20 taken as the dot product of the two facings).
        if (by.object != scriptWorld_.pc) {
            const float mine = api.facing(object);
            const float theirs = api.facing(by.object);
            const bool facingAlike = std::cos(mine) * std::cos(theirs) + std::sin(mine) * std::sin(theirs) >= 0.0f;
            nav_->actors[by.object].detourToggle = facingAlike ? !route.detourToggle : route.detourToggle;
        }
        return 1;
    };
    if (const Blocker hit = blockerAt(desired); hit.hit) {
        ++route.bumps;
        if (route.bumps >= kMaxBumps) {
            return abortWalk("Maximum number of bumps happened", hit, true);
        }
        const bool sameSpot = route.hasLastBump && route.lastBump == hit.object
            && length2D(route.lastBumpPosition, hit.position) <= kSameSpot;
        if (sameSpot) {
            return abortWalk("Bumped into this creature at this position already.", hit, true);
        }
        const int outcome = detourAround(hit);
        if (outcome == 0) {
            if (object == scriptWorld_.pc) {
                raiseCombatMessage(47859);
            }
            return abortWalk("we are totaly blocked. can't get around this creature at all.", hit, true);
        }
        if (outcome < 0) {
            return abortWalk("the goal lies inside the creature's space.", hit, false);
        }
        return finish(ActorStep::Moving, MoveOutcome::Moving, kNoDoor);
    }

    const nav::RayResult walked = mesh->stepAgainstWalls(from, startTri, desired, radius, nav_->closed, true);
    const float moved = length2D(from, walked.end);
    if (!walked.clear) {
        route.valid = false;
    } else {
        route.next = nextIndex;
    }
    if (moved < 1.0e-4f) {
        return finish(ActorStep::Blocked, MoveOutcome::Moving, kNoDoor);
    }
    api.setPosition(object, walked.end, std::atan2(walked.end.y - from.y, walked.end.x - from.x), false);
    if (object == scriptWorld_.pc) {
        nav_->playerStepped = true;
    }
    return finish(ActorStep::Moving, MoveOutcome::Moving, kNoDoor);
}

// The keyboard's step: straight, else along the edge that stopped it. A closed door
// stops the body short by its radius rather than its centre.
kotor::Vec3 GameSession::slidePlayer(kotor::Vec3 from, kotor::Vec3 to, bool& moved, float bodyRadius)
{
    moved = false;
    nav::NavMesh* mesh = navMesh();
    if (mesh->empty()) {
        moved = true;
        return to;
    }
    const int tri = mesh->locate(from);
    if (tri < 0) {
        // Off the walkable floor (a script or test put the player there): back on.
        Vec3 snapped;
        if (mesh->nearest(from, snapped) >= 0 && length2D(from, snapped) <= kSettleSnap) {
            moved = true;
            return snapped;
        }
        moved = true;
        return to;
    }
    const float wanted = length2D(from, to);
    if (wanted < 1.0e-6f) {
        return from;
    }
    // Look one body radius further, so a closed door stops the body and not its centre.
    const float radius = bodyRadius >= 0.0f ? bodyRadius : creatureRadius(scriptWorld_.pc);
    const Vec3 ahead = nav::lerp(from, to, (wanted + radius) / wanted);
    nav::RayResult first = mesh->ray(from, tri, ahead, nav_->closed);
    Vec3 end;
    if (first.hitDoor >= 0) {
        const float allowed = std::clamp(length2D(from, first.end) - radius, 0.0f, wanted);
        end = nav::lerp(from, to, allowed / wanted);
        end.z = mesh->height(tri, end.x, end.y);
        const int endAt = mesh->locate(end);
        end.z = endAt >= 0 ? mesh->height(endAt, end.x, end.y) : from.z;
    } else {
        first = mesh->stepAgainstWalls(from, tri, to, radius, nav_->closed, false);
        if (first.clear) {
            moved = true;
            return first.end;
        }
        end = first.end;
    }
    const float rest = std::max(0.0f, length2D(from, to) - length2D(from, end));
    const float along = ((to.x - from.x) * first.edge.x + (to.y - from.y) * first.edge.y)
        / std::max(length2D(from, to), 1.0e-5f);
    const Vec3 slid{end.x + first.edge.x * along * rest, end.y + first.edge.y * along * rest, end.z};
    const int endTri = mesh->locate(end);
    if (endTri >= 0 && rest > 1.0e-5f) {
        const nav::RayResult second = mesh->stepAgainstWalls(end, endTri, slid, radius, nav_->closed, false);
        end = second.end;
    }
    moved = length2D(from, end) > 1.0e-4f;
    return end;
}

// A slot behind the anchor, pulled back along the line from the anchor to whatever
// the walkmesh and doors allow. Retail keeps per-slot follow data in the party table
// (0051c360); the slot offsets themselves were not recovered, so the existing
// two-column pattern stays.
kotor::Vec3 GameSession::formationSlot(kotor::Vec3 anchor, float anchorYaw, int slot, float baseDistance)
{
    const Vec3 forward{std::cos(anchorYaw), std::sin(anchorYaw), 0.0f};
    const Vec3 right{-forward.y, forward.x, 0.0f};
    const float row = static_cast<float>(slot / 2);
    const float side = (slot % 2 == 0) ? -1.2f : 1.2f;
    const Vec3 wanted = anchor - forward * (baseDistance + row * 1.1f) + right * side;
    nav::NavMesh* mesh = navMesh();
    const int tri = mesh->locate(anchor);
    if (tri < 0) {
        return wanted;
    }
    return mesh->ray(anchor, tri, wanted, nav_->closed).end;
}

// Retail 0051d9c0 scales a follower's speed by squared distance: 0.9 inside 1, 1.2
// inside 225, 1.5 beyond. Which distance it measures is ambiguous in the
// decompile (taken here as follower to leader). No teleport for a far follower was
// found in that path, so none happens: a follower cut off stays put until a route
// opens, and the module-entry placement gathers the party.
void GameSession::followLeader(std::uint32_t self, kotor::Vec3 anchor, kotor::Vec3 slot, float dt)
{
    const kotor::Vec3 here = ScriptApi{*this}.position(self);
    const float toSlot = length2D(here, slot);
    if (toSlot <= kFormationArrive) {
        return;
    }
    const float apart = length2D(here, anchor);
    const float squared = apart * apart;
    const float scale = squared < 1.0f ? 0.9f : (squared < 225.0f ? 1.2f : 1.5f);
    const bool run = apart > 3.0f;
    stepActorToward(self, slot, creatureMoveRate(self, run) * scale, dt, kFormationArrive, false);
}

void GameSession::placePartyAtEntry()
{
    nav::NavMesh* mesh = navMesh();
    // loadModule put the entry point in position_. When a companion is steered it
    // stands on the entry point and the PC takes a formation slot like a member.
    const Vec3 entry = position_;
    const std::size_t none = static_cast<std::size_t>(-1);
    const std::size_t lead = companionControlled() ? controlledEntity() : none;
    const auto settle = [&](Vec3 spot) {
        const int tri = mesh->locate(spot);
        if (tri >= 0) {
            spot.z = mesh->height(tri, spot.x, spot.y);
        }
        return spot;
    };
    if (lead != none) {
        scene_.entities[lead].position = settle(entry);
        scene_.entities[lead].bearing = yaw_;
        syncEntityToScriptWorld(lead);
    }
    int slot = 0;
    if (lead != none) {
        position_ = settle(formationSlot(entry, yaw_, slot, 2.0f));
        playerBearing_ = yaw_;
        syncPlayerToScriptWorld();
        ++slot;
    }
    for (const CompanionState& companion : companions_) {
        const std::size_t index = findEntity(companion.tag);
        if (index == none || scene_.entities[index].dead || index == lead) {
            continue;
        }
        scene_.entities[index].position = settle(formationSlot(entry, yaw_, slot, 2.0f));
        scene_.entities[index].bearing = yaw_;
        syncEntityToScriptWorld(index);
        ++slot;
    }
}

bool GameSession::moveTo(kotor::Vec3 target, float stopDistance, bool run)
{
    nav::NavMesh* mesh = navMesh();
    if (mesh->empty()) {
        moveOutcome_ = MoveOutcome::Unreachable;
        return false;
    }
    const std::uint32_t self = controlledObject();
    ScriptApi api{*this};
    const nav::PathResult path =
        mesh->findPath(api.position(self), target, creatureRadius(self), nav_->closed);
    if (path.status == nav::PathStatus::Unreachable || path.status == nav::PathStatus::NoMesh) {
        moveOutcome_ = MoveOutcome::Unreachable;
        moveBlockingDoor_ = static_cast<std::size_t>(-1);
        return false;
    }
    api.clearQueue(self);
    ActorAction action;
    action.kind = ActorActionKind::MoveToPoint;
    action.point = target;
    action.run = run;
    action.range = stopDistance;
    action.userIssued = true;
    if (!api.enqueue(self, std::move(action))) {
        return false;
    }
    moveOutcome_ = MoveOutcome::Moving;
    moveBlockingDoor_ = static_cast<std::size_t>(-1);
    return true;
}

bool GameSession::pathExists(kotor::Vec3 from, kotor::Vec3 to, std::size_t* blockingDoor)
{
    nav::NavMesh* mesh = navMesh();
    if (blockingDoor != nullptr) {
        *blockingDoor = static_cast<std::size_t>(-1);
    }
    if (mesh->empty()) {
        return false;
    }
    const nav::PathResult path = mesh->findPath(from, to, creatureRadius(controlledObject()), nav_->closed);
    if (path.status == nav::PathStatus::DoorBlocked && blockingDoor != nullptr) {
        *blockingDoor = mesh->doors()[static_cast<std::size_t>(path.blockingDoor)].entity;
    }
    return path.status == nav::PathStatus::Reached;
}

bool GameSession::movePending() const
{
    const auto it = sim_->actors.find(controlledObject());
    if (it == sim_->actors.end()) {
        return false;
    }
    for (const ActorAction& action : it->second.queue) {
        if (action.userIssued && action.kind == ActorActionKind::MoveToPoint) {
            return true;
        }
    }
    return false;
}

bool GameSession::openDoorAsPlayer(std::size_t entityIndex)
{
    // The world-state family owns locks, keys, traps and door hooks.
    return openDoor(entityIndex, controlledObject()) == UseOutcome::Opened;
}

// ---- the family gate ------------------------------------------------------------

namespace {

struct PthNode {
    Vec3 point;
    int tri = -1;
    std::vector<int> neighbours;
};

bool crossesSegment(Vec3 a, Vec3 b, Vec3 c, Vec3 d)
{
    const float d1 = nav::triCross(c, d, a);
    const float d2 = nav::triCross(c, d, b);
    const float d3 = nav::triCross(a, b, c);
    const float d4 = nav::triCross(a, b, d);
    return ((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f))
        && ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f));
}

} // namespace

int runPathfindingFamilyTest(GameSession& game)
{
    using Clock = std::chrono::steady_clock;
    bool ok = true;
    const auto check = [&](bool condition, const std::string& label) {
        std::printf("pathfinding: %s=%d\n", label.c_str(), condition ? 1 : 0);
        ok = ok && condition;
    };
    const auto info = [](const std::string& line) { std::printf("pathfinding: %s\n", line.c_str()); };
    constexpr float kFrame = 1.0f / 60.0f;

    game.quietWorldForTest_ = true;
    ScriptApi api{game};

    // A walked route: every frame on walkable floor, no frame longer than the speed.
    struct Walk {
        int frames = 0;
        float travelled = 0.0f;
        float maxStep = 0.0f;
        int offFloor = 0;
        int jumps = 0;
        std::set<int> rooms;
    };
    const auto checkFrame = [&](Walk& walk, Vec3 before, Vec3 after, float limit) {
        nav::NavMesh* mesh = game.navMesh();
        const float step = length2D(before, after);
        walk.travelled += step;
        walk.maxStep = std::max(walk.maxStep, step);
        if (step > limit * kFrame * 1.02f + 1.0e-3f) {
            ++walk.jumps;
        }
        const int tri = mesh->locate(after);
        if (tri < 0 || std::fabs(mesh->height(tri, after.x, after.y) - after.z) > 0.05f) {
            ++walk.offFloor;
        } else {
            walk.rooms.insert(mesh->tris()[static_cast<std::size_t>(tri)].room);
        }
    };
    const auto walkPc = [&](Vec3 target, float stop, bool run, float seconds) {
        Walk walk;
        const float limit = game.creatureMoveRate(game.scriptWorld_.pc, run);
        if (!game.moveTo(target, stop, run)) {
            return walk;
        }
        const int frames = static_cast<int>(seconds / kFrame);
        while (walk.frames < frames && game.movePending()) {
            const Vec3 before = game.position();
            game.update(kFrame, InputState{});
            checkFrame(walk, before, game.position(), limit);
            ++walk.frames;
        }
        return walk;
    };

    for (const char* moduleName : {"end_m01aa", "tar_m03aa"}) {
        const std::string tag = moduleName;
        if (game.module() != tag) {
            check(game.loadModule(tag, nullptr, ""), tag + ".load");
        }
        for (kotor::Entity& entity : game.scene_.entities) {
            if (entity.kind == kotor::EntityKind::Door) {
                entity.open = false;
            }
        }
        nav::NavMesh* mesh = game.navMesh();
        std::vector<char>& closed = game.nav_->closed;

        // ---- geometry ----------------------------------------------------------
        float facetArea = 0.0f;
        std::size_t walkableFacets = 0;
        for (const kotor::CollisionFacet& facet : game.scene_.collision) {
            if (facet.walkable) {
                facetArea += 0.5f * std::fabs(nav::triCross(game.scene_.collisionVertices[facet.a],
                                                            game.scene_.collisionVertices[facet.b],
                                                            game.scene_.collisionVertices[facet.c]));
                ++walkableFacets;
            }
        }
        info(tag + " tris=" + std::to_string(mesh->tris().size()) + " walkable_facets="
             + std::to_string(walkableFacets) + " rooms=" + std::to_string(game.scene_.rooms.size())
             + " doors=" + std::to_string(mesh->doors().size()));
        check(walkableFacets > 0, tag + ".has_walkable_facets");
        check(std::fabs(mesh->walkableArea() - facetArea) < 0.001f * facetArea + 0.01f,
              tag + ".door_cuts_preserve_area");
        bool ccw = true;
        for (const nav::Tri& tri : mesh->tris()) {
            ccw = ccw && nav::triCross(tri.v[0], tri.v[1], tri.v[2]) > 0.0f;
        }
        check(ccw, tag + ".triangles_ccw");
        bool symmetric = true;
        int badLinks = 0;
        int seamLinks = 0;
        for (std::size_t t = 0; t < mesh->tris().size(); ++t) {
            const nav::Tri& tri = mesh->tris()[t];
            for (int l = 0; l < tri.linkCount; ++l) {
                const nav::Link& link = mesh->links()[static_cast<std::size_t>(tri.firstLink + l)];
                const nav::Tri& other = mesh->tris()[static_cast<std::size_t>(link.to)];
                bool back = false;
                for (int k = 0; k < other.linkCount; ++k) {
                    back = back
                        || mesh->links()[static_cast<std::size_t>(other.firstLink + k)].to
                            == static_cast<int>(t);
                }
                symmetric = symmetric && back;
                // The neighbour really is on the side the funnel assumes.
                const bool oriented = nav::triCross(link.p, link.q, nav::centroid(tri)) > 0.0f;
                symmetric = symmetric && oriented;
                if ((!back || !oriented) && badLinks++ < 4) {
                    info(tag + " bad link tri " + std::to_string(t) + " back=" + std::to_string(back ? 1 : 0)
                         + " oriented=" + std::to_string(oriented ? 1 : 0) + " area2=" + std::to_string(nav::triCross(tri.v[0], tri.v[1], tri.v[2]))
                         + " cross=" + std::to_string(nav::triCross(link.p, link.q, nav::centroid(tri)))
                         + " p=" + std::to_string(link.p.x) + "," + std::to_string(link.p.y) + " q=" + std::to_string(link.q.x) + "," + std::to_string(link.q.y)
                         + " to=" + std::to_string(link.to));
                }
                seamLinks += tri.room != other.room ? 1 : 0;
            }
        }
        check(symmetric, tag + ".links_symmetric_and_oriented");
        check(seamLinks >= 2 * (static_cast<int>(game.scene_.rooms.size()) - 1) / 2,
              tag + ".rooms_stitched(" + std::to_string(seamLinks) + ")");
        // No walkable triangle may come from a material surfacemat.2da forbids.
        bool materialsRespected = true;
        for (const kotor::CollisionFacet& facet : game.scene_.collision) {
            if (facet.walkable) {
                continue;
            }
            const Vec3& a = game.scene_.collisionVertices[facet.a];
            const Vec3& b = game.scene_.collisionVertices[facet.b];
            const Vec3& c = game.scene_.collisionVertices[facet.c];
            const Vec3 mid{(a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f};
            const int on = mesh->locate(mid);
            if (on >= 0 && std::fabs(mesh->height(on, mid.x, mid.y) - mid.z) < 0.01f
                && facet.floor) {
                materialsRespected = false;
            }
        }
        check(materialsRespected, tag + ".nonwalkable_faces_not_in_mesh");

        // ---- the shipped PTH graph against the walkmesh --------------------------
        std::vector<PthNode> pth;
        if (auto data = game.install_.find(game.scene_.area, 3003); data.has_value()) {
            const kotor::Gff gff = kotor::Gff::parse(std::move(*data), game.scene_.area + ".pth");
            std::vector<int> destinations;
            if (const kotor::GffField* list = gff.field(gff.rootIndex(), "Path_Conections")) {
                for (const std::uint32_t row : gff.list(*list)) {
                    const kotor::GffField* field = gff.field(row, "Destination");
                    destinations.push_back(field != nullptr ? gff.integer(*field) : -1);
                }
            }
            if (const kotor::GffField* list = gff.field(gff.rootIndex(), "Path_Points")) {
                for (const std::uint32_t row : gff.list(*list)) {
                    PthNode node;
                    const kotor::GffField* x = gff.field(row, "X");
                    const kotor::GffField* y = gff.field(row, "Y");
                    node.point.x = x != nullptr ? gff.number(*x) : 0.0f;
                    node.point.y = y != nullptr ? gff.number(*y) : 0.0f;
                    const kotor::GffField* count = gff.field(row, "Conections");
                    const kotor::GffField* first = gff.field(row, "First_Conection");
                    const int n = count != nullptr ? gff.integer(*count) : 0;
                    const int f = first != nullptr ? gff.integer(*first) : 0;
                    for (int i = 0; i < n && f + i < static_cast<int>(destinations.size()); ++i) {
                        node.neighbours.push_back(destinations[static_cast<std::size_t>(f + i)]);
                    }
                    pth.push_back(node);
                }
            }
        }
        check(pth.size() >= 2, tag + ".pth_loaded(" + std::to_string(pth.size()) + ")");
        // PTH is 2D. Every node takes the height of its containing triangle closest
        // to the previous level; several storeys are resolved by the edge walk below.
        int onMesh = 0;
        for (PthNode& node : pth) {
            node.point.z = game.scene_.entry.z;
            node.tri = mesh->locate(node.point);
            if (node.tri >= 0) {
                node.point.z = mesh->height(node.tri, node.point.x, node.point.y);
                ++onMesh;
            }
        }
        check(onMesh == static_cast<int>(pth.size()),
              tag + ".pth_nodes_on_walkmesh(" + std::to_string(onMesh) + "/" + std::to_string(pth.size()) + ")");
        std::vector<char> allOpen(closed.size(), 0);
        int edges = 0;
        int edgesConnected = 0;
        for (const PthNode& node : pth) {
            for (const int n : node.neighbours) {
                if (n < 0 || n >= static_cast<int>(pth.size())) {
                    continue;
                }
                ++edges;
                const nav::PathResult route = mesh->findPath(
                    node.point, pth[static_cast<std::size_t>(n)].point, 0.35f, allOpen);
                edgesConnected += route.status == nav::PathStatus::Reached ? 1 : 0;
            }
        }
        check(edges > 0 && edges == edgesConnected,
              tag + ".pth_edges_connected_doors_open(" + std::to_string(edgesConnected) + "/"
                  + std::to_string(edges) + ")");

        // ---- the longest route between PTH nodes ---------------------------------
        const auto farthestFrom = [&](const PthNode& source, nav::PathResult* out) {
            std::size_t best = 0;
            float bestLength = -1.0f;
            for (std::size_t i = 0; i < pth.size(); ++i) {
                nav::PathResult route = mesh->findPath(source.point, pth[i].point, 0.35f, allOpen);
                if (route.status == nav::PathStatus::Reached && route.length > bestLength) {
                    bestLength = route.length;
                    best = i;
                    if (out != nullptr) {
                        *out = std::move(route);
                    }
                }
            }
            return best;
        };
        const std::size_t endA = farthestFrom(pth.front(), nullptr);
        nav::PathResult longest;
        const std::size_t endB = farthestFrom(pth[endA], &longest);
        std::set<int> roomsOnRoute;
        for (const int tri : longest.corridor) {
            roomsOnRoute.insert(mesh->tris()[static_cast<std::size_t>(tri)].room);
        }
        info(tag + " longest route length=" + std::to_string(longest.length) + " points="
             + std::to_string(longest.points.size()) + " corridor=" + std::to_string(longest.corridor.size())
             + " rooms=" + std::to_string(roomsOnRoute.size()));
        check(roomsOnRoute.size() >= 3, tag + ".route_crosses_3_rooms");
        bool segmentsClear = true;
        float polyline = 0.0f;
        for (std::size_t i = 1; i < longest.points.size(); ++i) {
            const nav::RayResult ray = mesh->ray(longest.points[i - 1], mesh->locate(longest.points[i - 1]),
                                                 longest.points[i], allOpen);
            segmentsClear = segmentsClear && ray.clear;
            polyline += nav::length3D(longest.points[i - 1], longest.points[i]);
        }
        check(segmentsClear, tag + ".funnel_segments_stay_on_mesh");
        check(polyline + 1.0e-2f >= nav::length3D(pth[endA].point, pth[endB].point),
              tag + ".route_not_shorter_than_straight_line");

        // ---- timing: the real cost of a long query. The bound below is this gate's
        // choice (a tenth of a 60 Hz frame), not a retail figure.
        {
            const auto built0 = Clock::now();
            const nav::NavMesh rebuilt(game.scene_, buildDoorLines(game.scene_, game.install_),
                                       buildPlaceableWalls(game.scene_, game.install_));
            const double buildMs = std::chrono::duration<double, std::milli>(Clock::now() - built0).count();
            double worst = 0.0;
            double total = 0.0;
            constexpr int kRuns = 2000;
            for (int run = 0; run < kRuns; ++run) {
                const auto t0 = Clock::now();
                const nav::PathResult route = mesh->findPath(pth[endA].point, pth[endB].point, 0.35f, allOpen);
                const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
                worst = std::max(worst, us);
                total += us;
                if (route.points.empty()) {
                    worst = 1.0e9;
                }
            }
            char line[200];
            std::snprintf(line, sizeof line, "%s timing: mesh build %.2f ms, long query avg %.1f us, max %.1f us (%d runs, length %.1f)",
                          tag.c_str(), buildMs, total / kRuns, worst, kRuns, longest.length);
            info(line);
            check(worst < 1667.0, tag + ".long_query_under_tenth_of_frame");
        }

        // ---- walk it, all doors open ---------------------------------------------
        for (kotor::Entity& entity : game.scene_.entities) {
            if (entity.kind == kotor::EntityKind::Door) {
                entity.open = true;
            }
        }
        game.navMesh();
        game.position_ = pth[endA].point;
        const float run = game.creatureMoveRate(game.scriptWorld_.pc, true);
        const float expected = longest.length / std::max(run, 0.1f);
        const Walk walk = walkPc(pth[endB].point, 0.3f, true, expected * 1.5f + 10.0f);
        const float finalGap = length2D(game.position(), pth[endB].point);
        char line[240];
        std::snprintf(line, sizeof line,
                      "%s walk: frames=%d seconds=%.2f expected=%.2f travelled=%.1f route=%.1f maxStep=%.4f limit=%.4f rooms=%zu gap=%.2f",
                      tag.c_str(), walk.frames, walk.frames * kFrame, expected, walk.travelled,
                      longest.length, walk.maxStep, run * kFrame, walk.rooms.size(), finalGap);
        info(line);
        info(tag + " walk ended at " + std::to_string(game.position().x) + "," + std::to_string(game.position().y)
             + " target " + std::to_string(pth[endB].point.x) + "," + std::to_string(pth[endB].point.y));
        check(game.moveOutcome() == GameSession::MoveOutcome::Arrived, tag + ".walk_arrived");
        check(finalGap <= 0.5f, tag + ".walk_reached_target");
        check(walk.jumps == 0, tag + ".walk_no_teleport(" + std::to_string(walk.jumps) + ")");
        check(walk.offFloor == 0, tag + ".walk_every_step_on_floor(" + std::to_string(walk.offFloor) + ")");
        check(walk.rooms.size() >= 3, tag + ".walk_visited_3_rooms");
        check(walk.frames * kFrame <= expected * 1.25f + 1.0f, tag + ".walk_at_table_speed");

        // ---- a closed door blocks, an opened one passes ---------------------------
        for (kotor::Entity& entity : game.scene_.entities) {
            if (entity.kind == kotor::EntityKind::Door) {
                entity.open = false;
                entity.locked = false;
            }
        }
        mesh = game.navMesh();
        int doorIndex = -1;
        Vec3 sideA;
        Vec3 sideB;
        int linkedDoorLines = 0;
        int fromDwk = 0;
        for (std::size_t d = 0; d < mesh->doors().size(); ++d) {
            const nav::DoorLine& door = mesh->doors()[d];
            fromDwk += door.fromDwk ? 1 : 0;
            bool hasLink = false;
            for (const nav::Link& link : mesh->links()) {
                hasLink = hasLink || link.doors[0] == static_cast<int>(d) || link.doors[1] == static_cast<int>(d);
            }
            linkedDoorLines += hasLink ? 1 : 0;
            if (doorIndex >= 0 || !hasLink || !game.scene_.entities[door.entity].linkedModule.empty()) {
                continue;
            }
            const Vec3 mid = nav::lerp(door.a, door.b, 0.5f);
            const float span = length2D(door.a, door.b);
            const Vec3 normal{-(door.b.y - door.a.y) / span, (door.b.x - door.a.x) / span, 0.0f};
            const Vec3 a = mid + normal * 1.6f;
            const Vec3 b = mid - normal * 1.6f;
            const int triA = mesh->locate(a);
            const int triB = mesh->locate(b);
            if (triA < 0 || triB < 0) {
                continue;
            }
            const nav::PathResult route = mesh->findPath(a, b, 0.35f, allOpen);
            if (route.status == nav::PathStatus::Reached && route.length < 4.0f) {
                doorIndex = static_cast<int>(d);
                sideA = {a.x, a.y, mesh->height(triA, a.x, a.y)};
                sideB = {b.x, b.y, mesh->height(triB, b.x, b.y)};
            }
        }
        info(tag + " door lines from DWK=" + std::to_string(fromDwk) + "/" + std::to_string(mesh->doors().size())
             + ", sealing a walkable gap=" + std::to_string(linkedDoorLines));
        check(fromDwk == static_cast<int>(mesh->doors().size()), tag + ".all_doors_have_dwk");
        // Placeable walkmeshes (W046, 00506650): the non-walk faces stop a step. A body walking at a face
        // from either side stops short of it, by about its radius, and does not get through.
        {
            constexpr float kBody = 0.35f;
            int tested = 0;
            int crossed = 0;
            int stoppedShort = 0;
            int skipped = 0;
            int offFloor = 0;
            int noHit = 0;
            int crowded = 0;
            for (const nav::DoorLine& wall : mesh->walls()) {
                const float wallSpan = length2D(wall.a, wall.b);
                if (wallSpan < 0.3f || tested >= 40) {
                    continue;
                }
                const Vec3 wallMid = nav::lerp(wall.a, wall.b, 0.5f);
                const Vec3 wallNormal{-(wall.b.y - wall.a.y) / wallSpan, (wall.b.x - wall.a.x) / wallSpan, 0.0f};
                // Candidate starts: on floor, clear of every face by more than a body (a crate's faces sit
                // closer than that to each other), from the face's normal and from either side of it.
                bool onFloor = false;
                bool anyHit = false;
                bool done = false;
                for (const float turn : {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.4f, -1.4f}) {
                    const float cs = std::cos(turn);
                    const float sn = std::sin(turn);
                    const Vec3 dir{wallNormal.x * cs - wallNormal.y * sn, wallNormal.x * sn + wallNormal.y * cs, 0.0f};
                    for (const float sign : {1.0f, -1.0f}) {
                        for (const float back : {0.9f, 1.4f, 2.0f, 2.8f}) {
                            if (done) {
                                break;
                            }
                            const Vec3 sideP = wallMid + dir * (sign * back);
                            const int triP = mesh->locate(sideP);
                            if (triP < 0 || std::fabs(mesh->height(triP, sideP.x, sideP.y) - wallMid.z) > 0.6f) {
                                continue;
                            }
                            onFloor = true;
                            bool clear = true;
                            for (const nav::DoorLine& other : mesh->walls()) {
                                if (length2D(sideP, nav::closestOnSegment2D(sideP, other.a, other.b)) <= kBody + 0.05f) {
                                    clear = false;
                                    break;
                                }
                            }
                            if (!clear) {
                                continue;
                            }
                            // The far end may lie inside a crate, where there is no floor: only the start needs it.
                            const Vec3 sideQ = wallMid - dir * (sign * 0.9f);
                            // The first face the line from P to Q meets (any placeable face): the step must stop
                            // before that one.
                            const nav::DoorLine* hit = nullptr;
                            float hitT = 2.0f;
                            for (const nav::DoorLine& other : mesh->walls()) {
                                if (!crossesSegment(sideP, sideQ, other.a, other.b)) {
                                    continue;
                                }
                                const float dP = nav::triCross(other.a, other.b, sideP);
                                const float dQ = nav::triCross(other.a, other.b, sideQ);
                                const float t = dP / (dP - dQ);
                                if (t < hitT) {
                                    hitT = t;
                                    hit = &other;
                                }
                            }
                            if (hit == nullptr) {
                                continue;
                            }
                            anyHit = true;
                            // Every sample of the run up to the stop point stays more than a body from every other
                            // face; faces that crowd the run would stop the step first, and that is their own test.
                            const float hitSpan = length2D(hit->a, hit->b);
                            if (hitSpan <= 0.01f) {
                                continue;
                            }
                            const Vec3 hitPoint = nav::lerp(sideP, sideQ, hitT);
                            // The run ends where the body stops, a body radius before the face.
                            const float runLength = length2D(sideP, hitPoint);
                            const float runEnd = runLength > kBody ? (runLength - kBody) / runLength : 0.0f;
                            bool free = true;
                            for (int k = 0; k <= 12 && free; ++k) {
                                const Vec3 sample = nav::lerp(sideP, hitPoint, runEnd * static_cast<float>(k) / 12.0f);
                                for (const nav::DoorLine& other : mesh->walls()) {
                                    // The same face again (a box side is several triangles' edges on one line).
                                    const bool sameLine = std::fabs(nav::triCross(hit->a, hit->b, other.a)) / hitSpan <= 0.02f
                                        && std::fabs(nav::triCross(hit->a, hit->b, other.b)) / hitSpan <= 0.02f;
                                    if (&other == hit || sameLine) {
                                        continue;
                                    }
                                    if (length2D(sample, nav::closestOnSegment2D(sample, other.a, other.b)) <= kBody + 0.02f) {
                                        free = false;
                                        break;
                                    }
                                }
                            }
                            if (!free) {
                                continue;
                            }
                            done = true;
                            ++tested;
                            const nav::RayResult step = mesh->stepAgainstWalls(sideP, triP, sideQ, kBody, allOpen, false);
                            const float sideStart = nav::triCross(hit->a, hit->b, sideP);
                            const float sideEnd = nav::triCross(hit->a, hit->b, step.end);
                            if (step.clear || sideStart * sideEnd < 0.0f) {
                                ++crossed;
                            }
                            const float gap = std::fabs(sideEnd) / hitSpan;
                            if (!step.clear && gap > 0.0f && gap <= kBody + 0.01f) {
                                ++stoppedShort;
                            }
                        }
                    }
                }
                if (!done) {
                    ++skipped;
                    ++(onFloor ? (anyHit ? crowded : noHit) : offFloor);
                }
            }
            info(tag + " placeable walls=" + std::to_string(mesh->walls().size()) + " tested=" + std::to_string(tested)
                 + " skipped=" + std::to_string(skipped) + " (off floor " + std::to_string(offFloor) + ", no hit "
                 + std::to_string(noHit) + ", crowded " + std::to_string(crowded) + ") crossed=" + std::to_string(crossed)
                 + " stopped_short=" + std::to_string(stoppedShort));
            check(crossed == 0, tag + ".placeable_walls_stop_a_step");
            check(tested >= 1, tag + ".placeable_walls_tested_not_vacuous");
            check(tested == 0 || stoppedShort * 10 >= tested * 9, tag + ".placeable_walls_stop_a_body_by_its_radius");
        }
        check(doorIndex >= 0, tag + ".found_interior_door");
        if (doorIndex >= 0) {
            const nav::DoorLine door = mesh->doors()[static_cast<std::size_t>(doorIndex)];
            const auto sideOf = [&](Vec3 p) { return nav::triCross(door.a, door.b, p); };
            info(tag + " door under test: " + game.scene_.entities[door.entity].tag);
            game.position_ = sideA;
            const nav::PathResult blocked = mesh->findPath(sideA, sideB, 0.35f, game.nav_->closed);
            check(blocked.status == nav::PathStatus::DoorBlocked || blocked.length > 4.0f,
                  tag + ".closed_door_blocks_or_detours");
            bool crosses = false;
            for (std::size_t i = 1; i < blocked.points.size(); ++i) {
                crosses = crosses || crossesSegment(blocked.points[i - 1], blocked.points[i], door.a, door.b);
            }
            check(!crosses, tag + ".route_never_crosses_closed_door");
            std::size_t blocker = static_cast<std::size_t>(-1);
            const bool direct = game.pathExists(sideA, sideB, &blocker);
            if (blocked.status == nav::PathStatus::DoorBlocked) {
                check(!direct && blocker == door.entity, tag + ".pathExists_names_the_door");

                // Walk to it: stops on this side and reports the door.
                const Walk toDoor = walkPc(sideB, 0.3f, false, 15.0f);
                check(game.moveOutcome() == GameSession::MoveOutcome::DoorBlocked
                          && game.moveBlockingDoor() == door.entity,
                      tag + ".walk_stops_at_closed_door");
                check(sideOf(game.position()) * sideOf(sideA) > 0.0f, tag + ".walk_stayed_on_near_side");
                check(toDoor.jumps == 0 && toDoor.offFloor == 0, tag + ".walk_to_door_on_floor");

                // Push into it with the keyboard for two seconds.
                game.position_ = sideA;
                game.faceTowards(sideB);
                InputState push;
                push.forward = true;
                Walk pushing;
                for (int i = 0; i < 120; ++i) {
                    const Vec3 before = game.position();
                    game.update(kFrame, push);
                    checkFrame(pushing, before, game.position(), game.creatureMoveRate(game.scriptWorld_.pc, false));
                }
                check(sideOf(game.position()) * sideOf(sideA) > 0.0f, tag + ".keyboard_stopped_by_closed_door");
                check(length2D(game.position(), nav::closestOnSegment2D(game.position(), door.a, door.b)) >= 0.3f,
                      tag + ".body_held_off_door(" + std::to_string(length2D(
                          game.position(), nav::closestOnSegment2D(game.position(), door.a, door.b))) + ")");
                check(pushing.offFloor == 0, tag + ".keyboard_on_floor");

                // Locked refuses, unlocked opens.
                game.scene_.entities[door.entity].locked = true;
                check(!game.openDoorAsPlayer(door.entity), tag + ".locked_door_refuses");
                game.scene_.entities[door.entity].locked = false;
                check(game.openDoorAsPlayer(door.entity) && game.scene_.entities[door.entity].open,
                      tag + ".door_opens");
                // The open state counts once the opening clip is done (retail 00683f00 -> 00589680): a door still
                // opening blocks like a closed one, so the path exists only after the clip has run.
                game.position_ = sideA;
                const bool blockedWhileOpening = !doorStillOpening(game.scene_, door.entity)
                    || !game.pathExists(sideA, sideB);
                check(blockedWhileOpening, tag + ".opening_door_still_blocks");
                for (int i = 0; i < 600 && doorStillOpening(game.scene_, door.entity); ++i) {
                    game.update(kFrame, InputState{});
                }
                check(!doorStillOpening(game.scene_, door.entity), tag + ".opening_clip_finishes");
                game.navMesh();
                check(game.pathExists(sideA, sideB), tag + ".open_door_has_path");
                game.position_ = sideA;
                const Walk through = walkPc(sideB, 0.3f, false, 15.0f);
                check(game.moveOutcome() == GameSession::MoveOutcome::Arrived, tag + ".walks_through_open_door");
                check(sideOf(game.position()) * sideOf(sideB) > 0.0f, tag + ".reached_far_side");
                check(through.jumps == 0 && through.offFloor == 0, tag + ".through_door_on_floor");
                // Closing it again blocks the same query.
                game.scene_.entities[door.entity].open = false;
                game.navMesh();
                check(!game.pathExists(sideA, sideB), tag + ".reclosed_door_blocks");
            }
        }

        // ---- targets that cannot be stood on -------------------------------------
        for (kotor::Entity& entity : game.scene_.entities) {
            if (entity.kind == kotor::EntityKind::Door) {
                entity.open = true;
            }
        }
        mesh = game.navMesh();
        game.position_ = pth[endA].point;
        // A point well outside every room.
        Vec3 offMesh = game.scene_.collisionVertices.front();
        for (const Vec3& v : game.scene_.collisionVertices) {
            offMesh.x = std::max(offMesh.x, v.x);
            offMesh.y = std::max(offMesh.y, v.y);
        }
        offMesh.x += 40.0f;
        offMesh.y += 40.0f;
        offMesh.z = game.position_.z;
        const nav::PathResult adjusted = mesh->findPath(game.position_, offMesh, 0.35f, game.nav_->closed);
        check(adjusted.status == nav::PathStatus::Adjusted, tag + ".offmesh_target_adjusted");
        Vec3 nearestPoint;
        mesh->nearest(offMesh, nearestPoint);
        std::snprintf(line, sizeof line, "%s off-mesh target: end is %.2f from the nearest walkable point, path length %.1f",
                      tag.c_str(), length2D(adjusted.points.back(), nearestPoint), adjusted.length);
        info(line);
        check(!game.pathExists(game.position_, offMesh), tag + ".offmesh_is_not_a_path");
        const Walk toEdge = walkPc(offMesh, 0.2f, true, adjusted.length / run * 1.5f + 5.0f);
        check(game.moveOutcome() == GameSession::MoveOutcome::Arrived
                  && length2D(game.position(), adjusted.points.back()) < 0.5f,
              tag + ".offmesh_walks_to_nearest_point");
        check(toEdge.jumps == 0 && toEdge.offFloor == 0, tag + ".offmesh_walk_on_floor");
        // A walkable island the actor cannot reach, where the module has one.
        int islandTri = -1;
        const int here = mesh->locate(game.position_);
        for (std::size_t t = 0; t < mesh->tris().size() && islandTri < 0; ++t) {
            Vec3 c = nav::centroid(mesh->tris()[t]);
            if (mesh->findPath(game.position_, c, 0.35f, game.nav_->closed).status == nav::PathStatus::Unreachable) {
                islandTri = static_cast<int>(t);
            }
        }
        info(tag + " unreachable island triangle: " + std::to_string(islandTri) + " (from " + std::to_string(here) + ")");
        if (islandTri >= 0) {
            const Vec3 before = game.position();
            const Vec3 island = nav::centroid(mesh->tris()[static_cast<std::size_t>(islandTri)]);
            check(!game.moveTo(island, 0.3f, true), tag + ".island_refused");
            check(game.moveOutcome() == GameSession::MoveOutcome::Unreachable && !game.movePending(),
                  tag + ".island_reported_unreachable");
            game.update(kFrame, InputState{});
            check(length2D(before, game.position()) < 1.0e-3f, tag + ".island_no_movement");
        }

        // ---- speed from the tables ----------------------------------------------
        std::size_t creatureEntity = static_cast<std::size_t>(-1);
        std::size_t secondEntity = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < game.scene_.entities.size(); ++i) {
            const kotor::Entity& entity = game.scene_.entities[i];
            if (entity.kind == kotor::EntityKind::Creature && !entity.dead && i != game.playerEntity_
                && game.creatureMoveRate(game.objectForEntity(i), true) > 1.0f) {
                if (creatureEntity == static_cast<std::size_t>(-1)) {
                    creatureEntity = i;
                } else if (secondEntity == static_cast<std::size_t>(-1)) {
                    secondEntity = i;
                }
            }
        }
        if (auto data = game.install_.find("creaturespeed", 2017); data.has_value()) {
            const kotor::TwoDa table = kotor::TwoDa::parse(std::move(*data), "creaturespeed.2da");
            const int name = columnNoCase(table, "2daname");
            const int runColumn = columnNoCase(table, "runrate");
            const int walkColumn = columnNoCase(table, "walkrate");
            float tableRun = -1.0f;
            float tableWalk = -1.0f;
            for (std::size_t row = 0; row < table.rowCount(); ++row) {
                if (table.cell(row, static_cast<std::size_t>(name)) == "PLAYER") {
                    tableRun = std::stof(table.cell(row, static_cast<std::size_t>(runColumn)));
                    tableWalk = std::stof(table.cell(row, static_cast<std::size_t>(walkColumn)));
                }
            }
            check(std::fabs(run - tableRun) < 1.0e-4f
                      && std::fabs(game.creatureMoveRate(game.scriptWorld_.pc, false) - tableWalk) < 1.0e-4f,
                  tag + ".pc_speed_is_creaturespeed_row");
        }

        // ---- two creatures in each other's way; force-move timeout ----------------
        if (secondEntity != static_cast<std::size_t>(-1)) {
            const std::uint32_t mover = game.objectForEntity(creatureEntity);
            const std::uint32_t standing = game.objectForEntity(secondEntity);
            // Along the longest route: the mover's way runs straight through the other.
            const Vec3 start = longest.points.front();
            const Vec3 goal = longest.points[std::min<std::size_t>(1, longest.points.size() - 1)];
            const float legLength = length2D(start, goal);
            api.setPosition(mover, start);
            api.setPosition(standing, nav::lerp(start, goal, 0.5f));
            api.clearQueue(mover);
            api.clearQueue(standing);
            ActorAction walkPast;
            walkPast.kind = ActorActionKind::MoveToPoint;
            walkPast.point = goal;
            walkPast.run = true;
            api.enqueue(mover, std::move(walkPast));
            const float rate = game.creatureMoveRate(mover, true);
            const float allowed = legLength / rate + 8.0f;
            int frames = 0;
            while (frames < static_cast<int>(allowed / kFrame) && game.actorQueueSize(mover) > 0) {
                game.update(kFrame, InputState{});
                ++frames;
            }
            std::snprintf(line, sizeof line, "%s avoidance: leg=%.1f rate=%.2f finished in %.2f s, gap to goal %.2f",
                          tag.c_str(), legLength, rate, frames * kFrame, length2D(api.position(mover), goal));
            info(line);
            check(game.actorQueueSize(mover) == 0 && length2D(api.position(mover), goal) < 0.5f,
                  tag + ".creatures_do_not_stick");

            // Retail 0051f4f0: "Force Timeout" moves the actor to the point. Before the
            // timeout it must still be walking.
            const Vec3 farPoint = pth[endB].point;
            api.setPosition(mover, pth[endA].point);
            api.setPosition(standing, pth[endB].point);
            ActorAction forced;
            forced.kind = ActorActionKind::MoveToPoint;
            forced.point = farPoint;
            forced.force = true;
            forced.remaining = 0.5f;
            api.enqueue(mover, std::move(forced));
            for (int i = 0; i < 12; ++i) {
                game.update(kFrame, InputState{});
            }
            const bool stillWalking = game.actorQueueSize(mover) > 0
                && length2D(api.position(mover), farPoint) > 1.0f;
            for (int i = 0; i < 30; ++i) {
                game.update(kFrame, InputState{});
            }
            check(longest.length > 6.0f && stillWalking, tag + ".force_move_walks_before_timeout");
            check(game.actorQueueSize(mover) == 0 && length2D(api.position(mover), farPoint) < 0.5f,
                  tag + ".force_move_jumps_on_timeout");
            api.setPosition(standing, pth[endB].point + Vec3{50.0f, 50.0f, 0.0f});
        }

        // ---- the party follows across rooms ---------------------------------------
        game.position_ = pth[endA].point;
        game.yaw_ = 0.0f;
        if (game.companions_.empty()) {
            check(game.addPartyMember("Carth"), tag + ".party_member_added");
        }
        game.placePartyAtEntry();
        const std::size_t follower = game.companions_.empty()
            ? static_cast<std::size_t>(-1)
            : game.ensureCompanionEntity(game.companions_.front());
        game.placePartyAtEntry();
        check(follower != static_cast<std::size_t>(-1), tag + ".companion_present");
        if (follower != static_cast<std::size_t>(-1)) {
            nav::NavMesh* current = game.navMesh();
            const int startTriangle = current->locate(game.scene_.entities[follower].position);
            check(startTriangle >= 0, tag + ".entry_placement_on_floor");
            check(length2D(game.scene_.entities[follower].position, game.position_) < 6.0f,
                  tag + ".entry_placement_near_leader");
            const float speedLimit = game.creatureMoveRate(game.objectForEntity(follower), true) * 1.5f;
            Walk followed;
            const float expectedSeconds = longest.length / run;
            game.moveTo(pth[endB].point, 0.3f, true);
            int frames = 0;
            const int maxFrames = static_cast<int>((expectedSeconds * 1.5f + 15.0f) / kFrame);
            while (frames < maxFrames) {
                const Vec3 before = game.scene_.entities[follower].position;
                game.update(kFrame, InputState{});
                checkFrame(followed, before, game.scene_.entities[follower].position, speedLimit);
                ++frames;
                if (!game.movePending()
                    && length2D(game.scene_.entities[follower].position, game.position()) < 4.0f) {
                    break;
                }
            }
            const float gap = length2D(game.scene_.entities[follower].position, game.position());
            std::snprintf(line, sizeof line,
                          "%s follow: seconds=%.2f (leader expected %.2f) gap=%.2f rooms=%zu travelled=%.1f maxStep=%.4f",
                          tag.c_str(), frames * kFrame, expectedSeconds, gap, followed.rooms.size(),
                          followed.travelled, followed.maxStep);
            info(line);
            check(gap < 4.0f, tag + ".companion_caught_up");
            check(followed.rooms.size() >= 3, tag + ".companion_crossed_rooms");
            check(followed.jumps == 0, tag + ".companion_no_teleport(" + std::to_string(followed.jumps) + ")");
            check(followed.offFloor == 0, tag + ".companion_on_floor(" + std::to_string(followed.offFloor) + ")");
        }
    }
    game.quietWorldForTest_ = false;
    std::printf("pathfinding gate: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace ke
