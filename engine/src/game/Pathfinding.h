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

#include <array>
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
    std::uint8_t seamEdges = 0; // Bit i: the edge from v[i] leads into another room.
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

// Retail 005d0840 detours a creature round whatever it bumped by a hexagon about the blocker (005cf1f0): the
// walker's route is cut where it enters and leaves the hexagon and the way round (005cf7a0 forward, 005cf650
// backward) is spliced in (005cecc0).
struct HexCrossing {
    std::size_t segment = 0; // Index of the route point that ends the crossed segment.
    int edge = 0;            // Hexagon edge i runs from vertex i to vertex i + 1.
    kotor::Vec3 point;
};

struct Hexagon {
    std::array<kotor::Vec3, 6> vertex;

    // 005cfc70: on the inner side of every edge.
    bool contains(kotor::Vec3 point) const;
    // Where the polyline enters and leaves, in route order (005d0160, with both crossings of one segment kept).
    std::vector<HexCrossing> crossings(const std::vector<kotor::Vec3>& polyline) const;
    // 005cfae0.
    int nearestVertex(kotor::Vec3 point) const;
    // 005d0690: the points of the way round from the entry to the exit.
    std::vector<kotor::Vec3> arc(const HexCrossing& entry, const HexCrossing& exit, bool forward) const;
};

// The first vertex lies on the walker's side of the blocker and the others follow clockwise (the sixth turn is
// 60 degrees); the inscribed circle has the given radius.
Hexagon makeHexagon(kotor::Vec3 centre, float walkerFacing, float apothem);

class NavMesh {
public:
    // `walls` are the non-walkable faces of the placeable walkmeshes (retail 00506650 NoNonWalkPolysDetailed,
    // 00585350 CSWSPlaceable::NoNonWalkPolys). They do not touch the mesh: a step is tested against them
    // (stepAgainstWalls), as retail tests each step of a creature.
    NavMesh(const kotor::Scene& scene, std::vector<DoorLine> doors, std::vector<DoorLine> walls = {});

    bool empty() const { return tris_.empty(); }
    const std::vector<Tri>& tris() const { return tris_; }
    const std::vector<Link>& links() const { return links_; }
    const std::vector<DoorLine>& doors() const { return doors_; }
    const std::vector<DoorLine>& walls() const { return walls_; }
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
    // ray() for a body of `radius`: the step also stops where the body would touch a placeable's non-walk
    // face (00506650 tests the step segment with the creature's radius against those faces). A body
    // already closer than its radius may move parallel to the face or away, not closer. With `slide`
    // the rest of the step continues along the face. `edge` is the face's direction when it stopped the step.
    RayResult stepAgainstWalls(kotor::Vec3 from, int fromTri, kotor::Vec3 to, float radius,
                               const std::vector<char>& closed, bool slide) const;

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
    std::vector<DoorLine> walls_;
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
    // Time left on the pathfinding wait a walker stands out at a door that is not open yet.
    float waitLeft = 0.0f;
    bool valid = false;
    // Retail CSWSCreature path data: +0x268 bump counter, +0x254 and +0x258 the creature and spot of the last bump,
    // +0x244 which way round a blocker the next detour goes first (005d0840).
    int bumps = 0;
    bool hasLastBump = false;
    std::uint32_t lastBump = 0;
    kotor::Vec3 lastBumpPosition;
    bool detourToggle = false;
    float abortLogLeft = 0.0f; // Quiet time before the next abort is logged.
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
    std::map<std::uint32_t, float> cameraSpace;   // appearance.2da CAMERASPACE, CREPERSPACE when empty (004ed6e0, +0x340 +0x10).
    std::map<std::uint32_t, float> bodyHeight;    // appearance.2da HEIGHT.
    // appearance.2da DRIVEACCL / DRIVEMAXSPEED / DRIVEANIMWALK / DRIVEANIMRUN: the keyboard-driven body's acceleration and
    // top speed, and the walk/run speeds its clips are cut for (client appearance record +0x58/+0x5c/+0x60/+0x64).
    struct DriveRow {
        float accel = 50.0f;
        float maxSpeed = 5.4f;
        float walk = 1.7f;
        float run = 5.4f;
    };
    std::map<std::uint32_t, DriveRow> drive;
    // The velocity (x, y) the drive controller carries for a controlled creature (CSWPlayerControlCamRelative).
    std::map<std::uint32_t, std::array<float, 2>> driveVelocity;
    // WalkUpdateLocation_QuickWalk's speed factor (creature +0xb8) of a creature walking an action's path.
    struct WalkFactor {
        float factor = 0.0f;
        float scale = 1.0f; // (factor + last factor) / 2 of the last step: measured speed over the table speed.
        float lastStep = -100.0f; // Session clock of the last step: a walk that starts after a pause starts the ramp over.
    };
    std::map<std::uint32_t, WalkFactor> walkFactor;
};

} // namespace nav

// The family gate, behind --family-test pathfinding.
int runPathfindingFamilyTest(GameSession& game);

} // namespace ke
