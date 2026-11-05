// Walkmesh navigation. See Pathfinding.h for how this relates to retail.
#include "game/Pathfinding.h"

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


// Room seams in the shipped walkmeshes share exact vertices (measured on the
// shipped modules: every BWM perimeter edge with a room transition matches its
// neighbour at 1 mm), so this only absorbs float noise from door cuts.
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

NavMesh::NavMesh(const kotor::Scene& scene, std::vector<DoorLine> doors)
    : doors_(std::move(doors))
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
        const float area = triCross(tri.v[0], tri.v[1], tri.v[2]);
        if (std::fabs(area) < kMinArea) {
            continue; // Vertical in plan view: nothing can stand on it.
        }
        if (area < 0.0f) {
            std::swap(tri.v[1], tri.v[2]);
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
    // T-junctions from a door cut against an uncut neighbour: edges that overlap
    // along a line but do not share both end points.
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
            if (!a.fromCut && !b.fromCut) {
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
constexpr float kContactBeforeGhost = 1.0f;
constexpr float kGhostSeconds = 2.0f;
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

} // namespace

nav::NavMesh* GameSession::navMesh()
{
    if (!nav_) {
        nav_ = std::make_unique<nav::Runtime>();
    }
    if (!nav_->mesh) {
        std::shared_ptr<nav::NavMesh>& cached = nav_->cache[module_];
        if (!cached) {
            cached = std::make_shared<nav::NavMesh>(scene_, buildDoorLines(scene_, install_));
        }
        nav_->mesh = cached;
        nav_->closed.assign(nav_->mesh->doors().size(), 0);
        ++nav_->doorVersion;
    }
    const std::vector<nav::DoorLine>& doors = nav_->mesh->doors();
    for (std::size_t d = 0; d < doors.size(); ++d) {
        const std::size_t index = doors[d].entity;
        const bool closed = index < scene_.entities.size() && !scene_.entities[index].open
            && !scene_.entities[index].dead;
        if ((nav_->closed[d] != 0) != closed) {
            nav_->closed[d] = closed ? 1 : 0;
            ++nav_->doorVersion;
        }
    }
    return nav_->mesh.get();
}

void GameSession::loadMovementTables()
{
    navMesh();
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
        for (std::size_t row = 0; row < table.rowCount(); ++row) {
            const auto id = static_cast<std::uint32_t>(row);
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
            if (creature >= 0) {
                nav_->creatureSpace[id] = parseCell(table.cell(row, static_cast<std::size_t>(creature)),
                                                    kDefaultPersonalSpace);
            }
        }
    }
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
                                       float dt, float stopDistance)
{
    ScriptApi api{*this};
    const Vec3 from = api.position(object);
    const float straight = length2D(from, target);
    nav::NavMesh* mesh = navMesh();

    const auto finish = [&](ActorStep step, MoveOutcome outcome, std::size_t door) {
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
        api.setPosition(object, next, std::atan2(target.y - from.y, target.x - from.x));
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
    route.ghost = std::max(0.0f, route.ghost - dt);
    const float radius = creatureRadius(object);
    bool replan = !route.valid || route.doorVersion != nav_->doorVersion
        || length2D(route.target, target) > kRepathTargetMove;
    if (!replan && route.next < route.path.points.size()) {
        // Pushed or jumped off the route.
        const Vec3 near = nav::closestOnSegment2D(from, route.path.points[route.next - 1],
                                                  route.path.points[route.next]);
        replan = length2D(near, from) > kRouteDriftLimit;
    }
    if (replan) {
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
    // The margin absorbs float error left by the last stride, which is sized to end
    // exactly at the stop distance.
    if (reachable && remaining <= std::max(stopDistance, 0.02f) + 0.005f) {
        return finish(ActorStep::Arrived, MoveOutcome::Arrived, kNoDoor);
    }
    // Within range but not routed: what is wanted may stand behind the closed door
    // or on an island, and the actor has come as far as it can.
    if (!reachable && straight <= stopDistance) {
        return finish(ActorStep::Arrived, MoveOutcome::Arrived, kNoDoor);
    }
    if (!reachable && remaining <= 0.02f) {
        return path.status == nav::PathStatus::DoorBlocked
            ? finish(ActorStep::Blocked, MoveOutcome::DoorBlocked, blockingEntity)
            : finish(ActorStep::Blocked, MoveOutcome::Unreachable, kNoDoor);
    }

    // Where this frame's stride along the route ends.
    const float stride = std::min(speed * dt,
                                  reachable ? remaining - std::max(stopDistance, 0.0f) : remaining);
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
                            std::atan2(onto.y - from.y, onto.x - from.x));
            if (object == scriptWorld_.pc) {
                nav_->playerStepped = true;
            }
            return finish(ActorStep::Moving, MoveOutcome::Moving, kNoDoor);
        }
    }

    // Other creatures. Retail 004bdb00 detours around whatever stands on an edge,
    // at both bodies' personal space; here it is a local steer around them. After
    // a second of contact the actor stops yielding for two, so scripted moves
    // always finish. Those two times are this engine's, not retail's.
    const std::size_t selfEntity = object == scriptWorld_.pc ? playerEntity_ : api.entityFor(object);
    std::size_t bumped = kNoDoor;
    const auto blockedAt = [&](Vec3 point) {
        if (route.ghost > 0.0f) {
            return false;
        }
        const auto against = [&](Vec3 other, float otherRadius) {
            const float combined = radius + otherRadius;
            if (length2D(target, other) < combined) {
                return false; // Walking up to something standing on the target.
            }
            const float there = length2D(point, other);
            return there < combined && there < length2D(from, other) - 1.0e-4f;
        };
        if (object != scriptWorld_.pc && against(position_, kPlayerPersonalSpace)) {
            return true;
        }
        for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
            const kotor::Entity& other = scene_.entities[i];
            if (other.kind != kotor::EntityKind::Creature || other.dead || i == selfEntity
                || i == playerEntity_) {
                continue;
            }
            const auto space = nav_->personalSpace.find(other.appearance);
            if (against(other.position,
                        space != nav_->personalSpace.end() ? space->second : kDefaultPersonalSpace)) {
                bumped = i;
                return true;
            }
        }
        return false;
    };
    bool detoured = false;
    if (blockedAt(desired)) {
        const float heading = std::atan2(desired.y - from.y, desired.x - from.x);
        const float reach = length2D(from, desired);
        bool found = false;
        for (const float turn : {1.0f, -1.0f, 1.8f, -1.8f}) {
            const Vec3 aside{from.x + std::cos(heading + turn) * reach,
                             from.y + std::sin(heading + turn) * reach, from.z};
            if (blockedAt(aside)) {
                continue;
            }
            const nav::RayResult probe = mesh->ray(from, startTri, aside, nav_->closed);
            if (probe.clear) {
                desired = probe.end;
                found = true;
                detoured = true;
                break;
            }
        }
        route.contact += dt;
        if (route.contact >= kContactBeforeGhost) {
            // GetBlockingCreature 00546e00. When retail fills it was not traced; a second
            // of contact is this engine's moment.
            if (bumped != kNoDoor) {
                api.actor(object).blockingCreature = objectForEntity(bumped);
                if (object != controlledObject()) {
                    api.raiseHook(object, "Blocked");
                }
            }
            route.ghost = kGhostSeconds;
            route.contact = 0.0f;
        }
        if (!found) {
            return finish(ActorStep::Moving, MoveOutcome::Moving, kNoDoor);
        }
    } else {
        route.contact = std::max(0.0f, route.contact - dt);
    }

    const nav::RayResult walked = mesh->ray(from, startTri, desired, nav_->closed);
    const float moved = length2D(from, walked.end);
    if (!walked.clear) {
        route.valid = false;
    }
    if (detoured) {
        route.valid = false;
    } else if (walked.clear) {
        route.next = nextIndex;
    }
    if (moved < 1.0e-4f) {
        return finish(ActorStep::Blocked, MoveOutcome::Moving, kNoDoor);
    }
    api.setPosition(object, walked.end, std::atan2(walked.end.y - from.y, walked.end.x - from.x));
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
        first = mesh->ray(from, tri, to, nav_->closed);
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
        const nav::RayResult second = mesh->ray(end, endTri, slid, nav_->closed);
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
    stepActorToward(self, slot, creatureMoveRate(self, run) * scale, dt, kFormationArrive);
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
                symmetric = symmetric && nav::triCross(link.p, link.q, nav::centroid(tri)) > 0.0f;
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
            const nav::NavMesh rebuilt(game.scene_, buildDoorLines(game.scene_, game.install_));
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
            const float allowed = legLength / rate + kContactBeforeGhost + kGhostSeconds + 5.0f;
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
