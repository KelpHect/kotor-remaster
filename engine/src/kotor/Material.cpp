// Render state for shipped materials: TXI directives, batch state from a node header and its
// texture, the light a dynamic object reads, and VIS room culling.

#include "kotor/Formats.h"

#include <vector>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <tuple>

namespace ke::kotor {

namespace {

std::string lowerText(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool imageHasAlpha(const Image& image)
{
    for (std::size_t i = 3; i < image.rgba.size(); i += 4) {
        if (image.rgba[i] != 255u) {
            return true;
        }
    }
    return false;
}

// Loads one shipped texture with its TXI. Returns false when absent or undecodable.
bool loadTexture(Install& install, const std::string& name, Texture& out)
{
    std::uint32_t type = 0;
    auto data = install.findTexture(name, &type);
    if (!data.has_value()) {
        return false;
    }
    try {
        const auto separate = install.find(name, 2022);
        std::string txiText = type == resType(ResType::Tpc) ? tpcEmbeddedTxi(*data) : std::string{};
        if (txiText.empty() && separate.has_value()) {
            txiText.assign(separate->begin(), separate->end());
        }
        out.name = name;
        out.txi = parseTxi(txiText);
        out.clamp = textureClampFlags(*data, type, separate.has_value() ? &*separate : nullptr);
        if (out.txi.cube && type == resType(ResType::Tpc)) {
            out.image = decodeCubeLatLong(*data, name);
        }
        if (out.image.rgba.empty()) {
            out.image = decodeTexture(*data, type, name);
            // Model UVs address a texture in file order: v = 0 is the first stored row, as in a
            // TPC. decodeTexture turns a bottom-up TGA upright for screens, which would put every
            // lightmap island on the wrong row (seams measured across shipped m01aa rooms: 49
            // in file order, 184 upright, on summed 0-255 RGB).
            if (type == resType(ResType::Tga) && data->size() > 17u && ((*data)[17] & 0x20u) == 0u
                && out.image.width > 0 && out.image.height > 1) {
                const std::size_t row = static_cast<std::size_t>(out.image.width) * 4u;
                for (int y = 0; y < out.image.height / 2; ++y) {
                    std::swap_ranges(out.image.rgba.begin() + static_cast<std::ptrdiff_t>(row * y),
                                     out.image.rgba.begin() + static_cast<std::ptrdiff_t>(row * (y + 1)),
                                     out.image.rgba.begin() + static_cast<std::ptrdiff_t>(row * (out.image.height - 1 - y)));
                }
            }
        }
        out.hasAlpha = imageHasAlpha(out.image);
        return true;
    } catch (const FormatError&) {
        return false;
    }
}

} // namespace

TxiInfo parseTxi(const std::string& text)
{
    TxiInfo info;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string key;
        std::string value;
        if (!(row >> key)) {
            continue;
        }
        row >> value;
        key = lowerText(std::move(key));
        const std::string lowered = lowerText(value);
        const auto number = [&](float fallback) {
            try {
                return value.empty() ? fallback : std::stof(value);
            } catch (const std::exception&) {
                return fallback;
            }
        };
        if (key == "blending") {
            // retail 0047abd0: additive and punchthrough; anything else keeps the default.
            if (lowered == "additive") {
                info.blending = TxiInfo::Blend::Additive;
            } else if (lowered == "punchthrough") {
                info.blending = TxiInfo::Blend::PunchThrough;
            }
        } else if (key == "decal") {
            info.decal = number(0.0f) != 0.0f;
        } else if (key == "cube") {
            info.cube = number(0.0f) != 0.0f;
        } else if (key == "envmaptexture") {
            info.envMap = lowered;
        } else if (key == "bumpyshinytexture") {
            info.bumpyShiny = lowered;
        } else if (key == "bumpmaptexture") {
            info.bumpMap = lowered;
        } else if (key == "isbumpmap") {
            info.isBumpMap = number(0.0f) != 0.0f;
        } else if (key == "bumpmapscaling") {
            info.bumpScale = number(1.0f);
        } else if (key == "isspecularbumpmap") {
            info.isSpecularBump = number(1.0f) != 0.0f;
        } else if (key == "specularcolor") {
            float green = number(1.0f);
            float blue = green;
            const float red = green;
            row >> green >> blue;
            info.specular = (red + green + blue) / 3.0f;
        } else if (key == "proceduretype") {
            info.procedure = lowered == "cycle" ? TxiInfo::Procedure::Cycle
                : lowered == "arturo"           ? TxiInfo::Procedure::Arturo
                : lowered == "water"            ? TxiInfo::Procedure::Water
                                                : TxiInfo::Procedure::Other;
        } else if (key == "distort") {
            info.distort = number(0.0f) != 0.0f;
        } else if (key == "distortangle") {
            info.distortAngle = number(1.0f) != 0.0f;
        } else if (key == "distortionamplitude") {
            info.distortAmplitude = number(5.0f);
        } else if (key == "speed") {
            info.speed = number(1.0f);
        } else if (key == "arturowidth" || key == "waterwidth") {
            info.gridWidth = static_cast<int>(number(0.0f));
        } else if (key == "arturoheight" || key == "waterheight") {
            info.gridHeight = static_cast<int>(number(0.0f));
        } else if (key == "numx") {
            info.numX = std::max(1, static_cast<int>(number(1.0f)));
        } else if (key == "numy") {
            info.numY = std::max(1, static_cast<int>(number(1.0f)));
        } else if (key == "fps") {
            info.fps = number(0.0f);
        } else if (key == "wateralpha") {
            info.waterAlpha = number(-1.0f);
        }
    }
    return info;
}

std::uint32_t ensureOptionalTexture(Scene& scene, Install& install, const std::string& rawName)
{
    const std::string name = lowerText(rawName);
    if (name.empty() || name == "null") {
        return kNoTexture;
    }
    if (const auto it = scene.textureIndex.find(name); it != scene.textureIndex.end()) {
        return it->second;
    }
    Texture texture;
    if (!loadTexture(install, name, texture)) {
        scene.textureIndex[name] = kNoTexture;
        return kNoTexture;
    }
    const std::uint32_t index = static_cast<std::uint32_t>(scene.textures.size());
    scene.textures.push_back(std::move(texture));
    scene.textureIndex[name] = index;
    return index;
}

std::uint32_t ensureSceneTexture(Scene& scene, Install& install, const std::string& rawName)
{
    const std::string name = lowerText(rawName);
    if (name.empty() || name == "null" || name == "__default") {
        return 0;
    }
    const std::uint32_t index = ensureOptionalTexture(scene, install, name);
    if (index == kNoTexture) {
        ++scene.missingTextures[name];
        return 0;
    }
    return index;
}

static bool sameColour(Vec3 a, Vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

std::string materialKey(const MdlMeshTriangle& triangle)
{
    std::string key = lowerText(triangle.texture);
    if (key.empty()) {
        key = "__default";
    }
    if (triangle.material != nullptr) {
        const MdlNodeMaterial& material = *triangle.material;
        if (!material.lightmap.empty()) {
            key += '|';
            key += material.lightmap;
        }
        if (material.fades) {
            key += "|f";
        }
        if (!sameColour(material.diffuse, {1.0f, 1.0f, 1.0f}) || !sameColour(material.ambient, {1.0f, 1.0f, 1.0f})) {
            key += "|m";
            for (const float value : {material.diffuse.x, material.diffuse.y, material.diffuse.z,
                                      material.ambient.x, material.ambient.y, material.ambient.z}) {
                key += std::to_string(value);
                key += ',';
            }
        }
        if (material.noFog) {
            key += "|n";
        }
        if (material.saber) {
            key += "|s";
        }
        if (material.hasTangents) {
            key += "|t";
        }
        if (material.animateUv) {
            key += "|u";
            key += std::to_string(material.uvDirX);
            key += ',';
            key += std::to_string(material.uvDirY);
        }
        if (material.transparencyHint != 0.0f) {
            key += "|h";
            key += std::to_string(static_cast<int>(material.transparencyHint));
        }
    }
    return key;
}

TextureBatch makeMaterialBatch(Scene& scene, Install& install, const MdlMeshTriangle& triangle, bool dynamicPart)
{
    TextureBatch batch;
    batch.blend = triangle.blend;
    batch.texture = ensureSceneTexture(scene, install, triangle.texture);
    // A scene not built by loadModule (chargen, menu) has no default texture at index 0.
    const bool hasTexture = batch.texture < scene.textures.size();
    const TxiInfo txi = hasTexture ? scene.textures[batch.texture].txi : TxiInfo{};
    const bool hasAlpha = hasTexture && scene.textures[batch.texture].hasAlpha;
    if (triangle.material != nullptr) {
        const MdlNodeMaterial& material = *triangle.material;
        batch.transparencyHint = material.transparencyHint;
        batch.noFog = material.noFog;
        // Retail 00473900 asks StaticPartNeedsMat 0046f410 only for a part with no owner object
        // (+0x40 null, 00473a1c): a room part with white diffuse, alpha 1 and no self-illumination
        // leaves GL's material at white whatever the mesh's ambient says (00473af6). A part of a
        // placed object (+0x40 set) takes the mesh colours always (00473a15 sets the flag, 00473b55
        // loads +0x8c ambient and +0x98 diffuse). RETAIL row 33: MGG_turret Shape01 and MGF_ebonhawk
        // Shape02, white diffuse, draw with GL ambient 0.
        if (dynamicPart || !sameColour(material.diffuse, {1.0f, 1.0f, 1.0f}) || triangle.alpha != 1.0f
            || !sameColour(triangle.selfIllum, {})) {
            batch.materialDiffuse = material.diffuse;
            batch.materialAmbient = material.ambient;
        }
        if (!material.lightmap.empty()) {
            batch.lightmap = ensureOptionalTexture(scene, install, material.lightmap);
            if (batch.lightmap == kNoTexture) {
                ++scene.missingTextures[material.lightmap];
            }
        }
        if (material.animateUv) {
            batch.scrollU = material.uvDirX;
            batch.scrollV = material.uvDirY;
        }
    }
    // retail 0047abd0 keeps the names; a shipped name of "default" asks for the area's map.
    std::string envName = !txi.envMap.empty() ? txi.envMap : txi.bumpyShiny;
    if (envName == "default") {
        envName = lowerText(scene.defaultEnvMap);
    }
    if (!envName.empty()) {
        batch.envmap = ensureOptionalTexture(scene, install, envName);
        if (batch.envmap == kNoTexture) {
            ++scene.missingTextures[envName];
        }
    }
    // Code only: retail 00470d30 takes a bump route only when the mesh has tangents (0043e410).
    // On the retail captures' card the NV2x shader level is off (0045fbd0 = 0), so the only bump
    // route left is 6 (RenderBumpmapFallback), reached with no lightmap and no environment map. A
    // lightmapped surface goes to 9 or 14 (00470590 / 0046fb10) and an environment-mapped one to
    // 10 or 11 (0046ff70): none of those reads the bump map (tar_m02ad LTS_Pwall01i walls). The
    // material still loads it (00470d30 asks Material::HasBumpMap of every mesh).
    if (!txi.bumpMap.empty() && triangle.material != nullptr && triangle.material->hasTangents) {
        batch.bumpmap = ensureOptionalTexture(scene, install, txi.bumpMap);
        if (batch.bumpmap != kNoTexture && (batch.lightmap != kNoTexture || batch.envmap != kNoTexture)) {
            batch.bumpmap = kNoTexture;
        } else if (batch.bumpmap == kNoTexture) {
            ++scene.missingTextures[txi.bumpMap];
        } else {
            Texture& bump = scene.textures[batch.bumpmap];
            const bool cycles = bump.txi.procedure == TxiInfo::Procedure::Cycle;
            batch.bumpCycleX = cycles ? bump.txi.numX : 1;
            batch.bumpCycleY = cycles ? bump.txi.numY : 1;
            batch.bumpCycleFps = cycles ? bump.txi.fps : 0.0f;
            // Code only, ATI route: case 5 of 00470d30 (bump, tangents, no lightmap, no environment
            // map) is 00476690, whose program 830250 has the term. Its con0 is light colour times
            // the map's specularcolor (0041ed60) times 1 when isspecularbumpmap (0041edf0).
            if (batch.envmap == kNoTexture && batch.lightmap == kNoTexture && bump.txi.isSpecularBump) {
                batch.bumpSpecular = bump.txi.specular;
            }
            // Retail 00424980 / 00424dd0 turn a one-channel isbumpmap texture into normals
            // (0041eef0, then 0041fec0); four-channel maps upload as they are. Every shipped bump
            // map that is one channel decodes grey here and every four-channel one does not.
            bool grey = bump.txi.isBumpMap && !bump.image.rgba.empty();
            for (std::size_t i = 0; grey && i + 3 < bump.image.rgba.size(); i += 4) {
                grey = bump.image.rgba[i] == bump.image.rgba[i + 1] && bump.image.rgba[i] == bump.image.rgba[i + 2];
            }
            if (grey) {
                heightToNormal(bump.image, bump.txi.bumpScale, batch.bumpCycleX, batch.bumpCycleY);
            }
        }
    }
    batch.twoSided = txi.decal;
    batch.decal = txi.decal;
    // PartLightsaber::Draw 00471860 turns culling off (004718ff, glDisable 0078d454 = GL_CULL_FACE)
    // for the blade and its swing trail, whose strips face either way with the swing.
    if (triangle.material != nullptr && triangle.material->saber) {
        batch.twoSided = true;
    }
    if (txi.procedure == TxiInfo::Procedure::Cycle && (txi.numX > 1 || txi.numY > 1)) {
        batch.cycleX = txi.numX;
        batch.cycleY = txi.numY;
        batch.cycleFps = txi.fps;
    }
    // Code only, ATI route: on 00476690 the texture's alpha masks the specular term of 830250, and
    // the output alpha is not the texture's, so the surface stays solid (c_rancor01, c_hutt01).
    const bool bumpMasks = batch.bumpmap != kNoTexture && batch.envmap == kNoTexture && batch.lightmap == kNoTexture;
    // Blend state from retail 0047af70 and the table at 0073f270:
    //   additive     SRC_ALPHA / ONE, no depth write, alpha > 0
    //   punchthrough ONE / ZERO, depth write, alpha > kPunchThroughAlpha
    //   default      SRC_ALPHA / ONE_MINUS_SRC_ALPHA, depth write, alpha > 0
    if (txi.blending == TxiInfo::Blend::Additive) {
        batch.blend = BlendMode::Additive;
        batch.writeDepth = false;
        batch.alphaRef = 0.0f;
    } else if (txi.blending == TxiInfo::Blend::PunchThrough) {
        batch.blend = BlendMode::Opaque;
        batch.alphaRef = kPunchThroughAlpha;
    } else if (batch.envmap != kNoTexture) {
        // Retail 0046ff70 with combiner setup 0x13 in 0042e600: texture times light, plus the map
        // times one minus the texture's alpha. The output alpha is the vertex's, so it stays solid.
        batch.envMask = true;
        // Retail 00470860: a bump-mapped surface under wateralpha 1 skips the texture pass and
        // draws only the map, blended at wateralpha.
        if (!txi.bumpMap.empty() && txi.waterAlpha >= 0.0f && txi.waterAlpha < 1.0f) {
            batch.envOnlyAlpha = txi.waterAlpha;
            batch.blend = BlendMode::Alpha;
        }
    } else if (hasAlpha && !bumpMasks) {
        batch.blend = BlendMode::Alpha;
        batch.alphaRef = 0.0f;
    }
    // Retail blends every default-state surface, so an alpha controller or a wateralpha needs the
    // blended pass. Punch-through is ONE / ZERO whatever the node's alpha says (0047af70).
    const bool punchThrough = txi.blending == TxiInfo::Blend::PunchThrough;
    if (txi.waterAlpha >= 0.0f && batch.blend == BlendMode::Opaque && !punchThrough) {
        batch.blend = BlendMode::Alpha;
    }
    if (triangle.material != nullptr && triangle.material->fades
        && batch.blend == BlendMode::Opaque && !punchThrough) {
        batch.blend = BlendMode::Alpha;
    }
    if (triangle.blend == BlendMode::Additive) {
        batch.blend = BlendMode::Additive;
    }
    // The environment path's output alpha is the vertex's, whatever the texture's alpha says.
    batch.showsBehind = (hasAlpha && !batch.envMask && !bumpMasks) || batch.envOnlyAlpha >= 0.0f || txi.waterAlpha >= 0.0f;
    return batch;
}

bool drawsWithStaticGeometry(const TextureBatch& batch)
{
    return batch.blend == BlendMode::Alpha && !batch.showsBehind && batch.envOnlyAlpha < 0.0f && batch.shell == 0u
        && batch.transparencyHint == 0.0f && !batch.roomObject;
}

bool blendedDrawsBefore(const TextureBatch& a, float distanceA, const TextureBatch& b, float distanceB)
{
    // Blended surfaces write depth (0047af70), so a scorch mark drawn before the floor under it
    // leaves a hole in the floor wherever its alpha is above zero. Which meshes retail draws first
    // goes through its texture lists (0046bdf0) and is not fully traced; this order is inferred.
    const bool additiveA = a.blend == BlendMode::Additive;
    const bool additiveB = b.blend == BlendMode::Additive;
    if (additiveA != additiveB) {
        return additiveB;
    }
    if (a.showsBehind != b.showsBehind) {
        return b.showsBehind;
    }
    if (a.transparencyHint != b.transparencyHint) {
        return a.transparencyHint < b.transparencyHint;
    }
    return distanceA > distanceB;
}

Vec3 meshEmission(const MdlMeshTriangle& triangle)
{
    // Paired frame korr_m33aa: lza_flr11 has self illumination 0.24,0.18,0.05 and a lightmap, and
    // retail draws texel x lightmap alone (26,23,15 from 99,102,96 x 67,58,40). Retail 00473af6 sets
    // the emission to zero on the path without the lit material. Lightmap meshes take that path.
    return triangle.material != nullptr && !triangle.material->lightmap.empty() ? Vec3{} : triangle.selfIllum;
}

SceneVertex toSceneVertex(const MdlMeshVertex& vertex, const MdlMeshTriangle& triangle)
{
    SceneVertex out;
    out.position = vertex.position;
    out.normal = vertex.normal;
    out.uv = vertex.uv;
    out.uv1 = vertex.uv1;
    out.selfIllum = meshEmission(triangle);
    out.alpha = triangle.alpha;
    return out;
}

SampledLight materialLight(const TextureBatch& batch, const SampledLight& light)
{
    SampledLight out = light;
    out.ambient = {light.ambient.x * batch.materialAmbient.x, light.ambient.y * batch.materialAmbient.y,
                   light.ambient.z * batch.materialAmbient.z};
    if (batch.decal) {
        // Retail 0046a9e0 starts a vertex's light at 1, 1, 1 instead of the part's ambient when the
        // material's decal byte (+0x40, TXI "decal", read at 0047aca0) is set; the lights add on
        // top and the sum clamps to 1, so a decal draws at its full texel. Pair:
        // minigame_turret_pair.png (mgf_turret MGF_ledred01, MGF_ledamber01, MGF_target02).
        out.ambient = {1.0f, 1.0f, 1.0f};
    }
    for (SampledLight::Light& one : out.lights) {
        one.colour = {one.colour.x * batch.materialDiffuse.x, one.colour.y * batch.materialDiffuse.y,
                      one.colour.z * batch.materialDiffuse.z};
    }
    return out;
}

Vec3 environmentComposite(Vec3 lit, Vec3 environment, float texelAlpha, bool adds, Vec3 lighting)
{
    // Code only, ATI route (the pair frames hide that extension). Fragment program 830258, with a
    // lightmap: r0 * (primary + r1) + r2 * (1 - r0.a). Program 830248, without: lerp(r0.a, r0 * primary, r1).
    // Bump route 00470860 at wateralpha 1: first pass keeps the texel alpha, second is
    // ONE_MINUS_DST_ALPHA / ONE, so it adds with or without a lightmap. The register combiner route
    // 0042e600 state 0x15 adds in both cases.
    // Without a lightmap the retail captures take 0046ff70's route without fragment shaders (their AMD driver
    // faults in GL_ATI_fragment_shader): the cube map is drawn alone under the vertex lighting (texture env
    // MODULATE), then the texture over it with SRC_ALPHA / ONE_MINUS_SRC_ALPHA (0073f270). The reflection is lit.
    // Paired on the swoop bike: every measured part within 2 levels (paired\minigame_swoop_env_pair.png).
    const Vec3 added = environment * (1.0f - texelAlpha);
    if (adds) {
        return lit + added;
    }
    return lit * texelAlpha + Vec3{added.x * lighting.x, added.y * lighting.y, added.z * lighting.z};
}

void heightToNormal(Image& image, float scale, int cellsX, int cellsY)
{
    const int cellW = image.width / std::max(cellsX, 1);
    const int cellH = image.height / std::max(cellsY, 1);
    if (cellW < 1 || cellH < 1) {
        return;
    }
    const std::vector<std::uint8_t> source = image.rgba;
    const auto height = [&](int x, int y) {
        return static_cast<float>(source[(static_cast<std::size_t>(y) * image.width + x) * 4u]) / 255.0f;
    };
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const int left = x / cellW * cellW;
            const int top = y / cellH * cellH;
            const int inX = x - left;
            const int inY = y - top;
            // Retail 0041fec0 (from 004260a0, upload GL_BGRA): forward differences that wrap inside
            // the cell, h scaled by 1/255 and the difference by bumpmapscaling (texture +0x18).
            // Bytes are trunc(v * 127 + 128) for B = 1/len, G = -dx/len and R = dy/len.
            const float here = height(x, y);
            const float dx = (here - height(left + (inX + 1) % cellW, y)) * scale;
            const float dy = (here - height(x, top + (inY + 1) % cellH)) * scale;
            const float inv = 1.0f / std::sqrt(dx * dx + dy * dy + 1.0f);
            const auto encode = [](float v) { return static_cast<std::uint8_t>(static_cast<int>(v * 127.0f + 128.0f)); };
            std::uint8_t* out = &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4u];
            out[0] = encode(dy * inv);
            out[1] = encode(-dx * inv);
            out[2] = encode(inv);
            out[3] = 255u;
        }
    }
}

Vec3 bumpReflection(Vec3 toEye, Vec3 tangent, Vec3 bitangent, Vec3 normal, Vec3 mapNormal)
{
    // Program 83025c, second phase: r3 = 2 r5 (r5.e) - e (r5.r5), r5 being the map's normal through
    // the frame in texture units 1 to 3.
    const Vec3 n = tangent * mapNormal.x + bitangent * mapNormal.y + normal * mapNormal.z;
    return n * (2.0f * dot(n, toEye)) - toEye * dot(n, n);
}

BumpCell bumpCellAt(const TextureBatch& batch, float clock)
{
    BumpCell cell;
    const int frames = batch.bumpCycleX * batch.bumpCycleY;
    if (frames > 1 && batch.bumpCycleFps > 0.0f) {
        const int frame = static_cast<int>(clock * batch.bumpCycleFps) % frames;
        cell.x = frame % batch.bumpCycleX;
        cell.y = frame / batch.bumpCycleX;
    }
    return cell;
}

float batchAlphaRef(const TextureBatch& batch, Vec3 eye)
{
    if (batch.maxDistance <= 0.0f) {
        return batch.alphaRef;
    }
    // Retail 004a6d30: a chunk's alpha is 1 - (d2 - 225) / (900 - 225), so it fades over 15 to 30 m
    // and is not drawn beyond. Here the fade raises the test instead of scaling the texel, which
    // passes the same pixels. The fade rides on the GL material's diffuse alpha (0044f880), which
    // only the lit route of a bin without a lightmap uses: a lightmapped bin draws its texel with
    // GL_REPLACE (0x1e01) and keeps the texel's alpha whole up to 30 m.
    if (batch.lightmap != kNoTexture) {
        return batch.alphaRef;
    }
    const Vec3 to = batch.center - eye;
    const float squared = dot(to, to);
    if (squared < 225.0f) {
        return batch.alphaRef;
    }
    const float fade = std::max(1.0f - (squared - 225.0f) / (900.0f - 225.0f), 0.001f);
    return batch.alphaRef / fade;
}

Vec3 batchFogColour(const TextureBatch& batch, Vec3 sceneFog)
{
    // Retail's blend state setter 0047af70 leaves fog alone, so an additive mesh fogs like the rest.
    // Only the additive emitter draws turn fog off, and they carry noFog.
    (void)batch;
    return sceneFog;
}

float batchShadeMode(const TextureBatch& batch)
{
    // Retail 00477830 (static meshes) and 0048d040 (skinned) draw a force shell's pass with the
    // "bumped-out" vertex programs at 0x797488 / 0x7a0b10 and 0x794600 / 0x79bc40: position plus
    // normal x env[12].x (the model's +0x11c, 0.02 from 006a1220 through 00448130) and colour
    // env[12].w = 1. So the pass is unlit, slightly outside the body, at the texel's own alpha.
    if (batch.shell != 0u) {
        return 3.0f;
    }
    return batch.opaqueOutput ? 2.0f : batch.unlit ? 1.0f : 0.0f;
}

BatchUv batchUvAt(const TextureBatch& batch, float clock)
{
    BatchUv uv;
    if (batch.scrollU != 0.0f || batch.scrollV != 0.0f) {
        // Retail 00483630 (ProceduralAnimateHierarchy, from Gob::Animate 00486670 once a drawn frame)
        // adds the trimesh's uv direction to every texture coordinate per call, with no time step.
        // Inferred: one call per frame at 60 frames a second.
        constexpr float kAnimateCallsPerSecond = 60.0f;
        uv.offsetU = batch.scrollU * clock * kAnimateCallsPerSecond;
        uv.offsetV = batch.scrollV * clock * kAnimateCallsPerSecond;
        uv.offsetU -= std::floor(uv.offsetU);
        uv.offsetV -= std::floor(uv.offsetV);
    }
    if (batch.cycleX > 1 || batch.cycleY > 1) {
        const int frames = batch.cycleX * batch.cycleY;
        const int frame = batch.cycleFps > 0.0f
            ? static_cast<int>(clock * batch.cycleFps) % frames : 0;
        uv.scaleU = 1.0f / static_cast<float>(batch.cycleX);
        uv.scaleV = 1.0f / static_cast<float>(batch.cycleY);
        uv.offsetU = static_cast<float>(frame % batch.cycleX) * uv.scaleU;
        uv.offsetV = static_cast<float>(frame / batch.cycleX) * uv.scaleV;
    }
    return uv;
}

namespace {

constexpr std::size_t kSunAmbientLight = static_cast<std::size_t>(-2);
constexpr std::size_t kSunDiffuseLight = static_cast<std::size_t>(-3);

SampledLight sampleLights(const Scene& scene, Vec3 position, bool useMultiplier, bool roomPass = false,
                          std::vector<std::pair<std::size_t, Scene::WorldLight>>* chosen = nullptr)
{
    SampledLight out;
    // Retail 00607610 hands the ARE's DynAmbientColor to scene +0x88 (0044fcd0), and 00451674 makes
    // that GL_LIGHT_MODEL_AMBIENT. It reaches everything that is lit, whichever lights are picked.
    out.ambient = scene.dynamicAmbient;
    // Retail's sun is the shipped model gidy_sun, placed at 4000, 4500, 7000 (00608edd to 00608f13),
    // with the ARE's colours set on its two lights (00606490, 006058c0). Its lights compete for the
    // three slots like any other: priority 1, type 1, radius 100000, one of them ambient only.
    constexpr Vec3 kSunPosition{4000.0f, 4500.0f, 7000.0f};
    constexpr float kSunRadius = 100000.0f;
    std::array<Scene::WorldLight, 2> sun{};
    for (Scene::WorldLight& light : sun) {
        light.position = kSunPosition;
        light.radius = kSunRadius;
        light.priority = 1;
        light.dynamicType = 1;
    }
    sun[0].colour = scene.ambient;
    sun[0].ambientOnly = true;
    sun[1].colour = scene.diffuse;
    // Order of retail's sort (004a28c0): lower priority number, then type 1, 2, 0, then nearer
    // over the light's radius, measured in the XY plane (004a2f30). Beyond the radius a light is
    // left out (004a3cf0). Inferred: the point measured from is the object's.
    struct Pick {
        const Scene::WorldLight* light = nullptr;
        float reach = 0.0f;
    };
    const auto typeRank = [](int type) { return type == 1 ? 0 : (type == 2 ? 1 : 2); };
    const auto before = [&](const Pick& a, const Pick& b) {
        if (a.light->priority != b.light->priority) {
            return a.light->priority < b.light->priority;
        }
        const int rankA = typeRank(a.light->dynamicType);
        const int rankB = typeRank(b.light->dynamicType);
        return rankA != rankB ? rankA < rankB : a.reach < b.reach;
    };
    std::array<Pick, 3> picks{};
    std::size_t picked = 0;
    const auto consider = [&](const Scene::WorldLight& light) {
        if (light.radius <= 0.0f
            || (light.colour.x <= 0.0f && light.colour.y <= 0.0f && light.colour.z <= 0.0f)) {
            return;
        }
        if (roomPass && light.dynamicType != 1) {
            return; // 004a4480 keeps only type 1 for room parts (004a44d6)
        }
        const float dx = light.position.x - position.x;
        const float dy = light.position.y - position.y;
        const float reach = std::sqrt(dx * dx + dy * dy) / light.radius;
        if (reach > 1.0f && !roomPass) { // 0046e5c0 picks for rooms with 1: no reach cut (004a3cf0)
            return;
        }
        const Pick pick{&light, reach};
        std::size_t at = picked;
        while (at > 0u && before(pick, picks[at - 1u])) {
            --at;
        }
        if (at >= picks.size()) {
            return;
        }
        for (std::size_t i = std::min(picked, picks.size() - 1u); i > at; --i) {
            picks[i] = picks[i - 1u];
        }
        picks[at] = pick;
        picked = std::min(picked + 1u, picks.size());
    };
    for (const Scene::WorldLight& light : sun) {
        consider(light);
    }
    for (const Scene::WorldLight& light : scene.lights) {
        consider(light);
    }
    if (chosen != nullptr) {
        // Identities for the object binder's per-object list: an index into scene.lights, or the two
        // sun lights at the top of the range.
        for (std::size_t i = 0; i < picked; ++i) {
            const Scene::WorldLight* light = picks[i].light;
            const std::size_t id = light == &sun[0] ? kSunAmbientLight
                : light == &sun[1]                 ? kSunDiffuseLight
                                                   : static_cast<std::size_t>(light - scene.lights.data());
            chosen->emplace_back(id, *light);
        }
    }
    std::size_t used = 0;
    for (std::size_t i = 0; i < picked; ++i) {
        const Scene::WorldLight& light = *picks[i].light;
        // Retail binds an object's lights in 004a3f70 as colour times its own fade (+0x78); the
        // multiplier (+0x8c) is only read by the other two binders, 004a2e70 and 004a3770.
        const Vec3 colour = useMultiplier ? light.colour * light.multiplier : light.colour;
        // Retail 004a2a00: quadratic attenuation 9 / (2 r^2) with r at least 0.1. An ambient-only
        // light sets GL_AMBIENT and leaves its diffuse black.
        const float radius = std::max(light.radius, 0.1f);
        const float quadratic = 9.0f / (2.0f * radius * radius);
        if (light.ambientOnly) {
            const Vec3 toLight = light.position - position;
            const float attenuation = 1.0f / (1.0f + quadratic * (toLight.x * toLight.x + toLight.y * toLight.y + toLight.z * toLight.z));
            out.ambient = out.ambient + colour * attenuation;
            continue;
        }
        out.lights[used].position = light.position;
        out.lights[used].colour = colour;
        out.lights[used].quadratic = quadratic;
        ++used;
    }
    return out;
}

} // namespace

SampledLight sampleSceneLight(const Scene& scene, Vec3 position)
{
    return sampleLights(scene, position, false);
}

// The object binder 004a3f70 keeps, per object, the lights it bound and a fade for each (state +4 / +0x10).
// Once per frame: a bound light still among the object's first three candidates fades in by
// 3 x frame time (007a1940 x 0078e574, capped at 1), or goes straight to 1 when the light does not fade
// (MDL light fadinglight, node +0xa8 == 0); a bound light no longer a candidate fades out the same way
// (straight to 0 when it does not fade) and leaves the list at 0; a candidate not yet bound joins at
// fade 0 while fewer than three are bound. GL gets colour x fade (004a43a9 onward, 004a2a00). RETAIL
// row 40 (soldier_sheen.json): the Endar soldier draws with GL_LIGHT1..3 enabled and black, because his
// lights joined while the game was paused and never faded in.
SampledLight objectSceneLight(const Scene& scene, std::uint32_t entity, Vec3 position)
{
    if (!scene.lightFades) {
        return sampleLights(scene, position, false);
    }
    std::vector<std::pair<std::size_t, Scene::WorldLight>> candidates;
    SampledLight out = sampleLights(scene, position, false, false, &candidates);
    Scene::ObjectLightFade& state = scene.objectLightFades[entity];
    if (state.frame != scene.lightFrame) {
        state.frame = scene.lightFrame;
        const float step = scene.lightFadeStep;
        for (std::size_t i = state.lights.size(); i-- > 0;) {
            const bool present = std::any_of(candidates.begin(), candidates.end(),
                                             [&](const auto& c) { return c.first == state.lights[i].first; });
            const bool fading = state.lights[i].second.fading;
            if (present) {
                state.fades[i] = fading ? std::min(state.fades[i] + step, 1.0f) : 1.0f;
            } else {
                state.fades[i] = fading ? std::max(state.fades[i] - step, 0.0f) : 0.0f;
                if (state.fades[i] <= 0.0f) {
                    state.lights.erase(state.lights.begin() + static_cast<std::ptrdiff_t>(i));
                    state.fades.erase(state.fades.begin() + static_cast<std::ptrdiff_t>(i));
                }
            }
        }
        for (const auto& candidate : candidates) {
            if (state.lights.size() >= 3u) {
                break;
            }
            const bool bound = std::any_of(state.lights.begin(), state.lights.end(),
                                           [&](const auto& l) { return l.first == candidate.first; });
            if (!bound) {
                state.lights.push_back(candidate);
                state.fades.push_back(0.0f);
            }
        }
    }
    out.ambient = scene.dynamicAmbient;
    out.lights = {};
    std::size_t used = 0;
    for (std::size_t i = 0; i < state.lights.size() && used < out.lights.size(); ++i) {
        const std::size_t id = state.lights[i].first;
        const Scene::WorldLight& light = id < scene.lights.size() ? scene.lights[id] : state.lights[i].second;
        const Vec3 colour = light.colour * state.fades[i];
        const float radius = std::max(light.radius, 0.1f);
        const float quadratic = 9.0f / (2.0f * radius * radius);
        if (light.ambientOnly) {
            const Vec3 toLight = light.position - position;
            out.ambient = out.ambient + colour * (1.0f / (1.0f + quadratic * dot(toLight, toLight)));
            continue;
        }
        out.lights[used].position = light.position;
        out.lights[used].colour = colour;
        out.lights[used].quadratic = quadratic;
        ++used;
    }
    return out;
}

SampledLight roomMeshLight(const Scene& scene, Vec3 position, bool blended)
{
    // Room parts are drawn by 00455710 under the lights 00450c30 binds through light-manager slot 2
    // (004a4480): from the list 004a3cf0 sorted (called with 1 from 0046e5c0, so no reach cut), only
    // lights with dynamic type 1 (+0x70 == 1, test at 004a44d6), at most three, bound by 004a4580 with
    // colour x fade x multiplier (+0x8c, 004a4a3d) into GL_LIGHT1..3; GL_LIGHT0 stays off (004a4549 ->
    // 0044da80, only 00401c43 / 0040468f turn it on, from 007a3c50, which nothing writes). The sun's
    // two gidy_sun lights are type 1. Inferred: the sort point is the part's (retail measures from
    // scene +0xa8, not decoded). Paired frames: texel x self illumination alone on end_m01ab and
    // sw_m01aa_10_E lhr_wall01 (retail 72,73,73 from 214 x 0.31) and sw_m01aa_08a_E lhr_wall07 (47,39,39
    // from 93,79,79 x 0.47), whose nearby lights are type 2.
    // RETAIL row 41 (palm_order_m41aa / m01aa): every frame runs 004a4580 once, then 00455710 draws every
    // room part under those lights, then each object binds its own through 004a3f70 before its parts. In
    // m41aa 004a4580 finds only type 2 lights and disables GL_LIGHT1..3, so all 52 room parts draw under
    // the model ambient alone; the palms are objects (OR_SmallPalm01/02, drawn after their own
    // ShadowAurora bind), not room parts.
    SampledLight light = sampleLights(scene, position, true, true);
    // Inferred from pairs: retail switches the model ambient between black (00451635) and DynAmbient
    // (00451674). Blended meshes without a lightmap measure as drawn under black (lta_flr01a in
    // tat_m18aa: a difference of 8.5 without DynAmbient, 28.2 with it; sta_m45aa and end_m01ab agree),
    // solid and punch-through ones under DynAmbient (lun_palm01 in unk_m41aa: 57,84,59 in retail).
    if (blended) {
        light.ambient = light.ambient - scene.dynamicAmbient;
    }
    return light;
}

namespace {

// The CRT rand of retail's static runtime: BuildGrassPolys 004a9630 reseeds it with srand
// (006fc43a) for every grass triangle, so each triangle's blades are fixed by its corners.
struct CrtRand {
    std::uint32_t state = 1u;
    int next()
    {
        state = state * 214013u + 2531011u;
        return static_cast<int>((state >> 16) & 0x7FFFu);
    }
    float unit() { return static_cast<float>(next() % 10000) * 0.0001f; } // _DAT_00741870
};

// Retail's quaternion as (w, x, y, z): Quaternion 004ac960 and its use on a vector, operator*
// 004a9bb0 (also the body of WindManager 004a5950's rotation).
struct GrassQuat {
    float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;
};

Vec3 grassRotate(const GrassQuat& q, Vec3 v)
{
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float a = q.y * q.w + q.z * q.x;
    const float b = q.y * q.x - q.z * q.w;
    const float c = q.z * q.y - q.x * q.w;
    const float d = q.z * q.w + q.y * q.x;
    const float e = q.z * q.y + q.x * q.w;
    const float f = q.z * q.x - q.y * q.w;
    return {(a + a) * v.z + (b + b) * v.y + (1.0f - (zz + yy + zz + yy)) * v.x,
            (c + c) * v.z + (d + d) * v.x + (1.0f - (zz + xx + zz + xx)) * v.y,
            (e + e) * v.y + (f + f) * v.x + (1.0f - (yy + xx + yy + xx)) * v.z};
}

// WindManager::FillNoiseField 004a57c0: a 16 x 16 field summed from blocks of 16, 8, 4, 2 and 1
// cells, each block one draw of rand() % (500 / (16 / size)) / 1000.
void fillGrassNoise(std::array<float, 256>& field, std::uint32_t& random)
{
    CrtRand rand{random};
    field.fill(0.0f);
    for (int size = 16; size > 0; size /= 2) {
        const int range = 500 / (16 / size);
        for (int row = 0; row + size <= 16; row += size) {
            for (int column = 0; column + size <= 16; column += size) {
                const float value = static_cast<float>(rand.next() % range) * 0.001f; // _DAT_0073d708
                for (int y = row; y < row + size; ++y) {
                    for (int x = column; x < column + size; ++x) {
                        field[static_cast<std::size_t>(y * 16 + x)] += value;
                    }
                }
            }
        }
    }
    random = rand.state;
}

// WindManager::GetGlobalWindNoPoint 004a5950 at `point`, scale 1: the global wind times the
// noise under the point, turned by two angles of up to the maximum deviations.
Vec3 grassGlobalWind(const GrassField& field, Vec3 point)
{
    if (field.wind.x == 0.0f && field.wind.y == 0.0f && field.wind.z == 0.0f) {
        return field.wind;
    }
    // ftol (truncate), times 4, absolute, modulo 16.
    const int cellX = std::abs(static_cast<int>(point.x) * 4) & 15;
    const int cellY = std::abs(static_cast<int>(point.y) * 4) & 15;
    const auto noiseAt = [&](int index) {
        return field.nextNoise[static_cast<std::size_t>(index)] * field.blend
             + (1.0f - field.blend) * field.noise[static_cast<std::size_t>(index)];
    };
    const float noise = noiseAt(cellX * 16 + cellY);
    const Vec3 wind = field.wind * noise;
    float first = 0.0f;
    float second = 0.0f;
    if (field.deviation != 0.0f) {
        const float scaled = noise * field.deviation;
        first = ((scaled + scaled) - field.deviation) * 0.5f;
    }
    if (field.deviationSecond != 0.0f) {
        const float scaled = noiseAt(cellY * 16 + cellX) * field.deviationSecond;
        second = ((scaled + scaled) - field.deviationSecond) * 0.5f;
    }
    // 004aadf0 / 004aae30: 32-step cosine and sine tables (00743fe8, 00744070), index
    // round(angle * 32 / 2pi) & 31.
    const auto step = [](float angle) {
        const int index = static_cast<int>(std::nearbyint(static_cast<double>(angle) * 5.0929580f)) & 31;
        return static_cast<double>(index) * (6.283185307179586 / 32.0);
    };
    const float cosFirst = static_cast<float>(std::cos(step(first)));
    const float sinFirst = static_cast<float>(std::sin(step(first)));
    const float cosSecond = static_cast<float>(std::cos(step(second)));
    const float sinSecond = static_cast<float>(std::sin(step(second)));
    GrassQuat turn;
    turn.w = cosSecond * cosFirst;
    turn.x = cosFirst * sinSecond;
    turn.y = sinSecond * sinFirst;
    turn.z = cosSecond * sinFirst;
    return grassRotate(turn, wind);
}

} // namespace

std::size_t addGrass(Scene& scene, Install& install, const GrassParams& params,
                     const std::map<std::uint32_t, std::vector<MdlMeshTriangle>>& walkmesh,
                     const std::vector<char>& surfaceGrass)
{
    GrassField& field = scene.grassField;
    field = GrassField{};
    // The scene's WindManager (004a6640) fills both noise fields and blends over 2 s. CSWCArea
    // 00605260 sets the wind from ARE WindPower: along (1, 1, 0) normalised, strength 0, 1 or 2,
    // with maximum deviations (0, 0), (100, 3) or (150, 5) degrees (00453140).
    fillGrassNoise(field.noise, field.random);
    fillGrassNoise(field.nextNoise, field.random);
    if (params.windPower == 1 || params.windPower == 2) {
        const float strength = params.windPower == 1 ? 1.0f : 2.0f;
        const float axis = static_cast<float>(1.0 / std::sqrt(2.0));
        field.wind = Vec3{axis, axis, 0.0f} * strength;
        constexpr float kDegrees = 0.017453292f; // _DAT_007415f8
        field.deviation = (params.windPower == 1 ? 100.0f : 150.0f) * kDegrees;
        field.deviationSecond = (params.windPower == 1 ? 3.0f : 5.0f) * kDegrees;
    }
    const float probTotal = params.probLL + params.probLR + params.probUL + params.probUR;
    if (params.texture.empty() || params.density <= 0.0f || params.quadSize <= 0.0f || probTotal <= 0.0f) {
        return 0;
    }
    const std::uint32_t texture = ensureOptionalTexture(scene, install, params.texture);
    if (texture == kNoTexture) {
        ++scene.missingTextures[lowerText(params.texture)];
        return 0;
    }
    // AurGrassSetInfo 004a74f0: the four weights are scaled to sum to one, in the order LL, LR,
    // UL, UR (ARE reader 00508c50, record +0x40).
    std::array<float, 4> weights{params.probLL, params.probLR, params.probUL, params.probUR};
    if (probTotal != 1.0f) {
        const float inverse = 1.0f / probTotal;
        for (float& weight : weights) {
            weight *= inverse;
        }
    }
    const float height = params.quadSize; // record +0x24
    // Scene::SpawnRoom 00456f30 bins only the room's AABB part: the part method at vtable +0x48
    // returns the part itself for PartAABB (0x741090 -> 00641db0) and null for every other part.
    // So grass grows on the AABB node's own faces, in their own corner order, with that node's
    // lightmap and lightmap uvs. Scene::AddFacesToBins 00452cc0: the faces go to bins on a 2.6 m
    // grid (_DAT_0078e428) from the lower corner of the AABB's bounds, by the face centre rounded
    // to 0.001. CAurTriangleBin::AddTriangle 004a7860 gives a face round(area x density) blades
    // (frndint) and leaves it out with none.
    constexpr float kCell = 2.6f;
    struct BinFaces {
        std::vector<std::pair<const MdlMeshTriangle*, int>> faces; // face, blade count
        std::uint32_t room = 0;
        Vec3 low{}, high{};
    };
    std::map<std::tuple<std::uint32_t, int, int>, BinFaces> bins;
    const auto roundMilli = [](float value) { return static_cast<float>(std::floor(value / 0.001 + 0.5) * 0.001); };
    for (const auto& [room, triangles] : walkmesh) {
        if (triangles.empty()) {
            continue;
        }
        Vec3 low = triangles.front().v[0].position;
        Vec3 high = low;
        for (const MdlMeshTriangle& triangle : triangles) {
            for (const MdlMeshVertex& vertex : triangle.v) {
                const Vec3& p = vertex.position;
                low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
                high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
            }
        }
        // InitGrassBins 00456e00: ceil((high - low) / 2.6) cells a side.
        const int cellsX = std::max(static_cast<int>(std::ceil((high.x - low.x) / kCell)), 1);
        const int cellsY = std::max(static_cast<int>(std::ceil((high.y - low.y) / kCell)), 1);
        for (const MdlMeshTriangle& triangle : triangles) {
            // AurGrassMaterialInUse 004a6840: the face's surface row has Grass set (004a74f0 is
            // called for rows 0..31 only, from 00607610).
            if (triangle.surface >= 32u || triangle.surface >= surfaceGrass.size() || surfaceGrass[triangle.surface] == 0) {
                continue;
            }
            const Vec3& c0 = triangle.v[0].position;
            const Vec3& c1 = triangle.v[1].position;
            const Vec3& c2 = triangle.v[2].position;
            const double area = 0.5 * static_cast<double>(length(cross(c1 - c0, c2 - c1)));
            const int blades = static_cast<int>(std::nearbyint(area * params.density));
            if (blades <= 0) {
                continue;
            }
            const Vec3 centre = (c0 + c1 + c2) * 0.33333334f; // _DAT_007415f4
            const int cellX = std::clamp(static_cast<int>(std::floor((roundMilli(centre.x) - low.x) / kCell)), 0, cellsX - 1);
            const int cellY = std::clamp(static_cast<int>(std::floor((roundMilli(centre.y) - low.y) / kCell)), 0, cellsY - 1);
            BinFaces& bin = bins[{room, cellX, cellY}];
            if (bin.faces.empty()) {
                bin.room = room;
                bin.low = c0;
                bin.high = c0;
            }
            for (const Vec3& corner : {c0, c1, c2}) {
                bin.low = {std::min(bin.low.x, corner.x), std::min(bin.low.y, corner.y), std::min(bin.low.z, corner.z)};
                bin.high = {std::max(bin.high.x, corner.x), std::max(bin.high.y, corner.y), std::max(bin.high.z, corner.z)};
            }
            bin.faces.emplace_back(&triangle, blades);
        }
    }
    // Light. 004a7860 sets bin flag 8 when the part has a lightmap (node +0x184, the MDL lightmap
    // byte) and lightmaps are on (DAT_0078e608 = 1); 004a9630 then gives each blade the face's
    // lightmap uv at the blade's barycentric weights. 004a6d30 draws such a bin with lighting off,
    // glColor 1, the grass texture on unit 0 as GL_REPLACE and the lightmap on unit 1 as
    // GL_MODULATE: texel x lightmap, the texel's own alpha. Grass_Ambient / Grass_Diffuse go to
    // glMaterialfv (0044f880: GL_FRONT, GL_AMBIENT / GL_DIFFUSE, alpha 1 / the fade) and to vertex
    // program env 94 / 95, which that texture setup does not read. A bin without a lightmap draws
    // under the pass's GL lighting (00450150 leaves it on; 004a6d30 turns it back on after a
    // lightmapped bin) with that material: here the room light times Grass_Ambient / Grass_Diffuse.
    std::map<std::string, std::uint32_t> lightmaps;
    const auto lightmapOf = [&](const MdlMeshTriangle& triangle) {
        if (triangle.material == nullptr || triangle.material->lightmap.empty()) {
            return kNoTexture;
        }
        auto [it, inserted] = lightmaps.try_emplace(triangle.material->lightmap, kNoTexture);
        if (inserted) {
            it->second = ensureOptionalTexture(scene, install, triangle.material->lightmap);
        }
        return it->second;
    };
    std::size_t quads = 0;
    for (auto& entry : bins) {
        BinFaces& bin = entry.second;
        // A lightmapped bin draws one run per lightmap (SortTriangles 004a8500, 004a6d30).
        std::stable_sort(bin.faces.begin(), bin.faces.end(), [&](const auto& a, const auto& b) {
            return lightmapOf(*a.first) < lightmapOf(*b.first);
        });
        GrassField::Bin record;
        record.center = (bin.low + bin.high) * 0.5f;
        record.height = height;
        record.room = bin.room;
        record.firstVertex = static_cast<std::uint32_t>(scene.vertices.size());
        TextureBatch batch;
        batch.texture = texture;
        // Retail's grass pass 00450150 sets glAlphaFunc(GREATER, 0.4) at 004501ed, whatever the ARE's
        // AlphaTest says, and blends SRC_ALPHA / ONE_MINUS_SRC_ALPHA (0x302, 0x303 at 0045017d).
        batch.blend = BlendMode::Alpha;
        batch.alphaRef = 0.4f;
        batch.twoSided = true; // 00450150 disables GL_CULL_FACE (0x0b44)
        batch.room = static_cast<int>(bin.room);
        batch.maxDistance = 30.0f; // 004a6d30: squared distance 900
        batch.center = record.center;
        const auto closeRun = [&]() {
            batch.indexCount = static_cast<std::uint32_t>(scene.indices.size()) - batch.firstIndex;
            if (batch.indexCount == 0) {
                return;
            }
            batch.bakedLight = batch.lightmap == kNoTexture;
            batch.baked = {};
            batch.materialAmbient = {1.0f, 1.0f, 1.0f};
            batch.materialDiffuse = {1.0f, 1.0f, 1.0f};
            if (batch.bakedLight) {
                batch.baked = roomMeshLight(scene, batch.center, true);
                batch.materialAmbient = params.ambient;
                batch.materialDiffuse = params.diffuse;
            }
            scene.batches.push_back(batch);
        };
        bool first = true;
        for (const auto& [triangle, blades] : bin.faces) {
            const std::uint32_t lightmap = lightmapOf(*triangle);
            if (first || lightmap != batch.lightmap) {
                if (!first) {
                    closeRun();
                }
                batch.lightmap = lightmap;
                batch.firstIndex = static_cast<std::uint32_t>(scene.indices.size());
                first = false;
            }
            const MdlMeshVertex* corner = triangle->v;
            const Vec3& c0 = corner[0].position;
            const Vec3& c1 = corner[1].position;
            const Vec3& c2 = corner[2].position;
            Vec3 normal = cross(c1 - c0, c2 - c0);
            const float normalLength = length(normal);
            normal = normalLength > 0.0f ? normal * (1.0f / normalLength) : Vec3{0.0f, 0.0f, 1.0f};
            // 004a9630: srand(round(c2.z + c1.y + c0.x)), then six rand() draws a blade: three
            // barycentric weights, the turn about z, the atlas quarter and the top's scale.
            CrtRand rand;
            rand.state = static_cast<std::uint32_t>(static_cast<int>(
                std::nearbyint(static_cast<double>(c2.z) + static_cast<double>(c1.y) + static_cast<double>(c0.x))));
            for (int blade = 0; blade < blades; ++blade) {
                float w0 = rand.unit();
                float w1 = rand.unit();
                float w2 = rand.unit();
                const float sum = w2 + w1 + w0;
                if (sum != 0.0f) {
                    const float inverse = 1.0f / sum;
                    w0 *= inverse;
                    w1 *= inverse;
                    w2 = inverse * w2;
                }
                const Vec3 base{w0 * c0.x + w1 * c1.x + w2 * c2.x, w0 * c0.y + w1 * c1.y + w2 * c2.y,
                                w0 * c0.z + w1 * c1.z + w2 * c2.z};
                const Vec2 lightUv{w0 * corner[0].uv1.x + w1 * corner[1].uv1.x + w2 * corner[2].uv1.x,
                                   w0 * corner[0].uv1.y + w1 * corner[1].uv1.y + w2 * corner[2].uv1.y};
                const float angle = rand.unit() * 6.2831855f; // _DAT_00740c88
                GrassQuat turn;
                if (angle != 0.0f) {
                    turn.w = static_cast<float>(std::cos(static_cast<double>(angle) * 0.5));
                    turn.z = static_cast<float>(std::sin(static_cast<double>(angle) * 0.5));
                }
                float pick = rand.unit();
                int quarter = 0; // also when the pick passes all four (rounding)
                for (int i = 0; i < 4; ++i) {
                    if (pick < weights[static_cast<std::size_t>(i)]) {
                        quarter = i;
                        break;
                    }
                    pick -= weights[static_cast<std::size_t>(i)];
                }
                // Quarter bit 0 picks the right half in u, bit 1 the second half in v; v runs from
                // the bottom of the blade (the texture's first rows) to its top.
                const float u0 = (quarter & 1) != 0 ? 0.5f : 0.0f;
                const float v0 = (quarter & 2) != 0 ? 0.5f : 0.0f;
                const float topScale = (rand.unit() - 0.5f) * 0.4f + 1.0f; // _DAT_0073e320
                const auto push = [&](Vec3 offset, float scale, float u, float v) {
                    SceneVertex vertex;
                    vertex.position = base + offset * scale;
                    vertex.normal = normal;
                    vertex.uv = {u, v};
                    vertex.uv1 = lightUv;
                    vertex.selfIllum = {};
                    vertex.alpha = 1.0f;
                    scene.vertices.push_back(vertex);
                };
                const float half = height * 0.5f;
                const std::uint32_t at = static_cast<std::uint32_t>(scene.vertices.size());
                push(grassRotate(turn, {-half, 0.0f, 0.0f}), 1.0f, u0, v0);
                push(grassRotate(turn, {half, 0.0f, 0.0f}), 1.0f, u0 + 0.5f, v0);
                push(grassRotate(turn, {half, 0.0f, height}), topScale, u0 + 0.5f, v0 + 0.5f);
                push(grassRotate(turn, {-half, 0.0f, height}), topScale, u0, v0 + 0.5f);
                // GL_QUADS (00426230) as two triangles.
                for (const std::uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u}) {
                    scene.indices.push_back(at + index);
                }
                ++quads;
            }
        }
        if (!first) {
            closeRun();
        }
        record.vertexCount = static_cast<std::uint32_t>(scene.vertices.size()) - record.firstVertex;
        if (record.vertexCount > 0) {
            field.bins.push_back(record);
        }
    }
    return quads;
}

void addPointSourceWind(Scene& scene, Vec3 position, float radius, float life, float strength)
{
    // 004a6510 -> 004a64c0: appended to the scene WindManager's array, kept in order.
    scene.grassField.pointWinds.push_back({position, radius, life, strength});
}

Vec3 grassPointWind(const GrassField& field, Vec3 point)
{
    // 004a5c80: each source within its radius, by the largest of |dx|, |dy|, |dz|, pushes along the
    // normalised offset (004ab180: (1, 0, 0) below length 1e-24) by (radius - d) / radius x strength.
    Vec3 sum{};
    for (const GrassField::PointWind& source : field.pointWinds) {
        const Vec3 offset = point - source.position;
        float d = std::abs(offset.x);
        d = std::max(d, std::abs(offset.y));
        d = std::max(d, std::abs(offset.z));
        if (!(d < source.radius)) {
            continue;
        }
        const float size = length(offset);
        const Vec3 direction = size < 1e-24f ? Vec3{1.0f, 0.0f, 0.0f} : offset * (1.0f / size);
        const float push = (source.radius - d) / source.radius * source.strength;
        sum = sum + direction * push;
    }
    return sum;
}

std::pair<std::size_t, std::size_t> stepGrassWind(Scene& scene, float frameSeconds, Vec3 eye)
{
    GrassField& field = scene.grassField;
    // WindManager::updateTimer 004a5e30 (from 00451427, with the frame time): every 2 s the next
    // field becomes the current one and a new next field is drawn; between, the two blend.
    constexpr float kNoisePeriod = 2.0f; // +0x848
    field.timer += frameSeconds;
    if (field.timer > kNoisePeriod) {
        std::swap(field.noise, field.nextNoise);
        fillGrassNoise(field.nextNoise, field.random);
        field.timer = 0.0f;
    }
    field.blend = field.timer / kNoisePeriod;
    // The same call ages each point source by the frame time and drops it once below 0, keeping
    // the others in order. Their strength does not fade meanwhile.
    for (GrassField::PointWind& source : field.pointWinds) {
        source.life -= frameSeconds;
    }
    field.pointWinds.erase(std::remove_if(field.pointWinds.begin(), field.pointWinds.end(),
                                          [](const GrassField::PointWind& source) { return source.life < 0.0f; }),
                           field.pointWinds.end());
    if (field.bins.empty()) {
        return {0, 0};
    }
    // 004a68b0: the frame time capped at 0.1 s, the wind scaled down to at most 5 long.
    const float dt = frameSeconds >= 0.1f ? 0.1f : frameSeconds;
    const float windLength = length(field.wind);
    const float windScale = windLength > 5.0f ? 5.0f / windLength : 1.0f;
    std::size_t first = scene.vertices.size();
    std::size_t end = 0;
    for (const GrassField::Bin& bin : field.bins) {
        // 004a6d30 runs the sway for a drawn bin closer than squared distance 225 (_DAT_007a1964).
        if (!scene.visibleRooms.empty()
            && (bin.room >= scene.visibleRooms.size() || scene.visibleRooms[bin.room] == 0)) {
            continue;
        }
        const Vec3 to = bin.center - eye;
        if (dot(to, to) >= 225.0f) {
            continue;
        }
        if (static_cast<std::size_t>(bin.firstVertex) + bin.vertexCount > scene.vertices.size()) {
            continue;
        }
        const float strength = 1.0f * bin.height * dt; // _DAT_007a1978 x record +0x24 x dt
        for (std::uint32_t q = 0; q + 4u <= bin.vertexCount; q += 4u) {
            SceneVertex* blade = &scene.vertices[bin.firstVertex + q];
            const Vec3& v0 = blade[0].position;
            const Vec3& v1 = blade[1].position;
            Vec3& v2 = blade[2].position;
            Vec3& v3 = blade[3].position;
            const Vec3 point{(v1.x + v0.x) * 0.5f, (v1.y + v0.y) * 0.5f, v0.z};
            const Vec3 global = grassGlobalWind(field, point) * windScale;
            // Plus the point sources at the same point (004a6440, unscaled); z only counts for the
            // test below.
            const Vec3 local = field.pointWinds.empty() ? Vec3{} : grassPointWind(field, point);
            const Vec3 pull{global.x + (v0.x - v3.x) + local.x, global.y + (v0.y - v3.y) + local.y,
                            global.z + local.z};
            if (pull.x == 0.0f && pull.y == 0.0f && pull.z == 0.0f) {
                continue;
            }
            v3.x = pull.x * strength + v3.x;
            v3.y = pull.y * strength + v3.y;
            v2.x = (v3.x - v0.x) + v1.x;
            v2.y = (v3.y - v0.y) + v1.y;
        }
        first = std::min<std::size_t>(first, bin.firstVertex);
        end = std::max<std::size_t>(end, static_cast<std::size_t>(bin.firstVertex) + bin.vertexCount);
    }
    return end > first ? std::pair<std::size_t, std::size_t>{first, end} : std::pair<std::size_t, std::size_t>{0, 0};
}

bool captureCameraPose(const Scene& scene, const std::string& fullSpec, Vec3& eye, Vec3& target, float& fov)
{
    const std::string spec = fullSpec.rfind("novis:", 0) == 0 ? fullSpec.substr(6) : fullSpec;
    if (spec.rfind("tex:", 0) == 0) {
        // tex:<texture>: straight at the first surface drawn with it, for a close look.
        const std::string wanted = spec.substr(4u);
        const auto aim = [&](const std::vector<SceneVertex>& vertices, const std::vector<std::uint32_t>& indices,
                             const TextureBatch& batch, std::uint32_t entity) {
            if (batch.texture >= scene.textures.size() || scene.textures[batch.texture].name != wanted
                || batch.indexCount < 3u) {
                return false;
            }
            // The largest triangle, so that the view is not on a sliver.
            float best = -1.0f;
            Vec3 centre, normal;
            for (std::uint32_t i = 0; i + 2u < batch.indexCount; i += 3u) {
                const Vec3 p0 = vertices[indices[batch.firstIndex + i]].position;
                const Vec3 p1 = vertices[indices[batch.firstIndex + i + 1u]].position;
                const Vec3 p2 = vertices[indices[batch.firstIndex + i + 2u]].position;
                const Vec3 facing = cross(p1 - p0, p2 - p0);
                if (length(facing) > best) {
                    best = length(facing);
                    centre = (p0 + p1 + p2) * (1.0f / 3.0f);
                    normal = normalize(facing);
                }
            }
            if (entity < scene.entities.size()) {
                const Entity& owner = scene.entities[entity];
                const float c = std::cos(owner.drawBearing());
                const float n = std::sin(owner.drawBearing());
                centre = owner.position + Vec3{c * centre.x - n * centre.y, n * centre.x + c * centre.y, centre.z};
                normal = {c * normal.x - n * normal.y, n * normal.x + c * normal.y, normal.z};
            }
            const float away = std::clamp(std::sqrt(best) * 1.2f, 0.6f, 6.0f);
            eye = centre + normal * away + Vec3{0.0f, 0.0f, 0.02f};
            target = centre;
            fov = 55.0f;
            return true;
        };
        for (const TextureBatch& batch : scene.batches) {
            if (aim(scene.vertices, scene.indices, batch, UINT32_MAX)) {
                return true;
            }
        }
        for (const EntityMesh& mesh : scene.entityMeshes) {
            for (const TextureBatch& batch : mesh.batches) {
                if (aim(mesh.vertices, mesh.indices, batch, mesh.entity)) {
                    return true;
                }
            }
        }
        return false;
    }
    if (spec.rfind("grass:", 0) == 0) {
        // grass:<metres>: the nearest grass chunk to the entry, seen from this far away.
        const float away = std::strtof(spec.c_str() + 6u, nullptr);
        const TextureBatch* best = nullptr;
        for (const TextureBatch& batch : scene.batches) {
            if (batch.maxDistance > 0.0f
                && (best == nullptr || length(batch.center - scene.entry) < length(best->center - scene.entry))) {
                best = &batch;
            }
        }
        if (best == nullptr) {
            return false;
        }
        target = best->center;
        eye = {target.x + away * 0.7071f, target.y + away * 0.7071f, target.z + 1.7f};
        return true;
    }
    if (spec.rfind("reference:", 0) == 0) {
        // reference:<n>: the nth skinned model shown by a reference node.
        std::size_t left = std::strtoul(spec.c_str() + 10u, nullptr, 10);
        for (const EntityMesh& mesh : scene.entityMeshes) {
            if (!mesh.referenceHolder || mesh.vertices.empty()) {
                continue;
            }
            if (left-- != 0u) {
                continue;
            }
            Vec3 sum{};
            for (const SceneVertex& vertex : mesh.vertices) {
                sum = sum + vertex.position;
            }
            target = sum * (1.0f / static_cast<float>(mesh.vertices.size()));
            eye = {target.x + 3.5f, target.y + 3.5f, target.z + 1.5f};
            return true;
        }
        return false;
    }
    if (spec.rfind("lightning:", 0) == 0) {
        // lightning:<n>: the nth world Lightning emitter, nearest the entry first.
        const std::size_t index = std::strtoul(spec.c_str() + 10u, nullptr, 10);
        std::vector<std::pair<float, Vec3>> found;
        for (const EmitterInstance& emitter : scene.emitters) {
            if (lowerText(emitter.header.update) != "lightning") {
                continue;
            }
            if (emitter.entity < scene.entities.size()) {
                found.emplace_back(length(scene.entities[emitter.entity].position - scene.entry),
                                   scene.entities[emitter.entity].position + Vec3{0.0f, 0.0f, 1.0f});
                continue;
            }
            if (!emitter.poseValid) {
                continue;
            }
            const Mat3& turn = emitter.orientation;
            const Vec3& local = emitter.worldPosition;
            const Vec3 at = emitter.origin
                + Vec3{turn.m[0][0] * local.x + turn.m[0][1] * local.y + turn.m[0][2] * local.z,
                       turn.m[1][0] * local.x + turn.m[1][1] * local.y + turn.m[1][2] * local.z,
                       turn.m[2][0] * local.x + turn.m[2][1] * local.y + turn.m[2][2] * local.z};
            found.emplace_back(length(at - scene.entry), at);
        }
        std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (index >= found.size()) {
            return false;
        }
        target = found[index].second;
        eye = {target.x + 5.0f, target.y + 5.0f, target.z + 1.5f};
        return true;
    }
    if (spec.rfind("emitter:", 0) == 0) {
        // emitter:<node name part>:<n>, nearest the entry first.
        const std::size_t split = spec.find(':', 8u);
        const std::string part = spec.substr(8u, split == std::string::npos ? std::string::npos : split - 8u);
        const std::size_t index = split == std::string::npos
            ? 0u : static_cast<std::size_t>(std::strtoul(spec.c_str() + split + 1u, nullptr, 10));
        std::vector<std::pair<float, Vec3>> found;
        for (const EmitterInstance& emitter : scene.emitters) {
            if (emitter.entity != UINT32_MAX || emitter.textureMissing || !emitter.poseValid) {
                continue;
            }
            std::string name = emitter.node;
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name.find(part) == std::string::npos) {
                continue;
            }
            const Mat3& turn = emitter.orientation;
            const Vec3& local = emitter.worldPosition;
            const Vec3 at = emitter.origin
                + Vec3{turn.m[0][0] * local.x + turn.m[0][1] * local.y + turn.m[0][2] * local.z,
                       turn.m[1][0] * local.x + turn.m[1][1] * local.y + turn.m[1][2] * local.z,
                       turn.m[2][0] * local.x + turn.m[2][1] * local.y + turn.m[2][2] * local.z};
            found.emplace_back(length(at - scene.entry), at);
        }
        std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (index >= found.size()) {
            return false;
        }
        target = found[index].second;
        eye = {target.x + 1.8f, target.y + 1.8f, target.z + 0.8f};
        return true;
    }
    if (spec == "entry") {
        const float dirLength = std::max(length(scene.entryDirection), 0.001f);
        const Vec3 dir{scene.entryDirection.x / dirLength, scene.entryDirection.y / dirLength, 0.0f};
        eye = {scene.entry.x - dir.x * 0.5f, scene.entry.y - dir.y * 0.5f, scene.entry.z + 1.7f};
        target = {scene.entry.x + dir.x * 6.0f, scene.entry.y + dir.y * 6.0f, scene.entry.z + 1.2f};
        return true;
    }
    if (spec.rfind("node:", 0) == 0) {
        // node:<tag>:<model node>:<distance>[:<degrees round from the facing>]: a close-up of a
        // creature's node as it is posed now, such as its head hook or hand.
        const std::size_t tagEnd = spec.find(':', 5u);
        const std::size_t nodeEnd = tagEnd == std::string::npos ? tagEnd : spec.find(':', tagEnd + 1u);
        if (nodeEnd == std::string::npos) {
            return false;
        }
        const std::string tag = spec.substr(5u, tagEnd - 5u);
        const std::string node = spec.substr(tagEnd + 1u, nodeEnd - tagEnd - 1u);
        char* rest = nullptr;
        const float reach = std::strtof(spec.c_str() + nodeEnd + 1u, &rest);
        const float round = rest != nullptr && *rest == ':' ? std::strtof(rest + 1, nullptr) * 0.0174533f : 0.0f;
        for (const EntityMesh& mesh : scene.entityMeshes) {
            if (mesh.attachModel || !mesh.model || mesh.entity >= scene.entities.size()
                || scene.entities[mesh.entity].tag != tag) {
                continue;
            }
            const Entity& entity = scene.entities[mesh.entity];
            const std::optional<Mdl::Pose> pose = mesh.model->nodePose(node, mesh.animation, mesh.animationTime);
            if (!pose.has_value()) {
                return false;
            }
            const float c = std::cos(entity.drawBearing());
            const float s = std::sin(entity.drawBearing());
            target = {entity.position.x + c * pose->translation.x - s * pose->translation.y,
                      entity.position.y + s * pose->translation.x + c * pose->translation.y,
                      entity.position.z + pose->translation.z};
            // A model faces +Y, a quarter turn on from the bearing.
            const float angle = entity.drawBearing() + 1.5707963f + round;
            target.z += reach < 1.0f ? 0.1f : 0.0f; // A head's hook is at its neck.
            eye = {target.x + std::cos(angle) * reach, target.y + std::sin(angle) * reach, target.z + reach * 0.1f};
            return true;
        }
        return false;
    }
    const std::size_t colon = spec.find(':');
    const std::string kindName = spec.substr(0, colon);
    if (kindName == "creature" || kindName == "placeable" || kindName == "door") {
        const EntityKind kind = kindName == "creature" ? EntityKind::Creature
            : kindName == "placeable" ? EntityKind::Placeable : EntityKind::Door;
        std::vector<const Entity*> found;
        for (const Entity& entity : scene.entities) {
            if (entity.kind == kind && !entity.dead) {
                found.push_back(&entity);
            }
        }
        // Nearest the entry first, so an index is stable and close to where the shot starts.
        std::stable_sort(found.begin(), found.end(), [&](const Entity* a, const Entity* b) {
            return length(a->position - scene.entry) < length(b->position - scene.entry);
        });
        const std::size_t index = colon == std::string::npos
            ? 0u : static_cast<std::size_t>(std::strtoul(spec.c_str() + colon + 1u, nullptr, 10));
        if (index >= found.size()) {
            return false;
        }
        const Entity& entity = *found[index];
        const float reach = kind == EntityKind::Creature ? 2.2f : 3.5f;
        target = {entity.position.x, entity.position.y, entity.position.z + 1.1f};
        // The first of eight directions, starting in front of the object, with a clear walkmesh.
        const auto clear = [&](Vec3 from) {
            const Vec3 ray = from - target;
            for (const CollisionFacet& facet : scene.collision) {
                const Vec3& a = scene.collisionVertices[facet.a];
                const Vec3 e1 = scene.collisionVertices[facet.b] - a;
                const Vec3 e2 = scene.collisionVertices[facet.c] - a;
                const Vec3 p = cross(ray, e2);
                const float det = dot(e1, p);
                if (std::abs(det) < 1e-6f) {
                    continue;
                }
                const Vec3 t = target - a;
                const float u = dot(t, p) / det;
                const Vec3 q = cross(t, e1);
                const float v = dot(ray, q) / det;
                const float hit = dot(e2, q) / det;
                if (u >= 0.0f && v >= 0.0f && u + v <= 1.0f && hit > 0.02f && hit < 1.15f) {
                    return false;
                }
            }
            return true;
        };
        eye = {entity.position.x + std::cos(entity.bearing) * reach, entity.position.y + std::sin(entity.bearing) * reach,
               entity.position.z + 1.5f};
        for (int turn = 0; turn < 8; ++turn) {
            const float angle = entity.bearing + static_cast<float>(turn) * 0.7853982f;
            const Vec3 from{entity.position.x + std::cos(angle) * reach, entity.position.y + std::sin(angle) * reach,
                            entity.position.z + 1.5f};
            if (clear(from)) {
                eye = from;
                break;
            }
        }
        return true;
    }
    float v[7] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, fov};
    const char* cursor = spec.c_str();
    int read = 0;
    for (; read < 7 && *cursor != '\0'; ++read) {
        char* end = nullptr;
        v[read] = std::strtof(cursor, &end);
        if (end == cursor) {
            return false;
        }
        cursor = *end == ',' ? end + 1 : end;
    }
    if (read < 6) {
        return false;
    }
    eye = {v[0], v[1], v[2]};
    target = {v[3], v[4], v[5]};
    fov = v[6];
    return true;
}

void updateVisibleRooms(Scene& scene, Vec3 eye)
{
    if (scene.visibility.empty() || scene.rooms.empty()) {
        scene.visibleRooms.clear();
        return;
    }
    // The room is the one whose walkmesh lies nearest below the camera.
    float bestHeight = 1e9f;
    std::int64_t best = -1;
    for (const CollisionFacet& facet : scene.collision) {
        const Vec3& a = scene.collisionVertices[facet.a];
        const Vec3& b = scene.collisionVertices[facet.b];
        const Vec3& c = scene.collisionVertices[facet.c];
        const float minX = std::min({a.x, b.x, c.x});
        const float maxX = std::max({a.x, b.x, c.x});
        const float minY = std::min({a.y, b.y, c.y});
        const float maxY = std::max({a.y, b.y, c.y});
        if (eye.x < minX || eye.x > maxX || eye.y < minY || eye.y > maxY) {
            continue;
        }
        const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::abs(d) < 1e-6f) {
            continue;
        }
        const float u = ((b.y - c.y) * (eye.x - c.x) + (c.x - b.x) * (eye.y - c.y)) / d;
        const float v = ((c.y - a.y) * (eye.x - c.x) + (a.x - c.x) * (eye.y - c.y)) / d;
        if (u < 0.0f || v < 0.0f || u + v > 1.0f) {
            continue;
        }
        const float z = a.z * u + b.z * v + c.z * (1.0f - u - v);
        const float height = eye.z - z;
        if (height >= -0.5f && height < bestHeight) {
            bestHeight = height;
            best = facet.room;
        }
    }
    if (best < 0 || static_cast<std::size_t>(best) >= scene.visibility.size()) {
        scene.visibleRooms.clear(); // Outside every walkmesh: nothing to cull by.
        return;
    }
    scene.visibleRooms.assign(scene.rooms.size(), 0u);
    scene.visibleRooms[static_cast<std::size_t>(best)] = 1u;
    for (const std::uint32_t room : scene.visibility[static_cast<std::size_t>(best)]) {
        if (room < scene.visibleRooms.size()) {
            scene.visibleRooms[room] = 1u;
        }
    }
    // A sky room (Taris, m02a?_sky in the LYT) stands behind every other room.
    for (std::size_t room = 0; room < scene.rooms.size(); ++room) {
        const std::string name = lowerText(scene.rooms[room].room);
        if (name.size() > 4u && name.ends_with("_sky")) {
            scene.visibleRooms[room] = 1u;
        }
    }
}

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// Retail's table cosine and sine (004aadf0, 004aae30): 32 steps, the index rounded.
float tableCosAt(int index)
{
    // The quarter of retail's table at 00743fe8; its zeros are exact.
    static constexpr float kQuarter[9] = {1.0f,        0.980785251f, 0.923879504f, 0.831469595f, 0.707106769f,
                                          0.555570245f, 0.382683426f, 0.195090324f, 0.0f};
    const int i = index & 31;
    return i <= 8 ? kQuarter[i] : i <= 16 ? -kQuarter[16 - i] : i <= 24 ? -kQuarter[i - 16] : kQuarter[32 - i];
}

float tableCos(float radians)
{
    return tableCosAt(static_cast<int>(std::lrintf(radians * 5.09295798f)));
}

float tableSin(float radians)
{
    return tableCosAt(static_cast<int>(std::lrintf(radians * 5.09295798f)) - 8);
}

int proceduralRandom(ProceduralTexture& state)
{
    // The CRT's rand() sequence (006fc447), seeded per texture so that a run repeats.
    state.random = state.random * 214013u + 2531011u;
    return static_cast<int>((state.random >> 16u) & 0x7fffu);
}

// Bilinear with wrap (retail 004608a0).
float gridSample(const std::vector<float>& grid, float u, float v, int width, int height)
{
    const int u0 = static_cast<int>(std::lrintf(u - 0.5f));
    const int v0 = static_cast<int>(std::lrintf(v - 0.5f));
    const float fu = u - static_cast<float>(u0);
    const float fv = v - static_cast<float>(v0);
    const int u1 = (u0 + 1) % width;
    const int v1 = (v0 + 1) % height;
    const auto at = [&](int x, int y) { return grid[static_cast<std::size_t>(y) * width + x]; };
    return ((1.0f - fu) * at(u0, v0) + fu * at(u1, v0)) * (1.0f - fv)
        + ((1.0f - fu) * at(u0, v1) + fu * at(u1, v1)) * fv;
}

} // namespace

ProceduralTexture makeProceduralTexture(std::size_t index, const Texture& texture)
{
    ProceduralTexture state;
    state.name = texture.name;
    state.texture = index;
    state.random = 0x1234u + static_cast<std::uint32_t>(index);
    const bool water = texture.txi.procedure == TxiInfo::Procedure::Water;
    const int fallback = water ? 31 : 15;
    state.gridWidth = texture.txi.gridWidth > 0 ? texture.txi.gridWidth : fallback;
    state.gridHeight = texture.txi.gridHeight > 0 ? texture.txi.gridHeight : fallback;
    const std::size_t cells = static_cast<std::size_t>(state.gridWidth) * state.gridHeight;
    state.original = texture.image.rgba;
    state.heights.assign(static_cast<std::size_t>(texture.image.width) * texture.image.height, 127u);
    state.next.assign(cells, 0.0f);
    state.current.assign(cells, 0.0f);
    state.previous.assign(cells, 0.0f);
    if (water) {
        // 0045ff40: all heights 127, all velocities 0.
        state.next.assign(cells, 127.0f);
        return state;
    }
    // 00460560: a ring around the centre, then two wave steps.
    for (int y = 0; y < state.gridHeight; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(state.gridHeight) * 2.0f - 1.0f;
        for (int x = 0; x < state.gridWidth; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(state.gridWidth) * 2.0f - 1.0f;
            state.current[static_cast<std::size_t>(y) * state.gridWidth + x] =
                std::sin((1.0f - std::sqrt(u * u + v * v)) * 3.14159265358979f) * 127.0f;
        }
    }
    proceduralArturoWave(state, true);
    proceduralArturoWave(state, true);
    return state;
}

// 00460690, the wave equation on the grid. The damping constant at 00741c04 is zero.
void proceduralArturoWave(ProceduralTexture& state, bool poke)
{
    const int width = state.gridWidth;
    const int height = state.gridHeight;
    const int pokeX = proceduralRandom(state) % width;
    const int pokeY = proceduralRandom(state) % height;
    for (int y = 0; y < height; ++y) {
        const int up = (y + height - 1) % height;
        const int down = (y + 1) % height;
        for (int x = 0; x < width; ++x) {
            const int left = (x + width - 1) % width;
            const int right = (x + 1) % width;
            const auto at = [&](int cx, int cy) { return state.current[static_cast<std::size_t>(cy) * width + cx]; };
            const std::size_t cell = static_cast<std::size_t>(y) * width + x;
            float value = (at(left, y) + at(right, y) + at(x, up) + at(x, down)) * 0.5f - state.previous[cell];
            if (poke && x == pokeX && y == pokeY) {
                value += static_cast<float>((proceduralRandom(state) & 0xff) - 128);
            }
            state.next[cell] = std::clamp(value + 127.0f, 0.0f, 255.0f) - 127.0f;
        }
    }
    state.previous = state.current;
    state.current = state.next;
}

// 00461820, the sum over the eight neighbours with a stiffness that drifts in time, one velocity
// Verlet step, a random kick to a velocity, and the height limit 40..215.
void proceduralWaterWave(ProceduralTexture& state, float step, bool poke)
{
    constexpr float kDegree = 0.0174532924f;
    state.angleA += step * 57.2957764f * kDegree; // +0x54
    if (state.angleA > kTwoPi) {
        state.angleA -= kTwoPi;
    }
    state.angleB += step * 2.86478901f * kDegree; // +0x58
    if (state.angleB > kTwoPi) {
        state.angleB -= kTwoPi;
    }
    const float swing = tableCos(state.angleA) * 0.25f + 0.2f;
    const float drift = tableCos(state.angleB);
    const auto corner = [&](float angle) { return tableCos(drift - angle) * swing + 0.5f; };
    const float stiffness[3][3] = {
        {corner(0.785398185f), 1.0f, corner(2.3561945f)},
        {1.0f, 0.0f, 1.0f},
        {corner(5.497787f), 1.0f, corner(3.92699075f)},
    };
    const int size = state.gridWidth;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const std::size_t cell = static_cast<std::size_t>(y) * size + x;
            const float centre = state.next[cell] - 127.0f;
            float force = 0.0f;
            for (int i = 0; i < 3; ++i) {
                const int row = (y + i + size - 1) % size;
                for (int j = 0; j < 3; ++j) {
                    if (i == 1 && j == 1) {
                        continue; // its weight is zero
                    }
                    const int column = (x + j + size - 1) % size;
                    const float other = state.next[static_cast<std::size_t>(row) * size + column] - 127.0f;
                    force += (other - centre) * stiffness[i][j];
                }
            }
            const float velocity = state.previous[cell];
            const float accelerated = (force - 0.025f * velocity) * step;
            state.current[cell] = (accelerated * 0.5f + velocity) * step + centre;
            state.previous[cell] = velocity + accelerated;
        }
    }
    if (poke) {
        const int pokeX = proceduralRandom(state) % size;
        const int pokeY = proceduralRandom(state) % size;
        state.previous[static_cast<std::size_t>(pokeX) * size + pokeY] +=
            static_cast<float>(proceduralRandom(state) % 127);
    }
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const std::size_t cell = static_cast<std::size_t>(y) * size + x;
            float value = state.current[cell] + 127.0f;
            if (value < 40.0f) {
                value = 40.0f;
                state.previous[cell] = 0.0f;
            }
            if (value > 215.0f) {
                value = 215.0f;
                state.previous[cell] = 0.0f;
            }
            state.next[cell] = value;
        }
    }
}

// 0045ea70: half of a texel plus an eighth of each of its four neighbours, each shifted down
// (shift table 0078e475), wrapping.
void proceduralSmoothHeights(std::vector<std::uint8_t>& heights, int width, int height)
{
    std::vector<std::uint8_t> out(heights.size());
    for (int y = 0; y < height; ++y) {
        const int up = (y + height - 1) % height;
        const int down = (y + 1) % height;
        for (int x = 0; x < width; ++x) {
            const int left = (x + width - 1) % width;
            const int right = (x + 1) % width;
            const auto at = [&](int cx, int cy) {
                return static_cast<unsigned>(heights[static_cast<std::size_t>(cy) * width + cx]);
            };
            out[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>(
                (at(x, up) >> 3u) + (at(left, y) >> 3u) + (at(x, y) >> 1u) + (at(right, y) >> 3u) + (at(x, down) >> 3u));
        }
    }
    heights = std::move(out);
}

// 00460d70: each texel is the original one at an offset chosen by the height at this texel.
void proceduralDistort(const std::vector<std::uint8_t>& heights, const std::vector<std::uint8_t>& original,
                       int width, int height, bool byAngle, float amplitude, std::vector<std::uint8_t>& out)
{
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float value = static_cast<float>(heights[static_cast<std::size_t>(y) * width + x]);
            int dx = 0;
            int dy = 0;
            if (byAngle) {
                const float angle = value * 0.0245436933f;
                dy = static_cast<int>(std::lrintf(tableCos(angle) * amplitude - 0.5f));
                dx = static_cast<int>(std::lrintf(tableSin(angle) * amplitude - 0.5f));
            } else {
                dx = static_cast<int>(std::lrintf((value - 127.0f) * 0.0078125f * amplitude - 0.5f));
                dy = dx;
            }
            const std::size_t from =
                static_cast<std::size_t>(((y + dy + height) & (height - 1)) * width + ((x + dx + width) & (width - 1)));
            const std::size_t to = static_cast<std::size_t>(y) * width + x;
            for (std::size_t channel = 0; channel < 4u; ++channel) {
                out[to * 4u + channel] = original[from * 4u + channel];
            }
        }
    }
}

bool stepProceduralTexture(ProceduralTexture& state, Texture& texture, float dt)
{
    // Engine choice: retail steps with its frame time, once a frame, so its animation speed moves
    // with the frame rate. Here the steps are those of retail at 60 frames a second, at any rate:
    // arturo every second frame (0.03 s gate at 00462520), water every second frame (00461847).
    constexpr float kTick = 1.0f / 30.0f;
    constexpr float kFrame = 1.0f / 60.0f;
    state.accumulator += dt;
    if (state.accumulator < kTick) {
        return false;
    }
    state.accumulator = std::fmod(state.accumulator, kTick);
    const int width = texture.image.width;
    const int height = texture.image.height;
    if (texture.txi.procedure == TxiInfo::Procedure::Water) {
        proceduralWaterWave(state, std::min(kFrame, 0.04f) * texture.txi.speed, true);
        const float stepU = static_cast<float>(state.gridWidth) / static_cast<float>(width);
        const float stepV = static_cast<float>(state.gridHeight) / static_cast<float>(height);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                state.heights[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>(static_cast<int>(
                    gridSample(state.next, static_cast<float>(x) * stepU, static_cast<float>(y) * stepV,
                               state.gridWidth, state.gridHeight)));
            }
        }
        proceduralSmoothHeights(state.heights, width, height);
    } else {
        // 00462520: three phases, half the rows, the other half, then the upload.
        state.phase = (state.phase + 1) % 3;
        if (state.phase == 0) {
            state.blend += kFrame * texture.txi.speed * 1.5f;
            if (state.blend > 1.0f) {
                proceduralArturoWave(state, true);
                state.blend -= std::floor(state.blend);
            }
        }
        if (state.phase < 2) {
            const int half = height / 2;
            const int first = half * state.phase;
            const int last = state.phase == 1 ? height : half * (state.phase + 1);
            const float stepU = static_cast<float>(state.gridWidth) / static_cast<float>(width);
            const float stepV = static_cast<float>(state.gridHeight) / static_cast<float>(height);
            for (int y = first; y < last; ++y) {
                for (int x = 0; x < width; ++x) {
                    const float u = static_cast<float>(x) * stepU;
                    const float v = static_cast<float>(y) * stepV;
                    const float older = gridSample(state.previous, u, v, state.gridWidth, state.gridHeight);
                    const float newer = gridSample(state.current, u, v, state.gridWidth, state.gridHeight);
                    state.heights[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>(
                        std::lrintf((1.0f - state.blend) * older + state.blend * newer + 127.0f) & 0xff);
                }
            }
            return false;
        }
    }
    proceduralDistort(state.heights, state.original, width, height, texture.txi.distortAngle,
                      texture.txi.distortAmplitude, texture.image.rgba);
    ++texture.revision;
    return true;
}

void updateProceduralTextures(Scene& scene, float dt)
{
    // A scene reused for another module replaced its textures.
    if (scene.proceduralsScanned > scene.textures.size()
        || std::any_of(scene.procedurals.begin(), scene.procedurals.end(), [&](const ProceduralTexture& state) {
               return state.texture >= scene.textures.size() || scene.textures[state.texture].name != state.name;
           })) {
        scene.procedurals.clear();
        scene.proceduralsScanned = 0;
    }
    for (; scene.proceduralsScanned < scene.textures.size(); ++scene.proceduralsScanned) {
        const Texture& texture = scene.textures[scene.proceduralsScanned];
        const bool animated = texture.txi.procedure == TxiInfo::Procedure::Arturo
            || texture.txi.procedure == TxiInfo::Procedure::Water;
        // Retail's other path, without distort (00461060), turns the heights into a colour through
        // channelscale and channeltranslate. No drawn texture ships with it.
        if (animated && texture.txi.distort && texture.image.width > 0 && texture.image.height > 0
            && (texture.image.width & (texture.image.width - 1)) == 0
            && (texture.image.height & (texture.image.height - 1)) == 0) {
            scene.procedurals.push_back(makeProceduralTexture(scene.proceduralsScanned, texture));
        }
    }
    for (ProceduralTexture& state : scene.procedurals) {
        stepProceduralTexture(state, scene.textures[state.texture], dt);
    }
}

} // namespace ke::kotor
