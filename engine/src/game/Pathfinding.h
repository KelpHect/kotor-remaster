#pragma once

// Walkmesh navigation: every actor's path planning and collision.
//
// Retail plans on the module's PTH waypoint graph (loaded by 00508400; nearest
// usable node 004bd770; neighbour expansion and A* 004bdb00/004c2200), then checks
// straight segments against the room walkmeshes. This engine plans on the walkmesh
// triangles instead, which keep the retail constraints (walkable materials from
// surfacemat.2da, room seams, closed doors) and need no second graph. The family
// gate checks the PTH graph against it, node by node, so a disagreement with what
// retail would walk shows up as a failure.

#include "kotor/Formats.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ke {

class GameSession;

namespace nav {

// A door's closed walkmesh reduced to its centre line, in world space.
struct DoorLine {
    std::size_t entity = 0;
    kotor::Vec3 a;
    kotor::Vec3 b;
    float zMin = 0.0f;
    float zMax = 0.0f;
    bool fromDwk = false; // False: the documented model-bounds fallback.
};

struct Link {
    int to = -1;
    kotor::Vec3 p; // Portal end points. Link::to lies to the right of p -> q.
    kotor::Vec3 q;
    int doors[2] = {-1, -1}; // Door lines covering this portal.
};

struct Tri {
    kotor::Vec3 v[3]; // Counter-clockwise seen from above.
    int firstLink = 0;
    int linkCount = 0;
    int room = 0;
    bool fromCut = false;
};

enum class PathStatus {
    Reached,     // A path to the target itself.
    Adjusted,    // The target is off the walkmesh; the path ends at the nearest reachable point.
    DoorBlocked, // A closed door is in the way; the path stops on this side of it.
    Unreachable, // No connection. The path ends at the closest point that can be reached.
    NoMesh,
};

struct PathResult {
    PathStatus status = PathStatus::NoMesh;
    std::vector<kotor::Vec3> points;
    std::vector<int> corridor;   // Triangles walked, in order.
    std::vector<int> doorsCrossed; // Door lines the path goes through (open at query time).
    int blockingDoor = -1;       // Door line index when DoorBlocked.
    float length = 0.0f;
};

struct RayResult {
    bool clear = false;
    kotor::Vec3 end;
    int endTri = -1;
    int hitDoor = -1;  // Door line index when a closed door stopped the ray.
    kotor::Vec3 edge;  // Direction of the edge that stopped it.
};

class NavMesh {
public:
    NavMesh(const kotor::Scene& scene, std::vector<DoorLine> doors);

    bool empty() const { return tris_.empty(); }
    const std::vector<Tri>& tris() const { return tris_; }
    const std::vector<Link>& links() const { return links_; }
    const std::vector<DoorLine>& doors() const { return doors_; }
    float walkableArea() const;

    // The triangle under (x, y) closest in height to z, or -1.
    int locate(kotor::Vec3 point) const;
    float height(int tri, float x, float y) const;
    // Closest point on any triangle (height differences count half).
    int nearest(kotor::Vec3 point, kotor::Vec3& out) const;

    // Marches a straight line over the triangles. `closed` is indexed by door line.
    RayResult ray(kotor::Vec3 from, int fromTri, kotor::Vec3 to,
                  const std::vector<char>& closed) const;
    PathResult findPath(kotor::Vec3 from, kotor::Vec3 to, float radius,
                        const std::vector<char>& closed) const;

private:
    void cutAlong(const DoorLine& door);
    void buildLinks();
    void buildGrid();
    bool containsPoint(int tri, float x, float y) const;
    bool search(int start, int goal, kotor::Vec3 from, kotor::Vec3 to, float radius,
                const std::vector<char>* closed, std::vector<int>& corridor,
                std::vector<int>& viaLink, std::vector<char>* reached) const;
    void funnel(const std::vector<int>& corridor, const std::vector<int>& viaLink,
                kotor::Vec3 from, kotor::Vec3 to, float radius, PathResult& out) const;

    std::vector<Tri> tris_;
    std::vector<Link> links_;
    std::vector<DoorLine> doors_;
    float gridMinX_ = 0.0f;
    float gridMinY_ = 0.0f;
    int gridW_ = 0;
    int gridH_ = 0;
    std::vector<std::vector<int>> grid_;
};

// An actor's current route, kept between frames.
struct ActorPath {
    PathResult path;
    std::size_t next = 1;
    kotor::Vec3 target;
    std::uint64_t doorVersion = 0;
    float contact = 0.0f;
    float ghost = 0.0f;
    bool valid = false;
};

// Per-session navigation state. Meshes are built on first use, per module, and
// kept for the session: the geometry of a module never changes.
struct Runtime {
    std::map<std::string, std::shared_ptr<NavMesh>> cache;
    std::shared_ptr<NavMesh> mesh;
    std::map<std::uint32_t, ActorPath> actors;
    std::vector<char> closed;
    std::uint64_t doorVersion = 1;
    bool playerStepped = false;

    bool tablesLoaded = false;
    std::vector<float> walkRates; // By creaturespeed.2da row.
    std::vector<float> runRates;
    std::vector<std::string> rateNames;
    std::map<std::uint32_t, int> appearanceRateRow;
    std::map<std::uint32_t, float> personalSpace; // appearance.2da PERSPACE.
    std::map<std::uint32_t, float> creatureSpace; // appearance.2da CREPERSPACE.
};

} // namespace nav

// The family gate, behind --family-test pathfinding.
int runPathfindingFamilyTest(GameSession& game);

} // namespace ke
