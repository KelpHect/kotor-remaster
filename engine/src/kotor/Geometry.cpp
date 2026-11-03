#include "kotor/Formats.h"

#include <cmath>
#include <cstring>
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
        face.a = readU16(bytes.data(), bytes.size(), o, "WOK face a");
        face.b = readU16(bytes.data(), bytes.size(), o + 2u, "WOK face b");
        face.c = readU16(bytes.data(), bytes.size(), o + 4u, "WOK face c");
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
    return wok;
}

namespace {

struct Mat3 {
    float m[3][3]{};
};

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
    const std::uint32_t root = readU32(b.data(), b.size(), 12 + 40, "MDL root");
    const std::uint32_t nodeCount = readU32(b.data(), b.size(), 12 + 44, "MDL node count");
    const std::uint32_t nameOff = readU32(b.data(), b.size(), 12 + 184, "MDL name offsets");
    const std::uint32_t nameCount = readU32(b.data(), b.size(), 12 + 188, "MDL name count");
    (void)nodeCount;
    std::vector<std::uint32_t> nameOffsets;
    if (nameCount != 0) {
        requireRange(b.size(), 12u + nameOff, static_cast<std::size_t>(nameCount) * 4u, "MDL name table");
        nameOffsets.reserve(nameCount);
        for (std::uint32_t i = 0; i < nameCount; ++i) {
            nameOffsets.push_back(readU32(b.data(), b.size(), 12u + nameOff + static_cast<std::size_t>(i) * 4u, "MDL name offset"));
        }
    }
    const auto nameAt = [&](std::uint32_t index) -> std::string {
        if (index >= nameOffsets.size()) {
            return {};
        }
        return readCString(b.data(), b.size(), 12u + nameOffsets[index], 64, "MDL node name");
    };

    std::vector<std::uint32_t> stack;
    if (root != 0) {
        stack.push_back(root);
    }
    while (!stack.empty()) {
        const std::uint32_t offset = stack.back();
        stack.pop_back();
        if (model.nodeByOffset_.find(offset) != model.nodeByOffset_.end()) {
            continue; // cycle or shared node: the first parse wins
        }
        const std::size_t a = 12u + offset;
        if (a + 80u > b.size()) {
            throw FormatError(model.name_ + ": MDL node overruns file");
        }
        Node node;
        node.offset = offset;
        node.type = readU16(b.data(), b.size(), a, "MDL node type");
        node.nameIndex = readU16(b.data(), b.size(), a + 4u, "MDL node name index");
        node.name = nameAt(node.nameIndex);
        node.parent = readU32(b.data(), b.size(), a + 12u, "MDL parent");
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
            requireRange(b.size(), 12u + kidsOff, static_cast<std::size_t>(kidsCount) * 4u, "MDL children");
            for (std::uint32_t i = 0; i < kidsCount; ++i) {
                node.children.push_back(readU32(b.data(), b.size(), 12u + kidsOff + static_cast<std::size_t>(i) * 4u, "MDL child"));
            }
        }
        if (node.type == 0x21u) {
            const std::size_t m = a + 80u;
            node.trimesh = true;
            node.faceOffset = readU32(b.data(), b.size(), m + 8u, "MDL face offset");
            node.faceCount = readU32(b.data(), b.size(), m + 12u, "MDL face count");
            node.texture0 = readCString(b.data(), b.size(), m + 88u, 32, "MDL texture0");
            node.texture1 = readCString(b.data(), b.size(), m + 120u, 32, "MDL texture1");
            node.stride = readU32(b.data(), b.size(), m + 252u, "MDL stride");
            node.vertexCount = readU32(b.data(), b.size(), m + 304u, "MDL vertex count") & 0xFFFFu;
            node.mdxOffset = readU32(b.data(), b.size(), m + 324u, "MDL mdx offset");
            if (node.stride < 12u) {
                throw FormatError(model.name_ + ": MDL vertex stride " + std::to_string(node.stride));
            }
            requireRange(model.mdx_.size(), node.mdxOffset, static_cast<std::size_t>(node.vertexCount) * node.stride, "MDX vertices");
            requireRange(b.size(), 12u + node.faceOffset, static_cast<std::size_t>(node.faceCount) * 32u, "MDL faces");
        }
        model.nodeByOffset_[offset] = model.nodes_.size();
        for (const std::uint32_t child : node.children) {
            stack.push_back(child);
        }
        model.nodes_.push_back(std::move(node));
    }
    return model;
}

std::vector<MdlTriangle> Mdl::triangles(Vec3 roomOrigin) const
{
    std::vector<MdlTriangle> out;
    for (const Node& node : nodes_) {
        if (!node.trimesh) {
            continue;
        }
        Mat3 rotation = identity();
        Vec3 translation{};
        std::vector<const Node*> chain;
        const Node* current = &node;
        for (int depth = 0; depth < 64 && current != nullptr; ++depth) {
            chain.push_back(current);
            const auto parent = nodeByOffset_.find(current->parent);
            current = parent == nodeByOffset_.end() ? nullptr : &nodes_[parent->second];
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const Node& part = **it;
            translation = translation + transform(rotation, part.position);
            rotation = multiply(rotation, quatMatrix(part.qw, part.qx, part.qy, part.qz));
        }
        std::vector<Vec3> vertices;
        std::vector<Vec2> uvs;
        vertices.reserve(node.vertexCount);
        uvs.reserve(node.vertexCount);
        for (std::uint32_t i = 0; i < node.vertexCount; ++i) {
            const std::size_t o = node.mdxOffset + static_cast<std::size_t>(i) * node.stride;
            Vec3 local{readF32(mdx_.data(), mdx_.size(), o, "MDX x"),
                       readF32(mdx_.data(), mdx_.size(), o + 4u, "MDX y"),
                       readF32(mdx_.data(), mdx_.size(), o + 8u, "MDX z")};
            const Vec3 world = roomOrigin + translation + transform(rotation, local);
            vertices.push_back(world);
            if (node.stride >= 32u) {
                uvs.push_back({readF32(mdx_.data(), mdx_.size(), o + 24u, "MDX u"),
                               readF32(mdx_.data(), mdx_.size(), o + 28u, "MDX v")});
            } else {
                uvs.push_back({0.0f, 0.0f});
            }
        }
        const std::string texture = !node.texture0.empty() ? node.texture0 : node.texture1;
        for (std::uint32_t f = 0; f < node.faceCount; ++f) {
            const std::size_t o = 12u + node.faceOffset + static_cast<std::size_t>(f) * 32u + 26u;
            const std::uint16_t ia = readU16(mdl_.data(), mdl_.size(), o, "MDL face a");
            const std::uint16_t ib = readU16(mdl_.data(), mdl_.size(), o + 2u, "MDL face b");
            const std::uint16_t ic = readU16(mdl_.data(), mdl_.size(), o + 4u, "MDL face c");
            if (ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size()) {
                throw FormatError(name_ + ": MDL face index out of range");
            }
            MdlTriangle triangle;
            triangle.p[0] = vertices[ia];
            triangle.p[1] = vertices[ib];
            triangle.p[2] = vertices[ic];
            triangle.uv[0] = uvs[ia];
            triangle.uv[1] = uvs[ib];
            triangle.uv[2] = uvs[ic];
            triangle.normal = normalize(cross(triangle.p[1] - triangle.p[0], triangle.p[2] - triangle.p[0]));
            triangle.texture = texture;
            out.push_back(std::move(triangle));
        }
    }
    return out;
}

} // namespace ke::kotor
