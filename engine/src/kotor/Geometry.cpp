#include "kotor/Formats.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
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
    model.supermodel_ = readCString(b.data(), b.size(), 12 + 136, 32, "MDL supermodel", model.name_);    const std::uint32_t root = readU32(b.data(), b.size(), 12 + 40, "MDL root");
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
            node.stride = readU32(b.data(), b.size(), m + 252u, "MDL stride");
            node.vertexCount = readU32(b.data(), b.size(), m + 304u, "MDL vertex count") & 0xFFFFu;
            requireRange(b.size(), m + 313u, 1u, "MDL render flag");
            node.render = b[m + 313u] != 0;
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
            model.animationData_.push_back(std::move(animation));
        }
    }
    model.bindWorld_ = model.poseWorld(nullptr, 0.0f);
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
    for (const Animation& animation : animationData_) {
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
    for (const Animation& animation : supermodel.animationData_) {
        if (animationIndex(animation.name) >= 0) {
            continue;
        }
        animations_.push_back({animation.name, animation.duration, animation.transition});
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
    for (const Animation& animation : animationData_) {
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
    for (const Animation& animation : animationData_) {
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
    for (const Animation& animation : animationData_) {
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
            for (const Node& animated : animationData_[static_cast<std::size_t>(clip)].nodes) {
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
    const Animation* anim = clip >= 0 ? &animationData_[static_cast<std::size_t>(clip)] : nullptr;
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
    return poseWorld(base.first, base.second, {}, &resolved);
}

std::vector<MdlMeshTriangle> Mdl::posedTrianglesLayered(const std::vector<MdlAnimLayer>& layers,
                                                        bool renderableOnly) const
{
    const auto filter = [renderableOnly](const Node& node) { return !renderableOnly || node.render; };
    return trianglesFromPose(layeredWorld(layers), -1.0f, filter);
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
                                     const std::map<std::string, Mat3>& extraRotations,
                                     const std::vector<std::pair<const Animation*, float>>* overlays) const
{
    std::vector<Pose> world(nodes_.size());
    std::vector<Mat3> extra(nodes_.size());
    for (Mat3& matrix : extra) {
        matrix = identity();
    }
    if (!extraRotations.empty()) {
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            const auto it = extraRotations.find(nodes_[i].name);
            if (it != extraRotations.end()) {
                extra[i] = it->second;
            }
        }
    }
    struct Local {
        Vec3 position;
        std::array<float, 4> rotation{1.0f, 0.0f, 0.0f, 0.0f};
        Vec3 scale{1.0f, 1.0f, 1.0f};
    };
    std::vector<Local> local(nodes_.size());
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        local[i].position = nodes_[i].position;
        local[i].rotation = {nodes_[i].qw, nodes_[i].qx, nodes_[i].qy, nodes_[i].qz};
    }
    // The base clip, then any overlays: a later layer replaces the channels it animates.
    const auto applyLayer = [&](const Animation& layer, float layerTime) {
    std::map<std::uint16_t, const Node*> animated;
    for (const Node& node : layer.nodes) {
        animated[node.supernode] = &node;
    }
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const Node& node = nodes_[i];
        const auto it = animated.find(node.supernode);
        if (it == animated.end()) {
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
                    const float g = 1.0f - t;
                    for (int c = 0; c < 3; ++c) {
                        const float p0 = a.data[static_cast<std::size_t>(c)];
                        const float p1 = a.data[static_cast<std::size_t>(6 + c)];
                        const float p2 = b.data[static_cast<std::size_t>(3 + c)];
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
        applyLayer(*animation, time);
    }
    if (overlays != nullptr) {
        for (const auto& [overlay, overlayTime] : *overlays) {
            applyLayer(*overlay, overlayTime);
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
        const Local& localPose = local[index];
        const Mat3 localRotation = multiply(
            quatMatrix(localPose.rotation[0], localPose.rotation[1],
                       localPose.rotation[2], localPose.rotation[3]),
            extra[index]);
        world[index].rotation = multiply(parent.rotation, localRotation);
        const Vec3 scaled{localPose.position.x * localPose.scale.x,
                          localPose.position.y * localPose.scale.y,
                          localPose.position.z * localPose.scale.z};
        const Vec3 offset = transform(parent.rotation, scaled);
        world[index].translation = {parent.translation.x + offset.x,
                                    parent.translation.y + offset.y,
                                    parent.translation.z + offset.z};
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
    const Animation* anim = clip >= 0 ? &animationData_[static_cast<std::size_t>(clip)] : nullptr;
    return trianglesFromPose(anim != nullptr ? poseWorld(anim, time) : bindWorld_, danglyTime);
}

std::vector<MdlMeshTriangle> Mdl::posedTrianglesRigged(
    const std::string& animation, float time, float danglyTime,
    const std::map<std::string, Mat3>& extraRotations) const
{
    if (extraRotations.empty()) {
        return posedTriangles(animation, time, danglyTime);
    }
    const int clip = animation.empty() ? -1 : animationIndex(animation);
    const Animation* anim = clip >= 0 ? &animationData_[static_cast<std::size_t>(clip)] : nullptr;
    return trianglesFromPose(poseWorld(anim, time, extraRotations), danglyTime);
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
        ? poseWorld(&animationData_[static_cast<std::size_t>(clipA)], timeA) : bindWorld_;
    const std::vector<Pose> worldB = clipB >= 0
        ? poseWorld(&animationData_[static_cast<std::size_t>(clipB)], timeB) : bindWorld_;
    const float t = std::clamp(blend, 0.0f, 1.0f);
    std::vector<Pose> blended = worldA;
    for (std::size_t i = 0; i < blended.size() && i < worldB.size(); ++i) {
        blended[i].translation = {worldA[i].translation.x
                                      + (worldB[i].translation.x - worldA[i].translation.x) * t,
                                  worldA[i].translation.y
                                      + (worldB[i].translation.y - worldA[i].translation.y) * t,
                                  worldA[i].translation.z
                                      + (worldB[i].translation.z - worldA[i].translation.z) * t};
        Mat3 rotation{};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                rotation.m[r][c] = worldA[i].rotation.m[r][c] * (1.0f - t)
                    + worldB[i].rotation.m[r][c] * t;
            }
        }
        // Gram-Schmidt keeps the blended rotation orthonormal.
        Vec3 x{rotation.m[0][0], rotation.m[1][0], rotation.m[2][0]};
        x = normalize(x);
        Vec3 y{rotation.m[0][1], rotation.m[1][1], rotation.m[2][1]};
        y = normalize(y - x * dot(x, y));
        Vec3 z = normalize(cross(x, y));
        blended[i].rotation.m[0][0] = x.x;
        blended[i].rotation.m[1][0] = x.y;
        blended[i].rotation.m[2][0] = x.z;
        blended[i].rotation.m[0][1] = y.x;
        blended[i].rotation.m[1][1] = y.y;
        blended[i].rotation.m[2][1] = y.z;
        blended[i].rotation.m[0][2] = z.x;
        blended[i].rotation.m[1][2] = z.y;
        blended[i].rotation.m[2][2] = z.z;
    }
    return trianglesFromPose(blended, danglyTime);
}

Vec3 Mdl::rootMotion(const std::string& animation, float time) const
{
    const int clip = animation.empty() ? -1 : animationIndex(animation);
    const Animation* anim = clip >= 0 ? &animationData_[static_cast<std::size_t>(clip)] : nullptr;
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

std::vector<MdlMeshTriangle> Mdl::trianglesFromPose(const std::vector<Pose>& world,
                                                    float danglyTime,
                                                    const std::function<bool(const Node&)>& filter) const
{
    std::vector<MdlMeshTriangle> out;
    for (std::size_t nodeIndex = 0; nodeIndex < nodes_.size(); ++nodeIndex) {
        const Node& node = nodes_[nodeIndex];
        if (!node.trimesh || (filter && !filter(node))) {
            continue;
        }
        const Pose& nodePose = world[nodeIndex];
        std::vector<Vec3> positions;
        std::vector<Vec3> normals;
        std::vector<Vec2> uvs;
        positions.reserve(node.vertexCount);
        normals.reserve(node.vertexCount);
        uvs.reserve(node.vertexCount);
        const bool skinned = node.skin.has_value();
        for (std::uint32_t i = 0; i < node.vertexCount; ++i) {
            const std::size_t o = node.mdxOffset + static_cast<std::size_t>(i) * node.stride;
            Vec3 local;
            Vec3 normal;
            Vec2 uv{0.0f, 0.0f};
            if (node.saber.has_value() && i < node.saber->positions.size()) {
                local = node.saber->positions[i];
                normal = node.saber->normals[i];
                uv = node.saber->uvs[i];
            } else {
                local = {readF32(mdx_.data(), mdx_.size(), o, "MDX x"),
                         readF32(mdx_.data(), mdx_.size(), o + 4u, "MDX y"),
                         readF32(mdx_.data(), mdx_.size(), o + 8u, "MDX z")};
                normal = {readF32(mdx_.data(), mdx_.size(), o + 12u, "MDX nx"),
                          readF32(mdx_.data(), mdx_.size(), o + 16u, "MDX ny"),
                          readF32(mdx_.data(), mdx_.size(), o + 20u, "MDX nz")};
                if (node.stride >= 32u) {
                    uv = {readF32(mdx_.data(), mdx_.size(), o + 24u, "MDX u"),
                          readF32(mdx_.data(), mdx_.size(), o + 28u, "MDX v")};
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
                    const float weight = readF32(mdx_.data(), mdx_.size(),
                                                 weightBase + static_cast<std::size_t>(boneSlot) * 4u,
                                                 "MDX bone weight");
                    if (weight <= 0.0f) {
                        continue;
                    }
                    const std::int32_t boneSlotIndex = static_cast<std::int32_t>(
                        std::lround(readF32(mdx_.data(), mdx_.size(),
                                            boneBase + static_cast<std::size_t>(boneSlot) * 4u,
                                            "MDX bone index")));
                    if (boneSlotIndex < 0
                        || static_cast<std::size_t>(boneSlotIndex) >= skin.slotNode.size()) {
                        continue;
                    }
                    const std::int32_t boneIndex =
                        skin.slotNode[static_cast<std::size_t>(boneSlotIndex)];
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
                    const Vec3 t = qIndex < skin.tbones.size()
                        ? skin.tbones[qIndex]
                        : Vec3{0.0f, 0.0f, 0.0f};
                    const Vec3 localOffset = local;
                    const Vec3 inverseRotated = transform(quatMatrix(q[0], q[1], q[2], q[3]),
                                                          localOffset);
                    const Vec3 bonePoint{inverseRotated.x + t.x, inverseRotated.y + t.y,
                                         inverseRotated.z + t.z};
                    const Pose& bonePose = world[boneNode];
                    const Vec3 transformed = transform(bonePose.rotation, bonePoint);
                    position.x += weight * (bonePose.translation.x + transformed.x);
                    position.y += weight * (bonePose.translation.y + transformed.y);
                    position.z += weight * (bonePose.translation.z + transformed.z);
                    const Vec3 rotatedNormal = transform(
                        multiply(bonePose.rotation, quatMatrix(q[0], q[1], q[2], q[3])), normal);
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
                const Vec3 transformed = transform(nodePose.rotation, local);
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
        }
        const std::string texture = !node.texture0.empty() ? node.texture0 : node.texture1;
        for (std::uint32_t f = 0; f < node.faceCount; ++f) {
            const std::size_t o = 12u + node.faceOffset + static_cast<std::size_t>(f) * 32u + 26u;
            const std::uint16_t ia = readU16(mdl_.data(), mdl_.size(), o, "MDL face a");
            const std::uint16_t ib = readU16(mdl_.data(), mdl_.size(), o + 2u, "MDL face b");
            const std::uint16_t ic = readU16(mdl_.data(), mdl_.size(), o + 4u, "MDL face c");
            if (ia >= positions.size() || ib >= positions.size() || ic >= positions.size()) {
                throw FormatError(name_ + ": MDL face index out of range");
            }
            MdlMeshTriangle triangle;
            triangle.texture = texture;
            const std::uint16_t indices[3] = {ia, ib, ic};
            for (int k = 0; k < 3; ++k) {
                triangle.v[k].position = positions[indices[k]];
                triangle.v[k].normal = normals[indices[k]];
                triangle.v[k].uv = uvs[indices[k]];
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

std::vector<MdlMeshTriangle> Mdl::fxTriangles(const std::string& animation, float time) const
{
    const int clip = animation.empty() ? -1 : animationIndex(animation);
    const Animation* anim = clip >= 0 ? &animationData_[static_cast<std::size_t>(clip)] : nullptr;
    const std::vector<Pose> world = poseWorld(anim, time);
    const auto addQuad = [](std::vector<MdlMeshTriangle>& out, Vec3 center, Vec3 extent,
                            const std::string& texture, BlendMode blend) {
        const Vec3 x{extent.x, 0.0f, 0.0f};
        const Vec3 y{0.0f, extent.y, 0.0f};
        const Vec3 z{0.0f, 0.0f, extent.z};
        const Vec3 axes[3][2] = {{x, y}, {x, z}, {y, z}};
        for (const auto& axis : axes) {
            MdlMeshTriangle triangle;
            triangle.texture = texture;
            triangle.blend = blend;
            triangle.v[0].position = center - axis[0] - axis[1];
            triangle.v[1].position = center + axis[0] - axis[1];
            triangle.v[2].position = center + axis[0] + axis[1];
            triangle.v[0].normal = {0.0f, 0.0f, 1.0f};
            triangle.v[1].normal = {0.0f, 0.0f, 1.0f};
            triangle.v[2].normal = {0.0f, 0.0f, 1.0f};
            out.push_back(triangle);
            triangle.v[0].position = center - axis[0] - axis[1];
            triangle.v[1].position = center + axis[0] + axis[1];
            triangle.v[2].position = center - axis[0] + axis[1];
            out.push_back(triangle);
        }
    };
    const auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    const auto mixColour = [&](Vec3 a, Vec3 b, float t) {
        return Vec3{mix(a.x, b.x, t), mix(a.y, b.y, t), mix(a.z, b.z, t)};
    };
    const auto curve = [&](float start, float mid, float end, float t) {
        if (mid < 0.0f) {
            return mix(start, end, t);
        }
        return t < 0.5f ? mix(start, mid, t * 2.0f) : mix(mid, end, (t - 0.5f) * 2.0f);
    };
    const auto curveColour = [&](Vec3 start, bool hasMid, Vec3 mid, Vec3 end, float t) {
        if (!hasMid) {
            return mixColour(start, end, t);
        }
        return t < 0.5f ? mixColour(start, mid, t * 2.0f)
                        : mixColour(mid, end, (t - 0.5f) * 2.0f);
    };
    const auto textureName = [](Vec3 colour, float alpha) {
        char name[32]{};
        std::snprintf(name, sizeof(name), "__fx_%02x%02x%02x%02x",
                      static_cast<int>(std::clamp(colour.x, 0.0f, 1.0f) * 255.0f),
                      static_cast<int>(std::clamp(colour.y, 0.0f, 1.0f) * 255.0f),
                      static_cast<int>(std::clamp(colour.z, 0.0f, 1.0f) * 255.0f),
                      static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
        return std::string(name);
    };
    std::vector<MdlMeshTriangle> out;
    for (std::size_t ordinal = 0; ordinal < fxNodeCount(); ++ordinal) {
        const FxNodeParameters params = fxNodeParameters(ordinal, animation, time);
        if (params.nodeIndex >= nodes_.size() || params.parameters.empty()) {
            continue;
        }
        const Vec3 origin = world[params.nodeIndex].translation;
        if (params.type & 0x02u) {
            const Vec3 colour = params.colour(76, {1.0f, 0.7f, 0.25f});
            const float alpha = params.scalar(132, 1.0f);
            const float radius = params.scalar(88, 0.15f);
            const float shadow = params.scalar(96, radius);
            const float multiplier = params.scalar(140, 1.0f);
            const float vertical = params.scalar(100, 0.0f);
            const float size = std::clamp(std::max(radius, shadow) * 0.05f
                                              * std::max(multiplier, 0.01f),
                                          0.05f, 0.5f);
            Vec3 centre = origin;
            centre.z += vertical;
            addQuad(out, centre, {size, size, size}, textureName(colour, alpha),
                    BlendMode::Additive);
            continue;
        }

        const Vec3 colourStart = params.colour(392, {1.0f, 0.7f, 0.25f});
        const Vec3 colourEnd = params.colour(380, colourStart);
        const bool hasColourMid = params.has(284);
        const Vec3 colourMid = params.colour(284, mixColour(colourStart, colourEnd, 0.5f));
        const float alphaStart = params.scalar(84, 1.0f);
        const float alphaEnd = params.scalar(80, alphaStart);
        const float alphaMidValue = params.has(216) ? params.scalar(216, 1.0f) : -1.0f;
        const float alphaMid = alphaMidValue >= 0.0f
            ? alphaMidValue : mix(alphaStart, alphaEnd, 0.5f);
        const float percentStart = params.scalar(220, 1.0f);
        const float percentMid = params.scalar(224, percentStart);
        const float percentEnd = params.scalar(228, percentStart);
        const float sizeStart = params.scalar(144, 0.2f);
        const float sizeEnd = params.scalar(148, sizeStart);
        const float sizeMidValue = params.has(232) ? params.scalar(232, sizeStart) : -1.0f;
        const float sizeMid = sizeMidValue >= 0.0f ? sizeMidValue : mix(sizeStart, sizeEnd, 0.5f);
        const float sizeStartYRaw = params.scalar(152, 0.0f);
        const float sizeEndYRaw = params.scalar(156, 0.0f);
        const float sizeMidYRaw = params.scalar(236, 0.0f);
        const float sizeStartY = sizeStartYRaw > 0.0f ? sizeStartYRaw : sizeStart;
        const float sizeEndY = sizeEndYRaw > 0.0f ? sizeEndYRaw : sizeEnd;
        const float sizeMidY = sizeMidYRaw > 0.0f ? sizeMidYRaw : mix(sizeStartY, sizeEndY, 0.5f);
        const float xsize = params.scalar(172, 0.0f);
        const float ysize = params.scalar(176, 0.0f);
        const float targetSize = params.scalar(252, 0.0f);
        const float birthRate = params.scalar(88, 4.0f);
        const float fps = std::max(params.scalar(104, 25.0f), 1.0f);
        const float life = std::clamp(params.scalar(120, 1.0f), 0.1f, 10.0f);
        const float velocity = params.scalar(168, 0.0f);
        const float spread = params.scalar(160, 0.0f);
        const float gravity = params.scalar(116, 0.0f);
        const float drag = params.scalar(100, 0.0f);
        const float mass = std::max(params.scalar(124, 1.0f), 0.01f);
        const float randVel = params.scalar(140, 0.0f);
        const float particleRot = params.scalar(136, 0.0f);
        const float blurLength = params.scalar(180, 0.0f);
        const float threshold = params.scalar(164, 0.0f);
        const float combine = params.scalar(96, 0.0f);
        const float frameStart = params.scalar(112, 0.0f);
        const float frameEnd = params.scalar(108, frameStart + 1.0f);
        const float p2p2 = params.scalar(128, 0.0f);
        const float p2p3 = params.scalar(132, 0.0f);
        const float bounce = params.scalar(92, 0.0f);
        const float lightningDelay = params.scalar(184, 0.0f);
        const float lightningRadius = params.scalar(188, 0.0f);
        const float lightningScale = params.scalar(192, 1.0f);
        const float lightningSubDiv = params.scalar(196, 1.0f);
        const float lightningZigzag = params.scalar(200, 0.0f);
        const float randomBirth = params.scalar(240, 0.0f);

        const int particleCount = std::clamp(
            static_cast<int>(std::lround(std::max(birthRate, 1.0f))) + 4, 4, 16);
        const Vec3 colour = curveColour(colourStart, hasColourMid, colourMid, colourEnd, 0.5f);
        const float alpha = curve(alphaStart, alphaMid, alphaEnd, 0.5f)
            * curve(percentStart, percentMid, percentEnd, 0.5f);
        const std::string texture = textureName(colour, alpha);

        for (int particle = 0; particle < particleCount; ++particle) {
            float phase = std::fmod(time / life + static_cast<float>(particle)
                                        / static_cast<float>(particleCount)
                                        + randomBirth * 0.0001f + combine * 0.01f,
                                    1.0f);
            if (phase < 0.0f) {
                phase += 1.0f;
            }
            phase = std::floor(phase * fps) / fps;
            const float age = phase * life;
            const float damping = drag > 0.0f && age > 0.0f
                ? (1.0f - std::exp(-drag * age)) / (drag * age) : 1.0f;
            const float framePhase = frameStart + phase * (frameEnd - frameStart);
            const float angle = particleRot + framePhase * 0.1f;
            const float particleAlpha = curve(alphaStart, alphaMid, alphaEnd, phase)
                * curve(percentStart, percentMid, percentEnd, phase);
            if (particleAlpha <= threshold) {
                continue;
            }
            float size = curve(sizeStart, sizeMid, sizeEnd, phase) * 0.01f;
            float sizeY = curve(sizeStartY, sizeMidY, sizeEndY, phase) * 0.01f;
            if (targetSize > 0.0f) {
                size = mix(size, targetSize * 0.01f, phase);
                sizeY = mix(sizeY, targetSize * 0.01f, phase);
            }
            if (xsize > 0.0f) {
                size = xsize * 0.005f;
            }
            if (ysize > 0.0f) {
                sizeY = ysize * 0.005f;
            }
            size = std::clamp(size, 0.01f, 0.2f);
            sizeY = std::clamp(sizeY, 0.01f, 0.2f);

            const float lateral = (spread + randVel) * age;
            const float lightning = (lightningRadius + lightningScale - 1.0f) * 0.01f
                * std::sin(phase * 6.283f * std::max(lightningSubDiv, 1.0f) + lightningDelay)
                * (1.0f + lightningZigzag * 0.1f);
            const float curveOffset = (p2p2 * std::sin(phase * 3.14159f)
                                          + p2p3 * std::sin(phase * 6.28318f)) * 0.01f;
            const float gravityDrop = 0.5f * (gravity / mass) * age * age;
            Vec3 centre = origin;
            centre.x += std::sin(angle + phase * 6.283f) * lateral + lightning + curveOffset;
            centre.y += std::cos(angle + phase * 4.712f) * lateral + lightning * 0.5f;
            centre.z += velocity * age * damping + gravityDrop + bounce * 0.1f
                * std::sin(phase * 3.14159f);
            centre.z += std::fmod(phase, 1.0f) * 0.05f;
            addQuad(out, centre, {size, sizeY, size + blurLength * 0.001f}, texture,
                    BlendMode::Alpha);
        }
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
