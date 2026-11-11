#include "kotor/Formats.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <sstream>

namespace ke::kotor {

Lyt parseLyt(std::vector<std::uint8_t> bytes, const std::string& name)
{
    Lyt lyt;
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::istringstream input(text);
    std::string line;
    enum class Mode { None, Room, Door } mode = Mode::None;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::size_t begin = line.find_first_not_of(" \t");
        if (begin == std::string::npos) {
            continue;
        }
        std::size_t end = line.find_last_not_of(" \t");
        const std::string s = line.substr(begin, end - begin + 1);
        if (s.empty() || s[0] == '#') {
            continue;
        }
        std::string lower = s;
        for (char& ch : lower) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (lower.rfind("roomcount", 0) == 0) {
            mode = Mode::Room;
            continue;
        }
        if (lower.rfind("doorhookcount", 0) == 0) {
            mode = Mode::Door;
            continue;
        }
        if (lower.rfind("trackcount", 0) == 0 || lower.rfind("obstaclecount", 0) == 0
            || lower.rfind("donelayout", 0) == 0 || lower.rfind("beginlayout", 0) == 0) {
            mode = Mode::None;
            continue;
        }
        std::istringstream row(s);
        if (mode == Mode::Room) {
            LytRoom room;
            row >> room.room >> room.origin.x >> room.origin.y >> room.origin.z;
            if (row) {
                lyt.rooms.push_back(std::move(room));
            }
        } else if (mode == Mode::Door) {
            LytHook hook;
            std::string attach;
            float qw = 0.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f;
            row >> hook.room >> hook.node >> attach >> hook.position.x >> hook.position.y >> hook.position.z
                >> qw >> qx >> qy >> qz;
            if (row) {
                lyt.hooks.push_back(std::move(hook));
            }
        }
    }
    if (lyt.rooms.empty()) {
        throw FormatError(name + ": LYT has no rooms");
    }
    return lyt;
}

Wok parseWok(std::vector<std::uint8_t> bytes, const std::string& name)
{
    if (bytes.size() < 72 || std::memcmp(bytes.data(), "BWM V1.0", 8) != 0) {
        throw FormatError(name + ": not a BWM V1.0 walkmesh");
    }
    const std::uint32_t type = readU32(bytes.data(), bytes.size(), 8, "WOK type");
    const std::uint32_t vc = readU32(bytes.data(), bytes.size(), 72, "WOK vertex count");
    const std::uint32_t vo = readU32(bytes.data(), bytes.size(), 76, "WOK vertex offset");
    const std::uint32_t fc = readU32(bytes.data(), bytes.size(), 80, "WOK face count");
    const std::uint32_t fo = readU32(bytes.data(), bytes.size(), 84, "WOK face offset");
    const std::uint32_t mo = readU32(bytes.data(), bytes.size(), 88, "WOK material offset");
    const std::uint32_t no = readU32(bytes.data(), bytes.size(), 92, "WOK normal offset");
    requireRange(bytes.size(), vo, static_cast<std::size_t>(vc) * 12u, "WOK vertices");
    requireRange(bytes.size(), fo, static_cast<std::size_t>(fc) * 12u, "WOK faces");
    if (type == 1 && no != 0) {
        requireRange(bytes.size(), no, static_cast<std::size_t>(fc) * 12u, "WOK normals");
    }
    if (mo != 0) {
        requireRange(bytes.size(), mo, static_cast<std::size_t>(fc) * 4u, "WOK surfaces");
    }
    Wok wok;
    wok.vertices.reserve(vc);
    for (std::uint32_t i = 0; i < vc; ++i) {
        const std::size_t o = vo + static_cast<std::size_t>(i) * 12u;
        wok.vertices.push_back({readF32(bytes.data(), bytes.size(), o, "WOK vx"),
                                readF32(bytes.data(), bytes.size(), o + 4u, "WOK vy"),
                                readF32(bytes.data(), bytes.size(), o + 8u, "WOK vz")});
    }
    wok.faces.reserve(fc);
    for (std::uint32_t i = 0; i < fc; ++i) {
        const std::size_t o = fo + static_cast<std::size_t>(i) * 12u;
        WokFace face;
        // Three u32 indices per face (stride 12; tools/formats/bwm.py).
        face.a = readU32(bytes.data(), bytes.size(), o, "WOK face a");
        face.b = readU32(bytes.data(), bytes.size(), o + 4u, "WOK face b");
        face.c = readU32(bytes.data(), bytes.size(), o + 8u, "WOK face c");
        if (face.a >= vc || face.b >= vc || face.c >= vc) {
            throw FormatError(name + ": WOK face index out of range");
        }
        face.surface = mo != 0 ? readU32(bytes.data(), bytes.size(), mo + static_cast<std::size_t>(i) * 4u, "WOK surface") : 0u;
        if (type == 1 && no != 0) {
            const std::size_t n = no + static_cast<std::size_t>(i) * 12u;
            face.normal = {readF32(bytes.data(), bytes.size(), n, "WOK nx"),
                           readF32(bytes.data(), bytes.size(), n + 4u, "WOK ny"),
                           readF32(bytes.data(), bytes.size(), n + 8u, "WOK nz")};
        } else {
            const Vec3& a = wok.vertices[face.a];
            const Vec3& b = wok.vertices[face.b];
            const Vec3& c = wok.vertices[face.c];
            face.normal = normalize(cross(b - a, c - a));
        }
        face.floor = face.normal.z > 0.5f;
        wok.faces.push_back(face);
    }
    // Perimeter edges, 8 bytes each: edge index (3 * face + side), then the room it leads
    // to or -1 (tools/formats/bwm.py).
    const std::uint32_t ec = readU32(bytes.data(), bytes.size(), 120, "WOK edge count");
    const std::uint32_t eo = readU32(bytes.data(), bytes.size(), 124, "WOK edge offset");
    if (ec != 0) {
        requireRange(bytes.size(), eo, static_cast<std::size_t>(ec) * 8u, "WOK edges");
    }
    for (std::uint32_t i = 0; i < ec; ++i) {
        const std::size_t o = eo + static_cast<std::size_t>(i) * 8u;
        const std::uint32_t edge = readU32(bytes.data(), bytes.size(), o, "WOK edge index");
        const std::uint32_t leadsTo = readU32(bytes.data(), bytes.size(), o + 4u, "WOK edge transition");
        if (leadsTo != 0xFFFFFFFFu && edge / 3u < fc) {
            wok.faces[edge / 3u].seamEdges |= static_cast<std::uint8_t>(1u << (edge % 3u));
        }
    }
    return wok;
}

namespace {

Mat3 identity()
{
    Mat3 r{};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0f;
    return r;
}

Mat3 quatMatrix(float w, float x, float y, float z)
{
    const float n = w * w + x * x + y * y + z * z;
    if (n == 0.0f) {
        return identity();
    }
    const float s = 2.0f / n;
    const float xx = x * x * s;
    const float yy = y * y * s;
    const float zz = z * z * s;
    const float xy = x * y * s;
    const float xz = x * z * s;
    const float yz = y * z * s;
    const float wx = w * x * s;
    const float wy = w * y * s;
    const float wz = w * z * s;
    Mat3 r{};
    r.m[0][0] = 1.0f - yy - zz;
    r.m[0][1] = xy - wz;
    r.m[0][2] = xz + wy;
    r.m[1][0] = xy + wz;
    r.m[1][1] = 1.0f - xx - zz;
    r.m[1][2] = yz - wx;
    r.m[2][0] = xz - wy;
    r.m[2][1] = yz + wx;
    r.m[2][2] = 1.0f - xx - yy;
    return r;
}

Mat3 multiply(const Mat3& a, const Mat3& b)
{
    Mat3 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < 3; ++k) {
                r.m[i][j] += a.m[i][k] * b.m[k][j];
            }
        }
    }
    return r;
}

Vec3 transform(const Mat3& r, Vec3 v)
{
    return {r.m[0][0] * v.x + r.m[0][1] * v.y + r.m[0][2] * v.z,
            r.m[1][0] * v.x + r.m[1][1] * v.y + r.m[1][2] * v.z,
            r.m[2][0] * v.x + r.m[2][1] * v.y + r.m[2][2] * v.z};
}

std::array<float, 4> quatNormalize(const std::array<float, 4>& q)
{
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n <= 0.0f) {
        return {1.0f, 0.0f, 0.0f, 0.0f};
    }
    return {q[0] / n, q[1] / n, q[2] / n, q[3] / n};
}

std::array<float, 4> quatSlerp(std::array<float, 4> a, std::array<float, 4> b, float t)
{
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0.0f) {
        for (float& value : b) {
            value = -value;
        }
        dot = -dot;
    }
    if (dot > 0.9995f) {
        std::array<float, 4> result{};
        for (int i = 0; i < 4; ++i) {
            result[i] = a[i] + (b[i] - a[i]) * t;
        }
        return quatNormalize(result);
    }
    const float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
    const float sinTheta = std::sin(theta);
    const float s0 = std::sin((1.0f - t) * theta) / sinTheta;
    const float s1 = std::sin(t * theta) / sinTheta;
    std::array<float, 4> result{};
    for (int i = 0; i < 4; ++i) {
        result[i] = a[i] * s0 + b[i] * s1;
    }
    return quatNormalize(result);
}

std::string readCString(const std::uint8_t* data, std::size_t size, std::size_t offset,
                        std::size_t maxLength, const char* what, const std::string& name)
{
    if (offset >= size) {
        return {};
    }
    std::string out;
    for (std::size_t i = 0; i < maxLength && offset + i < size; ++i) {
        const char ch = static_cast<char>(data[offset + i]);
        if (ch == '\0') {
            break;
        }
        out.push_back(ch);
    }
    (void)what;
    (void)name;
    return out;
}

std::string lowerAscii(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string hex32(std::uint32_t value)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(8, '0');
    for (int i = 7; i >= 0; --i) {
        text[static_cast<std::size_t>(i)] = digits[value & 0xFu];
        value >>= 4;
    }
    return text;
}

} // namespace

Mdl Mdl::parse(std::vector<std::uint8_t> mdl, std::vector<std::uint8_t> mdx, std::string name)
{
    if (mdl.size() < 12 || readU32(mdl.data(), mdl.size(), 0, "MDL header") != 0) {
        throw FormatError(name + ": MDL first u32 must be 0");
    }
    Mdl model;
    model.name_ = std::move(name);
    model.mdl_ = std::move(mdl);
    model.mdx_ = std::move(mdx);
    const auto& b = model.mdl_;
    const std::uint32_t geometryId = readU32(b.data(), b.size(), 12 + 0, "MDL geometry id");
    // KotOR 2 uses the same MDL container with a different geometry token and a
    // 340-byte trimesh header (8 bytes of dirt fields replacing K1's 2-byte
    // tail). Everything else (node header, name table, animation table) keeps
    // the K1 offsets.
    if (geometryId == 0x413670u) {
        model.k2_ = false;
    } else if (geometryId == 0x416310u) {
        model.k2_ = true;
    } else {
        throw FormatError(model.name_ + ": unknown MDL geometry id 0x"
                          + hex32(geometryId));
    }
    // The byte after classification, subclassification and a pad. Retail turns fog on or off per
    // model while the area has fog on (the mesh draw at 0x4739a7).
    model.affectedByFog_ = b.size() > 12u + 83u ? b[12u + 83u] != 0u : true;
    model.classification_ = b.size() > 12u + 80u ? b[12u + 80u] : 0u;
    model.supermodel_ = readCString(b.data(), b.size(), 12 + 136, 32, "MDL supermodel", model.name_);    const std::uint32_t root = readU32(b.data(), b.size(), 12 + 40, "MDL root");
    // The animation root node: retail 004a1030 relocates this word as a node pointer and 00483d80
    // applies clips only from that node down. Shipped data: the root node on every model except
    // the heads, where it is neck_g.
    const std::uint32_t headRootOffset = readU32(b.data(), b.size(), 12 + 168, "MDL head root");
    const std::uint32_t nameOff = readU32(b.data(), b.size(), 12 + 184, "MDL name offsets");
    const std::uint32_t nameCount = readU32(b.data(), b.size(), 12 + 188, "MDL name count");
    std::vector<std::uint32_t> nameOffsets;
    if (nameCount != 0) {
        requireRange(b.size(), 12u + nameOff, static_cast<std::size_t>(nameCount) * 4u, "MDL name table");
        nameOffsets.reserve(nameCount);
        for (std::uint32_t i = 0; i < nameCount; ++i) {
            nameOffsets.push_back(readU32(b.data(), b.size(),
                                          12u + nameOff + static_cast<std::size_t>(i) * 4u,
                                          "MDL name offset"));
        }
    }
    const auto nameAt = [&](std::uint32_t index) -> std::string {
        if (index >= nameOffsets.size()) {
            return {};
        }
        return readCString(b.data(), b.size(), 12u + nameOffsets[index], 64, "MDL node name", model.name_);
    };

    std::vector<std::uint32_t> stack;
    if (root != 0) {
        stack.push_back(root);
    }
    while (!stack.empty()) {
        const std::uint32_t offset = stack.back();
        stack.pop_back();
        if (model.nodeByOffset_.find(offset) != model.nodeByOffset_.end()) {
            continue;
        }
        const std::size_t a = 12u + offset;
        if (a + 80u > b.size()) {
            throw FormatError(model.name_ + ": MDL node overruns file");
        }
        Node node;
        node.offset = offset;
        node.type = readU16(b.data(), b.size(), a, "MDL node type");
        node.supernode = readU16(b.data(), b.size(), a + 2u, "MDL node supernode");
        node.nodeId = readU16(b.data(), b.size(), a + 4u, "MDL node id");
        node.name = nameAt(node.nodeId);
        node.parent = readU32(b.data(), b.size(), a + 12u, "MDL node parent");
        node.position = {readF32(b.data(), b.size(), a + 16u, "MDL px"),
                         readF32(b.data(), b.size(), a + 20u, "MDL py"),
                         readF32(b.data(), b.size(), a + 24u, "MDL pz")};
        node.qw = readF32(b.data(), b.size(), a + 28u, "MDL qw");
        node.qx = readF32(b.data(), b.size(), a + 32u, "MDL qx");
        node.qy = readF32(b.data(), b.size(), a + 36u, "MDL qy");
        node.qz = readF32(b.data(), b.size(), a + 40u, "MDL qz");
        const std::uint32_t kidsOff = readU32(b.data(), b.size(), a + 44u, "MDL children offset");
        const std::uint32_t kidsCount = readU32(b.data(), b.size(), a + 48u, "MDL children count");
        if (kidsCount != 0) {
            requireRange(b.size(), 12u + kidsOff, static_cast<std::size_t>(kidsCount) * 4u,
                         "MDL children");
            for (std::uint32_t i = 0; i < kidsCount; ++i) {
                node.children.push_back(readU32(b.data(), b.size(),
                                                12u + kidsOff + static_cast<std::size_t>(i) * 4u,
                                                "MDL child"));
            }
        }
        if (node.type & 0x20u) {
            const std::size_t m = a + 80u;
            const std::size_t trimeshBase = model.k2_ ? 340u : 332u;
            node.trimesh = true;
            node.faceOffset = readU32(b.data(), b.size(), m + 8u, "MDL face offset");
            node.faceCount = readU32(b.data(), b.size(), m + 12u, "MDL face count");
            node.texture0 = readCString(b.data(), b.size(), m + 88u, 32, "MDL texture0", model.name_);
            node.texture1 = readCString(b.data(), b.size(), m + 120u, 32, "MDL texture1", model.name_);
            node.material.transparencyHint = static_cast<float>(
                readU32(b.data(), b.size(), m + 84u, "MDL transparency hint"));
            node.material.diffuse = {readF32(b.data(), b.size(), m + 60u, "MDL diffuse"),
                                     readF32(b.data(), b.size(), m + 64u, "MDL diffuse"),
                                     readF32(b.data(), b.size(), m + 68u, "MDL diffuse")};
            node.material.ambient = {readF32(b.data(), b.size(), m + 72u, "MDL ambient"),
                                     readF32(b.data(), b.size(), m + 76u, "MDL ambient"),
                                     readF32(b.data(), b.size(), m + 80u, "MDL ambient")};
            node.material.animateUv = readU32(b.data(), b.size(), m + 232u, "MDL animate uv") != 0;
            node.material.uvDirX = readF32(b.data(), b.size(), m + 236u, "MDL uv dir x");
            node.material.uvDirY = readF32(b.data(), b.size(), m + 240u, "MDL uv dir y");
            node.material.uvJitter = readF32(b.data(), b.size(), m + 244u, "MDL uv jitter");
            node.material.uvJitterSpeed = readF32(b.data(), b.size(), m + 248u, "MDL uv jitter speed");
            node.uv0Offset = readU32(b.data(), b.size(), m + 272u, "MDL uv0 offset");
            node.uv1Offset = readU32(b.data(), b.size(), m + 276u, "MDL uv1 offset");
            // Retail 0043e410 reads this as node +0x170: -1 means no tangent frame.
            node.material.hasTangents = readU32(b.data(), b.size(), m + 288u, "MDL tangent offset") != 0xFFFFFFFFu;
            requireRange(b.size(), m + 313u, 1u, "MDL mesh flags");
            if (b[m + 308u] != 0 && lowerAscii(node.texture1) != "null") {
                node.material.lightmap = lowerAscii(node.texture1);
            }
            node.material.shadow = b[m + 311u] != 0;
            node.material.beaming = b[m + 312u] != 0;
            node.stride = readU32(b.data(), b.size(), m + 252u, "MDL stride");
            node.vertexCount = readU32(b.data(), b.size(), m + 304u, "MDL vertex count") & 0xFFFFu;
            requireRange(b.size(), m + 313u, 1u, "MDL render flag");
            node.render = b[m + 313u] != 0;
            // Retail 0046ee40 draws a mesh on its render flag (node +0x189), alpha and vertex count;
            // a "NULL" texture0 draws untextured (path 0046f900). An AABB node becomes a PartAABB,
            // whose draw 00471560 runs only under a debug flag (007fbf5c, zero-initialised), so a
            // hit mesh never draws but still gives faces to the collision readers.
            if (node.type & 0x200u) {
                node.render = false;
            }
            node.mdxOffset = readU32(b.data(), b.size(),
                                     m + (model.k2_ ? 332u : 324u), "MDL mdx offset");
            if (node.stride < 12u && node.vertexCount != 0 && !(node.type & 0x800u)) {
                throw FormatError(model.name_ + ": MDL vertex stride "
                                  + std::to_string(node.stride));
            }
            requireRange(model.mdx_.size(), node.mdxOffset,
                         static_cast<std::size_t>(node.vertexCount) * node.stride, "MDX vertices");
            requireRange(b.size(), 12u + node.faceOffset,
                         static_cast<std::size_t>(node.faceCount) * 32u, "MDL faces");
            // Stencil shadow caster data (00474220): the shadow or beaming byte, no skin (0x40), not a
            // lightsaber (0x821) and not an AABB hit node (0x200, a PartAABB).
            if ((node.material.shadow || node.material.beaming) && (node.type & 0x40u) == 0u
                && (node.type & 0x821u) != 0x821u && (node.type & 0x200u) == 0u && node.faceCount != 0u) {
                const std::uint32_t coordinates = readU32(b.data(), b.size(), m + (model.k2_ ? 336u : 328u),
                                                          "MDL vertex coordinates");
                if (coordinates != 0xFFFFFFFFu
                    && 12u + static_cast<std::size_t>(coordinates) + static_cast<std::size_t>(node.vertexCount) * 12u
                           <= b.size()) {
                    auto shadow = std::make_shared<ShadowMesh>();
                    shadow->beaming = node.material.beaming;
                    shadow->vertices.reserve(node.vertexCount);
                    for (std::uint32_t i = 0; i < node.vertexCount; ++i) {
                        const std::size_t vo = 12u + coordinates + static_cast<std::size_t>(i) * 12u;
                        shadow->vertices.push_back({readF32(b.data(), b.size(), vo, "MDL coordinate x"),
                                                    readF32(b.data(), b.size(), vo + 4u, "MDL coordinate y"),
                                                    readF32(b.data(), b.size(), vo + 8u, "MDL coordinate z")});
                    }
                    shadow->faces.reserve(node.faceCount);
                    for (std::uint32_t i = 0; i < node.faceCount; ++i) {
                        const std::size_t fo = 12u + node.faceOffset + static_cast<std::size_t>(i) * 32u;
                        ShadowMesh::Face face;
                        face.normal = {readF32(b.data(), b.size(), fo, "MDL face nx"),
                                       readF32(b.data(), b.size(), fo + 4u, "MDL face ny"),
                                       readF32(b.data(), b.size(), fo + 8u, "MDL face nz")};
                        face.distance = readF32(b.data(), b.size(), fo + 12u, "MDL face distance");
                        for (std::size_t k = 0; k < 3u; ++k) {
                            face.adjacent[k] = static_cast<std::int16_t>(
                                readU16(b.data(), b.size(), fo + 20u + k * 2u, "MDL face adjacent"));
                            face.vertex[k] = readU16(b.data(), b.size(), fo + 26u + k * 2u, "MDL face vertex");
                        }
                        shadow->faces.push_back(face);
                    }
                    node.shadowMesh = std::move(shadow);
                }
            }
            if (node.type & 0x800u) {
                const std::size_t sh = m + trimeshBase;
                requireRange(b.size(), sh, 20u, "MDL saber header");
                const std::uint32_t vertexOffset = readU32(b.data(), b.size(), sh, "MDL saber vertices");
                const std::uint32_t uvOffset = readU32(b.data(), b.size(), sh + 4u, "MDL saber uvs");
                const std::uint32_t normalOffset = readU32(b.data(), b.size(), sh + 8u, "MDL saber normals");
                Saber saber;
                requireRange(b.size(), 12u + vertexOffset,
                             static_cast<std::size_t>(node.vertexCount) * 12u, "MDL saber vertex data");
                requireRange(b.size(), 12u + uvOffset,
                             static_cast<std::size_t>(node.vertexCount) * 8u, "MDL saber uv data");
                requireRange(b.size(), 12u + normalOffset,
                             static_cast<std::size_t>(node.vertexCount) * 12u, "MDL saber normal data");
                for (std::uint32_t i = 0; i < node.vertexCount; ++i) {
                    const std::size_t vo = 12u + vertexOffset + static_cast<std::size_t>(i) * 12u;
                    const std::size_t uo = 12u + uvOffset + static_cast<std::size_t>(i) * 8u;
                    const std::size_t no = 12u + normalOffset + static_cast<std::size_t>(i) * 12u;
                    saber.positions.push_back({readF32(b.data(), b.size(), vo, "MDL saber x"),
                                               readF32(b.data(), b.size(), vo + 4u, "MDL saber y"),
                                               readF32(b.data(), b.size(), vo + 8u, "MDL saber z")});
                    saber.uvs.push_back({readF32(b.data(), b.size(), uo, "MDL saber u"),
                                         readF32(b.data(), b.size(), uo + 4u, "MDL saber v")});
                    saber.normals.push_back({readF32(b.data(), b.size(), no, "MDL saber nx"),
                                             readF32(b.data(), b.size(), no + 4u, "MDL saber ny"),
                                             readF32(b.data(), b.size(), no + 8u, "MDL saber nz")});
                }
                node.saber = std::move(saber);
                node.material.saber = true;
            } else if (node.type & 0x100u) {
                const std::size_t dh = m + trimeshBase;
                requireRange(b.size(), dh, 28u, "MDL dangly header");
                const std::uint32_t constraintOffset = readU32(b.data(), b.size(), dh, "MDL dangly constraints");
                const std::uint32_t constraintCount = readU32(b.data(), b.size(), dh + 4u, "MDL dangly count");
                Dangly dangly;
                dangly.displacement = readF32(b.data(), b.size(), dh + 12u, "MDL dangly displacement");
                dangly.tightness = readF32(b.data(), b.size(), dh + 16u, "MDL dangly tightness");
                dangly.period = readF32(b.data(), b.size(), dh + 20u, "MDL dangly period");
                requireRange(b.size(), 12u + constraintOffset,
                             static_cast<std::size_t>(constraintCount) * 4u, "MDL dangly constraints data");
                dangly.constraints.reserve(constraintCount);
                for (std::uint32_t i = 0; i < constraintCount; ++i) {
                    dangly.constraints.push_back(readF32(
                        b.data(), b.size(), 12u + constraintOffset + i * 4u, "MDL dangly constraint"));
                }
                node.dangly = std::move(dangly);
            } else if (node.type & 0x40u) {
                const std::size_t sh = m + trimeshBase;
                requireRange(b.size(), sh, 100u, "MDL skin header");
                Skin skin;
                skin.weightsOffset = readU32(b.data(), b.size(), sh + 12u, "MDL skin weights");
                skin.bonesOffset = readU32(b.data(), b.size(), sh + 16u, "MDL skin bones");
                const std::uint32_t boneMapOffset = readU32(b.data(), b.size(), sh + 20u, "MDL bonemap");
                const std::uint32_t boneMapCount = readU32(b.data(), b.size(), sh + 24u, "MDL bonemap count");
                const std::uint32_t qboneOffset = readU32(b.data(), b.size(), sh + 28u, "MDL qbones");
                const std::uint32_t qboneCount = readU32(b.data(), b.size(), sh + 32u, "MDL qbone count");
                const std::uint32_t tboneOffset = readU32(b.data(), b.size(), sh + 40u, "MDL tbones");
                const std::uint32_t tboneCount = readU32(b.data(), b.size(), sh + 44u, "MDL tbone count");
                requireRange(b.size(), 12u + boneMapOffset,
                             static_cast<std::size_t>(boneMapCount) * 4u, "MDL bonemap data");
                requireRange(b.size(), 12u + qboneOffset,
                             static_cast<std::size_t>(qboneCount) * 16u, "MDL qbone data");
                requireRange(b.size(), 12u + tboneOffset,
                             static_cast<std::size_t>(tboneCount) * 12u, "MDL tbone data");
                skin.boneMap.reserve(boneMapCount);
                for (std::uint32_t i = 0; i < boneMapCount; ++i) {
                    skin.boneMap.push_back(static_cast<std::int32_t>(
                        readF32(b.data(), b.size(), 12u + boneMapOffset + i * 4u, "MDL bonemap value")));
                }
                skin.qbones.reserve(qboneCount);
                for (std::uint32_t i = 0; i < qboneCount; ++i) {
                    const std::size_t o = 12u + qboneOffset + static_cast<std::size_t>(i) * 16u;
                    skin.qbones.push_back({readF32(b.data(), b.size(), o, "MDL qbone w"),
                                           readF32(b.data(), b.size(), o + 4u, "MDL qbone x"),
                                           readF32(b.data(), b.size(), o + 8u, "MDL qbone y"),
                                           readF32(b.data(), b.size(), o + 12u, "MDL qbone z")});
                }
                skin.tbones.reserve(tboneCount);
                for (std::uint32_t i = 0; i < tboneCount; ++i) {
                    const std::size_t o = 12u + tboneOffset + static_cast<std::size_t>(i) * 12u;
                    skin.tbones.push_back({readF32(b.data(), b.size(), o, "MDL tbone x"),
                                           readF32(b.data(), b.size(), o + 4u, "MDL tbone y"),
                                           readF32(b.data(), b.size(), o + 8u, "MDL tbone z")});
                }
                node.skin = std::move(skin);
            }
        }
        if (node.type & 0x02u) {
            // Light header, 92 bytes after the node header.
            const std::size_t l = a + 80u;
            requireRange(b.size(), l, 92u, "MDL light header");
            MdlLight light;
            light.flareRadius = readF32(b.data(), b.size(), l, "MDL flare radius");
            light.priority = static_cast<int>(readU32(b.data(), b.size(), l + 64u, "MDL light priority"));
            light.ambientOnly = readU32(b.data(), b.size(), l + 68u, "MDL light ambient") != 0;
            light.dynamicType = static_cast<int>(readU32(b.data(), b.size(), l + 72u, "MDL light dynamic"));
            light.affectDynamic = readU32(b.data(), b.size(), l + 76u, "MDL light affect dynamic") != 0;
            light.shadow = readU32(b.data(), b.size(), l + 80u, "MDL light shadow") != 0;
            // +88 = node +0xa8, read by 004a3f70 as the fade-in flag.
            light.fading = readU32(b.data(), b.size(), l + 88u, "MDL light fading") != 0;
            const std::uint32_t sizeOffset = readU32(b.data(), b.size(), l + 16u, "MDL flare sizes");
            const std::uint32_t sizeCount = readU32(b.data(), b.size(), l + 20u, "MDL flare size count");
            const std::uint32_t positionOffset = readU32(b.data(), b.size(), l + 28u, "MDL flare positions");
            const std::uint32_t shiftOffset = readU32(b.data(), b.size(), l + 40u, "MDL flare shifts");
            const std::uint32_t nameOffset = readU32(b.data(), b.size(), l + 52u, "MDL flare names");
            const std::uint32_t flareNames = readU32(b.data(), b.size(), l + 56u, "MDL flare name count");
            if (sizeCount != 0 && sizeCount == flareNames && sizeCount < 64u) {
                for (std::uint32_t i = 0; i < sizeCount; ++i) {
                    MdlLight::Flare flare;
                    flare.size = readF32(b.data(), b.size(), 12u + sizeOffset + i * 4u, "MDL flare size");
                    flare.position = readF32(b.data(), b.size(), 12u + positionOffset + i * 4u, "MDL flare position");
                    const std::size_t shift = 12u + shiftOffset + static_cast<std::size_t>(i) * 12u;
                    flare.colourShift = {readF32(b.data(), b.size(), shift, "MDL flare r"),
                                         readF32(b.data(), b.size(), shift + 4u, "MDL flare g"),
                                         readF32(b.data(), b.size(), shift + 8u, "MDL flare b")};
                    const std::uint32_t textureOffset = readU32(b.data(), b.size(), 12u + nameOffset + i * 4u, "MDL flare name");
                    flare.texture = readCString(b.data(), b.size(), 12u + textureOffset, 32, "MDL flare texture", model.name_);
                    light.flares.push_back(std::move(flare));
                }
            }
            node.light = std::move(light);
        }
        if (node.type & 0x04u) {
            // Emitter header, 224 bytes after the node header.
            const std::size_t e = a + 80u;
            requireRange(b.size(), e, 224u, "MDL emitter header");
            MdlEmitter emitter;
            emitter.deadSpace = readF32(b.data(), b.size(), e, "MDL emitter dead space");
            emitter.blastRadius = readF32(b.data(), b.size(), e + 4u, "MDL emitter blast radius");
            emitter.blastLength = readF32(b.data(), b.size(), e + 8u, "MDL emitter blast length");
            emitter.branchCount = readU32(b.data(), b.size(), e + 12u, "MDL emitter branches");
            emitter.smoothing = readF32(b.data(), b.size(), e + 16u, "MDL emitter smoothing");
            emitter.gridX = static_cast<std::int32_t>(readU32(b.data(), b.size(), e + 20u, "MDL emitter grid x"));
            emitter.gridY = static_cast<std::int32_t>(readU32(b.data(), b.size(), e + 24u, "MDL emitter grid y"));
            emitter.spawnType = static_cast<std::int32_t>(readU32(b.data(), b.size(), e + 28u, "MDL emitter spawn"));
            emitter.update = readCString(b.data(), b.size(), e + 32u, 32, "MDL emitter update", model.name_);
            emitter.render = readCString(b.data(), b.size(), e + 64u, 32, "MDL emitter render", model.name_);
            emitter.blend = readCString(b.data(), b.size(), e + 96u, 32, "MDL emitter blend", model.name_);
            emitter.texture = readCString(b.data(), b.size(), e + 128u, 32, "MDL emitter texture", model.name_);
            emitter.chunk = readCString(b.data(), b.size(), e + 160u, 16, "MDL emitter chunk", model.name_);
            emitter.twoSided = readU32(b.data(), b.size(), e + 176u, "MDL emitter two sided") != 0;
            emitter.loop = readU32(b.data(), b.size(), e + 180u, "MDL emitter loop") != 0;
            emitter.renderOrder = readU16(b.data(), b.size(), e + 184u, "MDL emitter order");
            emitter.frameBlend = b[e + 186u] != 0;
            emitter.flags = readU32(b.data(), b.size(), e + 220u, "MDL emitter flags");
            node.emitter = std::move(emitter);
        }
        node.controllers = model.parseControllers(static_cast<std::uint32_t>(a));
        model.nodeByOffset_[offset] = model.nodes_.size();
        model.nodeById_[node.nodeId] = model.nodes_.size();
        for (const std::uint32_t child : node.children) {
            stack.push_back(child);
        }
        model.nodes_.push_back(std::move(node));
    }

    const std::uint32_t animationOffset = readU32(b.data(), b.size(), 12 + 88, "MDL animation table");
    const std::uint32_t animationCount = readU32(b.data(), b.size(), 12 + 92, "MDL animation count");
    if (animationCount != 0 && animationOffset != 0) {
        requireRange(b.size(), 12u + animationOffset,
                     static_cast<std::size_t>(animationCount) * 4u, "MDL animation offsets");
        for (std::uint32_t i = 0; i < animationCount; ++i) {
            const std::uint32_t offset = readU32(b.data(), b.size(),
                                                 12u + animationOffset + i * 4u,
                                                 "MDL animation offset");
            Animation animation = model.parseAnimation(offset);
            model.animations_.push_back({animation.name, animation.duration, animation.transition});
            model.animationData_.push_back(std::make_shared<const Animation>(std::move(animation)));
        }
    }
    for (Node& node : model.nodes_) {
        if (node.trimesh) {
            const auto hasAlpha = [](const std::vector<Controller>& controllers) {
                return std::any_of(controllers.begin(), controllers.end(),
                                   [](const Controller& c) { return c.type == 132; });
            };
            bool fades = hasAlpha(node.controllers);
            for (const auto& animation : model.animationData_) {
                for (const Node& animated : animation->nodes) {
                    fades = fades || (animated.supernode == node.supernode
                                      && hasAlpha(animated.controllers));
                }
            }
            node.material.fades = fades;
            node.material.noFog = !model.affectedByFog_;
        }
        if (node.light.has_value()) {
            MdlLight& light = *node.light;
            for (const Controller& controller : node.controllers) {
                const std::vector<float> value = sampleController(controller, 0.0f);
                if (controller.type == 76 && value.size() >= 3) {
                    light.colour = {value[0], value[1], value[2]};
                } else if (controller.type == 88 && !value.empty()) {
                    light.radius = value[0];
                } else if (controller.type == 140 && !value.empty()) {
                    light.multiplier = value[0];
                } else if (controller.type == 96 && !value.empty()) {
                    light.shadowRadius = value[0];
                } else if (controller.type == 100 && !value.empty()) {
                    light.verticalDisplacement = value[0];
                }
            }
        }
    }
    model.bindWorld_ = model.poseWorld(nullptr, 0.0f);
    model.underHeadRoot_.assign(model.nodes_.size(), false);
    if (const auto headRoot = model.nodeByOffset_.find(headRootOffset); headRoot != model.nodeByOffset_.end()) {
        model.headRootName_ = model.nodes_[headRoot->second].name;
        for (std::size_t i = 0; i < model.nodes_.size(); ++i) {
            std::size_t at = i;
            for (std::size_t guard = 0; guard < model.nodes_.size(); ++guard) {
                if (at == headRoot->second) {
                    model.underHeadRoot_[i] = true;
                    break;
                }
                const auto parent = model.nodeByOffset_.find(model.nodes_[at].parent);
                if (parent == model.nodeByOffset_.end()) {
                    break;
                }
                at = parent->second;
            }
        }
    }
    model.resolveSkins();
    return model;
}

void Mdl::resolveSkins()
{
    for (Node& node : nodes_) {
        if (!node.skin.has_value()) {
            continue;
        }
        Skin& skin = *node.skin;
        std::int32_t maxSlot = -1;
        for (const std::int32_t slot : skin.boneMap) {
            maxSlot = std::max(maxSlot, slot);
        }
        skin.slotNode.assign(static_cast<std::size_t>(maxSlot + 1), -1);
        for (std::size_t nodeNumber = 0; nodeNumber < skin.boneMap.size(); ++nodeNumber) {
            const std::int32_t slot = skin.boneMap[nodeNumber];
            if (slot < 0) {
                continue;
            }
            const auto it = nodeById_.find(static_cast<std::uint16_t>(nodeNumber));
            if (it != nodeById_.end()) {
                skin.slotNode[static_cast<std::size_t>(slot)] =
                    static_cast<std::int32_t>(it->second);
            }
        }
    }
}

std::vector<Mdl::Node> Mdl::parseNodes(std::uint32_t rootOffset) const
{
    std::vector<Node> nodes;
    std::unordered_map<std::uint32_t, std::size_t> seen;
    std::vector<std::uint32_t> stack;
    if (rootOffset != 0) {
        stack.push_back(rootOffset);
    }
    while (!stack.empty()) {
        const std::uint32_t offset = stack.back();
        stack.pop_back();
        if (seen.find(offset) != seen.end()) {
            continue;
        }
        const std::size_t a = 12u + offset;
        if (a + 80u > mdl_.size()) {
            throw FormatError(name_ + ": MDL animation node overruns file");
        }
        Node node;
        node.offset = offset;
        node.type = readU16(mdl_.data(), mdl_.size(), a, "MDL node type");
        node.supernode = readU16(mdl_.data(), mdl_.size(), a + 2u, "MDL node supernode");
        node.nodeId = readU16(mdl_.data(), mdl_.size(), a + 4u, "MDL node id");
        node.parent = readU32(mdl_.data(), mdl_.size(), a + 12u, "MDL node parent");
        node.position = {readF32(mdl_.data(), mdl_.size(), a + 16u, "MDL px"),
                         readF32(mdl_.data(), mdl_.size(), a + 20u, "MDL py"),
                         readF32(mdl_.data(), mdl_.size(), a + 24u, "MDL pz")};
        node.qw = readF32(mdl_.data(), mdl_.size(), a + 28u, "MDL qw");
        node.qx = readF32(mdl_.data(), mdl_.size(), a + 32u, "MDL qx");
        node.qy = readF32(mdl_.data(), mdl_.size(), a + 36u, "MDL qy");
        node.qz = readF32(mdl_.data(), mdl_.size(), a + 40u, "MDL qz");
        const std::uint32_t kidsOff = readU32(mdl_.data(), mdl_.size(), a + 44u, "MDL children offset");
        const std::uint32_t kidsCount = readU32(mdl_.data(), mdl_.size(), a + 48u, "MDL children count");
        if (kidsCount != 0) {
            requireRange(mdl_.size(), 12u + kidsOff, static_cast<std::size_t>(kidsCount) * 4u,
                         "MDL children");
            for (std::uint32_t i = 0; i < kidsCount; ++i) {
                node.children.push_back(readU32(mdl_.data(), mdl_.size(),
                                                12u + kidsOff + static_cast<std::size_t>(i) * 4u,
                                                "MDL child"));
            }
        }
        node.controllers = parseControllers(static_cast<std::uint32_t>(a));
        seen[offset] = nodes.size();
        for (const std::uint32_t child : node.children) {
            stack.push_back(child);
        }
        nodes.push_back(std::move(node));
    }
    return nodes;
}

std::vector<Mdl::Controller> Mdl::parseControllers(std::uint32_t nodeHeader) const
{
    std::vector<Controller> out;
    const std::uint32_t controllersOffset = readU32(mdl_.data(), mdl_.size(), nodeHeader + 56u,
                                                    "MDL controller offset");
    const std::uint32_t controllerCount = readU32(mdl_.data(), mdl_.size(), nodeHeader + 60u,
                                                  "MDL controller count");
    const std::uint32_t dataOffset = readU32(mdl_.data(), mdl_.size(), nodeHeader + 68u,
                                             "MDL controller data");
    if (controllerCount == 0 || controllersOffset == 0) {
        return out;
    }
    requireRange(mdl_.size(), 12u + controllersOffset,
                 static_cast<std::size_t>(controllerCount) * 16u, "MDL controller array");
    out.reserve(controllerCount);
    const std::uint8_t* bytes = mdl_.data();
    for (std::uint32_t i = 0; i < controllerCount; ++i) {
        const std::size_t co = 12u + controllersOffset + static_cast<std::size_t>(i) * 16u;
        Controller controller;
        controller.type = static_cast<std::int32_t>(readU32(bytes, mdl_.size(), co, "MDL controller type"));
        const std::uint16_t rows = readU16(bytes, mdl_.size(), co + 6u, "MDL controller rows");
        const std::uint16_t keyOffset = readU16(bytes, mdl_.size(), co + 8u, "MDL controller keys");
        const std::uint16_t valueOffset = readU16(bytes, mdl_.size(), co + 10u, "MDL controller data");
        const std::uint8_t columns = bytes[co + 12u];
        controller.bezier = (columns & 0x10u) != 0;
        const std::size_t baseColumns = controller.bezier ? (columns & ~0x10u) : columns;
        const std::size_t keyBase = 12u + dataOffset + static_cast<std::size_t>(keyOffset) * 4u;
        const std::size_t valueBase = 12u + dataOffset + static_cast<std::size_t>(valueOffset) * 4u;
        requireRange(mdl_.size(), keyBase, static_cast<std::size_t>(rows) * 4u,
                     "MDL controller keyframes");
        controller.keys.reserve(rows);
        if (controller.type == 20 && columns == 2) {
            requireRange(mdl_.size(), valueBase, static_cast<std::size_t>(rows) * 4u,
                         "MDL compressed orientation");
            for (std::uint16_t row = 0; row < rows; ++row) {
                ControllerKey key;
                key.time = readF32(bytes, mdl_.size(), keyBase + row * 4u, "MDL key time");
                const std::uint32_t packed = readU32(bytes, mdl_.size(), valueBase + row * 4u,
                                                     "MDL packed quaternion");
                const float x = ((packed & 0x7FFu) / 1023.0f) - 1.0f;
                const float y = (((packed >> 11) & 0x7FFu) / 1023.0f) - 1.0f;
                const float z = ((packed >> 22) / 511.0f) - 1.0f;
                const float w = std::sqrt(std::max(0.0f, 1.0f - (x * x + y * y + z * z)));
                key.data = {w, x, y, z};
                controller.keys.push_back(std::move(key));
            }
        } else {
            const std::size_t effectiveColumns = controller.bezier ? baseColumns * 3u : baseColumns;
            if (effectiveColumns == 0) {
                continue;
            }
            requireRange(mdl_.size(), valueBase,
                         static_cast<std::size_t>(rows) * effectiveColumns * 4u,
                         "MDL controller values");
            for (std::uint16_t row = 0; row < rows; ++row) {
                ControllerKey key;
                key.time = readF32(bytes, mdl_.size(), keyBase + row * 4u, "MDL key time");
                key.data.reserve(effectiveColumns);
                for (std::size_t column = 0; column < effectiveColumns; ++column) {
                    key.data.push_back(readF32(bytes, mdl_.size(),
                                               valueBase + (row * effectiveColumns + column) * 4u,
                                               "MDL controller value"));
                }
                if (controller.type == 20 && !controller.bezier && effectiveColumns == 4) {
                    // Uncompressed orientation keys are stored XYZW; the VM
                    // and quatSlerp use WXYZ.
                    key.data = {key.data[3], key.data[0], key.data[1], key.data[2]};
                }
                controller.keys.push_back(std::move(key));
            }
        }
        if (!controller.keys.empty()) {
            out.push_back(std::move(controller));
        }
    }
    return out;
}

Mdl::Animation Mdl::parseAnimation(std::uint32_t offset) const
{
    Animation animation;
    const std::size_t base = 12u + offset;
    requireRange(mdl_.size(), base, 136u, "MDL animation header");
    animation.name = readCString(mdl_.data(), mdl_.size(), base + 8u, 32, "MDL animation name", name_);
    animation.duration = readF32(mdl_.data(), mdl_.size(), base + 80u, "MDL animation duration");
    animation.transition = readF32(mdl_.data(), mdl_.size(), base + 84u, "MDL animation transition");
    const std::uint32_t rootOffset = readU32(mdl_.data(), mdl_.size(), base + 40u, "MDL animation root");
    animation.nodes = parseNodes(rootOffset);
    const std::uint32_t eventsOffset = readU32(mdl_.data(), mdl_.size(), base + 120u, "MDL animation events");
    const std::uint32_t eventCount = readU32(mdl_.data(), mdl_.size(), base + 124u, "MDL animation event count");
    if (eventCount != 0u) {
        requireRange(mdl_.size(), 12u + static_cast<std::size_t>(eventsOffset),
                     static_cast<std::size_t>(eventCount) * 36u, "MDL animation event table");
        for (std::uint32_t i = 0; i < eventCount; ++i) {
            const std::size_t at = 12u + static_cast<std::size_t>(eventsOffset) + static_cast<std::size_t>(i) * 36u;
            animation.events.push_back({readF32(mdl_.data(), mdl_.size(), at, "MDL event time"),
                                        readCString(mdl_.data(), mdl_.size(), at + 4u, 32, "MDL event name", name_)});
        }
    }
    return animation;
}

const Mdl::Animation* Mdl::findAnimationAnyCase(const std::string& name) const
{
    const std::string wanted = lowerAscii(name);
    for (const std::shared_ptr<const Animation>& held : animationData_) {
        const Animation& animation = *held;
        if (lowerAscii(animation.name) == wanted) {
            return &animation;
        }
    }
    return nullptr;
}

bool Mdl::hasAnimationAnyCase(const std::string& name) const
{
    return findAnimationAnyCase(name) != nullptr;
}

float Mdl::animationDurationAnyCase(const std::string& name) const
{
    const Animation* animation = findAnimationAnyCase(name);
    return animation != nullptr ? animation->duration : 0.0f;
}

std::vector<MdlAnimEvent> Mdl::animationEvents(const std::string& name) const
{
    const Animation* animation = findAnimationAnyCase(name);
    return animation != nullptr ? animation->events : std::vector<MdlAnimEvent>{};
}

int Mdl::animationIndex(const std::string& name) const
{
    for (std::size_t i = 0; i < animations_.size(); ++i) {
        if (animations_[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Mdl::mergeAnimations(const Mdl& supermodel)
{
    for (const std::shared_ptr<const Animation>& animation : supermodel.animationData_) {
        if (animationIndex(animation->name) >= 0) {
            continue;
        }
        animations_.push_back({animation->name, animation->duration, animation->transition});
        animationData_.push_back(animation);
    }
}

float Mdl::animationDuration(const std::string& name) const
{
    const int index = animationIndex(name);
    return index < 0 ? 0.0f : animations_[static_cast<std::size_t>(index)].duration;
}

float Mdl::animationTransition(const std::string& name) const
{
    const int index = animationIndex(name);
    return index < 0 ? 0.0f : animations_[static_cast<std::size_t>(index)].transition;
}

bool Mdl::hasProxyBody() const
{
    for (const Node& node : nodes_) {
        const std::string name = lowerAscii(node.name);
        if ((name == "torso_g" || name == "pelvis_g") && !node.render) {
            return true;
        }
    }
    return false;
}

std::size_t Mdl::skinnedNodeCount() const
{
    std::size_t count = 0;
    for (const Node& node : nodes_) {
        if (node.skin.has_value()) {
            ++count;
        }
    }
    return count;
}

std::size_t Mdl::danglyNodeCount() const
{
    std::size_t count = 0;
    for (const Node& node : nodes_) {
        if (node.dangly.has_value()) {
            ++count;
        }
    }
    return count;
}

bool Mdl::hasNonPoseControllers() const
{
    for (const std::shared_ptr<const Animation>& held : animationData_) {
        const Animation& animation = *held;
        for (const Node& node : animation.nodes) {
            for (const Controller& controller : node.controllers) {
                if (controller.type != 8 && controller.type != 20 && controller.type != 36
                    && controller.type != 132) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool Mdl::hasControllerType(std::int32_t type) const
{
    for (const std::shared_ptr<const Animation>& held : animationData_) {
        const Animation& animation = *held;
        for (const Node& node : animation.nodes) {
            for (const Controller& controller : node.controllers) {
                if (controller.type == type) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::set<std::int32_t> Mdl::fxControllerTypes() const
{
    std::set<std::int32_t> types;
    for (const Node& node : nodes_) {
        if (!(node.type & 0x02u) && !(node.type & 0x04u)) {
            continue;
        }
        for (const Controller& controller : node.controllers) {
            types.insert(controller.type);
        }
    }
    for (const std::shared_ptr<const Animation>& held : animationData_) {
        const Animation& animation = *held;
        for (const Node& node : animation.nodes) {
            if (!(node.type & 0x02u) && !(node.type & 0x04u)) {
                continue;
            }
            for (const Controller& controller : node.controllers) {
                types.insert(controller.type);
            }
        }
    }
    return types;
}

float FxParameter::scalar(std::size_t component, float fallback) const
{
    if (component >= values.size()) {
        return fallback;
    }
    return values[component];
}

Vec3 FxParameter::colour(Vec3 fallback) const
{
    if (values.size() < 3) {
        return fallback;
    }
    return {values[0], values[1], values[2]};
}

const FxParameter* FxNodeParameters::find(std::int32_t family) const
{
    for (const FxParameter& parameter : parameters) {
        if (parameter.type == family) {
            return &parameter;
        }
    }
    return nullptr;
}

float FxNodeParameters::scalar(std::int32_t family, float fallback, std::size_t component) const
{
    const FxParameter* parameter = find(family);
    return parameter == nullptr ? fallback : parameter->scalar(component, fallback);
}

Vec3 FxNodeParameters::colour(std::int32_t family, Vec3 fallback) const
{
    const FxParameter* parameter = find(family);
    return parameter == nullptr ? fallback : parameter->colour(fallback);
}

std::vector<float> Mdl::sampleController(const Controller& controller, float time)
{
    if (controller.keys.empty()) {
        return {};
    }
    const float clamped = std::clamp(time, controller.keys.front().time,
                                     controller.keys.back().time);
    std::size_t hi = 1;
    while (hi < controller.keys.size() && controller.keys[hi].time <= clamped) {
        ++hi;
    }
    hi = std::min(hi, controller.keys.size() - 1);
    const std::size_t lo = hi == 0 ? 0 : hi - 1;
    const float span = controller.keys[hi].time - controller.keys[lo].time;
    const float t = span <= 0.0f ? 0.0f : (clamped - controller.keys[lo].time) / span;
    const std::vector<float>& a = controller.keys[lo].data;
    const std::vector<float>& b = controller.keys[hi].data;
    const std::size_t count = std::min(a.size(), b.size());
    std::vector<float> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = a[i] + (b[i] - a[i]) * t;
    }
    return out;
}

std::size_t Mdl::fxNodeCount() const
{
    std::size_t count = 0;
    for (const Node& node : nodes_) {
        if ((node.type & 0x02u) || (node.type & 0x04u)) {
            ++count;
        }
    }
    return count;
}

FxNodeParameters Mdl::fxNodeParameters(std::size_t ordinal, const std::string& animation,
                                       float time) const
{
    FxNodeParameters result;
    std::size_t seen = 0;
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        const Node& node = nodes_[index];
        if (!(node.type & 0x02u) && !(node.type & 0x04u)) {
            continue;
        }
        if (seen++ != ordinal) {
            continue;
        }
        result.nodeIndex = index;
        result.nodeId = node.nodeId;
        result.type = node.type;
        result.name = node.name;
        std::map<std::int32_t, const Controller*> selected;
        for (const Controller& controller : node.controllers) {
            selected[controller.type] = &controller;
        }
        const int clip = animation.empty() ? -1 : animationIndex(animation);
        if (clip >= 0) {
            for (const Node& animated : animationData_[static_cast<std::size_t>(clip)]->nodes) {
                if (animated.supernode != node.supernode) {
                    continue;
                }
                for (const Controller& controller : animated.controllers) {
                    selected[controller.type] = &controller;
                }
            }
        }
        for (const auto& [type, controller] : selected) {
            FxParameter parameter;
            parameter.type = type;
            parameter.values = sampleController(*controller, time);
            if (!parameter.values.empty()) {
                result.parameters.push_back(std::move(parameter));
            }
        }
        return result;
    }
    return result;
}

float Mdl::skinBindError() const
{
    float maxError = 0.0f;
    for (std::size_t nodeIndex = 0; nodeIndex < nodes_.size(); ++nodeIndex) {
        const Node& node = nodes_[nodeIndex];
        if (!node.skin.has_value()) {
            continue;
        }
        const Skin& skin = *node.skin;
        for (std::uint32_t i = 0; i < node.vertexCount; ++i) {
            const std::size_t o = node.mdxOffset + static_cast<std::size_t>(i) * node.stride;
            const Vec3 p{readF32(mdx_.data(), mdx_.size(), o, "MDX x"),
                         readF32(mdx_.data(), mdx_.size(), o + 4u, "MDX y"),
                         readF32(mdx_.data(), mdx_.size(), o + 8u, "MDX z")};
            Vec3 out{0.0f, 0.0f, 0.0f};
            float totalWeight = 0.0f;
            for (int slot = 0; slot < 4; ++slot) {
                const float weight = readF32(mdx_.data(), mdx_.size(),
                                             o + skin.weightsOffset + static_cast<std::size_t>(slot) * 4u,
                                             "MDX bone weight");
                if (weight <= 0.0f) {
                    continue;
                }
                const std::int32_t boneSlot = static_cast<std::int32_t>(
                    std::lround(readF32(mdx_.data(), mdx_.size(),
                                        o + skin.bonesOffset + static_cast<std::size_t>(slot) * 4u,
                                        "MDX bone index")));
                if (boneSlot < 0 || static_cast<std::size_t>(boneSlot) >= skin.slotNode.size()) {
                    continue;
                }
                const std::int32_t boneIndex = skin.slotNode[static_cast<std::size_t>(boneSlot)];
                if (boneIndex < 0 || static_cast<std::size_t>(boneIndex) >= nodes_.size()) {
                    continue;
                }
                const std::size_t boneNode = static_cast<std::size_t>(boneIndex);
                const std::uint16_t boneId = nodes_[boneNode].nodeId;
                const std::size_t qIndex = static_cast<std::size_t>(boneId) < skin.qbones.size()
                    ? static_cast<std::size_t>(boneId) : boneNode;
                const std::array<float, 4> q = qIndex < skin.qbones.size()
                    ? skin.qbones[qIndex] : std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f};
                const Vec3 t = qIndex < skin.tbones.size()
                    ? skin.tbones[qIndex] : Vec3{0.0f, 0.0f, 0.0f};
                const Vec3 local = p;
                const Vec3 rotated = transform(quatMatrix(q[0], q[1], q[2], q[3]), local);
                const Vec3 bonePoint{rotated.x + t.x, rotated.y + t.y, rotated.z + t.z};
                const Pose& bonePose = bindWorld_[boneNode];
                const Vec3 transformed = transform(bonePose.rotation, bonePoint);
                out.x += weight * (bonePose.translation.x + transformed.x);
                out.y += weight * (bonePose.translation.y + transformed.y);
                out.z += weight * (bonePose.translation.z + transformed.z);
                totalWeight += weight;
            }
            if (totalWeight <= 0.0f) {
                continue;
            }
            const Vec3 expected = transform(bindWorld_[nodeIndex].rotation, p);
            const float dx = out.x - (bindWorld_[nodeIndex].translation.x + expected.x);
            const float dy = out.y - (bindWorld_[nodeIndex].translation.y + expected.y);
            const float dz = out.z - (bindWorld_[nodeIndex].translation.z + expected.z);
            maxError = std::max(maxError, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
    }
    return maxError;
}

std::optional<Mdl::Pose> Mdl::nodePose(const std::string& node, const std::string& animation,
                                       float time) const
{
    const int clip = animation.empty() ? -1 : animationIndex(animation);
    const Animation* anim = clip >= 0 ? animationData_[static_cast<std::size_t>(clip)].get() : nullptr;
    const std::vector<Pose> world = anim != nullptr ? poseWorld(anim, time) : bindWorld_;
    const std::string wanted = lowerAscii(node);
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (lowerAscii(nodes_[i].name) == wanted) {
            return world[i];
        }
    }
    return std::nullopt;
}

std::vector<Mdl::Pose> Mdl::layeredWorld(const std::vector<MdlAnimLayer>& layers) const
{
    std::vector<std::pair<const Animation*, float>> resolved;
    for (const MdlAnimLayer& layer : layers) {
        if (const Animation* animation = findAnimationAnyCase(layer.animation)) {
            resolved.emplace_back(animation, layer.time);
        }
    }
    if (resolved.empty()) {
        return bindWorld_;
    }
    const std::pair<const Animation*, float> base = resolved.front();
    resolved.erase(resolved.begin());
    return poseWorld(base.first, base.second, nullptr, false, &resolved);
}

std::vector<MdlMeshTriangle> Mdl::posedTrianglesLayered(const std::vector<MdlAnimLayer>& layers,
                                                        bool renderableOnly) const
{
    const auto filter = [renderableOnly](const Node& node) { return !renderableOnly || node.render; };
    // Alpha and self-illumination come from every layer, as the pose does: a later layer replaces
    // only the channels it animates (the swoop start lights stay lit under the timer digits).
    std::vector<std::pair<const Animation*, float>> tints;
    for (const MdlAnimLayer& layer : layers) {
        if (const Animation* animation = findAnimationAnyCase(layer.animation)) {
            tints.emplace_back(animation, layer.time);
        }
    }
    const Animation* tintClip = tints.empty() ? nullptr : tints.back().first;
    const float tintTime = tints.empty() ? 0.0f : tints.back().second;
    return trianglesFromPose(layeredWorld(layers), -1.0f, filter, tintClip, tintTime, nullptr, &tints);
}

std::optional<Mdl::Pose> Mdl::nodePoseLayered(const std::string& node,
                                              const std::vector<MdlAnimLayer>& layers) const
{
    const std::string wanted = lowerAscii(node);
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (lowerAscii(nodes_[i].name) == wanted) {
            return layeredWorld(layers)[i];
        }
    }
    return std::nullopt;
}

std::vector<std::array<Vec3, 3>> Mdl::nodeFaces(const std::string& node) const
{
    const std::string wanted = lowerAscii(node);
    const auto filter = [&wanted](const Node& candidate) { return lowerAscii(candidate.name) == wanted; };
    std::vector<std::array<Vec3, 3>> faces;
    for (const MdlMeshTriangle& triangle : trianglesFromPose(bindWorld_, -1.0f, filter)) {
        faces.push_back({triangle.v[0].position, triangle.v[1].position, triangle.v[2].position});
    }
    return faces;
}

std::vector<Mdl::Pose> Mdl::poseWorld(const Animation* animation, float time,
                                     const MdlFacePose* face, bool /*headRootOnly: see the gate below*/,
                                     const std::vector<std::pair<const Animation*, float>>* overlays,
                                     const Mat3* headTurn) const
{
    std::vector<Pose> world(nodes_.size());
    std::size_t turnNode = nodes_.size();
    if (headTurn != nullptr) {
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (lowerAscii(nodes_[i].name) == "hturn_g") {
                turnNode = i;
                break;
            }
        }
    }
    struct Local {
        Vec3 position;
        std::array<float, 4> rotation{1.0f, 0.0f, 0.0f, 0.0f};
        Vec3 scale{1.0f, 1.0f, 1.0f};
    };
    std::vector<Local> poseLocal(nodes_.size());
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        poseLocal[i].position = nodes_[i].position;
        poseLocal[i].rotation = {nodes_[i].qw, nodes_[i].qx, nodes_[i].qy, nodes_[i].qz};
    }
    // The base clip, then any overlays: a later layer replaces the channels it animates.
    const auto applyLayer = [&](const Animation& layer, float layerTime, std::vector<Local>& local) {
    std::map<std::uint16_t, const Node*> animated;
    for (const Node& node : layer.nodes) {
        animated[node.supernode] = &node;
    }
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const Node& node = nodes_[i];
        const auto it = animated.find(node.supernode);
        // Retail 00483d80 dispatches a clip's controllers only from the model's animation root
        // (header +168) down, on every model: the root itself on all but the 106 shipped heads,
        // whose root is neck_g. So the gate holds whatever the caller asks for.
        if (it == animated.end() || (!underHeadRoot_.empty() && !underHeadRoot_[i])) {
            continue;
        }
        for (const Controller& controller : it->second->controllers) {
            if (controller.keys.empty()) {
                continue;
            }
            if (controller.type == 8) {
                local[i].position = node.position;
            } else if (controller.type == 36) {
                local[i].scale = {1.0f, 1.0f, 1.0f};
            }
            const float clamped = std::clamp(layerTime, controller.keys.front().time,
                                             controller.keys.back().time);
            std::size_t hi = 1;
            while (hi < controller.keys.size() && controller.keys[hi].time <= clamped) {
                ++hi;
            }
            hi = std::min(hi, controller.keys.size() - 1);
            const std::size_t lo = hi == 0 ? 0 : hi - 1;
            const ControllerKey& a = controller.keys[lo];
            const ControllerKey& b = controller.keys[hi];
            const float span = b.time - a.time;
            const float t = span <= 0.0f ? 0.0f : (clamped - a.time) / span;
            if (controller.type == 20 && a.data.size() >= 4 && b.data.size() >= 4) {
                const std::array<float, 4> qa{a.data[0], a.data[1], a.data[2], a.data[3]};
                const std::array<float, 4> qb{b.data[0], b.data[1], b.data[2], b.data[3]};
                local[i].rotation = quatSlerp(qa, qb, t);
            } else if (controller.type == 8 && a.data.size() >= 3 && b.data.size() >= 3) {
                if (controller.bezier && a.data.size() >= 9 && b.data.size() >= 9) {
                    // A bezier key is value, in tangent, out tangent; the tangents are offsets
                    // from the key's value. RETAIL row 38 (m12ab_mgt02 modelhook, track 0.3904)
                    // places the fighter at -14.57,-24.17,-5.30 from the hook: this curve gives
                    // -14.56,-24.15; taking the tangents as points flew it back and forth.
                    const float g = 1.0f - t;
                    for (int c = 0; c < 3; ++c) {
                        const float p0 = a.data[static_cast<std::size_t>(c)];
                        const float p1 = p0 + a.data[static_cast<std::size_t>(6 + c)];
                        const float p2 = b.data[static_cast<std::size_t>(c)] + b.data[static_cast<std::size_t>(3 + c)];
                        const float p3 = b.data[static_cast<std::size_t>(c)];
                        const float value = g * g * g * p0 + 3.0f * g * g * t * p1
                            + 3.0f * g * t * t * p2 + t * t * t * p3;
                        if (c == 0) {
                            local[i].position.x += value;
                        } else if (c == 1) {
                            local[i].position.y += value;
                        } else {
                            local[i].position.z += value;
                        }
                    }
                } else {
                    local[i].position.x += a.data[0] + (b.data[0] - a.data[0]) * t;
                    local[i].position.y += a.data[1] + (b.data[1] - a.data[1]) * t;
                    local[i].position.z += a.data[2] + (b.data[2] - a.data[2]) * t;
                }
            } else if (controller.type == 36 && !a.data.empty() && !b.data.empty()) {
                if (a.data.size() >= 3 && b.data.size() >= 3) {
                    local[i].scale.x *= a.data[0] + (b.data[0] - a.data[0]) * t;
                    local[i].scale.y *= a.data[1] + (b.data[1] - a.data[1]) * t;
                    local[i].scale.z *= a.data[2] + (b.data[2] - a.data[2]) * t;
                } else {
                    const float scale = a.data[0] + (b.data[0] - a.data[0]) * t;
                    local[i].scale.x *= scale;
                    local[i].scale.y *= scale;
                    local[i].scale.z *= scale;
                }
            }
        }
    }
    };
    if (animation != nullptr) {
        applyLayer(*animation, time, poseLocal);
    }
    if (overlays != nullptr) {
        for (const auto& [overlay, overlayTime] : *overlays) {
            applyLayer(*overlay, overlayTime, poseLocal);
        }
    }
    // retail 00486670: the lip track samples `talk` at the two keys around the moment and blends
    // them; `weight` is the half second it fades over when the track ends. Only the nodes the clip
    // animates (the mouth) are touched.
    if (face != nullptr && face->weight > 0.0f) {
        if (const Animation* talk = findAnimationAnyCase("talk"); talk != nullptr) {
            std::vector<Local> a = poseLocal;
            std::vector<Local> b = poseLocal;
            applyLayer(*talk, face->fractionA * talk->duration, a);
            applyLayer(*talk, face->fractionB * talk->duration, b);
            std::set<std::uint16_t> mouth;
            for (const Node& node : talk->nodes) {
                if (!node.controllers.empty()) {
                    mouth.insert(node.supernode);
                }
            }
            const auto mix = [](Vec3 from, Vec3 to, float t) {
                return Vec3{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t,
                            from.z + (to.z - from.z) * t};
            };
            for (std::size_t i = 0; i < nodes_.size(); ++i) {
                if (mouth.count(nodes_[i].supernode) == 0) {
                    continue;
                }
                const Vec3 shape = mix(a[i].position, b[i].position, face->blend);
                const std::array<float, 4> turn = quatSlerp(a[i].rotation, b[i].rotation, face->blend);
                poseLocal[i].position = mix(poseLocal[i].position, shape, face->weight);
                poseLocal[i].rotation = quatSlerp(poseLocal[i].rotation, turn, face->weight);
            }
        }
    }

    std::vector<int> state(nodes_.size(), 0);
    std::function<Pose(std::size_t)> resolve = [&](std::size_t index) -> Pose {
        if (state[index] == 2) {
            return world[index];
        }
        if (state[index] == 1) {
            return world[index];
        }
        state[index] = 1;
        Pose parent;
        parent.rotation = identity();
        const Node& node = nodes_[index];
        const auto parentIt = nodeByOffset_.find(node.parent);
        if (parentIt != nodeByOffset_.end()) {
            parent = resolve(parentIt->second);
        }
        const Local& localPose = poseLocal[index];
        const Mat3 localRotation = quatMatrix(localPose.rotation[0], localPose.rotation[1],
                                              localPose.rotation[2], localPose.rotation[3]);
        world[index].rotation = multiply(parent.rotation, localRotation);
        if (index == turnNode) {
            world[index].rotation = multiply(*headTurn, world[index].rotation);
        }
        const Vec3 scaled{localPose.position.x * localPose.scale.x,
                          localPose.position.y * localPose.scale.y,
                          localPose.position.z * localPose.scale.z};
        const Vec3 offset = transform(parent.rotation, scaled);
        world[index].translation = {parent.translation.x + offset.x,
                                    parent.translation.y + offset.y,
                                    parent.translation.z + offset.z};
        world[index].scale = localPose.scale.x;
        state[index] = 2;
        return world[index];
    };
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        resolve(i);
    }
    return world;
}

std::vector<MdlMeshTriangle> Mdl::meshTriangles() const
{
    return posedTriangles({}, 0.0f);
}

std::vector<MdlMeshTriangle> Mdl::posedTriangles(const std::string& animation, float time,
                                                 float danglyTime) const
{
    const int clip = animation.empty() ? -1 : animationIndex(animation);
    const Animation* anim = clip >= 0 ? animationData_[static_cast<std::size_t>(clip)].get() : nullptr;
    return trianglesFromPose(anim != nullptr ? poseWorld(anim, time) : bindWorld_, danglyTime,
                             [](const Node& node) { return node.render; }, anim, time);
}

namespace {

// Blends two world poses: positions linearly, rotations made orthonormal after a linear mix.
Mdl::Pose blendPose(const Mdl::Pose& a, const Mdl::Pose& b, float t)
{
    Mdl::Pose out = a;
    out.translation = {a.translation.x + (b.translation.x - a.translation.x) * t,
                       a.translation.y + (b.translation.y - a.translation.y) * t,
                       a.translation.z + (b.translation.z - a.translation.z) * t};
    out.scale = a.scale + (b.scale - a.scale) * t;
    Mat3 rotation{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            rotation.m[r][c] = a.rotation.m[r][c] * (1.0f - t) + b.rotation.m[r][c] * t;
        }
    }
    // Gram-Schmidt keeps the blended rotation orthonormal.
    Vec3 x{rotation.m[0][0], rotation.m[1][0], rotation.m[2][0]};
    x = normalize(x);
    Vec3 y{rotation.m[0][1], rotation.m[1][1], rotation.m[2][1]};
    y = normalize(y - x * dot(x, y));
    const Vec3 z = normalize(cross(x, y));
    out.rotation.m[0][0] = x.x;
    out.rotation.m[1][0] = x.y;
    out.rotation.m[2][0] = x.z;
    out.rotation.m[0][1] = y.x;
    out.rotation.m[1][1] = y.y;
    out.rotation.m[2][1] = y.z;
    out.rotation.m[0][2] = z.x;
    out.rotation.m[1][2] = z.y;
    out.rotation.m[2][2] = z.z;
    return out;
}

} // namespace

std::vector<Mdl::Pose> Mdl::worldFor(const MdlPoseRequest& request) const
{
    const auto clipOf = [this](const std::string& name) -> const Animation* {
        const int clip = name.empty() ? -1 : animationIndex(name);
        return clip >= 0 ? animationData_[static_cast<std::size_t>(clip)].get() : nullptr;
    };
    const Animation* current = clipOf(request.animation);
    const bool noFace = (request.face == nullptr || request.face->weight <= 0.0f)
        && request.headTurn == nullptr;
    std::vector<Pose> world = current == nullptr && noFace
        ? bindWorld_ : poseWorld(current, request.time, request.face, request.headRootOnly, nullptr,
                                 request.headTurn);
    if (request.blend < 1.0f && !request.previous.empty()) {
        const Animation* earlier = clipOf(request.previous);
        const std::vector<Pose> from = earlier == nullptr && noFace
            ? bindWorld_ : poseWorld(earlier, request.previousTime, request.face, request.headRootOnly,
                                     nullptr, request.headTurn);
        const float t = std::clamp(request.blend, 0.0f, 1.0f);
        for (std::size_t i = 0; i < world.size() && i < from.size(); ++i) {
            world[i] = blendPose(from[i], world[i], t);
        }
    }
    return world;
}

std::vector<MdlMeshTriangle> Mdl::posedTrianglesFor(const MdlPoseRequest& request) const
{
    const int clip = request.animation.empty() ? -1 : animationIndex(request.animation);
    const Animation* tint = clip >= 0 ? animationData_[static_cast<std::size_t>(clip)].get() : nullptr;
    return trianglesFromPose(worldFor(request), request.danglyTime,
                             [](const Node& node) { return node.render; }, tint, request.time,
                             request.saberTrail);
}

std::vector<Mdl::ShadowPart> Mdl::shadowPartsFor(const MdlPoseRequest& request) const
{
    std::vector<ShadowPart> out;
    if (!hasShadowMeshes()) {
        return out;
    }
    const std::vector<Pose> world = worldFor(request);
    for (std::size_t i = 0; i < nodes_.size() && i < world.size(); ++i) {
        if (nodes_[i].shadowMesh) {
            out.push_back({nodes_[i].shadowMesh.get(), world[i]});
        }
    }
    return out;
}

bool Mdl::hasShadowMeshes() const
{
    return std::any_of(nodes_.begin(), nodes_.end(), [](const Node& node) { return node.shadowMesh != nullptr; });
}

std::optional<Mdl::Pose> Mdl::nodePoseFor(const std::string& node, const MdlPoseRequest& request) const
{
    const std::string wanted = lowerAscii(node);
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (lowerAscii(nodes_[i].name) == wanted) {
            return worldFor(request)[i];
        }
    }
    return std::nullopt;
}

std::string Mdl::clipNamed(const std::string& name) const
{
    const Animation* clip = findAnimationAnyCase(name);
    return clip != nullptr ? clip->name : std::string{};
}

bool Mdl::hasNode(const std::string& name) const
{
    const std::string wanted = lowerAscii(name);
    for (const Node& node : nodes_) {
        if (lowerAscii(node.name) == wanted) {
            return true;
        }
    }
    return false;
}

std::vector<MdlMeshTriangle> Mdl::blendedTriangles(const std::string& animationA, float timeA,
                                                   const std::string& animationB, float timeB,
                                                   float blend, float danglyTime) const
{
    const int clipA = animationIndex(animationA);
    const int clipB = animationIndex(animationB);
    const std::vector<Pose> worldA = clipA >= 0
        ? poseWorld(animationData_[static_cast<std::size_t>(clipA)].get(), timeA) : bindWorld_;
    const std::vector<Pose> worldB = clipB >= 0
        ? poseWorld(animationData_[static_cast<std::size_t>(clipB)].get(), timeB) : bindWorld_;
    const float t = std::clamp(blend, 0.0f, 1.0f);
    std::vector<Pose> blended = worldA;
    for (std::size_t i = 0; i < blended.size() && i < worldB.size(); ++i) {
        blended[i] = blendPose(worldA[i], worldB[i], t);
    }
    return trianglesFromPose(blended, danglyTime, [](const Node& node) { return node.render; },
                             clipB >= 0 ? animationData_[static_cast<std::size_t>(clipB)].get() : nullptr, timeB);
}

Vec3 Mdl::rootMotion(const std::string& animation, float time) const
{
    const int clip = animation.empty() ? -1 : animationIndex(animation);
    const Animation* anim = clip >= 0 ? animationData_[static_cast<std::size_t>(clip)].get() : nullptr;
    const std::vector<Pose> world = anim != nullptr ? poseWorld(anim, time) : bindWorld_;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (lowerAscii(nodes_[i].name) == "root" && i < world.size()) {
            return world[i].translation;
        }
    }
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].parent == 0 && i < world.size()) {
            return world[i].translation;
        }
    }
    return {};
}

namespace {

// The lightsaber swing trail, from retail PartLightsaber (vtable 00740ce0): Update 00481ae0
// (slot +0x68) keeps a ring of the blade node's world place, Draw 00471860 (slot +0x64) sweeps the
// blade through it. The x87 runs at double precision in this process, so the sums and tests below
// are done in double and stored as float where retail stores to a float.
using SaberQuat = std::array<float, 4>; // w, x, y, z, as retail keeps them

constexpr std::int16_t kTrailRing = 20;     // 00740a4c
constexpr float kTrailMaxAge = 0.012f;      // 00798b30
constexpr float kTrailMaxDt = 0.01f;        // 00798b34
constexpr float kTrailStep = 0.001f;        // 0078e838
constexpr float kTrailFinShort = 0.5f;      // 0078e83c
constexpr bool kTrailOn = true;             // 0078e828
constexpr bool kTrailExtrapolate = true;    // 0078e834
// 0.001 added from zero passes 0.012 on the twelfth add, so a pass draws at most 11 strips.
constexpr std::size_t kTrailStripsMax = 11;
constexpr std::uint16_t kSaberCore = 8;     // 00740a50
constexpr std::uint16_t kSaberRow = 4;      // 00740a54
constexpr std::uint16_t kSaberRows = 20;    // 00740a48
constexpr std::uint16_t kSaberSecond = kSaberCore + kSaberRow * kSaberRows; // 88
constexpr std::size_t kStripFaces = 2u * (kSaberRow - 1u);
// A blade node always gives this many faces (unused ones degenerate), so its mesh keeps its size:
// two passes of strips, each with its fin.
constexpr std::size_t kSaberFaces = 2u * (kTrailStripsMax + 1u) * kStripFaces;

// 004a9bb0 (the identity shortcut is on: 007a1980).
Vec3 saberRotate(const SaberQuat& q, Vec3 v)
{
    if (q[0] == 1.0f || q[0] == -1.0f) {
        return v;
    }
    const float xx = q[1] * q[1];
    const float yy = q[2] * q[2];
    const float zz = q[3] * q[3];
    const float xy = q[2] * q[1];
    const float xz = q[3] * q[1];
    const float wx = q[1] * q[0];
    const float yz = q[3] * q[2];
    const float wy = q[2] * q[0];
    const float wz = q[3] * q[0];
    return {(1.0f - (zz + yy) * 2.0f) * v.x + (xy - wz) * 2.0f * v.y + (wy + xz) * 2.0f * v.z,
            (1.0f - (zz + xx) * 2.0f) * v.y + (wz + xy) * 2.0f * v.x + (yz - wx) * 2.0f * v.z,
            (1.0f - (yy + xx) * 2.0f) * v.z + (xz - wy) * 2.0f * v.x + (yz + wx) * 2.0f * v.y};
}

// 004a9b50: the inverse turn (w negated).
SaberQuat saberInverse(const SaberQuat& q)
{
    return {-q[0], q[1], q[2], q[3]};
}

// 004a9ac0: a then b, Hamilton order.
SaberQuat saberMultiply(const SaberQuat& a, const SaberQuat& b)
{
    return {a[0] * b[0] - b[1] * a[1] - a[2] * b[2] - b[3] * a[3],
            b[3] * a[2] + b[1] * a[0] + b[0] * a[1] - a[3] * b[2],
            b[2] * a[0] - b[3] * a[1] + a[2] * b[0] + b[1] * a[3],
            b[2] * a[1] + b[3] * a[0] - b[1] * a[2] + a[3] * b[0]};
}

// 004a9e00: shortest-way slerp, unnormalised; a is taken by value.
SaberQuat saberSlerp(SaberQuat a, const SaberQuat& b, float t)
{
    double d = static_cast<double>(a[3]) * b[3] + static_cast<double>(a[2]) * b[2]
        + static_cast<double>(a[1]) * b[1] + static_cast<double>(a[0]) * b[0];
    if (d < 0.0) {
        for (float& c : a) {
            c = -c;
        }
        d = -d;
    }
    const float dot = static_cast<float>(d);
    if (!(d < 1.0)) {
        return a;
    }
    const float theta = static_cast<float>(std::acos(static_cast<double>(dot)));
    if (theta == 0.0f) {
        return a;
    }
    const double sinTheta = static_cast<float>(std::sin(static_cast<double>(theta)));
    const float part = static_cast<float>(static_cast<double>(theta) * t);
    const float sb = static_cast<float>(std::sin(static_cast<double>(part)) / sinTheta);
    const float sa = static_cast<float>(std::sin(static_cast<double>(theta) - part) / sinTheta);
    return {a[0] * sa + b[0] * sb, a[1] * sa + b[1] * sb, a[2] * sa + b[2] * sb, a[3] * sa + b[3] * sb};
}

// 004ab130: unit length, or (1, 0, 0) under 1e-9 (007440f0).
Vec3 saberUnit(Vec3 v)
{
    const float n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(n >= 1e-9)) {
        return {1.0f, 0.0f, 0.0f};
    }
    const float inv = 1.0f / n;
    return {v.x * inv, v.y * inv, v.z * inv};
}

// 004ab630: the turn that takes direction u to direction v.
SaberQuat saberArc(Vec3 u, Vec3 v)
{
    u = saberUnit(u);
    v = saberUnit(v);
    const Vec3 c{u.y * v.z - u.z * v.y, u.z * v.x - v.z * u.x, u.x * v.y - u.y * v.x}; // 004ab210
    const float d = v.z * u.z + v.x * u.x + v.y * u.y;
    if (d <= -1.0f) {
        return {0.0f, 1.0f, 0.0f, 0.0f};
    }
    const float s = std::sqrt((d + 1.0f) * 2.0f);
    const float inv = 1.0f / s;
    return {s * 0.5f, c.x * inv, c.y * inv, c.z * inv};
}

SaberQuat saberFromMatrix(const Mat3& r)
{
    const float trace = r.m[0][0] + r.m[1][1] + r.m[2][2];
    SaberQuat q{};
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {0.25f * s, (r.m[2][1] - r.m[1][2]) / s, (r.m[0][2] - r.m[2][0]) / s, (r.m[1][0] - r.m[0][1]) / s};
    } else if (r.m[0][0] > r.m[1][1] && r.m[0][0] > r.m[2][2]) {
        const float s = std::sqrt(1.0f + r.m[0][0] - r.m[1][1] - r.m[2][2]) * 2.0f;
        q = {(r.m[2][1] - r.m[1][2]) / s, 0.25f * s, (r.m[0][1] + r.m[1][0]) / s, (r.m[0][2] + r.m[2][0]) / s};
    } else if (r.m[1][1] > r.m[2][2]) {
        const float s = std::sqrt(1.0f + r.m[1][1] - r.m[0][0] - r.m[2][2]) * 2.0f;
        q = {(r.m[0][2] - r.m[2][0]) / s, (r.m[0][1] + r.m[1][0]) / s, 0.25f * s, (r.m[1][2] + r.m[2][1]) / s};
    } else {
        const float s = std::sqrt(1.0f + r.m[2][2] - r.m[0][0] - r.m[1][1]) * 2.0f;
        q = {(r.m[1][0] - r.m[0][1]) / s, (r.m[0][2] + r.m[2][0]) / s, (r.m[1][2] + r.m[2][1]) / s, 0.25f * s};
    }
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    return n > 0.0f ? SaberQuat{q[0] / n, q[1] / n, q[2] / n, q[3] / n} : SaberQuat{1.0f, 0.0f, 0.0f, 0.0f};
}

std::int16_t trailBack(std::int16_t index)
{
    return index < 0 ? static_cast<std::int16_t>(index + kTrailRing) : index;
}

// 00481ae0: age the samples (dropping the expired from the tail but keeping two), then add the
// blade's place unless it has not moved; a jump over 1 m starts the ring again.
void saberTrailUpdate(SaberTrail& trail, float dt, Vec3 position, const SaberQuat& rotation)
{
    if (dt > kTrailMaxDt) {
        dt = kTrailMaxDt;
    }
    // 007b92a0 vfunc +0xa8 gates the ageing; our loop only poses a hand item while the game runs.
    std::int16_t index = trail.tail;
    std::int16_t removed = 0;
    for (std::int16_t i = 0; i < trail.count; ++i) {
        const double aged = static_cast<double>(dt) + trail.age[static_cast<std::size_t>(index)];
        trail.age[static_cast<std::size_t>(index)] = static_cast<float>(aged);
        if (aged > kTrailMaxAge && trail.count - removed > 1) {
            ++removed;
            if (++trail.tail == kTrailRing) {
                trail.tail = 0;
            }
        }
        if (++index == kTrailRing) {
            index = 0;
        }
    }
    trail.count = static_cast<std::int16_t>(trail.count - removed);
    if (position.x == trail.last.x && position.y == trail.last.y && position.z == trail.last.z) { // 004aa980
        return;
    }
    const Vec3 moved = position - trail.last;
    if (std::sqrt(moved.x * moved.x + moved.y * moved.y + moved.z * moved.z) > 1.0f) {
        trail.head = 0;
        trail.tail = 0;
        trail.count = 0;
    }
    trail.last = position;
    const std::size_t head = static_cast<std::size_t>(trail.head);
    trail.age[head] = 0.0f;
    trail.position[head] = position;
    trail.rotation[head] = rotation;
    if (trail.count != kTrailRing) {
        ++trail.head;
        ++trail.count;
        if (trail.head >= kTrailRing) {
            trail.head = 0;
        }
    }
}

// One drawn trail vertex in the blade node's own space, and the source vertex it came from.
struct TrailPoint {
    Vec3 local;
    std::uint16_t uvFrom = 0;
    std::uint16_t source = 0;
};

// 00471860 with 0078e828 set and two or more samples: the blade is drawn only as its trail.
// Pass one sweeps the second plane (vertices 88..91) back through the ring, lerp and slerp
// between samples in 1 ms steps of age; pass two (0078e834) sweeps the first plane (0..3) ahead
// along the last move carried on (2 * newest - older, turned by the arc between the two blade
// directions). Each pass ends with a fin a blade width wide along the motion, faded by the
// blade's own across-texture (uvs 92..95, 4..7). Strips come back as pairs of rows, in GL order.
std::vector<std::vector<std::pair<TrailPoint, TrailPoint>>> saberTrailStrips(
    const SaberTrail& trail, const std::vector<Vec3>& sp, Vec3 centre, const SaberQuat& turn)
{
    std::vector<std::vector<std::pair<TrailPoint, TrailPoint>>> strips;
    const SaberQuat back = saberInverse(turn);
    const auto local = [&](Vec3 at, const SaberQuat& q, Vec3 v) {
        const Vec3 r = saberRotate(q, v);
        const Vec3 w{at.x + r.x, at.y + r.y, at.z + r.z};
        return saberRotate(back, w - centre);
    };
    const auto lerp = [](Vec3 from, Vec3 to, float t) {
        const double u = 1.0 - static_cast<double>(t);
        return Vec3{static_cast<float>(t * static_cast<double>(to.x) + u * from.x),
                    static_cast<float>(t * static_cast<double>(to.y) + u * from.y),
                    static_cast<float>(t * static_cast<double>(to.z) + u * from.z)};
    };
    const Vec3 width = sp[4] - sp[0];
    const float bladeWidth = std::sqrt(width.x * width.x + width.y * width.y + width.z * width.z);
    std::array<Vec3, kSaberRow> fin;
    fin.fill(width);
    const auto shapeFin = [&](float sign) {
        for (Vec3& f : fin) {
            const float len = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
            if (len == 0.0f) {
                continue;
            }
            if (kTrailFinShort * bladeWidth <= len) {
                const float inv = 1.0f / len;
                f = Vec3{f.x * inv, f.y * inv, f.z * inv} * (sign * bladeWidth);
            } else {
                f = width * sign;
            }
        }
    };
    const auto ageOf = [&](std::int16_t i) { return static_cast<double>(trail.age[static_cast<std::size_t>(i)]); };
    // The shared 1 ms walk: calls strip(t) while the age step stays inside the segment and the
    // pass's total stays within 0.012.
    float total = 0.0f;
    const auto walk = [&](std::int16_t older, std::int16_t newer, const auto& strip) {
        const double span = ageOf(older) - ageOf(newer);
        double next = static_cast<double>(kTrailStep) / span;
        if (!(next < 1.0)) {
            return;
        }
        float t = static_cast<float>(next);
        for (;;) {
            const double sum = static_cast<double>(kTrailStep) + total;
            total = static_cast<float>(sum);
            if (sum > kTrailMaxAge) {
                return;
            }
            strip(t);
            next = static_cast<double>(kTrailStep) / span + t;
            t = static_cast<float>(next);
            if (!(next < 1.0)) {
                return;
            }
        }
    };

    // Pass one, 00471d57..00472813.
    {
        std::int16_t newer = trailBack(static_cast<std::int16_t>(trail.head - 1));
        std::int16_t older = trailBack(static_cast<std::int16_t>(trail.head - 2));
        Vec3 at = trail.position[static_cast<std::size_t>(newer)];
        SaberQuat q = trail.rotation[static_cast<std::size_t>(newer)];
        for (std::int16_t k = 1; k < trail.count; ++k) {
            const Vec3 from = trail.position[static_cast<std::size_t>(newer)];
            const Vec3 to = trail.position[static_cast<std::size_t>(older)];
            const SaberQuat qFrom = trail.rotation[static_cast<std::size_t>(newer)];
            const SaberQuat qTo = trail.rotation[static_cast<std::size_t>(older)];
            walk(older, newer, [&](float t) {
                const Vec3 wasAt = at;
                const SaberQuat wasQ = q;
                at = lerp(from, to, t);
                q = saberSlerp(qFrom, qTo, t);
                auto& strip = strips.emplace_back();
                for (std::uint16_t j = 0; j < kSaberRow; ++j) {
                    const std::uint16_t v = static_cast<std::uint16_t>(kSaberSecond + j);
                    const Vec3 now = local(at, q, sp[v]);
                    const Vec3 was = local(wasAt, wasQ, sp[v]);
                    strip.push_back({{now, v, v}, {was, v, v}});
                    fin[j] = now - was;
                }
            });
            newer = older;
            older = trailBack(static_cast<std::int16_t>(older - 1));
        }
        shapeFin(1.0f);
        auto& strip = strips.emplace_back();
        for (std::uint16_t j = 0; j < kSaberRow; ++j) {
            const std::uint16_t v = static_cast<std::uint16_t>(kSaberSecond + j);
            const Vec3 edge = local(at, q, sp[v]);
            strip.push_back({{edge + fin[j], static_cast<std::uint16_t>(v + kSaberRow), v}, {edge, v, v}});
        }
    }

    if (!kTrailExtrapolate) {
        return strips;
    }
    // Pass two, 00472826..00473854.
    total = 0.0f;
    {
        std::int16_t newer = trailBack(static_cast<std::int16_t>(trail.head - 1));
        std::int16_t older = trailBack(static_cast<std::int16_t>(trail.head - 2));
        Vec3 nAt = trail.position[static_cast<std::size_t>(newer)];
        SaberQuat nQ = trail.rotation[static_cast<std::size_t>(newer)];
        Vec3 oAt = trail.position[static_cast<std::size_t>(older)];
        SaberQuat oQ = trail.rotation[static_cast<std::size_t>(older)];
        Vec3 at = nAt;
        SaberQuat q = nQ;
        const std::uint16_t tip = kSaberRow - 1;
        for (std::int16_t k = 1; k < trail.count; ++k) {
            const Vec3 ahead{static_cast<float>(-static_cast<double>(oAt.x) + 2.0 * nAt.x),
                             static_cast<float>(-static_cast<double>(oAt.y) + 2.0 * nAt.y),
                             static_cast<float>(-static_cast<double>(oAt.z) + 2.0 * nAt.z)};
            const Vec3 oldDir = local(oAt, oQ, sp[tip]) - local(oAt, oQ, sp[0]);
            const Vec3 newDir = local(nAt, nQ, sp[tip]) - local(nAt, nQ, sp[0]);
            const SaberQuat aheadQ = saberMultiply(nQ, saberArc(oldDir, newDir));
            walk(older, newer, [&](float t) {
                const Vec3 wasAt = at;
                const SaberQuat wasQ = q;
                at = lerp(nAt, ahead, t);
                q = saberSlerp(nQ, aheadQ, t);
                auto& strip = strips.emplace_back();
                for (std::uint16_t j = 0; j < kSaberRow; ++j) {
                    const Vec3 was = local(wasAt, wasQ, sp[j]);
                    const Vec3 now = local(at, q, sp[j]);
                    strip.push_back({{was, j, j}, {now, j, j}});
                    fin[j] = was - now;
                }
            });
            oAt = nAt;
            oQ = nQ;
            nAt = ahead;
            nQ = aheadQ;
            newer = older;
            older = trailBack(static_cast<std::int16_t>(older - 1));
        }
        shapeFin(-1.0f);
        auto& strip = strips.emplace_back();
        for (std::uint16_t j = 0; j < kSaberRow; ++j) {
            const Vec3 edge = local(at, q, sp[j]);
            strip.push_back({{edge, j, j}, {edge + fin[j], static_cast<std::uint16_t>(j + kSaberRow), j}});
        }
    }
    return strips;
}

} // namespace

std::vector<MdlMeshTriangle> Mdl::trianglesFromPose(const std::vector<Pose>& world,
                                                    float danglyTime,
                                                    const std::function<bool(const Node&)>& filter,
                                                    const Animation* tintClip, float tintTime,
                                                    SaberTrailFeed* saberTrail,
                                                    const std::vector<std::pair<const Animation*, float>>* tintLayers) const
{
    std::size_t saberOrdinal = 0;
    std::vector<MdlMeshTriangle> out;
    std::size_t faceTotal = 0;
    for (const Node& node : nodes_) {
        faceTotal += node.trimesh ? node.faceCount : 0u;
    }
    out.reserve(faceTotal);
    // The bounds check stays on every read; only its call goes, on the path that passes.
    const auto mdxFloat = [&](std::size_t offset, const char* what) {
        if (offset + 4u > mdx_.size() || offset + 4u < offset) {
            return readF32(mdx_.data(), mdx_.size(), offset, what);
        }
        float value = 0.0f;
        std::memcpy(&value, mdx_.data() + offset, sizeof(value));
        return value;
    };
    // One skinned node's bones, resolved once. A slot's values depend only on the pose.
    struct BoneSlot {
        bool valid = false;
        const Pose* pose = nullptr;
        Mat3 inverse;
        Mat3 normal;
        Vec3 offset;
    };
    std::vector<BoneSlot> boneSlots;
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    std::vector<Vec2> uvs1;
    for (std::size_t nodeIndex = 0; nodeIndex < nodes_.size(); ++nodeIndex) {
        const Node& node = nodes_[nodeIndex];
        if (!node.trimesh || (filter && !filter(node))) {
            continue;
        }
        const Pose& nodePose = world[nodeIndex];
        positions.clear();
        normals.clear();
        uvs.clear();
        uvs1.clear();
        uvs1.reserve(node.vertexCount);
        positions.reserve(node.vertexCount);
        normals.reserve(node.vertexCount);
        uvs.reserve(node.vertexCount);
        const bool skinned = node.skin.has_value();
        if (skinned) {
            const Skin& skin = *node.skin;
            boneSlots.assign(skin.slotNode.size(), BoneSlot{});
            for (std::size_t slot = 0; slot < skin.slotNode.size(); ++slot) {
                const std::int32_t boneIndex = skin.slotNode[slot];
                if (boneIndex < 0 || static_cast<std::size_t>(boneIndex) >= world.size()) {
                    continue;
                }
                const std::size_t boneNode = static_cast<std::size_t>(boneIndex);
                const std::uint16_t boneId = nodes_[boneNode].nodeId;
                const std::size_t qIndex = static_cast<std::size_t>(boneId) < skin.qbones.size()
                    ? static_cast<std::size_t>(boneId)
                    : boneNode;
                const std::array<float, 4> q = qIndex < skin.qbones.size()
                    ? skin.qbones[qIndex]
                    : std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f};
                BoneSlot& entry = boneSlots[slot];
                entry.valid = true;
                entry.pose = &world[boneNode];
                entry.inverse = quatMatrix(q[0], q[1], q[2], q[3]);
                entry.normal = multiply(entry.pose->rotation, entry.inverse);
                entry.offset = qIndex < skin.tbones.size()
                    ? skin.tbones[qIndex]
                    : Vec3{0.0f, 0.0f, 0.0f};
            }
        }
        for (std::uint32_t i = 0; i < node.vertexCount; ++i) {
            const std::size_t o = node.mdxOffset + static_cast<std::size_t>(i) * node.stride;
            Vec3 local;
            Vec3 normal;
            Vec2 uv{0.0f, 0.0f};
            Vec2 uv1{0.0f, 0.0f};
            if (node.saber.has_value() && i < node.saber->positions.size()) {
                local = node.saber->positions[i];
                normal = node.saber->normals[i];
                uv = node.saber->uvs[i];
            } else {
                local = {mdxFloat(o, "MDX x"),
                         mdxFloat(o + 4u, "MDX y"),
                         mdxFloat(o + 8u, "MDX z")};
                normal = {mdxFloat(o + 12u, "MDX nx"),
                          mdxFloat(o + 16u, "MDX ny"),
                          mdxFloat(o + 20u, "MDX nz")};
                if (node.uv0Offset != 0xFFFFFFFFu && node.uv0Offset + 8u <= node.stride) {
                    uv = {mdxFloat(o + node.uv0Offset, "MDX u"),
                          mdxFloat(o + node.uv0Offset + 4u, "MDX v")};
                }
                if (node.uv1Offset != 0xFFFFFFFFu && node.uv1Offset + 8u <= node.stride) {
                    uv1 = {mdxFloat(o + node.uv1Offset, "MDX u1"),
                           mdxFloat(o + node.uv1Offset + 4u, "MDX v1")};
                }
            }
            if (skinned) {
                const Skin& skin = *node.skin;
                const std::size_t weightBase = o + skin.weightsOffset;
                const std::size_t boneBase = o + skin.bonesOffset;
                Vec3 position{0.0f, 0.0f, 0.0f};
                Vec3 normalSum{0.0f, 0.0f, 0.0f};
                float totalWeight = 0.0f;
                for (int boneSlot = 0; boneSlot < 4; ++boneSlot) {
                    const float weight = mdxFloat(weightBase + static_cast<std::size_t>(boneSlot) * 4u,
                                                  "MDX bone weight");
                    if (weight <= 0.0f) {
                        continue;
                    }
                    const std::int32_t boneSlotIndex = static_cast<std::int32_t>(
                        std::lround(mdxFloat(boneBase + static_cast<std::size_t>(boneSlot) * 4u,
                                             "MDX bone index")));
                    if (boneSlotIndex < 0
                        || static_cast<std::size_t>(boneSlotIndex) >= skin.slotNode.size()) {
                        continue;
                    }
                    const BoneSlot& bone = boneSlots[static_cast<std::size_t>(boneSlotIndex)];
                    if (!bone.valid) {
                        continue;
                    }
                    const Vec3 inverseRotated = transform(bone.inverse, local);
                    const Vec3 bonePoint{inverseRotated.x + bone.offset.x, inverseRotated.y + bone.offset.y,
                                         inverseRotated.z + bone.offset.z};
                    const Pose& bonePose = *bone.pose;
                    const Vec3 transformed = transform(bonePose.rotation, bonePoint);
                    position.x += weight * (bonePose.translation.x + transformed.x);
                    position.y += weight * (bonePose.translation.y + transformed.y);
                    position.z += weight * (bonePose.translation.z + transformed.z);
                    const Vec3 rotatedNormal = transform(bone.normal, normal);
                    normalSum.x += weight * rotatedNormal.x;
                    normalSum.y += weight * rotatedNormal.y;
                    normalSum.z += weight * rotatedNormal.z;
                    totalWeight += weight;
                }
                if (totalWeight > 0.0f) {
                    positions.push_back(position);
                    normals.push_back(normalize(normalSum));
                } else {
                    const Vec3 transformed = transform(nodePose.rotation, local);
                    positions.push_back({nodePose.translation.x + transformed.x,
                                         nodePose.translation.y + transformed.y,
                                         nodePose.translation.z + transformed.z});
                    normals.push_back(transform(nodePose.rotation, normal));
                }
            } else {
                const Vec3 scaledLocal{local.x * nodePose.scale, local.y * nodePose.scale,
                                       local.z * nodePose.scale};
                const Vec3 transformed = transform(nodePose.rotation, scaledLocal);
                positions.push_back({nodePose.translation.x + transformed.x,
                                     nodePose.translation.y + transformed.y,
                                     nodePose.translation.z + transformed.z});
                normals.push_back(normalize(transform(nodePose.rotation, normal)));
            }
            if (danglyTime >= 0.0f && node.dangly.has_value()
                && i < node.dangly->constraints.size()) {
                const float constraint = std::clamp(node.dangly->constraints[i], 0.0f, 1.0f);
                const float stiffness = std::max(node.dangly->tightness, 0.1f);
                const float frequency = std::max(node.dangly->period, 0.1f);
                const float settle = 1.0f
                    - std::exp(-stiffness * danglyTime) * std::cos(frequency * danglyTime);
                const float droop = node.dangly->displacement * constraint
                    * std::clamp(settle, 0.0f, 1.0f);
                positions.back().z -= droop;
            }
            uvs.push_back(uv);
            uvs1.push_back(uv1);
        }
        const std::string texture = lowerAscii(node.texture0) != "null" ? node.texture0 : std::string{};
        Vec3 selfIllum{0.0f, 0.0f, 0.0f};
        float alpha = 1.0f;
        const auto applyTint = [&](const Controller& controller, float at) {
            if (controller.type == 100) {
                const std::vector<float> value = sampleController(controller, at);
                if (value.size() >= 3) {
                    selfIllum = {value[0], value[1], value[2]};
                }
            } else if (controller.type == 132) {
                const std::vector<float> value = sampleController(controller, at);
                if (!value.empty()) {
                    alpha = value[0];
                }
            }
        };
        for (const Controller& controller : node.controllers) {
            applyTint(controller, tintTime);
        }
        const auto applyClip = [&](const Animation& clip, float at) {
            for (const Node& animated : clip.nodes) {
                if (animated.supernode == node.supernode) {
                    for (const Controller& controller : animated.controllers) {
                        applyTint(controller, at);
                    }
                    break;
                }
            }
        };
        if (tintLayers != nullptr) {
            for (const auto& [clip, at] : *tintLayers) {
                applyClip(*clip, at);
            }
        } else if (tintClip != nullptr) {
            applyClip(*tintClip, tintTime);
        }
        std::vector<std::array<std::uint16_t, 3>> faces;
        std::vector<std::uint32_t> faceSurfaces;
        if (node.saber.has_value()) {
            // Retail draws a blade without its MDL face list (a placeholder run on all 64 shipped
            // blades). PartLightsaber::Draw 00471860, still blade path: two GL_QUAD_STRIPs over the
            // 176 vertices (constants 00740a48..00740a54: 8 core, 4 per row, 20 trail rows), pairs
            // (i, i+4) for i in 0..3, then (j+4, j) for j in 88..91. With two or more samples in the
            // swing trail the blade is drawn as the trail instead (saberTrailStrips).
            const Saber& blade = *node.saber;
            const bool laidOut = positions.size() >= static_cast<std::size_t>(kSaberSecond + 2 * kSaberRow)
                && blade.positions.size() == positions.size() && blade.uvs.size() == positions.size();
            // GL quad strip order: rows (a, c), quads (a_k, c_k, c_k+1, a_k+1).
            const auto stripFaces = [&](const std::vector<std::uint16_t>& a, const std::vector<std::uint16_t>& c) {
                for (std::size_t k = 0; k + 1 < a.size(); ++k) {
                    faces.push_back({a[k], c[k], a[k + 1]});
                    faces.push_back({a[k + 1], c[k], c[k + 1]});
                }
            };
            bool drewTrail = false;
            if (laidOut && saberTrail != nullptr && saberTrail->trails != nullptr) {
                std::vector<SaberTrail>& trails = *saberTrail->trails;
                if (trails.size() <= saberOrdinal) {
                    trails.resize(saberOrdinal + 1u);
                }
                SaberTrail& trail = trails[saberOrdinal];
                // The blade node's place in the world, without its scale (the part's vfunc +0xc).
                const Vec3 scaled{nodePose.translation.x * saberTrail->scale, nodePose.translation.y * saberTrail->scale,
                                  nodePose.translation.z * saberTrail->scale};
                const Vec3 turned = transform(saberTrail->rotation, scaled);
                const Vec3 centre{saberTrail->origin.x + turned.x, saberTrail->origin.y + turned.y,
                                  saberTrail->origin.z + turned.z};
                const SaberQuat turn = saberFromMatrix(multiply(saberTrail->rotation, nodePose.rotation));
                // 0046be00 updates a part once a frame; a refresh between frames only draws.
                if (saberTrail->step) {
                    saberTrailUpdate(trail, saberTrail->dt, centre, turn);
                }
                if (kTrailOn && trail.count >= 2) {
                    drewTrail = true;
                    for (const auto& rows : saberTrailStrips(trail, blade.positions, centre, turn)) {
                        std::vector<std::uint16_t> a;
                        std::vector<std::uint16_t> c;
                        for (const auto& [first, second] : rows) {
                            for (const TrailPoint* point : {&first, &second}) {
                                const Vec3 local{point->local.x * nodePose.scale, point->local.y * nodePose.scale,
                                                 point->local.z * nodePose.scale};
                                const Vec3 moved = transform(nodePose.rotation, local);
                                (point == &first ? a : c).push_back(static_cast<std::uint16_t>(positions.size()));
                                positions.push_back({nodePose.translation.x + moved.x, nodePose.translation.y + moved.y,
                                                     nodePose.translation.z + moved.z});
                                const Vec3 normal = normals[point->source];
                                normals.push_back(normal);
                                uvs.push_back(blade.uvs[point->uvFrom]);
                                uvs1.push_back({0.0f, 0.0f});
                            }
                        }
                        stripFaces(a, c);
                    }
                }
                ++saberOrdinal;
            }
            if (laidOut && !drewTrail) {
                const auto row = [](std::uint16_t first) {
                    std::vector<std::uint16_t> out;
                    for (std::uint16_t k = 0; k < kSaberRow; ++k) {
                        out.push_back(static_cast<std::uint16_t>(first + k));
                    }
                    return out;
                };
                stripFaces(row(0), row(kSaberRow));
                stripFaces(row(kSaberSecond + kSaberRow), row(kSaberSecond));
            }
            // A fixed count keeps the mesh's batches; the rest are degenerate.
            if (laidOut) {
                faces.resize(std::max(faces.size(), kSaberFaces), std::array<std::uint16_t, 3>{0, 0, 0});
            }
        } else {
            faces.reserve(node.faceCount);
            for (std::uint32_t f = 0; f < node.faceCount; ++f) {
                const std::size_t o = 12u + node.faceOffset + static_cast<std::size_t>(f) * 32u + 26u;
                faces.push_back({readU16(mdl_.data(), mdl_.size(), o, "MDL face a"),
                                 readU16(mdl_.data(), mdl_.size(), o + 2u, "MDL face b"),
                                 readU16(mdl_.data(), mdl_.size(), o + 4u, "MDL face c")});
                if ((node.type & 0x200u) != 0u) {
                    // An AABB face's surface (face +0x10), which retail's grass reads (00452cc0).
                    faceSurfaces.push_back(readU32(mdl_.data(), mdl_.size(), o - 10u, "MDL face surface"));
                }
            }
        }
        for (const std::array<std::uint16_t, 3>& face : faces) {
            const std::size_t faceIndex = static_cast<std::size_t>(&face - faces.data());
            const std::uint16_t ia = face[0];
            const std::uint16_t ib = face[1];
            const std::uint16_t ic = face[2];
            if (ia >= positions.size() || ib >= positions.size() || ic >= positions.size()) {
                throw FormatError(name_ + ": MDL face index out of range");
            }
            MdlMeshTriangle triangle;
            triangle.texture = texture;
            triangle.material = &node.material;
            if (faceIndex < faceSurfaces.size()) {
                triangle.surface = faceSurfaces[faceIndex];
            }
            triangle.selfIllum = selfIllum;
            triangle.alpha = alpha;
            const std::uint16_t indices[3] = {ia, ib, ic};
            for (int k = 0; k < 3; ++k) {
                triangle.v[k].position = positions[indices[k]];
                triangle.v[k].normal = normals[indices[k]];
                triangle.v[k].uv = uvs[indices[k]];
                triangle.v[k].uv1 = uvs1[indices[k]];
                triangle.v[k].node = static_cast<std::int32_t>(nodeIndex);
            }
            out.push_back(std::move(triangle));
        }
    }
    return out;
}

bool Mdl::hasFxNodes() const
{
    for (const Node& node : nodes_) {
        if ((node.type & 0x02u) || (node.type & 0x04u)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> Mdl::markRoomLoopMeshes()
{
    std::vector<std::string> clips;
    const auto differs = [](const Pose& a, const Pose& b) {
        const Vec3 x{1.0f, 0.0f, 0.0f};
        const Vec3 y{0.0f, 1.0f, 0.0f};
        return length(a.translation - b.translation) > 1e-3f
            || length(transform(a.rotation, x) - transform(b.rotation, x)) > 1e-3f
            || length(transform(a.rotation, y) - transform(b.rotation, y)) > 1e-3f;
    };
    constexpr int kSamples = 8;
    for (const char* clip : {"animloop1", "animloop2", "animloop3"}) {
        const int clipIndex = animationIndex(clip);
        if (clipIndex < 0) {
            continue;
        }
        const Animation& animation = *animationData_[static_cast<std::size_t>(clipIndex)];
        const auto claim = [&](Node& node) {
            if (node.loopClip < 0 && !node.scriptLoop) {
                node.loopAnimated = true;
                node.loopClip = clipIndex;
            }
        };
        for (int sample = 0; sample < kSamples; ++sample) {
            const std::vector<Pose> posed = poseWorld(
                &animation, animation.duration * static_cast<float>(sample) / static_cast<float>(kSamples));
            for (std::size_t i = 0; i < nodes_.size() && i < posed.size(); ++i) {
                if (nodes_[i].trimesh && differs(posed[i], bindWorld_[i])) {
                    claim(nodes_[i]);
                }
            }
        }
        for (const Node& animated : animation.nodes) {
            const bool tinted = std::any_of(animated.controllers.begin(), animated.controllers.end(),
                                            [](const Controller& controller) {
                return (controller.type == 100 || controller.type == 132) && controller.keys.size() > 1u;
            });
            // Retail applies these per frame: 00483330 writes each controller at part + its type, and the
            // mesh draw reads +0x64 (self illumination, 00473ba2) and +0x84 (alpha, 0046f460).
            if (!tinted) {
                continue;
            }
            for (Node& node : nodes_) {
                if (node.trimesh && node.supernode == animated.supernode) {
                    claim(node);
                }
            }
        }
        if (std::any_of(nodes_.begin(), nodes_.end(), [&](const Node& node) {
                return node.render && node.loopAnimated && node.loopClip == clipIndex;
            })) {
            clips.emplace_back(clip);
        }
    }
    return clips;
}

bool Mdl::markRoomScriptMeshes()
{
    // The clip names ACT_738 builds: "scriptloop0%d" below 10, else "scriptloop%d".
    std::vector<int> clips;
    for (int i = 1; i <= 99; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), i < 10 ? "scriptloop0%d" : "scriptloop%d", i);
        if (const int index = animationIndex(name); index >= 0) {
            clips.push_back(index);
        }
    }
    if (clips.empty()) {
        return false;
    }
    bool any = false;
    const auto claim = [&](Node& node) {
        if (node.trimesh && !node.scriptLoop) {
            node.scriptLoop = true;
            node.loopAnimated = true; // out of roomTriangles
            node.loopClip = -2;       // and out of every animloop mesh
            any = true;
        }
    };
    const auto differs = [](const Pose& a, const Pose& b) {
        const Vec3 x{1.0f, 0.0f, 0.0f};
        const Vec3 y{0.0f, 1.0f, 0.0f};
        return length(a.translation - b.translation) > 1e-3f
            || length(transform(a.rotation, x) - transform(b.rotation, x)) > 1e-3f
            || length(transform(a.rotation, y) - transform(b.rotation, y)) > 1e-3f
            || std::abs(a.scale - b.scale) > 1e-3f;
    };
    for (const int clipIndex : clips) {
        const Animation& animation = *animationData_[static_cast<std::size_t>(clipIndex)];
        // Any controller on a mesh node, one key or many: a clip that holds a skybox at alpha 0
        // or scale 0 does it with a constant.
        for (const Node& animated : animation.nodes) {
            if (animated.controllers.empty()) {
                continue;
            }
            for (Node& node : nodes_) {
                if (node.supernode == animated.supernode) {
                    claim(node);
                }
            }
        }
        // Children carried by an animated parent.
        constexpr int kSamples = 8;
        for (int sample = 0; sample <= kSamples; ++sample) {
            const std::vector<Pose> posed = poseWorld(
                &animation, animation.duration * static_cast<float>(sample) / static_cast<float>(kSamples));
            for (std::size_t i = 0; i < nodes_.size() && i < posed.size(); ++i) {
                if (nodes_[i].trimesh && differs(posed[i], bindWorld_[i])) {
                    claim(nodes_[i]);
                }
            }
        }
    }
    return any && std::any_of(nodes_.begin(), nodes_.end(), [](const Node& node) {
        return node.render && node.scriptLoop;
    });
}

std::vector<MdlMeshTriangle> Mdl::scriptLoopTriangles(const std::string& clip, float time, Vec3 origin) const
{
    const int index = clip.empty() ? -1 : animationIndex(clip);
    const Animation* animation = index >= 0 ? animationData_[static_cast<std::size_t>(index)].get() : nullptr;
    std::vector<MdlMeshTriangle> out = trianglesFromPose(
        animation != nullptr ? poseWorld(animation, time) : bindWorld_, -1.0f,
        [](const Node& node) { return node.render && node.scriptLoop; }, animation, time);
    for (MdlMeshTriangle& triangle : out) {
        for (MdlMeshVertex& vertex : triangle.v) {
            vertex.position = vertex.position + origin;
        }
    }
    return out;
}

std::vector<MdlMeshTriangle> Mdl::loopMeshTriangles(const std::string& clip, float time, Vec3 origin) const
{
    const int index = animationIndex(clip);
    const Animation* animation = index >= 0 ? animationData_[static_cast<std::size_t>(index)].get() : nullptr;
    std::vector<MdlMeshTriangle> out = trianglesFromPose(
        animation != nullptr ? poseWorld(animation, time) : bindWorld_, -1.0f,
        [index](const Node& node) { return node.render && node.loopAnimated && node.loopClip == index; },
        animation, time);
    for (MdlMeshTriangle& triangle : out) {
        for (MdlMeshVertex& vertex : triangle.v) {
            vertex.position = vertex.position + origin;
        }
    }
    return out;
}

std::vector<MdlMeshTriangle> Mdl::roomTriangles(Vec3 origin, bool gobParts) const
{
    // Which nodes sit under the "<model>a" node (the node itself included); <model> is the root node's name.
    std::string gobName;
    for (const Node& node : nodes_) {
        const auto parent = nodeByOffset_.find(node.parent);
        if (parent == nodeByOffset_.end() || &nodes_[parent->second] == &node) {
            gobName = lowerAscii(node.name) + "a";
            break;
        }
    }
    std::vector<std::uint8_t> inGob(nodes_.size(), 0u);
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        std::size_t at = i;
        for (std::size_t guard = 0; guard < nodes_.size(); ++guard) {
            if (!gobName.empty() && lowerAscii(nodes_[at].name) == gobName) {
                inGob[i] = 1u;
                break;
            }
            const auto parent = nodeByOffset_.find(nodes_[at].parent);
            if (parent == nodeByOffset_.end() || parent->second == at) {
                break;
            }
            at = parent->second;
        }
    }
    const Node* first = nodes_.data();
    std::vector<MdlMeshTriangle> out = trianglesFromPose(
        bindWorld_, -1.0f, [&](const Node& node) {
            const bool gob = inGob[static_cast<std::size_t>(&node - first)] != 0u;
            return node.render && !node.loopAnimated && gob == gobParts;
        });
    for (MdlMeshTriangle& triangle : out) {
        for (MdlMeshVertex& vertex : triangle.v) {
            vertex.position = vertex.position + origin;
        }
    }
    return out;
}

std::vector<MdlMeshTriangle> Mdl::walkmeshTriangles(Vec3 origin) const
{
    std::vector<MdlMeshTriangle> out = trianglesFromPose(
        bindWorld_, -1.0f, [](const Node& node) { return (node.type & 0x200u) != 0u; });
    for (MdlMeshTriangle& triangle : out) {
        for (MdlMeshVertex& vertex : triangle.v) {
            vertex.position = vertex.position + origin;
        }
    }
    return out;
}

std::vector<MdlMeshTriangle> Mdl::placedTriangles(Vec3 origin, const Mat3& rotation) const
{
    std::vector<MdlMeshTriangle> out = trianglesFromPose(bindWorld_, -1.0f, [](const Node& node) { return node.render; });
    for (MdlMeshTriangle& triangle : out) {
        for (MdlMeshVertex& vertex : triangle.v) {
            vertex.position = transform(rotation, vertex.position) + origin;
            vertex.normal = transform(rotation, vertex.normal);
        }
    }
    return out;
}

std::vector<Mdl::ReferenceNode> Mdl::references() const
{
    std::vector<ReferenceNode> out;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const Node& node = nodes_[i];
        if ((node.type & 0x10u) == 0u || (node.type & 0x20u) != 0u) {
            continue;
        }
        ReferenceNode reference;
        reference.model = readCString(mdl_.data(), mdl_.size(), 12u + static_cast<std::size_t>(node.offset) + 80u, 32,
                                      "MDL reference model", name_);
        reference.position = bindWorld_[i].translation;
        reference.rotation = bindWorld_[i].rotation;
        reference.node = i;
        reference.nodeName = node.name;
        if (!reference.model.empty()) {
            out.push_back(std::move(reference));
        }
    }
    return out;
}

void Mdl::nodeWorldAt(std::size_t node, const std::string& clip, float time, Vec3& position, Mat3& rotation) const
{
    const int index = clip.empty() ? -1 : animationIndex(clip);
    const Animation* animation = index >= 0 ? animationData_[static_cast<std::size_t>(index)].get() : nullptr;
    const std::vector<Pose> world = animation != nullptr ? poseWorld(animation, time) : bindWorld_;
    if (node < world.size()) {
        position = world[node].translation;
        rotation = world[node].rotation;
    }
}

std::vector<MdlMeshTriangle> Mdl::placedPosedTriangles(const std::string& clip, float time, Vec3 origin,
                                                       const Mat3& rotation) const
{
    const int index = clip.empty() ? -1 : animationIndex(clip);
    const Animation* animation = index >= 0 ? animationData_[static_cast<std::size_t>(index)].get() : nullptr;
    std::vector<MdlMeshTriangle> out = trianglesFromPose(
        animation != nullptr ? poseWorld(animation, time) : bindWorld_, -1.0f,
        [](const Node& node) { return node.render; }, animation, time);
    for (MdlMeshTriangle& triangle : out) {
        for (MdlMeshVertex& vertex : triangle.v) {
            vertex.position = transform(rotation, vertex.position) + origin;
            vertex.normal = transform(rotation, vertex.normal);
        }
    }
    return out;
}

bool Mdl::hasSkinnedMesh() const
{
    return std::any_of(nodes_.begin(), nodes_.end(), [](const Node& node) { return (node.type & 0x40u) != 0u; });
}

std::vector<MdlLight> Mdl::lights() const
{
    std::vector<MdlLight> out;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].light.has_value()) {
            MdlLight light = *nodes_[i].light;
            light.position = bindWorld_[i].translation;
            light.node = i;
            out.push_back(std::move(light));
        }
    }
    return out;
}

std::vector<Mdl::EmitterNode> Mdl::emitters() const
{
    std::vector<EmitterNode> out;
    std::size_t ordinal = 0;
    for (const Node& node : nodes_) {
        if (!(node.type & 0x02u) && !(node.type & 0x04u)) {
            continue;
        }
        if (node.emitter.has_value()) {
            out.push_back({ordinal, node.name, *node.emitter});
            for (const Node& child : nodes_) {
                if (child.parent == node.offset && (child.type & 0x10u) != 0u) {
                    out.back().header.targetNode = child.name;
                    break;
                }
            }
        }
        ++ordinal;
    }
    return out;
}

std::vector<MdlTriangle> Mdl::triangles(Vec3 roomOrigin) const
{
    std::vector<MdlTriangle> out;
    for (const MdlMeshTriangle& meshTriangle : meshTriangles()) {
        const std::int32_t nodeIndex = meshTriangle.v[0].node;
        if (nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= nodes_.size()
            || nodes_[static_cast<std::size_t>(nodeIndex)].type != 0x21u) {
            continue;
        }
        MdlTriangle triangle;
        for (int i = 0; i < 3; ++i) {
            triangle.p[i] = {meshTriangle.v[i].position.x + roomOrigin.x,
                             meshTriangle.v[i].position.y + roomOrigin.y,
                             meshTriangle.v[i].position.z + roomOrigin.z};
            triangle.uv[i] = meshTriangle.v[i].uv;
        }
        triangle.normal = normalize(cross(triangle.p[1] - triangle.p[0],
                                          triangle.p[2] - triangle.p[0]));
        triangle.texture = meshTriangle.texture;
        out.push_back(std::move(triangle));
    }
    return out;
}

} // namespace ke::kotor
