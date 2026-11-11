// --family-test textureaudit: every texture of the install and every material that names one, with
// the draw state the engine's own functions choose for it. Nothing here restates a rule: a mesh
// batch comes from makeMaterialBatch or from loadModule, an emitter's from makeEmitterBatch.
//
// KE_TEX_AUDIT_DIR names the output folder; each row is engine data for an offline comparison.
#include "game/TextureAudit.h"

#include "core/Log.h"
#include "game/Effects.h"
#include "game/GameSession.h"
#include "kotor/Formats.h"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ke {

namespace {

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string oneLine(const std::string& text)
{
    std::string out;
    for (const char ch : text) {
        if (ch == '\r') {
            continue;
        }
        out += ch == '\n' || ch == '\t' ? ';' : ch;
    }
    return out;
}

const char* blendName(kotor::BlendMode blend)
{
    switch (blend) {
    case kotor::BlendMode::Additive:
        return "additive";
    case kotor::BlendMode::Alpha:
        return "alpha";
    default:
        return "opaque";
    }
}

class Audit {
public:
    Audit(kotor::Install& install, const std::filesystem::path& dir)
        : install_(install), textures_(dir / "tex_engine.tsv"), uses_(dir / "use_engine.tsv"), dir_(dir)
    {
        textures_ << "name\ttype\tsource\tw\th\tencoding\tmips\tfile_bytes\thas_alpha\ta_min\ta_max\ta0_frac"
                     "\tapartial_frac\tblack_frac\tblack_opaque_frac\tmean_r\tmean_g\tmean_b\thash\tclamp\tcube"
                     "\tblending\tproc\tnumx\tnumy\tfps\tdecal\tenv\tbumpy\tbump\twateralpha\ttxi\terror\n";
        uses_ << "kind\towner\ttexture\tlightmap\tblend\tdepth_write\talpha_ref\ttwo_sided\tenv_mask\tenv\tcycle_x"
                 "\tcycle_y\tcycle_fps\tscroll_u\tscroll_v\thint\tfades\tbeaming\tshadow\tunlit\tno_fog\tself_r"
                 "\tself_g\tself_b\tnode_alpha\temit_update\temit_render\temit_blend\tcount\n";
        scene_.textures.push_back({"__default", {}});
        scene_.textureIndex["__default"] = 0;
    }

    void addTexture(const std::string& rawName)
    {
        const std::string name = lower(rawName);
        if (!seenTextures_.insert(name).second) {
            return;
        }
        std::uint32_t type = 0;
        std::string source;
        std::string error;
        kotor::Texture texture;
        std::vector<std::uint8_t> data;
        try {
            auto bytes = install_.findTexture(name, &type, &source);
            if (!bytes.has_value()) {
                error = "unresolved";
            } else {
                data = std::move(*bytes);
            }
        } catch (const std::exception& exception) {
            error = exception.what();
        }
        const std::uint32_t index = error.empty() ? kotor::ensureOptionalTexture(scene_, install_, name) : kotor::kNoTexture;
        if (error.empty() && index == kotor::kNoTexture) {
            error = "undecodable";
        }
        const kotor::Texture empty;
        const kotor::Texture& t = index < scene_.textures.size() ? scene_.textures[index] : empty;
        const std::size_t pixels = t.image.rgba.size() / 4u;
        int aMin = 255;
        int aMax = 0;
        std::size_t a0 = 0;
        std::size_t partial = 0;
        std::size_t black = 0;
        std::size_t blackOpaque = 0;
        std::uint64_t sums[3] = {0, 0, 0};
        // A position-weighted sum, cheap to reproduce in an independent reader.
        std::uint64_t hash = 0;
        for (std::size_t i = 0; i < pixels; ++i) {
            const std::uint8_t* p = t.image.rgba.data() + i * 4u;
            aMin = std::min<int>(aMin, p[3]);
            aMax = std::max<int>(aMax, p[3]);
            a0 += p[3] == 0u ? 1u : 0u;
            partial += p[3] != 0u && p[3] != 255u ? 1u : 0u;
            if (std::max({p[0], p[1], p[2]}) < 24u) {
                ++black;
                blackOpaque += p[3] >= 250u ? 1u : 0u;
            }
            for (int c = 0; c < 3; ++c) {
                sums[c] += p[c];
            }
            for (std::size_t c = 0; c < 4u; ++c) {
                hash += ((i * 4u + c) % 65521u + 1u) * p[c];
            }
        }
        const double count = pixels != 0u ? static_cast<double>(pixels) : 1.0;
        const int encoding = type == kotor::resType(kotor::ResType::Tpc) && data.size() > 13u ? data[12] : -1;
        const int mips = type == kotor::resType(kotor::ResType::Tpc) && data.size() > 13u ? data[13] : -1;
        std::string txiText;
        if (type == kotor::resType(kotor::ResType::Tpc)) {
            txiText = kotor::tpcEmbeddedTxi(data);
        }
        if (txiText.empty()) {
            if (const auto separate = install_.find(name, 2022); separate.has_value()) {
                txiText.assign(separate->begin(), separate->end());
            }
        }
        const char* blending = t.txi.blending == kotor::TxiInfo::Blend::Additive ? "additive"
            : t.txi.blending == kotor::TxiInfo::Blend::PunchThrough              ? "punchthrough"
                                                                                 : "default";
        const char* procedure = t.txi.procedure == kotor::TxiInfo::Procedure::Cycle ? "cycle"
            : t.txi.procedure == kotor::TxiInfo::Procedure::Arturo                  ? "arturo"
            : t.txi.procedure == kotor::TxiInfo::Procedure::Water                   ? "water"
            : t.txi.procedure == kotor::TxiInfo::Procedure::Other                   ? "other"
                                                                                    : "";
        textures_ << name << '\t' << type << '\t' << source << '\t' << t.image.width << '\t' << t.image.height << '\t'
                  << encoding << '\t' << mips << '\t' << data.size() << '\t' << (t.hasAlpha ? 1 : 0) << '\t'
                  << (pixels != 0u ? aMin : 0) << '\t' << aMax << '\t' << static_cast<double>(a0) / count << '\t'
                  << static_cast<double>(partial) / count << '\t' << static_cast<double>(black) / count << '\t'
                  << static_cast<double>(blackOpaque) / count << '\t' << static_cast<double>(sums[0]) / count << '\t'
                  << static_cast<double>(sums[1]) / count << '\t' << static_cast<double>(sums[2]) / count << '\t'
                  << std::hex << hash << std::dec << '\t' << static_cast<int>(t.clamp) << '\t' << (t.txi.cube ? 1 : 0)
                  << '\t' << blending << '\t' << procedure << '\t' << t.txi.numX << '\t' << t.txi.numY << '\t' << t.txi.fps
                  << '\t' << (t.txi.decal ? 1 : 0) << '\t' << t.txi.envMap << '\t' << t.txi.bumpyShiny << '\t'
                  << t.txi.bumpMap << '\t' << t.txi.waterAlpha << '\t' << oneLine(txiText) << '\t' << error << '\n';
        // Metadata stays for makeMaterialBatch; the pixels are not needed again.
        if (index < scene_.textures.size()) {
            std::vector<std::uint8_t>().swap(scene_.textures[index].image.rgba);
        }
    }

    void addBatch(const char* kind, const std::string& owner, const std::vector<kotor::Texture>& textures,
                  const kotor::TextureBatch& batch, const kotor::MdlMeshTriangle* lead, std::size_t triangles,
                  const kotor::MdlEmitter* emitter = nullptr)
    {
        const auto nameOf = [&](std::uint32_t index) {
            return index < textures.size() ? textures[index].name : std::string{};
        };
        std::ostringstream row;
        row << kind << '\t' << owner << '\t' << nameOf(batch.texture) << '\t' << nameOf(batch.lightmap) << '\t'
            << blendName(batch.blend) << '\t' << (batch.writeDepth ? 1 : 0) << '\t' << batch.alphaRef << '\t'
            << (batch.twoSided ? 1 : 0) << '\t' << (batch.envMask ? 1 : 0) << '\t' << nameOf(batch.envmap) << '\t'
            << batch.cycleX << '\t' << batch.cycleY << '\t' << batch.cycleFps << '\t' << batch.scrollU << '\t'
            << batch.scrollV << '\t' << batch.transparencyHint << '\t';
        if (lead != nullptr && lead->material != nullptr) {
            row << (lead->material->fades ? 1 : 0) << '\t' << (lead->material->beaming ? 1 : 0) << '\t'
                << (lead->material->shadow ? 1 : 0) << '\t';
        } else {
            row << "\t\t\t";
        }
        row << (batch.unlit ? 1 : 0) << '\t' << (batch.noFog ? 1 : 0) << '\t';
        if (lead != nullptr) {
            row << lead->selfIllum.x << '\t' << lead->selfIllum.y << '\t' << lead->selfIllum.z << '\t' << lead->alpha;
        } else {
            row << "\t\t\t";
        }
        row << '\t';
        if (emitter != nullptr) {
            row << emitter->update << '\t' << emitter->render << '\t' << emitter->blend;
        } else {
            row << "\t\t";
        }
        const std::string line = row.str();
        lines_[line] += triangles;
    }

    void addModel(const std::string& rawName)
    {
        const std::string name = lower(rawName);
        if (!seenModels_.insert(name).second) {
            return;
        }
        auto mdl = install_.find(name, kotor::resType(kotor::ResType::Mdl));
        auto mdx = install_.find(name, kotor::resType(kotor::ResType::Mdx));
        if (!mdl.has_value() || !mdx.has_value()) {
            modelErrors_ << name << "\tno mdl or mdx\n";
            return;
        }
        try {
            const kotor::Mdl model = kotor::Mdl::parse(std::move(*mdl), std::move(*mdx), name + ".mdl");
            std::map<std::string, std::vector<kotor::MdlMeshTriangle>> groups;
            for (kotor::MdlMeshTriangle& triangle : model.meshTriangles()) {
                groups[kotor::materialKey(triangle)].push_back(std::move(triangle));
            }
            for (const auto& [key, triangles] : groups) {
                const kotor::MdlMeshTriangle& lead = triangles.front();
                addTexture(lead.texture);
                if (lead.material != nullptr && !lead.material->lightmap.empty()) {
                    addTexture(lead.material->lightmap);
                }
                const kotor::TextureBatch batch = kotor::makeMaterialBatch(scene_, install_, lead);
                addEnvironment(batch);
                addBatch("model", name, scene_.textures, batch, &lead, triangles.size());
            }
            for (const kotor::Mdl::EmitterNode& node : model.emitters()) {
                if (node.header.texture.empty() || lower(node.header.texture) == "null") {
                    continue;
                }
                addTexture(node.header.texture);
                const std::uint32_t texture = kotor::ensureOptionalTexture(scene_, install_, node.header.texture);
                if (texture != kotor::kNoTexture) {
                    const kotor::TextureBatch batch = kotor::makeEmitterBatch(scene_, node.header, texture);
                    addBatch("emitter", name + ":" + node.name, scene_.textures, batch, nullptr, 1u, &node.header);
                } else {
                    missing_[lower(node.header.texture)].insert("emitter:" + name);
                }
            }
            for (const kotor::MdlLight& light : model.lights()) {
                for (const kotor::MdlLight::Flare& flare : light.flares) {
                    if (!flare.texture.empty()) {
                        addTexture(flare.texture);
                        flares_ << name << '\t' << lower(flare.texture) << '\t' << flare.size << '\t' << flare.position
                                << '\t' << flare.colourShift.x << '\t' << flare.colourShift.y << '\t'
                                << flare.colourShift.z << '\n';
                    }
                }
            }
        } catch (const std::exception& exception) {
            modelErrors_ << name << '\t' << exception.what() << '\n';
        }
        // Everything this model used is recorded; only the last model's pixels could remain.
        for (kotor::Texture& texture : scene_.textures) {
            if (!texture.image.rgba.empty()) {
                std::vector<std::uint8_t>().swap(texture.image.rgba);
            }
        }
    }

    void addModule(const std::string& stem)
    {
        const kotor::Scene scene = kotor::loadModule(install_, stem);
        ++modules_;
        scene_.defaultEnvMap = scene.defaultEnvMap;
        scene_.alphaTest = scene.alphaTest;
        areas_ << stem << '\t' << scene.defaultEnvMap << '\t' << scene.alphaTest << '\n';
        for (const kotor::TextureBatch& batch : scene.batches) {
            addBatch("room", stem, scene.textures, batch, nullptr, batch.indexCount / 3u);
        }
        for (const kotor::EntityMesh& mesh : scene.entityMeshes) {
            const std::string owner = stem + ":" + (mesh.model ? mesh.model->name() : std::string("?"));
            for (const kotor::TextureBatch& batch : mesh.batches) {
                addBatch(mesh.fx ? "entity_fx" : "entity", owner, scene.textures, batch, nullptr, batch.indexCount / 3u);
            }
        }
        for (const kotor::EmitterInstance& emitter : scene.emitters) {
            const kotor::TextureBatch batch = kotor::makeEmitterBatch(scene, emitter.header, emitter.texture);
            addBatch("scene_emitter", stem + ":" + emitter.node, scene.textures, batch, nullptr, 1u, &emitter.header);
        }
        for (const auto& [name, count] : scene.missingTextures) {
            missing_[name].insert(stem);
        }
        for (const kotor::Texture& texture : scene.textures) {
            sceneTextures_[texture.name].insert(stem);
        }
        for (const std::string& name : install_.resourceNames(kotor::resType(kotor::ResType::Tpc))) {
            addTexture(name);
        }
        for (const std::string& name : install_.resourceNames(kotor::resType(kotor::ResType::Tga))) {
            addTexture(name);
        }
        for (const std::string& name : install_.resourceNames(kotor::resType(kotor::ResType::Mdl))) {
            addModel(name);
        }
    }

    int finish()
    {
        for (const auto& [line, count] : lines_) {
            uses_ << line << '\t' << count << '\n';
        }
        std::ofstream missing(dir_ / "missing_engine.tsv");
        for (const auto& [name, owners] : missing_) {
            missing << name << '\t' << owners.size() << '\t' << *owners.begin() << '\n';
        }
        std::ofstream inScene(dir_ / "scene_textures.tsv");
        for (const auto& [name, owners] : sceneTextures_) {
            inScene << name << '\t' << owners.size() << '\t' << *owners.begin() << '\n';
        }
        std::ofstream(dir_ / "model_errors.tsv") << modelErrors_.str();
        std::ofstream(dir_ / "flares_engine.tsv") << flares_.str();
        std::ofstream(dir_ / "areas_engine.tsv") << areas_.str();
        logInfo(concat("textureaudit: modules ", modules_, " textures ", seenTextures_.size(), " models ",
                       seenModels_.size(), " material rows ", lines_.size()));
        return modules_ != 0u ? 0 : 1;
    }

private:
    void addEnvironment(const kotor::TextureBatch& batch)
    {
        if (batch.envmap < scene_.textures.size()) {
            addTexture(scene_.textures[batch.envmap].name);
        }
    }

    kotor::Install& install_;
    std::ofstream textures_;
    std::ofstream uses_;
    std::filesystem::path dir_;
    kotor::Scene scene_;
    std::set<std::string> seenTextures_;
    std::set<std::string> seenModels_;
    std::map<std::string, std::size_t> lines_;
    std::map<std::string, std::set<std::string>> missing_;
    std::map<std::string, std::set<std::string>> sceneTextures_;
    std::ostringstream modelErrors_;
    std::ostringstream flares_;
    std::ostringstream areas_;
    std::size_t modules_ = 0;
};

} // namespace

int runMaterialFamilyTest(GameSession& game)
{
    kotor::Install& install = game.install();
    bool ok = true;
    const auto check = [&](bool condition, const std::string& label) {
        logInfo(concat("materials: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    kotor::Scene scene;
    scene.textures.push_back({"__default", {}});
    scene.textureIndex["__default"] = 0;
    scene.alphaTest = 0.2f;
    {
        const kotor::Vec3 lerped = kotor::environmentComposite({0.8f, 0.8f, 0.8f}, {0.4f, 0.4f, 0.4f}, 0.25f, false);
        const kotor::Vec3 summed = kotor::environmentComposite({0.8f, 0.8f, 0.8f}, {0.4f, 0.4f, 0.4f}, 0.25f, true);
        check(std::abs(lerped.x - 0.5f) < 1e-5f, "env_without_lightmap_lerps_830248");
        const kotor::Vec3 litReflection =
            kotor::environmentComposite({0.8f, 0.8f, 0.8f}, {0.4f, 0.4f, 0.4f}, 0.25f, false, {0.5f, 0.5f, 0.5f});
        check(std::abs(litReflection.x - 0.35f) < 1e-5f, "env_without_lightmap_lit_reflection_0046ff70");
        check(std::abs(summed.x - 1.1f) < 1e-5f, "env_with_lightmap_adds_830258");
        // Program 83025c: a flat map reflects like the smooth normal; a tilted one moves the reflection.
        const kotor::Vec3 eye = kotor::normalize(kotor::Vec3{0.3f, 0.0f, 1.0f});
        const kotor::Vec3 flat = kotor::bumpReflection(eye, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 1});
        check(std::abs(flat.x + 0.2873f) < 1e-3f && std::abs(flat.z - 0.9578f) < 1e-3f, "bump_flat_map_reflects_smooth_83025c");
        const kotor::Vec3 tilted = kotor::bumpReflection(eye, {1, 0, 0}, {0, 1, 0}, {0, 0, 1},
                                                         kotor::normalize(kotor::Vec3{0.2f, 0.0f, 1.0f}));
        check(tilted.x > flat.x + 0.2f, "bump_tilted_map_moves_reflection_83025c");
        // A ramp in x gives a normal leaning against it, in each atlas cell on its own.
        kotor::Image height;
        height.width = 8;
        height.height = 4;
        height.rgba.resize(8u * 4u * 4u);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 8; ++x) {
                const std::uint8_t h = static_cast<std::uint8_t>((x % 4) * 60);
                std::uint8_t* out = &height.rgba[static_cast<std::size_t>(y * 8 + x) * 4u];
                out[0] = out[1] = out[2] = h;
                out[3] = 255;
            }
        }
        kotor::heightToNormal(height, 2.0f, 2, 1);
        // Retail 0041fec0: G carries -dx and R carries dy, each trunc(v * 127 + 128).
        check(height.rgba[static_cast<std::size_t>(1) * 4u + 1u] == 182, "height_to_normal_leans_against_the_ramp_0041fec0");
        check(height.rgba[static_cast<std::size_t>(1) * 4u] == 128, "height_to_normal_flat_axis_stays_level_0041fec0");
        check(height.rgba[static_cast<std::size_t>(5) * 4u + 1u] == height.rgba[static_cast<std::size_t>(1) * 4u + 1u], "height_to_normal_each_cell_on_its_own");
        kotor::TextureBatch cycling;
        cycling.bumpCycleX = 8;
        cycling.bumpCycleY = 8;
        cycling.bumpCycleFps = 30.0f;
        const kotor::BumpCell cell = kotor::bumpCellAt(cycling, 1.0f);
        check(cell.x == 6 && cell.y == 3, "bump_cycle_cell_from_its_own_txi");
    }
    {
        // Specular defaults of a bump map, retail ctor 00423150, and the parts of its TXI that change them.
        const kotor::TxiInfo plainBump = kotor::parseTxi("isbumpmap 1\nbumpmapscaling 1\n");
        check(plainBump.isSpecularBump && plainBump.specular == 1.0f, "bump_specular_defaults_white_00423150");
        const kotor::TxiInfo shipBump = kotor::parseTxi("isbumpmap 1\nspecularcolor .5 .5 .5\n");
        check(shipBump.specular == 0.5f, "bump_specular_from_txi");
        check(!kotor::parseTxi("isspecularbumpmap 0\n").isSpecularBump, "bump_specular_can_be_off");
        // Animated textures, from retail 00460690, 00461820, 0045ea70 and 00460d70.
        kotor::Texture texture;
        texture.name = "probe";
        texture.txi = kotor::parseTxi("proceduretype arturo\ndistort 1\narturowidth 16\narturoheight 16\n"
                                      "distortionamplitude 2\nspeed 6\n");
        check(texture.txi.procedure == kotor::TxiInfo::Procedure::Arturo && texture.txi.distort
                  && texture.txi.gridWidth == 16 && texture.txi.distortAmplitude == 2.0f && texture.txi.speed == 6.0f,
              "procedure_txi_read");
        texture.image.width = 8;
        texture.image.height = 8;
        texture.image.rgba.resize(8u * 8u * 4u);
        for (std::size_t i = 0; i < texture.image.rgba.size(); ++i) {
            texture.image.rgba[i] = static_cast<std::uint8_t>(i * 13u);
        }
        const std::vector<std::uint8_t> before = texture.image.rgba;

        kotor::ProceduralTexture state = kotor::makeProceduralTexture(0u, texture);
        // A single raised cell, with a flat past: its neighbours get half of it, it loses itself.
        std::fill(state.current.begin(), state.current.end(), 0.0f);
        std::fill(state.previous.begin(), state.previous.end(), 0.0f);
        state.current[16u * 5u + 5u] = 40.0f;
        kotor::proceduralArturoWave(state, false);
        check(std::abs(state.current[16u * 5u + 6u] - 20.0f) < 0.001f && std::abs(state.current[16u * 5u + 5u]) < 0.001f
                  && state.previous[16u * 5u + 5u] == 40.0f,
              "arturo_wave_step_00460690");

        std::vector<std::uint8_t> heights(64u, 0u);
        std::vector<std::uint8_t> out(before.size());
        // Height 0 is angle 0: cos 1, sin 0. With amplitude 2 the texel comes from two rows on.
        kotor::proceduralDistort(heights, before, 8, 8, true, 2.0f, out);
        check(std::equal(out.begin(), out.begin() + 4, before.begin() + (2u * 8u) * 4u), "distort_height_0_moves_rows_00460d70");
        // Height 64 is a quarter turn: the texel comes from two columns on.
        std::fill(heights.begin(), heights.end(), static_cast<std::uint8_t>(64u));
        kotor::proceduralDistort(heights, before, 8, 8, true, 2.0f, out);
        check(std::equal(out.begin(), out.begin() + 4, before.begin() + 2u * 4u), "distort_height_64_moves_columns_00460d70");

        // 0045ea70: half the centre, an eighth of each neighbour.
        std::vector<std::uint8_t> spot(64u, 0u);
        spot[8u * 3u + 3u] = 200u;
        kotor::proceduralSmoothHeights(spot, 8, 8);
        check(spot[8u * 3u + 3u] == 100u && spot[8u * 3u + 4u] == 25u && spot[8u * 2u + 3u] == 25u && spot[8u * 2u + 2u] == 0u,
              "water_smooth_0045ea70");

        // 00461820: a flat surface stays flat without its kick, and a raised cell pulls its neighbours.
        kotor::Texture water = texture;
        water.txi = kotor::parseTxi("proceduretype water\ndistort 1\nwaterwidth 8\nwaterheight 8\n"
                                    "distortionamplitude 0.2\nspeed 4\n");
        kotor::ProceduralTexture wave = kotor::makeProceduralTexture(0u, water);
        kotor::proceduralWaterWave(wave, 1.0f / 15.0f, false);
        bool flat = true;
        for (const float value : wave.next) {
            flat = flat && std::abs(value - 127.0f) < 0.001f;
        }
        check(flat, "water_flat_stays_flat_00461820");
        wave.next[8u * 4u + 4u] = 200.0f;
        kotor::proceduralWaterWave(wave, 1.0f / 15.0f, false);
        check(wave.next[8u * 4u + 5u] > 127.05f && wave.next[8u * 4u + 4u] < 200.0f, "water_raised_cell_pulls_neighbours_00461820");

        // Running: the image changes, with the revision backends watch.
        kotor::ProceduralTexture running = kotor::makeProceduralTexture(0u, texture);
        const std::uint32_t revision = texture.revision;
        for (int frame = 0; frame < 120; ++frame) {
            kotor::stepProceduralTexture(running, texture, 1.0f / 60.0f);
        }
        check(texture.revision > revision && texture.image.rgba != before, "arturo_rewrites_the_image");
        kotor::ProceduralTexture runningWater = kotor::makeProceduralTexture(0u, water);
        for (int frame = 0; frame < 120; ++frame) {
            kotor::stepProceduralTexture(runningWater, water, 1.0f / 60.0f);
        }
        check(water.revision > 0u, "water_rewrites_the_image");
    }
    // Retail 00492450 and 0048e460: bounce response and a particle's sprite frame.
    {
        const kotor::Vec3 bounced = kotor::bouncedVelocity({1.0f, 0.0f, -2.0f}, 0.7f, 0.033f);
        check(std::abs(bounced.x - 0.75f) < 1e-4f && std::abs(bounced.z - 1.05f) < 1e-4f, "bounce_reflects_at_075");
        check(kotor::length(kotor::bouncedVelocity({1.0f, 0.0f, -2.0f}, 0.0f, 0.033f)) == 0.0f, "bounce_zero_stops");
        check(kotor::particleFrame(0, 15, 0.0f, 0.9f, false, 7u) == 0, "no_fps_keeps_first_frame");
        check(kotor::particleFrame(0, 15, 10.0f, 5.0f, false, 7u) == 15, "fps_stops_on_last_frame");
        check(kotor::particleFrame(0, 15, 0.0f, 0.9f, true, 7u) == 7, "random_start_frame");
        check(kotor::particleFrame(0, 15, 10.0f, 1.0f, true, 7u) == 1, "random_frame_wraps");
    }
    // Shipped data: a room's reference nodes name whole models that must be placed with the room.
    {
        const auto load = [&](const char* name) -> std::optional<kotor::Mdl> {
            auto mdl = install.find(name, kotor::resType(kotor::ResType::Mdl));
            auto mdx = install.find(name, kotor::resType(kotor::ResType::Mdx));
            if (!mdl.has_value() || !mdx.has_value()) {
                return std::nullopt;
            }
            return kotor::Mdl::parse(std::move(*mdl), std::move(*mdx), std::string(name) + ".mdl");
        };
        const std::optional<kotor::Mdl> room = load("m41aa_01a");
        const std::optional<kotor::Mdl> palm = load("or_smallpalm01");
        bool named = false;
        if (room.has_value()) {
            for (const kotor::Mdl::ReferenceNode& reference : room->references()) {
                named = named || reference.model == "or_smallpalm01" || reference.model == "OR_SmallPalm01";
            }
        }
        check(named, "room_names_reference_model");
        check(palm.has_value() && !palm->hasSkinnedMesh(), "reference_model_loads_unskinned");
        if (palm.has_value()) {
            const kotor::Mat3 quarter{{{0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
            const kotor::Mat3 still{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
            const std::vector<kotor::MdlMeshTriangle> base = palm->placedTriangles({}, still);
            const std::vector<kotor::MdlMeshTriangle> moved = palm->placedTriangles({10.0f, 0.0f, 0.0f}, quarter);
            check(!base.empty() && base.size() == moved.size(), "reference_has_triangles");
            if (!base.empty()) {
                const kotor::Vec3 from = base[0].v[0].position;
                const kotor::Vec3 to = moved[0].v[0].position;
                check(std::abs(to.x - (10.0f - from.y)) < 1e-3f && std::abs(to.y - from.x) < 1e-3f,
                      "reference_placed_by_node_transform");
            }
        }
    }
    const auto textureOf = [&](const char* name) -> const kotor::Texture* {
        const std::uint32_t index = kotor::ensureOptionalTexture(scene, install, name);
        return index < scene.textures.size() ? &scene.textures[index] : nullptr;
    };
    const auto batchFor = [&](const char* name, bool fades) {
        static std::vector<std::unique_ptr<kotor::MdlNodeMaterial>> kept;
        kept.push_back(std::make_unique<kotor::MdlNodeMaterial>());
        kept.back()->fades = fades;
        kotor::MdlMeshTriangle triangle;
        triangle.texture = name;
        triangle.material = kept.back().get();
        return kotor::makeMaterialBatch(scene, install, triangle);
    };

    // A cube map keeps its TXI after six mip chains; read as one image it was a 1:6 strip that the
    // reflection lookup turned into blocky bands of colour.
    for (const char* name : {"cm_baremetal", "cm_bright", "cm_jedcom", "window"}) {
        const kotor::Texture* cube = textureOf(name);
        check(cube != nullptr && cube->txi.cube, concat(name, "_is_cube"));
        check(cube != nullptr && cube->image.width == cube->image.height * 2, concat(name, "_unpacked_latlong"));
    }
    std::size_t strips = 0;
    std::size_t cubes = 0;
    for (const std::string& name : install.resourceNames(kotor::resType(kotor::ResType::Tpc))) {
        const auto data = install.find(name, kotor::resType(kotor::ResType::Tpc));
        if (!data.has_value() || data->size() < 128u) {
            continue;
        }
        const int w = kotor::readU16(data->data(), data->size(), 8, "w");
        const int h = kotor::readU16(data->data(), data->size(), 10, "h");
        if (h == w * 6) {
            ++cubes;
            strips += kotor::parseTxi(kotor::tpcEmbeddedTxi(*data)).cube ? 0u : 1u;
        }
    }
    logInfo(concat("materials: 1:6 textures ", cubes, " without a cube TXI ", strips));
    check(cubes >= 60u && strips == 0u, "every_stacked_texture_reads_as_cube");

    // Retail's alpha reference is a constant (00798a90) and punch-through never blends (0047af70),
    // even on a node whose alpha is animated.
    for (const bool fades : {false, true}) {
        const kotor::TextureBatch punch = batchFor("lsf_matt01", fades);
        check(punch.blend == kotor::BlendMode::Opaque, concat("punchthrough_no_blend_fades_", fades ? 1 : 0));
        check(punch.alphaRef == 0.35f, concat("punchthrough_ref_035_fades_", fades ? 1 : 0));
        check(punch.writeDepth, concat("punchthrough_depth_fades_", fades ? 1 : 0));
    }
    const kotor::TextureBatch additive = batchFor("lsp_nebula01", true);
    check(additive.blend == kotor::BlendMode::Additive && !additive.writeDepth, "txi_additive_no_depth");

    // Water under wateralpha 1 is the reflection alone (00470860); at 1 it keeps its texture.
    // A scorch mark and the floor under it, floor nearer the camera by centre: the floor comes first.
    {
        const kotor::TextureBatch mark = batchFor("lhr_blst02", false);
        const kotor::TextureBatch floor = batchFor("lhr_flr01", true);
        check(mark.showsBehind && mark.blend == kotor::BlendMode::Alpha, "scorch_mark_shows_behind");
        check(!floor.showsBehind && floor.blend == kotor::BlendMode::Alpha, "env_floor_hides_behind");
        check(kotor::blendedDrawsBefore(floor, 2.0f, mark, 9.0f), "floor_before_mark_when_nearer");
        check(!kotor::blendedDrawsBefore(mark, 9.0f, floor, 2.0f), "mark_not_before_nearer_floor");
    }
    // A lit saber swung through two frames draws its swing trail (00471860 / 00481ae0) in place of
    // the still blade's 12 faces a blade node, two-sided, and keeps its face count so the mesh keeps its batches.
    if (const std::shared_ptr<kotor::Mdl> saber = kotor::loadWorldModel(install, "w_lghtsbr_001")) {
        const auto drawn = [](const std::vector<kotor::MdlMeshTriangle>& list, std::size_t& faces) {
            std::size_t area = 0;
            faces = 0;
            for (const kotor::MdlMeshTriangle& triangle : list) {
                if (triangle.material == nullptr || !triangle.material->saber) {
                    continue;
                }
                ++faces;
                const kotor::Vec3 a = triangle.v[1].position - triangle.v[0].position;
                const kotor::Vec3 b = triangle.v[2].position - triangle.v[0].position;
                area += kotor::length(kotor::cross(a, b)) > 1e-6f ? 1u : 0u;
            }
            return area;
        };
        std::size_t stillFaces = 0;
        const std::size_t stillDrawn = drawn(saber->meshTriangles(), stillFaces);
        std::vector<kotor::SaberTrail> trails;
        kotor::SaberTrailFeed feed;
        feed.trails = &trails;
        feed.dt = 0.033f;
        kotor::MdlPoseRequest request;
        request.saberTrail = &feed;
        std::size_t trailFaces = 0;
        std::size_t trailDrawn = 0;
        bool twoSided = false;
        for (int frame = 0; frame < 3; ++frame) {
            const float turn = 0.3f * static_cast<float>(frame);
            feed.rotation = {{{std::cos(turn), -std::sin(turn), 0.0f}, {std::sin(turn), std::cos(turn), 0.0f}, {0.0f, 0.0f, 1.0f}}};
            feed.origin = {0.05f * static_cast<float>(frame), 0.0f, 1.0f};
            const std::vector<kotor::MdlMeshTriangle> posed = saber->posedTrianglesFor(request);
            trailDrawn = drawn(posed, trailFaces);
            for (const kotor::MdlMeshTriangle& triangle : posed) {
                if (triangle.material != nullptr && triangle.material->saber) {
                    twoSided = kotor::makeMaterialBatch(scene, install, triangle).twoSided;
                    break;
                }
            }
        }
        logInfo(concat("materials: saber still ", stillDrawn, "/", stillFaces, " trail ", trailDrawn, "/", trailFaces,
                       " samples ", trails.empty() ? 0 : trails.front().count));
        check(stillDrawn > 0 && stillDrawn % 12 == 0 && trailDrawn > stillDrawn, "saber_swing_draws_trail_00471860");
        check(stillFaces == trailFaces, "saber_trail_keeps_face_count");
        // 0046be00 runs a part's Update once a frame: a pose between frames draws, adds no sample.
        const int samplesBefore = trails.empty() ? -1 : trails.front().count;
        feed.step = false;
        feed.origin = {0.5f, 0.0f, 1.0f};
        (void)saber->posedTrianglesFor(request);
        check(!trails.empty() && trails.front().count == samplesBefore, "saber_trail_updates_once_a_frame_0046be00");
        check(twoSided, "saber_two_sided_004718ff");
    } else {
        check(false, "saber_model_loaded");
    }

    // A sky model's header turns fog off for it; fogged, it was only the fog colour.
    for (const auto& [name, fogged] : {std::pair{"m13aa_99z", false}, std::pair{"m13aa_01a", true}}) {
        const std::shared_ptr<kotor::Mdl> model = kotor::loadWorldModel(install, name);
        bool any = false;
        bool right = model != nullptr;
        if (model) {
            for (const kotor::MdlMeshTriangle& triangle : model->meshTriangles()) {
                any = true;
                const kotor::TextureBatch batch = kotor::makeMaterialBatch(scene, install, triangle);
                right = right && batch.noFog == !fogged;
            }
        }
        check(any && right, concat(name, "_fog_", fogged ? 1 : 0));
    }

    // Paired frames: the ARE's DynAmbientColor lights creatures, not room meshes without a lightmap,
    // and a mesh with a lightmap draws no self illumination.
    {
        kotor::Scene lit;
        lit.ambient = {};
        lit.diffuse = {};
        lit.dynamicAmbient = {0.4f, 0.3f, 0.2f};
        check(kotor::length(kotor::roomMeshLight(lit, {}, true).ambient) < 1e-5f, "blended_room_mesh_no_dynamic_ambient");
        check(kotor::length(kotor::sampleSceneLight(lit, {}).ambient) > 0.1f, "creature_keeps_dynamic_ambient");
        // Retail 004a3f70 binds an object's light without the multiplier; the room binder 004a3770
        // applies it. The sun is gidy_sun at 4000, 4500, 7000 and its two lights take slots.
        kotor::Scene::WorldLight boosted;
        boosted.position = {1.0f, 0.0f, 0.0f};
        boosted.colour = {0.5f, 0.5f, 0.5f};
        boosted.multiplier = 2.0f;
        boosted.radius = 10.0f;
        boosted.priority = 2;
        boosted.dynamicType = 1;
        lit.lights.push_back(boosted);
        check(kotor::sampleSceneLight(lit, {}).lights[0].colour.x == 0.5f, "object_light_without_multiplier");
        // Room parts: 004a4480 binds type 1 lights only, through 004a4580 with the multiplier.
        check(kotor::roomMeshLight(lit, {}).lights[0].colour.x == 1.0f, "room_light_with_multiplier_004a4580");
        {
            kotor::Scene typeTwo = lit;
            typeTwo.lights.back().dynamicType = 2;
            check(kotor::roomMeshLight(typeTwo, {}).lights[0].quadratic < 0.0f, "room_mesh_skips_type_2_lights_004a44d6");
            // RETAIL row 41: with no type 1 light 004a4580 disables GL_LIGHT1..3 and every room part draws
            // unlit, even under an area-wide type 2 light (m41aa ShadowAurora, radius 5000).
            typeTwo.rooms.push_back({"m41aa_01a", {50.0f, 50.0f, 0.0f}});
            typeTwo.lights.back().radius = 5000.0f;
            check(kotor::roomMeshLight(typeTwo, {}).lights[0].quadratic < 0.0f,
                  "room_mesh_unlit_without_type_1_light_004a4580");
            // 004a3f70: a fading light joins an object at fade 0 and grows by 3 x frame time; a paused
            // frame (step 0) leaves it black (RETAIL row 40, the Endar soldier).
            kotor::Scene fades = typeTwo;
            fades.lights.back().fading = true;
            fades.lightFades = true;
            fades.lightFadeStep = 0.0f;
            fades.lightFrame = 1;
            const float joined = kotor::objectSceneLight(fades, 7u, {}).lights[0].colour.x;
            fades.lightFrame = 2;
            const float paused = kotor::objectSceneLight(fades, 7u, {}).lights[0].colour.x;
            fades.lightFadeStep = 0.1f;
            fades.lightFrame = 3;
            const float grown = kotor::objectSceneLight(fades, 7u, {}).lights[0].colour.x;
            check(joined == 0.0f && paused == 0.0f && std::fabs(grown - 0.05f) < 1e-5f,
                  "object_light_fades_in_004a3f70");
        }
        check(kotor::length(kotor::roomMeshLight(lit, {}, false).ambient) > 0.1f, "solid_room_mesh_keeps_dynamic_ambient");
        lit.ambient = {0.5f, 0.5f, 0.5f};
        lit.diffuse = {1.0f, 1.0f, 1.0f};
        const kotor::SampledLight withSun = kotor::sampleSceneLight(lit, {});
        check(withSun.lights[0].position.x == 4000.0f && withSun.lights[0].position.y == 4500.0f
                  && withSun.lights[0].position.z == 7000.0f && withSun.lights[0].quadratic > 0.0f,
              "sun_is_a_point_light_at_retail_position");
        const float sunFade = 1.0f / (1.0f + 4.5e-10f * (4000.0f * 4000.0f + 4500.0f * 4500.0f + 7000.0f * 7000.0f));
        check(std::abs(withSun.ambient.x - (0.4f + 0.5f * sunFade)) < 1e-4f, "ambient_is_dynamic_plus_sun_light");
        check(withSun.lights[1].position.x == 1.0f && withSun.lights[2].quadratic < 0.0f, "sun_takes_two_of_three_slots");
        lit.lights.push_back(boosted);
        lit.lights.push_back(boosted);
        check(kotor::sampleSceneLight(lit, {}).lights[2].quadratic < 0.0f, "fourth_light_left_out");
        lit.lights.clear();
        kotor::MdlNodeMaterial material;
        kotor::MdlMeshTriangle triangle;
        triangle.material = &material;
        triangle.selfIllum = {0.5f, 0.5f, 0.5f};
        check(kotor::length(kotor::meshEmission(triangle)) > 0.1f, "emission_without_lightmap");
        material.lightmap = "lm";
        check(kotor::length(kotor::meshEmission(triangle)) < 1e-5f, "no_emission_with_lightmap");
    }
    // Mesh material (0046f410, 00473b55). The krayt dragon's 0.8 grey is shipped data.
    {
        kotor::MdlNodeMaterial material;
        kotor::MdlMeshTriangle triangle;
        triangle.texture = "lhr_wall01";
        triangle.material = &material;
        material.ambient = {0.0f, 0.0f, 0.0f};
        kotor::TextureBatch batch = kotor::makeMaterialBatch(scene, install, triangle);
        check(batch.materialAmbient.x == 1.0f, "white_diffuse_ignores_mesh_ambient");
        // 00473900: a placed object's part skips 0046f410 and takes the mesh ambient (RETAIL row 33).
        batch = kotor::makeMaterialBatch(scene, install, triangle, true);
        check(batch.materialAmbient.x == 0.0f, "object_part_uses_mesh_ambient");
        triangle.selfIllum = {0.2f, 0.2f, 0.2f};
        batch = kotor::makeMaterialBatch(scene, install, triangle);
        check(batch.materialAmbient.x == 0.0f, "selfillum_uses_mesh_ambient");
        triangle.selfIllum = {};
        material.diffuse = {0.5f, 0.5f, 0.5f};
        material.ambient = {0.25f, 0.25f, 0.25f};
        batch = kotor::makeMaterialBatch(scene, install, triangle);
        kotor::SampledLight in;
        in.ambient = {0.8f, 0.8f, 0.8f};
        in.lights[0].colour = {1.0f, 1.0f, 1.0f};
        const kotor::SampledLight lit = kotor::materialLight(batch, in);
        check(lit.ambient.x == 0.2f && lit.lights[0].colour.x == 0.5f, "material_scales_light");
        check(kotor::materialKey(triangle) != "lhr_wall01", "material_splits_batches");
        // Retail 0047af70 leaves the fog colour alone: additive does not fog to black.
        kotor::TextureBatch glow;
        glow.blend = kotor::BlendMode::Additive;
        check(kotor::batchFogColour(glow, {0.3f, 0.4f, 0.5f}).y == 0.4f, "additive_mesh_fogs_like_the_rest");
    }
    // gearsteam16 in m41aa_01a: alpha 0.3, 0.12, 0 at percent 0, 0.5, 1. Multiplying by the percents
    // made it start at nothing and stay near it.
    check(kotor::emitterCurve(0.3f, 0.12f, 0.0f, 0.0f, 0.5f, 1.0f, 0.0f) == 0.3f, "percent_is_time_not_alpha");
    check(kotor::emitterCurve(0.3f, 0.12f, 0.0f, 0.0f, 0.5f, 1.0f, 0.5f) == 0.12f, "percent_places_the_middle_key");
    check(kotor::emitterCurve(1.0f, 9.0f, 0.0f, 255.0f, 0.5f, 1.0f, 0.5f) == 0.5f, "percent_255_means_no_middle_key");
    // Grass by 004a6d30: whole to 15 m, gone by 30 m. A hard cut at 64 m was the old form. The fade
    // rides on the GL material alpha (0044f880), so only a bin without a lightmap fades; a
    // lightmapped bin draws its texel with GL_REPLACE and stays whole until the cut.
    {
        kotor::TextureBatch grass;
        grass.alphaRef = 0.4f;
        grass.maxDistance = 30.0f;
        grass.center = {0.0f, 0.0f, 0.0f};
        check(kotor::batchAlphaRef(grass, {10.0f, 0.0f, 0.0f}) == 0.4f, "grass_whole_within_15m");
        const float half = kotor::batchAlphaRef(grass, {std::sqrt(562.5f), 0.0f, 0.0f});
        check(half > 0.79f && half < 0.81f, "grass_half_faded_midway");
        check(kotor::batchAlphaRef(grass, {29.99f, 0.0f, 0.0f}) > 1.0f, "grass_gone_by_30m");
        grass.lightmap = 1u;
        check(kotor::batchAlphaRef(grass, {std::sqrt(562.5f), 0.0f, 0.0f}) == 0.4f,
              "lightmapped_grass_does_not_fade_004a6d30_replace");
    }
    // Grass blades by 004a9630 and their sway by 004a68b0. A face of area 8 at density 1 grows
    // round(8) = 8 blades (004a7860), each H wide at the foot, its top at 0.8..1.2 H; the atlas
    // quarter's v starts at the foot (the old form drew the texture upside down). With no wind the
    // top drifts back over the foot and the top edge takes the foot's width.
    {
        kotor::Scene grassScene;
        grassScene.textures.push_back({"__default", {}});
        grassScene.textureIndex["__default"] = 0;
        // Retail grows grass on the room's AABB faces (00456f30 bins the part answering vtable +0x48,
        // PartAABB) with that node's surfaces, lightmap and lightmap uvs. A face whose surface row
        // has no Grass grows none.
        kotor::MdlNodeMaterial lit;
        lit.lightmap = "m14aa_01b_lm1";
        kotor::MdlNodeMaterial unlit;
        kotor::MdlMeshTriangle face;
        face.material = &lit;
        face.surface = 3; // Grass set
        face.v[0].position = {0.0f, 0.0f, 0.0f};
        face.v[1].position = {4.0f, 0.0f, 0.0f};
        face.v[2].position = {0.0f, 4.0f, 0.0f};
        for (kotor::MdlMeshVertex& corner : face.v) {
            corner.uv1 = {corner.position.x * 0.25f, corner.position.y * 0.25f};
        }
        kotor::MdlMeshTriangle bare = face;
        bare.surface = 2; // Grass clear
        for (kotor::MdlMeshVertex& corner : bare.v) {
            corner.position.x += 20.0f;
        }
        kotor::MdlMeshTriangle dark = face;
        dark.material = &unlit;
        for (kotor::MdlMeshVertex& corner : dark.v) {
            corner.position.y += 40.0f;
        }
        const std::map<std::uint32_t, std::vector<kotor::MdlMeshTriangle>> walkmesh{{0u, {face, bare}}, {1u, {dark}}};
        kotor::GrassParams params;
        params.texture = "lda_grass2";
        params.density = 1.0f;
        params.quadSize = 1.0f;
        params.probLL = 1.0f;
        params.ambient = {0.25f, 0.5f, 0.75f};
        params.diffuse = {0.5f, 0.25f, 0.125f};
        const std::size_t quads = kotor::addGrass(grassScene, install, params, walkmesh, {0, 0, 0, 1});
        check(quads == 16 && grassScene.vertices.size() == 64, "grass_round_area_times_density_004a7860");
        bool shaped = quads == 16;
        for (std::size_t q = 0; shaped && q < quads; ++q) {
            const kotor::SceneVertex* blade = &grassScene.vertices[q * 4];
            const float foot = kotor::length(blade[1].position - blade[0].position);
            const float rise = blade[3].position.z - blade[0].position.z;
            shaped = std::abs(foot - 1.0f) < 1e-4f && rise > 0.79f && rise < 1.21f
                && blade[0].uv.x == 0.0f && blade[0].uv.y == 0.0f && blade[2].uv.x == 0.5f && blade[2].uv.y == 0.5f
                && blade[0].position.x < 5.0f;
        }
        check(shaped, "grass_blade_shape_and_quarter_004a9630");
        // The lightmapped face: its blades carry the face's lightmap uv at their own foot (the
        // barycentric weights of 004a9630) and draw texel x lightmap, unlit (004a6d30).
        const kotor::TextureBatch* litBatch = nullptr;
        const kotor::TextureBatch* darkBatch = nullptr;
        for (const kotor::TextureBatch& batch : grassScene.batches) {
            (batch.room == 0 ? litBatch : darkBatch) = &batch;
        }
        bool lightUv = quads == 16;
        for (std::size_t q = 0; lightUv && q < 8u; ++q) {
            const kotor::SceneVertex* blade = &grassScene.vertices[q * 4];
            const kotor::Vec3 foot = (blade[0].position + blade[1].position) * 0.5f;
            lightUv = std::abs(blade[2].uv1.x - foot.x * 0.25f) < 1e-4f && std::abs(blade[2].uv1.y - foot.y * 0.25f) < 1e-4f;
        }
        check(litBatch != nullptr && litBatch->lightmap != kotor::kNoTexture && !litBatch->bakedLight && lightUv,
              "grass_on_lightmapped_aabb_uses_its_lightmap_004a7860");
        // The face without a lightmap draws under GL lighting with Grass_Ambient / Grass_Diffuse.
        check(darkBatch != nullptr && darkBatch->lightmap == kotor::kNoTexture && darkBatch->bakedLight
                  && darkBatch->materialAmbient.y == 0.5f && darkBatch->materialDiffuse.x == 0.5f,
              "grass_without_lightmap_takes_grass_material_0044f880");
        for (int frame = 0; frame < 600; ++frame) {
            kotor::stepGrassWind(grassScene, 0.1f, {1.0f, 1.0f, 1.5f});
        }
        bool settled = quads == 16;
        for (std::size_t q = 0; settled && q < 8u; ++q) {
            const kotor::SceneVertex* blade = &grassScene.vertices[q * 4];
            const kotor::Vec3 lean = blade[3].position - blade[0].position;
            const kotor::Vec3 top = blade[2].position - blade[3].position;
            const kotor::Vec3 foot = blade[1].position - blade[0].position;
            settled = std::abs(lean.x) < 1e-3f && std::abs(lean.y) < 1e-3f
                && std::abs(top.x - foot.x) < 1e-4f && std::abs(top.y - foot.y) < 1e-4f;
        }
        check(settled, "grass_top_drifts_over_foot_without_wind_004a68b0");
        // A point source (004a6510): within its radius by the largest axis distance, pushing along
        // the offset by (radius - d) / radius x strength (004a5c80), aged by the frame time and
        // dropped below 0 (004a5e30). At (1.5, 1.5) the distance is 1.5 by the largest axis, not 2.12.
        kotor::addPointSourceWind(grassScene, {0.0f, 0.0f, 0.0f}, 10.0f, 1.0f, 1.0f);
        const kotor::Vec3 push = kotor::grassPointWind(grassScene.grassField, {1.5f, 1.5f, 0.0f});
        check(std::abs(push.x - 0.85f * 0.70710678f) < 1e-4f && std::abs(push.y - push.x) < 1e-6f && push.z == 0.0f,
              "point_wind_by_largest_axis_004a5c80");
        const kotor::Vec3 before = grassScene.vertices[3].position;
        kotor::stepGrassWind(grassScene, 0.05f, {1.0f, 1.0f, 1.5f});
        const kotor::Vec3 after = grassScene.vertices[3].position;
        check(after.x != before.x || after.y != before.y, "point_wind_moves_blades_004a68b0");
        kotor::stepGrassWind(grassScene, 0.5f, {1.0f, 1.0f, 1.5f});
        const bool kept = grassScene.grassField.pointWinds.size() == 1u;
        kotor::stepGrassWind(grassScene, 0.5f, {1.0f, 1.0f, 1.5f});
        check(kept && grassScene.grassField.pointWinds.empty(), "point_wind_dropped_after_its_life_004a5e30");
    }
    // A visual effect row spawned on a creature leaves a drawable model or emitters in the scene.
    {
        kotor::Scene& world = game.scene();
        std::size_t creature = world.entities.size();
        for (std::size_t i = 0; i < world.entities.size(); ++i) {
            if (world.entities[i].kind == kotor::EntityKind::Creature && !world.entities[i].dead) {
                creature = i;
                break;
            }
        }
        check(creature < world.entities.size(), "world_has_a_creature");
        // Row labels, the VFX_* constants: heal, force shield and a Mark I energy shield.
        for (const int row : {1019, 1015, 2032}) {
            EffectDirector::Spawn request;
            request.vfx = row;
            request.target = creature;
            request.targetObject = game.objectForEntityIndex(creature);
            request.permanent = true;
            const std::vector<std::uint32_t> ids = game.effects().spawn(request);
            check(!ids.empty(), concat("vfx_", row, "_spawns_a_model"));
        }
        // A progfx shell (006a1220): these rows have no model of their own, and drew nothing.
        const auto shellBatches = [&](const char* name) {
            std::size_t found = 0;
            for (const kotor::EntityMesh& mesh : world.entityMeshes) {
                for (const kotor::TextureBatch& batch : mesh.batches) {
                    found += batch.shell != 0u && batch.texture < world.textures.size()
                        && world.textures[batch.texture].name == name ? 1u : 0u;
                }
            }
            return found;
        };
        const std::pair<int, const char*> shellRows[] = {
            {1009, "fx_tex_03"}, {1006, "fx_tex_02"}, {1021, "fx_tex_07"}, {1035, "fx_tex_06"},
            {2040, "fx_tex_14"}, {2041, "fx_tex_17"}, {2042, "fx_tex_20"}, {2043, "fx_tex_21"},
            {2044, "fx_tex_15"}, {2045, "fx_tex_16"}, {2046, "fx_tex_22"}, {2047, "fx_tex_18"},
        };
        for (const auto& [row, name] : shellRows) {
            const std::uint32_t revision = world.meshRevision;
            EffectDirector::Spawn request;
            request.vfx = row;
            request.target = creature;
            request.targetObject = game.objectForEntityIndex(creature);
            request.permanent = true;
            game.effects().spawn(request);
            check(shellBatches(name) != 0u, concat("vfx_", row, "_shell_", name));
            // Retail 00448130 takes the blend from the TXI (these are additive), and 00477830 / 0048d040 draw
            // the pass unlit through the bumped-out vertex program (shade mode 3).
            bool shellAdds = false;
            for (const kotor::EntityMesh& mesh : world.entityMeshes) {
                for (const kotor::TextureBatch& batch : mesh.batches) {
                    shellAdds = shellAdds
                        || (batch.shell != 0u && batch.texture < world.textures.size()
                            && world.textures[batch.texture].name == name
                            && batch.blend == kotor::BlendMode::Additive && !batch.writeDepth
                            && kotor::batchShadeMode(batch) == 3.0f);
                }
            }
            check(shellAdds, concat("vfx_", row, "_shell_adds_unlit_pushed_out"));
            check(world.meshRevision != revision, concat("vfx_", row, "_shell_reaches_renderer"));
        }
        // A door's reference node shows another model, moving with the door (00447ce0).
        {
            const std::size_t door = kotor::spawnModelEntity(world, game.install(), "dor_ukn01", {}, 0.0f);
            std::size_t shown = 0;
            for (const kotor::EntityMesh& mesh : world.entityMeshes) {
                shown += mesh.entity == door && mesh.attachModel != nullptr && !mesh.vertices.empty() ? 1u : 0u;
            }
            check(door < world.entities.size() && shown == 1u, "door_reference_node_shows_its_model");
            for (kotor::EntityMesh& mesh : world.entityMeshes) {
                mesh.visible = mesh.entity == door ? false : mesh.visible;
            }
        }
        // A beam (006a0680, 006a0d30) runs from the source to the target as a lightning bolt.
        std::size_t other = creature;
        for (std::size_t i = 0; i < world.entities.size(); ++i) {
            if (i != creature && world.entities[i].kind == kotor::EntityKind::Creature) {
                other = i;
                break;
            }
        }
        {
            EffectDirector::Spawn request;
            request.vfx = 2037;
            request.target = creature;
            request.targetObject = game.objectForEntityIndex(creature);
            request.permanent = true;
            request.beamSource = other;
            const std::size_t liveBefore = game.effects().liveInstances();
            game.effects().spawn(request);
            check(game.effects().liveInstances() > liveBefore, "vfx_2037_beam_starts");
            game.effects().update(0.05f);
            kotor::stepParticles(world, 0.05f, world.entities[other].position);
            std::size_t bolts = 0;
            for (const kotor::EmitterInstance& emitter : world.emitters) {
                bolts += emitter.boltTargetSet && emitter.bolt.size() > 2u ? 1u : 0u;
            }
            check(bolts != 0u, "vfx_2037_beam_has_bolts");
        }
        // A world lightning emitter ends on its reference child (the bolt was never drawn before).
        {
            kotor::Scene bare;
            const std::shared_ptr<kotor::Mdl> model = kotor::loadWorldModel(install, "v_lightns_dur");
            kotor::Mat3 axes{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
            kotor::addModelEmitters(bare, install, model, UINT32_MAX, {0.0f, 0.0f, 0.0f}, axes);
            kotor::stepParticles(bare, 0.05f, {0.0f, 0.0f, 0.0f});
            std::size_t bolts = 0;
            for (const kotor::EmitterInstance& emitter : bare.emitters) {
                bolts += emitter.bolt.size() > 2u ? 1u : 0u;
            }
            check(bolts != 0u, "lightning_emitter_makes_a_bolt");
            kotor::buildParticleMesh(bare, {3.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
            check(!bare.particleMesh.batches.empty(), "lightning_bolt_reaches_the_particle_mesh");
        }
    }
    const kotor::TextureBatch water = batchFor("lda_water01", false);
    check(water.envMask && water.envOnlyAlpha == 0.4f && water.blend == kotor::BlendMode::Alpha, "water_map_only_at_wateralpha");
    const kotor::TextureBatch ocean = batchFor("lun_water01", false);
    check(ocean.envMask && ocean.envOnlyAlpha < 0.0f, "wateralpha_one_keeps_texture");

    // Emitter blend names, as 0049caa0 sorts them; every draw clears the depth mask.
    const auto emit = [&](const char* blendName_) {
        kotor::MdlEmitter header;
        header.blend = blendName_;
        return kotor::makeEmitterBatch(scene, header, 0u);
    };
    const kotor::TextureBatch normal = emit("NORMAL");
    check(normal.blend == kotor::BlendMode::Alpha && !normal.writeDepth && normal.alphaRef < 0.0f && !normal.noFog, "emitter_normal");
    for (const char* name : {"PunchThrough", "Punch-Through"}) {
        const kotor::TextureBatch punch = emit(name);
        check(punch.blend == kotor::BlendMode::Opaque && !punch.writeDepth && punch.alphaRef == 0.0f,
              concat("emitter_", name));
    }
    for (const char* name : {"Lighten", "", "SomeOtherName"}) {
        const kotor::TextureBatch light = emit(name);
        check(light.blend == kotor::BlendMode::Additive && !light.writeDepth && light.noFog,
              concat("emitter_additive_", name));
    }

    // Lens flares, 00457ec0: a sprite with position 1 sits on the line to the light, size and colour
    // fall with distance, nothing draws behind the camera or beyond the flare radius.
    kotor::Scene flares;
    flares.textures.push_back({"t", {}});
    kotor::FlareLight light;
    light.local = {10.0f, 0.0f, 0.0f};
    light.radius = 20.0f;
    light.colour = {0.5f, 0.25f, 0.0f};
    light.visibility = 1.0f;
    light.sprites.push_back({0u, 2.0f, 1.0f, {0.0f, 0.0f, 0.0f}});
    flares.flareLights.push_back(light);
    const kotor::Vec3 eye{};
    kotor::buildParticleMesh(flares, eye, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    check(flares.particleMesh.batches.size() == 1u, "flare_draws");
    if (!flares.particleMesh.batches.empty()) {
        const kotor::TextureBatch& batch = flares.particleMesh.batches.front();
        check(batch.blend == kotor::BlendMode::Additive && batch.opaqueOutput && !batch.depthTest && batch.noFog
                  && !batch.writeDepth, "flare_state");
        kotor::Vec3 sum{};
        for (const kotor::SceneVertex& vertex : flares.particleMesh.vertices) {
            sum = sum + vertex.position;
        }
        sum = sum * (1.0f / 6.0f);
        check(std::abs(sum.x - 2.0f) < 1e-3f && std::abs(sum.y) < 1e-3f && std::abs(sum.z) < 1e-3f, "flare_on_plane_at_light");
        const kotor::SceneVertex& corner = flares.particleMesh.vertices.front();
        check(std::abs(std::abs(corner.position.y) - 0.1f) < 1e-3f, "flare_half_size"); // 2 * (10/20) * 0.1
        check(std::abs(corner.selfIllum.x - 0.25f) < 1e-3f && std::abs(corner.selfIllum.y - 0.125f) < 1e-3f, "flare_colour");
    }
    kotor::buildParticleMesh(flares, eye, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    check(flares.particleMesh.batches.empty(), "flare_not_behind_camera");
    flares.flareLights.front().radius = 5.0f;
    kotor::buildParticleMesh(flares, eye, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    check(flares.particleMesh.batches.empty(), "flare_not_beyond_radius");
    flares.flareLights.front().radius = 20.0f;
    flares.flareLights.front().visibility = 0.0f;
    kotor::stepParticles(flares, 0.1f, eye);
    check(std::abs(flares.flareLights.front().visibility - 0.3f) < 1e-4f, "flare_fades_in_at_three_per_second");

    // Creature body maps against the names retail binds (00697610 "%s%02d" for model-type B rows,
    // the model's own names for whole-body rows, racetex where the row has one). Each list is what
    // retail drew for that model in that module (texture hook on the row 28 draw route, paired
    // capture texnames_<module>.json); every base map our meshes of that model pick must be in it.
    struct RetailBodyMaps {
        const char* module;
        const char* model;
        const char* names; // space separated, lower case
    };
    static const RetailBodyMaps kRetailBodyMaps[] = {
        {"danm14aa", "c_brith", "c_brith01"},
        {"danm14aa", "c_khounda", "c_khounda01 c_khounda02"},
        {"danm14aa", "c_khoundb", "c_khoundb01 c_khoundb02"},
        {"danm14aa", "n_commf", "n_commf05"},
        {"danm14aa", "n_commm", "n_commm02"},
        {"danm14aa", "n_jedicounm", "n_jedicounm01"},
        {"danm14aa", "n_rodian", "n_rodian04"},
        {"danm14aa", "pmhb05", "pmhb05"},
        {"danm14aa", "pmhc02", "pmhc02"},
        {"danm14aa", "pmhc04", "pmhc04"},
        {"kas_m23ad", "n_czerkaoff", "n_czerkaoff01"},
        {"kas_m23ad", "n_wookiem", "n_wookiem02 n_wookiem03 n_wookiem04 n_wookiem05 n_wookiem06"},
        {"kas_m23ad", "p_zaalbar", "p_zaalbar02"},
        {"kas_m23ad", "pmhc04", "pmhc04"},
        {"tar_m02aa", "c_drdwar", "c_drdwar01 w_rptnblstr_001"},
        {"tar_m02aa", "c_ithorian", "c_ithorian03"},
        {"tar_m02aa", "n_commf", "n_commf08"},
        {"tar_m02aa", "n_commm", "n_commm02"},
        {"tar_m02aa", "n_duros", "n_duros02 n_duros03"},
        {"tar_m02aa", "n_sithcomm", "n_sithcomm01"},
        {"tar_m02aa", "n_twilekm", "n_twilekmb01"},
        {"tar_m02aa", "pfbcm", "pfbc01"},
        {"tar_m02aa", "pfbem", "pfbe04"},
        {"tar_m02aa", "pmhc04", "pmhc04"},
        {"tar_m02ac", "c_drdprot", "c_drdprot03"},
        {"tar_m02ac", "n_commf", "n_commf02"},
        {"tar_m02ac", "pmhc04", "pmhc04"},
        {"tar_m03ae", "c_hutt", "c_hutt01"},
        {"tar_m03ae", "c_twohead", "c_twohead01"},
        {"tar_m03ae", "n_bith", "n_bith01"},
        {"tar_m03ae", "n_calonord", "n_calonordh01 n_calonord01"},
        {"tar_m03ae", "n_rodian", "n_rodian01 n_rodian02 n_rodian03 n_rodian04"},
        {"tar_m03ae", "n_smuggler", "n_smuggler01 n_smuggler02"},
        {"tar_m03ae", "n_twilekf", "n_twilekfb01 n_twilekfc01"},
        {"tar_m03ae", "n_twilekm", "n_twilekma01 n_twilekmb01"},
        {"tar_m03ae", "n_walrusman", "n_walrusman01"},
        {"tar_m03ae", "n_swoopgang", "n_swoopgang01"},
        {"tar_m03ae", "p_missionbb", "p_missionbb01"},
        {"tar_m03ae", "p_missionh", "p_missionh01 p_missionbb01"},
        {"tar_m03ae", "p_zaalbar", "p_zaalbar02"},
        {"tar_m03ae", "pmhc04", "pmhc04"},
        {"tar_m04aa", "c_rakghoul", "c_rakghoul01"},
        {"tar_m04aa", "n_commm", "n_commmd01"},
        {"tar_m04aa", "pmhc04", "pmhc04"},
    };
    std::map<std::string, std::map<std::string, std::set<std::string>>> retailMaps;
    for (const RetailBodyMaps& row : kRetailBodyMaps) {
        std::istringstream names(row.names);
        for (std::string name; names >> name;) {
            retailMaps[row.module][row.model].insert(name);
        }
    }
    const auto modelKey = [](const kotor::Mdl& model) {
        std::string name = lower(model.name());
        if (name.size() > 4u && name.compare(name.size() - 4u, 4u, ".mdl") == 0) {
            name.resize(name.size() - 4u);
        }
        return name;
    };
    for (const auto& [module, models] : retailMaps) {
        const kotor::Scene area = kotor::loadModule(install, module);
        // Retail drew each listed name on some creature of that model, so ours must draw it on one
        // too; a creature retail did not show may pick another map. No creature may lack its map.
        std::map<std::string, std::set<std::string>> ours;
        std::map<std::string, std::set<std::string>> rowMaps; // each creature's appearance map or stem
        bool complete = true;
        for (const kotor::EntityMesh& mesh : area.entityMeshes) {
            if (mesh.fx || !mesh.attachNode.empty() || !mesh.model || mesh.entity >= area.entities.size()
                || area.entities[mesh.entity].kind != kotor::EntityKind::Creature) {
                continue;
            }
            const std::string key = modelKey(*mesh.model);
            if (models.find(key) == models.end()) {
                continue;
            }
            std::set<std::string> headOnly;
            if (mesh.headModel && models.find(modelKey(*mesh.headModel)) == models.end()) {
                for (const kotor::MdlMeshTriangle& triangle : mesh.headModel->posedTriangles({}, 0.0f)) {
                    headOnly.insert(lower(triangle.texture));
                }
            }
            const kotor::Entity& entity = area.entities[mesh.entity];
            // The row predicts a map when it is a body-part row (its stem) or has a racetex (the map).
            if (const std::shared_ptr<const kotor::TwoDa> rows = install.table("appearance"); rows != nullptr) {
                const std::string row = std::to_string(entity.appearance);
                const std::string race = rows->rowIndex(row) >= 0 ? rows->cell(row, "racetex") : std::string{};
                if ((rows->rowIndex(row) >= 0 && rows->cell(row, "modeltype") == "B") || (!race.empty() && race != "****")) {
                    rowMaps[key].insert(lower(entity.texture));
                } else {
                    rowMaps[key];
                }
            }
            for (const kotor::TextureBatch& batch : mesh.batches) {
                const std::string name = batch.texture < area.textures.size() ? lower(area.textures[batch.texture].name) : "?";
                if (headOnly.count(name) != 0u) {
                    continue;
                }
                ours[key].insert(name);
                if (mesh.headModel) {
                    ours[modelKey(*mesh.headModel)].insert(name);
                }
                if (name == "__default" || name == "?") {
                    complete = false;
                    logInfo(concat("materials: body map ", module, " ", entity.tag, " appearance ", entity.appearance,
                                   " model ", key, " has no map"));
                }
            }
        }
        bool same = true;
        for (const auto& [model, names] : models) {
            const auto found = ours.find(model);
            if (found == ours.end()) {
                continue; // no creature of ours shows that model at load
            }
            for (const std::string& name : names) {
                // A map no creature of ours names at load (a row only an encounter or a script
                // spawns) cannot be compared here.
                const std::set<std::string>& stems = rowMaps[model];
                const bool named = std::any_of(stems.begin(), stems.end(), [&](const std::string& stem) {
                    return !stem.empty() && name.compare(0, stem.size(), stem) == 0;
                });
                if (found->second.count(name) == 0u && !named && rowMaps.count(model) != 0u) {
                    logInfo(concat("materials: body map ", module, " ", model, ": retail draws ", name,
                                   ", no creature of ours names it at load"));
                    continue;
                }
                if (found->second.count(name) == 0u) {
                    same = false;
                    std::string picked;
                    for (const std::string& one : found->second) {
                        picked += " " + one;
                    }
                    logInfo(concat("materials: body map ", module, " ", model, ": retail draws ", name, ", ours picks", picked));
                }
            }
        }
        check(!ours.empty(), concat("body_maps_compared_", module));
        check(complete, concat("body_maps_present_", module));
        check(same, concat("body_maps_match_retail_", module));
    }
    return ok ? 0 : 1;
}

int runTextureAudit(GameSession& game)
{
    kotor::Install& install = game.install();
    const char* folder = SDL_getenv("KE_TEX_AUDIT_DIR");
    if (folder == nullptr) {
        logError("textureaudit: set KE_TEX_AUDIT_DIR");
        return 2;
    }
    const std::filesystem::path dir(folder);
    std::filesystem::create_directories(dir);
    std::set<std::string> stems;
    for (const auto& entry : std::filesystem::directory_iterator(install.root() / "modules")) {
        const std::string low = lower(entry.path().filename().string());
        if (low.ends_with(".rim") && !low.ends_with("_s.rim")) {
            stems.insert(entry.path().stem().string());
        }
    }
    Audit audit(install, dir);
    for (const std::string& stem : stems) {
        try {
            audit.addModule(stem);
            logInfo(concat("textureaudit: ", stem, " done"));
        } catch (const std::exception& exception) {
            logWarn(concat("textureaudit: ", stem, " failed: ", exception.what()));
        }
    }
    return audit.finish();
}

} // namespace ke
