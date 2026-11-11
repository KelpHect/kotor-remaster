#include "app/Application.h"
#include "app/FramePacer.h"
#include "app/InputScript.h"
#include "app/Soak.h"

#include "audio/Output.h"
#include "audio/Reverb.h"
#include "audio/ReverbGate.h"
#include "core/Log.h"
#include "game/QuestGraph.h"
#include "game/Ambience.h"
#include "game/AnimationFamily.h"
#include "game/WorldFamily.h"
#include "game/TextureAudit.h"
#include "game/AudioSystem.h"
#include "game/BugParity.h"
#include "game/Chargen.h"
#include "game/ChargenGui.h"
#include "game/ChargenPreview.h"
#include "game/Factions.h"
#include "game/GameSession.h"
#include "game/GuiFrame.h"
#include "game/GuiPaint.h"
#include "game/GuiSystem.h"
#include "game/DialogueScreen.h"
#include "game/GameScreens.h"
#include "game/Hud.h"
#include "game/FrontEnd.h"
#include "game/FrontEndGate.h"
#include "game/MenuScene.h"
#include "game/Minigames.h"
#include "game/Pathfinding.h"
#include "game/StateTrace.h"
#include "kotor/BinkDecoder.h"
#include "kotor/Formats.h"
#include "kotor/GffWrite.h"
#include "kotor/Gui.h"
#include "kotor/SaveGame.h"
#include "render/Backend.h"
#include "render/Bmp.h"
#include "script/Corpus.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace ke {

namespace {

constexpr const char* kEngineVersion = "0.1.0";

std::string jsonEscape(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char buffer[7] = {};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                out += buffer;
            } else {
                out += static_cast<char>(ch);
            }
            break;
        }
    }
    return out;
}

struct RuntimeState {
    SDL_Window* window = nullptr;
    std::unique_ptr<IRenderBackend> backend;
    bool imguiReady = false;
    BackendKind activeBackend = BackendKind::Null;
    std::unique_ptr<kotor::Install> guiInstall;
    std::unique_ptr<GuiSystem> guiSystem;
    std::unique_ptr<GuiPainter> guiPainter;
    bool requestChargen = false;
};

// Window-side companions of the chargen screens: portrait art uploaded as backend UI
// images, and the 3D model controls' rectangles (physical pixels) for the backend's
// preview pass. Also feeds the PC portrait shown in the HUD and character panel.
struct ChargenVisuals {
    IRenderBackend* backend = nullptr;
    PortraitImages* portraits = nullptr;
    ChargenPreview* preview = nullptr;
    MenuScene* menu = nullptr;                      // the main menu's 3D scene
    FrontEnd* frontEnd = nullptr;                   // menu, options, load/save, movies, credits
    GameScreens* gameScreens = nullptr;             // inventory, journal, character, store, container...
    bool menuStaged = false;                        // that scene is the backend's preview scene
    std::map<std::string, std::uint64_t> textures; // resref -> backend UI image id
    std::map<std::string, PreviewRect> rects;       // 3D control tag -> pixels, last frame
    std::uint32_t uploadedRevision = UINT32_MAX;
    std::string portraitResref;                     // last portrait drawn by the overlay

    std::uint64_t portraitTexture(const std::string& resref)
    {
        if (backend == nullptr || portraits == nullptr || resref.empty()) {
            return 0;
        }
        const auto found = textures.find(resref);
        if (found != textures.end()) {
            return found->second;
        }
        std::uint64_t id = 0;
        std::string error;
        const kotor::Image* image = portraits->image(resref);
        if (image == nullptr) {
            logWarn(concat("portrait art not found: ", resref));
        } else if (!backend->uploadUiImage(image->width, image->height, image->rgba.data(), id,
                                           error)) {
            logWarn(concat("portrait upload failed (", resref, "): ", error));
            id = 0;
        }
        textures[resref] = id;
        return id;
    }

    void syncPreviewScene()
    {
        if (backend == nullptr || preview == nullptr || uploadedRevision == preview->revision()) {
            return;
        }
        std::string error;
        if (!backend->setPreviewScene(&preview->scene(), error)) {
            logWarn(concat("preview scene upload failed: ", error));
        }
        uploadedRevision = preview->revision();
    }

    void release()
    {
        if (backend != nullptr) {
            for (auto& entry : textures) {
                if (entry.second != 0) {
                    backend->releaseUiImage(entry.second);
                }
            }
        }
        textures.clear();
    }
};

ChargenVisuals* gVisuals = nullptr;
GuiPainter* gGuiPainter = nullptr;

// Retail 00688e10: fnt_d16x16 through 0040b360, which appends 'b' for the shipped Use Small Fonts=0 (GuiPainter::font
// does that; the retail XP float is 59 px wide at 1024x768, paired burst cb2_003499 - 'a' drew it 33 px), centred in a 200 wide box on the
// object's projected point, each older line one height up. Retail 00688ff0 counts the time left (+0x140) down and
// sets the label's alpha (+0x110) to time left / lifetime (+0x144): a straight fade over the whole life.
void paintFloatingText(GameSession& game, ImDrawList* draw, float width, float height, float scale)
{
    const GuiFont* font = gGuiPainter != nullptr ? gGuiPainter->font("fnt_d16x16") : nullptr;
    if (font == nullptr) {
        return;
    }
    const SceneCamera camera = game.camera();
    const auto dot = [](const kotor::Vec3& a, const kotor::Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const auto unit = [&](kotor::Vec3 v) {
        const float size = std::sqrt(dot(v, v));
        return size > 0.0f ? kotor::Vec3{v.x / size, v.y / size, v.z / size} : v;
    };
    const kotor::Vec3 forward = unit(camera.forward);
    const kotor::Vec3 right = unit({forward.y * camera.up.z - forward.z * camera.up.y, forward.z * camera.up.x - forward.x * camera.up.z,
                                    forward.x * camera.up.y - forward.y * camera.up.x});
    const kotor::Vec3 up{right.y * forward.z - right.z * forward.y, right.z * forward.x - right.x * forward.z,
                         right.x * forward.y - right.y * forward.x};
    const float focal = (height * 0.5f) / std::tan(camera.fovYDegrees * 0.5f * 3.14159265f / 180.0f);
    const float lineHeight = 16.0f * scale;
    for (const FloatingText& line : game.floatingTexts()) {
        kotor::Vec3 at;
        if (!game.floatingAnchor(line.object, at)) {
            continue;
        }
        const kotor::Vec3 d{at.x - camera.eye.x, at.y - camera.eye.y, at.z - camera.eye.z};
        const float depth = dot(d, forward);
        if (depth <= camera.nearClip) {
            continue;
        }
        const float x = width * 0.5f + dot(d, right) / depth * focal;
        const float y = height * 0.5f - dot(d, up) / depth * focal - static_cast<float>(line.line) * lineHeight;
        const float alpha = std::clamp((line.seconds - line.age) / line.seconds, 0.0f, 1.0f);
        const auto channel = [](float v) { return static_cast<unsigned>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        const unsigned color = channel(line.color.x) | channel(line.color.y) << 8 | channel(line.color.z) << 16 | channel(alpha) << 24;
        // RETAIL-PROVEN (AddFloatyText 0062b080 builds 0068b7c0, whose vtable 00753ea8 takes CSWGuiBlackenedLabel::Draw
        // 006aeac0 at +0x38): the text is drawn in black four times with its extent moved (-1,+1), (+1,+1), (+1,-1),
        // (-1,-1) at the same alpha, then once in its own colour. Retail frame burst\cb2_003499_0295798 shows the
        // magenta "XP 300" with that 1 px black outline.
        const unsigned black = channel(alpha) << 24;
        // Retail positions the label by a whole-number extent (00688e10 converts the projected point with ftol), and
        // the string draw 0045a850 lays its quads out on whole pixels, so the glyphs are crisp. A fractional anchor
        // made ours bilinear-soft: snap the text block to whole pixels (the same centring, rounded).
        const float block = (font->height + font->spacingB) * scale;
        const float textW = font->textWidth(line.text) * scale;
        const float left = std::round(x - 0.5f * textW);
        const float top = std::round(y - lineHeight + 0.5f * (lineHeight - block));
        for (const std::pair<float, float>& shift : {std::pair{-1.0f, 1.0f}, std::pair{1.0f, 1.0f}, std::pair{1.0f, -1.0f},
                                                      std::pair{-1.0f, -1.0f}}) {
            const float dx = shift.first * scale;
            const float dy = shift.second * scale;
            gGuiPainter->drawText(draw, *font, line.text, left + dx, top + dy, left + textW + dx, top + block + dy, 1, black, scale,
                                  false);
        }
        gGuiPainter->drawText(draw, *font, line.text, left, top, left + textW, top + block, 1, color, scale, false);
    }
}

// The pointer's box is the "tooltip" label of tooltip<size>.gui (strings 0074ffd4.., loaded per resolution; the
// controls match, only the panel size differs): corner confirm5, edge confirm6, fill dialog3, DIMENSION 4,
// dialogfont16x16 in white, centred. Retail 00624af0 sizes it to the text plus 8 each way and puts it at the
// pointer plus 15, kept 2 inside the window. Worked in HUD design units (ENGINE CHOICE: scaled with the HUD,
// 1:1 at 1024x768).
void paintTooltip(GuiSystem* tooltipGui, const std::string& text, ImDrawList* draw, float width, float height, float scale)
{
    if (text.empty() || gGuiPainter == nullptr || tooltipGui == nullptr || scale <= 0.0f) {
        return;
    }
    kotor::GuiScreen* screen = tooltipGui->editableScreen();
    kotor::GuiControl* box = nullptr;
    if (screen != nullptr) {
        for (kotor::GuiControl& control : screen->controls) {
            if (control.tag == "tooltip") {
                box = &control;
            }
        }
    }
    const GuiFont* font = box != nullptr ? gGuiPainter->font(box->font) : nullptr;
    if (box == nullptr || font == nullptr) {
        return;
    }
    const float areaW = width / scale;
    const float areaH = height / scale;
    const int boxWidth = static_cast<int>(std::ceil(font->textWidth(text))) + 8;
    const int boxHeight = static_cast<int>(std::ceil(font->height)) + 8;
    const ImVec2 pointer = ImGui::GetIO().MousePos;
    int x = static_cast<int>(pointer.x / scale) + 15;
    int y = static_cast<int>(pointer.y / scale) + 15;
    if (static_cast<float>(x + boxWidth) >= areaW) {
        x = static_cast<int>(areaW) - boxWidth - 2;
    }
    if (static_cast<float>(y + boxHeight) >= areaH) {
        y = static_cast<int>(areaH) - boxHeight - 2;
    }
    box->extent.left = x;
    box->extent.top = y;
    box->extent.width = boxWidth;
    box->extent.height = boxHeight;
    tooltipGui->setText(box->tag, text);
    GuiView view = gGuiPainter->layout(*screen, width, height);
    view.scale = scale;
    view.offsetX = 0.0f;
    view.offsetY = 0.0f;
    view.panelLeft = 0.0f;
    view.panelTop = 0.0f;
    view.anchored = false;
    GuiPaintOptions options;
    options.view = view;
    options.opaqueBackdrop = false;
    options.windowBackdrop = false;
    options.skipPanelFill = true;
    options.timeSeconds = ImGui::GetTime();
    gGuiPainter->paint(*tooltipGui, draw, width, height, options);
}
// The bark bubble (barkbubble.gui). 006a9920 sizes the panel to its text: height = the label's text
// height + 10 (the label's border adds nothing); 006a9ce0 keeps the file's LEFT and TOP (48, 140) and
// stretches it to the screen less LEFT on each side; 006a9fd0 puts LBL_BARKTEXT 5 inside the panel.
// NOT TRACED: the TOP offset 006a9ce0 adds when 005ede70 is set (0062f8f0's byte).
void paintBark(GuiSystem* barkGui, const std::string& text, ImDrawList* draw, float width, float height, float scale)
{
    if (text.empty() || gGuiPainter == nullptr || barkGui == nullptr || scale <= 0.0f) {
        return;
    }
    kotor::GuiScreen* screen = barkGui->editableScreen();
    kotor::GuiControl* label = nullptr;
    if (screen != nullptr) {
        for (kotor::GuiControl& control : screen->controls) {
            if (control.tag == "LBL_BARKTEXT") {
                label = &control;
            }
        }
    }
    const GuiFont* font = label != nullptr ? gGuiPainter->font(label->font) : nullptr;
    if (label == nullptr || font == nullptr) {
        return;
    }
    static constexpr int kLeft = 48;
    static constexpr int kTop = 140;
    const int panelWidth = static_cast<int>(width / scale) - 2 * kLeft;
    const int labelWidth = panelWidth - 10;
    // The label's text height: its lines, wrapped at word breaks to the label's width.
    int lines = 1;
    std::string line;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t next = text.find(' ', at);
        const std::string word = text.substr(at, next == std::string::npos ? std::string::npos : next - at);
        const std::string tried = line.empty() ? word : line + " " + word;
        if (!line.empty() && font->textWidth(tried) > static_cast<float>(labelWidth)) {
            ++lines;
            line = word;
        } else {
            line = tried;
        }
        at = next == std::string::npos ? text.size() : next + 1;
    }
    const int panelHeight = lines * static_cast<int>(std::ceil(font->height)) + 10;
    screen->extent.left = kLeft;
    screen->extent.top = kTop;
    screen->extent.width = panelWidth;
    screen->extent.height = panelHeight;
    label->extent.left = 5;
    label->extent.top = 5;
    label->extent.width = labelWidth;
    label->extent.height = panelHeight - 10;
    barkGui->setText(label->tag, text);
    GuiView view = gGuiPainter->layout(*screen, width, height);
    view.scale = scale;
    view.offsetX = 0.0f;
    view.offsetY = 0.0f;
    view.panelLeft = static_cast<float>(kLeft);
    view.panelTop = static_cast<float>(kTop);
    view.anchored = false;
    GuiPaintOptions options;
    options.view = view;
    options.opaqueBackdrop = false;
    options.windowBackdrop = false;
    options.timeSeconds = ImGui::GetTime();
    gGuiPainter->paint(*barkGui, draw, width, height, options);
}

LoadScreenProbe* gLoadProbe = nullptr; // the front-end gate's view of the loading screen

// Fills [low, high] with `color` except the rectangles in `holes` (where a 3D model
// control draws underneath the UI). Horizontal bands keep it to a few quads.
void fillExcluding(ImDrawList* draw, ImVec2 low, ImVec2 high, ImU32 color,
                   const std::vector<std::pair<ImVec2, ImVec2>>& holes)
{
    std::vector<float> ys = {low.y, high.y};
    for (const auto& hole : holes) {
        ys.push_back(std::clamp(hole.first.y, low.y, high.y));
        ys.push_back(std::clamp(hole.second.y, low.y, high.y));
    }
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    for (std::size_t band = 0; band + 1 < ys.size(); ++band) {
        const float y0 = ys[band];
        const float y1 = ys[band + 1];
        std::vector<std::pair<float, float>> cuts;
        for (const auto& hole : holes) {
            if (hole.first.y < y1 && hole.second.y > y0) {
                cuts.emplace_back(std::clamp(hole.first.x, low.x, high.x),
                                  std::clamp(hole.second.x, low.x, high.x));
            }
        }
        std::sort(cuts.begin(), cuts.end());
        float x = low.x;
        for (const auto& cut : cuts) {
            if (cut.first > x) {
                draw->AddRectFilled(ImVec2(x, y0), ImVec2(cut.first, y1), color);
            }
            x = std::max(x, cut.second);
        }
        if (x < high.x) {
            draw->AddRectFilled(ImVec2(x, y0), ImVec2(high.x, y1), color);
        }
    }
}

// --texture-wrap-check: renders one shipped room triangle whose UVs run far past 1 with a
// probe texture (R = U texel, G = V texel, B constant) under each wrap setting and measures
// the presented frame. Where a coordinate leaves [0,1] a clamped axis pins its channel to the
// edge texel (0 or 255); a repeating axis keeps cycling through the whole ramp. The saturated
// share of the covered pixels is compared with the share the triangle's own UVs predict.
class TextureWrapGate {
public:
    bool finished() const { return phase_ == Phase::Done; }
    int exitCode() const { return ok_ ? 0 : 67; }

    void step(IRenderBackend& backend, GameSession& game)
    {
        if (phase_ == Phase::Done) {
            return;
        }
        ++frames_;
        backend_ = backend.info().name;
        CapturedFrame frame;
        const bool captured = backend.captureFrame(frame);
        if (frames_ > 3000) {
            fail("gate_timeout");
            return;
        }
        // The module opens from a black fade and a fade tints every pixel the probe draws.
        if (game.fade().opacity > 0.0f) {
            settle_ = 0;
            return;
        }
        if (phase_ == Phase::Select) {
            if (frames_ < 3) {
                return; // let the first frames present so the readback is armed
            }
            if (!build(game.scene())) {
                fail("no_tiled_triangle");
                return;
            }
            apply(backend, flagsOrder_[0]);
            return;
        }
        if (++settle_ < 5) {
            return;
        }
        if (!captured || frame.rgba.empty()) {
            fail("capture");
            return;
        }
        if (variant_ < flagsOrder_.size()) {
            measure(frame, flagsOrder_[variant_]);
        } else if (variant_ < flagsOrder_.size() + kBumpVariants) {
            measureBump(frame);
        } else if (variant_ < flagsOrder_.size() + kBumpVariants + kProceduralVariants) {
            measureProcedural(frame);
        } else {
            measureSpecular(frame);
        }
        ++variant_;
        if (variant_ >= flagsOrder_.size() + kBumpVariants + kProceduralVariants + kSpecularVariants) {
            finish(backend);
            return;
        }
        if (variant_ < flagsOrder_.size()) {
            apply(backend, flagsOrder_[variant_]);
        } else if (variant_ < flagsOrder_.size() + kBumpVariants) {
            applyBump(backend, variant_ - flagsOrder_.size());
        } else if (variant_ < flagsOrder_.size() + kBumpVariants + kProceduralVariants) {
            applyProcedural(backend, variant_ - flagsOrder_.size() - kBumpVariants);
        } else {
            applySpecular(backend, variant_ - flagsOrder_.size() - kBumpVariants - kProceduralVariants);
        }
    }

private:
    enum class Phase { Select, Measure, Done };

    void fail(const char* what)
    {
        logError(concat("texture wrap check: ", what, " FAIL"));
        ok_ = false;
        phase_ = Phase::Done;
    }

    void verdict(bool pass, const std::string& what)
    {
        logInfo(concat("texture wrap check: ", what, pass ? " PASS" : " FAIL"));
        ok_ = ok_ && pass;
    }

    // The room triangle that tiles its texture the most, among textures that repeat.
    bool build(const kotor::Scene& source)
    {
        std::size_t triangles = 0;
        std::size_t tiled = 0;
        float bestScore = 0.0f;
        float bestArea = 0.0f;
        std::array<std::size_t, 3> best{};
        std::uint32_t bestTexture = 0;
        for (const kotor::TextureBatch& batch : source.batches) {
            if (batch.texture >= source.textures.size()) {
                continue;
            }
            const bool repeats = source.textures[batch.texture].clamp == 0;
            for (std::uint32_t at = batch.firstIndex; at + 2 < batch.firstIndex + batch.indexCount;
                 at += 3) {
                const std::array<std::size_t, 3> v{source.indices[at], source.indices[at + 1],
                                                   source.indices[at + 2]};
                float uLow = 1e9f;
                float uHigh = -1e9f;
                float vLow = 1e9f;
                float vHigh = -1e9f;
                for (const std::size_t index : v) {
                    uLow = std::min(uLow, source.vertices[index].uv.x);
                    uHigh = std::max(uHigh, source.vertices[index].uv.x);
                    vLow = std::min(vLow, source.vertices[index].uv.y);
                    vHigh = std::max(vHigh, source.vertices[index].uv.y);
                }
                ++triangles;
                const float score = std::min(uHigh - uLow, vHigh - vLow);
                const float area = 0.5f * kotor::length(kotor::cross(
                    source.vertices[v[1]].position - source.vertices[v[0]].position,
                    source.vertices[v[2]].position - source.vertices[v[0]].position));
                tiled += (uHigh - uLow > 1.0f || vHigh - vLow > 1.0f) ? 1u : 0u;
                const float longest = std::max({
                    kotor::length(source.vertices[v[1]].position - source.vertices[v[0]].position),
                    kotor::length(source.vertices[v[2]].position - source.vertices[v[1]].position),
                    kotor::length(source.vertices[v[0]].position - source.vertices[v[2]].position)});
                // Compact (not a sliver), repeating, and not so extreme that float UVs lose
                // their fraction.
                if (repeats && area >= 1.0f && area >= 0.15f * longest * longest && score < 200.0f
                    && (score > bestScore || (score == bestScore && area > bestArea))) {
                    bestScore = score;
                    bestArea = area;
                    best = v;
                    bestTexture = batch.texture;
                }
            }
        }
        logInfo(concat("texture wrap check: module=", source.module, " room triangles=", triangles,
                       " with a UV span above 1=", tiled, " chosen texture=",
                       source.textures[bestTexture].name, " min UV span=", bestScore,
                       " area=", bestArea));
        if (bestScore < 2.0f) {
            return false;
        }
        probe_ = kotor::Scene{};
        probe_.module = source.module;
        probe_.ambient = {1.0f, 1.0f, 1.0f};
        probe_.dynamicAmbient = {1.0f, 1.0f, 1.0f}; // object light starts from it (retail 00451674)
        probe_.diffuse = {0.0f, 0.0f, 0.0f};
        probe_.fogColor = {0.0f, 0.0f, 0.0f};
        probe_.fogNear = 1.0e6f;
        probe_.fogFar = 2.0e6f;
        kotor::Texture texture;
        texture.name = "wrap_probe";
        texture.image.width = 16;
        texture.image.height = 16;
        texture.image.rgba.resize(16u * 16u * 4u);
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const std::size_t o = static_cast<std::size_t>(y * 16 + x) * 4u;
                texture.image.rgba[o] = static_cast<std::uint8_t>(x * 17);
                texture.image.rgba[o + 1] = static_cast<std::uint8_t>(y * 17);
                texture.image.rgba[o + 2] = kProbeBlue;
                texture.image.rgba[o + 3] = 255;
            }
        }
        probe_.textures.push_back(std::move(texture));
        for (int i = 0; i < 3; ++i) {
            probe_.vertices.push_back(source.vertices[best[static_cast<std::size_t>(i)]]);
            probe_.indices.push_back(static_cast<std::uint32_t>(i));
        }
        kotor::TextureBatch batch;
        batch.texture = 0;
        batch.firstIndex = 0;
        batch.indexCount = 3;
        probe_.batches.push_back(batch);
        // Face the triangle head on so its UVs map affinely to the screen.
        const kotor::Vec3 a = probe_.vertices[0].position;
        const kotor::Vec3 b = probe_.vertices[1].position;
        const kotor::Vec3 c = probe_.vertices[2].position;
        const kotor::Vec3 centre = (a + b + c) * (1.0f / 3.0f);
        const kotor::Vec3 normal = kotor::normalize(kotor::cross(b - a, c - a));
        const float edge = std::max({kotor::length(b - a), kotor::length(c - b), kotor::length(a - c)});
        camera_.eye = centre + normal * (edge * 1.6f);
        camera_.forward = normal * -1.0f;
        kotor::Vec3 up = kotor::Vec3{0.0f, 0.0f, 1.0f} - normal * normal.z;
        if (kotor::length(up) < 0.1f) {
            up = kotor::Vec3{1.0f, 0.0f, 0.0f} - normal * normal.x;
        }
        camera_.up = kotor::normalize(up);
        camera_.fovYDegrees = 60.0f;
        camera_.nearClip = 0.05f;
        camera_.farClip = 500.0f;
        // Expected clamped shares, by sampling the triangle's own UV area.
        const float lowEdge = 0.5f / 16.0f;
        const float highEdge = 15.5f / 16.0f;
        constexpr int kSteps = 96;
        std::size_t samples = 0;
        std::size_t uPinned = 0;
        std::size_t vPinned = 0;
        for (int i = 0; i < kSteps; ++i) {
            for (int j = 0; i + j < kSteps; ++j) {
                const float s = (static_cast<float>(i) + 0.33f) / kSteps;
                const float t = (static_cast<float>(j) + 0.33f) / kSteps;
                const float u = probe_.vertices[0].uv.x * (1.0f - s - t)
                    + probe_.vertices[1].uv.x * s + probe_.vertices[2].uv.x * t;
                const float v = probe_.vertices[0].uv.y * (1.0f - s - t)
                    + probe_.vertices[1].uv.y * s + probe_.vertices[2].uv.y * t;
                ++samples;
                uPinned += (u <= lowEdge || u >= highEdge) ? 1u : 0u;
                vPinned += (v <= lowEdge || v >= highEdge) ? 1u : 0u;
            }
        }
        expectedU_ = static_cast<float>(uPinned) / static_cast<float>(samples);
        expectedV_ = static_cast<float>(vPinned) / static_cast<float>(samples);
        logInfo(concat("texture wrap check: UV area outside the texel centres: u=", expectedU_,
                       " v=", expectedV_));
        phase_ = Phase::Measure;
        return true;
    }

    void apply(IRenderBackend& backend, std::uint8_t flags)
    {
        probe_.textures[0].clamp = flags;
        backend.setScene(&probe_);
        backend.setCamera(camera_);
        settle_ = 0;
    }

    void measure(const CapturedFrame& frame, std::uint8_t flags)
    {
        // The probe's blue is constant and the camera centres the triangle, so the most
        // common blue around the middle of the frame marks the covered pixels, whatever the
        // backend does to the colour encoding.
        const std::size_t pixels = frame.rgba.size() / 4u;
        std::array<std::size_t, 256> blues{};
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                const int x = std::clamp(frame.width / 2 + dx, 0, frame.width - 1);
                const int y = std::clamp(frame.height / 2 + dy, 0, frame.height - 1);
                ++blues[frame.rgba[(static_cast<std::size_t>(y) * frame.width + x) * 4u + 2u]];
            }
        }
        std::size_t modeBlue = 0;
        for (std::size_t b = 0; b < 256; ++b) {
            modeBlue = blues[b] > blues[modeBlue] ? b : modeBlue;
        }
        std::size_t covered = 0;
        std::size_t uPinned = 0;
        std::size_t vPinned = 0;
        for (std::size_t i = 0; i < pixels; ++i) {
            const std::uint8_t* p = &frame.rgba[i * 4u];
            if (p[2] != modeBlue) {
                continue;
            }
            ++covered;
            uPinned += (p[0] <= 3 || p[0] >= 252) ? 1u : 0u;
            vPinned += (p[1] <= 3 || p[1] >= 252) ? 1u : 0u;
        }
        const float u = covered > 0 ? static_cast<float>(uPinned) / static_cast<float>(covered) : 0.0f;
        const float v = covered > 0 ? static_cast<float>(vPinned) / static_cast<float>(covered) : 0.0f;
        const bool clampU = (flags & 1u) != 0u;
        const bool clampV = (flags & 2u) != 0u;
        // A repeating axis pins only where a sample lands on an end texel, a clamped axis
        // pins wherever the coordinate leaves the texel centres.
        const bool uOk = clampU ? std::fabs(u - expectedU_) <= 0.08f : u <= 0.06f;
        const bool vOk = clampV ? std::fabs(v - expectedV_) <= 0.08f : v <= 0.06f;
        logInfo(concat("texture wrap check: backend=", backend_, " clamp=", static_cast<int>(flags),
                       " covered=", covered, " u_pinned=", u, " v_pinned=", v));
        // Some of the frame, but not all of it: a frame the probe did not draw into would pass
        // the repeat bounds by being empty.
        verdict(covered >= 2000 && covered * 100 >= pixels && covered * 100 <= pixels * 60,
                concat("clamp=", static_cast<int>(flags), " covered_pixels"));
        verdict(uOk, concat("clamp=", static_cast<int>(flags), " u_axis ",
                            clampU ? "clamped" : "repeats"));
        verdict(vOk, concat("clamp=", static_cast<int>(flags), " v_axis ",
                            clampV ? "clamped" : "repeats"));
        if (flags == 0) {
            // The streak this gate exists for: with the old clamp-everywhere sampler the
            // pinned share would sit at the expected value, far above the repeat result.
            verdict(expectedU_ >= 0.4f || expectedV_ >= 0.4f, "tiling_would_pin_a_clamped_sampler");
        }
    }

    // A known wavy surface: map-only reflection of a striped environment (83025c), with a flat
    // normal map, then with a two-cell ripple atlas at each of its cells.
    void applyBump(IRenderBackend& backend, std::size_t which)
    {
        if (which == 0) {
            kotor::Texture stripes;
            stripes.name = "bump_probe_env";
            stripes.image.width = 128;
            stripes.image.height = 64;
            stripes.image.rgba.resize(128u * 64u * 4u);
            for (int y = 0; y < 64; ++y) {
                for (int x = 0; x < 128; ++x) {
                    const std::uint8_t v = ((x / 8) & 1) != 0 ? 240 : 20;
                    std::uint8_t* out = &stripes.image.rgba[static_cast<std::size_t>(y * 128 + x) * 4u];
                    out[0] = out[1] = out[2] = v;
                    out[3] = 255;
                }
            }
            kotor::Texture flat;
            flat.name = "bump_probe_flat";
            flat.image.width = 2;
            flat.image.height = 2;
            flat.image.rgba.assign(16u, 255u);
            for (std::size_t i = 0; i < 16u; i += 4u) {
                flat.image.rgba[i] = 128u;
                flat.image.rgba[i + 1u] = 128u;
            }
            kotor::Texture ripple;
            ripple.name = "bump_probe_ripple";
            ripple.image.width = 64;
            ripple.image.height = 32;
            ripple.image.rgba.resize(64u * 32u * 4u);
            for (int y = 0; y < 32; ++y) {
                for (int x = 0; x < 64; ++x) {
                    const float phase = static_cast<float>(x % 32) / 32.0f * 6.2831853f + (x >= 32 ? 3.14159265f : 0.0f);
                    const kotor::Vec3 n = kotor::normalize(kotor::Vec3{0.5f * std::sin(phase), 0.0f, 1.0f});
                    std::uint8_t* out = &ripple.image.rgba[static_cast<std::size_t>(y * 64 + x) * 4u];
                    out[0] = static_cast<std::uint8_t>((n.x * 0.5f + 0.5f) * 255.0f);
                    out[1] = 128u;
                    out[2] = static_cast<std::uint8_t>((n.z * 0.5f + 0.5f) * 255.0f);
                    out[3] = 255u;
                }
            }
            probe_.textures.resize(1u);
            probe_.textures[0].clamp = 0;
            probe_.textures.push_back(std::move(stripes));
            probe_.textures.push_back(std::move(flat));
            probe_.textures.push_back(std::move(ripple));
        }
        kotor::TextureBatch& batch = probe_.batches[0];
        batch.envmap = 1u;
        batch.envMask = true;
        batch.envOnlyAlpha = 1.0f;
        batch.blend = kotor::BlendMode::Alpha;
        batch.bumpmap = which == 0 ? 2u : 3u;
        batch.bumpCycleX = which == 0 ? 1 : 2;
        batch.bumpCycleY = 1;
        batch.bumpCycleFps = which == 0 ? 0.0f : 1.0f;
        probe_.clock = which == 2 ? 1.5f : 0.5f;
        backend.setScene(&probe_);
        backend.setCamera(camera_);
        settle_ = 0;
    }

    void measureBump(const CapturedFrame& frame)
    {
        std::vector<std::uint8_t> grey(static_cast<std::size_t>(frame.width) * frame.height);
        for (std::size_t i = 0; i < grey.size(); ++i) {
            grey[i] = frame.rgba[i * 4u];
        }
        const std::size_t which = variant_ - flagsOrder_.size();
        std::size_t differing = 0;
        const std::vector<std::uint8_t>& against = which == 2 ? rippleGrey_ : flatGrey_;
        for (std::size_t i = 0; i < grey.size() && i < against.size(); ++i) {
            differing += std::abs(static_cast<int>(grey[i]) - static_cast<int>(against[i])) > 30 ? 1u : 0u;
        }
        if (which == 0) {
            flatGrey_ = std::move(grey);
        } else if (which == 1) {
            logInfo(concat("texture wrap check: ripple differs from the flat map on ", differing, " pixels"));
            verdict(differing > 1000u, "bump_ripple_moves_the_reflection");
            rippleGrey_ = std::move(grey);
        } else {
            logInfo(concat("texture wrap check: bump cells differ on ", differing, " pixels"));
            verdict(differing > 200u, "bump_cycle_cell_changes_the_frame");
        }
    }

    // An animated texture on the probe triangle. The second frame has no setScene before it: only
    // the backend's per-frame upload can show the new image.
    void applyProcedural(IRenderBackend& backend, std::size_t which)
    {
        if (which == 0) {
            kotor::Texture stripes;
            stripes.name = "procedural_probe";
            stripes.txi = kotor::parseTxi("proceduretype arturo\ndistort 1\narturowidth 16\narturoheight 16\n"
                                          "distortionamplitude 2\nspeed 6\n");
            stripes.image.width = 64;
            stripes.image.height = 64;
            stripes.image.rgba.resize(64u * 64u * 4u);
            for (int y = 0; y < 64; ++y) {
                for (int x = 0; x < 64; ++x) {
                    const std::uint8_t v = (((x / 2) ^ (y / 2)) & 1) != 0 ? 240 : 20;
                    std::uint8_t* out = &stripes.image.rgba[static_cast<std::size_t>(y * 64 + x) * 4u];
                    out[0] = out[1] = out[2] = v;
                    out[3] = 255;
                }
            }
            probe_.textures.resize(1u);
            probe_.textures[0] = std::move(stripes);
            kotor::TextureBatch& batch = probe_.batches[0];
            batch.envmap = kotor::kNoTexture;
            batch.envMask = false;
            batch.envOnlyAlpha = -1.0f;
            batch.bumpmap = kotor::kNoTexture;
            batch.bumpCycleX = 1;
            batch.bumpCycleY = 1;
            batch.bumpCycleFps = 0.0f;
            batch.blend = kotor::BlendMode::Opaque;
            kotor::updateProceduralTextures(probe_, 0.0f);
            backend.setScene(&probe_);
            backend.setCamera(camera_);
        } else {
            for (int frame = 0; frame < 30; ++frame) {
                kotor::updateProceduralTextures(probe_, 1.0f / 60.0f);
            }
        }
        settle_ = 0;
    }

    // Program 830250 on a flat map under one light at the camera: the same frame without and with
    // the specular term, with texture alpha 0 so that its mask lets it all through.
    void applySpecular(IRenderBackend& backend, std::size_t which)
    {
        if (which == 0) {
            kotor::Texture dark;
            dark.name = "specular_probe";
            dark.image.width = 2;
            dark.image.height = 2;
            dark.image.rgba.assign(16u, 0u);
            kotor::Texture flat;
            flat.name = "specular_probe_flat";
            flat.image.width = 2;
            flat.image.height = 2;
            flat.image.rgba.assign(16u, 255u);
            for (std::size_t i = 0; i < 16u; i += 4u) {
                flat.image.rgba[i] = 128u;
                flat.image.rgba[i + 1u] = 128u;
            }
            // The texture list stays: the backend's per-frame uploads run on the scene it holds.
            probe_.procedurals.clear();
            probe_.textures.resize(1u);
            probe_.textures[0] = std::move(dark);
            probe_.textures.push_back(std::move(flat));
            kotor::TextureBatch& batch = probe_.batches[0];
            batch.lightmap = kotor::kNoTexture;
            batch.bumpmap = 1u;
            batch.bakedLight = true;
            batch.baked = {};
            batch.baked.lights[0].position = camera_.eye;
            batch.baked.lights[0].colour = {1.0f, 1.0f, 1.0f};
            batch.baked.lights[0].quadratic = 0.0f;
        }
        probe_.batches[0].bumpSpecular = which == 0 ? 0.0f : 1.0f;
        backend.setScene(&probe_);
        backend.setCamera(camera_);
        settle_ = 0;
    }

    void measureSpecular(const CapturedFrame& frame)
    {
        std::vector<std::uint8_t> grey(static_cast<std::size_t>(frame.width) * frame.height);
        for (std::size_t i = 0; i < grey.size(); ++i) {
            grey[i] = frame.rgba[i * 4u];
        }
        const std::size_t which = variant_ - flagsOrder_.size() - kBumpVariants - kProceduralVariants;
        if (which == 0) {
            proceduralGrey_ = std::move(grey);
            return;
        }
        std::size_t differing = 0;
        for (std::size_t i = 0; i < grey.size() && i < proceduralGrey_.size(); ++i) {
            differing += static_cast<int>(grey[i]) - static_cast<int>(proceduralGrey_[i]) > 30 ? 1u : 0u;
        }
        logInfo(concat("texture wrap check: specular brightens ", differing, " pixels"));
        verdict(differing > 200u, "bump_specular_reaches_the_frame");
    }

    void measureProcedural(const CapturedFrame& frame)
    {
        std::vector<std::uint8_t> grey(static_cast<std::size_t>(frame.width) * frame.height);
        for (std::size_t i = 0; i < grey.size(); ++i) {
            grey[i] = frame.rgba[i * 4u];
        }
        const std::size_t which = variant_ - flagsOrder_.size() - kBumpVariants;
        if (which == 0) {
            proceduralGrey_ = std::move(grey);
            return;
        }
        std::size_t differing = 0;
        for (std::size_t i = 0; i < grey.size() && i < proceduralGrey_.size(); ++i) {
            differing += std::abs(static_cast<int>(grey[i]) - static_cast<int>(proceduralGrey_[i])) > 30 ? 1u : 0u;
        }
        logInfo(concat("texture wrap check: animated texture differs on ", differing, " pixels"));
        verdict(differing > 200u, "animated_texture_reaches_the_frame");
    }

    void finish(IRenderBackend& backend)
    {
        backend.setScene(nullptr);
        logInfo(concat("texture wrap check: ", ok_ ? "PASS" : "FAIL"));
        phase_ = Phase::Done;
    }

    static constexpr std::uint8_t kProbeBlue = 200;
    static constexpr std::size_t kBumpVariants = 3;
    static constexpr std::size_t kProceduralVariants = 2;
    static constexpr std::size_t kSpecularVariants = 2;
    std::vector<std::uint8_t> proceduralGrey_;
    std::vector<std::uint8_t> flatGrey_;
    std::vector<std::uint8_t> rippleGrey_;
    Phase phase_ = Phase::Select;
    bool ok_ = true;
    int frames_ = 0;
    int settle_ = 0;
    std::size_t variant_ = 0;
    // Both axes repeat, both clamp (the old behaviour), U only, V only.
    std::array<std::uint8_t, 4> flagsOrder_{0, 3, 1, 2};
    kotor::Scene probe_;
    SceneCamera camera_;
    float expectedU_ = 0.0f;
    float expectedV_ = 0.0f;
    std::string backend_;
};

// --chargen-preview-test: drives the shipped classsel and portcust screens with pointer
// events through GuiSystem and asserts on the presented frame that each 3D model control
// holds drawn pixels, that they differ between classes and between portraits.
class ChargenPreviewGate {
public:
    // fromMenu: start on the main menu, click New Game and run the whole shipped flow
    // (classsel to the name screen) with pointer events, then expect the first module.
    ChargenPreviewGate(std::string outDir, bool fromMenu)
        : outDir_(std::move(outDir)), fromMenu_(fromMenu)
    {
        if (fromMenu_) {
            phase_ = Phase::MenuWait;
        }
    }

    bool finished() const { return phase_ == Phase::Done; }
    int exitCode() const { return ok_ ? 0 : 77; }

    void step(IRenderBackend& backend, GuiSystem& gui, ChargenGuiState* flowPointer,
              ChargenVisuals& visuals, GameSession& game)
    {
        if (phase_ == Phase::Done) {
            return;
        }
        if (phase_ == Phase::MenuWait || phase_ == Phase::MenuClick || phase_ == Phase::MenuOpen) {
            stepMenu(gui, flowPointer);
            return;
        }
        const bool afterPreviews = phase_ == Phase::AcceptChain || phase_ == Phase::ChainSettle
            || phase_ == Phase::ModuleWait;
        if (flowPointer == nullptr || (visuals.preview == nullptr && !afterPreviews)) {
            check(false, "preview_available");
            phase_ = Phase::Done;
            return;
        }
        ChargenGuiState& flow = *flowPointer;
        ++frames_;
        if (frames_ == 1) {
            // GL arms its pre-swap read on the first request; later captures return the
            // cached presented frame.
            CapturedFrame warm;
            backend.captureFrame(warm);
        }
        if (frames_ > 600) {
            check(false, "gate_timeout");
            phase_ = Phase::Done;
            return;
        }
        switch (phase_) {
        case Phase::ClassWait:
            if (frames_ >= 8) {
                classScreen(backend, visuals);
                // BTN_SEL1 = male Scoundrel (006dc3c0): the class of the retail walk, so the captured frames pair by class.
                click(gui, "BTN_SEL1");
                phase_ = Phase::ClassClick;
            }
            break;
        case Phase::ClassClick:
            if (clickStep(gui)) {
                phase_ = Phase::HubChoiceWait;
                frames_ = 0;
            }
            break;
        case Phase::HubChoiceWait:
            // Retail 006eb420: the class pick opens MAINCG with Quick or Custom, the portrait, the model
            // in its smoke, the scores blank and Quick's help (241) in LB_DESC. Fails on the old flow,
            // which went from the class straight to the portrait screen.
            if (gui.screenName() == "maincg_qorcpnl" && frames_ >= 12) {
                CapturedFrame shot;
                capture(backend, shot, "maincg_choice");
                const auto& tlk = game.localization().tlk();
                check(!gui.state("PORTRAIT_LBL").fill.empty(), "hub_portrait");
                check(gui.state("LBL_CLASS").text == game.rules().className(flow.chargen.classId()), "hub_class_name");
                check(gui.state("LB_DESC").items.size() == 1 && gui.state("LB_DESC").items[0].text == tlk.text(241),
                      "hub_quick_help");
                check(gui.state("STR_AB_LBL").text.empty() && !gui.state("OLD_LBL").visible, "hub_scores_blank");
                const auto model = visuals.rects.find("MODEL_LBL");
                check(model != visuals.rects.end() && !shot.rgba.empty() && drawnShare(shot, model->second) > 0.5,
                      "hub_model_drawn");
                click(gui, "CUST_CHAR_BTN");
                phase_ = Phase::HubChoiceClick;
            } else if (frames_ >= 200) {
                check(false, "hub_opens_after_class");
                phase_ = Phase::Done;
            }
            break;
        case Phase::HubChoiceClick:
            if (clickStep(gui)) {
                phase_ = Phase::HubCustomWait;
                frames_ = 0;
            }
            break;
        case Phase::HubCustomWait:
            if (gui.screenName() == "maincg_custpnl" && frames_ >= 8) {
                CapturedFrame shot;
                capture(backend, shot, "maincg_custom");
                check(flow.customStep == 0 && gui.state("LBL_1").marked && !gui.state("LBL_2").marked,
                      "hub_custom_first_step_lit");
                click(gui, "BTN_STEPNAME1");
                phase_ = Phase::HubCustomClick;
            } else if (frames_ >= 200) {
                check(false, "hub_custom_opens");
                phase_ = Phase::Done;
            }
            break;
        case Phase::HubCustomClick:
            if (clickStep(gui)) {
                phase_ = Phase::PortraitWait;
                frames_ = 0;
            }
            break;
        case Phase::PortraitWait:
            if (gui.screenName() == "portcust" && frames_ >= 8) {
                first_ = portraitScreen(backend, visuals, "portrait_a");
                firstIndex_ = flow.chargen.portraitIndex();
                click(gui, "BTN_ARRR");
                phase_ = Phase::PortraitClick;
            }
            break;
        case Phase::PortraitClick:
            if (clickStep(gui)) {
                phase_ = Phase::CycleWait;
                frames_ = 0;
            }
            break;
        case Phase::CycleWait:
            if (frames_ >= 8) {
                check(flow.chargen.portraitIndex() == firstIndex_ + 1, "cycled_portrait_index");
                const CapturedFrame second = portraitScreen(backend, visuals, "portrait_b");
                check(differs(first_, second, visuals.rects["LBL_HEAD"], 0.01), "head_changes");
                if (fromMenu_) {
                    chainIndex_ = 0;
                    phase_ = Phase::AcceptChain;
                    startChainClick(gui);
                } else {
                    phase_ = Phase::Done;
                }
            }
            break;
        case Phase::AcceptChain:
            if (clickStep(gui)) {
                phase_ = Phase::ChainSettle;
                frames_ = 0;
            }
            break;
        case Phase::ChainSettle:
            if (frames_ >= 4) {
                const ChainStep& done = kChain[chainIndex_];
                check(std::string(done.expectScreen).empty()
                          || gui.screenName() == done.expectScreen,
                      "chain_" + std::to_string(chainIndex_) + "_" + done.tag + "_screen");
                const std::string shown = done.expectScreen;
                if (shown == "abchrgen" || shown == "skchrgen" || shown == "ftchrgen") {
                    // The screen carries the build's numbers (the file's labels are empty).
                    const GuiControlState points = gui.state(shown == "ftchrgen" ? "STD_REMAINING_SELECTIONS_LBL"
                                                                                  : "REMAINING_SELECTIONS_LBL");
                    check(points.hasText && !points.text.empty() && !gui.state("MAIN_TITLE_LBL").text.empty(),
                          shown + "_shows_points");
                    CapturedFrame shot;
                    capture(backend, shot, shown.c_str());
                }
                if (shown == "skillinfo") {
                    CapturedFrame shot;
                    capture(backend, shot, "feats_granted");
                }
                if (shown == "maincg_custpnl") {
                    // Back on MAINCG: the step after the one accepted is lit, and from the attributes on the
                    // scores, vitality, defence and saves are shown (006ef500 -> 006eac30).
                    const int lit = flow.customStep;
                    check(gui.state("LBL_" + std::to_string(lit + 1)).marked
                              && !gui.state("LBL_" + std::to_string(lit == 0 ? 2 : lit)).marked,
                          "hub_step_" + std::to_string(lit + 1) + "_lit");
                    if (lit >= 2) {
                        check(!gui.state("STR_AB_LBL").text.empty() && !gui.state("LBL_VIT").text.empty()
                                  && !gui.state("NEW_REFL_LBL").text.empty(),
                              "hub_scores_after_attributes");
                    }
                    if (lit == 5) {
                        check(gui.state("LBL_NAME").text == flow.chargen.name(), "hub_shows_name");
                    }
                    CapturedFrame shot;
                    capture(backend, shot, ("maincg_custom_step" + std::to_string(lit + 1)).c_str());
                }
                if (shown == "name") {
                    // The shipped name screen: its box shows the default, and real key and text events
                    // (SDL, as the keyboard sends them) replace it.
                    if (!nameTyped_) {
                        CapturedFrame shot;
                        capture(backend, shot, "name");
                        check(gui.state("NAME_BOX_EDIT").text.size() > 3
                                  && gui.state("NAME_BOX_EDIT").text.rfind("Player", 0) != 0
                                  && !gui.state("MAIN_TITLE_LBL").text.empty(),
                              "name_box_shows_a_generated_name");
                        for (int i = 0; i < 18; ++i) {
                            SDL_Event key{};
                            key.type = SDL_EVENT_KEY_DOWN;
                            key.key.key = SDLK_BACKSPACE;
                            key.key.scancode = SDL_SCANCODE_BACKSPACE;
                            SDL_PushEvent(&key);
                        }
                        static const char typed[] = "Ney";
                        SDL_Event text{};
                        text.type = SDL_EVENT_TEXT_INPUT;
                        text.text.text = typed;
                        SDL_PushEvent(&text);
                        nameTyped_ = true;
                        frames_ = 0;
                        break;
                    }
                    const std::string boxText = gui.state("NAME_BOX_EDIT").text;
                    check(boxText.rfind("Ney", 0) == 0 && boxText.size() <= 4, "name_box_shows_the_typed_name");
                }
                ++chainIndex_;
                if (chainIndex_ >= kChain.size()) {
                    phase_ = Phase::ModuleWait;
                    frames_ = 0;
                } else {
                    phase_ = Phase::AcceptChain;
                    startChainClick(gui);
                }
            }
            break;
        case Phase::ModuleWait:
            if (lowerCase(game.module()) == "end_m01aa") {
                check(flow.finished && !flow.active, "flow_finished");
                check(game.character().classId == 2 && game.character().gender == 0,
                      "character_applied");
                check(game.character().name == "NEY", "name_applied");
                check(game.playerEntity() != static_cast<std::size_t>(-1), "avatar_in_module");
                check(!game.scene().entities.empty(), "first_module_scene");
                phase_ = Phase::Done;
            } else if (frames_ >= 300) {
                check(false, "first_module_entered");
                phase_ = Phase::Done;
            }
            break;
        case Phase::MenuWait:
        case Phase::MenuClick:
        case Phase::MenuOpen:
        case Phase::Done:
            break;
        }
    }

private:
    enum class Phase {
        MenuWait, MenuClick, MenuOpen,
        ClassWait, ClassClick, HubChoiceWait, HubChoiceClick, HubCustomWait, HubCustomClick,
        PortraitWait, PortraitClick, CycleWait,
        AcceptChain, ChainSettle, ModuleWait, Done
    };

    struct ChainStep {
        const char* tag;
        const char* expectScreen; // screen shown after the click ("" = not checked)
    };
    // Custom (CUSTPNL): each step screen's Accept goes back to MAINCG with the next step lit, and the
    // next numbered button opens it; Play (step 6) starts the game.
    // The feats screen opens under the granted-feats box (006f3460); its OK uncovers the screen.
    static constexpr std::array<ChainStep, 12> kChain{{
        {"BTN_ACCEPT", "maincg_custpnl"}, {"BTN_STEPNAME2", "abchrgen"}, {"BTN_ACCEPT", "maincg_custpnl"},
        {"BTN_STEPNAME3", "skchrgen"}, {"BTN_ACCEPT", "maincg_custpnl"}, {"BTN_STEPNAME4", "skillinfo"},
        {"BTN_OK", "ftchrgen"},
        {"BTN_SELECT", ""}, {"BTN_ACCEPT", "maincg_custpnl"}, {"BTN_STEPNAME5", "name"},
        {"END_BTN", "maincg_custpnl"}, {"BTN_STEPNAME6", ""}}};

    static std::string lowerCase(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    void startChainClick(GuiSystem& gui) { click(gui, kChain[chainIndex_].tag); }

    // Main menu: wait, click BTN_NEWGAME, then let the flow open classsel.
    void stepMenu(GuiSystem& gui, ChargenGuiState* flow)
    {
        ++frames_;
        if (frames_ > 600) {
            check(false, "menu_timeout");
            phase_ = Phase::Done;
            return;
        }
        switch (phase_) {
        case Phase::MenuWait:
            if (frames_ >= 8) {
                check(gui.screenName() == "mainmenu", "menu_screen");
                click(gui, "BTN_NEWGAME");
                phase_ = Phase::MenuClick;
            }
            break;
        case Phase::MenuClick:
            if (clickStep(gui)) {
                phase_ = Phase::MenuOpen;
                frames_ = 0;
            }
            break;
        default:
            if (frames_ >= 3 && gui.screenName() == "classsel" && flow != nullptr
                && flow->active) {
                check(true, "new_game_opens_classsel");
                phase_ = Phase::ClassWait;
                frames_ = 0;
            }
            break;
        }
    }

    void check(bool condition, const std::string& label)
    {
        logInfo(concat("chargen_preview: ", label, "=", condition ? 1 : 0));
        ok_ = ok_ && condition;
    }

    static bool near(std::uint8_t a, std::uint8_t b, int tolerance)
    {
        return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= tolerance;
    }

    // Share of the rectangle's pixels that differ from the view clear colour.
    static double drawnShare(const CapturedFrame& frame, const PreviewRect& rect)
    {
        const std::uint8_t clear[3] = {5, 5, 10}; // PreviewView::background in bytes
        long long total = 0;
        long long drawn = 0;
        for (int y = std::max(rect.y + 2, 0); y < std::min(rect.y + rect.height - 2, frame.height);
             ++y) {
            for (int x = std::max(rect.x + 2, 0);
                 x < std::min(rect.x + rect.width - 2, frame.width); ++x) {
                const std::uint8_t* pixel =
                    &frame.rgba[(static_cast<std::size_t>(y) * frame.width + x) * 4];
                ++total;
                if (!near(pixel[0], clear[0], 10) || !near(pixel[1], clear[1], 10)
                    || !near(pixel[2], clear[2], 10)) {
                    ++drawn;
                }
            }
        }
        return total > 0 ? static_cast<double>(drawn) / static_cast<double>(total) : 0.0;
    }

    // Share of pixels inside `rect` that differ noticeably between two frames.
    static double differenceShare(const CapturedFrame& a, const CapturedFrame& b,
                                  const PreviewRect& rect)
    {
        if (a.width != b.width || a.height != b.height) {
            return 1.0;
        }
        long long total = 0;
        long long changed = 0;
        for (int y = std::max(rect.y + 2, 0); y < std::min(rect.y + rect.height - 2, a.height);
             ++y) {
            for (int x = std::max(rect.x + 2, 0); x < std::min(rect.x + rect.width - 2, a.width);
                 ++x) {
                const std::size_t at = (static_cast<std::size_t>(y) * a.width + x) * 4;
                ++total;
                if (!near(a.rgba[at], b.rgba[at], 24) || !near(a.rgba[at + 1], b.rgba[at + 1], 24)
                    || !near(a.rgba[at + 2], b.rgba[at + 2], 24)) {
                    ++changed;
                }
            }
        }
        return total > 0 ? static_cast<double>(changed) / static_cast<double>(total) : 0.0;
    }

    static bool differs(const CapturedFrame& a, const CapturedFrame& b, const PreviewRect& rect,
                        double share)
    {
        return differenceShare(a, b, rect) > share;
    }

    // Compares the same rectangle of two frames cropped from one capture: the
    // second rectangle is moved onto the first so two controls can be compared.
    static double crossDifference(const CapturedFrame& frame, const PreviewRect& left,
                                  const PreviewRect& right)
    {
        const int width = std::min(left.width, right.width) - 4;
        const int height = std::min(left.height, right.height) - 4;
        long long changed = 0;
        long long total = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int ax = left.x + 2 + x;
                const int ay = left.y + 2 + y;
                const int bx = right.x + 2 + x;
                const int by = right.y + 2 + y;
                if (ax >= frame.width || ay >= frame.height || bx >= frame.width
                    || by >= frame.height) {
                    continue;
                }
                const std::size_t a = (static_cast<std::size_t>(ay) * frame.width + ax) * 4;
                const std::size_t b = (static_cast<std::size_t>(by) * frame.width + bx) * 4;
                ++total;
                if (!near(frame.rgba[a], frame.rgba[b], 24)
                    || !near(frame.rgba[a + 1], frame.rgba[b + 1], 24)
                    || !near(frame.rgba[a + 2], frame.rgba[b + 2], 24)) {
                    ++changed;
                }
            }
        }
        return total > 0 ? static_cast<double>(changed) / static_cast<double>(total) : 0.0;
    }

    bool capture(IRenderBackend& backend, CapturedFrame& frame, const char* name)
    {
        if (!backend.captureFrame(frame) || frame.rgba.empty()) {
            check(false, std::string("capture_") + name);
            return false;
        }
        if (!outDir_.empty()) {
            std::error_code ignored;
            std::filesystem::create_directories(outDir_, ignored);
            writeBmpTopDown(std::filesystem::path(outDir_) / (std::string(name) + ".bmp"),
                            frame.width, frame.height, frame.rgba);
        }
        return true;
    }

    void classScreen(IRenderBackend& backend, ChargenVisuals& visuals)
    {
        CapturedFrame frame;
        if (!capture(backend, frame, "classsel")) {
            return;
        }
        for (int slot = 1; slot <= 6; ++slot) {
            const std::string tag = "3D_MODEL" + std::to_string(slot);
            const auto found = visuals.rects.find(tag);
            check(found != visuals.rects.end(), tag + "_rect");
            if (found == visuals.rects.end()) {
                continue;
            }
            const double share = drawnShare(frame, found->second);
            logInfo(concat("chargen_preview: ", tag, " drawn share ", share));
            check(share > 0.03, tag + "_drawn");
        }
        // BTN_SEL2 = male Scout and BTN_SEL3 = male Soldier have equal rectangles;
        // BTN_SEL3 and BTN_SEL4 = female Soldier differ by gender only.
        if (visuals.rects.count("3D_MODEL2") && visuals.rects.count("3D_MODEL3")
            && visuals.rects.count("3D_MODEL4")) {
            const double classes = crossDifference(frame, visuals.rects["3D_MODEL2"],
                                                   visuals.rects["3D_MODEL3"]);
            const double genders = crossDifference(frame, visuals.rects["3D_MODEL3"],
                                                   visuals.rects["3D_MODEL4"]);
            logInfo(concat("chargen_preview: scout vs soldier ", classes, " male vs female ",
                           genders));
            check(classes > 0.02, "classes_differ");
            check(genders > 0.02, "genders_differ");
        }
    }

    CapturedFrame portraitScreen(IRenderBackend& backend, ChargenVisuals& visuals,
                                 const char* name)
    {
        CapturedFrame frame;
        if (!capture(backend, frame, name)) {
            return frame;
        }
        const auto head = visuals.rects.find("LBL_HEAD");
        check(head != visuals.rects.end(), std::string(name) + "_head_rect");
        if (head != visuals.rects.end()) {
            const double share = drawnShare(frame, head->second);
            logInfo(concat("chargen_preview: ", name, " head drawn share ", share));
            check(share > 0.02, std::string(name) + "_head_drawn");
        }
        return frame;
    }

    void click(GuiSystem& gui, const std::string& tag)
    {
        clickTag_ = tag;
        clickState_ = 0;
        const kotor::GuiControl* control = findControl(gui.screen(), tag);
        if (control == nullptr) {
            check(false, "click_target_" + tag);
            clickState_ = 3;
            return;
        }
        clickX_ = static_cast<float>(control->extent.left + control->extent.width / 2);
        clickY_ = static_cast<float>(control->extent.top + control->extent.height / 2);
    }

    // pointer move, down, up on consecutive frames; true once the release was sent.
    bool clickStep(GuiSystem& gui)
    {
        switch (clickState_) {
        case 0:
            gui.pointerMove(clickX_, clickY_);
            break;
        case 1:
            gui.pointerDown(clickX_, clickY_);
            break;
        case 2:
            check(gui.pointerUp(clickX_, clickY_), "click_" + clickTag_);
            break;
        default:
            return true;
        }
        return ++clickState_ > 2;
    }

    static const kotor::GuiControl* findControl(const kotor::GuiScreen* screen,
                                                const std::string& tag)
    {
        if (screen == nullptr) {
            return nullptr;
        }
        std::function<const kotor::GuiControl*(const kotor::GuiControl&)> search =
            [&](const kotor::GuiControl& control) -> const kotor::GuiControl* {
            if (control.tag == tag) {
                return &control;
            }
            for (const kotor::GuiControl& child : control.children) {
                if (const kotor::GuiControl* hit = search(child)) {
                    return hit;
                }
            }
            return nullptr;
        };
        for (const kotor::GuiControl& control : screen->controls) {
            if (const kotor::GuiControl* hit = search(control)) {
                return hit;
            }
        }
        return nullptr;
    }

    std::string outDir_;
    bool fromMenu_ = false;
    bool nameTyped_ = false; // the name screen has been typed into with real SDL events
    std::size_t chainIndex_ = 0;
    Phase phase_ = Phase::ClassWait;
    int frames_ = 0;
    bool ok_ = true;
    std::string clickTag_;
    int clickState_ = 3;
    float clickX_ = 0.0f;
    float clickY_ = 0.0f;
    CapturedFrame first_;
    int firstIndex_ = 0;
};

// The PC portrait (portraits.2da art for the player's row and alignment) as a UI image;
// nothing is drawn when the sheet has no portrait row or the backend cannot show images.
void drawPcPortrait(GameSession& game, float size)
{
    if (gVisuals == nullptr) {
        return;
    }
    const CharacterSheet& sheet = game.character();
    if (sheet.portraitRow < 0) {
        return;
    }
    const PortraitInfo info = game.rules().portraitRow(sheet.portraitRow);
    const std::uint64_t texture = gVisuals->portraitTexture(
        game.rules().portraitResrefFor(info, sheet.alignment));
    if (texture != 0) {
        ImGui::Image(static_cast<ImTextureID>(texture), ImVec2(size, size));
    }
}

// keymap.2da rows 209..216 (retail 00621210 cases 0xd1..0xd8): the page's key opens it, switches to it
// from another page and closes it on its own page. Refused on other shipped screens (chargen, menus).
void pageKey(GameSession& game, GuiSystem* gui, GameScreens* screens, UiPanel panel)
{
    // The status summary holds the page keys until its OK (0062ef90 adds it with 0040bc70; paired
    // walk W19: retail's equip..map steps leave it up and open nothing).
    if (game.statusSummaryOpen()) {
        return;
    }
    if (gui != nullptr && gui->active() && !(screens != nullptr && screens->open())) {
        if (gui->screenName() != "optionsingame") {
            return;
        }
        gui->close();
    }
    game.toggleUiPanel(panel);
}

bool isPreviewTag(const std::string& tag)
{
    return tag == "LBL_HEAD" || tag == "LBL_3D" || tag == "LBL_3DCHAR" || tag == "MODEL_LBL" || tag == "3D_PlanetDisplay"
        || tag == "3D_PlanetModel"
        || (tag.size() == 9 && tag.rfind("3D_MODEL", 0) == 0 && tag[8] >= '1' && tag[8] <= '6');
}

SDL_WindowFlags windowFlagsFor(BackendKind kind, const Options& options)
{
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    switch (kind) {
    case BackendKind::Vulkan:
        flags |= SDL_WINDOW_VULKAN;
        break;
    case BackendKind::OpenGL:
        flags |= SDL_WINDOW_OPENGL;
        break;
    case BackendKind::Null:
    case BackendKind::Auto:
        break;
    }
    if (options.fullscreen && !options.hidden && !options.headless && kind != BackendKind::Null) {
        flags |= SDL_WINDOW_FULLSCREEN; // no mode set: borderless at the desktop size
    }
    if (options.hidden || options.headless || kind == BackendKind::Null) {
        flags |= SDL_WINDOW_HIDDEN;
    } else if (options.noActivate) {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        flags |= SDL_WINDOW_NOT_FOCUSABLE;
    } else if (options.startMinimized) {
        flags |= SDL_WINDOW_MINIMIZED;
    }
    return flags;
}

std::string buildLabel(BackendKind kind)
{
    switch (kind) {
    case BackendKind::Vulkan:
        return "Vulkan";
    case BackendKind::OpenGL:
        return "OpenGL";
    case BackendKind::Null:
        return "Null";
    case BackendKind::Auto:
        return "Auto";
    }
    return "Unknown";
}

int runImGuiSelfTest()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;

    // No renderer backend exists in the self-test, so build the legacy font
    // atlas manually and assign a dummy texture id. This exercises the same
    // NewFrame/Render path used by the GPU backends.
    unsigned char* pixels = nullptr;
    int textureWidth = 0;
    int textureHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &textureWidth, &textureHeight);
    if (pixels == nullptr || textureWidth <= 0 || textureHeight <= 0) {
        ke::logError("ImGui self-test FAILED: font atlas rasterization produced no pixels");
        ImGui::DestroyContext();
        return 7;
    }
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));
    ke::logInfo(concat("ImGui self-test: atlas ", textureWidth, "x", textureHeight, ", ",
                       io.Fonts->Fonts.Size, " font(s), built=", io.Fonts->IsBuilt() ? 1 : 0));

    ImGui::NewFrame();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(10.0f, 10.0f), ImVec2(210.0f, 110.0f),
                                                  IM_COL32(40, 120, 200, 255));
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f));
    ImGui::SetNextWindowSize(ImVec2(360.0f, 120.0f));
    const bool windowVisible = ImGui::Begin("Headless ImGui self-test");
    ImGui::Text("Dear ImGui %s", IMGUI_VERSION);
    ImGui::TextUnformatted("Context, font atlas and draw-list generation are functional.");
    ImGui::End();
    ke::logInfo(concat("ImGui self-test: window visible=", windowVisible ? 1 : 0));
    ImGui::Render();

    const ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData != nullptr) {
        ke::logInfo(concat("ImGui self-test: drawData valid=", drawData->Valid ? 1 : 0,
                           " cmdLists=", drawData->CmdListsCount,
                           " vtx=", drawData->TotalVtxCount,
                           " idx=", drawData->TotalIdxCount,
                           " display=", drawData->DisplaySize.x, "x", drawData->DisplaySize.y));
    }
    const bool ok = drawData != nullptr && drawData->Valid && drawData->CmdListsCount > 0
        && drawData->TotalVtxCount > 0;

    if (ok) {
        logInfo(concat("ImGui self-test PASSED (ImGui ", IMGUI_VERSION, ", ", textureWidth, "x",
                       textureHeight, " font atlas, ", drawData->CmdListsCount,
                       " draw list(s), ", drawData->TotalVtxCount, " vertices)"));
    } else {
        logError("ImGui self-test FAILED: no valid draw data was produced");
    }

    ImGui::DestroyContext();
    return ok ? 0 : 7;
}

void destroyRuntime(RuntimeState& state)
{
    gGuiPainter = nullptr;
    state.guiPainter.reset(); // releases its images while the backend is alive
    if (state.backend) {
        state.backend->shutdown();
        state.backend.reset();
    }
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::DestroyContext();
    }
    if (state.window != nullptr) {
        SDL_DestroyWindow(state.window);
        state.window = nullptr;
    }
}

void drawDebugUi(const RuntimeState& state, std::uint64_t frames, double frameMs,
                 double elapsedSeconds, const std::string& exitReason)
{
    static bool showDemo = false;

    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("KotOR Remaster - Engine Scaffold", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize);

    const BackendInfo& info = state.backend->info();
    ImGui::Text("Engine scaffold %s (Lane 08)", kEngineVersion);
    ImGui::Text("Backend: %s", info.name.c_str());
    ImGui::Text("API: %s", info.apiVersion.c_str());
    ImGui::Text("Device: %s", info.device.c_str());
    ImGui::Text("Swapchain: %s", info.swapchain.c_str());
    ImGui::Separator();
    ImGui::Text("Frame: %llu", static_cast<unsigned long long>(frames));
    ImGui::Text("CPU frame time: %.2f ms (%.1f FPS)", frameMs,
                frameMs > 0.0 ? 1000.0 / frameMs : 0.0);
    ImGui::Text("Elapsed: %.2f s", elapsedSeconds);
    ImGui::Text("Exit: %s", exitReason.c_str());
    ImGui::Separator();
    ImGui::Text("Dear ImGui %s", IMGUI_VERSION);
    ImGui::Text("Vulkan 1.4 primary / OpenGL 4.5 fallback");
    ImGui::Checkbox("Show ImGui demo window", &showDemo);
    ImGui::Text("Close the window or press Esc to exit.");
    ImGui::End();

    if (showDemo) {
        ImGui::ShowDemoWindow(&showDemo);
    }
}

void drawSystemsUi(GameSession& game)
{
    if (game.activePanel() == UiPanel::None) {
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(520.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 520.0f), ImGuiCond_FirstUseEver);
    const std::string title = std::string("Systems - ") + uiPanelName(game.activePanel());
    ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoCollapse);

    const CharacterSheet& sheet = game.character();
    switch (game.activePanel()) {
    case UiPanel::Character: {
        drawPcPortrait(game, 96.0f);
        ImGui::Text("%s  %s %d  (%s)", sheet.name.c_str(),
                    game.rules().className(sheet.classId).c_str(), sheet.level,
                    sheet.gender == 1 ? "female" : "male");
        ImGui::Text("XP %d   HP %d/%d   FP %d/%d   Gold %d", sheet.xp, sheet.hp,
                    sheet.maxHp, sheet.fp, sheet.maxFp, game.gold());
        ImGui::Text("Alignment %d (0 dark .. 100 light)   Combat mode %s   Effects %zu",
                    sheet.alignment, combatModeName(game.combatMode()), sheet.effects.size());
        if (game.canStealth()) {
            bool stealth = game.stealthActive();
            if (ImGui::Checkbox("Stealth (Force Camouflage)", &stealth)) {
                if (stealth) {
                    game.enterStealth();
                } else {
                    game.leaveStealth();
                }
            }
        }
        ImGui::Separator();
        ImGui::Text("STR %d  DEX %d  CON %d", sheet.str, sheet.dex, sheet.con);
        ImGui::Text("INT %d  WIS %d  CHA %d", sheet.intel, sheet.wis, sheet.cha);
        ImGui::Text("AC %d  Attack +%d  Skill points %d  Feat points %d  Attribute points %d",
                    game.rules().armorClass(sheet), game.rules().attackBonus(sheet),
                    sheet.skillPoints, sheet.featPoints, sheet.attributePoints);
        if (sheet.attributePoints > 0) {
            static const char* attributes[] = {"STR", "DEX", "CON", "INT", "WIS", "CHA"};
            for (int i = 0; i < 6; ++i) {
                ImGui::SameLine();
                if (ImGui::SmallButton((std::string("+") + attributes[i]).c_str())) {
                    game.spendAttributePoint(i);
                }
            }
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Skills");
        for (const auto& [name, rank] : sheet.skills) {
            ImGui::Text("%s: %d", name.c_str(), rank);
            if (sheet.skillPoints > 0) {
                ImGui::SameLine();
                if (ImGui::SmallButton((std::string("+##") + name).c_str())) {
                    game.spendSkillPoint(name);
                }
            }
        }
        if (!sheet.feats.empty()) {
            ImGui::TextUnformatted("Feats");
            for (const std::int32_t feat : sheet.feats) {
                ImGui::Text("#%d %s", feat, game.rules().featName(feat).c_str());
            }
        }
        static int featId = 3;
        ImGui::InputInt("Feat ID", &featId);
        ImGui::SameLine();
        if (ImGui::Button("Learn Feat")) {
            game.learnFeat(featId);
        }
        if (!sheet.powers.empty()) {
            ImGui::TextUnformatted("Force powers");
            const std::string targetTag = game.combatTargetName();
            for (const std::int32_t power : sheet.powers) {
                ImGui::Text("#%d %s", power, game.rules().powerName(power).c_str());
                ImGui::SameLine();
                if (ImGui::Button((std::string("Use##") + std::to_string(power)).c_str())) {
                    const std::uint32_t target = targetTag.empty()
                        ? 0 : game.objectForTag(targetTag);
                    game.useForcePower(power, target);
                }
            }
        }
        static int powerId = 6;
        ImGui::InputInt("Power ID", &powerId);
        ImGui::SameLine();
        if (ImGui::Button("Learn Power")) {
            game.learnPower(powerId);
        }
        break;
    }
    case UiPanel::Inventory:
        ImGui::Text("Gold: %d   Items: %zu", game.gold(), game.inventory().size());
        ImGui::Separator();
        for (std::size_t i = 0; i < game.inventory().size(); ++i) {
            const InventoryItem& item = game.inventory()[i];
            const std::string label = item.name.empty() ? item.resref : item.name;
            ImGui::Text("%s  cost=%d  x%d%s", label.c_str(), item.cost, item.stack,
                        item.equipped ? "  [equipped]" : "");
            ImGui::SameLine();
            if (ImGui::Button((std::string("Equip##") + std::to_string(i)).c_str())) {
                game.equipItem(item.resref);
            }
        }
        break;
    case UiPanel::Party: // the shipped party selection screen (GameScreens::update)
        break;
    case UiPanel::Journal:
        ImGui::Text("Journal entries: %zu", game.journal().size());
        ImGui::Separator();
        for (const auto& [plot, state] : game.journal()) {
            ImGui::Text("%s = %d", plot.c_str(), state);
        }
        break;
    case UiPanel::Merchant: // the shipped store screen (GameScreens::update)
        break;
    case UiPanel::Loot:
        ImGui::Text("Loot (%zu)", game.loot().size());
        ImGui::Separator();
        for (std::size_t i = 0; i < game.loot().size(); ++i) {
            const InventoryItem& item = game.loot()[i];
            ImGui::Text("%s  cost=%d  x%d", (item.name.empty() ? item.resref : item.name).c_str(),
                        item.cost, item.stack);
            ImGui::SameLine();
            if (ImGui::Button((std::string("Take##") + std::to_string(i)).c_str())) {
                game.takeLootItem(i);
            }
        }
        if (ImGui::Button("Take All")) {
            game.takeAllLoot();
        }
        break;
    case UiPanel::None:
        break;
    }
    ImGui::End();
}

// A movie owns the whole window: black everywhere, the picture scaled to fit and centred
// (letterboxed or pillarboxed by its own aspect). It goes on the foreground list so no
// panel or fade overlay can sit on top of it, and the bars never show the world behind.
void drawMovieOverlay(const kotor::BinkVideoFrame& frame, std::uint64_t texture)
{
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(0, 0, 0, 255));
    const kotor::MovieRect fit = kotor::fitMovie(frame.width, frame.height, io.DisplaySize.x,
                                                 io.DisplaySize.y);
    draw->AddImage(static_cast<ImTextureID>(texture), ImVec2(fit.x, fit.y),
                   ImVec2(fit.x + fit.w, fit.y + fit.h));
}

void drawFacialRigOverlay(const FacialRig& rig)
{
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 360.0f, 60.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340.0f, 80.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Lip sync rig")) {
        ImGui::End();
        return;
    }
    ImGui::Text("viseme %d: %s", rig.viseme(), rig.visemeName(rig.viseme()).c_str());
    ImGui::Text("talk clip at %.1f%% of its length", FacialRig::clipFraction(rig.viseme()) * 100.0f);
    ImGui::End();
}

// The loading screen (shipped loadscreen.gui): the image comes from loadscreens.2da row
// ARE LoadScreenID of the destination area, the hint from loadscreenhints.2da, the music
// is mus_loadscreen. The load blocks the frame loop, so two frames are presented before
// it starts. The bar shows only that loading began and is halfway: the load has no
// measurable progress. Which hint is picked is this engine's choice (not recovered).
void presentLoadScreen(RuntimeState& state, GameSession& game, GuiSystem& gui, const std::string& next)
{
    if (!state.imguiReady || state.backend == nullptr || gGuiPainter == nullptr || !state.guiInstall) {
        return;
    }
    std::int32_t screenId = 0;
    try {
        kotor::Install peek(game.installPath(), false);
        screenId = kotor::readLoadScreenId(peek, next);
    } catch (const std::exception&) {
        screenId = -1;
    }
    GuiPaintOptions options;
    options.opaqueBackdrop = true;
    options.backdropKind = "load"; // 0067a710: the load screen names "<W>x<H>load"
    std::string hint;
    if (auto data = state.guiInstall->find("loadscreens", 2017)) {
        const kotor::TwoDa table = kotor::TwoDa::parse(std::move(*data), "loadscreens");
        const int row = screenId >= 0 && static_cast<std::size_t>(screenId) < table.rowCount() ? screenId : 0;
        const int column = table.columnIndex("bmpresref");
        if (column >= 0) {
            options.panelFill = table.cell(static_cast<std::size_t>(row), static_cast<std::size_t>(column));
        }
    }
    if (auto data = state.guiInstall->find("loadscreenhints", 2017)) {
        const kotor::TwoDa table = kotor::TwoDa::parse(std::move(*data), "loadscreenhints");
        if (table.rowCount() > 0) {
            const std::size_t hash = std::hash<std::string>{}(next);
            const int column = table.columnIndex((hash & 1u) != 0 ? "storyhint" : "gameplayhint");
            if (column >= 0) {
                const std::string ref = table.cell((hash >> 1) % table.rowCount(), static_cast<std::size_t>(column));
                if (!ref.empty() && ref != "****") {
                    hint = game.localization().tlk().text(static_cast<std::uint32_t>(std::strtoul(ref.c_str(), nullptr, 10)));
                }
            }
        }
    }
    if (!gui.open("loadscreen")) {
        return;
    }
    gui.setText("LBL_HINT", hint);
    if (gLoadProbe != nullptr) {
        CapturedFrame warm;
        state.backend->captureFrame(warm); // GL arms its pre-swap read here
    }
    game.audio().playLoadScreenMusic();
    logInfo(concat("loading screen: module ", next, " LoadScreenID ", screenId, " image ", options.panelFill));
    for (int step = 0; step < 2; ++step) {
        gui.setValue("PB_PROGRESS", step == 0 ? 25.0f : 75.0f);
        state.backend->newFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImGuiIO& io = ImGui::GetIO();
        options.timeSeconds = ImGui::GetTime();
        gGuiPainter->paint(gui, ImGui::GetBackgroundDrawList(), io.DisplaySize.x, io.DisplaySize.y, options);
        ImGui::Render();
        std::string error;
        state.backend->renderFrame(error);
        if (gLoadProbe != nullptr && step == 1) {
            gLoadProbe->module = next;
            gLoadProbe->image = options.panelFill;
            gLoadProbe->hint = hint;
            gLoadProbe->stats = gGuiPainter->stats();
            state.backend->captureFrame(gLoadProbe->frame);
            ++gLoadProbe->shown;
        }
    }
    gui.close();
}

// The map panel's drawing: the area's lbl_map<area> art one texel per design pixel from the
// control's top left, clipped to the control (440 x 256 in the file, the 512 x 256 art loses its
// right edge; retail 00694d50 builds the map object at 512 x 256 inside the 440 x 256 view), with
// the leader's arrow (mm_barrow, 32 x 32 in 00693f60) turned to the facing at the leader's place.
// The world-to-texel mapping is Minimap::project (fitted, see Formats.h). The turn direction is retail's:
// 00578ed0's formula against the projection points art-up along the facing in all 91 mapped areas checked.
// All 117 shipped AREs have MapZoom 1, so one texel per design pixel is retail's zoom as well.
// Retail 00688100 (minimap) and 006943d0 (map screen): each unseen cell's corner gets "blackdot" (0068c100 at +0xc8c8,
// 00693f60 at +0x5c), centred, four cells wide (0073f404) and tall. Its soft alpha and blue cross make the retail grid.
void drawMapFog(GameSession& game, ImDrawList* draw, float left, float top, float scale)
{
    const MapFog* fog = game.mapFog();
    if (fog == nullptr) {
        return;
    }
    const float cellW = kotor::Scene::Minimap::kViewWidth / static_cast<float>(fog->cols) * scale;
    const float cellH = kotor::Scene::Minimap::kViewHeight / static_cast<float>(fog->rows) * scale;
    const GuiTexture* dot = gGuiPainter->texture("blackdot");
    if (dot == nullptr) {
        return;
    }
    for (int row = 0; row <= fog->rows; ++row) {
        for (int col = 0; col <= fog->cols; ++col) {
            if (fog->seen[static_cast<std::size_t>(row) * static_cast<std::size_t>(fog->cols + 1) + static_cast<std::size_t>(col)] == 0) {
                const ImVec2 low(left + static_cast<float>(col) * cellW - cellW * 2.0f, top + static_cast<float>(row) * cellH - cellH * 2.0f);
                draw->AddImage(static_cast<ImTextureID>(dot->id), low, ImVec2(low.x + cellW * 4.0f, low.y + cellH * 4.0f));
            }
        }
    }
}

void drawAreaMap(GameSession& game, GameScreens* screens, float x0, float y0, float x1, float y1)
{
    const GuiTexture* art = gGuiPainter->texture("lbl_map" + game.scene().area);
    if (art == nullptr || art->width <= 0 || art->height <= 0) {
        return;
    }
    const float s = (x1 - x0) / kotor::Scene::Minimap::kViewWidth; // pixels per design pixel
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    draw->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    draw->AddImage(static_cast<ImTextureID>(art->id), ImVec2(x0, y0),
                   ImVec2(x0 + static_cast<float>(art->width) * s, y0 + static_cast<float>(art->height) * s));
    drawMapFog(game, draw, x0, y0, s);
    float px0 = 0.0f;
    float py0 = 0.0f;
    float px1 = 0.0f;
    float py1 = 0.0f;
    // Retail 00692c70: whitetarget, 14x14 in (0, 0.66, 0.98) and clickable, the selected one 20x20 in
    // (0.98, 1, 0). The hit test 00693300 is that rectangle.
    if (const GuiTexture* target = gGuiPainter->texture("whitetarget"); target != nullptr && screens != nullptr) {
        const std::vector<std::size_t> pins = game.mapPins();
        for (std::size_t i = 0; i < pins.size(); ++i) {
            const kotor::Vec3 where = game.scene().entities[pins[i]].position;
            if (!game.scene().minimap.project(where.x, where.y, px0, py0)) {
                continue;
            }
            const bool picked = static_cast<int>(i) == screens->mapPinSelected();
            const float half = (picked ? 10.0f : 7.0f) * s;
            const ImVec2 low(x0 + px0 * s - half, y0 + py0 * s - half);
            const ImVec2 high(x0 + px0 * s + half, y0 + py0 * s + half);
            draw->AddImage(static_cast<ImTextureID>(target->id), low, high, ImVec2(0, 0), ImVec2(1, 1),
                           picked ? IM_COL32(250, 255, 0, 255) : IM_COL32(0, 168, 250, 255));
            if (!picked && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(low, high, false)) {
                screens->selectMapPin(static_cast<int>(i));
            }
        }
    }
    // Retail 00693f60: the party's others, lbl_mapcircle at 16x16. Tint inferred: its 007a23f0 is filled at run time.
    if (const GuiTexture* circle = gGuiPainter->texture("lbl_mapcircle")) {
        for (const std::uint32_t member : game.scriptWorld().party) {
            const std::size_t index = ScriptApi{game}.entityFor(member);
            if (index >= game.scene().entities.size()) {
                continue;
            }
            const kotor::Vec3 where = game.scene().entities[index].position;
            if (game.scene().minimap.project(where.x, where.y, px0, py0)) {
                const ImVec2 low(x0 + (px0 - 8.0f) * s, y0 + (py0 - 8.0f) * s);
                draw->AddImage(static_cast<ImTextureID>(circle->id), low, ImVec2(low.x + 16.0f * s, low.y + 16.0f * s),
                               ImVec2(0, 0), ImVec2(1, 1), IM_COL32(0, 255, 255, 255));
            }
        }
    }
    const kotor::Vec3 at = game.position();
    const float yaw = game.yaw();
    const GuiTexture* arrow = gGuiPainter->texture("mm_barrow");
    if (arrow != nullptr && game.scene().minimap.project(at.x, at.y, px0, py0)
        && game.scene().minimap.project(at.x + std::cos(yaw), at.y + std::sin(yaw), px1, py1)) {
        const float turn = std::atan2(py1 - py0, px1 - px0) + 1.5707963f;
        const float cx = x0 + px0 * s;
        const float cy = y0 + py0 * s;
        const float half = 16.0f * s;
        const float c = std::cos(turn);
        const float sn = std::sin(turn);
        const auto corner = [&](float lx, float ly) { return ImVec2(cx + lx * c - ly * sn, cy + lx * sn + ly * c); };
        draw->AddImageQuad(static_cast<ImTextureID>(arrow->id), corner(-half, -half), corner(half, -half),
                           corner(half, half), corner(-half, half), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1),
                           ImVec2(0, 1), IM_COL32_WHITE);
    }
    draw->PopClipRect();
}

void drawGuiScreenOverlay(GuiSystem& gui, GameSession& game, bool& requestChargen,
                          const std::function<bool(const std::string&)>& actionHandler = {},
                          ChargenVisuals* visuals = nullptr)
{
    const kotor::GuiScreen* screen = gui.screen();
    if (screen == nullptr || gGuiPainter == nullptr) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (visuals != nullptr && visuals->frontEnd != nullptr) {
        visuals->frontEnd->update(io.DeltaTime);
    }
    const GuiView view = gGuiPainter->layout(*screen, io.DisplaySize.x, io.DisplaySize.y);
    pumpGuiInput(gui, view);

    // 3D model controls (classsel 3D_MODELn, portcust LBL_HEAD) are drawn by the backend
    // underneath the UI: their rectangles stay open in the backdrop and the controls draw
    // nothing themselves.
    const bool previewScreen = visuals != nullptr && visuals->preview != nullptr;
    GuiPaintOptions options;
    options.timeSeconds = ImGui::GetTime();
    // An in-game screen is drawn over the world (a container is a popup over it).
    options.opaqueBackdrop = !(visuals != nullptr && visuals->gameScreens != nullptr && visuals->gameScreens->open());
    // The surround art each panel class names (0040a900 / its table at 0073ad80): the galaxy map
    // 00695180, the store 006c1c00 and both Pazaak screens 006808a0 / 00681a90 set their own.
    {
        const std::string& name = gui.screenName();
        if (name == "galaxymap") {
            options.backdropKind = "map";
        } else if (name == "store") {
            options.backdropKind = "store";
        } else if (name == "pazaakgame" || name == "pazaaksetup" || name == "pazaakwager" || name == "pazaak_notice"
                   || name == "pazaak_ask" || name == "pazaak_help") {
            // The Pazaak message boxes are modals over the table: its surround stays (req_pazaak_set_result).
            // A box is no whole-window panel, so the surround is asked for outright.
            options.backdropKind = "pazaak";
            options.opaqueBackdrop = true;
        }
    }
    if (previewScreen) {
        visuals->rects.clear();
    }
    // The main menu's model scene fills the whole window behind the panel.
    const bool menuScreen = visuals != nullptr && visuals->menu != nullptr && visuals->menu->ready()
        && visuals->backend != nullptr && visuals->backend->supportsPreview()
        && gui.screenName().rfind("mainmenu", 0) == 0;
    std::vector<PreviewView> menuViews;
    if (menuScreen) {
        if (!visuals->menuStaged) {
            std::string stageError;
            visuals->backend->setScene(nullptr);
            if (visuals->backend->setPreviewScene(&visuals->menu->scene(), stageError)) {
                visuals->menuStaged = true;
            } else {
                logWarn(concat("main menu scene upload failed: ", stageError));
            }
        }
        if (visuals->menuStaged) {
            options.skipPanelFill = true;
            // The model view is the panel's design rectangle (800x600): the art in the model
            // lines up with the GUI controls only at that aspect, the rest stays black.
            options.holes.push_back({std::round(view.pxX(0.0f)), std::round(view.pxY(0.0f)),
                                     std::round(view.pxX(view.baseWidth)), std::round(view.pxY(view.baseHeight))});
            if (visuals->menu->advance(io.DeltaTime)) {
                visuals->backend->refreshPreviewMeshes();
            }
        }
    } else if (visuals != nullptr && visuals->menuStaged) {
        visuals->menuStaged = false;
        visuals->uploadedRevision = UINT32_MAX; // the chargen stage uploads its own scene again
        visuals->backend->setPreviewViews({});
    }
    options.controlHook = [&](const kotor::GuiControl& control, float x0, float y0, float x1, float y1) {
        if (menuScreen && visuals->menuStaged && control.tag == "LBL_3DVIEW") {
            const ImVec2 fb = io.DisplayFramebufferScale;
            menuViews.push_back(visuals->menu->view(static_cast<int>(std::lround(x0 * fb.x)),
                                                    static_cast<int>(std::lround(y0 * fb.y)),
                                                    static_cast<int>(std::lround((x1 - x0) * fb.x)),
                                                    static_cast<int>(std::lround((y1 - y0) * fb.y))));
            return true;
        }
        const bool modelControl = previewScreen && isPreviewTag(control.tag)
            && visuals->preview->entityForTag(control.tag) >= 0;
        if (modelControl) {
            const ImVec2 fb = io.DisplayFramebufferScale;
            PreviewRect rect;
            rect.tag = control.tag;
            rect.x = static_cast<int>(std::lround(std::round(x0) * fb.x));
            rect.y = static_cast<int>(std::lround(std::round(y0) * fb.y));
            rect.width = static_cast<int>(std::lround(std::round(x1) * fb.x)) - rect.x;
            rect.height = static_cast<int>(std::lround(std::round(y1) * fb.y)) - rect.y;
            visuals->rects[control.tag] = rect;
            return true;
        }
        if (visuals != nullptr && visuals->gameScreens != nullptr && control.tag == "LBL_Map"
            && gui.screenName() == "map") {
            drawAreaMap(game, visuals->gameScreens, x0, y0, x1, y1);
            return true;
        }
        if (visuals != nullptr && control.tag == "LBL_PORTRAIT") {
            const std::uint64_t texture = visuals->portraitTexture(visuals->portraitResref);
            if (texture != 0) {
                ImGui::GetBackgroundDrawList()->AddImage(static_cast<ImTextureID>(texture),
                                                         ImVec2(x0, y0), ImVec2(x1, y1));
                return true;
            }
        }
        return false;
    };
    // The holes must be known before the backdrop: collect them with a dry pass.
    if (previewScreen) {
        const GuiView* at = &view;
        std::function<void(const kotor::GuiControl&)> collect = [&](const kotor::GuiControl& control) {
            if (isPreviewTag(control.tag) && visuals->preview->entityForTag(control.tag) >= 0) {
                // Whole pixels: the art around a hole is clipped to bands, and a scissor at a fractional
                // edge loses a row (the world shows through it), and the view below must end at the same row.
                options.holes.push_back({std::round(at->pxX(static_cast<float>(control.extent.left))),
                                         std::round(at->pxY(static_cast<float>(control.extent.top))),
                                         std::round(at->pxX(static_cast<float>(control.extent.left + control.extent.width))),
                                         std::round(at->pxY(static_cast<float>(control.extent.top + control.extent.height)))});
            }
            for (const kotor::GuiControl& child : control.children) {
                collect(child);
            }
        };
        for (const kotor::GuiControl& control : screen->controls) {
            collect(control);
        }
        // A popup over the sheet (the level-up's granted feats, req_levelup_granted) keeps the sheet's model
        // showing below it: the screens under the popup keep their model holes, in their own layout.
        for (std::size_t layer = 0; layer < gui.underlayCount(); ++layer) {
            if (const kotor::GuiScreen* below = gui.underlayScreen(layer)) {
                const GuiView belowView = gGuiPainter->layout(*below, io.DisplaySize.x, io.DisplaySize.y);
                at = &belowView;
                for (const kotor::GuiControl& control : below->controls) {
                    collect(control);
                }
                at = &view;
            }
        }
    }
    const std::size_t holeCount = options.holes.size();
    gGuiPainter->paint(gui, draw, io.DisplaySize.x, io.DisplaySize.y, options);
    (void)holeCount;
    if (menuScreen && visuals->menuStaged) {
        visuals->backend->setPreviewViews(menuViews);
    }
    if (previewScreen && visuals->backend != nullptr) {
        std::vector<PreviewRect> rectList;
        for (const auto& entry : visuals->rects) {
            rectList.push_back(entry.second);
        }
        visuals->backend->setPreviewViews(visuals->preview->views(rectList));
    }
    if (!gui.lastAction().empty()) {
        const std::string action = gui.lastAction();
        gui.clearActions();
        if (gui.screenName() == "optionsingame"
            && (GameScreens::tabPanel(action) != UiPanel::None || action == "BTN_OPT")) {
            // The pause menu's own page tabs: another page leaves it, its own does nothing.
            if (const UiPanel page = GameScreens::tabPanel(action); page != UiPanel::None) {
                gui.close();
                game.setUiPanel(page);
            }
        } else if (actionHandler && actionHandler(action)) {
            // The chargen flow consumed the control.
        } else if (visuals != nullptr && visuals->gameScreens != nullptr && visuals->gameScreens->action(action)) {
            // An in-game shipped screen consumed the control.
        } else if (visuals != nullptr && visuals->frontEnd != nullptr && visuals->frontEnd->action(action)) {
            // The shipped front-end screen consumed the control.
        } else if (!game.applyGuiAction(action)) {
            if (action == "BTN_NEWGAME") {
                gui.close();
                requestChargen = true;
            }
        }
    }
}

void drawChargenUi(Chargen& chargen, GameSession& game, bool& active)
{
    ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(620.0f, 500.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Character Generation", nullptr, ImGuiWindowFlags_NoCollapse);
    ImGui::TextWrapped("Build a level-1 character. The same state machine is used by --chargen-test.");
    ImGui::Separator();
    const Rules& rules = game.rules();
    switch (chargen.step()) {
    case Chargen::Step::Class:
        ImGui::TextUnformatted("Choose a class");
        for (std::int32_t id = 0; id < chargen.classCount(); ++id) {
            if (ImGui::Selectable(rules.className(id).c_str(), chargen.classId() == id)) {
                chargen.selectClass(id);
            }
        }
        ImGui::Text("Class: %s", rules.className(chargen.classId()).c_str());
        break;
    case Chargen::Step::Abilities: {
        ImGui::Text("Points remaining: %d", chargen.pointsRemaining());
        static const char* labels[] = {"STR", "DEX", "CON", "INT", "WIS", "CHA"};
        for (int i = 0; i < 6; ++i) {
            ImGui::Text("%s %d", labels[i], chargen.attribute(i));
            ImGui::SameLine();
            if (ImGui::SmallButton((std::string(" -##a") + std::to_string(i)).c_str())) {
                chargen.adjustAttribute(i, -1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton((std::string(" +##a") + std::to_string(i)).c_str())) {
                chargen.adjustAttribute(i, 1);
            }
        }
        break;
    }
    case Chargen::Step::Skills: {
        ImGui::Text("Skill points remaining: %d", chargen.skillPointsRemaining());
        ImGui::BeginChild("skills", ImVec2(0.0f, 340.0f), true);
        for (std::int32_t i = 0; i < rules.skillCount(); ++i) {
            ImGui::Text("%-14s %d%s", rules.skillName(i).c_str(), chargen.skillRank(i),
                        rules.skillIsClassSkill(chargen.classId(), i) ? " *" : "");
            ImGui::SameLine();
            if (ImGui::SmallButton((std::string(" -##s") + std::to_string(i)).c_str())) {
                chargen.adjustSkill(i, -1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton((std::string(" +##s") + std::to_string(i)).c_str())) {
                chargen.adjustSkill(i, 1);
            }
        }
        ImGui::EndChild();
        break;
    }
    case Chargen::Step::Feats: {
        ImGui::Text("Feat points remaining: %d", chargen.featPointsRemaining());
        ImGui::BeginChild("feats", ImVec2(0.0f, 340.0f), true);
        for (const std::int32_t feat : chargen.availableFeats()) {
            const bool selected = chargen.featSelected(feat);
            if (ImGui::Selectable((std::to_string(feat) + " " + rules.featName(feat)).c_str(),
                                  selected)) {
                chargen.toggleFeat(feat);
            }
        }
        ImGui::EndChild();
        break;
    }
    case Chargen::Step::Portrait: {
        const PortraitInfo portrait = chargen.portrait();
        ImGui::Text("Portrait %d: %s", chargen.portraitIndex(), portrait.resref.c_str());
        ImGui::Text("Appearance row: %u", portrait.appearance);
        if (ImGui::Button("Previous")) {
            chargen.cyclePortrait(-1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Next")) {
            chargen.cyclePortrait(1);
        }
        break;
    }
    case Chargen::Step::Name: {
        static char nameBuffer[32] = "Player";
        if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer))) {
            chargen.setName(nameBuffer);
        }
        ImGui::Text("Name: %s", chargen.name().c_str());
        break;
    }
    case Chargen::Step::Done: {
        const CharacterSheet sheet = chargen.build();
        ImGui::Text("Class: %s", rules.className(sheet.classId).c_str());
        ImGui::Text("Name: %s", sheet.name.c_str());
        ImGui::Text("HP: %d  FP: %d", sheet.maxHp, sheet.maxFp);
        ImGui::Text("STR %d DEX %d CON %d INT %d WIS %d CHA %d",
                    sheet.str, sheet.dex, sheet.con, sheet.intel, sheet.wis, sheet.cha);
        if (ImGui::Button("Start Game")) {
            game.applyCharacter(sheet);
            game.setPlayerAppearance(static_cast<std::uint32_t>(std::max(0, chargen.appearanceId())),
                                     chargen.name());
            game.setThirdPerson(true);
            active = false;
        }
        break;
    }
    }
    ImGui::Separator();
    if (chargen.step() != Chargen::Step::Done) {
        if (ImGui::Button("Next")) {
            chargen.next();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Back")) {
        chargen.back();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        active = false;
    }
    ImGui::End();
}

// Script-driven fade (SetGlobalFadeOut/In) over the whole screen, and the
// galaxy map panel ShowGalaxyMap opens. Picking a planet runs the map script.
void drawWorldStateOverlay(GameSession& game)
{
    const ImGuiIO& io = ImGui::GetIO();
    const FadeState& fade = game.fade();
    if (fade.opacity > 0.0f && io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
        const auto channel = [](float value) {
            return static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
        };
        ImGui::GetForegroundDrawList()->AddRectFilled(
            ImVec2(0.0f, 0.0f), io.DisplaySize,
            IM_COL32(channel(fade.color[0]), channel(fade.color[1]), channel(fade.color[2]),
                     channel(fade.opacity)));
    }
    if (!game.galaxyMapOpen()) {
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f - 160.0f, 80.0f), ImGuiCond_Appearing);
    ImGui::Begin("Galaxy Map", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    const GalaxyMapState& map = game.galaxyMap();
    for (const auto& [planet, name] : game.galaxyPlanetNames()) {
        const auto slot = static_cast<std::size_t>(planet);
        if (!map.available[slot]) {
            continue;
        }
        const std::string label = name + (planet == map.highlighted ? "  (current)" : "");
        ImGui::BeginDisabled(!map.selectable[slot]);
        if (ImGui::Button(label.c_str(), ImVec2(300.0f, 0.0f))) {
            game.selectPlanet(planet);
        }
        ImGui::EndDisabled();
    }
    if (ImGui::Button("Close", ImVec2(300.0f, 0.0f))) {
        game.closeGalaxyMap();
    }
    ImGui::End();
}

void drawSlicePanel(GameSession& game, float fps, const std::string& prompt)
{
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(470.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("KotOR Remaster - Playable Slice", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    drawPcPortrait(game, 48.0f);
    ImGui::Text("Module: %s  Area: %s", game.module().c_str(), game.scene().area.c_str());
    const kotor::Vec3 steeredAt = game.controlledPosition();
    ImGui::Text("Position: %.2f %.2f %.2f  %s", steeredAt.x, steeredAt.y, steeredAt.z,
                game.grounded() ? "grounded" : "airborne");
    ImGui::Text("FPS: %.1f", fps);
    // The party strip: one button per creature the player may take control of
    // (Tab cycles the same list). The highlighted one is steered.
    for (const int npc : game.selectableLeaders()) {
        const bool steered = npc == game.partyTable().controlled;
        const std::string label = npc == kNpcPlayer
            ? std::string("PC")
            : game.partyTable().npcs[static_cast<std::size_t>(npc)].tag;
        if (steered) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.75f, 1.0f));
        }
        if (ImGui::Button((label + "##leader" + std::to_string(npc)).c_str())) {
            game.selectLeaderNpc(npc);
        }
        if (steered) {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Text("HP: %d / %d", game.controlledHp(), game.controlledMaxHp());
    ImGui::ProgressBar(static_cast<float>(game.controlledHp())
                           / static_cast<float>(std::max(1, game.controlledMaxHp())),
                       ImVec2(-1.0f, 0.0f));
    if (game.combatTarget() != UINT32_MAX) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "Target: %s", game.combatTargetName().c_str());
    }
    if (!prompt.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "%s", prompt.c_str());
    }
    ImGui::Separator();
    ImGui::TextUnformatted("WASD move, mouse look, Shift run, Space jump");
    ImGui::TextUnformatted("E interact, F attack, Tab next party member, P pazaak");
    ImGui::TextUnformatted("F5 save, F9 load, F1 mouse, Esc quit");
    ImGui::Text("Triangles: %zu  Collision facets: %zu  Entities: %zu",
                game.scene().indices.size() / 3, game.scene().collision.size(), game.scene().entities.size());
    if (!game.combatLog().empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted("Recent events:");
        const std::size_t start = game.combatLog().size() > 6 ? game.combatLog().size() - 6 : 0;
        for (std::size_t i = start; i < game.combatLog().size(); ++i) {
            ImGui::TextWrapped("%s", game.combatLog()[i].c_str());
        }
    }
    ImGui::End();
}

void drawGameUi(GameSession& game, float fps, const std::string& prompt, bool developerPanel)
{
    // A minigame's HUD is its shipped models: no slice panel or walking crosshair over it.
    const bool miniGame = game.miniGameActive();
    if (!miniGame && developerPanel) {
        drawSlicePanel(game, fps, prompt);
    }

    if (game.dialogue().active && developerPanel) {
        ImGui::SetNextWindowPos(ImVec2(16.0f, 390.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(760.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Dialogue", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextColored(ImVec4(0.95f, 0.86f, 0.55f, 1.0f), "%s", game.dialogue().speaker.c_str());
        ImGui::TextWrapped("%s", game.dialogue().text.c_str());
        ImGui::Separator();
        int chosen = -1;
        for (std::size_t i = 0; i < game.dialogue().options.size(); ++i) {
            const std::string label = std::to_string(i + 1) + ". " + game.dialogue().options[i].text;
            if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f))) {
                chosen = static_cast<int>(i);
            }
        }
        if (chosen >= 0) {
            game.chooseDialogueOption(static_cast<std::size_t>(chosen));
        }
        ImGui::End();
    }

    drawSystemsUi(game);

    const ImGuiIO& io = ImGui::GetIO();
    // The walking crosshair belongs to the developer view; the shipped HUD has none.
    if (!miniGame && developerPanel && io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        const ImVec2 centre(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        draw->AddLine(ImVec2(centre.x - 6.0f, centre.y), ImVec2(centre.x + 6.0f, centre.y), IM_COL32(235, 235, 235, 160));
        draw->AddLine(ImVec2(centre.x, centre.y - 6.0f), ImVec2(centre.x, centre.y + 6.0f), IM_COL32(235, 235, 235, 160));
    }
}

std::size_t firstHostile(const GameSession& game)
{
    for (std::size_t i = 0; i < game.scene().entities.size(); ++i) {
        const kotor::Entity& entity = game.scene().entities[i];
        if (entity.hostile && !entity.dead) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

bool hasEvent(const GameSession& game, const std::string& needle)
{
    for (const std::string& line : game.combatLog()) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Shipped timers and triggers open real conversations during a long run, and a
// conversation holds the player until it is answered. Abort rather than answer,
// so the journal and globals a later assertion reads are not advanced.
bool dismissDialogue(GameSession& game)
{
    if (!game.dialogue().active) {
        return false;
    }
    game.endDialogue();
    return true;
}

// One combat frame. A conversation interrupts the attack, so it is reissued.
void updateFighting(GameSession& game)
{
    if (dismissDialogue(game)) {
        game.attackNearestHostile();
    }
    game.update(1.0f / 60.0f, InputState{});
}

// Dialogue camera gate (--dialogue-test). Three measures on shipped data through the real
// camera paths; each reports a number and fails below a stated bound.
//  1. GIT cameras: the fraction of the module's cameras with a creature, placeable or door
//     within 20 degrees of the forward axis (25 m range). Measured over all 1,120 shipped
//     cameras of the install with the W-first quaternion: 42% (median best angle 17.5), against
//     3% when the quaternion is read X-first; in end_m01aa 33 of 40 against 3 of 40.
//  2. Static-camera dialogue lines (CameraAngle 6) of the module whose speaker stands within
//     25 m of the camera: the speaker lies inside the view frustum (16:9). Over the whole
//     install 1,443 of 1,744 resolvable lines are in view (median 8.9 degrees off axis), 8
//     with the old reading. Lines whose speaker is staged elsewhere by the cutscene script
//     (camera and speaker 80+ m apart in the GIT) cannot be judged and are counted apart.
//  3. Camera-model hooks (CameraAngle 4): every CUTnnnW clip of every shipped *_cam model
//     (236 clips). The hook looks along local -Z with local +Y up: forward is level
//     (|z| < 0.5) in 91% and up is within 26 degrees of world up in 90%; the old forward
//     (local +Y) was level in 5%.
std::string lowerCopy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

int dialogueCameraGate(GameSession& game)
{
    int failures = 0;
    const auto verdict = [&](bool ok, const std::string& what) {
        logInfo(concat("dialogue camera: ", what, ok ? " PASS" : " FAIL"));
        failures += ok ? 0 : 1;
    };
    constexpr float kRadiansToDegrees = 57.29578f;
    const auto angleDegrees = [&](kotor::Vec3 a, kotor::Vec3 b) {
        const float n = kotor::length(a) * kotor::length(b);
        return n <= 1e-8f ? 180.0f
                          : std::acos(std::clamp(kotor::dot(a, b) / n, -1.0f, 1.0f)) * kRadiansToDegrees;
    };
    const auto headOf = [](const kotor::Entity& entity) {
        kotor::Vec3 head = entity.position;
        head.z += entity.kind == kotor::EntityKind::Creature ? 1.5f : 1.0f;
        return head;
    };
    const auto isActor = [](const kotor::Entity& entity) {
        return entity.kind == kotor::EntityKind::Creature || entity.kind == kotor::EntityKind::Placeable
            || entity.kind == kotor::EntityKind::Door;
    };

    // 1. Static cameras aim at someone.
    std::vector<float> offAxis;
    for (const kotor::CameraDef& definition : game.scene().cameras) {
        const SceneCamera camera = game.cinematicCamera(static_cast<std::int32_t>(definition.id));
        float best = 180.0f;
        for (const kotor::Entity& entity : game.scene().entities) {
            if (!isActor(entity)) {
                continue;
            }
            const kotor::Vec3 toward = headOf(entity) - camera.eye;
            const float range = kotor::length(toward);
            if (range >= 0.8f && range <= 25.0f) {
                best = std::min(best, angleDegrees(camera.forward, toward));
            }
        }
        offAxis.push_back(best);
    }
    std::size_t aimed = 0;
    for (const float angle : offAxis) {
        aimed += angle <= 20.0f ? 1u : 0u;
    }
    std::vector<float> sortedAngles = offAxis;
    std::sort(sortedAngles.begin(), sortedAngles.end());
    const float median = sortedAngles.empty() ? 180.0f : sortedAngles[sortedAngles.size() / 2];
    logInfo(concat("dialogue camera: git cameras=", offAxis.size(), " aimed_within_20deg=", aimed,
                   " median_best_angle=", median));
    verdict(offAxis.size() >= 20 && aimed * 10 >= offAxis.size() * 7, "git_cameras_aim_at_actors");

    // 2. Static-camera lines frame their speaker.
    std::size_t measured = 0;
    std::size_t inView = 0;
    std::size_t staged = 0;
    std::size_t framed = 0;
    std::size_t turnedAroundFramed = 0;
    float worstAngle = 0.0f;
    const std::string moduleLower = lowerCopy(game.module());
    for (const std::string& name : game.install().resourceNames(2029)) {
        std::string source;
        const auto data = game.install().find(name, 2029, &source);
        // Only this module's own dialogues: their CameraIDs index this module's cameras.
        if (!data.has_value() || lowerCopy(source).find(moduleLower) == std::string::npos) {
            continue;
        }
        kotor::Dlg dlg;
        try {
            kotor::Gff gff = kotor::Gff::parse(*data, name + ".dlg");
            dlg = kotor::Dlg::parse(gff, nullptr, name);
        } catch (const kotor::FormatError&) {
            continue;
        }
        const std::string conversation = lowerCopy(name);
        // Replies are the player, whose position is not in the GIT, so only entries count.
        for (const kotor::DlgEntry& entry : dlg.entries) {
            if (entry.cameraAngle != 6 || entry.cameraId < 0) {
                continue;
            }
            const std::string speakerTag = lowerCopy(entry.speaker);
            const kotor::Entity* speaker = nullptr;
            for (const kotor::Entity& entity : game.scene().entities) {
                if (isActor(entity)
                    && (!speakerTag.empty() ? lowerCopy(entity.tag) == speakerTag
                                            : lowerCopy(entity.conversation) == conversation)) {
                    speaker = &entity;
                    break;
                }
            }
            const SceneCamera camera = game.cinematicCamera(entry.cameraId);
            if (speaker == nullptr || camera.fovYDegrees <= 0.0f) {
                continue;
            }
            const kotor::Vec3 toward = headOf(*speaker) - camera.eye;
            if (kotor::length(toward) > 25.0f) {
                ++staged;
                continue;
            }
            ++measured;
            const kotor::Vec3 right = kotor::normalize(kotor::cross(camera.forward, camera.up));
            const float depth = kotor::dot(toward, camera.forward);
            const float tanV = std::tan(camera.fovYDegrees * 0.5f / kRadiansToDegrees);
            const float tanH = tanV * (16.0f / 9.0f);
            const bool visible = depth > 0.05f
                && std::fabs(kotor::dot(toward, right) / depth) <= tanH
                && std::fabs(kotor::dot(toward, camera.up) / depth) <= tanV;
            inView += visible ? 1u : 0u;
            worstAngle = std::max(worstAngle, angleDegrees(camera.forward, toward));
            // Shipped static shots may show someone other than the speaker (voice-over from a
            // scene object or an off-screen character), so the shot is judged by whether it
            // frames any subject. The same camera turned around is the negative check.
            const auto nearestSubject = [&](kotor::Vec3 forward, std::string* tag) {
                float nearest = 180.0f;
                for (const kotor::Entity& entity : game.scene().entities) {
                    const kotor::Vec3 other = headOf(entity) - camera.eye;
                    const float range = kotor::length(other);
                    if (isActor(entity) && range >= 0.8f && range <= 25.0f
                        && angleDegrees(forward, other) < nearest) {
                        nearest = angleDegrees(forward, other);
                        if (tag != nullptr) {
                            *tag = entity.tag;
                        }
                    }
                }
                return nearest;
            };
            std::string subject;
            const float subjectAngle = nearestSubject(camera.forward, &subject);
            const bool subjectFramed = visible || subjectAngle <= 20.0f;
            framed += subjectFramed ? 1u : 0u;
            turnedAroundFramed += nearestSubject(camera.forward * -1.0f, nullptr) <= 20.0f ? 1u : 0u;
            if (!visible) {
                logInfo(concat("dialogue camera: off-screen speaker dlg=", conversation, " camera=",
                               entry.cameraId, " speaker=", speaker->tag, " range=", kotor::length(toward),
                               " angle=", angleDegrees(camera.forward, toward), " frames=", subject,
                               " at_angle=", subjectAngle));
            }
        }
    }
    logInfo(concat("dialogue camera: static lines judged=", measured, " speaker_in_view=", inView,
                   " staged_elsewhere=", staged, " worst_angle=", worstAngle, " subject_framed=", framed,
                   " turned_around_framed=", turnedAroundFramed));
    verdict(measured >= 2 && framed == measured && inView * 2 >= measured && turnedAroundFramed < measured,
            "static_camera_frames_a_subject");

    // 3. Camera-model hooks look along -Z with +Y up.
    std::size_t clips = 0;
    std::size_t level = 0;
    std::size_t upright = 0;
    std::size_t models = 0;
    for (const std::string& name : game.install().resourceNames(2002)) {
        const std::string lowerName = lowerCopy(name);
        if (lowerName.size() < 5 || lowerName.compare(lowerName.size() - 4, 4, "_cam") != 0) {
            continue;
        }
        std::size_t modelClips = 0;
        int misses = 0;
        for (int number = 1; number <= 60 && misses < 8; ++number) {
            char clip[16]{};
            std::snprintf(clip, sizeof(clip), "CUT%03dW", number);
            if (!game.setDialogueCameraAnimationForTest(lowerName, clip)) {
                ++misses;
                continue;
            }
            misses = 0;
            const SceneCamera camera = game.camera();
            ++modelClips;
            level += std::fabs(camera.forward.z) < 0.5f ? 1u : 0u;
            upright += camera.up.z > 0.9f ? 1u : 0u;
        }
        clips += modelClips;
        models += modelClips > 0 ? 1u : 0u;
    }
    game.endDialogue();
    logInfo(concat("dialogue camera: hook models=", models, " clips=", clips, " level=", level,
                   " upright=", upright));
    verdict(models >= 20 && clips >= 200 && level * 100 >= clips * 85 && upright * 100 >= clips * 85,
            "camera_hooks_level_and_upright");
    return failures;
}

int runDialogueTest(GameSession& game)
{
    std::size_t index = static_cast<std::size_t>(-1);
    if (game.findEntity("end_trask") != static_cast<std::size_t>(-1)) {
        index = game.findEntity("end_trask");
    } else {
        for (std::size_t i = 0; i < game.scene().entities.size(); ++i) {
            if (!game.scene().entities[i].conversation.empty()) {
                index = i;
                break;
            }
        }
    }
    if (index == static_cast<std::size_t>(-1)) {
        logError("dialogue test: no conversational entity");
        return 9;
    }
    const kotor::Entity& entity = game.scene().entities[index];
    logInfo(concat("dialogue test: ", entity.tag, " conversation ", entity.conversation));
    if (!game.startDialogue(index)) {
        logError("dialogue test: conversation did not start");
        return 10;
    }
    int lines = 0;
    while (game.dialogue().active && lines < 40) {
        logInfo(concat("  ", game.dialogue().speaker, ": ", game.dialogue().text));
        for (std::size_t i = 0; i < game.dialogue().options.size() && i < 5; ++i) {
            logInfo(concat("    ", i + 1, ". ", game.dialogue().options[i].text));
        }
        game.chooseDialogueOption(0);
        ++lines;
    }
    logInfo(concat("dialogue test: lines=", lines));
    for (const std::string& line : game.scriptLog()) {
        logInfo(concat("  script: ", line));
    }
    if (game.dialogue().active) {
        game.endDialogue();
    }
    if (dialogueCameraGate(game) != 0) {
        return 12;
    }
    return lines > 0 ? 0 : 11;
}

int runCombatTest(GameSession& game)
{
    const std::size_t hostile = firstHostile(game);
    if (hostile == static_cast<std::size_t>(-1)) {
        logError("combat test: no hostile creature");
        return 9;
    }
    kotor::Vec3 spawn = game.scene().entities[hostile].position;
    spawn.x += 1.0f;
    game.setPositionForTest(spawn);
    game.attackNearestHostile();
    int steps = 0;
    while (steps < 3600 && !game.playerDead() && !game.scene().entities[hostile].dead) {
        updateFighting(game);
        ++steps;
    }
    const kotor::Entity& target = game.scene().entities[hostile];
    const bool roundSeen = hasEvent(game, "round ");
    logInfo(concat("combat test: ", target.tag, " dead=", target.dead ? 1 : 0,
                   " hp=", target.hitPoints, "/", target.maxHitPoints,
                   " player=", game.playerHp(), "/", game.playerMaxHp(),
                   " steps=", steps, " rounds=", roundSeen ? 1 : 0));
    return target.dead && roundSeen ? 0 : 10;
}

constexpr std::size_t kNoEntityIndex = static_cast<std::size_t>(-1);

int runPlaythrough(GameSession& game)
{
    bool ok = true;
    int spokenLines = 0;
    // Click through whatever is being said, first reply each time.
    const auto talk = [&]() {
        for (int guard = 0; game.dialogue().active && guard < 64; ++guard) {
            logInfo(concat("  dialogue: ", game.dialogue().speaker, ": ", game.dialogue().text));
            game.chooseDialogueOption(0);
            ++spokenLines;
        }
        // A party screen a script opened: take what it offers.
        if (game.partySelection().active) {
            logInfo(concat("playthrough: party selection, chosen=", game.partySelection().chosen.size()));
            game.acceptPartySelection();
        }
    };
    // Let the world run, talking as it comes, until done() or the time is up.
    const auto play = [&](float seconds, const auto& done) {
        for (float t = 0.0f; t < seconds && !done(); t += 0.05f) {
            talk();
            game.update(0.05f, InputState{});
        }
        talk();
        return done();
    };
    const auto walkOrFail = [&](kotor::Vec3 target, float seconds) {
        for (int attempt = 0; attempt < 8; ++attempt) {
            talk();
            if (game.walkTo(target, seconds)) {
                return;
            }
            if (!game.dialogue().active) {
                break;
            }
        }
        talk();
        const std::size_t door = game.moveBlockingDoor();
        logError(concat("playthrough: walk blocked at ", game.controlledPosition().x, ", ",
                        game.controlledPosition().y,
                        " going to ", target.x, ", ", target.y, " outcome=",
                        static_cast<int>(game.moveOutcome()), " door=",
                        door < game.scene().entities.size() ? game.scene().entities[door].tag : "-",
                        " locked=", door < game.scene().entities.size()
                            && game.scene().entities[door].locked ? 1 : 0));
        ok = false;
    };
    // The shipped tutorial's own state variable: the conversation it selects next.
    const auto trainingStage = [&]() -> std::int64_t {
        const std::vector<std::vector<script::Value>> args{{script::Value::makeString("END_TRASK_DLG")}};
        const auto value = game.actionAsForTest(game.scriptWorld().pc, 580, args);
        return value.has_value() && !value->empty() ? value->front().asInt() : -999;
    };

    // The shipped opening of end_m01aa, as a player meets it. Trask locks himself and the
    // player in; he only moves on once the player has taken gear from the footlocker and
    // put it on, and then joins the party. The bulkhead end_door01 refuses the player:
    // its OnFailToOpen (k_pend_door01) opens only with Trask in the lead, so the party
    // leader is switched to him and he opens it.
    const std::size_t trask = game.findEntity("end_trask");
    const std::size_t locker = game.findEntity("end_locker01");
    const std::size_t bulkhead = game.findEntity("end_door01");
    const std::size_t triggerEntity = game.findEntity("end_trig02");
    constexpr std::size_t kAbsent = static_cast<std::size_t>(-1);
    if (trask == kAbsent || locker == kAbsent || bulkhead == kAbsent || triggerEntity == kAbsent) {
        logError("playthrough: end_trask, end_locker01, end_door01 or end_trig02 missing");
        ok = false;
    } else {
        play(30.0f, [] { return false; });
        logInfo(concat("playthrough: opening lines=", spokenLines, " stage=", trainingStage()));
        ok = ok && spokenLines > 0;

        walkOrFail(game.scene().entities[locker].position, 20.0f);
        talk();
        // Clicks wait out whatever still holds the controls.
        UseOutcome lockerOutcome = UseOutcome::Refused;
        const bool used = play(15.0f, [&] {
            lockerOutcome = game.useObject(locker);
            return lockerOutcome == UseOutcome::Looted || lockerOutcome == UseOutcome::Used;
        });
        const bool logged = hasEvent(game, "use end_locker01");
        logInfo(concat("playthrough: placeable end_locker01 used=", used ? 1 : 0, " outcome=",
                       static_cast<int>(lockerOutcome), " logged=", logged ? 1 : 0,
                       " pos=", game.controlledPosition().x, ", ", game.controlledPosition().y));
        ok = ok && used && logged;
        game.takeAllLoot();
        game.closeLoot();
        // Put on what the locker held, as the conversation asks.
        std::vector<std::string> gear;
        for (const InventoryItem& item : game.inventory()) {
            if (!item.equipped) {
                gear.push_back(item.resref);
            }
        }
        for (const std::string& resref : gear) {
            const bool worn = game.equipItem(resref);
            logInfo(concat("playthrough: equip ", resref, " ok=", worn ? 1 : 0));
        }

        const bool trained = play(240.0f, [&] { return trainingStage() == 9 && !game.dialogue().active; });
        logInfo(concat("playthrough: training stage=", trainingStage(), " reached=", trained ? 1 : 0));
        ok = ok && trained;

        const bool switched = play(10.0f, [&] { return game.selectLeaderNpc(0); });
        logInfo(concat("playthrough: leader switched to Trask=", switched ? 1 : 0,
                       " choices=", game.selectableLeaders().size(),
                       " member=", game.isNpcPartyMember(0) ? 1 : 0,
                       " solo=", game.soloMode() ? 1 : 0,
                       " control_locked=", game.controlLocked() ? 1 : 0,
                       " dialogue=", game.dialogue().active ? 1 : 0));
        ok = ok && switched;

        const auto bulkheadOpen = [&] { return game.scene().entities[bulkhead].open; };
        if (game.useObject(bulkhead) == UseOutcome::OutOfRange) {
            walkOrFail(game.scene().entities[bulkhead].position, 20.0f);
            game.useObject(bulkhead);
        }
        const bool opened = play(30.0f, [&] { return bulkheadOpen() && !game.dialogue().active; });
        logInfo(concat("playthrough: end_door01 opened=", opened ? 1 : 0, " stage=", trainingStage()));
        ok = ok && opened;
        kotor::Vec3 centre{};
        float count = 0.0f;
        for (const kotor::TriggerVolume& volume : game.scene().triggers) {
            if (volume.entity != triggerEntity) {
                continue;
            }
            for (const kotor::Vec3& vertex : volume.vertices) {
                centre = centre + vertex;
                count += 1.0f;
            }
        }
        centre = count > 0.0f ? centre * (1.0f / count) : game.scene().entities[triggerEntity].position;
        // A player clicks past the bulkhead once its leaves have finished opening: the walkmesh opens with the
        // clip, a moment after the door's flag turns.
        play(5.0f, [&] { return game.pathExists(game.controlledPosition(), centre); });
        walkOrFail(centre, 20.0f);
        const bool fired = play(2.0f, [&] { return hasEvent(game, "trigger end_trig02: enter"); });
        logInfo(concat("playthrough: trigger end_trig02 fired=", fired ? 1 : 0));
        ok = ok && fired;
        play(2.0f, [] { return false; });
        // k_pend_trig02 hands the lead back; the rest is the player's own walk.
        game.selectLeaderNpc(kNpcPlayer);
    }

    // From here the ship is walked to its exit as a player does it: stop for what is said,
    // fight what comes in range, click a door that stops the walk, pick a lock that holds.
    int kills = 0;
    std::set<std::size_t> gaveUpOn;
    const auto fightNearby = [&] {
        for (;;) {
            std::size_t nearest = static_cast<std::size_t>(-1);
            float nearestGap = 12.0f;
            const kotor::Vec3 at = game.position();
            for (std::size_t i = 0; i < game.scene().entities.size(); ++i) {
                const kotor::Entity& entity = game.scene().entities[i];
                const float gap = std::hypot(entity.position.x - at.x, entity.position.y - at.y);
                if (entity.hostile && !entity.dead && gap < nearestGap && !gaveUpOn.contains(i)) {
                    nearest = i;
                    nearestGap = gap;
                }
            }
            if (nearest == static_cast<std::size_t>(-1) || game.playerDead()) {
                return;
            }
            game.attackNearestHostile();
            int steps = 0;
            while (steps < 1800 && !game.playerDead() && !game.scene().entities[nearest].dead) {
                talk();
                updateFighting(game);
                ++steps;
            }
            const bool dead = game.scene().entities[nearest].dead;
            logInfo(concat("playthrough: fought ", game.scene().entities[nearest].tag, " dead=", dead ? 1 : 0,
                           " playerHp=", game.playerHp(), "/", game.playerMaxHp(), " steps=", steps));
            if (dead) {
                ++kills;
            } else {
                gaveUpOn.insert(nearest);
            }
        }
    };
    const auto advanceTo = [&](kotor::Vec3 target) {
        for (int attempt = 0; attempt < 60; ++attempt) {
            talk();
            fightNearby();
            if (game.walkTo(target, 30.0f)) {
                return true;
            }
            const std::size_t door = game.moveOutcome() == GameSession::MoveOutcome::DoorBlocked
                ? game.moveBlockingDoor() : static_cast<std::size_t>(-1);
            if (door < game.scene().entities.size() && game.scene().entities[door].locked
                && !game.dialogue().active && !game.controlLocked()) {
                // end_trask01: put Trask in the lead and use his security skill.
                const bool trasked = game.selectLeaderNpc(0);
                const bool picked = game.unlockDoor(door);
                logInfo(concat("playthrough: door ", game.scene().entities[door].tag, " locked, as Trask=",
                               trasked ? 1 : 0, " picked=", picked ? 1 : 0, " stage=", trainingStage()));
                game.selectLeaderNpc(kNpcPlayer);
                // A door no skill opens is opened by clearing the room: the shipped death
                // scripts count the kills (k_pend_room5_01 and its like). Go find the fight.
                if (!picked) {
                    std::size_t nearest = static_cast<std::size_t>(-1);
                    float nearestGap = 1.0e9f;
                    const kotor::Vec3 at = game.position();
                    for (std::size_t i = 0; i < game.scene().entities.size(); ++i) {
                        const kotor::Entity& entity = game.scene().entities[i];
                        const float gap = std::hypot(entity.position.x - at.x, entity.position.y - at.y);
                        if (entity.hostile && !entity.dead && gap < nearestGap && !gaveUpOn.contains(i)) {
                            nearest = i;
                            nearestGap = gap;
                        }
                    }
                    if (nearest != static_cast<std::size_t>(-1)) {
                        logInfo(concat("playthrough: hunting ", game.scene().entities[nearest].tag, " at ",
                                       game.scene().entities[nearest].position.x, ", ",
                                       game.scene().entities[nearest].position.y));
                        if (!game.walkTo(game.scene().entities[nearest].position, 30.0f)
                            && !game.dialogue().active && nearestGap > 12.0f
                            && game.moveOutcome() != GameSession::MoveOutcome::Moving) {
                            gaveUpOn.insert(nearest);
                        }
                        talk();
                        fightNearby();
                    }
                }
            }
            play(2.0f, [] { return false; });
        }
        talk();
        const std::size_t door = game.moveBlockingDoor();
        logError(concat("playthrough: stopped at ", game.position().x, ", ", game.position().y, " going to ",
                        target.x, ", ", target.y, " outcome=", static_cast<int>(game.moveOutcome()), " door=",
                        door < game.scene().entities.size() ? game.scene().entities[door].tag : "-",
                        " stage=", trainingStage()));
        const auto& lines = game.scriptLog();
        for (std::size_t i = lines.size() > 30 ? lines.size() - 30 : 0; i < lines.size(); ++i) {
            logInfo(concat("playthrough: recent ", lines[i]));
        }
        return false;
    };

    const std::size_t exitDoor = game.findEntity("end_door10");
    if (exitDoor == kNoEntityIndex) {
        logError("playthrough: end_door10 missing");
        ok = false;
    } else {
        const bool atExit = advanceTo(game.scene().entities[exitDoor].position);
        logInfo(concat("playthrough: reached exit door=", atExit ? 1 : 0, " pos=", game.position().x, ", ",
                       game.position().y, " kills=", kills, " playerDead=", game.playerDead() ? 1 : 0));
        ok = ok && atExit;
    }
    ok = ok && kills > 0;

    std::string from;
    std::string to;
    const bool transitioned = game.testTransition(from, to);
    logInfo(concat("playthrough: transition ", from, " -> ", to, " ok=", transitioned ? 1 : 0));
    ok = ok && transitioned;

    const std::filesystem::path savePath =
        std::filesystem::temp_directory_path() / "kotor_engine_playthrough.save";
    if (!game.save(savePath)) {
        logError("playthrough: save failed");
        ok = false;
    } else {
        GameSession restored(game.installPath(), "end_m01aa");
        if (!restored.load(savePath)) {
            logError("playthrough: load failed");
            ok = false;
        } else {
            const float dx = restored.position().x - game.position().x;
            const float dy = restored.position().y - game.position().y;
            const bool same = std::fabs(dx) < 0.001f && std::fabs(dy) < 0.001f
                && restored.module() == game.module();
            logInfo(concat("playthrough: save/load module=", restored.module(),
                           " same=", same ? 1 : 0));
            ok = ok && same;
        }
    }

    while (game.elapsedSeconds() < 1800.0f) {
        game.update(0.05f, InputState{});
    }
    logInfo(concat("playthrough: simulated ", game.elapsedSeconds(), " s, events=",
                   game.combatLog().size()));
    logInfo(ok ? "playthrough: PASS" : "playthrough: FAIL");
    return ok ? 0 : 12;
}

int runLandmarkTest(GameSession& game)
{
    bool ok = true;
    const auto close = [](float a, float b, float eps = 0.01f) {
        return std::fabs(a - b) <= eps;
    };
    const auto closeAngle = [](float a, float b, float eps = 0.001f) {
        const float twoPi = 6.28318530717958647692f;
        float delta = a - b;
        while (delta > 3.14159265358979323846f) {
            delta -= twoPi;
        }
        while (delta < -3.14159265358979323846f) {
            delta += twoPi;
        }
        return std::fabs(delta) <= eps;
    };
    const auto check = [&](const char* tag, kotor::Vec3 expected, float expectedBearing, bool checkBearing) {
        const std::size_t index = game.findEntity(tag);
        if (index == static_cast<std::size_t>(-1)) {
            logError(concat("landmark ", tag, ": missing"));
            ok = false;
            return;
        }
        const kotor::Entity& entity = game.scene().entities[index];
        const bool posOk = close(entity.position.x, expected.x) && close(entity.position.y, expected.y)
            && close(entity.position.z, expected.z);
        const bool bearingOk = !checkBearing || closeAngle(entity.bearing, expectedBearing);
        logInfo(concat("landmark ", tag, " pos=(", entity.position.x, ", ", entity.position.y, ", ",
                       entity.position.z, ") bearing=", entity.bearing, " pos_ok=", posOk ? 1 : 0,
                       " bearing_ok=", bearingOk ? 1 : 0));
        ok = ok && posOk && bearingOk;
    };

    check("end_trask", {22.883694f, 20.610802f, -1.2750006f}, -2.7488658f, true);
    check("end_door05", {46.574902f, 50.813400f, -1.3157600f}, 0.0f, true);
    check("end_locker01", {10.795940f, 21.870289f, -1.1049995f}, 3.1414495f, true);

    const std::size_t door = game.findEntity("end_door10");
    if (door == static_cast<std::size_t>(-1)) {
        logError("landmark end_door10: missing");
        ok = false;
    } else {
        const kotor::Entity& entity = game.scene().entities[door];
        const bool linkOk = entity.linkedModule == "end_m01ab" && entity.linkedTo == "from_01a";
        logInfo(concat("landmark end_door10 link=", entity.linkedModule, "/", entity.linkedTo,
                       " ok=", linkOk ? 1 : 0));
        ok = ok && linkOk;
    }

    const kotor::Vec3 entry = game.scene().entry;
    const bool entryOk = close(entry.x, 15.4248f) && close(entry.y, 20.1215f) && close(entry.z, -1.275f);
    logInfo(concat("landmark entry pos=(", entry.x, ", ", entry.y, ", ", entry.z,
                   ") ok=", entryOk ? 1 : 0));
    ok = ok && entryOk;

    logInfo(ok ? "landmark test: PASS" : "landmark test: FAIL");
    return ok ? 0 : 9;
}

bool scriptLogHas(const GameSession& game, const std::string& needle)
{
    for (const std::string& line : game.scriptLog()) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

int runScriptTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("script test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };

    std::size_t hookCount = 0;
    std::size_t entitiesWithHooks = 0;
    for (const kotor::Entity& entity : game.scene().entities) {
        if (!entity.scripts.empty()) {
            ++entitiesWithHooks;
            hookCount += entity.scripts.size();
        }
    }
    logInfo(concat("script test: hooks=", hookCount, " entities=", entitiesWithHooks,
                   " scene_hooks=", game.scene().scripts.size(),
                   " runs=", game.scriptLog().size()));
    check(hookCount > 0 && !game.scene().scripts.empty(), "hooks_loaded");
    check(scriptLogHas(game, "script k_pdan_trask_9"), "trask_spawn_ran");
    check(scriptLogHas(game, "script k_pend_area01"), "area_enter_ran");
    check(scriptLogHas(game, "script k_def_spawn01") || scriptLogHas(game, "script k_hen_spawn01"),
          "creature_spawn_ran");

    // Trigger enter: teleport into the shipped end_trig02 polygon.
    const std::size_t triggerEntity = game.findEntity("end_trig02");
    if (triggerEntity == static_cast<std::size_t>(-1)) {
        logError("script test: end_trig02 missing");
        return 9;
    }
    kotor::Vec3 centre{};
    float count = 0.0f;
    for (const kotor::TriggerVolume& volume : game.scene().triggers) {
        if (volume.entity != triggerEntity) {
            continue;
        }
        for (const kotor::Vec3& vertex : volume.vertices) {
            centre = centre + vertex;
            count += 1.0f;
        }
    }
    if (count > 0.0f) {
        centre = centre * (1.0f / count);
    } else {
        centre = game.scene().entities[triggerEntity].position;
    }
    game.setPositionForTest(centre);
    game.update(1.0f / 60.0f, InputState{});
    check(scriptLogHas(game, "script k_pend_trig02"), "trigger_script_ran");

    // Heartbeat: the module advances one 6 s beat.
    for (int tick = 0; tick < 130; ++tick) {
        game.update(0.05f, InputState{});
    }
    check(scriptLogHas(game, "hook ScriptHeartbeat"), "heartbeat_hook_ran");
    // A creature beats every 3.0 to 4.199 s (004eb6e0: 3000 + rand() % 1200 ms), not every 6 s:
    // over 30 s the busiest creature with a tag of its own beats 7 to 11 times (6 s would allow 6).
    {
        std::map<std::string, int> beats;
        for (int tick = 0; tick < 600; ++tick) {
            const std::size_t before = game.scriptLog().size();
            game.update(0.05f, InputState{});
            const std::vector<std::string>& log = game.scriptLog();
            for (std::size_t i = before < log.size() ? before : log.size(); i < log.size(); ++i) {
                const std::string prefix = "hook ScriptHeartbeat ";
                if (log[i].rfind(prefix, 0) == 0) {
                    ++beats[log[i].substr(prefix.size(), log[i].find(" -> ") - prefix.size())];
                }
            }
        }
        int busiest = 0;
        for (const auto& entry : beats) {
            int sharing = 0;
            for (const kotor::Entity& entity : game.scene().entities) {
                sharing += entity.tag == entry.first ? 1 : 0;
            }
            logInfo(concat("script test: heartbeats ", entry.first, " ", entry.second, " over ", sharing));
            if (sharing == 1) {
                busiest = std::max(busiest, entry.second);
            }
        }
        logInfo(concat("script test: busiest creature heartbeat count over 30 s = ", busiest));
        check(busiest >= 7 && busiest <= 11, "creature_heartbeat_interval");
    }

    // Door interaction hook: end_door16 carries a shipped OnOpen script.
    check(game.runHookForTest("OnOpen", "end_door16"), "door_hook_ran");
    check(scriptLogHas(game, "hook OnOpen end_door16"), "door_hook_logged");
    check(scriptLogHas(game, "script k_pend_door16 status=returned"), "door_script_ran");
    game.endDialogue();

    // Area enter with a real entering object. GetIsPC must be 1 for the script
    // to take its player branch.
    game.runScript("k_pend_area01", game.scriptWorld().pc, game.scriptWorld().pc,
                   game.scriptWorld().pc, game.scriptWorld().pc);
    check(scriptLogHas(game, "script k_pend_area01 status=returned"), "pc_branch_ran");

    // Death hook: run the shipped hostile to death and confirm ScriptDeath.
    const std::size_t hostile = firstHostile(game);
    if (hostile != static_cast<std::size_t>(-1)) {
        kotor::Vec3 spawn = game.scene().entities[hostile].position;
        spawn.x += 1.0f;
        game.setPositionForTest(spawn);
        dismissDialogue(game);
        game.attackNearestHostile();
        int steps = 0;
        while (steps < 3600 && !game.playerDead() && !game.scene().entities[hostile].dead) {
            updateFighting(game);
            ++steps;
        }
        logInfo(concat("script test: fight steps=", steps, " player_dead=", game.playerDead() ? 1 : 0,
                       " hostile=", game.scene().entities[hostile].tag,
                       " hostile_hp=", game.scene().entities[hostile].hitPoints,
                       " still_hostile=", game.scene().entities[hostile].hostile ? 1 : 0));
        check(game.scene().entities[hostile].dead, "hostile_died");
        check(scriptLogHas(game, "hook ScriptDeath"), "death_hook_ran");
    }

    // Dialogue conditions: the shipped DLG has 19 StartingList.Active entries.
    dismissDialogue(game);
    if (!game.startDialogue("end_trask")) {
        logError("script test: Trask dialogue did not start");
        return 10;
    }
    bool conditionSeen = false;
    std::string selected;
    for (const std::string& line : game.scriptLog()) {
        if (line.find("dialogue condition k_pend_trask00") != std::string::npos) {
            conditionSeen = true;
        }
        // The first start is the one k_pend_trask00 decides: End_TraskTalk is still
        // 0. A shipped script may have opened this conversation before this point,
        // and k_pend_traskinc on entry 54 then moves later starts to other links.
        if (line.rfind("dialogue start end_trask01", 0) == 0) {
            logInfo(concat("script test: ", line));
            if (selected.empty()) {
                selected = line;
            }
        }
    }
    check(conditionSeen, "dialogue_condition_ran");
    check(selected.find("entry=54") != std::string::npos, "dialogue_selected_54");
    logInfo(concat("script test: ", selected));
    game.endDialogue();

    logInfo(ok ? "script test: PASS" : "script test: FAIL");
    return ok ? 0 : 11;
}

int runRulesTest(GameSession& game)
{
    const Rules& rules = game.rules();
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("rules test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    if (!rules.loaded()) {
        logError(concat("rules test: ", rules.error()));
        return 9;
    }
    const CharacterSheet soldier = rules.createCharacter(0, 1);
    logInfo(concat("rules test: level(999)=", rules.levelForXp(999), " level(1000)=",
                   rules.levelForXp(1000), " xp(2)=", rules.xpForLevel(2)));
    logInfo(concat("rules test: soldier hp=", soldier.maxHp, " bab=",
                   rules.baseAttackBonus(0, 1), " fort=", rules.savingThrow(0, 1, 0),
                   " skills=", soldier.skillPoints, " feats=", soldier.featPoints,
                   " ac=", rules.armorClass(soldier), " attack=", rules.attackBonus(soldier)));
    check(rules.className(0) == "Soldier", "class_soldier");
    check(rules.classHitDie(0) == 10, "hit_die_10");
    check(rules.baseAttackBonus(0, 1) == 1, "bab_1");
    check(rules.savingThrow(0, 1, 0) == 2, "fort_2");
    check(soldier.maxHp == 12, "hp_12");
    // Level 1 awards four times (SkillPointBase/2 + INT mod), at least 4; the old
    // expectation of 2 used the level-up rule at level 1.
    check(soldier.skillPoints == 4, "skill_points_4");
    check(soldier.featPoints == 1, "feat_points_1");
    check(rules.levelForXp(0) == 1, "level_0");
    check(rules.levelForXp(999) == 1, "level_999");
    check(rules.levelForXp(1000) == 2, "level_1000");
    check(rules.xpForLevel(2) == 1000, "xp_level_2");
    check(rules.equipSlotFor(2) == 4, "long_sword_slot_4");
    check(rules.armorClass(soldier) == 12, "ac_12");
    check(rules.attackBonus(soldier) == 4, "attack_4");
    check(!rules.canTakeFeat(soldier, 1), "feat_prereq_refused");
    check(game.learnFeat(3), "learn_feat");
    check(game.character().feats.contains(3), "feat_added");
    check(!game.learnFeat(3), "duplicate_feat_refused");
    check(game.learnPower(6), "learn_power");
    check(game.character().powers.contains(6), "power_added");

    std::uint32_t rng = 0x12345678u;
    const AttackResult first = rules.resolveAttack(soldier, 10, rng, 1, 8, 2);
    const AttackResult second = rules.resolveAttack(soldier, 10, rng, 1, 8, 2);
    logInfo(concat("rules test: attack1 roll=", first.roll, " total=", first.total,
                   " hit=", first.hit ? 1 : 0, " crit=", first.critical ? 1 : 0,
                   " damage=", first.damage));
    logInfo(concat("rules test: attack2 roll=", second.roll, " total=", second.total,
                   " hit=", second.hit ? 1 : 0, " damage=", second.damage));
    check(first.roll >= 1 && first.roll <= 20, "d20_range");
    check(first.hit && first.damage >= 1, "attack_hits_ac10");
    check(second.roll >= 1 && second.roll <= 20, "d20_second");

    logInfo(ok ? "rules test: PASS" : "rules test: FAIL");
    return ok ? 0 : 10;
}

int runSystemsTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("systems test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    const std::size_t inventoryBefore = game.inventory().size();
    check(game.addJournalEntry("s6_test", 1), "journal_added");
    check(game.journalState("s6_test") == 1, "journal_state");
    check(game.removeJournalEntry("s6_test"), "journal_removed");
    check(game.journalState("s6_test") == 0, "journal_removed_state");

    const std::size_t journalBefore = game.journalSize();
    script::Vm dummy({});
    std::vector<std::vector<script::Value>> journalArgs = {
        {script::Value::makeString("s6_script_journal")}, {script::Value::makeInt(1)}};
    game.action(367, journalArgs, game.scriptWorld(), dummy);
    logInfo(concat("systems test: journal ", journalBefore, " -> ", game.journalSize()));
    check(game.journalSize() > journalBefore, "script_journal");
    check(game.journalState("s6_script_journal") == 1, "script_journal_state");

    const std::int32_t xpBeforeScript = game.character().xp;
    game.runScript("k_act_com31", game.scriptWorld().pc, game.scriptWorld().pc,
                   game.scriptWorld().pc, game.scriptWorld().pc);
    check(game.character().xp > xpBeforeScript, "script_xp");

    const std::int32_t goldBefore = game.gold();
    std::vector<std::vector<script::Value>> goldArgs = {
        {script::Value::makeObject(game.scriptWorld().pc)}, {script::Value::makeInt(200)}};
    game.action(322, goldArgs, game.scriptWorld(), dummy);
    logInfo(concat("systems test: gold ", goldBefore, " -> ", game.gold()));
    check(game.gold() > goldBefore, "script_gold");

    const std::int32_t levelBefore = game.character().level;
    game.giveXp(1000);
    check(game.character().xp >= 1000, "xp_granted");
    check(game.character().level > levelBefore, "level_up");
    logInfo(concat("systems test: xp=", game.character().xp, " level=",
                   game.character().level, " hp=", game.playerHp(), "/", game.playerMaxHp()));

    check(game.addInventoryItem("w_lngswrd_001", 2, 1), "item_added");
    check(game.inventory().size() == inventoryBefore + 1, "inventory_count");
    check(game.equipItem("w_lngswrd_001"), "item_equipped");
    bool equipped = false;
    for (const InventoryItem& item : game.inventory()) {
        if (item.resref == "w_lngswrd_001" && item.equipped) {
            equipped = true;
        }
    }
    check(equipped, "equipped_state");
    check(game.removeInventoryItem("w_lngswrd_001"), "item_removed");
    check(game.inventory().size() == inventoryBefore, "inventory_after_remove");
    check(game.addInventoryItem("w_lngswrd_001", 2, 1), "item_readded");

    check(game.addPartyMember("Carth"), "party_joined");
    bool partyFound = false;
    for (const std::string& tag : game.partyTags()) {
        if (tag == "Carth") {
            partyFound = true;
        }
    }
    check(partyFound, "party_roster");
    check(game.removePartyMember("Carth"), "party_left");
    check(game.addPartyMember("Carth"), "party_rejoined");

    // A shipped action script that creates items and writes a global.
    game.runScript("k_pend_traskdl50", game.scriptWorld().pc, game.scriptWorld().pc,
                   game.scriptWorld().pc, game.scriptWorld().pc);
    check(scriptLogHas(game, "script k_pend_traskdl50 status=returned"), "action_script_ran");
    const bool globalWritten = !game.scriptWorld().globals.empty();
    check(globalWritten, "script_global_written");
    check(game.learnFeat(3), "learn_feat");
    check(game.learnPower(6), "learn_power");

    const std::filesystem::path savePath =
        std::filesystem::temp_directory_path() / "kotor_engine_s6_systems.save";
    game.scriptWorld().setLocal(game.scriptWorld().pc, "s6_local",
                                script::Value::makeInt(7));
    check(game.save(savePath), "save_written");
    GameSession restored(game.installPath(), game.module());
    check(restored.load(savePath), "save_loaded");
    check(restored.character().xp == game.character().xp, "xp_roundtrip");
    check(restored.character().level == game.character().level, "level_roundtrip");
    check(restored.character().feats.contains(3), "feat_roundtrip");
    check(restored.character().powers.contains(6), "power_roundtrip");
    check(restored.inventory().size() == game.inventory().size(), "inventory_roundtrip");
    check(restored.partyTags().size() == game.partyTags().size(), "party_roundtrip");
    check(restored.module() == game.module(), "module_roundtrip");
    const script::Value local = restored.scriptWorld().getLocal(
        restored.scriptWorld().pc, "s6_local", script::ValueType::Int);
    check(local.integer == 7, "local_roundtrip");

    logInfo(ok ? "systems test: PASS" : "systems test: FAIL");
    return ok ? 0 : 10;
}

int runMerchantTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("merchant test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    check(!game.merchants().empty(), "stores_loaded");
    check(game.merchants().size() == 7, "seven_stores");
    if (game.merchants().empty()) {
        return 9;
    }
    std::size_t merchantIndex = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < game.merchants().size(); ++i) {
        if (game.merchants()[i].resref == "keblastore") {
            merchantIndex = i;
            break;
        }
    }
    if (merchantIndex == static_cast<std::size_t>(-1)) {
        merchantIndex = 0;
    }
    const MerchantState merchant = game.merchants()[merchantIndex];
    logInfo(concat("merchant test: store ", merchant.resref, " stock=", merchant.stock.size(),
                   " markup=", merchant.markUp, " markdown=", merchant.markDown,
                   " flag=", merchant.buySellFlag));
    check(merchant.stock.size() >= 40, "stock_41");
    check(merchant.buySellFlag == 3, "buy_and_sell");
    if (!game.openStore(merchant.entity)) {
        logError("merchant test: open failed");
        return 10;
    }
    check(game.activeMerchant() >= 0, "store_open");

    std::size_t stockIndex = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < merchant.stock.size(); ++i) {
        if (!merchant.stock[i].infinite && merchant.stock[i].item.cost > 0) {
            stockIndex = i;
            break;
        }
    }
    if (stockIndex == static_cast<std::size_t>(-1)) {
        stockIndex = 0;
    }
    const MerchantStockItem stock = game.merchants()[merchantIndex].stock[stockIndex];
    const std::int32_t buyPrice = game.buyPrice(stockIndex);
    logInfo(concat("merchant test: stock ", stock.item.resref, " cost=", stock.item.cost,
                   " buy=", buyPrice, " remaining=", stock.remaining));
    check(stock.item.cost > 0, "item_cost");
    check(buyPrice == std::max<std::int32_t>(1, stock.item.cost * merchant.markUp / 100),
          "buy_price_math");

    script::Vm dummy({});
    std::vector<std::vector<script::Value>> goldArgs = {
        {script::Value::makeObject(game.scriptWorld().pc)}, {script::Value::makeInt(5000)}};
    game.action(322, goldArgs, game.scriptWorld(), dummy);
    const std::int32_t goldBefore = game.gold();
    const std::size_t inventoryBefore = game.inventory().size();
    check(game.buyMerchantItem(stockIndex), "buy_item");
    check(game.gold() == goldBefore - buyPrice, "buy_gold");
    // A store has no purse (UTM has no gold field and K1 has no store-gold call).
    check(game.merchants()[merchantIndex].gold == 0, "store_has_no_gold");
    check(game.inventory().size() == inventoryBefore + 1, "buy_inventory");
    game.character().inventory.back().identified = false;
    const std::int32_t goldBeforeIdentify = game.gold();
    check(game.identifyInventoryItem(game.inventory().size() - 1), "identify_item");
    check(game.character().inventory.back().identified, "identified");
    check(game.gold() == goldBeforeIdentify, "identify_is_free");
    if (!stock.infinite) {
        check(game.merchants()[merchantIndex].stock[stockIndex].remaining == stock.remaining - 1,
              "buy_stock_decrement");
    }

    const std::filesystem::path savePath =
        std::filesystem::temp_directory_path() / "kotor_engine_s7_merchant.save";
    check(game.save(savePath), "save_written");
    {
        GameSession restored(game.installPath(), game.module());
        check(restored.load(savePath), "save_loaded");
        std::int32_t restoredRemaining = -1;
        for (const MerchantState& restoredMerchant : restored.merchants()) {
            if (restoredMerchant.resref == merchant.resref
                && stockIndex < restoredMerchant.stock.size()) {
                restoredRemaining = restoredMerchant.stock[stockIndex].remaining;
            }
        }
        check(restoredRemaining == game.merchants()[merchantIndex].stock[stockIndex].remaining,
              "stock_roundtrip");
        check(restored.gold() == game.gold(), "gold_roundtrip");
        check(restored.inventory().size() == game.inventory().size(), "inventory_roundtrip");
        check(!restored.inventory().empty() && restored.inventory().back().identified,
              "identified_roundtrip");
    }

    const std::int32_t goldAfterBuy = game.gold();
    const std::int32_t sellExpected = std::max<std::int32_t>(
        1, stock.item.cost * merchant.markDown / 100);
    check(game.sellPrice(game.inventory().size() - 1) == sellExpected, "sell_price_math");
    check(game.sellInventoryItem(game.inventory().size() - 1), "sell_item");
    check(game.gold() > goldAfterBuy, "sell_gold");
    check(game.inventory().size() == inventoryBefore, "sell_inventory");
    game.closeStore();
    logInfo(ok ? "merchant test: PASS" : "merchant test: FAIL");
    return ok ? 0 : 11;
}

// Encounters spawn when the player enters their volume. Puts the player at the
// volume's centroid (else its first spawn point) and ticks until it has noticed.
bool enterEncounterVolume(GameSession& game, std::size_t volumeIndex)
{
    const kotor::EncounterVolume& volume = game.scene().encounters[volumeIndex];
    kotor::Vec3 centre{};
    for (const kotor::Vec3& vertex : volume.vertices) {
        centre = centre + vertex;
    }
    if (!volume.vertices.empty()) {
        centre = centre * (1.0f / static_cast<float>(volume.vertices.size()));
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        game.setPositionForTest(attempt == 0 || volume.spawnPoints.empty()
                                    ? centre
                                    : volume.spawnPoints.front().position);
        for (int tick = 0; tick < 4; ++tick) {
            game.update(0.05f, InputState{});
        }
        if (game.encounters()[volumeIndex].triggered) {
            return true;
        }
    }
    return false;
}

int runEncounterTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("encounter test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    logInfo(concat("encounter test: volumes=", game.scene().encounters.size(),
                   " states=", game.encounters().size()));
    check(game.scene().encounters.size() >= 4, "volume_count");
    std::int32_t atLoad = 0;
    for (const EncounterState& state : game.encounters()) {
        atLoad += state.spawnedCount;
    }
    // Retail spawns on entry: nothing may exist before the player walks in.
    check(atLoad == 0, "no_spawn_before_entry");
    std::size_t volumeIndex = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < game.scene().encounters.size(); ++i) {
        const kotor::EncounterTemplate& data = game.scene().encounters[i].templateData;
        if (data.active && !data.creatures.empty() && !game.scene().encounters[i].spawnPoints.empty()) {
            volumeIndex = i;
            break;
        }
    }
    if (volumeIndex == static_cast<std::size_t>(-1)) {
        logError("encounter test: no usable volume");
        return 9;
    }
    const kotor::EncounterTemplate& data = game.scene().encounters[volumeIndex].templateData;
    // 005935d0: the budget picks up to MaxCreatures, PadOutCreatureCount (00590b10) tops up to RecCreatures.
    const std::int32_t expected = data.recCreatures;
    check(enterEncounterVolume(game, volumeIndex), "entry_triggers_spawn");
    std::int32_t total = 0;
    std::size_t firstEntity = static_cast<std::size_t>(-1);
    for (const EncounterState& state : game.encounters()) {
        total += state.spawnedCount;
        if (firstEntity == static_cast<std::size_t>(-1) && !state.entities.empty()) {
            firstEntity = state.entities.front();
        }
    }
    logInfo(concat("encounter test: spawned=", total, " expected=", expected));
    check(total > 0, "creatures_spawned");
    bool anySingle = false;
    for (const kotor::EncounterCreature& creature : data.creatures) {
        anySingle = anySingle || creature.singleSpawn;
    }
    check(game.encounters()[volumeIndex].spawnedCount >= 1, "retail_spawn_count");
    check(anySingle || game.encounters()[volumeIndex].spawnedCount >= expected, "retail_spawn_count_reaches_rec");
    check(data.maxCreatures <= 0
              || game.encounters()[volumeIndex].spawnedCount <= std::max(data.maxCreatures, data.recCreatures),
          "retail_spawn_count_within_max");
    if (firstEntity == static_cast<std::size_t>(-1)
        || firstEntity >= game.scene().entities.size()) {
        logError("encounter test: no spawned entity");
        return 10;
    }
    const kotor::Entity& entity = game.scene().entities[firstEntity];
    bool hasMesh = false;
    for (const kotor::EntityMesh& mesh : game.scene().entityMeshes) {
        if (mesh.entity == firstEntity) {
            hasMesh = true;
            break;
        }
    }
    logInfo(concat("encounter test: first ", entity.tag, " hp=", entity.hitPoints,
                   " hostile=", entity.hostile ? 1 : 0, " mesh=", hasMesh ? 1 : 0));
    check(entity.hostile, "spawned_hostile");
    check(hasMesh, "spawned_mesh");
    check(entity.hitPoints > 0, "spawned_hp");
    logInfo(ok ? "encounter test: PASS" : "encounter test: FAIL");
    return ok ? 0 : 11;
}

int runEncounterResetTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("encounter reset test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    std::size_t target = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < game.encounters().size(); ++i) {
        const EncounterState& state = game.encounters()[i];
        const kotor::EncounterTemplate& data = game.scene().encounters[i].templateData;
        if (data.reset && data.active && !data.creatures.empty() && state.resetTime > 0.0f
            && state.respawnsRemaining != 0 && !game.scene().encounters[i].spawnPoints.empty()) {
            target = i;
            break;
        }
    }
    if (target == static_cast<std::size_t>(-1)) {
        logError("encounter reset test: no resettable encounter");
        return 9;
    }
    // Retail wakes a wiped encounter only for SpawnOption 1 with Reset (00593fb0 reads both); a Reset
    // encounter of the other option stays inactive for good. Run the timer rule on the real encounter
    // with that option, and check the other option's wipe on a copy of the state below.
    const std::int32_t realOption = game.scene().encounters[target].templateData.spawnOption;
    game.scene().encounters[target].templateData.spawnOption = 1;
    check(!game.encounters()[target].spawned, "idle_until_entry");
    check(enterEncounterVolume(game, target), "entry_spawns");
    const EncounterState before = game.encounters()[target];
    logInfo(concat("encounter reset test: volume=", target, " spawned=", before.spawnedCount,
                   " reset_time=", before.resetTime, " respawns=", before.respawnsRemaining));
    for (const std::size_t entity : before.entities) {
        if (entity < game.scene().entities.size()) {
            game.scene().entities[entity].dead = true;
        }
    }
    for (int i = 0; i < 600; ++i) {
        game.update(0.08f, InputState{});
    }
    const EncounterState& after = game.encounters()[target];
    bool alive = false;
    for (const std::size_t entity : after.entities) {
        if (entity < game.scene().entities.size() && !game.scene().entities[entity].dead) {
            alive = true;
        }
    }
    logInfo(concat("encounter reset test: spawned ", before.spawnedCount, " -> ",
                   after.spawnedCount, " alive=", alive ? 1 : 0));
    check(after.spawnedCount > before.spawnedCount, "respawned");
    check(alive, "respawn_alive");
    logInfo(concat("encounter reset test: data SpawnOption was ", realOption));
    // The wipe of an encounter that is not SpawnOption 1: inactive afterwards and never woken.
    game.scene().encounters[target].templateData.spawnOption = 0;
    for (const std::size_t entity : after.entities) {
        if (entity < game.scene().entities.size()) {
            game.scene().entities[entity].dead = true;
        }
    }
    game.update(0.08f, InputState{});
    check(!game.scene().encounters[target].templateData.active, "wipe_makes_inactive");
    const std::int32_t countAtWipe = game.encounters()[target].spawnedCount;
    for (int i = 0; i < 600; ++i) {
        game.update(0.08f, InputState{});
    }
    check(game.encounters()[target].spawnedCount == countAtWipe, "no_respawn_without_option_1");
    logInfo(ok ? "encounter reset test: PASS" : "encounter reset test: FAIL");
    return ok ? 0 : 10;
}

int runPartyTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("party test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    const std::size_t carth = game.findEntity("Carth");
    if (carth == static_cast<std::size_t>(-1)) {
        logError("party test: Carth missing");
        return 9;
    }
    check(game.addPartyMember("Carth"), "join");
    check(game.companions().size() == 1, "companion_sheet");
    if (!game.companions().empty()) {
        const CompanionState& companion = game.companions().front();
        logInfo(concat("party test: ", companion.tag, " name=", companion.sheet.name,
                       " hp=", companion.sheet.hp, "/", companion.sheet.maxHp,
                       " level=", companion.sheet.level));
        check(!companion.sheet.name.empty(), "companion_name");
        check(companion.sheet.maxHp > 0, "companion_hp");
    }
    const std::int32_t xpBefore = game.character().xp;
    // A companion now starts from its template's level, not from the PC's XP.
    const std::int32_t companionXpBefore =
        game.companions().empty() ? 0 : game.companions().front().sheet.xp;
    game.giveXp(1000);
    check(game.character().xp == xpBefore + 1000, "pc_full_xp");
    if (!game.companions().empty()) {
        logInfo(concat("party test: companion xp=", game.companions().front().sheet.xp));
        check(game.companions().front().sheet.xp == companionXpBefore + 800,
              "companion_80_percent");
    }
    game.selectCompanion(0);
    check(game.selectedCompanion() == 0, "selected");
    check(game.partyLeaderTag() == "Carth", "leader_selected");
    game.setPartyLeader("PC");
    check(game.partyLeaderTag() == "PC", "leader_back_to_pc");
    {
        const std::size_t carthEntity = game.findEntity("Carth");
        const kotor::Vec3 start = game.scene().entities[carthEntity].position;
        game.setPositionForTest({start.x + 8.0f, start.y, start.z});
        for (int i = 0; i < 120; ++i) {
            game.update(0.05f, InputState{});
        }
        const kotor::Vec3 end = game.scene().entities[carthEntity].position;
        const float before = std::sqrt((start.x - (start.x + 8.0f)) * (start.x - (start.x + 8.0f)));
        const float after = std::sqrt((end.x - (start.x + 8.0f)) * (end.x - (start.x + 8.0f))
                                      + (end.y - start.y) * (end.y - start.y));
        logInfo(concat("party test: follower distance ", before, " -> ", after));
        check(after < before, "formation_follow");
    }
    {
        const std::size_t hostile = firstHostile(game);
        if (hostile != static_cast<std::size_t>(-1)) {
            kotor::Entity& target = game.scene().entities[hostile];
            target.hitPoints = 100;
            target.maxHitPoints = 100;
            target.damageMin = 0;
            target.damageMax = 0;
            target.attackBonus = -100;
            const std::size_t carthEntity = game.findEntity("Carth");
            if (carthEntity != static_cast<std::size_t>(-1)) {
                game.scene().entities[carthEntity].position = {
                    target.position.x + 1.0f, target.position.y, target.position.z};
            }
            game.setPositionForTest({target.position.x + 1.0f, target.position.y, target.position.z});
            game.attackNearestHostile();
            // Combat rounds do not advance under a conversation.
            const auto carthAttacked = [&] {
                return hasEvent(game, "Carth hits") || hasEvent(game, "Carth misses");
            };
            for (int i = 0; i < 600 && !carthAttacked(); ++i) {
                dismissDialogue(game);
                game.update(0.05f, InputState{});
            }
            if (!carthAttacked()) {
                for (const std::string& line : game.combatLog()) {
                    logInfo(concat("party test: event ", line));
                }
            }
            check(carthAttacked(), "companion_attacks");
        }
    }

    const std::filesystem::path savePath =
        std::filesystem::temp_directory_path() / "kotor_engine_s7_party.save";
    game.setPartyLeader("Carth");
    check(game.save(savePath), "save_written");
    GameSession restored(game.installPath(), game.module());
    check(restored.load(savePath), "save_loaded");
    check(restored.partyLeaderTag() == "Carth", "leader_roundtrip");
    check(restored.companions().size() == game.companions().size(), "companion_roundtrip");
    check(restored.partyTags().size() == game.partyTags().size(), "party_roundtrip");
    if (!restored.companions().empty()) {
        check(restored.companions().front().sheet.xp == game.companions().front().sheet.xp,
              "companion_xp_roundtrip");
    }

    check(game.removePartyMember("Carth"), "leave");
    check(game.companions().empty(), "companion_removed");
    logInfo(ok ? "party test: PASS" : "party test: FAIL");
    return ok ? 0 : 10;
}

int runLootTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("loot test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    std::size_t container = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < game.scene().entities.size(); ++i) {
        // A container is a placeable with HasInventory set; danm13 ships two
        // FootLker templates with items and the flag clear (retail 00587420).
        if (game.scene().entities[i].kind == kotor::EntityKind::Placeable
            && game.scene().entities[i].hasInventory
            && !game.scene().entities[i].lootItems.empty()) {
            container = i;
            break;
        }
    }
    if (container == static_cast<std::size_t>(-1)) {
        logError("loot test: no container with items");
        return 9;
    }
    const kotor::Entity& entity = game.scene().entities[container];
    const std::size_t expected = entity.lootItems.size();
    const std::size_t inventoryBefore = game.inventory().size();
    logInfo(concat("loot test: container ", entity.tag, " items=", expected));
    game.setPositionForTest(entity.position);
    check(game.usePlaceable(container), "container_used");
    check(game.loot().size() == expected, "loot_opened");
    game.takeAllLoot();
    check(game.loot().empty(), "loot_taken");
    check(game.inventory().size() == inventoryBefore + expected, "inventory_gained");
    logInfo(ok ? "loot test: PASS" : "loot test: FAIL");
    return ok ? 0 : 10;
}

int runUiTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("ui test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    check(game.activePanel() == UiPanel::None, "starts_closed");
    game.toggleUiPanel(UiPanel::Character);
    check(game.activePanel() == UiPanel::Character, "character_open");
    game.toggleUiPanel(UiPanel::Inventory);
    check(game.activePanel() == UiPanel::Inventory, "inventory_open");
    game.toggleUiPanel(UiPanel::Inventory);
    check(game.activePanel() == UiPanel::None, "toggle_closes");
    game.setUiPanel(UiPanel::Party);
    check(std::string(uiPanelName(game.activePanel())) == "party", "party_name");
    game.setUiPanel(UiPanel::Journal);
    check(std::string(uiPanelName(game.activePanel())) == "journal", "journal_name");
    game.setUiPanel(UiPanel::Merchant);
    check(std::string(uiPanelName(game.activePanel())) == "merchant", "merchant_name");
    game.setUiPanel(UiPanel::None);
    check(std::string(uiPanelName(game.activePanel())) == "none", "none_name");
    logInfo(ok ? "ui test: PASS" : "ui test: FAIL");
    return ok ? 0 : 10;
}

} // namespace

int runVmCorpus(const Options& options)
{
    if (options.vmCorpus.empty()) {
        logError("vm corpus: no directory");
        return 92;
    }
    const std::filesystem::path directory = options.vmCorpus;
    if (!std::filesystem::is_directory(directory)) {
        logError(concat("vm corpus: not a directory: ", directory.string()));
        return 93;
    }
    bool trace = false;
    {
#if defined(_WIN32)
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, "KE_VM_TRACE") == 0 && value != nullptr) {
            trace = true;
            std::free(value);
        }
#else
        trace = std::getenv("KE_VM_TRACE") != nullptr;
#endif
    }
    try {
        const ke::script::CorpusRun run = ke::script::runNcsCorpus(
            directory, options.vmFunctional, static_cast<std::uint64_t>(options.vmBudget),
            static_cast<std::size_t>(options.vmLimit), trace, options.vmSelfAware,
            options.vmDrain);
        logInfo(concat("vm corpus: files=", run.files.size(), " returned=", run.returned,
                       " blocked=", run.blocked, " budget=", run.budget,
                       " decode_errors=", run.decodeErrors, " vm_errors=", run.vmErrors,
                       " retail_run_errors=", run.retailRunErrors,
                       " invalid_jumps=", run.invalidJumpTargets, " steps=", run.totalSteps,
                       " action_calls=", run.totalActionCalls, " max_sp=", run.maxStack,
                       " functional=", options.vmFunctional ? 1 : 0));
        if (!options.vmOut.empty()) {
            ke::script::writeCorpusJsonl(options.vmOut, run);
            logInfo(concat("vm corpus: wrote ", options.vmOut));
        }
        return (run.decodeErrors == 0 && run.vmErrors == 0 && run.invalidJumpTargets == 0) ? 0 : 94;
    } catch (const std::exception& error) {
        logError(concat("vm corpus: ", error.what()));
        return 95;
    }
}

std::string resolveInstallPath(const Options& options)
{
    if (!options.install.empty()) {
        return options.install;
    }
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "KOTOR_INSTALL") == 0 && value != nullptr) {
        std::string result = value;
        std::free(value);
        return result;
    }
#else
    if (const char* value = std::getenv("KOTOR_INSTALL"); value != nullptr) {
        return value;
    }
#endif
    return "E:/SteamLibrary/steamapps/common/swkotor";
}

int runFactionTest(GameSession& game, const std::string& scriptPath)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("faction test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };

    const FactionSystem& factions = game.factions();
    check(factions.loaded(), "loaded");
    check(factions.count() == 21, "count");
    check(factions.name(0) == "Player" && factions.name(1) == "Hostile_1"
              && factions.name(14) == "Endar_Spire" && factions.name(20) == "Sacrifice",
          "names");
    check(factions.parent(0) == -1, "parents");
    check(factions.relations() == 405, "relations");

    // repute.2da defaults: column 0 holds the name, row 0 holds player
    // relations, and the remaining cells are faction-to-faction standings.
    check(factions.reputation(0, 1) == 0, "player_hostile1");
    check(factions.reputation(0, 2) == 100, "player_friendly1");
    check(factions.reputation(0, 5) == 50, "player_neutral");
    check(factions.reputation(0, 14) == 0, "player_endar_spire");
    check(factions.reputation(0, 7) == factions.reputation(7, 0), "player_mirror");
    check(factions.reputation(1, 1) == 100, "self_friendly");
    check(factions.reputation(18, 20) == 0, "czerka_sacrifice");
    check(factions.reputation(19, 18) == 100, "zone_czerka");
    check(factions.hostile(0, 1) && !factions.hostile(0, 2)
              && !factions.hostile(0, 5) && factions.hostile(0, 14),
          "hostility");

    // GIT creatures are classified by faction rather than a hardcoded id.
    std::size_t sith = static_cast<std::size_t>(-1);
    std::size_t soldier = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < game.scene().entities.size(); ++i) {
        const kotor::Entity& entity = game.scene().entities[i];
        if (entity.kind != kotor::EntityKind::Creature) {
            continue;
        }
        if (sith == static_cast<std::size_t>(-1) && entity.faction == 1) {
            sith = i;
        }
        if (soldier == static_cast<std::size_t>(-1) && entity.faction == 4) {
            soldier = i;
        }
    }
    check(sith != static_cast<std::size_t>(-1) && soldier != static_cast<std::size_t>(-1),
          "entity_targets");
    if (sith != static_cast<std::size_t>(-1) && soldier != static_cast<std::size_t>(-1)) {
        check(game.scene().entities[sith].hostile, "sith_hostile");
        check(!game.scene().entities[soldier].hostile, "soldier_friendly");
    }

    // Script bridge: GetReputation/AdjustReputation on OBJECT_SELF and a
    // shipped faction-1 creature.
    std::ifstream scriptFile(scriptPath, std::ios::binary | std::ios::ate);
    std::vector<std::uint8_t> scriptBytes;
    if (scriptFile) {
        const std::streamoff size = scriptFile.tellg();
        scriptFile.seekg(0, std::ios::beg);
        scriptBytes.resize(static_cast<std::size_t>(size));
        if (!scriptBytes.empty()) {
            scriptFile.read(reinterpret_cast<char*>(scriptBytes.data()), size);
        }
    }
    check(!scriptBytes.empty(), "script_loaded");
    const std::uint32_t pc = game.scriptWorld().pc;
    check(game.runScriptNcsForTest("faction_script_test", scriptBytes, pc),
          "script_ran");
    check(game.scriptWorld().localNumber(pc, 0) == 0, "script_before");
    check(game.scriptWorld().localNumber(pc, 1) == 25, "script_after");
    check(game.reputation(0, 1) == 25, "script_bridge");
    game.setReputation(0, 1, 0);

    // FAC round-trip through an original save.
    const int base = game.reputation(0, 7);
    check(game.adjustReputation(0, 7, 30), "adjust");
    const int expected = std::min(100, base + 30);
    check(game.reputation(0, 7) == expected, "adjust_value");
    const std::filesystem::path saveFolder =
        std::filesystem::temp_directory_path() / "kotor-faction-test-save";
    std::error_code error;
    std::filesystem::remove_all(saveFolder, error);
    const bool saved = game.saveOriginalSave(saveFolder, "Faction Test");
    check(saved, "save");
    if (saved) {
        const kotor::OriginalSave written = kotor::readOriginalSave(saveFolder);
        FactionSystem restored;
        std::string facError;
        check(restored.loadFac(written.repute, facError), "fac_parse");
        check(restored.count() == 21 && restored.name(0) == "Player", "fac_names");
        check(restored.reputation(0, 7) == expected, "fac_value");
        check(restored.reputation(0, 1) == 0, "fac_script_reset");
        GameSession reloaded(game.installPath(), game.module());
        check(reloaded.loadOriginalSave(saveFolder), "reload");
        check(reloaded.reputation(0, 7) == expected, "fac_reload");
        check(reloaded.reputation(0, 1) == 0, "fac_reload_hostility");
    }
    std::filesystem::remove_all(saveFolder, error);
    game.setReputation(0, 7, base);

    // Parent inheritance: a child faction with no explicit cell inherits its
    // parent's row (source and target fallback).
    kotor::GffNode parentRoot(0);
    const auto factionRow = [](const std::string& name, std::int32_t parentId) {
        auto node = std::make_shared<kotor::GffNode>(0);
        node->addString("FactionName", name);
        node->addInt32("FactionParentID", parentId);
        node->addInt32("FactionGlobal", 1);
        return node;
    };
    parentRoot.addList("FactionList", {
        factionRow("Player", -1), factionRow("Parent", -1),
        factionRow("Child", 1)});
    const auto repRow = [](std::int32_t a, std::int32_t b, std::int32_t value) {
        auto node = std::make_shared<kotor::GffNode>(0);
        node->addInt32("FactionID1", a);
        node->addInt32("FactionID2", b);
        node->addInt32("FactionRep", value);
        return node;
    };
    parentRoot.addList("RepList", {
        repRow(0, 0, 50), repRow(0, 1, 0), repRow(1, 0, 0),
        repRow(1, 1, 100), repRow(2, 2, 100)});
    const std::vector<std::uint8_t> parentBytes = kotor::writeGff("FAC ", parentRoot);
    FactionSystem parented;
    std::string parentError;
    check(parented.loadFac(parentBytes, parentError), "fac_parent_parse");
    check(parented.count() == 3 && parented.parent(2) == 1, "fac_parent_id");
    check(parented.reputation(2, 0) == 0, "fac_parent_source");
    check(parented.reputation(0, 2) == 0, "fac_parent_target");
    check(parented.reputation(2, 1) == 100, "fac_parent_friendly");
    check(parented.setParent(2, 0), "fac_parent_set");
    check(parented.reputation(2, 0) == 50, "fac_parent_set_value");
    FactionSystem parentRestored;
    check(parentRestored.loadFac(parented.saveFac(), parentError), "fac_parent_save");
    check(parentRestored.parent(2) == 0
              && parentRestored.reputation(2, 0) == 50,
          "fac_parent_roundtrip");

    logInfo(concat("faction test: count=", factions.count(),
                   " relations=", factions.relations(),
                   " player/hostile1=", factions.reputation(0, 1),
                   " player/friendly1=", factions.reputation(0, 2),
                   " player/endar=", factions.reputation(0, 14)));
    logInfo(ok ? "faction test: PASS" : "faction test: FAIL");
    return ok ? 0 : 67;
}

int runSaveMatrixTest(GameSession& base, const std::string& fixtureDirectory)
{
    bool ok = true;
    const auto check = [&](bool condition, const std::string& label) {
        logInfo(concat("save matrix: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    const auto near = [](kotor::Vec3 a, kotor::Vec3 b, float tolerance) {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz <= tolerance * tolerance;
    };
    const auto sameJournal = [](const auto& a, const auto& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (const auto& [key, value] : a) {
            const auto it = b.find(key);
            if (it == b.end() || it->second != value) {
                return false;
            }
        }
        return true;
    };

    const std::filesystem::path root(fixtureDirectory);
    std::vector<std::filesystem::path> folders;
    std::error_code error;
    if (std::filesystem::is_directory(root, error)) {
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(root, error)) {
            if (entry.is_directory()
                && std::filesystem::is_regular_file(entry.path() / "SAVEGAME.sav")) {
                folders.push_back(entry.path());
            }
        }
    }
    std::sort(folders.begin(), folders.end());
    check(folders.size() >= 7, "fixture_count");
    if (folders.empty()) {
        logInfo(ok ? "save matrix: PASS" : "save matrix: FAIL");
        return ok ? 0 : 68;
    }

    // Unique per run: kotor_save_matrix and kotor_differential both run this, and a shared
    // folder lets one run delete or overwrite the other's files.
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path()
        / ("kotor-save-matrix-"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(scratch, error);
    struct ScratchCleanup {
        std::filesystem::path path;
        ~ScratchCleanup()
        {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } scratchCleanup{scratch};
    std::size_t loaded = 0;
    std::size_t roundTrips = 0;
    for (const std::filesystem::path& folder : folders) {
        const std::string name = folder.filename().string();
        try {
            const kotor::OriginalSave source = kotor::readOriginalSave(folder);
            GameSession game(base.installPath(), source.module);
            const bool loadOk = game.loadOriginalSave(folder);
            check(loadOk, "load_" + name);
            if (!loadOk) {
                continue;
            }
            ++loaded;
            check(game.module() == source.module, "module_" + name);
            check(game.gold() == source.gold, "gold_" + name);
            check(game.inventory().size() == source.inventory.size(),
                  "inventory_" + name);
            // The PC's Equip_ItemList: each worn item keeps its slot (the struct type is the slot mask,
            // 1 body, 4 right hand, 5 left hand ...), so the body armour drives the model and the weapons
            // sit in the hands. The retail Taris save wears a long sword, a short vibroblade and armour.
            for (const kotor::SavedInventoryItem& worn : source.inventory) {
                if (!worn.equipped) {
                    continue;
                }
                const bool found = std::any_of(game.inventory().begin(), game.inventory().end(),
                                               [&](const InventoryItem& item) {
                    return item.equipped && item.resref == worn.resref && item.slot == worn.slot;
                });
                check(worn.slot >= 0 && found, "equipped_" + worn.resref + "_" + name);
                if (worn.slot != 1) {
                    continue;
                }
                // The Body slot's item picks the body model and map (00697610: its UTI's BodyVariation
                // and TextureVar). Retail's PC loaded from the Taris save wears g_a_class4001 (variation
                // 3, texture 1: modelc / the c map "01"), not the bare modela, so the avatar carries it.
                kotor::Install armourInstall(base.installPath(), false);
                const kotor::BodyVariant want = kotor::armorBodyVariant(armourInstall, worn.resref);
                check(want.variation >= 1 && game.wornBodyVariant() == want,
                      "body_variant_" + worn.resref + "_" + name);
                game.setThirdPerson(true);
                const kotor::Entity* avatar = nullptr;
                for (const kotor::Entity& candidate : game.scene().entities) {
                    if (candidate.tag == "PC") {
                        avatar = &candidate;
                    }
                }
                check(avatar != nullptr && avatar->bodyVariant == want, "body_avatar_" + worn.resref + "_" + name);
            }
            check(sameJournal(game.journal(), source.journal), "journal_" + name);
            check(game.partyTags().size() == source.partyTags.size(),
                  "party_" + name);
            check(game.companions().size() == source.partyTags.size(),
                  "companions_" + name);
            check(near(game.position(), source.position, 1.0f), "position_" + name);

            std::vector<bool> matchedUsed(game.scene().entities.size(), false);
            const auto kindMatches = [](kotor::SavedObjectState::Kind saved,
                                        kotor::EntityKind actual) {
                switch (saved) {
                case kotor::SavedObjectState::Kind::Creature: return actual == kotor::EntityKind::Creature;
                case kotor::SavedObjectState::Kind::Door: return actual == kotor::EntityKind::Door;
                case kotor::SavedObjectState::Kind::Placeable: return actual == kotor::EntityKind::Placeable;
                case kotor::SavedObjectState::Kind::Trigger: return actual == kotor::EntityKind::Trigger;
                case kotor::SavedObjectState::Kind::Store: return actual == kotor::EntityKind::Store;
                case kotor::SavedObjectState::Kind::Encounter: return actual == kotor::EntityKind::Encounter;
                case kotor::SavedObjectState::Kind::Waypoint: return actual == kotor::EntityKind::Waypoint;
                case kotor::SavedObjectState::Kind::Sound: return actual == kotor::EntityKind::Sound;
                case kotor::SavedObjectState::Kind::Other: return true;
                }
                return true;
            };
            std::size_t matched = 0;
            std::size_t mismatched = 0;
            std::vector<std::string> mismatchDetails;
            for (const kotor::SavedObjectState& state : source.objects) {
                std::size_t index = static_cast<std::size_t>(-1);
                float bestDistance = 1.0e30f;
                for (std::size_t i = 0; i < game.scene().entities.size(); ++i) {
                    const kotor::Entity& candidate = game.scene().entities[i];
                    if (matchedUsed[i] || candidate.tag != state.tag
                        || !kindMatches(state.kind, candidate.kind)) {
                        continue;
                    }
                    const float dx = candidate.position.x - state.position.x;
                    const float dy = candidate.position.y - state.position.y;
                    const float dz = candidate.position.z - state.position.z;
                    const float distance = dx * dx + dy * dy + dz * dz;
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        index = i;
                    }
                }
                if (index == static_cast<std::size_t>(-1)) {
                    continue;
                }
                matchedUsed[index] = true;
                ++matched;
                const kotor::Entity& entity = game.scene().entities[index];
                std::string why;
                if (!near(entity.position, state.position, 0.05f)) {
                    why += "position(" + std::to_string(entity.position.x) + ","
                        + std::to_string(entity.position.y) + ","
                        + std::to_string(entity.position.z) + " vs "
                        + std::to_string(state.position.x) + ","
                        + std::to_string(state.position.y) + ","
                        + std::to_string(state.position.z) + ") ";
                }
                if (entity.dead != state.dead) {
                    why += "dead ";
                }
                if (state.kind == kotor::SavedObjectState::Kind::Creature
                    && entity.hitPoints != state.hitPoints) {
                    why += "hp ";
                }
                if (state.kind == kotor::SavedObjectState::Kind::Door
                    && (entity.open != state.open || entity.locked != state.locked)) {
                    why += "door ";
                }
                if (state.kind == kotor::SavedObjectState::Kind::Placeable
                    && entity.lootItems != state.loot) {
                    why += "loot ";
                }
                if (!why.empty()) {
                    ++mismatched;
                    if (mismatchDetails.size() < 8) {
                        mismatchDetails.push_back(state.tag + " [" + why + "]");
                    }
                }
            }
            for (const std::string& detail : mismatchDetails) {
                logWarn(concat("save matrix: ", name, " object mismatch ", detail));
            }
            logInfo(concat("save matrix: ", name, " sourceObjects=", source.objects.size(),
                           " matched=", matched));
            const bool synthetic = name.find("SYNTH") != std::string::npos;
            check(synthetic || source.objects.empty() || matched > 0,
                  "objects_matched_" + name);
            check(mismatched == 0, "objects_state_" + name);

            const std::filesystem::path out = scratch / folder.filename();
            std::filesystem::remove_all(out, error);
            const bool saved = game.saveOriginalSave(out, source.saveName);
            if (!saved && !game.combatLog().empty()) {
                logWarn(concat("save matrix: ", name, " ", game.combatLog().back()));
            }
            check(saved, "save_" + name);
            if (!saved) {
                continue;
            }
            const kotor::OriginalSave written = kotor::readOriginalSave(out);
            check(written.module == source.module, "written_module_" + name);
            check(written.area == source.area, "written_area_" + name);
            check(written.gold == source.gold, "written_gold_" + name);
            check(written.inventory.size() == source.inventory.size(),
                  "written_inventory_" + name);
            check(sameJournal(written.journal, source.journal),
                  "written_journal_" + name);
            check(written.partyTags == source.partyTags, "written_party_" + name);
            check(!written.hasUseTemplates, "written_flattened_" + name);
            check(near(written.position, game.position(), 0.05f),
                  "written_position_" + name);
            GameSession reloaded(base.installPath(), source.module);
            const bool reloadOk = reloaded.loadOriginalSave(out);
            check(reloadOk, "reload_" + name);
            if (reloadOk) {
                check(reloaded.gold() == source.gold, "reload_gold_" + name);
                check(reloaded.inventory().size() == source.inventory.size(),
                      "reload_inventory_" + name);
                check(reloaded.journal().size() == source.journal.size(),
                      "reload_journal_" + name);
                check(reloaded.partyTags().size() == source.partyTags.size(),
                      "reload_party_" + name);
                ++roundTrips;
            }
        } catch (const std::exception& exception) {
            logWarn(concat("save matrix: ", name, ": ", exception.what()));
            check(false, "exception_" + name);
        }
    }
    // External <module>.rsv shadowing and a corrupt EventQueue in the cached
    // module IFO. Both use a temp copy so the retail fixtures stay untouched.
    const auto vanilla = std::find_if(folders.begin(), folders.end(), [](const auto& folder) {
        return folder.filename().string().find("VANILLA") != std::string::npos;
    });
    if (vanilla != folders.end()) {
        try {
            const kotor::OriginalSave source = kotor::readOriginalSave(*vanilla);
            const std::filesystem::path rsvRoot = scratch / "rsv";
            std::filesystem::create_directories(rsvRoot, error);
            const kotor::Archive outer =
                kotor::Archive::open(*vanilla / "SAVEGAME.sav");
            const kotor::ArchiveEntry* moduleEntry = nullptr;
            for (const kotor::ArchiveEntry& entry : outer.entries()) {
                if (entry.type == 2057) {
                    moduleEntry = &entry;
                    break;
                }
            }
            check(moduleEntry != nullptr, "rsv_module_entry");
            if (moduleEntry != nullptr) {
                const std::vector<std::uint8_t> nestedBytes = outer.read(*moduleEntry);
                const std::filesystem::path nestedPath = scratch / "nested.sav";
                {
                    std::ofstream nested(nestedPath, std::ios::binary | std::ios::trunc);
                    nested.write(reinterpret_cast<const char*>(nestedBytes.data()),
                                 static_cast<std::streamsize>(nestedBytes.size()));
                }
                const kotor::Archive nested = kotor::Archive::open(nestedPath);
                const auto writeRsv = [&](float x, bool corrupt, const std::filesystem::path& out) {
                    const kotor::ArchiveEntry* ifoEntry = nullptr;
                    for (const kotor::ArchiveEntry& entry : nested.entries()) {
                        if (entry.type == 2014) {
                            ifoEntry = &entry;
                            break;
                        }
                    }
                    if (ifoEntry == nullptr) {
                        return false;
                    }
                    const kotor::Gff ifo = kotor::Gff::parse(nested.read(*ifoEntry), "Module.ifo");
                    const kotor::GffNodePtr node = kotor::GffNode::fromGff(ifo);
                    node->remove("Mod_Entry_X");
                    node->addFloat("Mod_Entry_X", x);
                    if (corrupt) {
                        node->remove("EventQueue");
                        node->addList("EventQueue", {std::make_shared<kotor::GffNode>(0)});
                    }
                    const std::vector<std::uint8_t> ifoBytes =
                        kotor::writeGff("IFO ", *node);
                    std::vector<kotor::ErfWriteEntry> entries;
                    for (const kotor::ArchiveEntry& entry : nested.entries()) {
                        entries.push_back({entry.resref, entry.type,
                                           entry.type == 2014 ? ifoBytes : nested.read(entry)});
                    }
                    const std::vector<std::uint8_t> rsvBytes =
                        kotor::writeErf(entries, "MOD ", "V1.0");
                    std::ofstream file(out, std::ios::binary | std::ios::trunc);
                    file.write(reinterpret_cast<const char*>(rsvBytes.data()),
                               static_cast<std::streamsize>(rsvBytes.size()));
                    return static_cast<bool>(file);
                };
                const std::filesystem::path rsvPath =
                    rsvRoot / (source.module + ".rsv");
                check(writeRsv(321.0f, false, rsvPath), "rsv_write");
                GameSession rsvGame(base.installPath(), source.module);
                rsvGame.setRsvRootForTest(rsvRoot);
                const bool rsvLoaded = rsvGame.loadOriginalSave(*vanilla);
                check(rsvLoaded, "rsv_load");
                if (rsvLoaded) {
                    check(std::fabs(rsvGame.position().x - 321.0f) < 0.5f,
                          "rsv_shadow");
                }
                check(writeRsv(456.0f, true, rsvPath), "eventqueue_write");
                GameSession queued(base.installPath(), source.module);
                queued.setRsvRootForTest(rsvRoot);
                const bool queueLoaded = queued.loadOriginalSave(*vanilla);
                check(queueLoaded, "eventqueue_load");
                if (queueLoaded) {
                    check(std::fabs(queued.position().x - 456.0f) < 0.5f,
                          "eventqueue_shadow");
                }
            }
        } catch (const std::exception& exception) {
            logWarn(concat("save matrix: rsv/eventqueue: ", exception.what()));
            check(false, "rsv_exception");
        }
    } else {
        check(false, "rsv_fixture");
    }
    std::filesystem::remove_all(scratch, error);
    logInfo(concat("save matrix: fixtures=", folders.size(), " loaded=", loaded,
                   " roundTrips=", roundTrips));
    logInfo(ok ? "save matrix: PASS" : "save matrix: FAIL");
    return ok ? 0 : 68;
}

int runDialogueCorpusTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("dialogue corpus: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    kotor::Install install(game.installPath());
    const auto lowerKey = [](std::string text) {
        for (char& ch : text) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return text;
    };
    std::set<std::string> seen;
    std::size_t dlgs = 0;
    std::size_t entries = 0;
    std::size_t replies = 0;
    std::size_t scripts = 0;
    std::size_t sounds = 0;
    std::size_t cameraModels = 0;
    std::size_t camVidEffects = 0;
    std::size_t links = 0;
    std::size_t invalidLinks = 0;
    std::size_t parseFailures = 0;
    const auto parseOne = [&](const std::string& name, std::vector<std::uint8_t> data) {
        if (!seen.insert(lowerKey(name)).second) {
            return;
        }
        try {
            const kotor::Gff gff = kotor::Gff::parse(std::move(data), name + ".dlg");
            const kotor::Dlg dlg = kotor::Dlg::parse(gff, nullptr, name);
            ++dlgs;
            entries += dlg.entries.size();
            replies += dlg.replies.size();
            if (!dlg.cameraModel.empty()) {
                ++cameraModels;
            }
            for (const std::uint32_t start : dlg.starting) {
                ++links;
                invalidLinks += start >= dlg.entries.size() ? 1 : 0;
            }
            for (const kotor::DlgEntry& entry : dlg.entries) {
                for (const std::uint32_t reply : entry.replies) {
                    ++links;
                    invalidLinks += reply >= dlg.replies.size() ? 1 : 0;
                }
                scripts += entry.script.empty() ? 0 : 1;
                sounds += entry.sound.empty() ? 0 : 1;
                camVidEffects += entry.camVidEffect > 0 ? 1 : 0;
            }
            for (const kotor::DlgReply& reply : dlg.replies) {
                for (const std::uint32_t entry : reply.entries) {
                    ++links;
                    invalidLinks += entry >= dlg.entries.size() ? 1 : 0;
                }
                scripts += reply.script.empty() ? 0 : 1;
                sounds += reply.sound.empty() ? 0 : 1;
                camVidEffects += reply.camVidEffect > 0 ? 1 : 0;
            }
        } catch (const kotor::FormatError& exception) {
            ++parseFailures;
            if (parseFailures <= 5) {
                logWarn(concat("dialogue corpus: ", name, ": ", exception.what()));
            }
        }
    };

    for (const std::string& name : install.resourceNames(2029)) {
        if (const auto data = install.find(name, 2029); data.has_value()) {
            parseOne(name, *data);
        }
    }
    std::error_code error;
    const std::filesystem::path modules =
        std::filesystem::path(game.installPath()) / "modules";
    if (std::filesystem::is_directory(modules, error)) {
        for (const std::filesystem::directory_entry& item :
             std::filesystem::directory_iterator(modules, error)) {
            if (error || !item.is_regular_file()) {
                continue;
            }
            const std::string extension = item.path().extension().string();
            if (extension != ".rim" && extension != ".mod") {
                continue;
            }
            try {
                const kotor::Archive archive = kotor::Archive::open(item.path());
                for (const kotor::ArchiveEntry& entry : archive.entries()) {
                    if (entry.type == 2029) {
                        parseOne(entry.resref, archive.read(entry));
                    }
                }
            } catch (const kotor::FormatError&) {
            }
        }
    }
    check(parseFailures == 0, "parse");
    check(dlgs > 1000, "dlgs");
    check(entries > 0, "entries");
    check(replies > 0, "replies");
    check(scripts > 0, "scripts");
    check(sounds > 0, "sounds");
    check(cameraModels > 0, "camera_models");
    check(camVidEffects == 0, "camvid_defaults");
    check(links > 0 && invalidLinks == 0, "links");

    // Non-default CamVidEffect (none ship in the corpus). Retail applies the
    // value only through a static camera (CameraAngle 6): the row becomes the
    // video effect, a missing row is ignored, -1 leaves the state alone, any
    // other camera disables it, and ending the conversation disables it.
    const auto camVidDialogue = [&](std::int32_t effect, std::int32_t angle, std::int32_t cameraId) {
        kotor::Dlg dlg;
        kotor::DlgEntry node;
        node.text = "CamVid fixture";
        node.camVidEffect = effect;
        node.cameraAngle = angle;
        node.cameraId = cameraId;
        dlg.entries.push_back(node);
        game.setDialogueForTest(std::move(dlg), "camvid_fixture", "Tester");
    };
    game.disableVideoEffect();
    const int rejectedBefore = game.unsupportedCamVidEffects();
    camVidDialogue(1, 6, 1);
    check(game.dialogue().camVidEffect == 1, "camvid_fixture_effect");
    check(game.videoEffect().row == 1 && game.videoEffect().saturation
              && game.videoEffect().scanNoise,
          "camvid_fixture_applied");
    check(game.unsupportedCamVidEffects() == rejectedBefore, "camvid_fixture_not_rejected");
    game.endDialogue();
    check(!game.videoEffect().active(), "camvid_end_disables");
    camVidDialogue(2, 6, 1);
    check(game.videoEffect().row == 2, "camvid_row2_applied");
    game.endDialogue();
    camVidDialogue(0, 6, 1);
    check(game.videoEffect().row == 0, "camvid_row0_applied");
    game.endDialogue();
    camVidDialogue(99, 6, 1);
    check(!game.videoEffect().active()
              && game.unsupportedCamVidEffects() == rejectedBefore + 1,
          "camvid_undefined_row_rejected");
    game.endDialogue();
    camVidDialogue(-1, 6, 1);
    check(!game.videoEffect().active(), "camvid_default_leaves_off");
    game.endDialogue();
    game.enableVideoEffect(1);
    camVidDialogue(-1, 6, 1);
    check(game.videoEffect().row == 1, "camvid_default_keeps_running_effect");
    game.endDialogue();
    game.enableVideoEffect(1);
    camVidDialogue(1, -1, -1);
    check(!game.videoEffect().active(), "camvid_non_static_camera_disables");
    game.endDialogue();
    camVidDialogue(1, 6, 1);
    game.clearDialogueForTest();
    check(!game.videoEffect().active(), "camvid_any_dialogue_exit_disables");
    logInfo(concat("dialogue corpus: dlgs=", dlgs, " entries=", entries,
                   " replies=", replies, " scripts=", scripts,
                   " sounds=", sounds, " cameraModels=", cameraModels,
                   " camVidEffects=", camVidEffects, " links=", links,
                   " invalidLinks=", invalidLinks));
    logInfo(ok ? "dialogue corpus: PASS" : "dialogue corpus: FAIL");
    return ok ? 0 : 69;
}

int runModulePlaythroughMatrix(const std::string& installPath)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("module playthrough: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    std::vector<std::string> modules;
    std::error_code moduleError;
    const std::filesystem::path modulesRoot =
        std::filesystem::path(installPath) / "modules";
    if (std::filesystem::is_directory(modulesRoot, moduleError)) {
        for (const std::filesystem::directory_entry& item :
             std::filesystem::directory_iterator(modulesRoot, moduleError)) {
            if (moduleError || !item.is_regular_file()
                || item.path().extension() != ".rim") {
                continue;
            }
            const std::string stem = item.path().stem().string();
            if (stem.ends_with("_s") || stem.ends_with("_a")
                || stem.ends_with("_adx")) {
                continue;
            }
            modules.push_back(stem);
        }
    }
    std::sort(modules.begin(), modules.end());
    modules.erase(std::unique(modules.begin(), modules.end()), modules.end());
    check(modules.size() == 117, "module_count");
    std::size_t loaded = 0;
    std::size_t dialogues = 0;
    std::size_t conversationAttempts = 0;
    std::size_t conversationFailures = 0;
    std::size_t knownMissing = 0;
    std::size_t noStartLink = 0;
    std::size_t lines = 0;
    std::size_t warnings = 0;
    // Shipped KotOR 1 data references one DLG that is absent from the VFS;
    // retail cannot start it either. It is an explicit data anomaly, not an
    // engine failure.
    const std::set<std::string> knownMissingConversations = {"end_carth001"};
    for (const std::string& module : modules) {
        try {
            GameSession game(installPath, module);
            ++loaded;
            warnings += game.scene().warnings.size();
            check(!game.scene().area.empty(), "area");
            check(!game.scene().rooms.empty(), "rooms");
            for (int frame = 0; frame < 120; ++frame) {
                game.update(1.0f / 60.0f, InputState{});
            }
            const kotor::Vec3 position = game.position();
            check(std::isfinite(position.x) && std::isfinite(position.y)
                      && std::isfinite(position.z),
                  "position");
            std::size_t dialogueLines = 0;
            std::size_t moduleDialogues = 0;
            std::set<std::string> seenConversations;
            const std::string startModule = game.module();
            bool transitioned = false;
            for (std::size_t i = 0; i < game.scene().entities.size() && !transitioned; ++i) {
                // Copied: a conversation's scripts can spawn and move the entity vector.
                const std::string conversation = game.scene().entities[i].conversation;
                if (conversation.empty() || game.scene().entities[i].dead) {
                    continue;
                }
                std::string key = conversation;
                for (char& ch : key) {
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }
                if (!seenConversations.insert(key).second) {
                    continue;
                }
                ++conversationAttempts;
                if (!game.startDialogue(i)) {
                    if (knownMissingConversations.contains(key)) {
                        ++knownMissing;
                        logInfo(concat("module playthrough: ", module,
                                       " known missing dialogue ", conversation));
                    } else if (game.dialogueHadNoStart()) {
                        // Every starting link's Active failed in this world state.
                        // Retail shows nothing here; it is not a load failure.
                        ++noStartLink;
                    } else {
                        ++conversationFailures;
                        logWarn(concat("module playthrough: ", module,
                                       " conversation load failed ", conversation));
                    }
                    continue;
                }
                ++dialogues;
                ++moduleDialogues;
                std::size_t conversationLines = 0;
                for (int step = 0; step < 64 && game.dialogue().active; ++step) {
                    ++dialogueLines;
                    ++conversationLines;
                    game.chooseDialogueOption(0);
                    if (game.module() != startModule) {
                        transitioned = true;
                        break;
                    }
                }
                const bool completed = !game.dialogue().active;
                logInfo(concat("module playthrough: ", module,
                               " dialogue=", conversation,
                               " lines=", conversationLines,
                               " completed=", completed ? 1 : 0,
                               " transitioned=", transitioned ? 1 : 0));
                if (game.dialogue().active) {
                    game.endDialogue();
                }
            }
            lines += dialogueLines;
            logInfo(concat("module playthrough: ", module, " area=", game.scene().area,
                           " entities=", game.scene().entities.size(),
                           " conversations=", moduleDialogues,
                           " dialogueLines=", dialogueLines,
                           " transitioned=", transitioned ? 1 : 0,
                           " warnings=", game.scene().warnings.size()));
        } catch (const std::exception& exception) {
            logWarn(concat("module playthrough: ", module, ": ", exception.what()));
            check(false, "exception");
        }
    }
    check(loaded == modules.size(), "modules");
    check(dialogues > 0, "dialogues");
    check(conversationFailures == 0, "conversation_load");
    check(conversationAttempts == dialogues + knownMissing + noStartLink,
          "conversation_attempts");
    check(knownMissing == 1, "known_missing");
    check(lines > 0, "lines");
    check(warnings == 0, "warnings");
    logInfo(concat("module playthrough: modules=", loaded, " dialogues=", dialogues,
                   " attempts=", conversationAttempts,
                   " failures=", conversationFailures,
                   " knownMissing=", knownMissing, " noStartLink=", noStartLink,
                   " lines=", lines, " warnings=", warnings));
    logInfo(ok ? "module playthrough: PASS" : "module playthrough: FAIL");
    return ok ? 0 : 71;
}

int runChargenTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("chargen: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    Chargen chargen(game.rules());
    check(chargen.classCount() == 3, "class_count");
    check(chargen.selectClass(1), "select_scout");
    check(chargen.classId() == 1, "class_id");
    check(chargen.pointsRemaining() == 30, "points_start");
    for (int i = 0; i < 2; ++i) {
        check(chargen.adjustAttribute(0, 1), "str_plus");
        check(chargen.adjustAttribute(1, 1), "dex_plus");
        check(chargen.adjustAttribute(2, 1), "con_plus");
    }
    check(chargen.adjustAttribute(3, 1), "int_plus");
    check(chargen.attribute(0) == 10 && chargen.attribute(1) == 10
              && chargen.attribute(2) == 10 && chargen.attribute(3) == 9,
          "attributes");
    check(chargen.pointsRemaining() == 23, "points_spent");

    // Recommended (the abilities, skills and feats steps): each base class's own row of the shipped
    // tables. The class rows of classes.2da are full spends of the 30 points, so the proof is that the
    // pool lands on 0; the skills and feats then use their pools within the caps.
    for (int classId = 0; classId < chargen.classCount(); ++classId) {
        Chargen recommended(game.rules());
        check(recommended.selectClass(classId), "recommended_select_class");
        const std::array<std::int32_t, 6> wanted = game.rules().recommendedAbilities(classId);
        check(recommended.applyRecommended(Chargen::Step::Abilities), "recommended_abilities_apply");
        bool same = true;
        for (int i = 0; i < 6; ++i) {
            same = same && recommended.attribute(i) == wanted[static_cast<std::size_t>(i)];
        }
        check(same && recommended.pointsRemaining() == 0, "recommended_abilities_spend_all_30_points");
        check(recommended.applyRecommended(Chargen::Step::Skills), "recommended_skills_apply");
        int ranks = 0;
        bool withinCaps = true;
        for (int i = 0; i < game.rules().skillCount(); ++i) {
            ranks += recommended.skillRank(i);
            withinCaps = withinCaps && recommended.skillRank(i) <= recommended.skillRankCap(i);
        }
        check(ranks > 0 && withinCaps && recommended.skillPointsRemaining() >= 0, "recommended_skills_spend_points");
        check(recommended.applyRecommended(Chargen::Step::Feats), "recommended_feats_apply");
        check(recommended.featPointsRemaining() >= 0 && recommended.featPointsRemaining() < 2, "recommended_feats_fill_the_pool");
    }
    check(!chargen.applyRecommended(Chargen::Step::Portrait), "recommended_only_where_the_screen_has_it");

    int awareness = -1;
    for (int i = 0; i < game.rules().skillCount(); ++i) {
        if (game.rules().skillName(i) == "Awareness") {
            awareness = i;
            break;
        }
    }
    check(awareness >= 0, "skill_awareness");
    for (int i = 0; i < 4 && awareness >= 0; ++i) {
        check(chargen.adjustSkill(awareness, 1), "skill_plus");
    }
    check(awareness >= 0 && chargen.skillRank(awareness) == 4, "skill_rank");
    check(chargen.skillPointsRemaining() >= 0, "skill_points");

    const std::vector<std::int32_t> feats = chargen.availableFeats();
    check(!feats.empty(), "feat_choices");
    std::int32_t chosenFeat = -1;
    if (!feats.empty()) {
        chosenFeat = feats.front();
        check(chargen.toggleFeat(chosenFeat), "feat_select");
        check(chargen.featSelected(chosenFeat), "feat_selected");
    }
    check(game.rules().portraitCount() > 0, "portrait_table");
    if (game.rules().portraitCount() > 0) {
        chargen.cyclePortrait(1);
        check(chargen.portraitIndex() == 1, "portrait_cycle");
    }
    chargen.setName("Revan");
    check(chargen.name() == "REVAN", "name_upper");

    const CharacterSheet sheet = chargen.build();
    check(sheet.classId == 1, "sheet_class");
    check(sheet.name == "REVAN", "sheet_name");
    check(sheet.str == 10 && sheet.dex == 10 && sheet.con == 10
              && sheet.intel == 9, "sheet_attributes");
    check(sheet.maxHp == game.rules().maxHitPoints(1, 1, 10), "sheet_hp");
    check(awareness >= 0
              && sheet.skills.at("Awareness") == 4, "sheet_skill");
    check(chosenFeat < 0 || sheet.feats.contains(chosenFeat), "sheet_feat");

    game.applyCharacter(sheet);
    check(game.character().name == "REVAN", "applied_name");
    check(game.character().classId == 1, "applied_class");
    game.setPlayerAppearance(static_cast<std::uint32_t>(std::max(0, chargen.appearanceId())),
                                     chargen.name());
    game.setThirdPerson(true);
    check(game.playerEntity() != static_cast<std::size_t>(-1), "player_entity");
    check(!game.scene().entityMeshes.empty(), "player_mesh");

    const std::filesystem::path temp = std::filesystem::temp_directory_path()
        / "kotor-s14-chargen";
    std::error_code error;
    std::filesystem::remove_all(temp, error);
    const std::filesystem::path savePath = temp / "player.txt";
    check(game.save(savePath), "save");
    GameSession restored(game.installPath(), game.module());
    check(restored.load(savePath), "load");
    check(restored.character().name == "REVAN", "loaded_name");
    check(restored.character().classId == 1, "loaded_class");
    check(restored.character().str == 10, "loaded_str");
    check(restored.character().skills.contains("Awareness")
              && restored.character().skills.at("Awareness") == 4, "loaded_skill");

    const std::int32_t oldLevel = game.character().level;
    game.giveXp(game.rules().xpForLevel(2) + 1);
    check(game.character().level == oldLevel + 1, "level_up");
    check(game.character().skillPoints > 0, "level_skill_points");
    check(game.character().featPoints > 0, "level_feat_points");
    const auto skillBefore = game.character().skills.find("Awareness");
    const std::int32_t rankBefore = skillBefore != game.character().skills.end()
        ? skillBefore->second : 0;
    check(game.spendSkillPoint("Awareness"), "spend_skill");
    check(game.character().skills.at("Awareness") == rankBefore + 1, "skill_spent");
    game.giveXp(game.rules().xpForLevel(4) + 1);
    check(game.character().level >= 4, "level_four");
    check(game.character().attributePoints > 0, "attribute_points");
    const std::int32_t strBefore = game.character().str;
    check(game.spendAttributePoint(0), "spend_attribute");
    check(game.character().str == strBefore + 1, "attribute_spent");

    {
        // Retail 006dc3c0 builds the chargen bodies with body variation 2 / texture 1 (the class clothes); the old
        // preview left classsel and portcust on the row's bare model. 006100f0: the sheet camera rides charrec_light's
        // camerahook; the retail sheet frames at alignment 10, 50 and 90 (cs_al10, cs_before, cs_al90) keep one framing.
        ChargenPreview preview(game.install(), game.rules());
        check(chargenBodyVariant() == kotor::BodyVariant{2, 1}, "chargen_body_clothes");
        preview.showClassSelection(game.rules());
        check(preview.bodyVariant() == chargenBodyVariant() && preview.entityForTag("3D_MODEL1") >= 0, "classsel_dressed");
        const std::int32_t headAppearance = preview.appearanceForTag("3D_MODEL3");
        preview.showHead(headAppearance, 0);
        check(preview.bodyVariant() == chargenBodyVariant(), "portcust_dressed");
        preview.showSheet(headAppearance, chargenBodyVariant(), 50);
        const float neutral = preview.sheetCamera().eye.z;
        std::size_t smoke = 0;
        for (const kotor::EmitterInstance& emitter : preview.scene().emitters) {
            smoke += emitter.particles.size();
        }
        logInfo(concat("chargen: sheet smoke particles at open ", smoke));
        check(smoke > 50, "sheet_smoke_at_open"); // the hub and the sheet open on smoke, not a black box
        preview.showSheet(headAppearance, chargenBodyVariant(), 0);
        const float dark = preview.sheetCamera().eye.z;
        preview.showSheet(headAppearance, chargenBodyVariant(), 100);
        const float light = preview.sheetCamera().eye.z;
        logInfo(concat("chargen: sheet eye z dark ", dark, " neutral ", neutral, " light ", light));
        // 006100f0 attaches the camera to the hook the clip keys: evil 2.3 m above align11, good 0.77 m below it.
        check(dark > neutral + 2.0f && light < neutral - 0.7f, "sheet_camera_follows_keyed_hook");
        // 006100f0 / 0049f4f0: the camera rides the room hook (align11 keys it at -0.817) raised by the body's own
        // CAMERAHOOK height (006974d0; 1.5 to 1.7 m on the player bodies), not the old fixed 1.767.
        logInfo(concat("chargen: sheet raise ", preview.sheetRaise(), " eye z ", neutral));
        check(preview.sheetRaise() > 1.4f && preview.sheetRaise() < 1.75f && std::abs(neutral - (preview.sheetRaise() - 0.817f)) < 0.01f,
              "sheet_camera_raised_by_body_camerahook");
        preview.showSheet(headAppearance, chargenBodyVariant(), 50, "T3M4");
        check(preview.sheetHook() == "camerahookt", "sheet_hook_t3m4");
    }

    logInfo(ok ? "chargen: PASS" : "chargen: FAIL");
    return ok ? 0 : 73;
}

int runCampaignTest(GameSession& base, const std::string& fixtureDirectory)
{
    bool ok = true;
    const auto check = [&](bool condition, const std::string& label) {
        logInfo(concat("campaign: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    check(runPlaythrough(base) == 0, "endar_spire");

    const std::filesystem::path root(fixtureDirectory);
    std::vector<std::filesystem::path> folders;
    std::error_code error;
    if (std::filesystem::is_directory(root, error)) {
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(root, error)) {
            if (entry.is_directory()
                && std::filesystem::is_regular_file(entry.path() / "SAVEGAME.sav")) {
                folders.push_back(entry.path());
            }
        }
    }
    std::sort(folders.begin(), folders.end());
    check(folders.size() >= 4, "save_acts");
    std::size_t acts = 0;
    for (const std::filesystem::path& folder : folders) {
        const std::string name = folder.filename().string();
        try {
            const kotor::OriginalSave source = kotor::readOriginalSave(folder);
            GameSession game(base.installPath(), source.module);
            if (!game.loadOriginalSave(folder)) {
                check(false, "load_" + name);
                continue;
            }
            for (int frame = 0; frame < 600; ++frame) {
                game.update(1.0f / 60.0f, InputState{});
            }
            check(game.module() == source.module, "module_" + name);
            check(game.elapsedSeconds() > 9.0f, "clock_" + name);
            check(game.playerHp() > 0, "alive_" + name);
            check(!game.scene().entities.empty(), "entities_" + name);
            check(game.scene().warnings.empty(), "warnings_" + name);
            if (game.journalSize() > 0 || source.journal.empty()) {
                check(true, "journal_" + name);
            } else {
                check(false, "journal_" + name);
            }
            ++acts;
        } catch (const std::exception& exception) {
            check(false, "exception_" + name);
            logWarn(concat("campaign: ", name, ": ", exception.what()));
        }
    }
    check(acts >= 4, "acts_ran");

    for (const char* module : {"danm14aa", "kas_m22aa", "manm26aa", "korr_m33aa"}) {
        try {
            GameSession game(base.installPath(), module);
            for (int frame = 0; frame < 120; ++frame) {
                game.update(1.0f / 60.0f, InputState{});
            }
            check(!game.scene().entities.empty(), std::string("anchor_") + module);
        } catch (const std::exception& exception) {
            check(false, std::string("anchor_") + module);
            logWarn(concat("campaign: ", module, ": ", exception.what()));
        }
    }

    logInfo(ok ? "campaign: PASS" : "campaign: FAIL");
    return ok ? 0 : 74;
}

// Tiny NCS builder for the chargen gate (same encoding the objects gate uses).
class ChargenNcs {
public:
    ChargenNcs& constObject(std::uint32_t value)
    {
        op(0x04, 0x06);
        be32(value);
        return *this;
    }
    ChargenNcs& constString(const std::string& value)
    {
        op(0x04, 0x05);
        code_.push_back(static_cast<std::uint8_t>(value.size() >> 8));
        code_.push_back(static_cast<std::uint8_t>(value.size() & 0xFF));
        code_.insert(code_.end(), value.begin(), value.end());
        return *this;
    }
    ChargenNcs& action(std::uint16_t id, std::uint8_t argc)
    {
        op(0x05, 0x00);
        code_.push_back(static_cast<std::uint8_t>(id >> 8));
        code_.push_back(static_cast<std::uint8_t>(id & 0xFF));
        code_.push_back(argc);
        return *this;
    }
    std::vector<std::uint8_t> bytes()
    {
        op(0x20, 0x00); // RETN
        const std::string magic = "NCS V1.0";
        std::vector<std::uint8_t> out(magic.begin(), magic.end());
        out.push_back(0x42);
        const std::uint32_t size = static_cast<std::uint32_t>(13 + code_.size());
        for (const int shift : {24, 16, 8, 0}) {
            out.push_back(static_cast<std::uint8_t>(size >> shift));
        }
        out.insert(out.end(), code_.begin(), code_.end());
        return out;
    }

private:
    void op(std::uint8_t code, std::uint8_t qualifier)
    {
        code_.push_back(code);
        code_.push_back(qualifier);
    }
    void be32(std::uint32_t value)
    {
        for (const int shift : {24, 16, 8, 0}) {
            code_.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }
    std::vector<std::uint8_t> code_;
};

// GetGender(PC) (action 358) run through the script VM with the PC as self.
int chargenVmGender(GameSession& game)
{
    ChargenNcs script;
    // Object constant 0 is OBJECT_SELF (the PC here); the PC's own id is 1, which the VM
    // reads as OBJECT_INVALID in a compiled script.
    script.constObject(0).action(358, 1);
    script.constString("chargen_gender_out").action(581, 2); // SetGlobalNumber
    game.declareGlobalForTest("chargen_gender_out");
    game.scriptWorld().setGlobal("chargen_gender_out", script::Value::makeInt(-999));
    const bool ran = game.runScriptNcsForTest("chargen_gender", script.bytes(),
                                              game.scriptWorld().pc);
    if (!ran && !game.scriptLog().empty()) {
        logWarn(concat("chargen_gui: vm gender script: ", game.scriptLog().back()));
    }
    return static_cast<int>(game.scriptWorld()
                                .getGlobal("chargen_gender_out", script::ValueType::Int)
                                .asInt());
}

// Runs the shipped opening conversation (m01aa_c01) and returns the entries shown. The
// conversation branches on GetGender through the shipped k_pend_male / k_pend_female
// condition scripts (entries 6/5 for the small-body start, 11/10 for the medium one).
std::vector<std::uint32_t> chargenOpeningEntries(GameSession& game)
{
    std::vector<std::uint32_t> entries;
    ScriptApi api{game};
    if (!api.startConversation(game.scriptWorld().pc, "m01aa_c01")) {
        return entries;
    }
    for (int step = 0; step < 40 && game.dialogue().active; ++step) {
        if (entries.empty() || entries.back() != game.dialogue().entry) {
            entries.push_back(game.dialogue().entry);
        }
        if (!game.dialogue().options.empty()) {
            game.chooseDialogueOption(0);
        } else {
            game.update(0.5f, InputState{});
        }
    }
    return entries;
}

int runChargenGuiTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const std::string& label) {
        logInfo(concat("chargen_gui: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    kotor::Install install(game.installPath());
    install.bindModule(game.module());
    install.addPacks();
    GuiSystem gui(install);
    ChargenGuiState state(game.rules());
    PortraitImages portraits(game.install());
    check(gui.open("classsel") && gui.screenName() == "classsel", "classsel_open");
    // Real pointer events: move, press and release over the control centre, the way
    // the window loop feeds GuiSystem; the action then goes to the chargen flow.
    const auto click = [&](const std::string& tag) {
        const kotor::GuiScreen* screen = gui.screen();
        const kotor::GuiControl* hit = nullptr;
        std::function<void(const kotor::GuiControl&)> search = [&](const kotor::GuiControl& c) {
            if (hit == nullptr && c.tag == tag) {
                hit = &c;
            }
            for (const kotor::GuiControl& child : c.children) {
                search(child);
            }
        };
        if (screen != nullptr) {
            for (const kotor::GuiControl& control : screen->controls) {
                search(control);
            }
        }
        if (hit == nullptr) {
            return false;
        }
        const float x = static_cast<float>(hit->extent.left + hit->extent.width / 2);
        const float y = static_cast<float>(hit->extent.top + hit->extent.height / 2);
        gui.pointerMove(x, y);
        gui.pointerDown(x, y);
        const bool released = gui.pointerUp(x, y);
        if (!released || gui.lastAction().empty()) {
            return false;
        }
        const std::string action = gui.lastAction();
        gui.clearActions();
        return handleChargenGuiAction(action, state, gui, game);
    };
    const auto act = [&](const std::string& tag) {
        return handleChargenGuiAction(tag, state, gui, game);
    };

    // Paired frame cg_class: the focused slot names gender and class, LBL_DESC gives its line. Hovering
    // the third slot moves the focus. Fails on a binding that does not run at this step.
    {
        const auto& text = game.localization().tlk();
        bindChargenScreen(gui, state, game);
        check(gui.state("LBL_DESC").text == text.text(32109), "classsel_desc_first_slot");
        check(gui.state("LBL_CLASS").text.find(game.rules().className(Chargen::slotClass(0))) != std::string::npos,
              "classsel_class_first_slot");
        const kotor::GuiControl* third = gui.control("BTN_SEL3");
        check(third != nullptr, "classsel_third_slot_exists");
        if (third != nullptr) {
            gui.pointerMove(static_cast<float>(third->extent.left + third->extent.width / 2),
                            static_cast<float>(third->extent.top + third->extent.height / 2));
            bindChargenScreen(gui, state, game);
            check(state.classFocus == 2 && gui.state("LBL_DESC").text == text.text(32111), "classsel_hover_moves_focus");
            gui.pointerMove(-10.0f, -10.0f);
        }
    }
    // The six classsel buttons: class and gender per slot (retail 006dc3c0).
    {
        const int expectedClass[6] = {2, 1, 0, 0, 1, 2};
        const int expectedGender[6] = {0, 0, 0, 1, 1, 1};
        for (int slot = 0; slot < 6; ++slot) {
            check(click("BTN_SEL" + std::to_string(slot + 1)),
                  "slot_click_" + std::to_string(slot + 1));
            check(state.chargen.classId() == expectedClass[slot]
                      && state.chargen.gender() == expectedGender[slot],
                  "slot_class_gender_" + std::to_string(slot + 1));
            const std::vector<std::int32_t> rows =
                game.rules().pcPortraitRows(state.chargen.gender());
            check(!rows.empty() && game.rules().portraitRow(rows.front()).sex
                      == expectedGender[slot], "slot_portrait_sex_" + std::to_string(slot + 1));
            check(act("BTN_BACK") && state.step == 0 && gui.screenName() == "classsel",
                  "slot_back_" + std::to_string(slot + 1));
        }
    }

    check(click("BTN_SEL2"), "select_scout");
    check(state.chargen.classId() == 1 && state.chargen.gender() == 0, "class_id");
    // Retail 006eb420: the class opens MAINCG with Quick or Custom (QORCPNL laid in at 322,87), not the
    // portrait screen. Quick's help is up, the scores blank; the old flow fails here.
    check(state.step == kChargenHub && state.hubPanel == kHubChoice && gui.screenName() == "maincg_qorcpnl",
          "hub_after_class");
    bindChargenScreen(gui, state, game);
    check(gui.control("QUICK_CHAR_BTN") != nullptr && gui.control("QUICK_CHAR_BTN")->extent.left == 322 + 17
              && gui.control("MAIN_TITLE_LBL") != nullptr,
          "hub_composite_places_sub_panel");
    check(!gui.state("PORTRAIT_LBL").fill.empty() && gui.state("STR_AB_LBL").text.empty()
              && gui.state("LB_DESC").items.size() == 1
              && gui.state("LB_DESC").items[0].text == game.localization().tlk().text(241),
          "hub_choice_contents");
    // Quick: QUICKPNL's show applies the class template (006effd0) and MAINCG shows it (006eac30).
    check(act("QUICK_CHAR_BTN") && state.hubPanel == kHubQuick && gui.screenName() == "maincg_quickpnl",
          "hub_quick_opens");
    bindChargenScreen(gui, state, game);
    check(state.chargen.pointsRemaining() == 0 && gui.state("DEX_AB_LBL").text == std::to_string(state.chargen.attribute(1))
              && !gui.state("LBL_DEF").text.empty() && gui.state("LBL_1").marked && !gui.state("LBL_2").marked,
          "hub_quick_template_shown");
    check(act("BTN_STEPNAME2") && gui.screenName() == "maincg_quickpnl", "hub_quick_name_locked");
    check(act("BTN_BACK") && state.hubPanel == kHubChoice && state.chargen.pointsRemaining() == 30,
          "hub_quick_back_resets");
    check(act("CUST_CHAR_BTN") && state.hubPanel == kHubCustom && gui.screenName() == "maincg_custpnl"
              && state.customStep == 0,
          "hub_custom_opens");
    // CUSTPNL's LBL_NUMn carry their numbers in the file (TEXT "1".."6"); retail never sets them (006ef730).
    bindChargenScreen(gui, state, game);
    {
        const kotor::GuiControl* number = gui.control("LBL_NUM2");
        const GuiControlState shown = gui.state("LBL_NUM2");
        logInfo(concat("chargen_gui: LBL_NUM2 file text '", number != nullptr ? number->text : std::string("<none>"),
                       "' strref ", number != nullptr ? number->strref : 0u, " state text ", shown.hasText, " '",
                       shown.text, "' visible ", shown.visible));
        // The painter prefers a strref over TEXT: the file's 0xFFFFFFFF must survive loading (it read as 0x80000000).
        check(number != nullptr && shown.visible && number->strref == 0xFFFFFFFFu
                  && (shown.hasText ? shown.text : number->text) == "2",
              "hub_custom_numbers");
    }
    check(act("BTN_STEPNAME3") && gui.screenName() == "maincg_custpnl", "hub_custom_skills_locked");
    check(act("BTN_STEPNAME1") && state.step == 1 && gui.screenName() == "portcust", "portrait_screen");

    // Portrait control: the texture is the portraits.2da row's art, and cycling changes it.
    std::optional<kotor::TwoDa> portraitTable;
    if (auto data = install.find("portraits", 2017); data.has_value()) {
        portraitTable = kotor::TwoDa::parse(std::move(*data), "portraits");
    }
    check(portraitTable.has_value(), "portraits_table");
    const PortraitInfo first = state.chargen.portrait();
    check(first.row >= 0 && first.sex == 0, "portrait_row_male");
    std::string expectedResref;
    if (portraitTable.has_value() && first.row >= 0) {
        expectedResref = portraitTable->cell(portraitTable->rowLabel(
                                                 static_cast<std::size_t>(first.row)),
                                             "baseresref");
    }
    check(!expectedResref.empty() && first.resref == expectedResref, "portrait_matches_2da");
    const std::uint64_t firstDigest = portraits.digest(first.resref);
    const kotor::Image* firstImage = portraits.image(first.resref);
    check(firstImage != nullptr && firstImage->width >= 32 && firstImage->height >= 32
              && firstDigest != 0, "portrait_texture_decoded");
    check(act("BTN_ARRR"), "portrait_right");
    check(state.chargen.portraitIndex() == 1, "portrait_index");
    const PortraitInfo second = state.chargen.portrait();
    check(second.row != first.row && second.row >= 0, "portrait_row_changed");
    const std::uint64_t secondDigest = portraits.digest(second.resref);
    check(secondDigest != 0 && secondDigest != firstDigest, "portrait_art_changes");
    // The arrows cycle only this gender's rows, wrapping at the ends.
    const std::int32_t listSize = state.chargen.portraitListSize();
    check(listSize > 1 && listSize == static_cast<std::int32_t>(
                                          game.rules().pcPortraitRows(0).size()), "portrait_list");
    check(act("BTN_ARRL") && act("BTN_ARRL") && state.chargen.portraitIndex() == listSize - 1,
          "portrait_wraps");
    check(act("BTN_ARRR") && state.chargen.portraitIndex() == 0, "portrait_wraps_back");
    // Alignment art: the row's dark-side variants replace the base art below 41.
    if (!first.darkVariants[3].empty()) {
        const std::string dark = game.rules().portraitResrefFor(first, 5);
        check(dark == first.darkVariants[3] && portraits.digest(dark) != 0
                  && portraits.digest(dark) != firstDigest, "portrait_dark_variant");
        check(game.rules().portraitResrefFor(first, 50) == first.resref,
              "portrait_neutral_art");
    }
    // The body follows the portrait row and the class column (Scout = AppearanceNumber).
    check(state.chargen.appearanceId() == static_cast<std::int32_t>(first.appearance),
          "appearance_scout_column");
    check(game.rules().portraitAppearance(first.row, 0)
              == static_cast<std::int32_t>(first.appearanceLarge)
              && game.rules().portraitAppearance(first.row, 2)
                  == static_cast<std::int32_t>(first.appearanceSmall)
              && first.appearanceLarge != first.appearance
              && first.appearanceSmall != first.appearance, "appearance_class_columns");

    check(act("BTN_BACK") && state.step == kChargenHub && state.customStep == 0, "portrait_back_to_hub");
    check(act("BTN_BACK") && state.hubPanel == kHubChoice, "hub_back_to_choice");
    check(act("BTN_BACK") && state.step == 0 && gui.screenName() == "classsel", "back_to_class");
    check(click("BTN_SEL2") && act("CUST_CHAR_BTN") && act("BTN_STEPNAME1"), "reselect_scout");
    check(act("BTN_ACCEPT") && state.step == kChargenHub && state.customStep == 1, "portrait_accept");
    check(act("BTN_STEPNAME2"), "hub_attributes");
    check(state.step == 2 && gui.screenName() == "abchrgen", "abilities_screen");
    check(act("STR_PLUS_BTN") && act("STR_PLUS_BTN"), "strength_plus");
    check(state.chargen.attribute(0) == 10, "strength_value");
    // 006f70e0: the first row focused at open, its cost (1 below 14) and modifier ("-" for 0) shown, and the
    // label over the help keeps the file's "Description". Fails on the old screen, which hid both boxes.
    bindChargenScreen(gui, state, game);
    check(gui.state("COST_POINTS_LBL").visible && gui.state("COST_POINTS_LBL").text == "1"
              && gui.state("LBL_ABILITY_MOD").visible && gui.state("LBL_ABILITY_MOD").text == "-"
              && gui.state("SUB_TITLE_LBL").text == game.localization().tlk().text(209)
              && gui.state("DESC_LBL").text == game.localization().tlk().text(217)
              && !gui.state("LB_DESC").items.empty(),
          "abilities_cost_modifier_shown");
    check(act("BTN_ACCEPT") && state.customStep == 2 && state.statsShown, "abilities_accept");
    bindChargenScreen(gui, state, game);
    check(gui.state("STR_AB_LBL").text == "10", "hub_shows_scores");
    check(act("BTN_STEPNAME3"), "hub_skills");
    check(state.step == 3 && gui.screenName() == "skchrgen", "skills_screen");
    // 006f51d0 / 006f4bf0: the first skill focused at open with its class flag, cost and description.
    bindChargenScreen(gui, state, game);
    {
        const bool classSkill = game.rules().skillIsClassSkill(state.chargen.classId(), 0);
        check(gui.state("CLASSSKL_LBL").visible
                  && gui.state("CLASSSKL_LBL").text == game.localization().tlk().text(classSkill ? 38152 : 38153)
                  && gui.state("COST_POINTS_LBL").text == (classSkill ? "1" : "2")
                  && !gui.state("LB_DESC").items.empty(),
              "skills_first_row_described");
    }
    // Skill points: four times (base/2 + INT mod), at least 4 (INT 8 -> -1).
    const std::int32_t scoutPoints = state.chargen.skillPointsRemaining();
    check(scoutPoints == std::max(4, 4 * (game.rules().classSkillPointBase(1) / 2 - 1)),
          "scout_skill_points");
    for (int i = 0; i < 4; ++i) {
        check(act("AWA_PLUS_BTN"), "awareness_plus");
    }
    check(state.chargen.skillRank(3) == 4, "awareness_value");
    // Cross-class skills (Persuade for a Scout) cost 2 and stop at half the class cap.
    int persuade = -1;
    for (int i = 0; i < game.rules().skillCount(); ++i) {
        if (game.rules().skillName(i) == "Persuade") {
            persuade = i;
        }
    }
    check(persuade >= 0 && !game.rules().skillIsClassSkill(1, persuade), "persuade_cross_class");
    const std::int32_t beforeCross = state.chargen.skillPointsRemaining();
    check(act("PER_PLUS_BTN") && state.chargen.skillPointsRemaining() == beforeCross - 2,
          "cross_class_costs_two");
    check(act("PER_PLUS_BTN") && state.chargen.skillRank(persuade) == 2, "cross_class_rank_two");
    check(act("PER_PLUS_BTN") && state.chargen.skillRank(persuade) == 2, "cross_class_capped");
    check(act("BTN_ACCEPT") && state.customStep == 3, "skills_accept");
    check(act("BTN_STEPNAME4"), "hub_feats");
    // 006f3460: the feats screen opens under the box of the feats the class grants at this level (strref 0xa512).
    check(gui.screenName() == "skillinfo" && gui.state("LB_SKILLS").items.size() >= 3, "feats_granted_box");
    check(act("BTN_OK"), "feats_granted_box_ok");
    check(state.step == 4 && gui.screenName() == "ftchrgen", "feats_screen");
    act("BTN_SELECT");
    check(state.chargen.featPointsRemaining() == 0, "one_feat_spent");
    check(act("BTN_ACCEPT") && state.customStep == 4, "feats_accept");
    check(act("BTN_STEPNAME5"), "hub_name");
    check(state.step == 5 && gui.screenName() == "name", "name_screen");
    // Paired frame cg_name, retail 006f9bf0: a first and last name from the shipped tables, and the
    // button gives another. Fails on the old "Player" default and the old hidden button.
    {
        const std::string opened = state.nameBuffer;
        check(opened != "Player" && opened.find(' ') != std::string::npos && opened.size() <= 16, "name_generated_on_open");
        bindChargenScreen(gui, state, game);
        check(gui.state("BTN_RANDOM").visible && gui.state("BTN_RANDOM").text == game.localization().tlk().text(48579),
              "name_random_button_labelled");
        bool changed = false;
        for (int i = 0; i < 4 && !changed; ++i) {
            check(act("BTN_RANDOM"), "name_random_handled");
            changed = state.nameBuffer != opened && !state.nameBuffer.empty();
        }
        check(changed, "name_random_draws_another");
    }
    // The text box: Backspace clears the default, a blank name is refused with retail's text (42360),
    // typed characters fill it up to the limit and anything else is ignored.
    for (std::size_t i = 0; i < kChargenNameMax; ++i) {
        chargenNameBackspace(state);
    }
    check(state.nameBuffer.empty(), "name_cleared_by_backspace");
    check(act("END_BTN") && !state.finished && state.step == 5 && state.nameRefused, "blank_name_refused");
    bindChargenScreen(gui, state, game);
    check(gui.state("SUB_TITLE_LBL").text == game.localization().tlk().text(42360)
              && gui.state("SUB_TITLE_LBL").text == "You must specify a name.",
          "blank_name_message_shown");
    chargenNameInput(state, "Rev");
    chargenNameInput(state, "an 12!_/\
");
    // Retail 004184f0 refuses only control codes, '_', '/' and '\'.
    check(state.nameBuffer == "Revan 12!" && !state.nameRefused, "typed_name_filters_characters");
    chargenNameInput(state, "abcdefghijklmnopqrstuvwxyz");
    check(state.nameBuffer.size() == kChargenNameMax, "typed_name_is_capped");
    for (std::size_t i = 0; i < kChargenNameMax; ++i) {
        chargenNameBackspace(state);
    }
    chargenNameInput(state, "Revan  ");
    bindChargenScreen(gui, state, game);
    check(gui.state("NAME_BOX_EDIT").text.rfind("Revan", 0) == 0,
          "name_box_shows_the_typing");
    check(act("END_BTN") && state.step == kChargenHub && state.customStep == 5 && !state.finished, "name_end");
    bindChargenScreen(gui, state, game);
    check(gui.state("LBL_NAME").text == state.chargen.name() && !state.chargen.name().empty(), "hub_shows_name");
    // Cancel after the first step asks first (006ef6a0, strref 48541); No keeps everything.
    check(act("BTN_CANCEL") && gui.modalActive() && state.cancelConfirm, "hub_cancel_asks");
    check(act("BTN_CANCEL") && !gui.modalActive() && state.customStep == 5, "hub_cancel_no_keeps");
    check(act("BTN_STEPNAME6"), "hub_play");
    check(!state.active && state.finished, "flow_finished");
    check(game.character().name == "REVAN", "applied_name");
    check(game.character().classId == 1, "applied_class");
    check(game.character().str == 10, "applied_strength");
    check(game.playerEntity() != static_cast<std::size_t>(-1), "player_entity");
    check(game.character().gender == 0 && game.character().portraitRow == first.row,
          "applied_gender_portrait");
    check(game.playerAppearance() == first.appearance, "applied_appearance");
    const int maleVm = chargenVmGender(game);
    logInfo(concat("chargen_gui: male vm gender value=", maleVm));
    check(maleVm == 0, "male_get_gender_vm");

    // Recommended on the three steps, by pointer on the shipped screens (BTN_RECOMMENDED): the class's
    // own row of classes.2da (all 30 points), skills.2da <prefix>_reco and feat.2da <prefix>_recom.
    {
        ChargenGuiState recommended(game.rules());
        recommended.chargen.selectClass(2); // Scoundrel: a different spread than the Scout above
        recommended.step = 2;
        check(gui.open("abchrgen"), "recommended_abilities_screen");
        bindChargenScreen(gui, recommended, game);
        check(gui.state("BTN_RECOMMENDED").visible, "recommended_button_shown");
        const auto clickIn = [&](ChargenGuiState& target, const std::string& tag) {
            const kotor::GuiControl* hit = gui.control(tag);
            if (hit == nullptr) {
                return false;
            }
            const float x = static_cast<float>(hit->extent.left + hit->extent.width / 2);
            const float y = static_cast<float>(hit->extent.top + hit->extent.height / 2);
            gui.pointerMove(x, y);
            gui.pointerDown(x, y);
            const bool released = gui.pointerUp(x, y);
            const std::string action = gui.lastAction();
            gui.clearActions();
            return released && action == tag && handleChargenGuiAction(action, target, gui, game);
        };
        check(clickIn(recommended, "BTN_RECOMMENDED"), "recommended_abilities_click");
        const std::array<std::int32_t, 6> wanted = game.rules().recommendedAbilities(2);
        bool same = recommended.chargen.pointsRemaining() == 0;
        for (int i = 0; i < 6; ++i) {
            same = same && recommended.chargen.attribute(i) == wanted[static_cast<std::size_t>(i)];
        }
        check(same, "recommended_abilities_are_the_class_row");
        recommended.step = 3;
        check(gui.open("skchrgen"), "recommended_skills_screen");
        bindChargenScreen(gui, recommended, game);
        check(clickIn(recommended, "BTN_RECOMMENDED"), "recommended_skills_click");
        int ranks = 0;
        for (int i = 0; i < game.rules().skillCount(); ++i) {
            ranks += recommended.chargen.skillRank(i);
        }
        check(ranks > 0 && recommended.chargen.skillPointsRemaining() >= 0, "recommended_skills_spent");
        recommended.step = 4;
        check(gui.open("ftchrgen"), "recommended_feats_screen");
        bindChargenScreen(gui, recommended, game);
        check(clickIn(recommended, "BTN_RECOMMENDED"), "recommended_feats_click");
        check(recommended.chargen.featPointsRemaining() >= 0 && recommended.chargen.featPointsRemaining() < 2,
              "recommended_feats_taken");
    }

    // Derived statistics against classes.2da / the class tables, for every base class.
    {
        std::optional<kotor::TwoDa> classes;
        if (auto data = install.find("classes", 2017); data.has_value()) {
            classes = kotor::TwoDa::parse(std::move(*data), "classes");
        }
        check(classes.has_value(), "classes_table");
        for (int classId = 0; classId < 3 && classes.has_value(); ++classId) {
            Chargen chargen(game.rules());
            chargen.selectClass(classId);
            const CharacterSheet sheet = chargen.build();
            const std::string row = std::to_string(classId);
            const int hitDie = std::stoi(classes->cell(row, "hitdie"));
            const int conMod = (sheet.con - 10) / 2;
            check(sheet.maxHp == hitDie + conMod, "class_hp_" + row);
            check(sheet.maxFp == 0 && std::stoi(classes->cell(row, "forcedie")) == 0,
                  "class_fp_" + row);
            std::optional<kotor::TwoDa> saves;
            std::string tableName = classes->cell(row, "savingthrowtable");
            for (char& ch : tableName) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            if (auto data = install.find(tableName, 2017); data.has_value()) {
                saves = kotor::TwoDa::parse(std::move(*data), tableName);
            }
            check(saves.has_value()
                      && game.rules().savingThrow(sheet, 0) == std::stoi(saves->cell("0", "fortsave"))
                      && game.rules().savingThrow(sheet, 1) == std::stoi(saves->cell("0", "refsave"))
                      && game.rules().savingThrow(sheet, 2) == std::stoi(saves->cell("0", "willsave")),
                  "class_saves_" + row);
        }
        // Jedi: ForceDie per level plus the WIS and CHA modifiers; FeatGain columns by prefix.
        std::optional<kotor::TwoDa> featgain;
        if (auto data = install.find("featgain", 2017); data.has_value()) {
            featgain = kotor::TwoDa::parse(std::move(*data), "featgain");
        }
        check(featgain.has_value(), "featgain_table");
        const char* prefixes[6] = {"sol", "sct", "scd", "jgd", "jcn", "jsn"};
        for (int classId = 0; classId < 6 && classes.has_value() && featgain.has_value();
             ++classId) {
            const std::string row = std::to_string(classId);
            bool allLevels = true;
            for (int level = 1; level <= 20; ++level) {
                const std::string label = std::to_string(level - 1);
                const int expected =
                    std::stoi(featgain->cell(label, std::string(prefixes[classId]) + "_reg"))
                    + std::stoi(featgain->cell(label, std::string(prefixes[classId]) + "_bon"));
                allLevels = allLevels && game.rules().featsForLevel(classId, level) == expected;
            }
            check(allLevels, "feats_by_level_" + row);
            if (classId >= 3) {
                const CharacterSheet jedi = game.rules().createCharacter(classId, 1);
                const int die = std::stoi(classes->cell(row, "forcedie"));
                const int wisMod = (jedi.wis - 10) / 2;
                const int chaMod = (jedi.cha - 10) / 2;
                check(die > 0 && jedi.maxFp == std::max(1, die + wisMod + chaMod),
                      "jedi_fp_" + row);
                check(jedi.maxHp == std::stoi(classes->cell(row, "hitdie")) + (jedi.con - 10) / 2,
                      "jedi_hp_" + row);
            }
        }
        check(game.rules().classColumnPrefix(4) == "jcn" && game.rules().classColumnPrefix(5) == "jsn",
              "consular_sentinel_prefixes");
    }
    // Granted feats come from feat.2da (JEDI_DEFENSE etc. for a Guardian, none selectable).
    {
        const CharacterSheet guardian = game.rules().createCharacter(3, 1);
        check(guardian.feats.contains(43) && guardian.feats.contains(55), "guardian_granted_feats");
        const CharacterSheet soldier = game.rules().createCharacter(0, 1);
        check(soldier.feats.contains(4) && soldier.feats.contains(44)
                  && !soldier.feats.contains(55), "soldier_granted_feats");
    }

    // Engine save round trip keeps gender, portrait and body.
    {
        const std::filesystem::path temp =
            std::filesystem::temp_directory_path() / "kotor-chargen-gender";
        std::error_code ignored;
        std::filesystem::remove_all(temp, ignored);
        check(game.save(temp / "player.txt"), "gender_save");
        GameSession restored(game.installPath(), game.module());
        check(restored.load(temp / "player.txt"), "gender_load");
        check(restored.character().gender == 0 && restored.character().portraitRow == first.row
                  && restored.playerAppearance() == first.appearance,
              "gender_roundtrip_male");
    }

    // The female half: a fresh session, BTN_SEL5 (female Scout) through the same screens.
    GameSession womanGame(game.installPath(), game.module());
    {
        ChargenGuiState womanState(womanGame.rules());
        GuiSystem womanGui(install);
        womanGui.open("classsel");
        const auto womanAct = [&](const std::string& tag) {
            return handleChargenGuiAction(tag, womanState, womanGui, womanGame);
        };
        check(womanAct("BTN_SEL5"), "female_scout_select");
        check(womanState.chargen.classId() == 1 && womanState.chargen.gender() == 1,
              "female_scout_state");
        // Quick: Portrait, Name, Play (QUICKPNL).
        check(womanAct("QUICK_CHAR_BTN") && womanAct("BTN_STEPNAME1") && womanState.step == 1, "female_quick_portrait");
        const PortraitInfo woman = womanState.chargen.portrait();
        check(woman.row >= 0 && woman.sex == 1 && woman.row != first.row, "female_portrait_row");
        std::string womanResref;
        if (portraitTable.has_value() && woman.row >= 0) {
            womanResref = portraitTable->cell(portraitTable->rowLabel(
                                                  static_cast<std::size_t>(woman.row)),
                                              "baseresref");
        }
        check(!womanResref.empty() && woman.resref == womanResref, "female_portrait_matches_2da");
        check(portraits.digest(woman.resref) != 0
                  && portraits.digest(woman.resref) != firstDigest, "female_portrait_art");
        // Every female row stays female while cycling.
        bool allFemale = true;
        for (int i = 0; i < womanState.chargen.portraitListSize(); ++i) {
            allFemale = allFemale && womanState.chargen.portrait().sex == 1;
            womanAct("BTN_ARRR");
        }
        check(allFemale && womanState.chargen.portraitIndex() == 0, "female_cycle_stays_female");
        for (const char* step : {"BTN_ACCEPT", "BTN_STEPNAME2"}) {
            womanAct(step);
        }
        check(womanState.step == 5, "female_reaches_name");
        womanState.nameBuffer = "Bastila";
        check(womanAct("END_BTN") && womanState.quickStep == 2 && womanAct("BTN_STEPNAME3") && womanState.finished,
              "female_flow_finished");
        check(womanGame.character().gender == 1, "female_sheet_gender");
        check(chargenVmGender(womanGame) == 1, "female_get_gender_vm");
        const std::size_t avatar = womanGame.playerEntity();
        check(avatar != static_cast<std::size_t>(-1)
                  && womanGame.scene().entities[avatar].stats.gender == 1
                  && womanGame.scene().entities[avatar].appearance == woman.appearance
                  && womanGame.playerAppearance() == woman.appearance,
              "female_avatar_model");
        // Saves keep it: engine text save and the retail SAVEGAME layout.
        const std::filesystem::path temp =
            std::filesystem::temp_directory_path() / "kotor-chargen-gender-f";
        std::error_code ignored;
        std::filesystem::remove_all(temp, ignored);
        check(womanGame.save(temp / "player.txt"), "female_save");
        GameSession restored(game.installPath(), game.module());
        check(restored.load(temp / "player.txt") && restored.character().gender == 1
                  && restored.playerAppearance() == woman.appearance
                  && chargenVmGender(restored) == 1, "female_engine_roundtrip");
        const std::filesystem::path retail = temp / "slot";
        check(womanGame.saveOriginalSave(retail, "Chargen gender"), "female_original_save");
        kotor::OriginalSave readBack;
        try {
            readBack = kotor::readOriginalSave(retail, {});
        } catch (const std::exception& exception) {
            logWarn(concat("original save read failed: ", exception.what()));
        }
        check(readBack.gender == 1 && readBack.portraitId == woman.row
                  && readBack.appearance == static_cast<std::int32_t>(woman.appearance),
              "female_original_fields");
        GameSession retailLoaded(game.installPath(), game.module());
        check(retailLoaded.loadOriginalSave(retail) && retailLoaded.character().gender == 1
                  && chargenVmGender(retailLoaded) == 1, "female_original_roundtrip");

        // A shipped gender-conditional dialogue: the opening conversation takes the
        // male or the female branch (k_pend_male / k_pend_female).
        const std::vector<std::uint32_t> womanEntries = chargenOpeningEntries(womanGame);
        const std::vector<std::uint32_t> manEntries = chargenOpeningEntries(game);
        const auto has = [](const std::vector<std::uint32_t>& list, std::uint32_t entry) {
            return std::find(list.begin(), list.end(), entry) != list.end();
        };
        std::string visited;
        for (const std::uint32_t entry : manEntries) {
            visited += std::to_string(entry) + " ";
        }
        visited += "| ";
        for (const std::uint32_t entry : womanEntries) {
            visited += std::to_string(entry) + " ";
        }
        logInfo(concat("chargen_gui: opening conversation entries (male | female) ", visited));
        const bool manBranch = has(manEntries, 6) || has(manEntries, 11);
        const bool womanBranch = has(womanEntries, 5) || has(womanEntries, 10);
        check(manBranch && !has(manEntries, 5) && !has(manEntries, 10), "dialogue_male_branch");
        check(womanBranch && !has(womanEntries, 6) && !has(womanEntries, 11),
              "dialogue_female_branch");
    }

    logInfo(ok ? "chargen_gui: PASS" : "chargen_gui: FAIL");
    return ok ? 0 : 76;
}

int runAvatarTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const std::string& label) {
        logInfo(concat("avatar: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    game.setAnimationEnabled(true);
    game.setPlayerAppearance(92, "Revan");
    game.setThirdPerson(true);
    check(game.playerEntity() != static_cast<std::size_t>(-1), "player_entity");
    check(!game.scene().entityMeshes.empty(), "player_mesh");
    check(game.findEntity("PC") == game.playerEntity(), "player_tag");

    bool playerFinite = true;
    for (int frame = 0; frame < 60; ++frame) {
        InputState input;
        input.forward = true;
        input.run = true;
        dismissDialogue(game);
        game.update(1.0f / 60.0f, input);
    }
    dismissDialogue(game);
    game.update(1.0f / 60.0f, [] {
        InputState input;
        input.forward = true;
        input.run = true;
        return input;
    }());
    const std::string clip = game.animationName(game.playerEntity());
    check(clip == "run" || clip == "walk", "player_locomotion");
    check(game.animationTime(game.playerEntity()) > 0.0f, "player_clock");
    for (const kotor::EntityMesh& mesh : game.scene().entityMeshes) {
        if (mesh.entity != game.playerEntity()) {
            continue;
        }
        for (const kotor::SceneVertex& vertex : mesh.vertices) {
            playerFinite = playerFinite && std::isfinite(vertex.position.x)
                && std::isfinite(vertex.position.y) && std::isfinite(vertex.position.z);
        }
    }
    check(playerFinite, "player_finite");

    GameSession companionGame(game.installPath(), "tar_m02af");
    check(companionGame.addPartyMember("Mission"), "companion_join");
    const std::size_t mission = companionGame.findEntity("Mission");
    check(mission != static_cast<std::size_t>(-1), "companion_entity");
    if (mission != static_cast<std::size_t>(-1)) {
        check(companionGame.scene().entities[mission].partyAvatar, "companion_avatar");
        bool hasMesh = false;
        for (const kotor::EntityMesh& mesh : companionGame.scene().entityMeshes) {
            if (mesh.entity == mission) {
                hasMesh = true;
                break;
            }
        }
        check(hasMesh, "companion_mesh");
        const kotor::Vec3 start = companionGame.scene().entities[mission].position;
        for (int frame = 0; frame < 120; ++frame) {
            InputState input;
            input.forward = true;
            companionGame.update(1.0f / 60.0f, input);
        }
        const kotor::Vec3 end = companionGame.scene().entities[mission].position;
        const float moved = std::sqrt((end.x - start.x) * (end.x - start.x)
                                      + (end.y - start.y) * (end.y - start.y));
        logInfo(concat("avatar: companion moved ", moved));
        check(moved > 0.01f, "companion_follow");
    }

    const std::filesystem::path savePath =
        std::filesystem::temp_directory_path() / "kotor-s14-avatar.save";
    check(companionGame.save(savePath), "companion_save");
    GameSession restored(companionGame.installPath(), companionGame.module());
    check(restored.load(savePath), "companion_load");
    check(restored.findEntity("Mission") != static_cast<std::size_t>(-1),
          "companion_restored");

    logInfo(ok ? "avatar: PASS" : "avatar: FAIL");
    return ok ? 0 : 75;
}

int runBugParityTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("bug parity: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    const std::size_t trask = game.findEntity("end_trask");
    const std::string startModule = game.module();
    check(trask != static_cast<std::size_t>(-1), "trask");
    GameSession fixed(game.installPath(), startModule);
    fixed.setBugParityMode(false);
    if (trask != static_cast<std::size_t>(-1)) {
        check(game.startDialogue(trask), "classic_dialogue");
        game.toggleUiPanel(UiPanel::Character);
        check(game.activePanel() == UiPanel::Character, "classic_anywhere_menu");
        game.endDialogue();
        game.setUiPanel(UiPanel::None);

        check(fixed.startDialogue(trask), "fixed_dialogue");
        fixed.toggleUiPanel(UiPanel::Character);
        check(fixed.activePanel() == UiPanel::None, "fixed_anywhere_menu");
        fixed.endDialogue();
    }

    game.setPositionForTest({-999.0f, -999.0f, 0.0f});
    std::string from;
    std::string to;
    check(game.testTransition(from, to), "classic_displaced_load");
    fixed.setPositionForTest({-999.0f, -999.0f, 0.0f});
    std::string fixedFrom;
    std::string fixedTo;
    check(!fixed.testTransition(fixedFrom, fixedTo), "fixed_displaced_load");

    const kotor::Vec3 partyPosition{10.0f, 20.0f, 0.0f};
    const kotor::Vec3 destinationEntry{0.0f, 0.0f, 0.0f};
    check(BugParity::spawnWarp(true, partyPosition, destinationEntry).x == 10.0f,
          "classic_spawn_warp");
    check(BugParity::spawnWarp(false, partyPosition, destinationEntry).x == 0.0f,
          "fixed_spawn_warp");

    BugParityHotshotInput hotshotInput;
    hotshotInput.destinationSnapshot = true;
    hotshotInput.destinationLevel = 5;
    hotshotInput.destinationXp = 5000;
    BugParityHotshotState classicHotshot;
    classicHotshot.level = 10;
    classicHotshot.xp = 20000;
    classicHotshot.inventory = {"sword"};
    classicHotshot.quests = {{"q1", 1}};
    check(BugParity::hotshot(true, hotshotInput, classicHotshot), "classic_hotshot");
    check(classicHotshot.level == 5 && classicHotshot.xp == 5000,
          "classic_hotshot_revert");
    check(classicHotshot.inventory.size() == 1
              && classicHotshot.quests.at("q1") == 1,
          "classic_hotshot_persist");
    BugParityHotshotState fixedHotshot;
    fixedHotshot.level = 10;
    fixedHotshot.xp = 20000;
    check(!BugParity::hotshot(false, hotshotInput, fixedHotshot),
          "fixed_hotshot");
    check(fixedHotshot.level == 10 && fixedHotshot.xp == 20000,
          "fixed_hotshot_unchanged");

    BugParityLevel classicLevel{3, 6000};
    check(BugParity::fakeLevelUp(true, classicLevel), "classic_fake_level");
    check(classicLevel.level == 4 && classicLevel.xp == 6000,
          "classic_fake_level_state");
    BugParityLevel fixedLevel{3, 6000};
    check(!BugParity::fakeLevelUp(false, fixedLevel), "fixed_fake_level");
    check(fixedLevel.level == 3 && fixedLevel.xp == 6000,
          "fixed_fake_level_unchanged");

    BugParityItems classicItems;
    classicItems.equipped = {"blaster"};
    check(BugParity::airlockDupe(true, classicItems), "classic_dupe");
    check(classicItems.inventory.size() == 1
              && classicItems.inventory.front() == "blaster",
          "classic_dupe_state");
    BugParityItems fixedItems;
    fixedItems.equipped = {"blaster"};
    check(!BugParity::airlockDupe(false, fixedItems), "fixed_dupe");
    check(fixedItems.inventory.empty(), "fixed_dupe_unchanged");

    BugParityHotshotState routineHotshot;
    routineHotshot.level = 10;
    routineHotshot.xp = 20000;
    BugParityLevel routineLevel{3, 6000};
    BugParityItems routineItems;
    routineItems.equipped = {"blaster"};
    const BugParityRoutineResult classicRoutine = BugParity::routine(
        true, hotshotInput, routineHotshot, routineLevel, routineItems,
        partyPosition, destinationEntry);
    check(classicRoutine.completed, "classic_routine");
    BugParityHotshotState fixedRoutineHotshot;
    fixedRoutineHotshot.level = 10;
    fixedRoutineHotshot.xp = 20000;
    BugParityLevel fixedRoutineLevel{3, 6000};
    BugParityItems fixedRoutineItems;
    fixedRoutineItems.equipped = {"blaster"};
    const BugParityRoutineResult fixedRoutine = BugParity::routine(
        false, hotshotInput, fixedRoutineHotshot, fixedRoutineLevel,
        fixedRoutineItems, partyPosition, destinationEntry);
    check(!fixedRoutine.completed, "fixed_routine");
    check(fixedRoutineHotshot.level == 10 && fixedRoutineLevel.level == 3
              && fixedRoutineItems.inventory.empty(),
          "fixed_routine_unchanged");

    logInfo(ok ? "bug parity: PASS" : "bug parity: FAIL");
    return ok ? 0 : 72;
}

int runRetailDifferentialTest(GameSession& game, const std::string& tracePath,
                              const std::string& saveFixture, bool moduleTrace)
{
    int playCode = 0;
    std::string traceModule = game.module();
    if (moduleTrace) {
        traceModule = game.module();
        for (int frame = 0; frame < 3600; ++frame) {
            game.update(1.0f / 60.0f, InputState{});
        }
    } else if (saveFixture.empty()) {
        playCode = runPlaythrough(game);
    } else if (!game.loadOriginalSave(saveFixture)) {
        logError(concat("retail differential: cannot load save ", saveFixture));
        return 75;
    } else {
        traceModule = game.module();
        const std::string loadedModule = game.module();
        for (int frame = 0; frame < 3600; ++frame) {
            game.update(1.0f / 60.0f, InputState{});
        }
        logInfo(concat("retail differential: save=", saveFixture,
                       " module=", loadedModule, " simulated=60s"));
    }
    if (playCode != 0) {
        logError("retail differential: deterministic playthrough failed");
        return playCode;
    }
    // These AI actions sit behind branch-gated combat state that the
    // deterministic path does not reach. Probe the runtime directly so the
    // trace still records their execution (phase "probe").
    const std::array<std::uint16_t, 19> probeActions = {
        63u, 96u, 236u, 256u, 259u, 261u, 263u, 286u, 307u, 489u,
        490u, 495u, 522u, 723u, 724u, 725u, 726u, 734u, 735u};
    for (const std::uint16_t actionId : probeActions) {
        if (game.probeAction(actionId)) {
            logInfo(concat("retail differential: probe action=", actionId));
        }
    }
    if (tracePath.empty()) {
        logError("retail differential: --engine-trace path is required");
        return 73;
    }
    std::error_code error;
    const std::filesystem::path path(tracePath);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        logError(concat("retail differential: cannot write ", path.string()));
        return 74;
    }
    int seq = 0;
    const auto emit = [&](const char* kind, const std::string& payload) {
        out << "{\"kind\":\"" << kind << "\",\"seq\":" << seq++
            << ",\"payload\":" << payload << "}\n";
    };
    emit("header", concat("{\"schema\":\"kotor.engine-semantic/1\",\"module\":\"",
                          jsonEscape(traceModule), "\",\"final_module\":\"",
                          jsonEscape(game.module()), "\",\"build\":\"",
                          jsonEscape(game.buildSha256()), "\"}"));
    for (const script::WorldEvent& event : game.actionTrace()) {
        emit("kotor.action", concat("{\"action\":", event.id, ",\"name\":\"",
                                    jsonEscape(event.name), "\",\"phase\":\"",
                                    jsonEscape(event.kind), "\",\"time\":",
                                    event.time, "}"));
    }
    for (const DialogueChoiceEvent& choice : game.dialogueChoiceTrace()) {
        emit("kotor.dialogue", concat("{\"module\":\"", jsonEscape(choice.module),
                                      "\",\"dialogue\":\"", jsonEscape(choice.resref),
                                      "\",\"entry\":", choice.entry,
                                      ",\"option\":", choice.option,
                                      ",\"text\":\"", jsonEscape(choice.text), "\"}"));
    }
    for (const ModuleTransitionEvent& transition : game.transitionTrace()) {
        emit("kotor.transition", concat("{\"from\":\"", jsonEscape(transition.from),
                                        "\",\"to\":\"", jsonEscape(transition.to),
                                        "\"}"));
    }
    out.close();
    if (!out) {
        logError(concat("retail differential: write failed for ", path.string()));
        return 74;
    }
    logInfo(concat("retail differential: trace=", path.string(),
                   " actions=", game.actionTrace().size(),
                   " dialogueChoices=", game.dialogueChoiceTrace().size(),
                   " transitions=", game.transitionTrace().size()));
    logInfo("retail differential: TRACE PASS");
    return 0;
}

int runDifferentialTest(GameSession& game, const std::string& saveFixtures)
{
    const int saveCode = runSaveMatrixTest(game, saveFixtures);
    const int dialogueCode = runDialogueCorpusTest(game);
    const int playthroughCode = runPlaythrough(game);
    const int moduleCode = runModulePlaythroughMatrix(game.installPath());
    const bool ok = saveCode == 0 && dialogueCode == 0 && playthroughCode == 0
        && moduleCode == 0;
    logInfo(concat("differential test: saveMatrix=", saveCode,
                   " dialogueCorpus=", dialogueCode,
                   " playthrough=", playthroughCode,
                   " moduleMatrix=", moduleCode));
    logInfo(ok ? "differential test: PASS" : "differential test: FAIL");
    return ok ? 0 : 70;
}

int runK2Test(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("k2 test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };

    constexpr const char* kKotor2Sha =
        "6a522e71631dcee93467bd2010f3b23d9145326e1e2e89305f13ab104dbbffef";
    const kotor::GameProfile profile = kotor::GameProfile::detect(game.installPath());
    check(profile.kind == kotor::GameKind::KotOR2, "profile_kind");
    check(profile.recognized, "profile_recognized");
    check(profile.buildSha256 == kKotor2Sha, "profile_sha256");
    check(profile.supportsWorldLoad(), "profile_world");
    check(!profile.supportsOriginalSaves(), "profile_saves_k1_only");

    kotor::Install install(game.installPath());
    install.bindModule("001EBO");
    install.addPacks();
    install.addLips();

    // More shipped K2 areas load through the same profile-gated path.
    struct K2Area {
        const char* module;
        const char* area;
        std::size_t rooms;
    };
    const K2Area areas[] = {
        {"007EBO", "003ebo", 16},
        {"002EBO", "002ebo", 2},
    };
    for (const K2Area& expected : areas) {
        try {
            const kotor::Scene area = kotor::loadModule(install, expected.module);
            check(area.area == expected.area, "extra_area");
            check(area.rooms.size() == expected.rooms, "extra_rooms");
            check(!area.vertices.empty() && !area.batches.empty(), "extra_geometry");
            logInfo(concat("k2 test: module=", expected.module, " area=", area.area,
                           " rooms=", area.rooms.size(),
                           " vertices=", area.vertices.size()));
        } catch (const kotor::FormatError& exception) {
            logWarn(concat("k2 test: module ", expected.module, " failed: ",
                           exception.what()));
            check(false, "extra_area");
        }
    }
    install.bindModule("001EBO");
    install.addPacks();
    install.addLips();

    // K2 room geometry: 340-byte trimesh header, MDX offset at +332.
    const auto roomMdl = install.find("001ebo2", kotor::resType(kotor::ResType::Mdl));
    const auto roomMdx = install.find("001ebo2", kotor::resType(kotor::ResType::Mdx));
    check(roomMdl.has_value() && roomMdx.has_value(), "room_model");
    if (roomMdl.has_value() && roomMdx.has_value()) {
        try {
            const kotor::Mdl room = kotor::Mdl::parse(*roomMdl, *roomMdx, "001ebo2.mdl");
            check(room.isKotOR2(), "mdl_k2_layout");
            const std::vector<kotor::MdlMeshTriangle> triangles = room.meshTriangles();
            check(!triangles.empty(), "room_triangles");
            bool finite = true;
            double maxAbs = 0.0;
            for (const kotor::MdlMeshTriangle& triangle : triangles) {
                for (const kotor::MdlMeshVertex& vertex : triangle.v) {
                    const double x = vertex.position.x;
                    const double y = vertex.position.y;
                    const double z = vertex.position.z;
                    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
                        finite = false;
                    }
                    maxAbs = std::max({maxAbs, std::fabs(x), std::fabs(y), std::fabs(z)});
                }
            }
            check(finite, "room_finite");
            check(maxAbs > 1.0 && maxAbs < 10'000.0, "room_bounds");
            logInfo(concat("k2 test: room triangles=", triangles.size(),
                           " maxAbs=", maxAbs));
        } catch (const kotor::FormatError& exception) {
            logWarn(concat("k2 test: room parse failed: ", exception.what()));
            check(false, "mdl_k2_layout");
        }
    }

    // K2 ships dialogs in <stem>_dlg.erf, separate from the area RIM.
    std::string source;
    const auto dialog = install.find("3cfd", 2029, &source);
    check(dialog.has_value(), "dlg_resource");
    check(source.find("001EBO_dlg.erf") != std::string::npos, "dlg_erf_layer");

    // Scene built through GameSession.
    const kotor::Scene& scene = game.scene();
    check(game.module() == "001EBO", "module");
    check(scene.area == "001ebo", "area");
    check(scene.rooms.size() == 18, "rooms");
    // One more than before this wave: the shipped OnEnter script a_bet3m4 calls
    // CreateObject(creature, "c_medbaypc"), which never reached the scene.
    check(scene.entities.size() == 176, "entities");
    // Room loop meshes whose animloop only tints them (controllers 100 selfillumcolor / 132 alpha) are
    // dynamic too: a controller's type is the byte offset it writes in the part each frame (00483330 writes
    // *ctrl + part), PartTriMesh starts +0x64 at 0 and +0x84 at 1.0 (ctor 00445840), and the mesh draw reads
    // +0x64 as its self illumination (00473ba2) and +0x84 as its alpha (0046f460). Seven 001EBO rooms tint:
    // 33 meshes without that case, 40 with it; batches 246 -> 242 as those meshes leave the static rooms.
    check(scene.entityMeshes.size() == 40, "entity_meshes");
    check(scene.entities.back().tag == "MEDBAY_PC"
              && scene.entities.back().kind == kotor::EntityKind::Creature,
          "script_created_creature");
    check(scene.triggers.size() == 17, "triggers");
    check(scene.warnings.empty(), "warnings");
    check(scene.vertices.size() > 200'000, "vertices");
    check(scene.batches.size() == 242, "batches");
    check(!scene.scripts.empty(), "scripts");
    check(scene.area == "001ebo" && game.languageId() == 0, "language");

    logInfo(concat("k2 test: area=", scene.area, " rooms=", scene.rooms.size(),
                   " vertices=", scene.vertices.size(),
                   " entities=", scene.entities.size(),
                   " meshes=", scene.entityMeshes.size(),
                   " triggers=", scene.triggers.size(),
                   " warnings=", scene.warnings.size()));
    logInfo(ok ? "k2 test: PASS" : "k2 test: FAIL");
    return ok ? 0 : 66;
}

int runVfsTest(GameSession& game, const std::string& fixtureDirectory)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("vfs test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    const auto readBytes = [](const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        std::vector<std::uint8_t> bytes;
        if (!file) {
            return bytes;
        }
        const std::streamoff size = file.tellg();
        if (size <= 0) {
            return bytes;
        }
        file.seekg(0, std::ios::beg);
        bytes.resize(static_cast<std::size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
            bytes.clear();
        }
        return bytes;
    };
    const auto writeBytes = [](const std::filesystem::path& path,
                               const std::vector<std::uint8_t>& bytes) {
        std::error_code error;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), error);
        }
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        if (!bytes.empty()) {
            file.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
        }
        return static_cast<bool>(file);
    };

    const std::filesystem::path temporary =
        std::filesystem::temp_directory_path() / "kotor-remaster-vfs-test";
    std::error_code error;
    std::filesystem::remove_all(temporary, error);
    std::filesystem::create_directories(temporary / "Override", error);

    kotor::Install install(game.installPath());
    install.bindModule("end_m01aa");
    install.addPacks();
    install.addLips();
    std::string source;
    const auto moduleUtd = install.find("end_door05", 2042, &source);
    check(moduleUtd.has_value() && source == "modules/end_m01aa_s.rim",
          "module_rim_layer");
    const auto appearance = install.find("appearance",
                                         kotor::resType(kotor::ResType::TwoDa), &source);
    check(appearance.has_value() && source.find("(KEY)") != std::string::npos,
          "bif_layer");
    check(install.find("abilities", 2047, &source).has_value()
              && source == "patch.erf",
          "patch_layer");
    kotor::Install rawInstall(game.installPath());
    check(rawInstall.find("gui_click", 4, &source).has_value()
              && source.find("(KEY)") != std::string::npos,
          "key_bif_layer");

    if (appearance.has_value()) {
        std::vector<std::uint8_t> overrideBytes = *appearance;
        overrideBytes.push_back(0u);
        check(writeBytes(temporary / "Override" / "appearance.2da", overrideBytes),
              "override_write");
        install.setOverlayRoot(temporary);
        const auto overridden = install.find(
            "appearance", kotor::resType(kotor::ResType::TwoDa), &source);
        check(overridden.has_value() && *overridden == overrideBytes
                  && source.find("(loose)") != std::string::npos,
              "loose_override");
        std::filesystem::remove(temporary / "Override" / "appearance.2da", error);
        install.setOverlayRoot(temporary);
        check(install.find("appearance", kotor::resType(kotor::ResType::TwoDa),
                           &source).has_value()
                  && source.find("(KEY)") != std::string::npos,
              "override_removed");
    }
    if (moduleUtd.has_value()) {
        std::vector<std::uint8_t> overrideBytes = *moduleUtd;
        overrideBytes.push_back(0u);
        check(writeBytes(temporary / "Override" / "end_door05.utd", overrideBytes),
              "module_override_write");
        install.setOverlayRoot(temporary);
        const auto overridden = install.find("end_door05", 2042, &source);
        check(overridden.has_value() && *overridden == overrideBytes
                  && source.find("(loose)") != std::string::npos,
              "override_beats_rim");
        std::filesystem::remove(temporary / "Override" / "end_door05.utd", error);
        install.setOverlayRoot(temporary);
    }

    const auto patchGui = install.find("abilities", 2047, &source);
    if (patchGui.has_value()) {
        std::vector<std::uint8_t> overrideBytes = *patchGui;
        overrideBytes.push_back(0u);
        check(writeBytes(temporary / "Override" / "abilities.gui", overrideBytes),
              "patch_override_write");
        install.setOverlayRoot(temporary);
        const auto overridden = install.find("abilities", 2047, &source);
        check(overridden.has_value() && *overridden == overrideBytes
                  && source.find("(loose)") != std::string::npos,
              "override_beats_patch");
        std::filesystem::remove(temporary / "Override" / "abilities.gui", error);
    }

    const std::filesystem::path sourceLip =
        std::filesystem::path(game.installPath()) / "lips" / "localization.mod";
    std::error_code copyError;
    std::filesystem::create_directories(temporary / "modules", copyError);
    std::filesystem::copy_file(sourceLip, temporary / "modules" / "end_m01aa.mod",
                               std::filesystem::copy_options::overwrite_existing,
                               copyError);
    check(!copyError, "mod_fixture");
    install.setOverlayRoot(temporary);
    install.bindModule("end_m01aa");
    install.addPacks();
    install.addLips();
    const auto modLip = install.find("nglobebant00102_",
                                     kotor::resType(kotor::ResType::Lip), &source);
    check(modLip.has_value() && source == "modules/end_m01aa.mod", "mod_layer");
    std::filesystem::remove(temporary / "modules" / "end_m01aa.mod", error);
    install.setOverlayRoot(temporary);
    install.bindModule("end_m01aa");
    install.addPacks();
    install.addLips();
    const auto lipsLip = install.find("nglobebant00102_",
                                      kotor::resType(kotor::ResType::Lip), &source);
    check(lipsLip.has_value() && source.find("lips/") != std::string::npos,
          "lips_layer");

    if (!fixtureDirectory.empty()) {
        const auto baseBytes = readBytes(
            std::filesystem::path(fixtureDirectory) / "merge_base.2da");
        const auto patchBytes = readBytes(
            std::filesystem::path(fixtureDirectory) / "merge_patch.2da");
        check(!baseBytes.empty() && !patchBytes.empty(), "merge_fixtures");
        if (!baseBytes.empty() && !patchBytes.empty()) {
            try {
                const kotor::TwoDa base = kotor::TwoDa::parse(baseBytes, "merge_base.2da");
                const kotor::TwoDa patch = kotor::TwoDa::parse(patchBytes, "merge_patch.2da");
                const kotor::TwoDa merged = kotor::TwoDa::merge(base, patch, "merged.2da");
                check(merged.columnIndex("added") >= 0, "merge_column");
                check(merged.cell("0", "value") == "99"
                          && merged.cell("0", "added") == "P"
                          && merged.cell("0", "name") == "alpha",
                      "merge_overwrite");
                check(merged.cell("1", "value") == "20"
                          && merged.cell("1", "added").empty(),
                      "merge_untouched");
                check(merged.rowIndex("2") >= 0
                          && merged.cell("2", "value") == "30"
                          && merged.cell("2", "added") == "Q",
                      "merge_append");
            } catch (const kotor::FormatError& exception) {
                logWarn(concat("vfs test: merge parse failed: ", exception.what()));
                check(false, "merge_parse");
            }
        }
    } else {
        check(false, "merge_fixtures");
    }

    install.setOverlayRoot(game.installPath());
    std::filesystem::remove_all(temporary, error);
    logInfo("vfs test: PASS");
    return ok ? 0 : 65;
}

int runLocalizationTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("localization test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    const auto lower = [](std::string text) {
        for (char& ch : text) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return text;
    };

    kotor::Localization localization(game.installPath());
    std::string error;
    check(localization.load(-1, error), "tlk_load");
    check(localization.languageId() == 0 && localization.languageName() == "English",
          "tlk_language");
    check(localization.entryCount() == 49265, "tlk_entries");
    check(localization.tlk().sound(38167) == "c_drdastro_bat1", "tlk_sound");

    // TSLPatcher-style TLK merge: overwrite, sound-only patch, append.
    const std::string base0 = localization.tlk().text(0);
    const kotor::Tlk patch = kotor::Tlk::fromEntries(
        0, {{"PATCHED", ""}, {"", "patch_snd"}, {"APPENDED", "append_snd"}},
        "patch.tlk");
    const kotor::Tlk merged = kotor::Tlk::merge(localization.tlk(), patch, "merged.tlk", 2);
    check(merged.size() == localization.entryCount() + 1, "tlk_merge_size");
    check(merged.text(0) == "PATCHED", "tlk_merge_overwrite");
    check(merged.text(1) == localization.tlk().text(1)
              && merged.sound(1) == "patch_snd",
          "tlk_merge_sound_only");
    check(merged.text(static_cast<std::uint32_t>(localization.entryCount())) == "APPENDED"
              && merged.sound(static_cast<std::uint32_t>(localization.entryCount()))
                  == "append_snd",
          "tlk_merge_append");
    check(localization.tlk().text(0) == base0, "tlk_merge_base_unchanged");

    // VFS integration: Override/replace.tlk and Override/append.tlk merge into
    // the base table, and a DLG StrRef resolves through the merged table.
    const std::filesystem::path tlkOverlay =
        std::filesystem::temp_directory_path() / "kotor-tlk-overlay";
    std::error_code tlkError;
    std::filesystem::remove_all(tlkOverlay, tlkError);
    std::filesystem::create_directories(tlkOverlay / "Override", tlkError);
    const auto writeBytes = [](const std::filesystem::path& path,
                               const std::vector<std::uint8_t>& data) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
        return static_cast<bool>(file);
    };
    const kotor::Tlk replacePatch = kotor::Tlk::fromEntries(
        0, {{"REPLACED", ""}}, "replace.tlk");
    const kotor::Tlk appendPatch = kotor::Tlk::fromEntries(
        0, {{"APPENDED", "append_snd"}}, "append.tlk");
    check(writeBytes(tlkOverlay / "Override" / "replace.tlk",
                     replacePatch.toBytes()),
          "tlk_vfs_replace_write");
    check(writeBytes(tlkOverlay / "Override" / "append.tlk",
                     appendPatch.toBytes()),
          "tlk_vfs_append_write");
    kotor::Install tlkInstall(game.installPath());
    tlkInstall.setOverlayRoot(tlkOverlay);
    kotor::Localization tlkLocalization(game.installPath());
    check(tlkLocalization.load(tlkInstall, 0, error), "tlk_vfs_load");
    check(tlkLocalization.tlk().text(0) == "REPLACED", "tlk_vfs_replace");
    check(tlkLocalization.tlk().text(
              static_cast<std::uint32_t>(localization.entryCount())) == "APPENDED",
          "tlk_vfs_append");
    auto dlgEntry = std::make_shared<kotor::GffNode>(0);
    dlgEntry->addLocString("Text", static_cast<std::uint32_t>(localization.entryCount()), {});
    auto dlgRoot = std::make_shared<kotor::GffNode>(0);
    dlgRoot->addList("EntryList", {dlgEntry});
    const std::vector<std::uint8_t> dlgBytes = kotor::writeGff("DLG ", *dlgRoot);
    const kotor::Gff dlgGff = kotor::Gff::parse(dlgBytes, "overlay.dlg");
    const kotor::Dlg dlg = kotor::Dlg::parse(dlgGff, &tlkLocalization.tlk(),
                                             "overlay.dlg");
    check(dlg.entries.size() == 1 && dlg.entries.front().text == "APPENDED",
          "tlk_vfs_strref");
    std::filesystem::remove_all(tlkOverlay, tlkError);
    check(localization.load(0, error), "language_explicit");
    check(!localization.load(1, error), "language_missing");
    check(localization.languageId() == 0, "language_retained");
    check(kotor::languageId("English") == 0 && kotor::languageId("german") == 2,
          "language_names");
    check(kotor::languageId("99") == -1, "language_bad_id");

    const std::filesystem::path voice = localization.voiceFile("nm01aatras02000_");
    check(!voice.empty(), "localized_voice");
    check(lower(voice.filename().string()) == "nm01aatras02000_.wav",
          "voice_file");
    check(voice.parent_path().filename().string() == "tras02", "voice_speaker_path");
    check(voice.parent_path().parent_path().filename().string() == "m01aa",
          "voice_module_path");
    check(localization.voiceFile("does_not_exist").empty(), "voice_missing");
    check(localization.localizedLipArchives() == 118, "localized_lips");

    kotor::Install install(game.installPath());
    install.bindModule(game.module());
    install.addPacks();
    install.addLips();
    const auto lipData = install.find("nglobebant00102_",
                                      kotor::resType(kotor::ResType::Lip));
    check(lipData.has_value(), "localized_lip_resource");
    if (lipData.has_value()) {
        try {
            const kotor::LipSync lip = kotor::parseLip(*lipData, "nglobebant00102_.lip");
            check(lip.frames.size() > 0 && lip.duration > 0.0f, "localized_lip");
        } catch (const kotor::FormatError&) {
            check(false, "localized_lip");
        }
    }

    check(game.localization().loaded() && game.languageId() == 0,
          "session_language");
    audio::SoundData voiceData;
    std::string voiceError;
    check(game.audio().decodeProbe("nm01aatras02000_", voiceData, voiceError),
          "voice_decode");
    check(voiceData.sampleRate == 32000 && voiceData.channels == 1
              && voiceData.frameCount() > 0,
          "voice_pcm");
    logInfo(concat("localization test: voice rate=", voiceData.sampleRate,
                   " ch=", voiceData.channels,
                   " frames=", voiceData.frameCount(),
                   " error=", voiceError));
    logInfo(concat("localization test: language=", localization.languageName(),
                   " tlk=", localization.entryCount(),
                   " voice=", voice.filename().string(),
                   " localizedLips=", localization.localizedLipArchives()));
    logInfo(ok ? "localization test: PASS" : "localization test: FAIL");
    return ok ? 0 : 64;
}

int runGuiTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("gui test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    kotor::Install install(game.installPath());
    install.bindModule(game.module());
    install.addPacks();
    const std::vector<std::string> names = install.resourceNames(2047);
    std::size_t screens = 0;
    std::size_t controls = 0;
    std::size_t unknownTypes = 0;
    std::size_t deepTrees = 0;
    bool mainMenu = false;
    std::set<int> types;
    for (const std::string& name : names) {
        const auto data = install.find(name, 2047);
        if (!data.has_value()) {
            continue;
        }
        try {
            const kotor::GuiScreen screen = kotor::parseGui(*data, name + ".gui");
            ++screens;
            controls += screen.controlCount;
            deepTrees = std::max(deepTrees, static_cast<std::size_t>(screen.controlCount));
            std::function<void(const kotor::GuiControl&)> walk =
                [&](const kotor::GuiControl& control) {
                    types.insert(control.type);
                    if (control.type != 0 && control.type != 2 && control.type != 4
                        && control.type != 5 && control.type != 6 && control.type != 7
                        && control.type != 8 && control.type != 9 && control.type != 10
                        && control.type != 11) {
                        ++unknownTypes;
                    }
                    for (const kotor::GuiControl& child : control.children) {
                        walk(child);
                    }
                };
            for (const kotor::GuiControl& control : screen.controls) {
                walk(control);
            }
            if (name == "mainmenu") {
                mainMenu = screen.tag == "TGuiPanel" && screen.controlCount == 15;
            }
        } catch (const kotor::FormatError&) {
            ok = false;
        }
    }
    logInfo(concat("gui test: screens=", screens, " controls=", controls,
                   " types=", types.size(), " unknown=", unknownTypes,
                   " maxControls=", deepTrees));
    check(screens == 86, "screens");
    check(controls == 2383, "controls");
    check(types.size() == 8, "control_types");
    check(unknownTypes == 0, "unknown_control_types");
    check(mainMenu, "mainmenu");

    const auto findControl = [](const std::vector<kotor::GuiControl>& list,
                                const std::string& tag)
        -> const kotor::GuiControl* {
        std::function<const kotor::GuiControl*(const kotor::GuiControl&)> walk =
            [&](const kotor::GuiControl& control) -> const kotor::GuiControl* {
                if (control.tag == tag) {
                    return &control;
                }
                for (const kotor::GuiControl& child : control.children) {
                    if (const kotor::GuiControl* found = walk(child)) {
                        return found;
                    }
                }
                return nullptr;
            };
        for (const kotor::GuiControl& control : list) {
            if (const kotor::GuiControl* found = walk(control)) {
                return found;
            }
        }
        return nullptr;
    };
    const auto findType = [](const std::vector<kotor::GuiControl>& list, int type)
        -> const kotor::GuiControl* {
        std::function<const kotor::GuiControl*(const kotor::GuiControl&)> walk =
            [&](const kotor::GuiControl& control) -> const kotor::GuiControl* {
                if (control.type == type) {
                    return &control;
                }
                for (const kotor::GuiControl& child : control.children) {
                    if (const kotor::GuiControl* found = walk(child)) {
                        return found;
                    }
                }
                return nullptr;
            };
        for (const kotor::GuiControl& control : list) {
            if (const kotor::GuiControl* found = walk(control)) {
                return found;
            }
        }
        return nullptr;
    };

    GuiSystem gui(install);
    check(gui.open("mainmenu"), "gui_open");
    check(gui.screenName() == "mainmenu", "gui_screen");
    const kotor::GuiControl* newGame = findControl(gui.screen()->controls, "BTN_NEWGAME");
    check(newGame != nullptr, "button_found");
    if (newGame != nullptr) {
        const float cx = static_cast<float>(newGame->extent.left)
            + static_cast<float>(newGame->extent.width) * 0.5f;
        const float cy = static_cast<float>(newGame->extent.top)
            + static_cast<float>(newGame->extent.height) * 0.5f;
        check(gui.controlAt(cx, cy) == newGame->key, "hit_test");
        check(gui.pointerDown(cx, cy), "pointer_down");
        check(gui.state(newGame->key).pressed, "pointer_pressed");
        check(gui.pointerUp(cx, cy), "pointer_up");
        check(gui.lastAction() == "BTN_NEWGAME", "button_action");
        check(gui.actionLog().size() == 1, "action_log");
        check(!gui.state(newGame->key).pressed, "pointer_released");
        gui.clearActions();
        check(gui.setVisible(newGame->key, false), "control_hide");
        check(gui.controlAt(cx, cy) != newGame->key, "hidden_no_hit");
        check(gui.setVisible(newGame->key, true), "control_show");
    }
    check(gui.activate("BTN_EXIT"), "exit_activate");
    check(gui.quitRequested(), "quit_requested");
    gui.clearActions();
    check(gui.push("loadscreen"), "screen_push");
    check(gui.screenName() == "loadscreen" && gui.historyDepth() == 1,
          "screen_push_state");
    check(gui.pop() && gui.screenName() == "mainmenu" && gui.historyDepth() == 0,
          "screen_pop");

    check(gui.open("character"), "screen_slider");
    const kotor::GuiControl* slider = findControl(gui.screen()->controls, "SLD_ALIGN");
    check(slider != nullptr && slider->type == 8 && slider->maxValue == 100.0f,
          "slider_control");
    if (slider != nullptr) {
        check(gui.setValue(slider->key, 250.0f), "slider_set");
        check(gui.value(slider->key) == 100.0f, "slider_clamp_high");
        check(gui.setValue(slider->key, -5.0f), "slider_set_low");
        check(gui.value(slider->key) == 0.0f, "slider_clamp_low");
    }

    check(gui.open("loadscreen"), "screen_progress");
    const kotor::GuiControl* progress = findControl(gui.screen()->controls, "PB_PROGRESS");
    check(progress != nullptr && progress->type == 10 && progress->maxValue == 100.0f,
          "progress_control");
    if (progress != nullptr) {
        check(gui.setValue(progress->key, 42.0f) && gui.value(progress->key) == 42.0f,
              "progress_value");
    }

    check(gui.open("abchrgen"), "screen_listbox");
    const kotor::GuiControl* listBox = findControl(gui.screen()->controls, "LB_DESC");
    const kotor::GuiControl* scrollBar = findType(gui.screen()->controls, 9);
    check(listBox != nullptr && listBox->type == 11, "listbox_control");
    check(scrollBar != nullptr && scrollBar->maxValue > 0.0f, "scrollbar_control");
    if (listBox != nullptr) {
        check(gui.setListSelection(listBox->key, 3) && gui.listSelection(listBox->key) == 3,
              "list_selection");
    }
    if (scrollBar != nullptr) {
        check(gui.setValue(scrollBar->key, 33.0f)
                  && gui.value(scrollBar->key) == 33.0f,
              "scrollbar_value");
    }

    check(gui.open("mipc10x7"), "screen_checkbox");
    const kotor::GuiControl* checkbox = findControl(gui.screen()->controls, "TB_SOLO");
    check(checkbox != nullptr && checkbox->type == 7, "checkbox_control");
    if (checkbox != nullptr) {
        check(gui.activate(checkbox->key) && gui.state(checkbox->key).selected,
              "checkbox_on");
        check(gui.activate(checkbox->key) && !gui.state(checkbox->key).selected,
              "checkbox_off");
    }

    check(game.applyGuiAction("LBL_ICON1") && game.activePanel() == UiPanel::Character,
          "panel_character");
    check(game.applyGuiAction("LBL_ICON1") && game.activePanel() == UiPanel::None,
          "panel_toggle");
    check(game.applyGuiAction("LBL_ICON2")
              && game.activePanel() == UiPanel::Inventory,
          "panel_inventory");
    game.setUiPanel(UiPanel::None);
    return ok ? 0 : 62;
}

int runMinigameTest(GameSession& game, const std::string& scriptPath)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("minigame test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };

    // Pazaak (Minigames.h): the retail model. Each check names the routine it follows and fails on the
    // engine's earlier rules (a nine-card win, the opponent opening a set, its own AI, a wager from 1).
    const auto mainCard = [](int value) { return PazaakCard{17 + value, 0}; }; // kinds 18..27 are 1..10
    const auto autoplay = [](PazaakGame& match) {
        std::string transcript;
        for (int step = 0; step < 40000 && match.phase() != PazaakGame::Phase::MatchOver; ++step) {
            if (match.sequenceState() == 10) {
                match.acknowledgeSet();
            } else if (match.awaitingPlayer()) {
                if (match.playerTotal() < 17) {
                    match.playerEndTurn();
                } else {
                    match.playerStand();
                }
            }
            match.tick(0.1f);
            transcript += std::to_string(match.sequenceState()) + "|" + std::to_string(match.playerTotal()) + "|"
                + std::to_string(match.opponentTotal()) + "|" + std::to_string(match.playerSets()) + "|"
                + std::to_string(match.opponentSets()) + "|" + std::to_string(match.deckRemaining()) + ";";
        }
        return transcript;
    };
    const auto playPazaak = [&](std::uint32_t seed) {
        PazaakGame match;
        match.reset(seed);
        const std::string transcript = autoplay(match);
        return std::make_tuple(transcript, match.phase(), match.playerSets(), match.opponentSets());
    };
    const auto pazaakA = playPazaak(0x13572468u);
    const auto pazaakB = playPazaak(0x13572468u);
    const auto pazaakC = playPazaak(0x24681357u);
    check(std::get<0>(pazaakA) == std::get<0>(pazaakB), "pazaak_deterministic");
    check(std::get<0>(pazaakA) != std::get<0>(pazaakC), "pazaak_seed_varies");
    check(std::get<1>(pazaakA) == PazaakGame::Phase::MatchOver, "pazaak_match_over");
    // HandleEndDialog 0067e9a0: three sets end the match.
    check(std::get<2>(pazaakA) == 3 || std::get<3>(pazaakA) == 3, "pazaak_three_sets");

    // ChooseSidedeck 006e4580: four of the ten side cards each; the main deck is 40 (ShuffleDeck 006e4730).
    PazaakGame pazaak;
    const std::vector<PazaakCard> opponentTen{{0, 0}, {1, 0}, {2, 0}, {3, 0}, {6, 0},
                                              {7, 0}, {8, 0}, {12, 0}, {13, 0}, {14, 0}};
    pazaak.reset(7u, PazaakGame::defaultSide(), opponentTen);
    bool dealtFromTen = pazaak.opponent().handCount() == 4;
    for (const PazaakCard& card : pazaak.opponent().hand) {
        dealtFromTen = dealtFromTen
            && std::any_of(opponentTen.begin(), opponentTen.end(), [&](const PazaakCard& c) { return c.kind == card.kind; });
    }
    check(dealtFromTen && pazaak.player().handCount() == 4, "pazaak_four_of_ten_dealt");
    // DoGameSequence 00680030 states 0..2: the player draws first, after the 0.4 s start-turn wait.
    pazaak.tick(0.0f);
    check(pazaak.turnMark() == 1 && pazaak.player().tableCount() == 0 && pazaak.timer() > 0.39f,
          "pazaak_start_turn_waits");
    pazaak.tick(0.5f);
    pazaak.tick(0.0f);
    check(pazaak.player().tableCount() == 1 && pazaak.opponent().tableCount() == 0 && pazaak.deckRemaining() == 39
              && pazaak.awaitingPlayer(),
          "pazaak_player_draws_first");
    check(!pazaak.playerPlayHand(9), "pazaak_hand_bounds");
    const int beforePlay = pazaak.playerTotal();
    const int handValue = pazaak.player().hand[0].value();
    check(pazaak.playerPlayHand(0) && pazaak.playerTotal() == beforePlay + handValue
              && pazaak.player().handCount() == 3 && !pazaak.player().hand[0].present() && pazaak.handCardPlayed(),
          "pazaak_hand_card_plays");
    check(!pazaak.playerPlayHand(1), "pazaak_one_hand_card_per_turn"); // HandlePlayHandCard 0067ede0: state 3 only
    check(pazaak.playerEndTurn() && !pazaak.awaitingPlayer(), "pazaak_turn_order");
    check(!pazaak.playerEndTurn(), "pazaak_end_turn_wrong_phase");

    // SetCard 0067cd30 / GetTotal 006e4360: a +/- card flips to its minus side; a plain card does not.
    PazaakGame flipper;
    flipper.reset(11u, {{14, 0}, {14, 0}, {14, 0}, {14, 0}, {14, 0}, {14, 0}, {14, 0}, {14, 0}, {14, 0}, {14, 0}},
                  opponentTen);
    check(flipper.player().hand[0].flip() && flipper.player().hand[0].value() == 3 && flipper.playerFlipHand(0)
              && flipper.player().hand[0].value() == -3,
          "pazaak_flip_card_flips");
    PazaakGame plain;
    plain.reset(11u, {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}, opponentTen);
    check(!plain.playerFlipHand(0) && plain.player().hand[0].value() == 1, "pazaak_plain_card_does_not_flip");

    // AddGameCard 006e45f0: exactly 20 stands the side at once.
    {
        PazaakGame twenty;
        twenty.reset(3u);
        PazaakGame::Side me;
        me.table[0] = mainCard(10);
        me.table[1] = mainCard(5);
        me.hand[0] = {4, 0}; // +5
        PazaakGame::Side them;
        them.table[0] = mainCard(4);
        twenty.setForTest(me, them, 3);
        check(twenty.playerPlayHand(0) && twenty.playerTotal() == 20 && twenty.player().standing
                  && !twenty.awaitingPlayer(),
              "pazaak_twenty_stands");
    }
    // AddGameCard 006e45f0 + DetermineWinner 006e48a0: the ninth card stands the side; it wins nothing by
    // itself and the opponent still plays its turn.
    {
        PazaakGame nine;
        nine.reset(5u);
        PazaakGame::Side me;
        for (std::size_t i = 0; i < 8; ++i) {
            me.table[i] = mainCard(1);
        }
        me.hand[0] = {0, 0}; // +1
        PazaakGame::Side them;
        them.table[0] = mainCard(2);
        nine.setForTest(me, them, 3);
        check(nine.playerPlayHand(0), "pazaak_nine_played");
        nine.tick(0.5f);
        nine.tick(0.0f);
        check(nine.player().standing && nine.player().tableCount() == 9 && nine.playerSets() == 0
                  && nine.phase() == PazaakGame::Phase::OpponentTurn && nine.sequenceState() == 7,
              "pazaak_nine_cards_do_not_win");
    }
    // DoGameSequence state 3 / DetermineWinner: a draw over 20 is not a loss until the turn ends; a hand
    // card can bring it back.
    {
        PazaakGame bust;
        bust.reset(9u);
        PazaakGame::Side me;
        me.table[0] = mainCard(10);
        me.table[1] = mainCard(10);
        me.table[2] = mainCard(5);
        me.hand[0] = {11, 0}; // -6
        PazaakGame::Side them;
        them.table[0] = mainCard(3);
        bust.setForTest(me, them, 3);
        bust.tick(0.0f);
        check(bust.awaitingPlayer() && bust.opponentSets() == 0, "pazaak_bust_waits_for_the_player");
        check(bust.playerPlayHand(0) && bust.playerTotal() == 19 && bust.playerEndTurn(), "pazaak_bust_recovered");
        bust.tick(0.5f);
        bust.tick(0.0f);
        check(bust.opponentSets() == 0 && !bust.player().standing && bust.phase() == PazaakGame::Phase::OpponentTurn,
              "pazaak_bust_recovered_plays_on");
    }
    // PlayAITurn 006e4c60 / FindPotentialSidecardTotal 006e4910: at 16 with +2 and -5 the opponent keeps
    // the +2 (18 needs the lowest reach over 13) and ends its turn; at 16 with only +1 it stands.
    {
        PazaakGame ai;
        ai.reset(13u);
        PazaakGame::Side me;
        me.table[0] = mainCard(10);
        PazaakGame::Side them;
        them.table[0] = mainCard(10);
        them.table[1] = mainCard(6);
        them.hand[0] = {1, 0};  // +2
        them.hand[1] = {10, 0}; // -5
        ai.setForTest(me, them, 7);
        check(ai.playAITurn(1) == 4 && ai.opponent().handCount() == 2 && ai.opponentTotal() == 16
                  && !ai.opponent().standing,
              "pazaak_ai_keeps_its_card");
        them.hand = {};
        them.hand[0] = {0, 0}; // +1
        ai.setForTest(me, them, 7);
        check(ai.playAITurn(1) == 3 && ai.opponent().standing && ai.opponent().handCount() == 1, "pazaak_ai_stands_at_16");
        // The player stood on 19: the opponent at 17 does not stand short of it.
        me.table[1] = mainCard(9);
        me.standing = true;
        them.table[1] = mainCard(7);
        them.hand = {};
        ai.setForTest(me, them, 7);
        check(ai.playAITurn(1) == 4 && !ai.opponent().standing, "pazaak_ai_chases_a_standing_player");
        // A +/- card that reaches 20 is played on the side that does.
        them.table[1] = mainCard(7);
        them.hand = {};
        them.hand[2] = {14, 0}; // +/-3
        me.standing = false;
        ai.setForTest(me, them, 7);
        check(ai.playAITurn(1) == 2 && ai.opponentTotal() == 20 && ai.opponent().standing, "pazaak_ai_plays_to_twenty");
    }
    // DoGameSequence state 11 (StartNewGame 0067e770): after any set the player opens the next.
    {
        PazaakGame sets;
        sets.reset(0x51u);
        bool playerOpened = true;
        int setsSeen = 0;
        for (int step = 0; step < 40000 && sets.phase() != PazaakGame::Phase::MatchOver; ++step) {
            if (sets.sequenceState() == 10) {
                sets.acknowledgeSet();
                for (int wait = 0; wait < 100 && sets.phase() != PazaakGame::Phase::MatchOver
                                   && sets.sequenceState() != 3;
                     ++wait) {
                    sets.tick(0.1f);
                }
                if (sets.phase() != PazaakGame::Phase::MatchOver) {
                    ++setsSeen;
                    playerOpened = playerOpened && sets.player().tableCount() == 1 && sets.opponent().tableCount() == 0;
                }
                continue;
            }
            if (sets.awaitingPlayer()) {
                // Stand early so the opponent often takes sets.
                if (sets.playerTotal() < 14) {
                    sets.playerEndTurn();
                } else {
                    sets.playerStand();
                }
            }
            sets.tick(0.1f);
        }
        check(setsSeen >= 2 && playerOpened, "pazaak_player_opens_every_set");
    }
    // W178: PlayPazaak's bShowTutorial. CSWGuiTutorial ShowHelp 0067dfc0, ShowMultipleHelp 0067e0b0,
    // HandleInputEvent 0067d260, HandleTutorialClose 0067ea10, DoGameSequence 00680030, StartNewGame
    // 0067e770, HandleContinue 0067ec20, HandleStand 0067ed00, HandlePlayHandCard 0067ede0, HandleEndDialog
    // 0067e9a0.
    {
        PazaakGame quiet;
        quiet.reset(0x77u);
        check(!quiet.helpOpen() && !quiet.help().enabled(), "pazaak_no_help_without_the_tutorial");
        PazaakGame tut;
        tut.reset(0x77u, PazaakGame::defaultSide(), PazaakGame::defaultSide(), true);
        const PazaakHelp& help = tut.help();
        check(help.open() && help.message() == 0x96e7 && help.pageCount() == 2 && !help.twoButtons()
                  && help.okText() == PazaakHelp::kContinue && help.cancelText() == PazaakHelp::kBack,
              "pazaak_tutorial_rules_pages_at_start");
        tut.tick(0.1f);
        check(tut.sequenceState() == 0, "pazaak_tutorial_table_waits_for_help");
        tut.helpForward();
        check(help.open() && help.message() == 0x96e8 && help.twoButtons(), "pazaak_tutorial_continue_pages_on");
        tut.helpBack();
        check(help.open() && help.message() == 0x96e7 && !help.twoButtons(), "pazaak_tutorial_back_pages_back");
        tut.helpForward();
        tut.helpForward();
        check(!help.open(), "pazaak_tutorial_last_page_closes");
        for (int i = 0; i < 20 && !help.open(); ++i) {
            tut.tick(0.1f);
        }
        check(help.open() && help.message() == 0x96e9 && help.anchor() == PazaakHelp::Anchor::RightOfPlayerCard
                  && tut.player().tableCount() == 1,
              "pazaak_tutorial_first_draw_help");
        tut.helpForward();
        check(help.open() && help.message() == 0x96ea && help.pageCount() == 5
                  && help.anchor() == PazaakHelp::Anchor::AbovePlayerHand,
              "pazaak_tutorial_hand_pages_follow");
        for (int i = 0; i < 5; ++i) {
            tut.helpForward();
        }
        check(help.open() && help.message() == 0x96ed && help.pageCount() == 3
                  && help.anchor() == PazaakHelp::Anchor::AboveEndTurn,
              "pazaak_tutorial_turn_pages_follow");
        for (int i = 0; i < 3; ++i) {
            tut.helpForward();
        }
        check(!help.open() && tut.awaitingPlayer(), "pazaak_tutorial_player_turn_after_help");
        // End Turn at 16 or more asks first; Cancel leaves the turn alone, OK ends it.
        PazaakGame::Side me = tut.player();
        PazaakGame::Side them = tut.opponent();
        me.table = {};
        me.table[0] = {18 + 9, 0}; // 10
        me.table[1] = {18 + 6, 0}; // 7
        tut.setForTest(me, them, 3);
        tut.playerEndTurn();
        check(help.open() && help.message() == 0x96f1 && help.twoButtons() && help.okText() == 0x7e4c,
              "pazaak_tutorial_end_turn_asks_at_16");
        tut.helpBack();
        check(!help.open() && tut.sequenceState() == 3, "pazaak_tutorial_end_turn_cancelled");
        tut.playerEndTurn();
        tut.helpForward();
        check(tut.sequenceState() == 5, "pazaak_tutorial_end_turn_confirmed");
        // Stand at 13 or under asks; OK stands.
        me.table = {};
        me.table[0] = {18 + 4, 0}; // 5
        tut.setForTest(me, them, 3);
        tut.playerStand();
        check(help.open() && help.message() == 0x96f0 && help.okText() == 0x7e4d, "pazaak_tutorial_stand_asks_at_13");
        tut.helpForward();
        check(tut.player().standing && tut.sequenceState() == 5, "pazaak_tutorial_stand_confirmed");
        // A hand card at 12 or under asks; OK plays it with no wait.
        me.standing = false;
        me.hand = {};
        me.hand[1] = {1, 0}; // +2
        tut.setForTest(me, them, 3);
        tut.playerPlayHand(1);
        check(help.open() && help.message() == 0x96f2 && help.okText() == 0x7e4a
                  && tut.player().tableCount() == 1,
              "pazaak_tutorial_hand_card_asks_at_12");
        tut.helpForward();
        check(tut.player().tableCount() == 2 && tut.playerTotal() == 7 && tut.sequenceState() == 4
                  && tut.timer() < 0.0f,
              "pazaak_tutorial_hand_card_confirmed");
        // A bust at the end of the turn: 0x96f7, every time.
        me.table = {};
        me.table[0] = {18 + 9, 0};
        me.table[1] = {18 + 9, 0};
        me.table[2] = {18 + 1, 0}; // 22
        tut.setForTest(me, them, 5);
        tut.tick(0.1f);
        check(help.open() && help.message() == 0x96f7, "pazaak_tutorial_bust_help");
        tut.helpForward();
        for (int i = 0; i < 40 && tut.sequenceState() != 10; ++i) {
            tut.tick(0.1f);
        }
        // Between sets: 0x96f8 with End Tutorial, which ends the match.
        check(tut.sequenceState() == 10 && tut.acknowledgeSet() && help.open() && help.message() == 0x96f8
                  && help.twoButtons() && help.cancelText() == PazaakHelp::kEndTutorial
                  && help.okText() == PazaakHelp::kContinue,
              "pazaak_tutorial_set_end_offers_end_tutorial");
        tut.helpBack();
        check(!help.open() && tut.endDue(), "pazaak_tutorial_end_tutorial_ends_the_match");
        // Placement: right of the card, centred on it; above a control.
        const std::array<int, 2> right = PazaakHelp::placeNear(PazaakHelp::Anchor::RightOfPlayerCard, 100, 50, 40, 60, 290, 100);
        const std::array<int, 2> above = PazaakHelp::placeNear(PazaakHelp::Anchor::AboveEndTurn, 300, 400, 80, 20, 290, 127);
        check(right[0] == 140 && right[1] == 30 && above[0] == 300 && above[1] == 273, "pazaak_tutorial_placement");
        // The side deck panel: 0x96e3..0x96e5 when PlayPazaak asks for the tutorial.
        game.setGoldForTest(0);
        check(game.startPazaak(2, "", 0, true, 0) && game.pazaakSetupHelp().open()
                  && game.pazaakSetupHelp().message() == 0x96e3 && game.pazaakSetupHelp().pageCount() == 3,
              "pazaak_tutorial_side_deck_pages");
        game.quitPazaak();
        check(game.startPazaak(2, "", 0, false, 0) && !game.pazaakSetupHelp().open(), "pazaak_side_deck_no_help");
        game.quitPazaak();
    }

    // CItemRepository::AddItem 0055d330 / AddPazaakCard 004efb40 and PARTYTABLE ClearTable 00563200: a new
    // party owns two each of +1..+5, and a card item becomes a count, not an inventory item.
    check(game.pazaakCardsOwned(0) == 2 && game.pazaakCardsOwned(4) == 2 && game.pazaakCardsOwned(5) == 0,
          "pazaak_new_party_cards");
    const auto ncs = [](const std::vector<std::uint8_t>& code) {
        std::vector<std::uint8_t> out{'N', 'C', 'S', ' ', 'V', '1', '.', '0', 0x42};
        const std::uint32_t size = static_cast<std::uint32_t>(13 + code.size() + 2);
        for (const int shift : {24, 16, 8, 0}) {
            out.push_back(static_cast<std::uint8_t>(size >> shift));
        }
        out.insert(out.end(), code.begin(), code.end());
        out.push_back(0x20); // RETN
        out.push_back(0x00);
        return out;
    };
    const auto constInt = [](std::vector<std::uint8_t>& code, std::int32_t value, std::uint8_t type = 0x03) {
        code.push_back(0x04);
        code.push_back(type);
        for (const int shift : {24, 16, 8, 0}) {
            code.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(value) >> shift));
        }
    };
    const auto constString = [](std::vector<std::uint8_t>& code, const std::string& value) {
        code.push_back(0x04);
        code.push_back(0x05);
        code.push_back(static_cast<std::uint8_t>(value.size() >> 8));
        code.push_back(static_cast<std::uint8_t>(value.size() & 0xFF));
        code.insert(code.end(), value.begin(), value.end());
    };
    const auto action = [](std::vector<std::uint8_t>& code, std::uint16_t id, std::uint8_t argc) {
        code.push_back(0x05);
        code.push_back(0x00);
        code.push_back(static_cast<std::uint8_t>(id >> 8));
        code.push_back(static_cast<std::uint8_t>(id & 0xFF));
        code.push_back(argc);
    };
    {
        // SetGlobalNumber("MG_MADE", GetIsObjectValid(CreateItemOnObject("g_i_pazcard_003", GetFirstPC(), 1)))
        std::vector<std::uint8_t> code;
        constInt(code, 1);
        action(code, 548, 0);
        constString(code, "g_i_pazcard_003");
        action(code, 31, 3);
        action(code, 42, 1);
        constString(code, "MG_MADE");
        action(code, 581, 2);
        game.declareGlobalForTest("MG_MADE");
        const int before = game.pazaakCardsOwned(2);
        check(game.runScriptNcsForTest("mg_give_card", ncs(code), game.objectForEntityIndex(0)), "pazaak_card_script_ran");
        const auto& inventory = game.character().inventory;
        const bool cardItem = std::any_of(inventory.begin(), inventory.end(), [](const InventoryItem& item) {
            return item.baseItem == 86 || item.resref.rfind("g_i_pazcard", 0) == 0;
        });
        const bool sideboard = std::any_of(inventory.begin(), inventory.end(),
                                           [](const InventoryItem& item) { return item.baseItem == 87; });
        check(game.pazaakCardsOwned(2) == before + 1 && !cardItem && sideboard, "pazaak_card_item_becomes_a_count");
    }

    // EndPazaakGame 005f3950: the end script runs on every exit with OBJECT_SELF invalid (RunScript(script,
    // 0x7f000000)). The fixture end script records GetIsObjectValid(OBJECT_SELF) and that it ran.
    {
        std::vector<std::uint8_t> code;
        constInt(code, 0, 0x06); // OBJECT_SELF
        action(code, 42, 1);
        constString(code, "MG_SELF");
        action(code, 581, 2);
        constInt(code, 1);
        constString(code, "MG_RAN");
        action(code, 581, 2);
        game.declareGlobalForTest("MG_SELF");
        game.declareGlobalForTest("MG_RAN");
        game.addScriptForTest("mg_pazend", ncs(code));
    }
    const auto resetEndScript = [&]() {
        std::vector<std::uint8_t> code;
        constInt(code, 7);
        constString(code, "MG_SELF");
        action(code, 581, 2);
        constInt(code, 0);
        constString(code, "MG_RAN");
        action(code, 581, 2);
        game.runScriptNcsForTest("mg_pazend_reset", ncs(code), game.objectForEntityIndex(0));
    };
    const auto endScriptRanWithoutSelf = [&]() {
        return game.scriptWorld().getGlobal("MG_RAN").integer == 1 && game.scriptWorld().getGlobal("MG_SELF").integer == 0;
    };
    const std::uint32_t opponentObject = game.objectForEntityIndex(0);
    const auto chooseTen = [&]() {
        for (int kind = 0; kind < 18 && game.pazaakChosen().size() < GameSession::kPazaakDeckSize; ++kind) {
            while (game.pazaakChoose(kind)) {
            }
        }
    };

    // Script bridge: the compiled fixture calls PlayPazaak(1, "", 0, FALSE, OBJECT_SELF). With no wager
    // the start panel 00681a90 opens the side deck screen; the table follows a full ten.
    if (!scriptPath.empty()) {
        std::ifstream scriptFile(scriptPath, std::ios::binary | std::ios::ate);
        std::vector<std::uint8_t> scriptBytes;
        if (scriptFile) {
            const std::streamoff size = scriptFile.tellg();
            scriptFile.seekg(0, std::ios::beg);
            scriptBytes.resize(static_cast<std::size_t>(size));
            if (!scriptBytes.empty()) {
                scriptFile.read(reinterpret_cast<char*>(scriptBytes.data()), size);
            }
        }
        check(!scriptBytes.empty(), "pazaak_script_loaded");
        check(game.runScriptNcsForTest("minigame_script_test", scriptBytes,
                                       game.objectForEntityIndex(0)),
              "pazaak_script_ran");
        check(game.pazaakActive() && game.pazaakStage() == GameSession::PazaakStage::Setup, "pazaak_script_start");
        check(!game.pazaakPlay(), "pazaak_play_needs_ten"); // ShowDisabledButtons 0067e400
        chooseTen();
        check(game.pazaakPlay() && game.pazaakStage() == GameSession::PazaakStage::Game, "pazaak_script_table");
        if (game.pazaak() != nullptr) {
            autoplay(*game.pazaak());
            check(game.pazaak()->phase() == PazaakGame::Phase::MatchOver, "pazaak_script_match_over");
            const bool playerWon = game.pazaak()->playerWonMatch();
            game.resolvePazaakIfOver();
            check(!game.pazaakActive(), "pazaak_script_resolved");
            check(game.lastPazaakResult() == (playerWon ? 1 : 0), "pazaak_script_result");
        }
    }

    // StartPazaakGame 005f3810 / the wager popup 0067f000, 0067e150: the wager is cut to the gold held, the
    // popup opens at the maximum and moves by one, down to 1.
    game.setGoldForTest(100);
    check(game.startPazaak(2, "", 25, false, 0), "pazaak_session_start");
    check(game.pazaakStage() == GameSession::PazaakStage::Wager && game.pazaakMaxWager() == 25
              && game.pazaakWager() == 25 && game.pazaakWagerStep() == 1,
          "pazaak_wager_starts_at_the_maximum");
    check(!game.startPazaak(3, "", 10, false, 0), "pazaak_one_game_at_a_time");
    game.setPazaakWager(1000);
    check(game.pazaakWager() == 25, "pazaak_wager_clamped_to_the_maximum");
    game.setPazaakWager(0);
    check(game.pazaakWager() == 1, "pazaak_wager_minimum_one");
    game.setPazaakWager(20);
    game.confirmPazaakWager();
    check(game.pazaakStage() == GameSession::PazaakStage::Setup, "pazaak_wager_confirmed");
    // OnPanelAdded 0067f750: the ten kept in PARTYTABLE are chosen again.
    check(game.pazaakChosen().size() == GameSession::kPazaakDeckSize, "pazaak_side_list_kept");
    chooseTen();
    check(game.pazaakPlay(), "pazaak_setup_plays");
    check(game.pazaakStage() == GameSession::PazaakStage::Game && game.pazaak() != nullptr, "pazaak_table_stage");
    if (game.pazaak() != nullptr) {
        autoplay(*game.pazaak());
        const bool playerWon = game.pazaak()->playerWonMatch();
        game.resolvePazaakIfOver();
        check(game.lastPazaakResult() == (playerWon ? 1 : 0), "pazaak_session_result");
        check(game.gold() == 100 + (playerWon ? 20 : -20), "pazaak_wager");
    }
    game.setGoldForTest(10);
    game.startPazaak(2, "", 25, false, 0);
    check(game.pazaakMaxWager() == 10 && game.pazaakWager() == 10, "pazaak_wager_cut_to_gold");
    // 0067d3d0: leaving before the table loses nothing and still runs the end script, without OBJECT_SELF.
    game.stopPazaak();
    resetEndScript();
    game.setGoldForTest(100);
    game.startPazaak(2, "mg_pazend", 25, false, opponentObject);
    game.quitPazaak();
    check(!game.pazaakActive() && game.gold() == 100 && game.lastPazaakResult() == 0 && endScriptRanWithoutSelf(),
          "pazaak_quit_runs_the_end_script");
    // 0067db50: Escape at the table forfeits the wager; the end script runs the same way.
    resetEndScript();
    game.startPazaak(2, "mg_pazend", 25, false, opponentObject);
    game.confirmPazaakWager();
    chooseTen();
    game.pazaakPlay();
    game.forfeitPazaak();
    check(!game.pazaakActive() && game.gold() == 75 && game.lastPazaakResult() == 0 && endScriptRanWithoutSelf(),
          "pazaak_forfeit_loses_the_wager");
    // A won match also runs it without OBJECT_SELF and reports 1.
    resetEndScript();
    game.startPazaak(2, "mg_pazend", 0, false, opponentObject);
    chooseTen();
    game.pazaakPlay();
    game.endPazaakGame(true, 0);
    check(game.lastPazaakResult() == 1 && endScriptRanWithoutSelf(), "pazaak_win_result");

    // PARTYTABLE Save 005648c0 / Load 00565d20: the counts and the side list go through an original save.
    {
        const std::filesystem::path saveFolder = std::filesystem::temp_directory_path() / "kotor-pazaak-test-save";
        std::error_code error;
        std::filesystem::remove_all(saveFolder, error);
        const bool saved = game.saveOriginalSave(saveFolder, "Pazaak Test");
        bool same = false;
        if (saved) {
            try {
                const kotor::OriginalSave written = kotor::readOriginalSave(saveFolder);
                same = written.pazaakCards == game.partyTable().pazaakCards
                    && written.pazaakSideList == game.partyTable().pazaakSideList
                    && written.pazaakSideList[0] >= 0 && written.pazaakCards[2] == game.pazaakCardsOwned(2);
            } catch (const std::exception&) {
            }
        }
        check(saved && same, "pazaak_lists_saved");
        std::filesystem::remove_all(saveFolder, error);
    }

    // CTlkTable::ParseTokens 0041dd30: the swoop help (tutorial.2da) names its keys by token.
    {
        const std::string left = game.substituteTokens("<MGActionLeft>");
        logInfo(concat("minigame test: <MGActionLeft> -> ", left));
        check(!left.empty() && left.find('<') == std::string::npos, "swoop_help_tokens_resolve");
    }

    // The swoop and turret assertions that lived here encoded an invented 3 x 3000 m
    // track and invented targets. Those are gone; kotor_family_minigame3d plays the shipped
    // modules. What holds here: an ordinary module has no minigame, and SWMG getters there
    // return defaults rather than constants from a fake game.
    check(!game.miniGameActive(), "no_minigame_in_normal_module");
    script::Vm bridgeVm({});
    const auto enemyCount = game.action(612, {}, game.scriptWorld(), bridgeVm);
    check(!enemyCount.has_value() || enemyCount->empty() || enemyCount->front().asInt() == 0,
          "no_enemies_outside_minigame");

    logInfo(concat("minigame test: pazaak sets=", std::get<2>(pazaakA), "-",
                   std::get<3>(pazaakA), " turns=", std::get<0>(pazaakA).size()));
    logInfo(ok ? "minigame test: PASS" : "minigame test: FAIL");
    return ok ? 0 : 63;
}

int runMediaTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("media test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    const std::filesystem::path root = std::filesystem::path(game.installPath());
    std::size_t bikCount = 0;
    std::size_t bikFrames = 0;
    bool bikHeaders = true;
    std::error_code error;
    const std::filesystem::path movies = root / "movies";
    if (std::filesystem::is_directory(movies, error)) {
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(movies, error)) {
            if (error || !entry.is_regular_file()
                || entry.path().extension() != ".bik") {
                continue;
            }
            const std::uint64_t size = entry.file_size(error);
            if (error || size < 0x3c) {
                bikHeaders = false;
                continue;
            }
            std::ifstream file(entry.path(), std::ios::binary);
            std::vector<std::uint8_t> head(0x3c);
            if (!file.read(reinterpret_cast<char*>(head.data()), 0x3c)) {
                bikHeaders = false;
                continue;
            }
            const std::uint32_t frames = kotor::readU32(head.data(), head.size(), 8,
                                                         "BIK frames");
            const std::size_t table = 0x38u + (static_cast<std::size_t>(frames) + 1u) * 4u;
            std::vector<std::uint8_t> bytes(std::min<std::size_t>(
                static_cast<std::size_t>(size), table));
            file.clear();
            file.seekg(0, std::ios::beg);
            if (!file.read(reinterpret_cast<char*>(bytes.data()),
                           static_cast<std::streamsize>(bytes.size()))) {
                bikHeaders = false;
                continue;
            }
            try {
                const kotor::BikMovie movie = kotor::parseBik(
                    bytes, entry.path().filename().string(), size);
                bikHeaders = bikHeaders && movie.version == 'i'
                    && movie.width == 640
                    && (movie.height == 272 || movie.height == 360
                        || movie.height == 480);
                ++bikCount;
                bikFrames += movie.frames;
            } catch (const kotor::FormatError&) {
                bikHeaders = false;
            }
        }
    }
    check(bikCount == 61, "bik_movies");
    check(bikHeaders, "bik_headers");
    check(bikFrames > 10000, "bik_frames");

    // Native Bink pixel decode against the recorded FFmpeg RGBA oracle.
    constexpr const char* kOracleSha =
        "301f27f0e6022367e847de4861de2be72c25c1ffd0ccfdadbd920673f9bf1430";
    check(kotor::BinkPlayer::available(), "bink_decoder");
    kotor::BinkPlayer bink;
    std::string binkError;
    const std::filesystem::path firstMovie = movies / "01a.bik";
    check(bink.open(firstMovie.string(), binkError), "bink_open");
    kotor::BinkVideoFrame decodedFrame;
    check(bink.decodeFrame(0, decodedFrame, binkError), "bink_frame0_decode");
    check(decodedFrame.width == 640 && decodedFrame.height == 272
              && decodedFrame.rgba.size() == 640u * 272u * 4u,
          "bink_frame0_size");
    const std::string frameHash = kotor::sha256Bytes(
        decodedFrame.rgba.data(), decodedFrame.rgba.size());
    logInfo(concat("media test: 01a frame0 sha256=", frameHash));
    logInfo(concat("media test: 01a fps=", bink.framesPerSecond(),
                   " frames=", bink.frameCount()));
    check(frameHash == kOracleSha, "bink_frame0_sha256");
    kotor::BinkVideoFrame decodedFrame1;
    check(bink.decodeFrame(1, decodedFrame1, binkError), "bink_frame1_decode");
    check(decodedFrame1.width == 640 && decodedFrame1.height == 272,
          "bink_frame1_size");
    kotor::BinkVideoFrame replayedFrame;
    check(bink.decodeFrame(0, replayedFrame, binkError), "bink_rewind");
    check(kotor::sha256Bytes(replayedFrame.rgba.data(), replayedFrame.rgba.size())
              == kOracleSha,
          "bink_frame0_reproducible");
    check(bink.hasAudio(), "bink_audio");
    const kotor::BinkAudioTrack& binkAudio = bink.audio();
    check(binkAudio.sampleRate == 44100 && binkAudio.channels == 2,
          "bink_audio_format");
    check(binkAudio.frameCount() > 0, "bink_audio_frames");
    check(std::fabs(binkAudio.durationSeconds()
                    - static_cast<double>(bink.frameCount()) / bink.framesPerSecond())
              < 0.5,
          "bink_audio_duration");
    constexpr const char* kAudioOracleSha =
        "8a6f3225ace005d76c131a31479ecfc48bb548a3ed6fcfb0f297cc50d9c96622";
    const std::size_t audioHashBytes = 44100u * 2u * 4u;
    if (binkAudio.samples.size() * sizeof(float) >= audioHashBytes) {
        const std::string audioHash = kotor::sha256Bytes(
            reinterpret_cast<const std::uint8_t*>(binkAudio.samples.data()),
            audioHashBytes);
        logInfo(concat("media test: 01a audio rate=", binkAudio.sampleRate,
                       " ch=", binkAudio.channels,
                       " frames=", binkAudio.frameCount(),
                       " duration=", binkAudio.durationSeconds(),
                       " sha256=", audioHash));
        check(audioHash == kAudioOracleSha, "bink_audio_sha256");
    } else {
        check(false, "bink_audio_sha256");
    }
    bink.close();

    std::size_t lipMods = 0;
    std::size_t lipCount = 0;
    bool lipValid = true;
    bool lipSample = false;
    kotor::LipSync sampleLip;
    const std::filesystem::path lips = root / "lips";
    if (std::filesystem::is_directory(lips, error)) {
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(lips, error)) {
            if (error || !entry.is_regular_file()
                || entry.path().extension() != ".mod") {
                continue;
            }
            ++lipMods;
            try {
                const kotor::Archive archive = kotor::Archive::open(entry.path());
                for (const kotor::ArchiveEntry& resource : archive.entries()) {
                    if (resource.type != 3004) {
                        continue;
                    }
                    try {
                        const kotor::LipSync lip = kotor::parseLip(
                            archive.read(resource), entry.path().filename().string()
                                + "/" + resource.resref);
                        ++lipCount;
                        if (!lip.frames.empty()) {
                            if (sampleLip.frames.empty()) {
                                sampleLip = lip;
                            }
                            const std::uint8_t first = lip.visemeAt(0.0f);
                            const std::uint8_t last = lip.visemeAt(lip.duration);
                            const std::uint8_t middle = lip.visemeAt(lip.duration * 0.5f);
                            lipSample = lipSample
                                || (first == lip.frames.front().viseme
                                    && last == lip.frames.back().viseme
                                    && middle <= 15);
                        }
                    } catch (const kotor::FormatError&) {
                        lipValid = false;
                    }
                }
            } catch (const kotor::FormatError&) {
                lipValid = false;
            }
        }
    }
    check(lipMods == 123, "lip_mods");
    check(lipCount == 18206, "lip_resources");
    check(lipValid, "lip_timelines");
    check(lipSample, "lip_viseme_sample");
    check(!sampleLip.frames.empty(), "lip_playback_data");
    kotor::Install headInstall(game.installPath());
    std::size_t visemeRows = 0;
    std::vector<std::string> visemeAnimations;
    if (const auto data = headInstall.find("visemes", kotor::resType(kotor::ResType::TwoDa));
        data.has_value()) {
        try {
            const kotor::TwoDa table = kotor::TwoDa::parse(*data, "visemes");
            visemeRows = table.rowCount();
            for (std::size_t row = 0; row < table.rowCount(); ++row) {
                std::string name = table.cell(table.rowLabel(row), "animname");
                if (name.empty()) {
                    // visemes.2da shifts animname into the first column.
                    name = table.cell(row, 0);
                }
                if (!name.empty()) {
                    visemeAnimations.push_back(name);
                }
            }
        } catch (const kotor::FormatError&) {
        }
    }
    check(visemeRows == 16, "viseme_table");
    std::size_t headRows = 0;
    std::size_t headModels = 0;
    std::size_t headsWithVisemeClips = 0;
    if (const auto data = headInstall.find("heads", kotor::resType(kotor::ResType::TwoDa));
        data.has_value()) {
        try {
            const kotor::TwoDa heads = kotor::TwoDa::parse(*data, "heads");
            headRows = heads.rowCount();
            for (std::size_t row = 0; row < heads.rowCount(); ++row) {
                const std::string resref = heads.cell(std::to_string(row), "head");
                if (resref.empty()) {
                    continue;
                }
                const auto mdl = headInstall.find(resref, kotor::resType(kotor::ResType::Mdl));
                const auto mdx = headInstall.find(resref, kotor::resType(kotor::ResType::Mdx));
                if (!mdl.has_value() || !mdx.has_value()) {
                    continue;
                }
                try {
                    const kotor::Mdl model = kotor::Mdl::parse(*mdl, *mdx, resref + ".mdl");
                    ++headModels;
                    for (const std::string& animation : visemeAnimations) {
                        if (model.animationIndex(animation) >= 0) {
                            ++headsWithVisemeClips;
                            break;
                        }
                    }
                } catch (const kotor::FormatError&) {
                }
            }
        } catch (const kotor::FormatError&) {
        }
    }
    check(headRows >= 100, "head_rows");
    check(headModels >= 100, "head_models");
    check(headsWithVisemeClips == 0, "viseme_head_clips");
    logInfo(concat("media test: visemes=", visemeRows, " heads=", headRows,
                   " models=", headModels, " visemeClips=", headsWithVisemeClips));

    // Lip sync: visemes.2da names the shapes, the talk clip on the head's chain poses them.
    const FacialRig& rig = game.facialRig();
    check(rig.loaded(), "facial_rig");
    check(rig.visemeCount() == 16, "facial_visemes");
    check(rig.visemeName(0) == "Silence" && rig.visemeName(13) == "Roar",
          "facial_names");
    const auto headMdl = headInstall.find("p_CarthH", kotor::resType(kotor::ResType::Mdl));
    const auto headMdx = headInstall.find("p_CarthH", kotor::resType(kotor::ResType::Mdx));
    check(headMdl.has_value() && headMdx.has_value(), "facial_head_model");
    if (headMdl.has_value() && headMdx.has_value()) {
        try {
            const kotor::Mdl head = kotor::Mdl::parse(*headMdl, *headMdx, "p_CarthH.mdl");
            check(head.hasNode("f_jaw_g") && head.hasNode("f_lmc_g"),
                  "facial_head_bones");
            const std::vector<kotor::MdlMeshTriangle> bind =
                head.posedTriangles({}, 0.0f);
            // A head parsed alone has no chain, so these two only pin that an absent clip and an
            // absent track leave the bind pose alone; kotor_family_animation poses the real chain.
            kotor::MdlFacePose roarFace;
            roarFace.fractionA = roarFace.fractionB = FacialRig::clipFraction(13);
            roarFace.weight = 1.0f;
            kotor::MdlPoseRequest roarRequest;
            roarRequest.face = &roarFace;
            kotor::MdlPoseRequest silentRequest;
            const std::vector<kotor::MdlMeshTriangle> roar = head.posedTrianglesFor(roarRequest);
            const std::vector<kotor::MdlMeshTriangle> silence = head.posedTrianglesFor(silentRequest);
            double roarDelta = 0.0;
            double silenceDelta = 0.0;
            if (bind.size() == roar.size() && bind.size() == silence.size()) {
                for (std::size_t i = 0; i < bind.size(); ++i) {
                    for (int v = 0; v < 3; ++v) {
                        const kotor::Vec3 a = bind[i].v[v].position;
                        const kotor::Vec3 b = roar[i].v[v].position;
                        const kotor::Vec3 c = silence[i].v[v].position;
                        roarDelta = std::max(roarDelta, static_cast<double>(
                            std::fabs(a.x - b.x) + std::fabs(a.y - b.y)
                            + std::fabs(a.z - b.z)));
                        silenceDelta = std::max(silenceDelta, static_cast<double>(
                            std::fabs(a.x - c.x) + std::fabs(a.y - c.y)
                            + std::fabs(a.z - c.z)));
                    }
                }
            }
            check(roarDelta == 0.0, "facial_no_clip_bind");
            check(silenceDelta == 0.0, "facial_silence_bind");
            logInfo(concat("media test: facial jawDelta=", roarDelta,
                           " silenceDelta=", silenceDelta));
        } catch (const kotor::FormatError&) {
            check(false, "facial_head_bones");
        }
    }

    if (!sampleLip.frames.empty()) {
        game.setLipSyncForTest(sampleLip);
        check(game.lipSyncActive(), "lip_playback_active");
        check(game.lipViseme() == sampleLip.visemeAt(0.0f), "lip_playback_start");
        game.update(0.05f, InputState{});
        check(game.lipTime() > 0.0f, "lip_playback_clock");
        check(game.facialRig().viseme()
                  == static_cast<int>(game.lipViseme()),
              "facial_lip_drive");
        game.clearLipSyncForTest();
    }

    check(game.playMovie("01a"), "movie_play");
    check(game.moviePlaying() && game.movieName() == "01a", "movie_state");
    check(game.movieDuration() > 0.0f, "movie_duration");
    check(game.movieAudioVoice() != 0, "movie_audio_voice");
    check(game.audio().mixer().activeVoices(audio::Bus::Movie) == 1,
          "movie_audio_active");
    check(std::fabs(game.audio().mixer().voiceCursor(game.movieAudioVoice()))
              <= 0.001,
          "movie_audio_start");
    const kotor::BinkVideoFrame* playbackFrame = game.movieFrame();
    check(playbackFrame != nullptr && playbackFrame->width == 640
              && playbackFrame->height == 272,
          "movie_frame0_decode");
    check(playbackFrame != nullptr
              && kotor::sha256Bytes(playbackFrame->rgba.data(),
                                    playbackFrame->rgba.size()) == kOracleSha,
          "movie_frame0_sha256");
    for (int step = 0; step < 20; ++step) {
        game.update(0.05f, InputState{});
    }
    check(game.movieTime() > 0.0f, "movie_clock");
    logInfo(concat("media test: movie clock index=", game.movieFrameIndex(),
                   " time=", game.movieTime()));
    check(game.movieFrameIndex() >= 29 && game.movieFrameIndex() <= 31,
          "movie_frame_clock");
    check(std::fabs(game.audio().mixer().voiceCursor(game.movieAudioVoice())
                    - game.movieTime()) <= 0.002,
          "movie_audio_clock");
    check(game.seekMovie(5.0f), "movie_audio_seek");
    check(std::fabs(game.audio().mixer().voiceCursor(game.movieAudioVoice())
                    - 5.0f) <= 0.002,
          "movie_audio_seek_cursor");
    std::vector<float> movieMix(4800 * 2);
    {
        audio::Mixer& mixer = game.audio().mixer();
        const float music = mixer.busVolume(audio::Bus::Music);
        const float ambient = mixer.busVolume(audio::Bus::Ambient);
        const float effects = mixer.busVolume(audio::Bus::Effects);
        const float voice = mixer.busVolume(audio::Bus::Voice);
        mixer.setBusVolume(audio::Bus::Music, 0.0f);
        mixer.setBusVolume(audio::Bus::Ambient, 0.0f);
        mixer.setBusVolume(audio::Bus::Effects, 0.0f);
        mixer.setBusVolume(audio::Bus::Voice, 0.0f);
        mixer.mix(movieMix.data(), 4800);
        check(mixer.lastRms() > 0.0001f, "movie_audio_rms");
        mixer.setBusVolume(audio::Bus::Music, music);
        mixer.setBusVolume(audio::Bus::Ambient, ambient);
        mixer.setBusVolume(audio::Bus::Effects, effects);
        mixer.setBusVolume(audio::Bus::Voice, voice);
    }
    check(game.queueMovie("01c", false) && game.playMovieQueue(), "movie_queue");
    check(game.movieName() == "01c", "movie_queue_play");
    check(game.movieFrame() != nullptr && game.movieFrameIndex() == 0,
          "movie_queue_frame");
    check(game.movieAudioVoice() != 0, "movie_audio_queue");
    game.stopMovie();
    check(game.audio().mixer().activeVoices(audio::Bus::Movie) == 0,
          "movie_audio_stop");

    const std::size_t facialNpc = game.findEntity("end_trask");
    if (facialNpc != static_cast<std::size_t>(-1)) {
        check(game.startDialogue(facialNpc), "facial_scene_dialogue");
        kotor::LipSync sceneLip;
        sceneLip.name = "scene_facial";
        sceneLip.duration = 2.0f;
        sceneLip.frames.push_back({0.0f, 13});
        sceneLip.frames.push_back({2.0f, 13});
        game.setLipSyncForTest(std::move(sceneLip));
        game.update(0.05f, InputState{});
        check(game.entityHasFacialPose(facialNpc), "facial_scene_pose");
        game.clearLipSyncForTest();
        game.endDialogue();
    }

    logInfo(ok ? "media test: PASS" : "media test: FAIL");
    return ok ? 0 : 61;
}

int runAudioTest(GameSession& game, const std::string& scriptPath)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("audio test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    AudioSystem& audio = game.audio();
    logInfo(concat("audio test: music=", audio.areaAudio().musicDay,
                   " ambient=", audio.areaAudio().ambientDay,
                   " eax=", audio.areaAudio().eaxPreset,
                   " emitters=", audio.soundEmitterCount()));
    check(audio.areaAudio().musicDay == "mus_theme_sith", "area_music");
    check(audio.areaAudio().ambientDay == "al_en_starforge", "area_ambient");
    check(audio.areaAudio().eaxPreset == "HALLWAY", "area_eax");
    check(audio.soundEmitterCount() == 45, "sound_emitters");
    check(audio.playMusic(game.scene().areaAudio.musicDayId), "music_play");
    check(audio.playAmbient(game.scene().areaAudio.ambientDayId), "ambient_play");

    std::string decodeError;
    audio::SoundData mp3;
    const bool mp3Ok = audio.decodeProbe("as_el_alert", mp3, decodeError);
    if (!mp3Ok) {
        logWarn(concat("audio test: MP3 decode failed: ", decodeError));
    }
    check(mp3Ok && mp3.sampleRate > 0 && mp3.channels > 0 && mp3.frameCount() > 0,
          "mp3_decode");
    audio::SoundData wav;
    decodeError.clear();
    const bool wavOk = audio.decodeProbe("gui_click", wav, decodeError);
    if (!wavOk) {
        logWarn(concat("audio test: WAV decode failed: ", decodeError));
    }
    check(wavOk && wav.sampleRate == 22050 && wav.channels == 1 && wav.frameCount() > 0,
          "wav_decode");
    audio::SoundData resampled = mp3;
    check(audio::resample(resampled, 48000, 2) && resampled.sampleRate == 48000
              && resampled.channels == 2,
          "resample_48k_stereo");

    bool soundObjectFound = false;
    for (const kotor::SoundEmitter& emitter : game.scene().soundEmitters) {
        if (emitter.sounds.empty()) {
            continue;
        }
        soundObjectFound = true;
        check(audio.playSoundObject(emitter.entity), "sound_object_play");
        check(audio.setSoundObjectVolume(emitter.entity, 0.5f), "sound_object_volume");
        check(audio.setSoundObjectPosition(emitter.entity, emitter.position),
              "sound_object_position");
        check(audio.stopSoundObject(emitter.entity), "sound_object_stop");
        break;
    }
    check(soundObjectFound, "sound_object_template");

    kotor::Install ssfInstall(game.installPath());
    ssfInstall.bindModule(game.module());
    ssfInstall.addPacks();
    std::size_t creatureEntity = static_cast<std::size_t>(-1);
    std::string creatureSoundSet;
    std::size_t ssfSlots = 0;
    std::size_t ssfFilled = 0;
    for (std::size_t index = 0; index < game.scene().entities.size(); ++index) {
        const std::string& soundSet = game.scene().entities[index].soundSet;
        if (soundSet.empty()) {
            continue;
        }
        const auto ssfData = ssfInstall.find(soundSet, 2060);
        if (!ssfData.has_value()) {
            continue;
        }
        try {
            const kotor::Ssf ssf = kotor::Ssf::parse(*ssfData, soundSet + ".ssf");
            if (ssf.filledSlots() > 0) {
                creatureEntity = index;
                creatureSoundSet = soundSet;
                ssfSlots = ssf.slotCount();
                ssfFilled = ssf.filledSlots();
                break;
            }
        } catch (const kotor::FormatError&) {
        }
    }
    check(creatureEntity != static_cast<std::size_t>(-1) && !creatureSoundSet.empty(),
          "creature_soundset");
    check(ssfSlots >= 28, "ssf_slots");
    check(ssfFilled > 0, "ssf_filled");
    bool soundSetMapping = false;
    bool footstepTypeMapping = false;
    for (const kotor::Entity& entity : game.scene().entities) {
        if (entity.tag == "end_bridgerep1") {
            soundSetMapping = entity.soundSet == "n_repsold";
        }
        if (entity.kind == kotor::EntityKind::Creature && entity.footstepType > 0) {
            footstepTypeMapping = true;
        }
    }
    check(soundSetMapping, "soundset_2da_mapping");
    check(footstepTypeMapping, "appearance_footstep_mapping");
    int barkCount = 0;
    if (creatureEntity != static_cast<std::size_t>(-1)) {
        for (const int slot : {0, 6, 9, 12, 15}) {
            if (audio.playCreatureSound(static_cast<std::uint32_t>(creatureEntity), slot)) {
                ++barkCount;
            }
        }
    }
    check(barkCount >= 3, "creature_barks");

    // Retail 0061a2d0 skips a step further than MaxVolumeDist + 2 from the listener: step on the listener.
    const kotor::Vec3 stepAt = audio.listenerPosition();
    check(audio.playFootstep(0, 4, stepAt), "footstep_stone");
    check(audio.playFootstep(1, 10, stepAt), "footstep_metal");
    check(audio.playFootstep(2, 5, stepAt), "footstep_wood");
    check(audio.mixer().activeVoices(audio::Bus::Effects) > 0, "footstep_voices");

    // AUDIO: PriorityGroups.2da (005d7bd0) and the retail footstep rules (0061a2d0). Each check fails on
    // the old model (a soundgain.2da level, footstep rows clamped to 0..3, a mute for movies).
    {
        const auto level = [&](int group) { return audio.groupLevel(group); };
        check(std::fabs(level(AudioSystem::kGroupMusic) - 64.0f / 127.0f) < 1e-5f, "group_music_level_64_of_127");
        check(std::fabs(level(AudioSystem::kGroupGui) - 1.0f) < 1e-5f, "group_gui_level_127_of_127");
        check(audio.priorityGroup(AudioSystem::kGroupStinger).maxPlaying == 1
                  && audio.priorityGroup(AudioSystem::kGroupBark).maxPlaying == 2
                  && audio.priorityGroup(AudioSystem::kGroupGui).maxPlaying == 2,
              "group_concurrency_caps");
        check(AudioSystem::groupBus(AudioSystem::kGroupNonPlayerChat) == audio::Bus::Voice
                  && AudioSystem::groupBus(AudioSystem::kGroupMusic) == audio::Bus::Music
                  && AudioSystem::groupBus(AudioSystem::kGroupSpell) == audio::Bus::Effects,
              "group_bus_is_the_volume_category");

        AudioSystem::FootstepRequest step;
        step.position = stepAt;
        step.surface = 4; // Stone
        std::string picked;
        step.footstepType = 5; // Stomp: its own footstepsounds row, not row 3
        const bool stomp = audio.playCreatureFootstep(step, &picked) > 0;
        check(stomp && !picked.empty(), "footstep_type_5_is_a_real_row");
        step.footstepType = -1; // blank footsteptype: no sound
        check(audio.playCreatureFootstep(step, &picked) == 0, "footstep_blank_type_is_silent");
        step.footstepType = 4; // Shuffle: an empty row
        check(audio.playCreatureFootstep(step, &picked) == 0, "footstep_empty_row_is_silent");
        step.footstepType = 8; // Rolling_Droid: Rolling cell filled, no step sounds
        check(audio.playCreatureFootstep(step, &picked) == 0, "footstep_rolling_row_is_silent");
        step.footstepType = 0;
        step.surface = 12; // Swamp: blank surfacemat Name
        check(audio.playCreatureFootstep(step, &picked) == 0, "footstep_surface_12_swamp_is_silent");
        step.surface = 13; // Mud: puddles
        check(audio.playCreatureFootstep(step, &picked) > 0 && !picked.empty(), "footstep_surface_13_mud_is_puddles");
        step.position = stepAt + kotor::Vec3{500.0f, 0.0f, 0.0f};
        check(audio.playCreatureFootstep(step, &picked) == 0, "footstep_beyond_hearing_range_is_skipped");

        audio.setMovieMode(true);
        check(audio.mixer().busPaused(audio::Bus::Music) && audio.mixer().busPaused(audio::Bus::Voice)
                  && audio.mixer().busVolume(audio::Bus::Music) > 0.0f,
              "movie_pauses_the_world_buses_without_muting");
        audio.setMovieMode(false);
        check(!audio.mixer().busPaused(audio::Bus::Music), "movie_end_resumes_the_world_buses");

        // Retail battle music rule (004f3580 / 004ed110): an excited creature starts it only with an enemy
        // within 30 m and ExcitedDuration (10 s) later it stops. The old rule followed the combat flag.
        AreaAmbience& ambience = audio.ambience();
        ambience.setBattle(false);
        ambience.exciteTick(990001u, true, 8.0f, false, 0.016f);
        check(!ambience.battle(), "excited_without_an_enemy_in_30_m_starts_no_battle_music");
        ambience.exciteTick(990002u, true, 8.0f, true, 0.016f);
        check(ambience.battle(), "excited_with_an_enemy_in_30_m_starts_battle_music");
        check(audio.excitedDurationMs(1) == 10000 && audio.excitedDurationMs(2) == 10000, "excitedduration_2da_rows");
        for (int i = 0; i < 90; ++i) {
            ambience.exciteTick(990002u, false, 8.0f, true, 0.1f);
        }
        check(ambience.battle(), "battle_music_holds_before_the_excited_timer_runs_out");
        for (int i = 0; i < 20; ++i) {
            ambience.exciteTick(990002u, false, 8.0f, true, 0.1f);
        }
        check(!ambience.battle(), "battle_music_stops_when_the_excited_timer_runs_out");
        ambience.setBattle(false);

        // 005f5370: the listener sits 0.75 of the way from the camera to the followed creature.
        const SceneCamera eye = game.camera();
        const kotor::Vec3 target = game.controlledPosition();
        game.update(1.0f / 60.0f, InputState{});
        const kotor::Vec3 heard = audio.listenerPosition();
        const auto between = [](kotor::Vec3 a, kotor::Vec3 b) {
            return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
        };
        const float cameraToTarget = between(eye.eye, target);
        check(cameraToTarget < 0.1f || std::fabs(between(heard, target) - 0.25f * cameraToTarget) < 0.15f,
              "listener_is_three_quarters_of_the_way_to_the_followed_creature");
    }

    check(audio.mixer().activeVoices(audio::Bus::Music) > 0, "music_voice");
    check(audio.mixer().activeVoices(audio::Bus::Ambient) > 0, "ambient_voice");
    std::vector<float> mixed(4800 * 2);
    audio.mixer().mix(mixed.data(), 4800);
    check(audio.mixer().lastRms() > 0.0001f, "area_mix");

    auto tone = std::make_shared<audio::SoundData>();
    tone->name = "synthetic_tone";
    tone->sampleRate = 48000;
    tone->channels = 2;
    tone->samples.resize(4800 * 2);
    for (int i = 0; i < 4800; ++i) {
        const float value = std::sin(6.2831853f * 440.0f * static_cast<float>(i) / 48000.0f)
            * 0.35f;
        tone->samples[static_cast<std::size_t>(i) * 2] = value;
        tone->samples[static_cast<std::size_t>(i) * 2 + 1] = value;
    }
    audio::Mixer mixer;
    mixer.setBusVolume(audio::Bus::Effects, 1.0f);
    mixer.setReverb("NOREVERB");
    const int toneVoice = mixer.play(tone, audio::Bus::Effects, 1.0f, true);
    check(toneVoice > 0, "mixer_voice");
    std::vector<float> toneMix(4800 * 2);
    mixer.mix(toneMix.data(), 4800);
    check(mixer.lastRms() > 0.1f, "mixer_rms");
    mixer.setListener({0.0f, 0.0f, 0.0f}, 0.0f);
    mixer.setReverb("CAVE");
    const int rightVoice = mixer.play(tone, audio::Bus::Effects, 1.0f, true, true,
                                      {0.0f, -4.0f, 0.0f}, 1.0f, 10.0f);
    check(mixer.voiceGain(rightVoice) < 1.0f, "distance_attenuation");
    check(mixer.voicePan(rightVoice) > 0.5f, "stereo_pan");
    mixer.mix(toneMix.data(), 4800);
    check(mixer.lastRms() > 0.0001f, "eax_reverb");
    mixer.stopAll();
    mixer.mix(toneMix.data(), 4800);
    // The reverb tail of the positional voice keeps ringing after the voice stops
    // (retail's provider reverb does the same); with the reverb off the mixer is silent.
    check(mixer.activeVoices() == 0 && mixer.lastRms() > 0.0f, "reverb_tail_after_stop");
    mixer.setReverbEnvironment(-1, false);
    mixer.mix(toneMix.data(), 4800);
    check(mixer.activeVoices() == 0 && mixer.lastRms() == 0.0f, "stop_all");

    std::vector<float> loopMix(960 * 2);
    auto shortTone = std::make_shared<audio::SoundData>(*tone);
    shortTone->samples.resize(100 * 2);
    const int loopVoice = mixer.play(shortTone, audio::Bus::Effects, 1.0f, true);
    mixer.mix(loopMix.data(), 960);
    check(loopVoice > 0 && mixer.activeVoices() == 1, "loop_wrap");
    mixer.stopAll();

    // Per-room EAX environments: ARE Rooms/EnvAudio, matched to LYT rooms by name,
    // applied with a crossfade when the listener's room changes. The soundeax.2da
    // labels must agree with the engine's EAX preset table row for row.
    {
        bool labelsAgree = audio.eaxLabel(0) == "GENERIC";
        for (int row = 0; row < ke::audio::kEaxEnvironmentCount; ++row) {
            labelsAgree = labelsAgree && audio.eaxLabel(row) == ke::audio::eaxEnvironment(row).label;
        }
        check(labelsAgree, "soundeax_rows_match_presets");
        const auto lowerName = [](std::string text) {
            for (char& ch : text) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return text;
        };
        const std::vector<kotor::AreaRoomAudio>& rooms = audio.areaAudio().rooms;
        check(!rooms.empty(), "are_rooms_loaded");
        std::size_t matched = 0;
        for (const kotor::AreaRoomAudio& room : rooms) {
            for (const kotor::LytRoom& lyt : game.scene().rooms) {
                if (lowerName(room.room) == lowerName(lyt.room)) {
                    ++matched;
                    break;
                }
            }
        }
        check(matched == rooms.size(), "are_rooms_match_lyt");
        const kotor::AreaRoomAudio* roomA = rooms.empty() ? nullptr : &rooms.front();
        const kotor::AreaRoomAudio* roomB = nullptr;
        for (const kotor::AreaRoomAudio& room : rooms) {
            if (roomA != nullptr && room.envAudio != roomA->envAudio) {
                roomB = &room;
                break;
            }
        }
        check(roomA != nullptr && roomB != nullptr, "are_rooms_distinct_environments");
        if (roomA != nullptr && roomB != nullptr) {
            std::vector<float> roomMix(4800 * 2);
            audio.setListenerRoom(roomA->room);
            audio.mixer().mix(roomMix.data(), 4800);
            audio.mixer().mix(roomMix.data(), 4800);
            check(audio.mixer().reverbEnvironment() == roomA->envAudio, "room_a_environment");
            audio.setListenerRoom(roomB->room);
            audio.mixer().mix(roomMix.data(), 480);
            check(audio.mixer().reverbFading(), "room_change_crossfades");
            audio.mixer().mix(roomMix.data(), 4800);
            audio.mixer().mix(roomMix.data(), 4800);
            check(!audio.mixer().reverbFading()
                      && audio.mixer().reverbEnvironment() == roomB->envAudio,
                  "room_b_environment");
        }
        const ke::audio::ReverbGateResult reverbGate = ke::audio::runReverbGate();
        for (const std::string& line : reverbGate.lines) {
            logInfo(concat("reverb gate: ", line));
        }
        check(reverbGate.passed, "reverb_gate");
    }

    if (!scriptPath.empty()) {
        std::ifstream scriptFile(scriptPath, std::ios::binary | std::ios::ate);
        std::vector<std::uint8_t> scriptBytes;
        if (scriptFile) {
            const std::streamoff size = scriptFile.tellg();
            scriptFile.seekg(0, std::ios::beg);
            scriptBytes.resize(static_cast<std::size_t>(size));
            if (!scriptBytes.empty()) {
                scriptFile.read(reinterpret_cast<char*>(scriptBytes.data()), size);
            }
        }
        check(!scriptBytes.empty(), "audio_script_loaded");
        check(game.runScriptNcsForTest("audio_script_test", scriptBytes,
                                       game.objectForEntityIndex(0)),
              "audio_script_ran");
        bool played = false;
        for (const std::string& line : game.combatLog()) {
            if (line.find("sound as_el_alert") != std::string::npos) {
                played = true;
            }
        }
        check(played, "audio_script_play_sound");
        check(game.scene().areaAudio.musicDayId == 3, "audio_script_music");
        check(game.scene().areaAudio.ambientDayId == 32, "audio_script_ambient");
        // AmbientSoundSetDayVolume takes the 0..127 byte the area stores (retail
        // 005d5b50 clamps to 0x7f), so 40 is 40/127 of the ambient loop level, which plays in
        // priority group 4 (Volume 95 of 127, 0068ea10).
        check(std::fabs(audio.ambience().ambientLevel()
                        - 95.0f / 127.0f * 40.0f / 127.0f) < 0.005f,
              "audio_script_ambient_volume");
    }

    logInfo(ok ? "audio test: PASS" : "audio test: FAIL");
    return ok ? 0 : 60;
}

int runAnimationTest(GameSession& game, const std::string& scriptPath)
{
    game.setAnimationEnabled(true);
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("animation test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    if (!game.rules().loaded()) {
        logError("animation test: rules tables are not loaded");
        return 50;
    }
    kotor::Install install(game.installPath());
    install.bindModule(game.module());
    install.addPacks();
    const auto loadModel = [&](const std::string& name) -> std::optional<kotor::Mdl> {
        auto mdl = install.find(name, 2002);
        auto mdx = install.find(name, 3008);
        if (!mdl.has_value() || !mdx.has_value()) {
            return std::nullopt;
        }
        try {
            return kotor::Mdl::parse(std::move(*mdl), std::move(*mdx), name + ".mdl");
        } catch (const kotor::FormatError& exception) {
            logError(concat("animation test: ", name, " parse failed: ", exception.what()));
            return std::nullopt;
        }
    };

    const std::optional<kotor::Mdl> bith = loadModel("n_bith");
    check(bith.has_value(), "bith_loaded");
    if (!bith.has_value()) {
        return 51;
    }
    check(bith->animations().size() >= 10, "bith_clip_count");
    check(bith->supermodel() == "S_Male02", "bith_supermodel");
    const std::optional<kotor::Mdl> supermodel = loadModel("S_Male02");
    check(supermodel.has_value(), "bith_supermodel_loaded");
    if (supermodel.has_value()) {
        kotor::Mdl merged = *bith;
        merged.mergeAnimations(*supermodel);
        check(merged.animations().size() >= bith->animations().size(), "supermodel_merge");
    }
    check(std::fabs(bith->animationDuration("pause1") - 3.0f) < 0.01f, "bith_pause1_duration");
    check(std::fabs(bith->animationDuration("walk") - 1.0f) < 0.01f, "bith_walk_duration");
    check(bith->skinnedNodeCount() >= 4, "bith_skin_nodes");
    check(!bith->hasNonPoseControllers(), "no_ik_controllers");
    check(bith->skinBindError() < 0.001f, "bith_skin_bind");

    const std::vector<kotor::MdlMeshTriangle> bind = bith->meshTriangles();
    const std::vector<kotor::MdlMeshTriangle> walk = bith->posedTriangles("walk", 0.5f);
    check(!bind.empty() && bind.size() == walk.size(), "bith_mesh_stable");
    std::size_t moved = 0;
    bool finite = true;
    for (std::size_t i = 0; i < bind.size(); ++i) {
        for (int v = 0; v < 3; ++v) {
            const kotor::Vec3 a = bind[i].v[v].position;
            const kotor::Vec3 b = walk[i].v[v].position;
            finite = finite && std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.z);
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > 0.001f) {
                ++moved;
            }
        }
    }
    check(finite, "bith_pose_finite");
    check(moved > 0, "bith_pose_moves");
    const std::vector<kotor::MdlMeshTriangle> blended =
        bith->blendedTriangles("pause1", 0.0f, "walk", 0.5f, 0.5f);
    check(blended.size() == walk.size(), "blend_mesh");
    bool blendFinite = !blended.empty();
    for (const kotor::MdlMeshTriangle& triangle : blended) {
        for (const kotor::MdlMeshVertex& vertex : triangle.v) {
            blendFinite = blendFinite && std::isfinite(vertex.position.x)
                && std::isfinite(vertex.position.y) && std::isfinite(vertex.position.z);
        }
    }
    check(blendFinite, "blend_finite");
    const kotor::Vec3 rootA = bith->rootMotion("walk", 0.0f);
    const kotor::Vec3 rootB = bith->rootMotion("walk", 0.5f);
    check(std::fabs(rootA.x - rootB.x) < 0.0001f && std::fabs(rootA.y - rootB.y) < 0.0001f
              && std::fabs(rootA.z - rootB.z) < 0.0001f,
          "root_motion_in_place");

    const std::optional<kotor::Mdl> player = loadModel("PFBAM");
    check(player.has_value(), "player_loaded");
    if (player.has_value()) {
        kotor::Mdl mergedPlayer = *player;
        if (const std::optional<kotor::Mdl> female = loadModel("S_Female03");
            female.has_value()) {
            mergedPlayer.mergeAnimations(*female);
        }
        check(mergedPlayer.skinnedNodeCount() > 0, "player_skin_nodes");
        check(mergedPlayer.skinBindError() < 0.001f, "player_skin_bind");
        const std::vector<kotor::MdlMeshTriangle> playerBind = mergedPlayer.meshTriangles();
        const std::vector<kotor::MdlMeshTriangle> playerRun =
            mergedPlayer.posedTriangles("run", 0.3f);
        check(!playerBind.empty() && playerBind.size() == playerRun.size(), "player_mesh_stable");
        float bindLowZ = 1.0e9f;
        float bindHighZ = -1.0e9f;
        float maxMove = 0.0f;
        bool playerFinite = !playerRun.empty();
        for (std::size_t i = 0; i < playerBind.size() && i < playerRun.size(); ++i) {
            for (int v = 0; v < 3; ++v) {
                const kotor::Vec3 a = playerBind[i].v[v].position;
                const kotor::Vec3 b = playerRun[i].v[v].position;
                bindLowZ = std::min(bindLowZ, a.z);
                bindHighZ = std::max(bindHighZ, a.z);
                playerFinite = playerFinite && std::isfinite(b.x) && std::isfinite(b.y)
                    && std::isfinite(b.z);
                maxMove = std::max(maxMove, std::sqrt((a.x - b.x) * (a.x - b.x)
                                                      + (a.y - b.y) * (a.y - b.y)
                                                      + (a.z - b.z) * (a.z - b.z)));
            }
        }
        check(playerFinite, "player_pose_finite");
        check(bindLowZ > -0.15f && bindHighZ < 1.8f, "player_bind_bounds");
        check(maxMove > 0.01f && maxMove < 1.5f, "player_pose_motion");
    }

    const std::optional<kotor::Mdl> bantha = loadModel("c_bantha");
    check(bantha.has_value(), "bantha_loaded");
    if (bantha.has_value()) {
        check(bantha->skinnedNodeCount() >= 3, "bantha_skin_nodes");
        check(bantha->skinBindError() < 0.01f, "bantha_skin_bind");
        const std::vector<kotor::MdlMeshTriangle> posed =
            bantha->posedTriangles("cwalk", 0.5f);
        bool banthaFinite = !posed.empty();
        for (const kotor::MdlMeshTriangle& triangle : posed) {
            for (const kotor::MdlMeshVertex& vertex : triangle.v) {
                banthaFinite = banthaFinite && std::isfinite(vertex.position.x)
                    && std::isfinite(vertex.position.y) && std::isfinite(vertex.position.z);
            }
        }
        check(banthaFinite, "bantha_pose_finite");
    }

    const std::optional<kotor::Mdl> danglyModel = loadModel("c_rakghoul");
    check(danglyModel.has_value(), "dangly_loaded");
    if (danglyModel.has_value()) {
        check(danglyModel->danglyNodeCount() > 0, "dangly_nodes");
        const std::vector<kotor::MdlMeshTriangle> bindDangly = danglyModel->meshTriangles();
        const std::vector<kotor::MdlMeshTriangle> posedDangly =
            danglyModel->posedTriangles({}, 0.0f, 2.0f);
        check(!bindDangly.empty() && bindDangly.size() == posedDangly.size(), "dangly_mesh");
        std::size_t danglyMoved = 0;
        bool danglyFinite = true;
        for (std::size_t i = 0; i < bindDangly.size(); ++i) {
            for (int v = 0; v < 3; ++v) {
                const kotor::Vec3 a = bindDangly[i].v[v].position;
                const kotor::Vec3 b = posedDangly[i].v[v].position;
                danglyFinite = danglyFinite && std::isfinite(b.x) && std::isfinite(b.y)
                    && std::isfinite(b.z);
                if (std::fabs(a.x - b.x) > 0.0001f || std::fabs(a.y - b.y) > 0.0001f
                    || std::fabs(a.z - b.z) > 0.0001f) {
                    ++danglyMoved;
                }
            }
        }
        check(danglyFinite, "dangly_finite");
        check(danglyMoved > 0, "dangly_vertices");
    }

    const std::optional<kotor::Mdl> saberModel = loadModel("w_lghtsbr_001");
    check(saberModel.has_value(), "saber_loaded");
    if (saberModel.has_value()) {
        check(!saberModel->meshTriangles().empty(), "saber_static_geometry");
        check(saberModel->danglyNodeCount() == 0, "saber_no_dangly");
        check(saberModel->skinnedNodeCount() == 0, "saber_no_skin");
    }

    const std::optional<kotor::Mdl> fxModel = loadModel("fx_flame01");
    check(fxModel.has_value(), "fx_loaded");
    if (fxModel.has_value()) {
        check(fxModel->hasNonPoseControllers(), "fx_controllers_present");
        check(fxModel->hasControllerType(76) && fxModel->hasControllerType(80)
                  && fxModel->hasControllerType(84) && fxModel->hasControllerType(88)
                  && fxModel->hasControllerType(380) && fxModel->hasControllerType(392),
              "fx_controller_types");
        check(fxModel->skinnedNodeCount() == 0, "fx_no_skin");
        check(fxModel->hasFxNodes(), "fx_nodes");
        const std::set<std::int32_t> fxTypes = fxModel->fxControllerTypes();
        check(fxTypes.size() >= 30, "fx_controller_catalog");
        check(fxTypes.contains(80) && fxTypes.contains(84) && fxTypes.contains(132)
                  && fxTypes.contains(144) && fxTypes.contains(380)
                  && fxTypes.contains(392),
              "fx_controller_families");

        const auto parameterModel = [&](const kotor::Mdl& model, float time) {
            std::vector<kotor::FxNodeParameters> nodes;
            for (std::size_t ordinal = 0; ordinal < model.fxNodeCount(); ++ordinal) {
                nodes.push_back(model.fxNodeParameters(ordinal, {}, time));
            }
            return nodes;
        };
        const auto hasParameter = [](const std::vector<kotor::FxNodeParameters>& nodes,
                                     std::int32_t type) {
            for (const kotor::FxNodeParameters& node : nodes) {
                if (node.has(type)) {
                    return true;
                }
            }
            return false;
        };
        const std::vector<kotor::FxNodeParameters> fxParams0 = parameterModel(*fxModel, 0.0f);
        const std::vector<kotor::FxNodeParameters> fxParams1 = parameterModel(*fxModel, 1.0f);
        check(fxParams0.size() >= 3, "fx_param_nodes");
        check(fxParams0.size() == fxParams1.size(), "fx_param_node_stable");
        std::set<std::int32_t> modelledTypes;
        std::size_t parameterCount = 0;
        bool parameterFinite = true;
        bool parameterDeterministic = true;
        for (std::size_t n = 0; n < fxParams0.size() && n < fxParams1.size(); ++n) {
            const kotor::FxNodeParameters& a = fxParams0[n];
            const kotor::FxNodeParameters& b = fxParams1[n];
            parameterDeterministic = parameterDeterministic
                && a.nodeId == b.nodeId && a.parameters.size() == b.parameters.size();
            for (std::size_t p = 0; p < a.parameters.size(); ++p) {
                const kotor::FxParameter& pa = a.parameters[p];
                const kotor::FxParameter& pb = b.parameters[p];
                modelledTypes.insert(pa.type);
                ++parameterCount;
                parameterDeterministic = parameterDeterministic && pa.type == pb.type
                    && pa.values.size() == pb.values.size();
                for (float value : pa.values) {
                    parameterFinite = parameterFinite && std::isfinite(value);
                }
                if (p < b.parameters.size()) {
                    for (std::size_t c = 0; c < pa.values.size()
                         && c < pb.values.size(); ++c) {
                        parameterDeterministic = parameterDeterministic
                            && std::fabs(pa.values[c] - pb.values[c]) < 0.0001f;
                    }
                }
            }
        }
        check(parameterFinite, "fx_param_finite");
        check(parameterDeterministic, "fx_param_deterministic");
        check(modelledTypes == fxTypes, "fx_param_all_types");
        check(parameterCount >= 30, "fx_param_count");
        check(hasParameter(fxParams0, 80), "fx_param_80");
        check(hasParameter(fxParams0, 84), "fx_param_84");
        check(hasParameter(fxParams0, 132), "fx_param_132");
        check(hasParameter(fxParams0, 144), "fx_param_144");
        check(hasParameter(fxParams0, 380), "fx_param_380");
        check(hasParameter(fxParams0, 392), "fx_param_392");
        std::size_t remainingFamilies = 0;
        for (const std::int32_t family : fxTypes) {
            if (hasParameter(fxParams0, family)) {
                ++remainingFamilies;
            } else {
                logWarn(concat("animation test: missing FX parameter family ",
                               family));
            }
        }
        check(remainingFamilies == fxTypes.size(), "fx_param_remaining_families");
        bool parameterAlpha = false;
        for (const kotor::FxNodeParameters& node : fxParams0) {
            const kotor::FxParameter* start = node.find(84);
            const kotor::FxParameter* end = node.find(80);
            if (start != nullptr && end != nullptr && start->scalar(0, 0.0f) > 0.0f
                && end->scalar(0, 1.0f) <= start->scalar(0, 0.0f)) {
                parameterAlpha = true;
            }
        }
        check(parameterAlpha, "fx_param_alpha_curve");
    }
    std::size_t sceneAdditive = 0;
    std::size_t sceneAlpha = 0;
    std::size_t sceneOpaque = 0;
    for (const kotor::TextureBatch& batch : game.scene().batches) {
        if (batch.blend == kotor::BlendMode::Additive) {
            ++sceneAdditive;
        } else if (batch.blend == kotor::BlendMode::Alpha) {
            ++sceneAlpha;
        } else {
            ++sceneOpaque;
        }
    }
    logInfo(concat("animation test: blend batches room additive=", sceneAdditive,
                   " alpha=", sceneAlpha, " opaque=", sceneOpaque,
                   " emitters=", game.scene().emitters.size()));
    check(sceneAdditive > 0, "scene_additive_batches");
    check(sceneOpaque > 0, "scene_opaque_batches");
    check(!game.scene().emitters.empty(), "scene_model_emitters");
    const std::optional<kotor::Mdl> roomFx = loadModel("m01aa_01a");
    check(roomFx.has_value(), "room_light_loaded");
    if (roomFx.has_value()) {
        check(roomFx->hasFxNodes(), "room_light_nodes");
        const std::set<std::int32_t> roomTypes = roomFx->fxControllerTypes();
        check(roomTypes.contains(76) && roomTypes.contains(88) && roomTypes.contains(140),
              "room_light_controllers");
        check(roomFx->fxNodeCount() >= 13, "room_light_param_nodes");
        bool roomParameterFinite = true;
        bool roomParameterStable = true;
        std::size_t roomParameterTypes = 0;
        for (std::size_t ordinal = 0; ordinal < roomFx->fxNodeCount(); ++ordinal) {
            const kotor::FxNodeParameters a = roomFx->fxNodeParameters(ordinal, {}, 0.0f);
            const kotor::FxNodeParameters b = roomFx->fxNodeParameters(ordinal, {}, 1.0f);
            roomParameterStable = roomParameterStable
                && a.parameters.size() == b.parameters.size();
            for (const kotor::FxParameter& parameter : a.parameters) {
                ++roomParameterTypes;
                for (float value : parameter.values) {
                    roomParameterFinite = roomParameterFinite && std::isfinite(value);
                }
            }
            for (std::size_t p = 0; p < a.parameters.size() && p < b.parameters.size(); ++p) {
                for (std::size_t c = 0; c < a.parameters[p].values.size()
                     && c < b.parameters[p].values.size(); ++c) {
                    roomParameterStable = roomParameterStable
                        && std::fabs(a.parameters[p].values[c]
                                     - b.parameters[p].values[c]) < 0.0001f;
                }
            }
        }
        check(roomParameterFinite, "room_light_param_finite");
        check(roomParameterStable, "room_light_param_stable");
        check(roomParameterTypes >= 39, "room_light_param_types");
    }

    check(game.rules().animationName(6) == "pause1", "animation_2da_name");
    check(game.rules().animationLooping(6), "animation_2da_loop");

    std::size_t entityIndex = static_cast<std::size_t>(-1);
    std::string defaultAnimation;
    for (const kotor::EntityMesh& mesh : game.scene().entityMeshes) {
        if (!mesh.animation.empty()) {
            entityIndex = mesh.entity;
            defaultAnimation = mesh.animation;
            break;
        }
    }
    check(entityIndex != static_cast<std::size_t>(-1), "default_animation");
    check(!defaultAnimation.empty(), "default_animation_name");
    std::size_t playbackIndex = entityIndex;
    std::string playbackClip = defaultAnimation;
    for (const kotor::EntityMesh& mesh : game.scene().entityMeshes) {
        if (mesh.model && mesh.model->animationDuration("pause1") > 0.0f) {
            playbackIndex = mesh.entity;
            playbackClip = "pause1";
            break;
        }
    }
    if (playbackClip == defaultAnimation) {
        for (const kotor::EntityMesh& mesh : game.scene().entityMeshes) {
            if (mesh.model && mesh.model->animationDuration("walk") > 0.0f) {
                playbackIndex = mesh.entity;
                playbackClip = "walk";
                break;
            }
        }
    }
    if (playbackIndex != static_cast<std::size_t>(-1)) {
        const kotor::EntityMesh* mesh = nullptr;
        for (const kotor::EntityMesh& candidate : game.scene().entityMeshes) {
            if (candidate.entity == playbackIndex) {
                mesh = &candidate;
                break;
            }
        }
        std::vector<kotor::Vec3> beforeVertices;
        if (mesh != nullptr) {
            beforeVertices.reserve(mesh->vertices.size());
            for (const kotor::SceneVertex& vertex : mesh->vertices) {
                beforeVertices.push_back(vertex.position);
            }
            logInfo(concat("animation test: model ", mesh->model ? mesh->model->name() : "(none)",
                           " clip ", playbackClip, " vertices ", mesh->vertices.size(),
                           " animations ", mesh->model ? mesh->model->animations().size() : 0));
        }
        const std::uint32_t revision = game.animationRevision();
        check(game.playAnimation(playbackIndex, playbackClip, true), "play_animation");
        InputState idle;
        for (int i = 0; i < 12; ++i) {
            game.update(0.08f, idle);
        }
        check(game.animationTime(playbackIndex) > 0.0f, "playback_time");
        check(game.animationRevision() > revision, "playback_revision");
        std::size_t movedVertices = 0;
        if (mesh != nullptr && mesh->vertices.size() == beforeVertices.size()) {
            for (std::size_t i = 0; i < mesh->vertices.size(); ++i) {
                const kotor::Vec3 after = mesh->vertices[i].position;
                const kotor::Vec3 old = beforeVertices[i];
                if (std::fabs(after.x - old.x) > 0.0001f
                    || std::fabs(after.y - old.y) > 0.0001f
                    || std::fabs(after.z - old.z) > 0.0001f) {
                    ++movedVertices;
                }
            }
        }
        check(movedVertices > 0, "playback_vertices");
        check(game.animationName(playbackIndex) == playbackClip, "playback_name");
        if (mesh != nullptr && mesh->model) {
            for (const kotor::MdlAnimationInfo& info : mesh->model->animations()) {
                if (info.name == playbackClip || info.transition <= 0.0f) {
                    continue;
                }
                check(game.playAnimation(playbackIndex, info.name, true), "blend_start");
                check(mesh->blendRemaining > 0.0f, "blend_active");
                InputState blendIdle;
                game.update(0.05f, blendIdle);
                check(mesh->blendRemaining < info.transition, "blend_progress");
                for (int i = 0; i < 20; ++i) {
                    game.update(0.08f, blendIdle);
                }
                check(mesh->blendRemaining == 0.0f, "blend_finish");
                break;
            }
        }
    }

    const kotor::EntityMesh* weaponMesh = nullptr;
    std::size_t weaponEntity = static_cast<std::size_t>(-1);
    for (const kotor::EntityMesh& mesh : game.scene().entityMeshes) {
        if (mesh.attachModel) {
            weaponMesh = &mesh;
            weaponEntity = mesh.entity;
            break;
        }
    }
    check(weaponMesh != nullptr, "weapon_attachment");
    if (weaponMesh != nullptr) {
        check(weaponMesh->attachNode == "rhand" || weaponMesh->attachNode == "lhand",
              "weapon_hand_hook");
        check(weaponMesh->model != nullptr && weaponMesh->attachModel != nullptr,
              "weapon_models");
        bool weaponFinite = !weaponMesh->vertices.empty();
        for (const kotor::SceneVertex& vertex : weaponMesh->vertices) {
            weaponFinite = weaponFinite && std::isfinite(vertex.position.x)
                && std::isfinite(vertex.position.y) && std::isfinite(vertex.position.z);
        }
        check(weaponFinite, "weapon_vertices");
        if (weaponMesh->attachModel) {
            const std::optional<kotor::Mdl::Pose> hand = weaponMesh->attachModel->nodePose(
                weaponMesh->attachNode, weaponMesh->attachAnimation, weaponMesh->attachTime);
            if (hand.has_value() && !weaponMesh->vertices.empty()) {
                kotor::Vec3 centre{};
                for (const kotor::SceneVertex& vertex : weaponMesh->vertices) {
                    centre.x += vertex.position.x;
                    centre.y += vertex.position.y;
                    centre.z += vertex.position.z;
                }
                const float count = static_cast<float>(weaponMesh->vertices.size());
                centre = {centre.x / count, centre.y / count, centre.z / count};
                const float dx = centre.x - hand->translation.x;
                const float dy = centre.y - hand->translation.y;
                const float dz = centre.z - hand->translation.z;
                check(std::sqrt(dx * dx + dy * dy + dz * dz) < 2.0f, "weapon_at_hand");
            }
        }
        std::vector<kotor::Vec3> weaponBefore;
        weaponBefore.reserve(weaponMesh->vertices.size());
        for (const kotor::SceneVertex& vertex : weaponMesh->vertices) {
            weaponBefore.push_back(vertex.position);
        }
        const kotor::EntityMesh* parent = nullptr;
        for (const kotor::EntityMesh& mesh : game.scene().entityMeshes) {
            if (mesh.entity == weaponEntity && !mesh.attachModel) {
                parent = &mesh;
                break;
            }
        }
        check(parent != nullptr, "weapon_parent");
        if (parent != nullptr && parent->model) {
            const std::string clip = parent->model->animationDuration("pause1") > 0.0f
                ? "pause1" : parent->animation;
            if (!clip.empty()) {
                game.playAnimation(weaponEntity, clip, true);
                InputState idle;
                for (int i = 0; i < 10; ++i) {
                    game.update(0.08f, idle);
                }
                std::size_t weaponMoved = 0;
                if (weaponBefore.size() == weaponMesh->vertices.size()) {
                    for (std::size_t i = 0; i < weaponBefore.size(); ++i) {
                        const kotor::Vec3 after = weaponMesh->vertices[i].position;
                        if (std::fabs(after.x - weaponBefore[i].x) > 0.0001f
                            || std::fabs(after.y - weaponBefore[i].y) > 0.0001f
                            || std::fabs(after.z - weaponBefore[i].z) > 0.0001f) {
                            ++weaponMoved;
                        }
                    }
                }
                check(weaponMoved > 0, "weapon_follows_animation");
            }
        }
    }

    // The follow camera is camerastyle.2da plus the creature's camerahook. Retail measured on
    // Endar Spire and Taris: 3.2 behind, 2.1297 above pmbal's feet, 7 degrees below level, fovy 55.
    {
        const kotor::Scene::CameraStyle& style = game.scene().cameraStyle;
        check(style.viewAngle > 0.0f && style.distance > 0.0f, "camera_style_loaded");
        const SceneCamera follow = game.camera();
        if (const std::size_t led = game.controlledEntity(); led < game.scene().entities.size()
            && !game.scene().entityMeshes.empty()) {
            const kotor::Vec3 at = game.scene().entities[led].position;
            logInfo(concat("animation: follow camera eye height ", follow.eye.z - at.z, " fov ",
                           follow.fovYDegrees, " style ", style.distance, " ", style.pitch, " ", style.height));
            check(std::fabs(follow.fovYDegrees - style.viewAngle) < 0.001f, "camera_follow_fov_from_style");
            check(std::fabs(-std::asin(follow.forward.z) * 180.0f / 3.14159265f - (90.0f - style.pitch)) < 0.01f,
                  "camera_follow_pitch_from_style");
            check(follow.eye.z - at.z > style.height + 1.0f, "camera_follow_above_camerahook");
        }
    }

    check(!game.scene().cameras.empty(), "camera_catalog");
    if (!game.scene().cameras.empty()) {
        const std::uint32_t cameraId = game.scene().cameras.front().id;
        const SceneCamera cinematic = game.cinematicCamera(static_cast<std::int32_t>(cameraId));
        check(cinematic.fovYDegrees > 0.0f
                  && std::isfinite(cinematic.eye.x) && std::isfinite(cinematic.eye.y)
                  && std::isfinite(cinematic.eye.z) && std::isfinite(cinematic.forward.x)
                  && std::isfinite(cinematic.forward.y) && std::isfinite(cinematic.forward.z),
              "camera_math");
        const kotor::CameraDef& definition = game.scene().cameras.front();
        check(definition.fov <= 0.0f || std::fabs(cinematic.fovYDegrees - definition.fov) < 0.01f,
              "camera_static_fov_measured");
        check(std::fabs(cinematic.eye.z - (definition.position.z + definition.height)) < 0.001f,
              "camera_height");
        check(std::fabs(kotor::length(cinematic.forward) - 1.0f) < 0.01f, "camera_forward");
        // A shipped conversation may already be up with its own camera.
        dismissDialogue(game);
        game.setDialogueCameraForTest(static_cast<std::int32_t>(cameraId));
        const SceneCamera applied = game.camera();
        check(std::fabs(applied.eye.x - cinematic.eye.x) < 0.001f
                  && std::fabs(applied.eye.y - cinematic.eye.y) < 0.001f
                  && std::fabs(applied.eye.z - cinematic.eye.z) < 0.001f,
              "dialogue_camera_apply");
        game.endDialogue();
    }
    const auto cameraDlgData = install.find("m01aa_c01", 2029);
    check(cameraDlgData.has_value(), "camera_dialogue");
    if (cameraDlgData.has_value()) {
        try {
            kotor::Gff dlgGff = kotor::Gff::parse(*cameraDlgData, "m01aa_c01.dlg");
            const kotor::Dlg dlg = kotor::Dlg::parse(dlgGff, nullptr, "m01aa_c01.dlg");
            check(dlg.cameraModel == "m01aa_c01_cam", "camera_model");
            std::int32_t animatedValue = -1;
            for (const kotor::DlgEntry& entry : dlg.entries) {
                if (entry.cameraAngle == 4 && entry.cameraAnimation >= 1200) {
                    animatedValue = entry.cameraAnimation;
                    break;
                }
            }
            check(animatedValue >= 0, "camera_animation_field");
            check(game.setDialogueCameraAnimationForTest(dlg.cameraModel, "CUT001W"),
                  "camera_clip_load");
            const SceneCamera frame0 = game.camera();
            check(std::isfinite(frame0.eye.x) && std::isfinite(frame0.eye.y)
                      && std::isfinite(frame0.eye.z)
                      && std::fabs(kotor::length(frame0.forward) - 1.0f) < 0.01f,
                  "camera_clip_pose");
            InputState idle;
            game.update(1.0f, idle);
            const SceneCamera frame1 = game.camera();
            const float dx = frame1.eye.x - frame0.eye.x;
            const float dy = frame1.eye.y - frame0.eye.y;
            const float dz = frame1.eye.z - frame0.eye.z;
            check(std::sqrt(dx * dx + dy * dy + dz * dz) > 0.001f, "camera_clip_advance");
            game.endDialogue();
            for (int angle = 1; angle <= 3; ++angle) {
                const SceneCamera preset = game.presetCamera(
                    angle, 0.35f, 0.1f, {0.0f, 0.0f, 0.0f}, {0.0f, 3.0f, 0.0f});
                check(std::isfinite(preset.eye.x) && std::isfinite(preset.eye.y)
                          && std::isfinite(preset.eye.z)
                          && std::fabs(kotor::length(preset.forward) - 1.0f) < 0.01f,
                      angle == 1 ? "camera_preset_1"
                          : angle == 2 ? "camera_preset_2" : "camera_preset_3");
            }
            // Angle 2, over the other's shoulder. Numbers logged at
            // retail 006bb7b0 with Trask framed and the PC behind: eye 14.834, 20.540, 0.263. Fails
            // on the old preset (no angle 0, side 1 for angle 1 and -30 degrees only for angle 2).
            {
                const kotor::Vec3 trask{16.4923f, 20.2739f, 0.3589f};
                const kotor::Vec3 pc{15.4447f, 20.1241f, 0.3122f};
                const SceneCamera shot = game.presetCamera(2, 0.0f, 0.0f, trask, pc, 2);
                check(std::fabs(shot.eye.x - 14.834f) < 0.002f && std::fabs(shot.eye.y - 20.540f) < 0.002f
                          && std::fabs(shot.eye.z - 0.2632f) < 0.002f && std::fabs(shot.fovYDegrees - 55.0f) < 0.001f,
                      "camera_preset_0_over_the_shoulder");
                const SceneCamera back = game.presetCamera(2, 0.0f, 0.0f, pc, trask, 1);
                check(std::fabs(back.eye.x - 16.9613f) < 0.002f && std::fabs(back.eye.y - 20.8441f) < 0.002f,
                      "camera_swapped_pair_keeps_its_side");
                // Close-ups, measured: Trask framed, side 2, and the PC framed, side 1.
                const SceneCamera closeNpc = game.presetCamera(1, 0.0f, 0.0f, trask, pc, 2);
                const SceneCamera closePc = game.presetCamera(1, 0.0f, 0.0f, pc, trask, 1);
                check(std::fabs(closeNpc.eye.x - 15.832f) < 0.006f && std::fabs(closeNpc.eye.y - 20.437f) < 0.006f
                          && std::fabs(closePc.eye.x - 16.036f) < 0.006f && std::fabs(closePc.eye.y - 20.465f) < 0.006f
                          && std::fabs(closePc.eye.z - 0.272f) < 0.002f,
                      "camera_preset_1_close_up");
                // Two-shot, logged at 006bb7b0: eye 16.193, 18.628.
                const SceneCamera two = game.presetCamera(3, 0.0f, 0.0f, trask, pc, 1);
                check(std::fabs(two.eye.x - 16.1933f) < 0.002f && std::fabs(two.eye.y - 18.6276f) < 0.002f,
                      "camera_preset_3_two_shot");
            }
            bool camVidDefaults = true;
            for (const kotor::DlgEntry& entry : dlg.entries) {
                camVidDefaults = camVidDefaults && entry.camVidEffect <= 0;
            }
            for (const kotor::DlgReply& reply : dlg.replies) {
                camVidDefaults = camVidDefaults && reply.camVidEffect <= 0;
            }
            check(camVidDefaults, "camvid_defaults");
        } catch (const kotor::FormatError&) {
            check(false, "camera_dialogue");
        }
    }
    const auto dlgData = install.find("end_trask01", 2029);
    if (dlgData.has_value()) {
        try {
            kotor::Gff dlgGff = kotor::Gff::parse(*dlgData, "end_trask01.dlg");
            const kotor::Dlg dlg = kotor::Dlg::parse(dlgGff, nullptr, "end_trask01.dlg");
            bool cameraSeen = false;
            for (const kotor::DlgEntry& entry : dlg.entries) {
                cameraSeen = cameraSeen || entry.cameraId >= 0;
            }
            for (const kotor::DlgReply& reply : dlg.replies) {
                cameraSeen = cameraSeen || reply.cameraId >= 0;
            }
            check(cameraSeen, "dlg_camera_field");
        } catch (const kotor::FormatError&) {
            check(false, "dlg_camera_field");
        }
    }

    int camVidDlgs = 0;
    bool camVidNonDefault = false;
    std::map<std::int32_t, int> camVidValues;
    const std::filesystem::path modulesPath = std::filesystem::path(game.installPath()) / "modules";
    std::error_code directoryError;
    for (const std::filesystem::directory_entry& item :
         std::filesystem::directory_iterator(modulesPath, directoryError)) {
        if (!item.is_regular_file() || item.path().extension() != ".rim") {
            continue;
        }
        try {
            const kotor::Archive archive = kotor::Archive::open(item.path());
            for (const kotor::ArchiveEntry& entry : archive.entries()) {
                if (entry.type != 2029) {
                    continue;
                }
                try {
                    kotor::Gff dlgGff = kotor::Gff::parse(archive.read(entry), entry.resref + ".dlg");
                    const kotor::Dlg dlg = kotor::Dlg::parse(dlgGff, nullptr, entry.resref);
                    ++camVidDlgs;
                    for (const kotor::DlgEntry& dialogueEntry : dlg.entries) {
                        ++camVidValues[dialogueEntry.camVidEffect];
                        camVidNonDefault = camVidNonDefault || dialogueEntry.camVidEffect > 0;
                    }
                    for (const kotor::DlgReply& reply : dlg.replies) {
                        ++camVidValues[reply.camVidEffect];
                        camVidNonDefault = camVidNonDefault || reply.camVidEffect > 0;
                    }
                } catch (const kotor::FormatError&) {
                }
            }
        } catch (const kotor::FormatError&) {
        }
    }
    check(camVidDlgs > 0, "camvid_corpus_dlgs");
    check(!camVidNonDefault, "camvid_corpus_defaults");
    logInfo(concat("animation test: camvid_corpus dlgs=", camVidDlgs,
                   " nonDefault=", camVidNonDefault ? 1 : 0,
                   " values=", camVidValues[-1], "/", camVidValues[0]));

    if (!scriptPath.empty()) {
        std::ifstream scriptFile(scriptPath, std::ios::binary | std::ios::ate);
        std::vector<std::uint8_t> scriptBytes;
        if (scriptFile) {
            const std::streamoff size = scriptFile.tellg();
            scriptFile.seekg(0, std::ios::beg);
            scriptBytes.resize(static_cast<std::size_t>(size));
            if (!scriptBytes.empty()) {
                scriptFile.read(reinterpret_cast<char*>(scriptBytes.data()), size);
            }
        }
        check(!scriptBytes.empty(), "animation_script_loaded");
        std::size_t scriptedEntity = static_cast<std::size_t>(-1);
        for (const kotor::EntityMesh& mesh : game.scene().entityMeshes) {
            if (mesh.model && mesh.model->animationDuration("pause1") > 0.0f) {
                scriptedEntity = mesh.entity;
                break;
            }
        }
        check(scriptedEntity != static_cast<std::size_t>(-1), "animation_script_target");
        if (scriptedEntity != static_cast<std::size_t>(-1)) {
            game.stopAnimation(scriptedEntity);
            check(game.runScriptNcsForTest("anim_script_test", scriptBytes,
                                           game.objectForEntityIndex(scriptedEntity)),
                  "animation_script_ran");
            check(game.animationName(scriptedEntity) == "pause1", "animation_script_play");
        }
    }

    logInfo(ok ? "animation test: PASS" : "animation test: FAIL");
    return ok ? 0 : 52;
}

int runCombatForceTest(GameSession& game, const std::string& scriptPath)
{
    if (!game.rules().loaded()) {
        logError("combat/force test: rules tables are not loaded");
        return 40;
    }
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("combat force test: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };

    CharacterSheet sheet = game.rules().createCharacter(3, 5);
    sheet.maxHp = game.rules().maxHitPoints(3, 5, sheet.con);
    sheet.hp = 20;
    sheet.fp = 400;
    sheet.maxFp = 400;
    sheet.alignment = 50;
    sheet.featPoints = 10;
    sheet.feats.insert(28); // POWER_ATTACK
    sheet.feats.insert(11); // FLURRY
    sheet.feats.insert(29); // POWER_BLAST
    sheet.feats.insert(36); // WEAPON_FOCUS_LIGHTSABER
    sheet.feats.insert(50); // WEAPON_SPEC_LIGHTSABER
    sheet.powers.insert(28); // FORCE_POWER_HEAL
    sheet.powers.insert(35); // FORCE_POWER_LIGHTNING
    sheet.powers.insert(17); // FORCE_POWER_FORCE_ARMOR
    sheet.powers.insert(46); // FORCE_POWER_STUN
    sheet.powers.insert(20); // FORCE_POWER_FORCE_IMMUNITY
    sheet.powers.insert(19); // FORCE_POWER_FORCE_BREACH
    sheet.powers.insert(48); // FORCE_POWER_SUPRESS_FORCE
    sheet.powers.insert(24); // FORCE_POWER_FORCE_SHIELD
    sheet.feats.insert(101); // FORCE_JUMP
    sheet.feats.insert(102); // FORCE_JUMP_ADVANCED
    sheet.feats.insert(103); // FORCE_JUMP_MASTERY
    sheet.feats.insert(97); // FORCE_CAMOFLAGE
    sheet.feats.insert(55); // JEDI_DEFENSE
    sheet.feats.insert(1); // ADVANCED_JEDI_DEFENSE
    sheet.feats.insert(24); // MASTER_JEDI_DEFENSE
    InventoryItem saber = game.rules().makeItem("saber", 8);
    saber.equipped = true;
    sheet.inventory.push_back(saber);
    game.setCharacterForTest(sheet);

    check(game.maxForcePoints() == 400, "jedi_force_points");
    int forcePowerCount = 0;
    for (std::int32_t id = 0; id < 200; ++id) {
        if (game.rules().forcePower(id).has_value()) {
            ++forcePowerCount;
        }
    }
    check(forcePowerCount == 44, "force_catalog_44");
    check(game.rules().featCount() == 125, "feat_catalog_125");
    int rangedEntities = 0;
    for (const kotor::Entity& entity : game.scene().entities) {
        if (entity.ranged) {
            ++rangedEntities;
        }
    }
    check(rangedEntities > 0, "ranged_entities_detected");
    CharacterSheet passive = sheet;
    passive.feats = {7}; // CAUTIOUS
    check(game.rules().skillBonus(passive, "Demolitions") == 1, "caution_1");
    passive.feats.insert(117); // IMPROVED_CAUTION
    check(game.rules().skillBonus(passive, "Stealth") == 2, "caution_2");
    passive.feats.insert(118); // MASTER_CAUTION
    check(game.rules().skillBonus(passive, "Demolitions") == 3, "caution_3");
    passive.feats.insert(10); // EMPATHY
    check(game.rules().skillBonus(passive, "Persuade") == 1, "empathy_1");
    passive.feats.insert(12); // GEAR_HEAD
    check(game.rules().skillBonus(passive, "Repair") == 1, "gear_head_1");
    const std::int32_t baseFort = game.rules().savingThrow(3, 5, 0);
    passive.feats.insert(13); // CONDITIONING
    check(game.rules().savingThrow(passive, 0) == baseFort + 1, "conditioning_1");
    passive.feats.insert(21); // IMPROVED_CONDITIONING
    check(game.rules().savingThrow(passive, 0) == baseFort + 2, "conditioning_2");
    passive.feats.insert(22); // MASTER_CONDITIONING
    check(game.rules().savingThrow(passive, 0) == baseFort + 3, "conditioning_3");
    passive.feats.insert(60); // SNEAK_ATTACK_1D6
    check(game.rules().sneakAttackDice(passive) == 1, "sneak_1");
    passive.feats.insert(61); // SNEAK_ATTACK_2D6
    check(game.rules().sneakAttackDice(passive) == 2, "sneak_2");
    passive.feats.insert(123); // IMPROVED_TOUGHNESS
    check(game.rules().damageReduction(passive) == 2, "damage_reduction_2");
    const std::int32_t baseAc = game.rules().armorClass(passive);
    // retail 004ed1d0 has no feat term for these: Scoundrel's Luck and Jedi Sense are the class
    // columns of acbonus.2da (scd, jdg, jdc, jds), so the feats alone leave Defense as it is.
    for (const std::int32_t feat : {104, 105, 106, 107, 108, 109}) {
        passive.feats.insert(feat);
    }
    check(game.rules().armorClass(passive) == baseAc, "luck_and_sense_feats_add_nothing_004ed1d0");
    const std::int32_t baseDc = game.rules().forcePowerSaveDC(passive);
    passive.feats.insert(88); // FORCE_FOCUS
    check(game.rules().forcePowerSaveDC(passive) == baseDc + 1, "force_focus_1");
    passive.feats.insert(89); // FORCE_FOCUS_ADVANCED
    check(game.rules().forcePowerSaveDC(passive) == baseDc + 2, "force_focus_2");
    passive.feats.insert(90); // FORCE_FOCUS_MASTERY
    check(game.rules().forcePowerSaveDC(passive) == baseDc + 4, "force_focus_4");
    {
        // Retail 004ef6e0: levels + WIS mod + CHA mod + 5 + focus (old: 10 + level/2 + WIS + focus).
        CharacterSheet dcSheet = passive;
        dcSheet.level = 1;
        dcSheet.secondClassId = -1;
        dcSheet.wis = 14;
        dcSheet.cha = 12;
        check(game.rules().forcePowerSaveDC(dcSheet) == 1 + 2 + 1 + 5 + 4, "force_save_dc_retail_004ef6e0");
        dcSheet.level = 10;
        check(game.rules().forcePowerSaveDC(dcSheet) == 10 + 2 + 1 + 5 + 4, "force_save_dc_retail_level10");
    }
    passive.feats.insert(95); // WOOKIE_ENDURANCE
    check(game.rules().damageReduction(passive) == 2, "wookiee_endurance_dr");
    check(game.rules().toughnessBonus(passive) >= 2 * passive.level, "wookiee_endurance_hp");
    CharacterSheet noProf;
    check(!game.rules().canEquip(noProf, game.rules().makeItem("sword", 2)),
          "weapon_proficiency_required");
    check(game.rules().canEquip(sheet, game.rules().makeItem("robe", 35)),
          "jedi_robe_proficiency");
    check(!game.rules().canEquip(sheet, game.rules().makeItem("heavy", 43)),
          "heavy_armor_proficiency_required");
    check(game.rules().canEquip(game.rules().createCharacter(0, 1),
                                game.rules().makeItem("heavy", 43)),
          "soldier_heavy_armor");
    CharacterSheet dual = sheet;
    InventoryItem mainHand = game.rules().makeItem("main", 2);
    mainHand.equipped = true;
    InventoryItem offHand = game.rules().makeItem("off", 8);
    offHand.equipped = true;
    dual.inventory = {mainHand, offHand};
    check(game.rules().twoWeaponPenalty(dual) == -6, "two_weapon_penalty");
    dual.feats.insert(3);
    check(game.rules().twoWeaponPenalty(dual) == -4, "two_weapon_fighting");
    dual.feats.insert(9);
    check(game.rules().twoWeaponPenalty(dual) == -2, "two_weapon_advanced");
    dual.feats.insert(85);
    check(game.rules().twoWeaponPenalty(dual) == 0, "two_weapon_mastery");
    const std::size_t hostile = firstHostile(game);
    check(hostile != static_cast<std::size_t>(-1), "hostile_found");
    if (hostile == static_cast<std::size_t>(-1)) {
        return 41;
    }
    kotor::Entity& target = game.scene().entities[hostile];
    target.hitPoints = 200;
    target.maxHitPoints = 200;
    target.fortSave = -100;
    target.willSave = -100;
    target.refSave = -100;
    target.dead = false;
    target.position = game.position();
    target.position.x += 1.0f;
    const std::uint32_t targetObject = game.objectForEntityIndex(hostile);

    // Force powers run through the cast action and the shipped impact script, as in
    // retail 00514af0 AIActionCastSpell: impact (and the Force point spend) at spells.2da
    // conjtime, the action ends at conjtime + casttime. The old engine path spent the
    // points, applied invented effects and shifted alignment +-2 on the spot.
    const auto runFor = [&](float seconds) {
        InputState idle;
        for (float t = 0.0f; t < seconds; t += 0.02f) {
            game.update(0.02f, idle);
        }
    };
    ScriptApi castApi{game};
    const auto casting = [&]() {
        const ActorState& pcState = castApi.actor(castApi.pc());
        return std::any_of(pcState.queue.begin(), pcState.queue.end(), [](const ActorAction& queued) {
            return queued.kind == ActorActionKind::CastSpell;
        });
    };
    castApi.clearQueue(castApi.pc());
    const std::int32_t fpBefore = game.forcePoints();
    const std::int32_t hpBefore = target.hitPoints;
    const Rules::SpellTimes lightningTimes = game.rules().spellTimes(35);
    check(lightningTimes.conjure == 170 && lightningTimes.cast == 1330, "lightning_spell_times");
    check(game.useForcePower(35, targetObject), "lightning_used");
    check(game.forcePoints() == fpBefore, "lightning_not_instant");
    runFor(0.1f);
    check(game.forcePoints() == fpBefore && target.hitPoints == hpBefore, "lightning_conjuring");
    runFor(0.2f);
    check(game.forcePoints() == fpBefore - 20, "lightning_cost_at_conjtime");
    check(target.hitPoints < hpBefore, "lightning_impact_script_damage");
    check(game.alignment() == 50, "cast_keeps_alignment");
    check(casting(), "cast_action_runs_casttime");
    runFor(1.3f);
    check(!casting(), "cast_action_ends_conj_plus_cast");

    // retail 0059b8a0: forceadjust.2da row goodEvil/10, GoodCost for 'G', EvilCost for 'E',
    // party members only, truncated. Lightning (E, 20), Stun (G, 20), Push (-, 10?).
    check(game.rules().adjustedForceCost(35, 0, true) == 10, "force_cost_dark_evil_power");
    check(game.rules().adjustedForceCost(35, 100, true) == 35, "force_cost_light_evil_power");
    check(game.rules().adjustedForceCost(46, 100, true) == 10, "force_cost_light_good_power");
    check(game.rules().adjustedForceCost(46, 0, true) == 35, "force_cost_dark_good_power");
    check(game.rules().adjustedForceCost(35, 0, false) == 20, "force_cost_npc_flat");
    check(game.rules().adjustedForceCost(35, 55, true) == 20, "force_cost_neutral");

    game.character().hp = 5;
    const std::int32_t hpBeforeHeal = game.character().hp;
    check(game.useForcePower(28, 0), "heal_used");
    runFor(2.0f);
    check(game.character().hp > hpBeforeHeal, "heal_impact_script");
    check(game.alignment() == 50, "heal_keeps_alignment");

    for (std::size_t i = 0; i < game.scene().entities.size(); ++i) {
        if (i != hostile && game.scene().entities[i].hostile) {
            game.scene().entities[i].position.x += 1000.0f;
        }
    }
    target.position = game.position();
    target.position.x += 12.0f;
    game.attackNearestHostile();
    check(game.forceJumpBonus() == 4, "force_jump_bonus");
    const float jumpDx = game.position().x - target.position.x;
    const float jumpDy = game.position().y - target.position.y;
    check(std::sqrt(jumpDx * jumpDx + jumpDy * jumpDy) < 3.0f, "force_jump_closed");
    check(game.rules().jediDefenseBonus(sheet) == 6, "jedi_defense_6");
    check(game.rules().jediDeflects(sheet, 10, 5), "jedi_deflect_yes");
    check(!game.rules().jediDeflects(sheet, 20, 5), "jedi_deflect_no");
    check(game.canStealth(), "force_camouflage_stealth");
    check(game.enterStealth(), "stealth_enter");
    check(game.stealthActive(), "stealth_active");
    InputState stealthInput;
    game.update(0.1f, stealthInput);
    check(game.combatTargetName().empty(), "stealth_no_combat_target");
    game.attackNearestHostile();
    check(!game.stealthActive(), "stealth_broken_by_attack");

    target.ranged = true;
    target.attackBonus = 10;
    target.damageMin = 0;
    target.damageMax = 0;
    bool deflected = false;
    for (int attempt = 0; attempt < 200 && !deflected; ++attempt) {
        game.attackPlayerForTest(hostile);
        deflected = hasEvent(game, "player deflects " + target.tag);
    }
    check(deflected, "jedi_deflect_integration");

    const std::int32_t hpBeforeFeat = game.character().maxHp;
    check(game.useFeat(28, targetObject), "power_attack_used");
    check(game.combatMode() == CombatMode::PowerAttack, "power_attack_mode");
    check(game.useFeat(29, targetObject), "power_blast_used");
    check(game.combatMode() == CombatMode::PowerAttack, "power_blast_mode");
    check(game.useFeat(11, targetObject), "flurry_used");
    check(game.combatMode() == CombatMode::Flurry, "flurry_mode");
    check(game.learnFeat(84), "toughness_learned");
    check(game.character().maxHp > hpBeforeFeat, "toughness_hp");

    if (!scriptPath.empty()) {
        std::ifstream scriptFile(scriptPath, std::ios::binary | std::ios::ate);
        std::vector<std::uint8_t> scriptBytes;
        if (scriptFile) {
            const std::streamoff size = scriptFile.tellg();
            scriptFile.seekg(0, std::ios::beg);
            scriptBytes.resize(static_cast<std::size_t>(size));
            if (!scriptBytes.empty()) {
                scriptFile.read(reinterpret_cast<char*>(scriptBytes.data()), size);
            }
        }
        const std::int32_t fpBeforeScript = game.forcePoints();
        check(!scriptBytes.empty(), "force_script_loaded");
        check(game.runScriptNcsForTest("force_script_test", scriptBytes,
                                       game.scriptWorld().pc), "force_script_ran");
        const auto scriptLocal = [&](const char* name) {
            return game.scriptWorld().localNumber(game.scriptWorld().pc, std::strtol(name, nullptr, 10));
        };
        // A local number is one byte (retail 00543130): a value outside -128..127 is not stored,
        // and what is stored reads back unsigned.
        const auto asLocalByte = [](std::int32_t value) {
            return value < -128 || value > 127 ? 0 : (value & 0xFF);
        };
        check(scriptLocal("0") == 77, "script_alignment_read");
        check(scriptLocal("1") == 1, "script_has_feat");
        check(scriptLocal("2") == 1, "script_has_spell");
        check(scriptLocal("3") == asLocalByte(fpBeforeScript), "script_force_points");
        check(scriptLocal("4") == asLocalByte(game.maxForcePoints()), "script_max_force_points");
        // TalentSpell is TALENT_TYPE_FORCE, 0. retail 00544c70
        check(scriptLocal("5") == 0, "script_talent_type");
        check(scriptLocal("6") == 35, "script_talent_id");
        check(game.combatMode() == CombatMode::PowerAttack, "script_action_feat");
        // The feat queued an attack round on the current combat target ahead of the casts;
        // drop it so the queued casts run inside the window this check gives them.
        std::erase_if(ScriptApi{game}.actor(game.scriptWorld().pc).queue, [](const ActorAction& action) {
            return action.kind == ActorActionKind::Attack;
        });
        runFor(4.0f);
        check(ScriptApi{game}.actor(game.scriptWorld().pc).combat.lastForcePower == 17,
              "script_action_spell");
        // Casting shifts no alignment in retail (ModifyAlignment 005ae610 callers).
        check(game.alignment() == 77, "script_alignment_action");
    }

    {
        // The save round trip of a Force effect on the sheet (the effect itself now comes
        // from the impact script; this only exercises persistence).
        kotor::ActiveEffect armor;
        armor.spellId = 17;
        armor.label = game.rules().powerName(17);
        armor.remaining = 30.0f;
        armor.armorBonus = 5;
        armor.force = true;
        armor.dispellable = true;
        game.character().effects.push_back(armor);
    }
    check(game.hasEffect("FORCE_ARMOR"), "persist_effect_added");
    check(game.hasEffect("FORCE_ARMOR"), "persist_effect_live");
    game.setAlignment(20);
    game.adjustAlignment(5);
    check(game.alignment() == 25, "alignment_adjust");

    const std::filesystem::path temp = std::filesystem::temp_directory_path()
        / "kotor-s11-combat-force";
    std::error_code error;
    std::filesystem::remove_all(temp, error);
    const std::filesystem::path engineSave = temp / "player.txt";
    check(game.save(engineSave), "engine_save");
    GameSession restored(game.installPath(), game.module());
    check(restored.load(engineSave), "engine_load");
    check(restored.alignment() == 25, "alignment_engine_roundtrip");
    check(restored.forcePoints() == game.forcePoints(), "force_engine_roundtrip");
    check(restored.hasEffect("FORCE_ARMOR"), "effect_engine_roundtrip");
    check(restored.forceArmorBonus() > 0, "effect_bonus_engine_roundtrip");

    const std::filesystem::path originalSlot = temp / "000012 - ENGINE";
    check(game.saveOriginalSave(originalSlot, "S11 COMBAT"), "original_save");
    const kotor::OriginalSave written = kotor::readOriginalSave(originalSlot);
    check(written.alignment == 25, "alignment_original_roundtrip");
    check(written.forcePoints == game.forcePoints(), "force_original_roundtrip");
    check(!written.effects.empty(), "effect_original_roundtrip");
    check(std::find(written.powers.begin(), written.powers.end(), 35) != written.powers.end(),
          "powers_original_roundtrip");
    check(std::find(written.feats.begin(), written.feats.end(), 28) != written.feats.end(),
          "feats_original_roundtrip");
    GameSession reloaded(game.installPath(), game.module());
    check(reloaded.loadOriginalSave(originalSlot), "original_load");
    check(reloaded.alignment() == 25, "alignment_original_load");
    check(reloaded.character().powers.contains(35), "powers_original_load");
    check(reloaded.character().feats.contains(28), "feats_original_load");
    check(reloaded.hasEffect("FORCE_ARMOR"), "effect_original_load");
    check(reloaded.forceArmorBonus() > 0, "effect_bonus_original_load");

    logInfo(ok ? "combat force test: PASS" : "combat force test: FAIL");
    return ok ? 0 : 42;
}

// Assembles small NCS fixtures for the world-simulation gate, so the assertions
// run real bytecode through the real VM against shipped data.
class NcsFixture {
public:
    NcsFixture& constInt(std::int32_t value)
    {
        op(0x04, 0x03);
        be32(static_cast<std::uint32_t>(value));
        return *this;
    }
    NcsFixture& constFloat(float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        op(0x04, 0x04);
        be32(bits);
        return *this;
    }
    NcsFixture& constString(const std::string& value)
    {
        op(0x04, 0x05);
        code_.push_back(static_cast<std::uint8_t>(value.size() >> 8));
        code_.push_back(static_cast<std::uint8_t>(value.size() & 0xFF));
        code_.insert(code_.end(), value.begin(), value.end());
        return *this;
    }
    NcsFixture& constObject(std::uint32_t value) // 0 is OBJECT_SELF, as compiled.
    {
        op(0x04, 0x06);
        be32(value);
        return *this;
    }
    NcsFixture& action(std::uint16_t id, std::uint8_t argc)
    {
        op(0x05, 0x00);
        code_.push_back(static_cast<std::uint8_t>(id >> 8));
        code_.push_back(static_cast<std::uint8_t>(id & 0xFF));
        code_.push_back(argc);
        return *this;
    }
    NcsFixture& copyTop() // CPTOPSP -4, 4: duplicate the top cell.
    {
        op(0x03, 0x01);
        be32(static_cast<std::uint32_t>(-4));
        code_.push_back(0x00);
        code_.push_back(0x04);
        return *this;
    }
    NcsFixture& returns()
    {
        op(0x20, 0x00);
        return *this;
    }
    NcsFixture& location(kotor::Vec3 at, float facing)
    {
        constFloat(facing).constFloat(at.x).constFloat(at.y).constFloat(at.z);
        return action(215, 2);
    }
    // SetLocalBoolean(OBJECT_SELF, index, TRUE): a flag the checks read back (0..95, World::localBoolean).
    NcsFixture& setFlag(std::int32_t index)
    {
        constInt(1).constInt(index).constObject(0);
        return action(680, 3);
    }
    // A compiled action argument: STORESTATE, a JMP over the body, the body.
    template <typename Body>
    NcsFixture& closure(Body body)
    {
        op(0x2C, 0x10);
        be32(0);
        be32(0);
        const std::size_t jump = code_.size();
        op(0x1D, 0x00);
        be32(0);
        body(*this);
        returns();
        const std::uint32_t distance = static_cast<std::uint32_t>(code_.size() - jump);
        code_[jump + 2] = static_cast<std::uint8_t>(distance >> 24);
        code_[jump + 3] = static_cast<std::uint8_t>(distance >> 16);
        code_[jump + 4] = static_cast<std::uint8_t>(distance >> 8);
        code_[jump + 5] = static_cast<std::uint8_t>(distance);
        return *this;
    }
    std::vector<std::uint8_t> bytes()
    {
        returns();
        const std::string magic = "NCS V1.0";
        std::vector<std::uint8_t> out(magic.begin(), magic.end());
        out.push_back(0x42);
        const std::uint32_t size = static_cast<std::uint32_t>(13 + code_.size());
        for (const int shift : {24, 16, 8, 0}) {
            out.push_back(static_cast<std::uint8_t>(size >> shift));
        }
        out.insert(out.end(), code_.begin(), code_.end());
        return out;
    }

private:
    void op(std::uint8_t code, std::uint8_t qualifier)
    {
        code_.push_back(code);
        code_.push_back(qualifier);
    }
    void be32(std::uint32_t value)
    {
        for (const int shift : {24, 16, 8, 0}) {
            code_.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }
    std::vector<std::uint8_t> code_;
};

// --family-test <name>: one Wave 2 family gate per name. Each family adds one line.
int runFamilyTest(const std::string& name, GameSession& game)
{
    if (name == "combat") {
        return runCombatFamilyTest(game);
    }
    if (name == "minigame3d") {
        return runMiniGame3DFamilyTest(game);
    }
    if (name == "worldstate") {
        return runWorldStateFamilyTest(game);
    }
    if (name == "items") {
        return runItemsFamilyTest(game);
    }
    if (name == "pathfinding") {
        return runPathfindingFamilyTest(game);
    }
    if (name == "party") {
        return runPartyFamilyTest(game);
    }
    if (name == "objects") {
        return runObjectsFamilyTest(game);
    }
    if (name == "companion") {
        return runCompanionFamilyTest(game);
    }
    if (name == "endgame") {
        return runEndgameFamilyTest(game);
    }
    if (name == "actions") {
        return runRemainingActionsFamilyTest(game);
    }
    if (name == "cinema") {
        return runCinemaFamilyTest(game);
    }
    if (name == "audioquality") {
        return runAudioQualityFamilyTest(game);
    }
    if (name == "sfx") {
        return runEffectsFamilyTest(game);
    }
    if (name == "ambience") {
        return runAmbienceFamilyTest(game);
    }
    if (name == "animation") {
        return runAnimationFamilyTest(game);
    }
    if (name == "world3d") {
        return runWorldFamilyTest(game);
    }
    if (name == "worldposes") {
        return runWorldPoseSurvey(game);
    }
    if (name == "worldtextures") {
        return runWorldTextureAudit(game);
    }
    if (name == "textureaudit") {
        return runTextureAudit(game);
    }
    if (name == "materials") {
        return runMaterialFamilyTest(game);
    }
    logError(concat("unknown family test: ", name));
    return 2;
}

int runWorldSimTest(GameSession& game)
{
    bool ok = true;
    const auto check = [&](bool condition, const char* label) {
        logInfo(concat("world sim: ", label, "=", condition ? 1 : 0));
        ok = ok && condition;
    };
    const auto tick = [&](float seconds) {
        for (float t = 0.0f; t < seconds; t += 1.0f / 30.0f) {
            game.update(1.0f / 30.0f, InputState{});
        }
    };
    // Flags are SetLocalBoolean bits (0..95); numbers are SetLocalNumber bytes (0..7, -128..127),
    // kept apart as retail's CSWVarTable keeps them (World::localBoolean).
    const auto local = [&](std::uint32_t object, int index) {
        return game.scriptWorld().localBoolean(object, index) != 0 ? 1 : 0;
    };
    const auto number = [&](std::uint32_t object, int index) {
        return game.scriptWorld().localNumber(object, index);
    };
    const auto logged = [&](const char* text) { return scriptLogHas(game, text); };
    const auto flat = [](kotor::Vec3 a, kotor::Vec3 b) {
        return std::hypot(a.x - b.x, a.y - b.y);
    };
    const std::uint32_t pc = game.scriptWorld().pc;
    const std::string startModule = game.module();
    script::Vm dummy({});

    // ---- EngineStore: the proving family, called the way the VM calls it.
    {
        std::vector<std::vector<script::Value>> args = {
            {script::Value::makeFloat(3.0), script::Value::makeFloat(4.0),
             script::Value::makeFloat(5.0)},
            {script::Value::makeFloat(90.0)}};
        const auto created = game.action(215, args, game.scriptWorld(), dummy);
        check(created.has_value() && created->size() == 1
                  && created->front().type == script::ValueType::Engine
                  && created->front().integer != 0,
              "location_is_engine_value");
        const std::vector<std::vector<script::Value>> read = {{created->front()}};
        const auto position = game.action(223, read, game.scriptWorld(), dummy);
        check(position.has_value() && position->size() == 3 && (*position)[0].asFloat() == 3.0
                  && (*position)[2].asFloat() == 5.0,
              "location_position_read_back");
        const auto facing = game.action(225, read, game.scriptWorld(), dummy);
        check(facing.has_value() && facing->front().asFloat() == 90.0, "location_facing_read_back");
        const std::vector<std::vector<script::Value>> invalid = {{script::Value::makeEngine(0)}};
        const auto none = game.action(223, invalid, game.scriptWorld(), dummy);
        check(none.has_value() && (*none)[0].asFloat() == 0.0, "invalid_location_reads_zero");
    }

    // ---- Parser: gameplay flags on shipped templates.
    {
        std::size_t keyDoors = 0;
        std::size_t lockDc = 0;
        std::size_t plot = 0;
        for (const kotor::Entity& entity : game.scene().entities) {
            keyDoors += entity.kind == kotor::EntityKind::Door && entity.keyRequired ? 1u : 0u;
            lockDc += entity.openLockDC > 0 ? 1u : 0u;
            plot += entity.plot ? 1u : 0u;
        }
        logInfo(concat("world sim: key_doors=", keyDoors, " lock_dc=", lockDc, " plot=", plot));
        check(keyDoors > 0, "utd_key_required_parsed");
        check(lockDc > 0, "open_lock_dc_parsed");
        check(plot > 0, "plot_flag_parsed");
    }

    // ---- Dialogue: a shipped starting link reads a global. k_pend_trask00 passes
    // while End_TraskTalk is 0; setting it must select a different branch.
    const std::size_t trask = game.findEntity("end_trask");
    check(trask != static_cast<std::size_t>(-1), "fixture_trask");
    if (trask == static_cast<std::size_t>(-1)) {
        return 40;
    }
    const auto startEntry = [&]() {
        game.clearTraces();
        const std::size_t before = game.scriptLog().size();
        game.startDialogue(trask);
        std::string entry;
        for (std::size_t i = before; i < game.scriptLog().size(); ++i) {
            if (game.scriptLog()[i].rfind("dialogue start end_trask01", 0) == 0) {
                entry = game.scriptLog()[i];
            }
        }
        game.clearDialogueForTest();
        return entry;
    };
    game.scriptWorld().setGlobal("End_TraskTalk", script::Value::makeInt(0));
    const std::string branchBefore = startEntry();
    game.scriptWorld().setGlobal("End_TraskTalk", script::Value::makeInt(1));
    const std::string branchAfter = startEntry();
    logInfo(concat("world sim: ", branchBefore, " then ", branchAfter));
    check(!branchBefore.empty() && !branchAfter.empty() && branchBefore != branchAfter,
          "start_link_follows_global");
    game.scriptWorld().setGlobal("End_TraskTalk", script::Value::makeInt(0));

    // ---- Dialogue state machine, with fixture conditions on a built DLG.
    game.declareGlobalForTest("WSIM");
    game.declareGlobalForTest("WSIM_LEFT");
    game.declareGlobalForTest("WSIM_EXIT");
    game.addScriptForTest("wsim_cond", NcsFixture().constString("WSIM").action(580, 1).bytes());
    game.addScriptForTest("wsim_entry", NcsFixture().setFlag(21).bytes());
    game.addScriptForTest("wsim_end", NcsFixture().setFlag(22).bytes());
    game.addScriptForTest("wsim_abort", NcsFixture().setFlag(23).bytes());
    {
        kotor::Dlg dlg;
        kotor::DlgEntry first;
        first.text = "first";
        first.script = "wsim_entry";
        first.replies = {0, 1};
        first.repliesActive = {"wsim_cond", ""};
        kotor::DlgEntry gated;
        gated.text = "gated";
        kotor::DlgEntry open;
        open.text = "open";
        dlg.entries = {first, gated, open};
        kotor::DlgReply both;
        both.text = "both";
        both.entries = {1, 2};
        both.entriesActive = {"wsim_cond", ""};
        kotor::DlgReply leave;
        leave.text = "leave";
        dlg.replies = {both, leave};
        dlg.endConversation = "wsim_end";
        dlg.endConversationAbort = "wsim_abort";

        game.scriptWorld().setGlobal("WSIM", script::Value::makeInt(0));
        game.setDialogueForTest(dlg, "wsim", "fixture");
        check(game.dialogue().options.size() == 1 && game.dialogue().options[0].text == "leave",
              "reply_link_filtered_when_false");
        check(local(pc, 21) == 1, "entry_script_ran");
        game.chooseDialogueOption(0);
        check(!game.dialogue().active && local(pc, 22) == 1 && local(pc, 23) == 0,
              "reply_without_links_ends_normally");

        game.scriptWorld().setGlobal("WSIM", script::Value::makeInt(1));
        game.scriptWorld().setLocalBoolean(pc, 21, false);
        game.setDialogueForTest(dlg, "wsim", "fixture");
        check(game.dialogue().options.size() == 2, "reply_link_shown_when_true");
        check(local(pc, 21) == 1, "entry_script_runs_each_time_reached");
        game.chooseDialogueOption(0);
        check(game.dialogue().active && game.dialogue().text == "gated",
              "first_truthy_entry_link_wins");
        game.endDialogue();
        check(!game.dialogue().active && local(pc, 23) == 1, "abort_runs_end_converabort");

        game.scriptWorld().setGlobal("WSIM", script::Value::makeInt(1));
        game.setDialogueForTest(dlg, "wsim", "fixture");
        game.scriptWorld().setGlobal("WSIM", script::Value::makeInt(0));
        game.chooseDialogueOption(0);
        check(game.dialogue().active && game.dialogue().text == "open",
              "false_entry_link_skipped");
        game.clearDialogueForTest();
    }

    // ---- Journal and plot XP from a dialogue node, on the shipped global.jrl and
    // plot.2da. The category and row are ones shipped end_m01aa nodes use.
    {
        constexpr const char* kQuest = "end_attack";
        check(!game.journalEntryText(kQuest, 10).empty()
                  || !game.journalEntryText(kQuest, 1).empty(),
              "global_jrl_loaded");
        const std::int32_t plotAward = game.plotXpForIndex(47, 0.1f);
        logInfo(concat("world sim: plot row 47 at 0.1 = ", plotAward));
        check(plotAward > 0, "plot_2da_loaded");
        kotor::Dlg dlg;
        kotor::DlgEntry node;
        node.text = "node";
        node.quest = kQuest;
        node.hasQuestEntry = true;
        node.questEntry = 20;
        node.plotIndex = 47;
        node.plotXpPercentage = 0.1f;
        dlg.entries = {node};
        const std::int32_t xpBefore = game.character().xp;
        check(game.journalState(kQuest) == 0, "journal_starts_empty");
        game.setDialogueForTest(dlg, "wsim_q", "fixture");
        check(game.journalState(kQuest) == 20, "dialogue_node_sets_journal");
        check(game.character().xp >= xpBefore + plotAward, "dialogue_node_awards_xp");
        game.clearDialogueForTest();
        check(!game.addJournalQuest(kQuest, 10, false) && game.journalState(kQuest) == 20,
              "journal_cannot_go_back_without_override");
        check(game.addJournalQuest(kQuest, 10, true) && game.journalState(kQuest) == 10,
              "journal_override_goes_back");
        const std::int32_t xpGiven = game.character().xp;
        game.givePlotXp("no_such_plot_row", 100);
        check(game.character().xp == xpGiven, "unknown_plot_label_awards_nothing");
    }

    // ---- Action queues: walking blocks what is queued behind it.
    const std::uint32_t npc = game.objectForEntityIndex(trask);
    {
        // The conversations above ran shipped node scripts that queue on Trask, and
        // the module's opening has a timer that starts him talking a few seconds
        // in. Give the assertions below a known empty queue and a quiet clock.
        game.scriptWorld().timers.clear();
        const auto reset = NcsFixture()
                               .constObject(0)
                               .constInt(1)
                               .action(162, 2) // SetCommandable(TRUE, OBJECT_SELF)
                               .action(9, 0)   // ClearAllActions
                               .bytes();
        game.runScriptNcsForTest("wsim_reset", reset, npc);
        check(game.actorQueueSize(npc) == 0, "clear_all_actions_empties_shipped_queue");
        kotor::Vec3 target;
        check(game.findClearView(game.scene().entities[trask].position, 2.5f, target),
              "fixture_walk_target");
        const kotor::Vec3 start = game.actorPosition(npc);
        const auto bytes = NcsFixture()
                               .constInt(0)
                               .location(target, 0.0f)
                               .action(21, 2) // ActionMoveToLocation
                               .closure([](NcsFixture& body) { body.setFlag(7); })
                               .action(294, 1) // ActionDoCommand
                               .constInt(99)
                               .constInt(0)
                               .constObject(0)
                               .action(682, 3) // SetLocalNumber 0: runs now, before anything queued.
                               .bytes();
        check(game.runScriptNcsForTest("wsim_queue", bytes, npc), "queue_script_ran");
        check(game.actorQueueSize(npc) == 2, "move_and_docommand_queued");
        check(local(npc, 7) == 0 && number(npc, 0) == 99, "script_continues_past_queued_actions");
        game.update(1.0f / 30.0f, InputState{});
        check(local(npc, 7) == 0, "docommand_waits_for_move");
        float travelled = 0.0f;
        float atCommand = -1.0f;
        for (int step = 0; step < 900 && atCommand < 0.0f; ++step) {
            game.update(1.0f / 30.0f, InputState{});
            travelled = std::max(travelled, flat(game.actorPosition(npc), start));
            if (local(npc, 7) == 1) {
                atCommand = flat(game.actorPosition(npc), target);
            }
        }
        logInfo(concat("world sim: travelled=", travelled, " at_command=", atCommand));
        check(travelled > 0.5f, "actor_walked");
        check(atCommand >= 0.0f && atCommand < 0.5f, "docommand_ran_after_arrival");
        check(game.actorQueueSize(npc) == 0, "queue_drained");

        // ActionWait blocks the queue, not the script.
        const auto wait = NcsFixture()
                              .constFloat(1.0f)
                              .action(202, 1)
                              .closure([](NcsFixture& body) { body.setFlag(9); })
                              .action(294, 1)
                              .bytes();
        game.runScriptNcsForTest("wsim_wait", wait, npc);
        tick(0.5f);
        check(local(npc, 9) == 0, "wait_blocks_queue");
        tick(0.8f);
        check(local(npc, 9) == 1, "queue_resumes_after_wait");

        // Not commandable: retail drops the queued action (0057cb10).
        const auto locked = NcsFixture()
                                .constObject(0)
                                .constInt(0)
                                .action(162, 2)
                                .closure([](NcsFixture& body) { body.setFlag(10); })
                                .action(294, 1)
                                .constObject(0)
                                .constInt(1)
                                .action(162, 2)
                                .bytes();
        game.runScriptNcsForTest("wsim_locked", locked, npc);
        tick(0.2f);
        check(local(npc, 10) == 0 && game.actorQueueSize(npc) == 0, "not_commandable_drops");

        // ClearAllActions empties a commandable queue, closures included.
        const auto cleared = NcsFixture()
                                 .constFloat(5.0f)
                                 .action(202, 1)
                                 .closure([](NcsFixture& body) { body.setFlag(11); })
                                 .action(294, 1)
                                 .action(9, 0)
                                 .bytes();
        game.runScriptNcsForTest("wsim_cleared", cleared, npc);
        check(game.actorQueueSize(npc) == 0, "clear_all_actions");

        // AssignCommand runs later, on the target, with OBJECT_SELF = target. An
        // engine value captured by the closure arrives intact: the actor jumps to
        // the location the outer script built.
        const kotor::Vec3 home = start;
        const auto assign = NcsFixture()
                                .location(home, 180.0f)
                                .closure([](NcsFixture& body) {
                                    body.setFlag(12).copyTop().action(214, 1);
                                })
                                .constObject(npc)
                                .action(6, 2)
                                .bytes();
        game.runScriptNcsForTest("wsim_assign", assign, pc);
        check(local(npc, 12) == 0 && local(pc, 12) == 0, "assign_is_deferred");
        tick(0.2f);
        check(local(npc, 12) == 1 && local(pc, 12) == 0, "assign_runs_as_target");
        check(flat(game.actorPosition(npc), home) < 0.2f, "engine_value_survives_closure");
        check(flat(game.actorPosition(npc), target) > 0.5f, "jump_moved_actor");
    }

    // ---- Events and ExecuteScript.
    {
        kotor::Entity& entity = game.scene().entities[trask];
        entity.scripts["ScriptUserDefine"] = "wsim_ud";
        game.addScriptForTest("wsim_ud", NcsFixture()
                                             .action(247, 0) // GetUserDefinedEventNumber
                                             .constInt(1)
                                             .constObject(0)
                                             .action(682, 3)
                                             .bytes());
        const auto signal = NcsFixture()
                                .constInt(101) // a number local holds -128..127
                                .action(132, 1) // EventUserDefined
                                .constObject(npc)
                                .action(131, 2) // SignalEvent
                                .bytes();
        game.runScriptNcsForTest("wsim_signal", signal, pc);
        check(number(npc, 1) == 0, "signal_is_deferred");
        tick(0.1f);
        check(number(npc, 1) == 101, "signal_reaches_user_defined_as_target");
        check(logged("hook ScriptUserDefine end_trask -> wsim_ud"), "user_defined_hook_logged");

        game.scene().scripts["Mod_OnUsrDefined"] = "wsim_mod";
        game.addScriptForTest("wsim_mod", NcsFixture().setFlag(14).bytes());
        const auto signalModule = NcsFixture()
                                      .constInt(1)
                                      .action(132, 1)
                                      .action(242, 0) // GetModule
                                      .action(131, 2)
                                      .bytes();
        game.runScriptNcsForTest("wsim_signal_mod", signalModule, pc);
        tick(0.1f);
        check(local(game.moduleObject(), 14) == 1, "module_user_defined_runs_on_module");

        // ExecuteScript runs now, on the target, carries its var, and returning
        // leaves the outer script's OBJECT_SELF alone.
        game.addScriptForTest("wsim_inner", NcsFixture()
                                                .action(565, 0) // GetRunScriptVar
                                                .constInt(2)
                                                .constObject(0)
                                                .action(682, 3)
                                                .bytes());
        const auto outer = NcsFixture()
                               .constInt(77)
                               .action(548, 0) // GetFirstPC: 1 as a constant is OBJECT_INVALID.
                               .constString("wsim_inner")
                               .action(8, 3)
                               .setFlag(16)
                               .bytes();
        game.runScriptNcsForTest("wsim_outer", outer, npc);
        check(number(pc, 2) == 77 && number(npc, 2) == 0, "execute_script_on_target_with_var");
        check(local(npc, 16) == 1 && local(pc, 16) == 0, "nested_run_keeps_outer_self");
        check(game.runScriptVar() == 77, "run_script_var_is_not_restored");
    }

    // ---- Local variables as retail's CSWVarTable keeps them (vm_answers Q8): a boolean and a number of
    // the same index are apart; SetLocalNumber drops a value outside -128..127 and reads -1 back as 255;
    // GetLocalBoolean gives the raw bit (index 5 reads 32). Fails on the old single-key store.
    {
        const auto vars = NcsFixture()
                              .constInt(-1).constInt(5).constObject(0).action(682, 3)  // SetLocalNumber(self, 5, -1)
                              .constInt(1).constInt(5).constObject(0).action(680, 3)   // SetLocalBoolean(self, 5, TRUE)
                              .constInt(200).constInt(6).constObject(0).action(682, 3) // SetLocalNumber(self, 6, 200)
                              .constInt(5).constObject(0).action(679, 2)               // GetLocalBoolean(self, 5)
                              .constInt(7).constObject(0).action(682, 3)               // SetLocalNumber(self, 7, that)
                              .bytes();
        const std::int32_t number6Before = number(npc, 6);
        check(game.runScriptNcsForTest("wsim_vars", vars, npc), "local_vars_script_ran");
        logInfo(concat("world sim: number5=", number(npc, 5), " flag5=", game.scriptWorld().localBoolean(npc, 5),
                       " number6=", number(npc, 6), " number7=", number(npc, 7)));
        check(number(npc, 5) == 255 && local(npc, 5) == 1, "proof_local_boolean_and_number_are_apart");
        check(number(npc, 6) == number6Before, "proof_local_number_drops_values_past_a_byte");
        check(number(npc, 7) == 32, "proof_local_boolean_reads_the_raw_bit");
    }

    // ---- Kill, leave, return.
    const std::size_t hostile = firstHostile(game);
    check(hostile != static_cast<std::size_t>(-1), "fixture_hostile");
    if (hostile == static_cast<std::size_t>(-1)) {
        return 41;
    }
    const std::uint32_t hostileObject = game.objectForEntityIndex(hostile);
    const std::string hostileTemplate = game.scene().entities[hostile].templateResref;
    const std::uint32_t areaBefore = game.areaObject();
    const std::uint32_t trigger = static_cast<std::uint32_t>(game.scene().triggers.size());
    {
        game.scene().entities[hostile].scripts["ScriptDeath"] = "wsim_death";
        game.addScriptForTest("wsim_death", NcsFixture().setFlag(17).bytes());
        const std::int32_t xpBefore = game.character().xp;
        game.damageEntity(hostile, 100000, pc);
        check(game.scene().entities[hostile].dead, "kill_marks_dead");
        const std::int32_t xpAfter = game.character().xp;
        check(xpAfter > xpBefore, "kill_awards_xp");
        game.onCreatureKilled(hostile, pc);
        check(game.character().xp == xpAfter, "second_kill_is_ignored");
        game.flushScriptEvents();
        check(local(hostileObject, 17) == 1, "death_hook_ran_once_as_victim");
    }
    // Object locals, a pending timer, looted items and a created creature all have
    // to come back. The timer is longer than the time spent away.
    game.scriptWorld().setLocalNumber(npc, 3, 42);
    const auto delayed = NcsFixture()
                             .closure([](NcsFixture& body) { body.setFlag(19); })
                             .constFloat(3.0f)
                             .action(7, 2) // DelayCommand
                             .bytes();
    game.runScriptNcsForTest("wsim_delay", delayed, npc);
    const std::size_t entitiesBefore = game.scene().entities.size();
    const auto create = NcsFixture()
                            .constInt(0)
                            .location(game.scene().entities[trask].position, 0.0f)
                            .constString(hostileTemplate)
                            .constInt(1)
                            .action(243, 4) // CreateObject(OBJECT_TYPE_CREATURE, ...)
                            .bytes();
    game.runScriptNcsForTest("wsim_create", create, pc);
    check(game.scene().entities.size() == entitiesBefore + 1, "create_object_spawned");
    const std::uint32_t createdObject = game.objectForEntityIndex(entitiesBefore);
    game.scriptWorld().setLocalNumber(createdObject, 4, 5);

    const std::size_t locker = game.findEntity("end_locker01");
    std::size_t lootBefore = 0;
    if (locker != static_cast<std::size_t>(-1)) {
        game.scene().entities[locker].scripts["OnInvDisturbed"] = "wsim_disturb";
        game.addScriptForTest("wsim_disturb", NcsFixture()
                                                  .action(352, 0) // GetInventoryDisturbType
                                                  .constInt(5)
                                                  .constObject(0)
                                                  .action(682, 3)
                                                  .bytes());
        lootBefore = game.scene().entities[locker].lootItems.size();
        if (lootBefore > 0 && game.openLoot(locker)) {
            game.takeLootItem(0);
            game.flushScriptEvents();
            check(number(game.objectForEntityIndex(locker), 5) == 1, "loot_fires_disturbed");
        }
    }

    game.scene().scripts["Mod_OnClientLeav"] = "wsim_leave";
    game.scene().scripts["OnExit"] = "wsim_exit";
    game.addScriptForTest("wsim_leave", NcsFixture()
                                            .constInt(1)
                                            .constString("WSIM_LEFT")
                                            .action(581, 2)
                                            .bytes());
    game.addScriptForTest("wsim_exit", NcsFixture()
                                           .constInt(1)
                                           .constString("WSIM_EXIT")
                                           .action(581, 2)
                                           .bytes());
    std::string from;
    std::string nextModule;
    check(game.testTransition(from, nextModule) && nextModule != startModule, "left_module");
    logInfo(concat("world sim: ", from, " -> ", nextModule));
    const auto globalValue = [&](const char* name) {
        return game.scriptWorld().getGlobal(name, script::ValueType::Int).asInt();
    };
    check(globalValue("WSIM_LEFT") == 1 && globalValue("WSIM_EXIT") == 1,
          "leave_hooks_ran_before_the_swap");
    check(game.moduleSnapshot(startModule) != nullptr, "snapshot_captured");
    check(local(pc, 21) == 1, "pc_locals_travel_with_the_player");
    tick(0.5f);

    // ---- Arrive at the StartNewModule waypoint.
    std::string waypoint;
    kotor::Vec3 waypointAt;
    for (const kotor::Entity& entity : game.scene().entities) {
        if (entity.kind == kotor::EntityKind::Waypoint && !entity.tag.empty()) {
            waypoint = entity.tag;
            waypointAt = entity.position;
            break;
        }
    }
    check(!waypoint.empty(), "fixture_waypoint");
    const auto travel = [&](const std::string& module, const std::string& wp) {
        NcsFixture script;
        for (int i = 0; i < 6; ++i) {
            script.constString("");
        }
        script.constString(wp).constString(module).action(509, 8);
        game.runScriptNcsForTest("wsim_travel", script.bytes(), pc);
        game.update(1.0f / 30.0f, InputState{});
    };

    travel(startModule, "");
    check(game.module() == startModule, "returned_to_module");
    check(game.scene().entities[hostile].dead, "dead_creature_stays_dead");
    check(game.objectForEntityIndex(hostile) == hostileObject, "object_id_stable_across_return");
    check(game.areaObject() == areaBefore, "area_object_stable_across_return");
    check(game.objectForEntityIndex(trask) == npc, "npc_id_stable_across_return");
    check(number(npc, 3) == 42, "object_local_survives_return");
    check(flat(game.actorPosition(npc), game.scene().entities[trask].position) < 0.01f,
          "actor_position_is_entity_position");
    check(local(hostileObject, 17) == 1, "dead_creature_local_survives");
    check(game.scene().triggers.size() == trigger, "trigger_count_stable");
    bool createdBack = false;
    for (std::size_t i = 0; i < game.scene().entities.size(); ++i) {
        if (game.objectForEntityIndex(i) == createdObject) {
            createdBack = true;
            check(number(createdObject, 4) == 5, "created_object_local_survives");
        }
    }
    check(createdBack, "created_object_survives_return");
    if (locker != static_cast<std::size_t>(-1) && lootBefore > 0) {
        check(game.scene().entities[locker].lootItems.size() == lootBefore - 1,
              "looted_item_stays_taken");
    }
    check(local(npc, 19) == 0, "timer_did_not_fire_while_away");
    tick(3.5f);
    check(local(npc, 19) == 1, "pending_timer_fires_after_return");

    travel(nextModule, waypoint);
    check(game.module() == nextModule, "start_new_module_loaded");
    logInfo(concat("world sim: arrived ", game.position().x, ",", game.position().y,
                   " waypoint ", waypoint, " ", waypointAt.x, ",", waypointAt.y));
    check(flat(game.position(), waypointAt) < 0.5f, "start_new_module_uses_waypoint");
    travel(startModule, "");

    // ---- Through a save file. World state round-trips, including a timer.
    {
        const auto timer = NcsFixture()
                               .closure([](NcsFixture& body) { body.setFlag(26); })
                               .constFloat(2.0f)
                               .action(7, 2)
                               .bytes();
        game.addScriptForTest("wsim_save_timer", timer);
        game.runScript("wsim_save_timer", npc);
        check(!game.scriptWorld().timers.empty(), "timer_pending_at_save");
        const double worldTime = game.scriptWorld().time;
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "kotor_engine_world_sim.save";
        check(game.save(path), "saved");
        // Change the world so a load has something to undo.
        game.scriptWorld().setLocalNumber(npc, 3, 1);
        game.scriptWorld().setLocalBoolean(pc, 21, false);
        check(game.load(path), "loaded");
        check(game.module() == startModule, "save_module");
        check(game.scene().entities[hostile].dead, "save_keeps_dead");
        check(game.objectForEntityIndex(hostile) == hostileObject, "save_keeps_object_id");
        check(number(npc, 3) == 42, "save_keeps_object_local");
        check(local(pc, 21) == 1, "save_keeps_pc_local");
        check(std::fabs(game.scriptWorld().time - worldTime) < 0.01, "save_keeps_world_time");
        check(game.moduleSnapshot(nextModule) != nullptr, "save_keeps_other_module");
        check(local(npc, 26) == 0, "saved_timer_has_not_fired");
        tick(2.5f);
        check(local(npc, 26) == 1, "saved_timer_fires_after_load");
        std::filesystem::remove(path);

        // Older saves still load.
        const std::filesystem::path oldPath =
            std::filesystem::temp_directory_path() / "kotor_engine_world_sim_old.save";
        {
            std::ofstream old(oldPath, std::ios::binary | std::ios::trunc);
            old << "schema=kotor.engine.player/3\nmodule=" << startModule << "\nhp=10\n";
        }
        check(game.load(oldPath), "old_schema_loads");
        check(game.moduleSnapshot(nextModule) == nullptr, "old_schema_has_no_snapshots");
        std::filesystem::remove(oldPath);
    }

    if (!ok) {
        const std::vector<std::string>& lines = game.scriptLog();
        for (std::size_t i = lines.size() > 80 ? lines.size() - 80 : 0; i < lines.size(); ++i) {
            logInfo(concat("world sim: log ", lines[i]));
        }
    }
    logInfo(ok ? "world sim: PASS" : "world sim: FAIL");
    return ok ? 0 : 42;
}

int runDelayedTest(GameSession& game, const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        logError(concat("delayed test: cannot open ", path));
        return 30;
    }
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        logError("delayed test: short read");
        return 31;
    }
    const std::uint32_t self = game.scriptWorld().pc;
    bool ok = game.runScriptNcsForTest("delay_test", bytes, self);
    const auto local = [&](const char* name) {
        return game.scriptWorld().getLocal(self, name, script::ValueType::Int).asInt();
    };
    // ACT_202 queues and returns (00545800): the statements after ActionWait run
    // immediately, and only the action queue waits.
    ok = ok && local("0") == 1 && local("1") == 0 && local("2") == 0
        && local("5") == 1 && local("6") == 2;
    logInfo(concat("delayed test: immediate local0=", local("0"),
                   " local1=", local("1"), " local2=", local("2"),
                   " local5=", local("5"), " local6=", local("6")));
    InputState idle;
    for (int i = 0; i < 12; ++i) {
        game.update(0.08f, idle);
    }
    const std::uint32_t carth = game.objectForTag("Carth");
    const auto carthLocal = [&](const char* name) {
        return carth == script::kObjectInvalid
            ? -1
            : static_cast<int>(game.scriptWorld().getLocal(
                  carth, name, script::ValueType::Int).asInt());
    };
    ok = ok && local("1") == 0 && local("2") == 7 && carthLocal("3") == 9
        && local("4") == 0;
    logInfo(concat("delayed test: after_1s local1=", local("1"),
                   " local2=", local("2"), " carth3=", carthLocal("3"),
                   " local4=", local("4")));
    for (int i = 0; i < 20; ++i) {
        game.update(0.08f, idle);
    }
    ok = ok && local("1") == 42 && local("4") == 99 && local("6") == 2;
    logInfo(concat("delayed test: after_2.5s local1=", local("1"),
                   " local4=", local("4"), " local6=", local("6"),
                   " timers=", game.scriptWorld().timers.size(),
                   " queued=", game.scriptWorld().actionQueue.size()));

    // Real shipped script: k_pend_time01 schedules a 10 s user-defined event.
    game.runScript("k_pend_time01", self);
    logInfo(concat("delayed test: shipped timers_after_script=",
                   game.scriptWorld().timers.size(),
                   " functors_after_script=", game.scriptWorld().functors.size()));
    for (int i = 0; i < 140; ++i) {
        game.update(0.08f, idle);
    }
    bool shippedFunctor = false;
    bool shippedRan = false;
    for (const std::string& line : game.scriptLog()) {
        if (line.find("k_pend_time01") != std::string::npos) {
            shippedRan = true;
            logInfo(concat("delayed test: shipped log ", line));
        }
        if (line.find("functor ") != std::string::npos) {
            logInfo(concat("delayed test: functor log ", line));
        }
        if (line.find("functor DelayCommand status=returned") != std::string::npos) {
            shippedFunctor = true;
        }
    }
    ok = ok && shippedRan && shippedFunctor;
    logInfo(concat("delayed test: shipped_k_pend_time01 ran=", shippedRan ? 1 : 0,
                   " functor=", shippedFunctor ? 1 : 0,
                   " -> ", ok ? "PASS" : "FAIL"));
    return ok ? 0 : 32;
}

int runOriginalSaveCreate(const Options& options)
{
    const std::string installPath = resolveInstallPath(options);
    const std::string module = options.module.empty() ? "end_m01aa" : options.module;
    const std::filesystem::path out = options.originalSaveCreate;
    try {
        GameSession game(installPath, module);
        game.character().name = "S9 Create";
        game.character().inventory.clear();
        game.addInventoryItem("g_w_vbroshort01", 1, 1, 100);
        game.addJournalEntry("s9_create_plot", 1);
        game.giveXp(1000);
        std::error_code error;
        std::filesystem::remove_all(out, error);
        if (!game.saveOriginalSave(out, "S9 CREATE")) {
            logError("original save create: write FAIL");
            return 20;
        }
        const ke::kotor::OriginalSave written = ke::kotor::readOriginalSave(out);
        bool ok = written.module == module
            && written.inventory.size() == 1
            && written.journal.count("s9_create_plot") == 1
            && written.pcName == "S9 Create";
        GameSession reloaded(installPath, module);
        const bool reloadedOk = reloaded.loadOriginalSave(out);
        ok = ok && reloadedOk
            && reloaded.inventory().size() == 1
            && reloaded.journalState("s9_create_plot") == 1;
        const ke::kotor::OriginalSaveCheck check = ke::kotor::checkOriginalSave(out);
        ok = ok && check.ok;
        logInfo(concat("original save create: path=", out.string(),
                       " module=", written.module,
                       " inventory=", written.inventory.size(),
                       " journal=", written.journal.size(),
                       " reload=", reloadedOk ? 1 : 0,
                       " -> ", ok ? "PASS" : "FAIL"));
        for (const std::string& message : check.errors) {
            logInfo(concat("  original save create check: ", message));
        }
        return ok ? 0 : 21;
    } catch (const std::exception& exception) {
        logError(concat("original save create failed: ", exception.what()));
        return 22;
    }
}

int runOriginalSaveGate(const Options& options)
{
    const std::string sourceFolder = !options.originalSaveLoad.empty()
        ? options.originalSaveLoad
        : options.originalSaveRoundtrip;
    const std::string installPath = resolveInstallPath(options);
    try {
        const ke::kotor::OriginalSave source = ke::kotor::readOriginalSave(sourceFolder);
        logInfo(concat("original save source: module=", source.module, " area=", source.area,
                       " pc=", source.pcName, " gold=", source.gold,
                       " inventory=", source.inventory.size(),
                       " journal=", source.journal.size(),
                       " globals=", source.globals.size(),
                       " objects=", source.objects.size()));
        GameSession game(installPath, source.module);
        if (!game.loadOriginalSave(sourceFolder)) {
            logError("original save load: FAIL");
            return 10;
        }
        std::size_t runtimeGlobals = 0;
        std::size_t locationGlobals = 0;
        for (const ke::kotor::SavedGlobal& value : source.globals) {
            if (value.type == ke::kotor::SavedGlobal::Type::Location) {
                ++locationGlobals;
            } else {
                ++runtimeGlobals;
            }
        }
        bool ok = game.module() == source.module;
        ok = ok && game.gold() == source.gold;
        ok = ok && game.inventory().size() == source.inventory.size();
        ok = ok && game.journal().size() == source.journal.size();
        ok = ok && game.scriptWorld().globals.size() == runtimeGlobals;
        ok = ok && game.companions().size() == source.partyTags.size();
        const float dx = game.position().x - source.position.x;
        const float dy = game.position().y - source.position.y;
        const float dz = game.position().z - source.position.z;
        ok = ok && (dx * dx + dy * dy + dz * dz) < 1.0f;
        logInfo(concat("original save load: module=", game.module(),
                       " gold=", game.gold(),
                       " inventory=", game.inventory().size(),
                       " journal=", game.journal().size(),
                       " globals=", game.scriptWorld().globals.size(),
                       " locations=", locationGlobals,
                       " companions=", game.companions().size(),
                       " position=", game.position().x, ",", game.position().y, ",",
                       game.position().z, " -> ", ok ? "PASS" : "FAIL"));
        if (!ok) {
            return 11;
        }
        if (options.originalSaveRoundtrip.empty()) {
            return 0;
        }

        // Values the writer must carry exactly: a negative global number (retail keeps a signed
        // byte, 00529680 / 00529240), an item's NewItem bit (0055ccd0 / 0055fcd0) and the area's
        // explored map words (GIT "AreaMap", 005061d0 / 00505da0).
        std::string negativeGlobal;
        for (const ke::kotor::SavedGlobal& value : source.globals) {
            if (value.type == ke::kotor::SavedGlobal::Type::Number) {
                negativeGlobal = value.name;
                break;
            }
        }
        if (!negativeGlobal.empty()) {
            game.scriptWorld().globals[negativeGlobal] = script::Value::makeInt(-5);
        }
        std::size_t newItemIndex = game.inventory().size();
        for (std::size_t i = 0; i < game.inventory().size(); ++i) {
            if (game.inventory()[i].object != 0 && !game.inventory()[i].equipped) {
                game.clearNewItems();
                game.markNewItem(game.inventory()[i].object);
                newItemIndex = i;
                break;
            }
        }

        std::filesystem::path out = options.originalSaveOut;
        if (out.empty()) {
            out = std::filesystem::temp_directory_path() / "kotor-engine-s9-save";
        }
        std::error_code error;
        std::filesystem::remove_all(out, error);
        if (!game.saveOriginalSave(out, source.saveName)) {
            logError("original save write: FAIL");
            return 12;
        }
        const ke::kotor::OriginalSaveCheck check = ke::kotor::checkOriginalSave(out);
        logInfo(concat("original save write: path=", out.string(),
                       " errors=", check.errors.size(), " warnings=", check.warnings.size(),
                       " -> ", check.ok ? "PASS" : "FAIL"));
        for (const std::string& message : check.errors) {
            logInfo(concat("  original save check: ", message));
        }
        if (!check.ok) {
            return 13;
        }
        const ke::kotor::OriginalSave written = ke::kotor::readOriginalSave(out);
        bool roundtrip = written.module == source.module
            && written.area == source.area
            && written.areaName == source.areaName
            && written.timePlayed >= source.timePlayed
            && written.gold == source.gold
            && written.inventory.size() == source.inventory.size()
            && written.journal.size() == source.journal.size()
            && written.globals.size() == source.globals.size()
            && written.partyTags.size() == source.partyTags.size()
            && written.pcName == source.pcName;
        const float wx = written.position.x - source.position.x;
        const float wy = written.position.y - source.position.y;
        const float wz = written.position.z - source.position.z;
        roundtrip = roundtrip && (wx * wx + wy * wy + wz * wz) < 1.0f;

        GameSession reloaded(installPath, source.module);
        const bool reloadedOk = reloaded.loadOriginalSave(out);
        roundtrip = roundtrip && reloadedOk
            && reloaded.gold() == source.gold
            && reloaded.inventory().size() == source.inventory.size()
            && reloaded.journal().size() == source.journal.size()
            && reloaded.companions().size() == source.partyTags.size();
        bool negativeKept = negativeGlobal.empty();
        for (const ke::kotor::SavedGlobal& value : written.globals) {
            if (value.type == ke::kotor::SavedGlobal::Type::Number && value.name == negativeGlobal) {
                negativeKept = value.number == -5;
            }
        }
        if (!negativeGlobal.empty()) {
            const auto found = reloaded.scriptWorld().globals.find(negativeGlobal);
            negativeKept = negativeKept && found != reloaded.scriptWorld().globals.end() && found->second.integer == -5;
        }
        std::size_t newItemsWritten = 0;
        for (const ke::kotor::SavedInventoryItem& item : written.inventory) {
            newItemsWritten += item.newItem ? 1u : 0u;
        }
        const bool newItemKept = newItemIndex == game.inventory().size()
            || (newItemsWritten == 1 && newItemIndex < written.inventory.size() && written.inventory[newItemIndex].newItem
                && newItemIndex < reloaded.inventory().size()
                && reloaded.isNewItem(reloaded.inventory()[newItemIndex].object));
        // Every explored bit of the save survives (the session may explore more around the player
        // after the load, as retail does, so the written map can only gain bits).
        bool areaMapKept = !source.areaMap.present;
        if (source.areaMap.present && written.areaMap.present && written.areaMap.resX == source.areaMap.resX
            && written.areaMap.resY == source.areaMap.resY
            && written.areaMap.words.size() == source.areaMap.words.size()) {
            areaMapKept = true;
            for (std::size_t i = 0; i < source.areaMap.words.size(); ++i) {
                areaMapKept = areaMapKept && (written.areaMap.words[i] & source.areaMap.words[i]) == source.areaMap.words[i];
            }
        }
        // GLOBALVARS keeps retail's field sizes (005299b0): booleans count/8 + 1, a fixed 0x960 location blob.
        const bool globalLayoutKept = written.globalBooleanBytes == source.globalBooleanBytes
            && written.globalNumberBytes == source.globalNumberBytes
            && written.globalLocationBytes == source.globalLocationBytes && written.globalLocationBytes == 0x960u;
        // Retail 00555790 writes each item's PropertiesList, ChanceAppear included (100 in every shipped UTI).
        std::size_t sourceProperties = 0;
        std::size_t writtenProperties = 0;
        bool chanceAppearKept = true;
        for (const ke::kotor::SavedInventoryItem& item : source.inventory) {
            sourceProperties += item.properties.size();
        }
        for (const ke::kotor::SavedInventoryItem& item : written.inventory) {
            writtenProperties += item.properties.size();
            for (const ke::kotor::ItemTemplate::Property& property : item.properties) {
                chanceAppearKept = chanceAppearKept && property.chanceAppear == 100;
            }
        }
        const bool propertiesKept = chanceAppearKept && (sourceProperties == 0 || writtenProperties > 0);
        logInfo(concat("original save roundtrip item properties: source=", sourceProperties,
                       " written=", writtenProperties, " chance_appear_100=", chanceAppearKept ? 1 : 0,
                       " kept=", propertiesKept ? 1 : 0));
        // PT_TUT_WND_SHOWN: the tutorial windows the save has shown survive the load and the write
        // (00564eaa / 005661df), so a window retail already showed is not shown again.
        const bool tutorialsKept = written.tutorialShown == source.tutorialShown;
        int tutorialBits = 0;
        for (const std::uint8_t byte : source.tutorialShown) {
            tutorialBits += std::popcount(static_cast<unsigned>(byte));
        }
        logInfo(concat("original save roundtrip tutorials shown: source_bits=", tutorialBits, " kept=", tutorialsKept ? 1 : 0));
        roundtrip = roundtrip && negativeKept && newItemKept && areaMapKept && globalLayoutKept && propertiesKept
            && tutorialsKept;
        logInfo(concat("original save roundtrip globals layout: source=", source.globalBooleanBytes, "/",
                       source.globalNumberBytes, "/", source.globalLocationBytes, " written=", written.globalBooleanBytes,
                       "/", written.globalNumberBytes, "/", written.globalLocationBytes, " kept=",
                       globalLayoutKept ? 1 : 0));
        logInfo(concat("original save roundtrip values: negative_global=", negativeGlobal, " kept=", negativeKept ? 1 : 0,
                       " new_items_written=", newItemsWritten, " new_item_kept=", newItemKept ? 1 : 0,
                       " area_map source=", source.areaMap.present ? 1 : 0, " written=", written.areaMap.present ? 1 : 0,
                       " source_res=", source.areaMap.resX, "x", source.areaMap.resY, " source_words=",
                       source.areaMap.words.size(), " written_res=", written.areaMap.resX, "x", written.areaMap.resY,
                       " words=", written.areaMap.words.size(), " kept=", areaMapKept ? 1 : 0));
        logInfo(concat("original save roundtrip: module=", written.module,
                       " gold=", written.gold,
                       " inventory=", written.inventory.size(),
                       " journal=", written.journal.size(),
                       " globals=", written.globals.size(),
                       " companions=", reloaded.companions().size(),
                       " reload=", reloadedOk ? 1 : 0,
                       " -> ", roundtrip ? "PASS" : "FAIL"));
        return roundtrip ? 0 : 14;
    } catch (const std::exception& exception) {
        logError(concat("original save gate failed: ", exception.what()));
        return 15;
    }
}

// --capture-action: what a gallery shot does to the world on a chosen frame.
void runCaptureActions(GameSession& game, IRenderBackend& backend, const std::string& actions, std::uint64_t frame)
{
    std::size_t start = 0;
    while (start < actions.size()) {
        const std::size_t comma = actions.find(',', start);
        const std::string action = actions.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? actions.size() : comma + 1u;
        const std::size_t at = action.find('@');
        if (at == std::string::npos || std::strtoull(action.c_str() + at + 1u, nullptr, 10) != frame) {
            continue;
        }
        kotor::Scene& scene = game.scene();
        const auto nthFromEntry = [&](kotor::EntityKind kind, std::size_t n) {
            std::vector<std::size_t> found;
            for (std::size_t i = 0; i < scene.entities.size(); ++i) {
                if (scene.entities[i].kind == kind && !scene.entities[i].dead) {
                    found.push_back(i);
                }
            }
            std::stable_sort(found.begin(), found.end(), [&](std::size_t a, std::size_t b) {
                return kotor::length(scene.entities[a].position - scene.entry)
                    < kotor::length(scene.entities[b].position - scene.entry);
            });
            return n < found.size() ? found[n] : static_cast<std::size_t>(-1);
        };
        if (action.rfind("place:", 0) == 0) {
            // place:<tag or pc>:x:y:z:facing, to stage actors where a paired retail frame has them.
            const std::size_t tagEnd = action.find(':', 6u);
            if (tagEnd != std::string::npos && tagEnd < at) {
                std::string numbers = action.substr(tagEnd + 1u, at - tagEnd - 1u);
                char* cursor = numbers.data();
                const float x = std::strtof(cursor, &cursor);
                const float y = std::strtof(cursor + 1, &cursor);
                const float z = std::strtof(cursor + 1, &cursor);
                const float facing = std::strtof(cursor + 1, &cursor);
                const std::string tag = action.substr(6u, tagEnd - 6u);
                const std::size_t named = tag == "pc" ? game.controlledEntity() : game.findEntity(tag);
                if (named < scene.entities.size()) {
                    scene.entities[named].position = {x, y, z};
                    scene.entities[named].bearing = facing;
                    logInfo(concat("capture action: ", action));
                }
            }
        } else if (action.rfind("door:", 0) == 0) {
            const std::size_t door = nthFromEntry(kotor::EntityKind::Door, std::strtoul(action.c_str() + 5, nullptr, 10));
            if (door != static_cast<std::size_t>(-1)) {
                scene.entities[door].locked = false;
                const auto outcome = game.openDoor(door);
                logInfo(concat("capture action: ", action, " ", scene.entities[door].tag, " outcome ",
                               static_cast<int>(outcome), " clip '", game.animationName(door), "'"));
            }
        } else if (action.rfind("equip:", 0) == 0) {
            const std::size_t split = action.find(':', 6u);
            if (split == std::string::npos || split > at) {
                continue;
            }
            const std::size_t creature = nthFromEntry(kotor::EntityKind::Creature, std::strtoul(action.c_str() + 6, nullptr, 10));
            if (creature != static_cast<std::size_t>(-1)) {
                kotor::Entity& entity = scene.entities[creature];
                entity.weaponModel = action.substr(split + 1u, at - split - 1u);
                entity.offhandModel.clear();
                kotor::rebuildEntityWeapons(scene, game.install(), creature);
                logInfo(concat("capture action: ", action, " on ", entity.tag, " attach '", entity.weaponAttach, "'"));
            }
        } else if (action.rfind("equipitem:", 0) == 0) {
            // equipitem:<resref>: a real equip through the rules, so clips follow the item.
            const std::string item = action.substr(10u, at - 10u);
            game.giveItem(item, ScriptApi{game}.pc());
            const bool done = game.equipItem(item);
            logInfo(concat("capture action: ", action, done ? " ok" : " FAILED"));
        } else if (action.rfind("dumpai@", 0) == 0) {
            // What each creature is doing, to read a paired fight against the retail frames.
            ScriptApi api{game};
            logInfo(concat("ai camera yaw ", game.yaw(), " at frame ", frame, " timers ", game.scriptWorld().timers.size(),
                           " paused ", game.dialoguePaused(), " entry ", game.dialogue().entry, " camera ",
                           game.dialogue().cameraId, " options ", game.dialogue().options.size(), " age ", game.dialogueEntryAge(),
                           " auto ", game.dialogueAutoAdvance()));
            for (std::size_t i = 0; i < scene.entities.size(); ++i) {
                const kotor::Entity& e = scene.entities[i];
                if (e.kind != kotor::EntityKind::Creature || e.faction == 5) {
                    continue;
                }
                const std::uint32_t object = game.objectForEntityIndex(i);
                const ActorState& actor = api.actor(object);
                logInfo(concat("ai ", e.tag, " at ", e.position.x, ",", e.position.y, " bearing ", e.bearing,
                               " faction ", e.faction, " hp ", e.hitPoints, " dead ", e.dead,
                               " queue ", actor.queue.size(), " combat ", actor.combat.inCombat,
                               " clip '", game.animationName(i), "'"));
            }
        } else if (action.rfind("cam@", 0) == 0) {
            // The camera in force, to read a paired shot against retail's logged eye.
            const SceneCamera view = game.camera();
            logInfo(concat("cam ", frame, " eye ", view.eye.x, ",", view.eye.y, ",", view.eye.z, " fwd ",
                           view.forward.x, ",", view.forward.y, ",", view.forward.z));
        } else if (action.rfind("notutor@", 0) == 0) {
            // A retail save has seen its tutorial popups; a fresh run would stop at each.
            game.setTutorialWindowsEnabledForTest(false);
            logInfo(concat("capture action: ", action));
        } else if (action.rfind("store:", 0) == 0) {
            // store:<tag>@N opens that merchant, as retail's OpenStore on the same store (paired store shots).
            const bool done = game.openStoreByTag(action.substr(6u, at - 6u));
            logInfo(concat("capture action: ", action, done ? " ok" : " FAILED"));
        } else if (action.rfind("galaxy:", 0) == 0) {
            // galaxy:<planet>@N is ShowGalaxyMap(planet) (paired galaxy shots).
            game.showGalaxyMap(static_cast<std::int32_t>(std::strtol(action.c_str() + 7, nullptr, 10)));
            logInfo(concat("capture action: ", action));
        } else if (action.rfind("align:", 0) == 0) {
            // align:<0..100>@N sets the PC's alignment (paired character sheet shots).
            game.setAlignment(static_cast<std::int32_t>(std::strtol(action.c_str() + 6, nullptr, 10)));
            logInfo(concat("capture action: ", action));
        } else if (action.rfind("attack@", 0) == 0) {
            // attack@N, so a paired combat burst can start at a known frame.
            game.attackNearestHostile();
            logInfo(concat("capture action: ", action));
        } else if (runAnimationCaptureAction(game, action.substr(0, at))) {
            logInfo(concat("capture action: ", action));
        } else if (action.rfind("fullupload", 0) == 0) {
            const auto begin = std::chrono::steady_clock::now();
            backend.setScene(&scene);
            logInfo(concat("scene upload: ", std::chrono::duration<double, std::milli>(
                                                 std::chrono::steady_clock::now() - begin).count(), " ms"));
        }
    }
}

int runApplication(const Options& options)
{
    if (!options.profileInfo.empty()) {
        const ke::kotor::GameProfile profile =
            ke::kotor::GameProfile::detect(options.profileInfo);
        logInfo(concat("game profile: kind=", ke::kotor::gameKindName(profile.kind),
                       " name=", profile.name,
                       " executable=", profile.executable,
                       " sha256=", profile.buildSha256,
                       " recognized=", profile.recognized ? 1 : 0,
                       " world=", profile.supportsWorldLoad() ? 1 : 0,
                       " original_saves=", profile.supportsOriginalSaves() ? 1 : 0));
        return profile.recognized ? 0 : 1;
    }
    if (!options.originalSaveCreate.empty()) {
        return runOriginalSaveCreate(options);
    }
    if (!options.originalSaveLoad.empty() || !options.originalSaveRoundtrip.empty()) {
        return runOriginalSaveGate(options);
    }
    if (!options.vmCorpus.empty()) {
        return runVmCorpus(options);
    }
    if (options.selfTest) {
        return runImGuiSelfTest();
    }

    std::optional<GameSession> game;
    std::unique_ptr<StateTrace> stateTrace; // --state-trace; declared after `game` so it detaches first
    std::string traceHudLayout;             // the HUD layout drawn this frame (empty: none)
    std::unique_ptr<Chargen> chargen;
    std::unique_ptr<ChargenGuiState> chargenGui;
    bool chargenActive = options.chargen;
    std::unique_ptr<audio::Output> audioOutput;
    // --start-minigame needs no --module: swoop is the first race the story reaches.
    std::string startModule = options.module;
    if (startModule.empty() && options.startMinigame == "swoop") {
        startModule = "tar_m03mg";
    } else if (startModule.empty() && options.startMinigame == "turret") {
        startModule = "M12ab";
    }
    if (!startModule.empty()) {
        std::string installPath = options.install;
        if (installPath.empty()) {
#if defined(_WIN32)
            char* value = nullptr;
            std::size_t length = 0;
            if (_dupenv_s(&value, &length, "KOTOR_INSTALL") == 0 && value != nullptr) {
                installPath = value;
                std::free(value);
            }
#else
            if (const char* value = std::getenv("KOTOR_INSTALL"); value != nullptr) {
                installPath = value;
            }
#endif
            if (installPath.empty()) {
                installPath = "E:/SteamLibrary/steamapps/common/swkotor";
            }
        }
        try {
            game.emplace(installPath, startModule, options.language,
                         options.retailDifferentialTest || !options.engineTrace.empty());
            logInfo(concat("Language: ", game->localization().languageName(),
                           " (", game->languageId(), ") tlk entries=",
                           game->localization().entryCount()));
            if (chargenActive) {
                chargen = std::make_unique<Chargen>(game->rules());
            }
            game->setAnimationEnabled(!options.headless && options.backend != BackendKind::Null);
            // Retail plays movies in a blocking loop: a window holds the world while one plays.
            // Headless and null runs (the gates) keep ticking it.
            game->setMovieHoldsWorld(!options.headless && options.backend != BackendKind::Null);
            if (options.thirdPerson) {
                game->setPlayerAppearance(
                    static_cast<std::uint32_t>(std::max(0, options.playerAppearance)),
                    options.playerName);
                game->setThirdPerson(true);
                game->setCameraNoClip(options.cameraNoClip);
            }
            if (options.noMovies) {
                game->stopMovie();
            }
            if (options.startMinigame == "pazaak") {
                game->startPazaak(0, "", 0, false, 0);
            } else if (options.startMinigame == "swoop" && game->module() != "tar_m03mg") {
                game->enterMiniGameModule("tar_m03mg");
            } else if (options.startMinigame == "turret" && game->module() != "M12ab") {
                game->enterMiniGameModule("M12ab");
            }
            const ke::kotor::Scene& loaded = game->scene();
            const auto countKind = [&](ke::kotor::EntityKind kind) {
                return std::count_if(loaded.entities.begin(), loaded.entities.end(),
                                     [kind](const ke::kotor::Entity& entity) { return entity.kind == kind; });
            };
            logInfo(concat("KotOR module ", loaded.module, " area ", loaded.area, " from ", loaded.installPath));
            logInfo(concat("  build sha256: ", game->buildSha256()));
            logInfo(concat("  rooms: ", loaded.rooms.size(), " vertices: ", loaded.vertices.size(),
                           " indices: ", loaded.indices.size(), " batches: ", loaded.batches.size()));
            logInfo(concat("  textures: ", loaded.textures.size(), " collision vertices: ",
                           loaded.collisionVertices.size(), " facets: ", loaded.collision.size()));
            logInfo(concat("  entities: ", loaded.entities.size(), " (door ", countKind(ke::kotor::EntityKind::Door),
                           ", placeable ", countKind(ke::kotor::EntityKind::Placeable),
                           ", creature ", countKind(ke::kotor::EntityKind::Creature),
                           ", waypoint ", countKind(ke::kotor::EntityKind::Waypoint),
                           ", store ", countKind(ke::kotor::EntityKind::Store),
                           ", encounter ", countKind(ke::kotor::EntityKind::Encounter), ")"));
            logInfo(concat("  encounter volumes: ", loaded.encounters.size()));
            logInfo(concat("  entry: ", loaded.entry.x, ", ", loaded.entry.y, ", ", loaded.entry.z));
            {
                // A model with no drawable mesh (every slot NULL, or Render off) has nothing to show. The
                // helper placeables (PLC_Invis and kin) are meshless by design, and waypoints, triggers,
                // sounds and encounters have no model at all (Entity::model holds their template name), so
                // the list is a debug line, printed with KOTOR_LOG_DEBUG=1.
                std::map<std::string, int> withoutMesh;
                std::set<std::uint32_t> meshOwners;
                for (const kotor::EntityMesh& mesh : loaded.entityMeshes) {
                    meshOwners.insert(mesh.entity);
                }
                for (std::size_t i = 0; i < loaded.entities.size(); ++i) {
                    const kotor::Entity& entity = loaded.entities[i];
                    const bool modelled = entity.kind == kotor::EntityKind::Door || entity.kind == kotor::EntityKind::Placeable
                        || entity.kind == kotor::EntityKind::Creature;
                    if (modelled && !entity.model.empty() && meshOwners.count(static_cast<std::uint32_t>(i)) == 0u) {
                        ++withoutMesh[entity.model];
                    }
                }
                for (const auto& [model, count] : withoutMesh) {
                    logDebug(concat("  model without a mesh: ", model, " x", count));
                }
            }
            logInfo(concat("  entity meshes: ", loaded.entityMeshes.size(), " triggers: ",
                           loaded.triggers.size(), " warnings: ", loaded.warnings.size()));
            if (options.thirdPerson) {
                for (const ke::kotor::EntityMesh& mesh : loaded.entityMeshes) {
                    if (mesh.entity != game->playerEntity() || mesh.vertices.empty()) {
                        continue;
                    }
                    ke::kotor::Vec3 low = mesh.vertices.front().position;
                    ke::kotor::Vec3 high = low;
                    for (const ke::kotor::SceneVertex& vertex : mesh.vertices) {
                        low.x = std::min(low.x, vertex.position.x);
                        low.y = std::min(low.y, vertex.position.y);
                        low.z = std::min(low.z, vertex.position.z);
                        high.x = std::max(high.x, vertex.position.x);
                        high.y = std::max(high.y, vertex.position.y);
                        high.z = std::max(high.z, vertex.position.z);
                    }
                    logInfo(concat("  player mesh verts=", mesh.vertices.size(),
                                   " local=", low.x, ",", low.y, ",", low.z,
                                   " .. ", high.x, ",", high.y, ",", high.z));
                }
                if (game->playerEntity() < loaded.entities.size()) {
                    const ke::kotor::Entity& player = loaded.entities[game->playerEntity()];
                    const SceneCamera camera = game->camera();
                    logInfo(concat("  player entity pos=", player.position.x, ",",
                                   player.position.y, ",", player.position.z,
                                   " camera=", camera.eye.x, ",", camera.eye.y, ",",
                                   camera.eye.z, " forward=", camera.forward.x, ",",
                                   camera.forward.y, ",", camera.forward.z));
                    for (const ke::kotor::Entity& entity : loaded.entities) {
                        const float dx = entity.position.x - camera.eye.x;
                        const float dy = entity.position.y - camera.eye.y;
                        const float dz = entity.position.z - camera.eye.z;
                        if (dx * dx + dy * dy + dz * dz < 6.25f) {
                            logInfo(concat("  near camera: ", entity.tag, " kind=",
                                           static_cast<int>(entity.kind), " at ",
                                           entity.position.x, ",", entity.position.y, ",",
                                           entity.position.z));
                        }
                    }
                }
            }
            std::size_t hookCount = 0;
            std::size_t entitiesWithHooks = 0;
            for (const ke::kotor::Entity& entity : loaded.entities) {
                if (!entity.scripts.empty()) {
                    ++entitiesWithHooks;
                    hookCount += entity.scripts.size();
                }
            }
            logInfo(concat("  script hooks: ", hookCount, " on ", entitiesWithHooks,
                           " entities; scene hooks: ", loaded.scripts.size(),
                           "; script runs: ", game->scriptLog().size()));
            if (options.contentStats) {
                const auto jsonText = [](const std::string& value) {
                    std::string out = "\"";
                    for (const char ch : value) {
                        if (ch == '"' || ch == '\\') {
                            out += '\\';
                        }
                        out += ch;
                    }
                    out += '"';
                    return out;
                };
                std::ostringstream json;
                json << "{\"module\":" << jsonText(loaded.module)
                     << ",\"area\":" << jsonText(loaded.area)
                     << ",\"area_tag\":" << jsonText(loaded.areaTag)
                     << ",\"rooms\":" << loaded.rooms.size()
                     << ",\"vertices\":" << loaded.vertices.size()
                     << ",\"indices\":" << loaded.indices.size()
                     << ",\"batches\":" << loaded.batches.size()
                     << ",\"textures\":" << loaded.textures.size()
                     << ",\"collision_vertices\":" << loaded.collisionVertices.size()
                     << ",\"collision_facets\":" << loaded.collision.size()
                     << ",\"entities\":" << loaded.entities.size()
                     << ",\"doors\":" << countKind(ke::kotor::EntityKind::Door)
                     << ",\"placeables\":" << countKind(ke::kotor::EntityKind::Placeable)
                     << ",\"creatures\":" << countKind(ke::kotor::EntityKind::Creature)
                     << ",\"waypoints\":" << countKind(ke::kotor::EntityKind::Waypoint)
                     << ",\"stores\":" << countKind(ke::kotor::EntityKind::Store)
                     << ",\"encounter_entities\":" << countKind(ke::kotor::EntityKind::Encounter)
                     << ",\"sound_entities\":" << countKind(ke::kotor::EntityKind::Sound)
                     << ",\"entity_meshes\":" << loaded.entityMeshes.size()
                     << ",\"triggers\":" << loaded.triggers.size()
                     << ",\"encounter_volumes\":" << loaded.encounters.size()
                     << ",\"cameras\":" << loaded.cameras.size()
                     << ",\"sounds\":" << loaded.soundEmitters.size()
                     << ",\"script_hooks\":" << hookCount
                     << ",\"scripted_entities\":" << entitiesWithHooks
                     << ",\"scene_scripts\":" << loaded.scripts.size()
                     << ",\"script_runs\":" << game->scriptLog().size()
                     << ",\"warnings\":" << loaded.warnings.size() << "}";
                logInfo(concat("content-stats: ", json.str()));
                for (const auto& [hook, resref] : loaded.scripts) {
                    logInfo(concat("  scene script ", hook, " -> ", resref));
                }
                for (const std::string& line : game->scriptLog()) {
                    logInfo(concat("  script run ", line));
                }
                return 0;
            }
            if (!options.panel.empty()) {
                UiPanel panel = UiPanel::None;
                if (options.panel == "character") panel = UiPanel::Character;
                else if (options.panel == "inventory") panel = UiPanel::Inventory;
                else if (options.panel == "party") panel = UiPanel::Party;
                else if (options.panel == "journal") panel = UiPanel::Journal;
                else if (options.panel == "merchant") panel = UiPanel::Merchant;
                game->setUiPanel(panel);
                if (panel == UiPanel::Merchant && !game->merchants().empty()) {
                    std::size_t preferred = 0;
                    for (std::size_t i = 0; i < game->merchants().size(); ++i) {
                        if (game->merchants()[i].resref == "keblastore") {
                            preferred = i;
                            break;
                        }
                    }
                    game->openStore(game->merchants()[preferred].entity);
                }
                if (!options.addParty.empty()) {
                    game->addPartyMember(options.addParty);
                }
            }
            for (std::size_t i = 0; i < loaded.entityMeshes.size() && i < 3; ++i) {
                const ke::kotor::EntityMesh& mesh = loaded.entityMeshes[i];
                if (mesh.vertices.empty() || mesh.entity >= loaded.entities.size()) {
                    continue;
                }
                ke::kotor::Vec3 low = mesh.vertices.front().position;
                ke::kotor::Vec3 high = low;
                for (const ke::kotor::SceneVertex& vertex : mesh.vertices) {
                    low.x = std::min(low.x, vertex.position.x);
                    low.y = std::min(low.y, vertex.position.y);
                    low.z = std::min(low.z, vertex.position.z);
                    high.x = std::max(high.x, vertex.position.x);
                    high.y = std::max(high.y, vertex.position.y);
                    high.z = std::max(high.z, vertex.position.z);
                }
                logInfo(concat("  entity mesh ", i, " ", loaded.entities[mesh.entity].tag,
                               " verts=", mesh.vertices.size(), " local=", low.x, ",", low.y, ",", low.z,
                               " .. ", high.x, ",", high.y, ",", high.z));
            }
            for (const ke::kotor::EntityMesh& mesh : loaded.entityMeshes) {
                if (mesh.entity >= loaded.entities.size()
                    || loaded.entities[mesh.entity].kind != ke::kotor::EntityKind::Creature
                    || mesh.vertices.empty()) {
                    continue;
                }
                ke::kotor::Vec3 low = mesh.vertices.front().position;
                ke::kotor::Vec3 high = low;
                for (const ke::kotor::SceneVertex& vertex : mesh.vertices) {
                    low.x = std::min(low.x, vertex.position.x);
                    low.y = std::min(low.y, vertex.position.y);
                    low.z = std::min(low.z, vertex.position.z);
                    high.x = std::max(high.x, vertex.position.x);
                    high.y = std::max(high.y, vertex.position.y);
                    high.z = std::max(high.z, vertex.position.z);
                }
                logInfo(concat("  creature mesh ", loaded.entities[mesh.entity].tag,
                               " verts=", mesh.vertices.size(), " local=", low.x, ",", low.y, ",", low.z,
                               " .. ", high.x, ",", high.y, ",", high.z));
                break;
            }
            for (std::size_t i = 0; i < loaded.warnings.size() && i < 8; ++i) {
                logWarn(concat("  model warning: ", loaded.warnings[i]));
            }
            if (!loaded.vertices.empty()) {
                kotor::Vec3 low = loaded.vertices.front().position;
                kotor::Vec3 high = low;
                for (const kotor::SceneVertex& vertex : loaded.vertices) {
                    low.x = std::min(low.x, vertex.position.x);
                    low.y = std::min(low.y, vertex.position.y);
                    low.z = std::min(low.z, vertex.position.z);
                    high.x = std::max(high.x, vertex.position.x);
                    high.y = std::max(high.y, vertex.position.y);
                    high.z = std::max(high.z, vertex.position.z);
                }
                logInfo(concat("  bounds: ", low.x, ", ", low.y, ", ", low.z, " .. ",
                               high.x, ", ", high.y, ", ", high.z));
            }
            if (options.sceneAssert) {
                // Counts of the shipped module. Batches are per room and material, textures include
                // the lightmaps, and an emitter is a particle source instead of an entity mesh.
                const bool ok = loaded.rooms.size() == 15
                    // 948 vertices left the static rooms for 3 room loop meshes (animloop2 of
                    // M01aa_07a and animloop2 and 3 of M01aa_07b, retail 00607610): 232140 in all.
                    && loaded.vertices.size() == 231192
                    && loaded.indices.size() == 231192
                    && loaded.batches.size() == 351
                    // Hand items now use their UTI's ModelVariation (retail 005b30e0), which adds
                    // w_BlstrRfl_002 and the red blade w_lsabrered01 of w_Lghtsbr_002: 167, Carth's head included.
                    // The Dark Jedi's and Bandon's heads, dropped by the old head_g skip, add n_sithappren02_h and n_darthband01_h: 169.
                    // The chunk emitter on plc_footlker loads its piece plc_chunkybit02, whose map fx_ExpParts is the last one: 170.
                    // Body-part rows draw their rebuilt map on every body node (00697610 -> 006984a3
                    // -> 0063c0a0, texnames_summary.tsv), so the other maps those models name
                    // (N_CommM04 on N_RepSold, and the like) are no longer loaded: 162.
                    // Held items keep their own maps (texnames_summary.tsv: C_DrdWar's w_RptnBlstr_001), which
                    // the body rule had replaced on body-part owners: w_BlstrCrbn_001, w_BlstrRfl_001,
                    // w_DsrptRfl_001, w_Vbroswrd_001 (appearance 355), w_BlstrPstl_001 (378), w_Lghtsbr_001,
                    // w_lsabreblue01 (30) and w_lsabrered01 (41) come back: 170.
                    && loaded.textures.size() == 170
                    && loaded.collisionVertices.size() == 850
                    && loaded.collision.size() == 1365
                    && loaded.entities.size() == 218
                    && loaded.entityMeshes.size() == 131
                    && loaded.cameras.size() == 40
                    && loaded.triggers.size() == 13
                    && loaded.warnings.empty();
                logInfo(concat("scene assert: rooms=", loaded.rooms.size(),
                               " vertices=", loaded.vertices.size(),
                               " batches=", loaded.batches.size(),
                               " textures=", loaded.textures.size(),
                               " collision=", loaded.collision.size(),
                               " entities=", loaded.entities.size(),
                               " meshes=", loaded.entityMeshes.size(),
                               " emitter_entities=", [&loaded] {
                                   std::set<std::uint32_t> owners;
                                   for (const kotor::EmitterInstance& emitter : loaded.emitters) {
                                       if (emitter.entity != UINT32_MAX) {
                                           owners.insert(emitter.entity);
                                       }
                                   }
                                   return owners.size();
                               }(),
                               " cameras=", loaded.cameras.size(),
                               " triggers=", loaded.triggers.size(),
                               " warnings=", loaded.warnings.size(),
                               " ok=", ok ? 1 : 0));
                // Texture wrap (retail 00424980, 00424dd0): a texture repeats on an axis
                // unless its TXI `clamp` says otherwise. The shipped rooms tile their art
                // (UV spans up to 9.5 here), so every room triangle with a UV span above 1
                // must sit on a repeating texture, and the TXI-clamped art must stay clamped.
                std::size_t tiledRepeat = 0;
                std::size_t tiledClamped = 0;
                for (const ke::kotor::TextureBatch& batch : loaded.batches) {
                    if (batch.texture >= loaded.textures.size()) {
                        continue;
                    }
                    for (std::uint32_t at = batch.firstIndex; at + 2 < batch.firstIndex + batch.indexCount;
                         at += 3) {
                        float uLow = 1e9f;
                        float uHigh = -1e9f;
                        float vLow = 1e9f;
                        float vHigh = -1e9f;
                        for (std::uint32_t k = 0; k < 3; ++k) {
                            const ke::kotor::Vec2 uv = loaded.vertices[loaded.indices[at + k]].uv;
                            uLow = std::min(uLow, uv.x);
                            uHigh = std::max(uHigh, uv.x);
                            vLow = std::min(vLow, uv.y);
                            vHigh = std::max(vHigh, uv.y);
                        }
                        if (uHigh - uLow > 1.0f || vHigh - vLow > 1.0f) {
                            (loaded.textures[batch.texture].clamp == 0 ? tiledRepeat : tiledClamped) += 1;
                        }
                    }
                }
                std::size_t txiClamped = 0;
                bool fxClamped = true;
                for (const ke::kotor::Texture& texture : loaded.textures) {
                    if (texture.name.rfind("__fx_", 0) == 0) {
                        fxClamped = fxClamped && texture.clamp == 3;
                    } else {
                        txiClamped += texture.clamp != 0 ? 1u : 0u;
                    }
                }
                const auto wrapOf = [&](const std::string& name) {
                    std::uint32_t type = 0;
                    const auto data = game->install().findTexture(name, &type);
                    if (!data.has_value()) {
                        return -1;
                    }
                    const auto txi = game->install().find(name, 2022);
                    return static_cast<int>(ke::kotor::textureClampFlags(
                        *data, type, txi.has_value() ? &*txi : nullptr));
                };
                const int tiledArt = wrapOf("lhr_wall01"); // tiles up to 9.5 in m01aa_01a
                const int headArt = wrapOf("pmha01");     // TXI `clamp 3`
                const bool wrapOk = tiledRepeat >= 1000 && tiledClamped == 0 && fxClamped
                    && tiledArt == 0 && headArt == 3;
                logInfo(concat("scene assert: texture wrap tiled_triangles_repeating=", tiledRepeat,
                               " tiled_triangles_clamped=", tiledClamped,
                               " txi_clamped_textures=", txiClamped, " lhr_wall01=", tiledArt,
                               " pmha01=", headArt, " ok=", wrapOk ? 1 : 0));
                // Every creature whose appearance row names a head and whose body has the hook wears
                // one (retail 00698219, 0069b0fa). The party templates go through the other spawn path.
                kotor::Install headInstall(game->installPath());
                const std::shared_ptr<const kotor::TwoDa> looks = headInstall.table("appearance");
                for (const char* member : {"p_bastilla", "p_cand", "p_carth", "p_jolee", "p_juhani", "p_mission"}) {
                    kotor::spawnCreature(game->scene(), headInstall, member, {0.0f, 0.0f, 0.0f}, 0.0f);
                }
                std::size_t withHead = 0;
                std::size_t headless = 0;
                for (const kotor::EntityMesh& mesh : game->scene().entityMeshes) {
                    if (mesh.entity >= game->scene().entities.size()) {
                        continue;
                    }
                    const kotor::Entity& owner = game->scene().entities[mesh.entity];
                    if (owner.kind != kotor::EntityKind::Creature || mesh.attachModel || !mesh.model
                        || !mesh.model->hasNode("headhook") || looks == nullptr) {
                        continue;
                    }
                    const std::string row = std::to_string(owner.appearance);
                    const auto named = [&](const char* column) {
                        const std::string cell = looks->rowIndex(row) >= 0 ? looks->cell(row, column) : std::string{};
                        return !cell.empty() && cell != "****";
                    };
                    if (!named("normalhead") && !named("backuphead")) {
                        continue;
                    }
                    if (mesh.headModel) {
                        ++withHead;
                    } else {
                        ++headless;
                        logError(concat("scene assert: no head on ", owner.tag, " model ", owner.model));
                    }
                }
                const bool headsOk = headless == 0 && withHead >= 6;
                logInfo(concat("scene assert: creatures_with_head=", withHead, " headless=", headless,
                               " ok=", headsOk ? 1 : 0));
                return ok && wrapOk && headsOk ? 0 : 9;
            }
            if (options.dialogueTest) {
                return runDialogueTest(*game);
            }
            if (options.worldSimTest) {
                return runWorldSimTest(*game);
            }
            if (options.familyTest == "videoeffect") {
                return runVideoEffectFamilyTest(*game, options.videoEffectFixtures);
            }
            if (!options.actionCoverage.empty()) {
                return runActionCoverage(*game, options.actionCorpus, options.actionCoverage);
            }
            if (!options.conversationFlow.empty() || !options.plotReplay.empty()) {
                quest::FlowOptions flow;
                flow.outDir = options.conversationFlow.empty() ? options.plotReplay : options.conversationFlow;
                flow.baseline = options.questBaseline;
                flow.modules = options.questModules;
                flow.breakName = options.questBreak;
                return options.conversationFlow.empty() ? quest::runPlotReplay(*game, flow)
                                                        : quest::runConversationFlow(*game, flow);
            }
            if (!options.questTrace.empty() || !options.questGraph.empty()) {
                quest::QuestTraceOptions trace;
                trace.outDir = options.questTrace.empty() ? options.questGraph : options.questTrace;
                trace.graphOnly = options.questTrace.empty();
                trace.baseline = options.questBaseline;
                trace.modules = options.questModules;
                trace.breakName = options.questBreak;
                return quest::runQuestTrace(*game, trace);
            }
            if (!options.familyTest.empty()) {
                return runFamilyTest(options.familyTest, *game);
            }
            if (options.scriptTest) {
                return runScriptTest(*game);
            }
            if (options.rulesTest) {
                return runRulesTest(*game);
            }
            if (options.systemsTest) {
                return runSystemsTest(*game);
            }
            if (options.merchantTest) {
                return runMerchantTest(*game);
            }
            if (options.encounterTest) {
                return runEncounterTest(*game);
            }
            if (options.encounterResetTest) {
                return runEncounterResetTest(*game);
            }
            if (options.partyTest) {
                return runPartyTest(*game);
            }
            if (options.uiTest) {
                return runUiTest(*game);
            }
            if (options.lootTest) {
                return runLootTest(*game);
            }
            if (options.combatForceTest) {
                return runCombatForceTest(*game, options.combatForceScript);
            }
            if (options.animationTest) {
                return runAnimationTest(*game, options.animationScript);
            }
            if (options.audioTest) {
                return runAudioTest(*game, options.audioScript);
            }
            if (options.mediaTest) {
                return runMediaTest(*game);
            }
            if (options.guiTest) {
                return runGuiTest(*game);
            }
            if (options.minigameTest) {
                return runMinigameTest(*game, options.minigameScript);
            }
            if (options.localizationTest) {
                return runLocalizationTest(*game);
            }
            if (options.vfsTest) {
                return runVfsTest(*game, options.vfsFixtures);
            }
            if (options.k2Test) {
                return runK2Test(*game);
            }
            if (options.factionTest) {
                return runFactionTest(*game, options.factionScript);
            }
            if (options.saveMatrixTest) {
                return runSaveMatrixTest(*game, options.saveFixtures);
            }
            if (options.dialogueCorpusTest) {
                return runDialogueCorpusTest(*game);
            }
            if (options.differentialTest) {
                return runDifferentialTest(*game, options.saveFixtures);
            }
            if (options.retailDifferentialTest) {
                return runRetailDifferentialTest(*game, options.engineTrace,
                                                 options.saveFixture,
                                                 options.moduleTrace);
            }
            if (options.modulePlaythroughTest) {
                return runModulePlaythroughMatrix(game->installPath());
            }
            if (options.bugParityTest) {
                return runBugParityTest(*game);
            }
            if (options.chargenTest) {
                return runChargenTest(*game);
            }
            if (options.campaignTest) {
                return runCampaignTest(*game, options.saveFixtures);
            }
            if (options.avatarTest) {
                return runAvatarTest(*game);
            }
            if (options.chargenGuiTest) {
                return runChargenGuiTest(*game);
            }
            if (!options.delayedTest.empty()) {
                return runDelayedTest(*game, options.delayedTest);
            }
            if (options.combatTest) {
                return runCombatTest(*game);
            }
            if (options.playthrough) {
                return runPlaythrough(*game);
            }
            if (options.landmarkTest) {
                return runLandmarkTest(*game);
            }
            if (options.sceneStats) {
                return 0;
            }
            if (options.walkTest) {
                InputState input;
                input.forward = true;
                for (int i = 0; i < 240; ++i) {
                    input.mouseDx = (i < 120) ? 0.8f : -0.8f;
                    game->update(1.0f / 60.0f, input);
                    if (i % 60 == 0) {
                        const kotor::Vec3 p = game->position();
                        logInfo(concat("  walk ", i, ": ", p.x, ", ", p.y, ", ", p.z,
                                       " ground=", game->grounded() ? 1 : 0));
                    }
                }
                const kotor::Vec3 p = game->position();
                logInfo(concat("  walk result: ", p.x, ", ", p.y, ", ", p.z,
                               " floor=", game->floorHeightAt(p.x, p.y)));
                if (!options.savePath.empty()) {
                    if (!game->save(options.savePath)) {
                        logError(concat("Could not save player state to ", options.savePath));
                        return 10;
                    }
                    GameSession restored(installPath, startModule);
                    if (!restored.load(options.savePath)) {
                        logError(concat("Could not load player state from ", options.savePath));
                        return 11;
                    }
                    const kotor::Vec3 q = restored.position();
                    const bool same = std::abs(q.x - p.x) < 0.001f && std::abs(q.y - p.y) < 0.001f
                        && std::abs(q.z - p.z) < 0.001f && restored.module() == game->module();
                    logInfo(concat("  save/load: ", options.savePath, " restored ",
                                   q.x, ", ", q.y, ", ", q.z, " same=", same ? 1 : 0));
                    if (!same) {
                        return 12;
                    }
                }
                return 0;
            }
            if (options.doorTest) {
                std::string from;
                std::string to;
                const bool ok = game->testTransition(from, to);
                logInfo(concat("  door test: ", from, " -> ", to, " ok=", ok ? 1 : 0));
                return ok ? 0 : 9;
            }
            if (!options.playDialogue.empty()) {
                if (!ScriptApi{*game}.startConversation(game->controlledObject(), options.playDialogue)) {
                    logWarn(concat("play dialogue did not start: ", options.playDialogue));
                }
            } else if (options.demoDialogue) {
                const std::size_t index = game->findEntity("end_trask");
                if (index != static_cast<std::size_t>(-1)) {
                    const kotor::Entity& entity = game->scene().entities[index];
                    kotor::Vec3 view;
                    if (game->findClearView(entity.position, 3.5f, view)) {
                        game->setPositionForTest(view);
                    } else {
                        game->setPositionForTest(game->scene().entry);
                    }
                    game->faceTowards(entity.position);
                    if (!game->startDialogue(index)) {
                        logWarn("demo dialogue did not start");
                    }
                } else {
                    logWarn("demo dialogue: end_trask missing");
                }
            } else if (options.demoCombat) {
                const std::size_t index = firstHostile(*game);
                if (index != static_cast<std::size_t>(-1)) {
                    const kotor::Entity& entity = game->scene().entities[index];
                    kotor::Vec3 view;
                    if (game->findClearView(entity.position, 4.0f, view)) {
                        game->setPositionForTest(view);
                    } else {
                        game->setPositionForTest({entity.position.x, entity.position.y - 3.0f,
                                                  entity.position.z});
                    }
                    game->faceTowards(entity.position);
                    game->attackNearestHostile();
                    for (int i = 0; i < 2; ++i) {
                        game->update(1.0f / 60.0f, InputState{});
                    }
                } else {
                    logWarn("demo combat: no hostile target");
                }
            }
        } catch (const std::exception& error) {
            logError(concat("KotOR module load failed: ", error.what()));
            return 8;
        }
    } else if (options.sceneStats) {
        logError("--scene-stats requires --module <stem>");
        return 2;
    }

    logInfo(concat("kotor_engine ", kEngineVersion, " starting (SDL ", SDL_MAJOR_VERSION, ".",
                   SDL_MINOR_VERSION, ".", SDL_MICRO_VERSION, ")"));

    if (options.headless) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        logInfo("Headless mode requested: forcing the SDL dummy video driver");
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        logError(concat("SDL_Init failed: ", SDL_GetError()));
        return 3;
    }

    const char* videoDriver = SDL_GetCurrentVideoDriver();
    logInfo(concat("SDL video driver: ", videoDriver != nullptr ? videoDriver : "<unknown>"));

    RuntimeState state;

    if (game && !options.headless && !options.noAudio) {
        audioOutput = std::make_unique<audio::Output>(game->audio().mixer());
        if (audioOutput->valid()) {
            logInfo(concat("Audio device: ", audioOutput->device()));
        } else {
            logWarn("Audio device unavailable; continuing silent");
            audioOutput.reset();
        }
    }
    if (game && !options.audioCapture.empty()) {
        if (game->audio().mixer().startCapture(options.audioCapture)) {
            logInfo(concat("Audio capture: ", options.audioCapture, "_<bus>.wav"));
        } else {
            logWarn(concat("Audio capture could not open ", options.audioCapture));
        }
    }
    // The shipped GUI (loading screen, menus, panels) is available in every windowed run;
    // --gui-screen and --chargen-gui additionally open one screen at start.
    const bool guiRequested = !options.guiScreen.empty() || options.chargenGui;
    if (game && (guiRequested || !options.headless)) {
        if (guiRequested) {
            game->stopMovie();
        }
        try {
            state.guiInstall = std::make_unique<kotor::Install>(game->installPath());
            state.guiInstall->bindModule(game->module());
            state.guiInstall->addPacks();
            state.guiSystem = std::make_unique<GuiSystem>(*state.guiInstall);
            const std::string screen = options.chargenGui ? "classsel" : options.guiScreen;
            if (options.chargenGui) {
                chargenGui = std::make_unique<ChargenGuiState>(game->rules());
            }
            if (!guiRequested) {
                // nothing to open yet
            } else if (state.guiSystem->open(screen)) {
                logInfo(concat("GUI screen loaded: ", state.guiSystem->screenName(),
                               " controls=", state.guiSystem->screen()->controlCount));
                if (!options.guiAction.empty()
                    && !state.guiSystem->activate(options.guiAction)) {
                    logWarn(concat("GUI action not found: ", options.guiAction));
                }
            } else {
                logWarn(concat("GUI screen not found: ", screen));
            }
        } catch (const kotor::FormatError& error) {
            logWarn(concat("GUI screen parse failed: ", error.what()));
        }
    }
    if (game && !options.moviePlay.empty()) {
        if (game->playMovie(options.moviePlay)) {
            if (options.movieTime >= 0.0f) {
                game->seekMovie(options.movieTime);
            }
            logInfo(concat("Movie playing: ", game->movieName(),
                           " duration=", game->movieDuration(),
                           " frame=", game->movieFrameIndex(),
                           " decoder=", kotor::BinkPlayer::available() ? "ffmpeg" : "none"));
        } else {
            logWarn(concat("Movie failed to start: ", options.moviePlay));
        }
    }
    if (game) {
        game->setDialogueAutoAdvance(!options.noAutoAdvance);
    }
    if (game && options.startupMovies && !options.noMovies) {
        if (game->playStartupMovies()) {
            logInfo(concat("Startup movies: ", game->movieName(), " first, ",
                           game->queuedMovies(), " queued"));
        } else {
            logWarn("Startup movies could not start (leclogo/biologo/legal)");
        }
    }
    if (game && options.facialDemo >= 0) {
        game->stopMovie();
        kotor::LipSync lip;
        lip.name = "facial_demo";
        lip.duration = 2.0f;
        lip.frames.push_back({0.0f, static_cast<std::uint8_t>(options.facialDemo)});
        lip.frames.push_back({2.0f, static_cast<std::uint8_t>(options.facialDemo)});
        game->setLipSyncForTest(std::move(lip));
        logInfo(concat("Facial demo viseme: ", options.facialDemo,
                       " name=", game->facialRig().visemeName(options.facialDemo)));
    }

    std::vector<BackendKind> candidates;
    if (options.backend == BackendKind::Auto) {
        if (options.headless) {
            candidates = {BackendKind::Null};
        } else {
            candidates = {BackendKind::Vulkan};
            if (options.allowFallback) {
                candidates.push_back(BackendKind::OpenGL);
            }
        }
    } else {
        candidates = {options.backend};
    }

    std::string error;
    for (const BackendKind kind : candidates) {
        logInfo(concat("Trying backend: ", buildLabel(kind)));
        if (kind == BackendKind::OpenGL) {
            std::string attributeError;
            if (!setGlWindowAttributes(attributeError)) {
                logWarn(concat("SDL_GL_SetAttribute failed: ", attributeError));
                continue;
            }
        }
        state.window = SDL_CreateWindow(options.title.c_str(), options.width, options.height,
                                        windowFlagsFor(kind, options));
        if (state.window == nullptr) {
            logWarn(concat(buildLabel(kind), " window creation failed: ", SDL_GetError()));
            continue;
        }

        state.backend = createBackend(kind, error);
        if (!state.backend) {
            logWarn(concat(buildLabel(kind), " backend unavailable: ", error));
            SDL_DestroyWindow(state.window);
            state.window = nullptr;
            continue;
        }

        error.clear();
        if (!state.backend->initialize(state.window, options, error)) {
            logWarn(concat(buildLabel(kind), " initialization failed: ", error));
            state.backend->shutdown();
            state.backend.reset();
            SDL_DestroyWindow(state.window);
            state.window = nullptr;
            continue;
        }

        state.activeBackend = kind;
        break;
    }

    if (!state.backend || state.window == nullptr) {
        logError(concat("No usable render backend for the requested selection (",
                        backendName(options.backend), "). See the messages above."));
        audioOutput.reset();
        SDL_Quit();
        return 5;
    }

    logInfo(concat("Backend ready: ", state.backend->info().name));
    logInfo(concat("  API:       ", state.backend->info().apiVersion));
    logInfo(concat("  Device:    ", state.backend->info().device));
    logInfo(concat("  Swapchain: ", state.backend->info().swapchain));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // deterministic, no stray imgui.ini in test runs
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    if (state.backend->supportsImGui()) {
        if (!state.backend->initializeImGui(state.window, error)) {
            logError(concat("ImGui backend initialization failed: ", error));
            destroyRuntime(state);
            audioOutput.reset();
            SDL_Quit();
            return 5;
        }
        state.imguiReady = true;
        logInfo(concat("ImGui ", IMGUI_VERSION, " renderer initialized"));
    } else {
        logInfo("ImGui disabled for the null backend (use a GPU backend for the overlay)");
    }

    const std::string savePath = options.savePath.empty() ? "engine-save.txt" : options.savePath;
    if (game && options.loadSave) {
        if (game->load(savePath)) {
            logInfo(concat("Loaded player state from ", savePath));
        } else {
            logWarn(concat("Could not load player state from ", savePath));
        }
    }
    if (game) {
        state.backend->setScene(&game->scene());
        state.backend->setCamera(game->camera());
        // ENGINE CHOICE (pointer in play): the cursor is free, as in retail, so the top menu bar
        // and every HUD control answer it without a key press. Camera look is taken while the right
        // mouse button is held (the cursor hides and the motion turns the camera, then the cursor
        // is free again on release); F1 locks look on for good and releases it again.
        SDL_SetWindowRelativeMouseMode(state.window, false);
    }
    // Portrait art for the HUD, character panel and chargen, and the chargen 3D stage.
    ChargenVisuals visuals;
    std::unique_ptr<PortraitImages> portraitImages;
    std::unique_ptr<ChargenPreview> chargenPreview;
    std::unique_ptr<ChargenPreviewGate> previewGate;
    std::unique_ptr<MenuScene> menuScene;
    std::unique_ptr<UserSettings> userSettings;
    std::unique_ptr<FrontEnd> frontEnd;
    std::unique_ptr<GuiSystem> hudGui;
    std::unique_ptr<GuiSystem> pauseGui; // pause.gui, CSWGuiInGamePause (006c03b0)
    std::unique_ptr<GuiSystem> tooltipGui; // tooltip<size>.gui, the pointer's box (00624af0)
    std::unique_ptr<GuiSystem> barkGui;    // barkbubble.gui, BarkString's box (006a9770)
    int pauseReason = 4;                 // CGuiInGame::SetPaused reason; 4 is the pause key (00621210)
    std::unique_ptr<GameScreens> gameScreens;
    std::unique_ptr<GuiSystem> dialogueGui;
    std::unique_ptr<GuiSystem> computerGui; // the terminal screen of a Computer conversation
    std::unique_ptr<GuiSystem> computerCameraGui; // its live camera feed (computercamera.gui, 006a95f0)
    LoadScreenProbe loadScreenProbe;
    std::unique_ptr<DialogueScreen> dialogueScreen;
    std::unique_ptr<Hud> hud;
    std::unique_ptr<GuiSystem> loadScreenGui; // its own screen stack: the hook runs inside update()
    std::unique_ptr<FrontEndGate> frontEndGate;
    std::filesystem::path settingsPath;
    // Window size and fullscreen from the options screen or F11: (width, height, fullscreen).
    // 0 keeps the size, -1 keeps the fullscreen state.
    std::function<void(int, int, int)> applyDisplay;
    std::string hudTooltipShown;
    GuiPainter::Stats hudStats; // what the HUD's own paint drew (the menu bar paints after it)
    GuiView hudViewUsed;
    GuiView hudPickView; // the layout the pointer is tested against (the HUD moves its controls)
    std::function<std::string(float, float)> hudPickAt; // the HUD button under a window pixel (empty: none)
    std::unique_ptr<TextureWrapGate> wrapGate;
    int gateExit = options.textureWrapCheck && !game ? 67 : 0;
    if (options.textureWrapCheck && game) {
        wrapGate = std::make_unique<TextureWrapGate>();
    }
    bool chargenStage = false;
    bool gamePreviewShown = false; // an in-game 3D frame holds the backend's preview scene
    // New Game from the main menu runs the shipped chargen and then enters the first module.
    bool newGameFlow = false;
    // The 3D stage behind classsel/portcust: only the models, as in retail, no module.
    const auto setUpChargenStage = [&]() {
        if (!state.backend->supportsPreview()) {
            logWarn("chargen: this backend cannot draw 3D previews; model controls stay empty");
            return;
        }
        if (!chargenPreview) {
            chargenPreview = std::make_unique<ChargenPreview>(game->install(), game->rules());
        }
        if (!chargenPreview->ready()) {
            logWarn(concat("chargen: preview unavailable: ", chargenPreview->error()));
            chargenPreview.reset();
            return;
        }
        visuals.preview = chargenPreview.get();
        state.backend->setScene(nullptr);
        chargenStage = true;
    };
    if (game && state.imguiReady) {
        portraitImages = std::make_unique<PortraitImages>(game->install());
        visuals.backend = state.backend.get();
        visuals.portraits = portraitImages.get();
        gVisuals = &visuals;
        if (state.guiSystem && state.guiInstall) {
            GameSession* session = &*game;
            state.guiPainter = std::make_unique<GuiPainter>(
                *state.guiInstall, state.backend.get(),
                [session](std::uint32_t strref) { return session->localization().tlk().text(strref); });
            gGuiPainter = state.guiPainter.get();
            {
                std::filesystem::path userDir;
                if (char* pref = SDL_GetPrefPath("kotor-remaster", "engine")) {
                    userDir = pref;
                    SDL_free(pref);
                }
                settingsPath = options.settingsFile.empty() ? userDir / "settings.ini"
                                                            : std::filesystem::path(options.settingsFile);
                const std::filesystem::path savesRoot = options.savesDir.empty()
                    ? userDir / "saves" : std::filesystem::path(options.savesDir);
                userSettings = std::make_unique<UserSettings>();
                if (!userSettings->load(settingsPath)) {
                    // First run: start from the volumes the install's swkotor.ini carries.
                    const AudioSystem::UserVolumes& ini = game->audio().userVolumes();
                    userSettings->set("sound.music", ini.music * 100.0f);
                    userSettings->set("sound.voice", ini.voice * 100.0f);
                    userSettings->set("sound.effects", ini.effects * 100.0f);
                    userSettings->set("sound.movie", ini.movie * 100.0f);
                    userSettings->dirty = false;
                }
                frontEnd = std::make_unique<FrontEnd>(*state.guiSystem, *game, *state.guiPainter,
                                                      *userSettings, savesRoot);
                frontEnd->applyAudio();
                applyDisplay = [&](int width, int height, int fullscreen) {
                    SDL_Window* window = state.window;
                    if (window == nullptr || !frontEnd) {
                        return;
                    }
                    if (fullscreen == 1) {
                        SDL_SetWindowFullscreen(window, true);
                    } else {
                        if (fullscreen == 0) {
                            SDL_SetWindowFullscreen(window, false);
                        }
                        if (width > 0 && height > 0) {
                            SDL_SetWindowSize(window, width, height);
                            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                        }
                    }
                    SDL_SyncWindow(window);
                    int w = 0;
                    int h = 0;
                    SDL_GetWindowSizeInPixels(window, &w, &h);
                    SDL_Rect desktop{};
                    SDL_GetDisplayBounds(SDL_GetDisplayForWindow(window), &desktop);
                    const bool full = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
                    frontEnd->setDisplay(w, h, full, desktop.w, desktop.h);
                    logInfo(concat("Display: ", w, "x", h, full ? " fullscreen" : " windowed", " (desktop ",
                                   desktop.w, "x", desktop.h, ")"));
                };
                applyDisplay(0, 0, -1);
                if (!options.sizeGiven && !options.hidden && !options.headless) {
                    // The size the player last chose, unless the command line named one.
                    if (options.fullscreen || userSettings->get("graphics.fullscreen") != 0.0f) {
                        applyDisplay(0, 0, 1);
                    } else if (userSettings->get("graphics.width") >= 640.0f
                               && userSettings->get("graphics.height") >= 480.0f) {
                        applyDisplay(static_cast<int>(userSettings->get("graphics.width")),
                                     static_cast<int>(userSettings->get("graphics.height")), 0);
                    }
                }
                visuals.frontEnd = frontEnd.get();
                gameScreens = std::make_unique<GameScreens>(*state.guiSystem, *game, *state.guiPainter);
                visuals.gameScreens = gameScreens.get();
                loadScreenGui = std::make_unique<GuiSystem>(*state.guiInstall);
                hudGui = std::make_unique<GuiSystem>(*state.guiInstall);
                pauseGui = std::make_unique<GuiSystem>(*state.guiInstall);
                if (!pauseGui->open("pause")) {
                    pauseGui.reset();
                }
                tooltipGui = std::make_unique<GuiSystem>(*state.guiInstall);
                if (!tooltipGui->open("tooltip8x6")) {
                    tooltipGui.reset();
                }
                barkGui = std::make_unique<GuiSystem>(*state.guiInstall);
                if (!barkGui->open("barkbubble")) {
                    barkGui.reset();
                }
                {
                    int windowW = 0;
                    int windowH = 0;
                    SDL_GetWindowSizeInPixels(state.window, &windowW, &windowH);
                    float designW = 0.0f;
                    float designH = 0.0f;
                    const std::string layout = Hud::layoutFor(static_cast<float>(windowW), static_cast<float>(windowH), designW, designH);
                    if (hudGui->open(layout)) {
                        hud = std::make_unique<Hud>(*hudGui, *game, gameScreens.get());
                    } else {
                        logWarn(concat("the HUD screen ", layout, " is missing"));
                    }
                }
                // The HUD's controls move with the window edges, so the pointer is tested against
                // where the painter put them (window pixels), not against the file's rectangles.
                hudPickAt = [&hudGui, &hudPickView](float px, float py) -> std::string {
                    const kotor::GuiScreen* screen = hudGui->screen();
                    if (screen == nullptr) {
                        return {};
                    }
                    for (auto it = screen->controls.rbegin(); it != screen->controls.rend(); ++it) {
                        if (it->type != 6 && it->type != 7) {
                            continue;
                        }
                        const GuiControlState controlState = hudGui->state(it->key);
                        if (!controlState.visible) {
                            continue;
                        }
                        if (guiControlPixels(hudPickView, *it, controlState.offsetX, controlState.offsetY).contains(px, py)) {
                            return it->key;
                        }
                    }
                    return {};
                };
                hudGui->setPicker(hudPickAt);
                dialogueGui = std::make_unique<GuiSystem>(*state.guiInstall);
                if (dialogueGui->open("dialog")) {
                    computerGui = std::make_unique<GuiSystem>(*state.guiInstall);
                    if (!computerGui->open("computer")) {
                        logWarn("the terminal screen computer is missing");
                        computerGui.reset();
                    }
                    if (computerGui) {
                        computerCameraGui = std::make_unique<GuiSystem>(*state.guiInstall);
                        if (!computerCameraGui->open("computercamera")) {
                            logWarn("the terminal camera screen computercamera is missing");
                            computerCameraGui.reset();
                        }
                    }
                    dialogueScreen = std::make_unique<DialogueScreen>(*dialogueGui, *game, state.guiPainter.get(),
                                                                      computerGui.get(), computerCameraGui.get());
                } else {
                    logWarn("the conversation screen dialog is missing");
                }
                game->setModuleLoadHook([&](const std::string& next) {
                    presentLoadScreen(state, *game, *loadScreenGui, next);
                    if (stateTrace && loadScreenGui->screen() != nullptr) {
                        const kotor::GuiScreen& shown = *loadScreenGui->screen();
                        stateTrace->onLoadScreen(shown.name, shown.extent.left, shown.extent.top, shown.extent.width,
                                                 shown.extent.height);
                    }
                });
                if (options.frontEndTest) {
                    frontEndGate = std::make_unique<FrontEndGate>(options.frontEndOut, savesRoot, state.window,
                                                                  options.frontEndScenario);
                    gLoadProbe = &loadScreenProbe;
                }
            }
            if (state.backend->supportsPreview() && state.guiSystem->screenName().rfind("mainmenu", 0) == 0) {
                menuScene = std::make_unique<MenuScene>(*state.guiInstall);
                if (menuScene->ready()) {
                    visuals.menu = menuScene.get();
                } else {
                    logWarn(concat("main menu 3D scene unavailable: ", menuScene->error()));
                }
            }
        }
        if (chargenGui) {
            setUpChargenStage();
            if (options.chargenPreviewTest) {
                previewGate = std::make_unique<ChargenPreviewGate>(options.chargenPreviewOut,
                                                                   false);
            }
        }
        if (options.newGameFlowTest) {
            previewGate = std::make_unique<ChargenPreviewGate>(options.chargenPreviewOut, true);
        }
    }

    std::uint64_t frames = 0;
    bool shadowProbeAsked = false;
    int shadowProbeFrame = options.shadowProbeFrame; // --shadow-probe, 0 once reported
    // Verification only: --video-effect-sequence runs one compiled fixture per
    // frame (row0|row1|row2|off) so the GPU gate can drive the real script path.
    std::vector<std::string> videoEffectSequence;
    for (std::size_t start = 0; !options.videoEffectSequence.empty()
         && start <= options.videoEffectSequence.size();) {
        const std::size_t comma = options.videoEffectSequence.find(',', start);
        const std::size_t end = comma == std::string::npos ? options.videoEffectSequence.size() : comma;
        videoEffectSequence.push_back(options.videoEffectSequence.substr(start, end - start));
        start = end + 1;
    }
    bool loggedUiGeometry = false;
    std::uint32_t lastAnimationRevision = UINT32_MAX;
    bool vfxSpawned = false;
    std::uint64_t movieTexture = 0;
    double noDeviceMixFrames = 0.0;
    std::vector<float> noDeviceScratch;
    int uploadedMovieFrame = -1;
    std::string uploadedMovieName;
    bool movieShotSaved = false;
    bool movieUploadWarned = false;
    bool running = true;
    bool mouseRelative = false; // F1: look stays captured; the right button captures it while held
    bool lookHeld = false;
    bool userPause = false; // Space / the pause toggle: the world waits, the HUD stays live
    bool dialogueEnterConsumed = false; // Enter cancelled a live feed this frame: the terminal behind it must not also take it
    bool pauseMenuOpen = false; // optionsingame is up: the world update waits
    bool uiPausePrev = false;
    float mouseDx = 0.0f;
    bool miniGameFireHeld = false;
    bool miniGameFrozen = false;
    int renderCheckExit = options.minigameRenderCheck && !game ? 66 : 0;
    bool renderCheckMeasured = false; // an exit before the measurement is never a pass
    CapturedFrame renderCheckFull;
    std::string renderedModule = game ? game->module() : std::string{};
    std::uint64_t moduleArrivalTicks = 0;
    int moduleArrivalLogs = 10; // the first module is logged by its loader
    std::size_t renderedMeshKey = game ? game->scene().entityMeshes.size() + game->scene().meshRevision : 0u;
    float mouseDy = 0.0f;
    std::string exitReason = "loop exited";
    double smoothedFrameMs = 0.0;
    const std::uint64_t startTicks = SDL_GetTicksNS();
    std::uint64_t previousTicks = startTicks;

    std::unique_ptr<SoakGate> soak;
    if (!options.soak.empty() && game) {
        SoakGate::Settings soakSettings;
        soakSettings.scenario = options.soak;
        soakSettings.module = game->module();
        soakSettings.out = options.soakOut;
        soakSettings.cycles = options.soakCycles;
        for (std::size_t start = 0; start < options.soakModules.size();) {
            const std::size_t comma = options.soakModules.find(',', start);
            const std::size_t end = comma == std::string::npos ? options.soakModules.size() : comma;
            soakSettings.modules.push_back(options.soakModules.substr(start, end - start));
            start = end + 1;
        }
        soak = std::make_unique<SoakGate>(std::move(soakSettings));
    }
    // A run with its own budget or clock is not paced unless --max-fps says so: see pacedFps.
    const bool offlineRun = options.frames > 0 || options.fixedDt > 0.0 || soak || wrapGate || frontEndGate
        || previewGate || options.minigameRenderCheck;
    const bool windowShown = !options.hidden && !options.headless && state.activeBackend != BackendKind::Null;
    std::uint64_t paceDeadline = 0;
    std::uint64_t paceWaits = 0;
    std::uint64_t paceLateTotal = 0;
    std::uint64_t paceLateWorst = 0;

    // keymap.2da's icpcgui column is 0 for the world's keys: they do nothing while a screen, a conversation, a movie
    // or a minigame has the input.
    const auto worldKeys = [&]() {
        return game && !game->moviePlaying() && !game->dialogue().active && !game->miniGameActive() && !game->pazaakActive()
            && !(state.guiSystem && state.guiSystem->active()) && !(gameScreens && gameScreens->open());
    };

    std::uint64_t feedbackTicks = SDL_GetTicks();
    // --input-script: the lockstep walk's steps go into SDL's queue before this frame polls it; its
    // frames are taken after the frame is drawn (below).
    InputScript inputScript;
    std::vector<std::string> inputScriptShots;
    std::uint64_t inputScriptStart = 0;
    if (!options.stateTrace.empty() && game) {
        stateTrace = std::make_unique<StateTrace>();
        std::string traceError;
        if (stateTrace->open(options.stateTrace, options.stateTraceSamp, traceError)) {
            // A menu launch stands on a backdrop module that retail has no counterpart for.
            const bool menuBackdrop = state.guiSystem && state.guiSystem->active()
                && state.guiSystem->screenName().rfind("mainmenu", 0) == 0;
            stateTrace->attach(*game, menuBackdrop);
            logInfo(concat("state trace: ", options.stateTrace, " (every ", options.stateTraceSamp, " frames)"));
        } else {
            logError(concat("state trace: ", traceError));
            stateTrace.reset();
        }
    }
    if (!options.inputScript.empty()) {
        if (!inputScript.load(options.inputScript, options.inputScriptOut)) {
            logError(concat("input script: ", inputScript.error()));
            running = false;
        } else {
            logInfo(concat("input script: ", inputScript.steps().size(), " steps from ", options.inputScript));
        }
    }
    while (running) {
        if (inputScript.active()) {
            if (inputScriptStart == 0) {
                inputScriptStart = SDL_GetTicks();
                // Arms the pre-swap read: a hidden window's front buffer is black.
                CapturedFrame warm;
                state.backend->captureFrame(warm);
            }
            // `waitfor` conditions: the walk waits on the game's state instead of a fixed time, so a
            // slower or faster load keeps ours in step with retail's frames.
            const auto walkCondition = [&](const std::string& condition) -> bool {
                std::istringstream words(condition);
                std::string what;
                std::string arg;
                words >> what >> arg;
                const bool guiUp = state.guiSystem && state.guiSystem->active();
                const bool talking = game && game->dialogue().active;
                if (what == "nogui") {
                    return game && !guiUp;
                }
                if (what == "gui") {
                    return guiUp && state.guiSystem->screenName() == arg;
                }
                if (what == "panel") {
                    return gameScreens && gameScreens->open() && gameScreens->screen() == arg;
                }
                if (what == "nopanel") {
                    return !(gameScreens && gameScreens->open());
                }
                if (what == "dialogue") {
                    return talking;
                }
                if (what == "nodialogue") {
                    return game && !talking;
                }
                // The reply list is up as DialogueScreen::update decides: a reply to say, and the entry no
                // longer being spoken (006a86a0).
                const auto repliesUp = [&]() {
                    bool sayable = false;
                    for (const DialogueOption& option : game->dialogue().options) {
                        sayable = sayable || (option.reply != UINT32_MAX && option.text != "(Continue)");
                    }
                    return sayable && !game->dialogueLineSpeaking();
                };
                if (what == "entry") { // the entry's line is on screen
                    return talking && std::to_string(game->dialogue().entry) == arg && game->dialogueLineShown()
                        && !repliesUp();
                }
                if (what == "replies") { // `replies` or `replies <entry>`: the list is shown
                    return talking && repliesUp() && (arg.empty() || std::to_string(game->dialogue().entry) == arg);
                }
                if (what == "replylist") {
                    // The reply list is up (the line is over and a reply can be said), as DialogueScreen shows it.
                    if (!talking || game->dialogueLineSpeaking()) {
                        return false;
                    }
                    for (const DialogueOption& option : game->dialogue().options) {
                        if (option.reply != UINT32_MAX && option.text != "(Continue)") {
                            return true;
                        }
                    }
                    return false;
                }
                if (what == "status") {
                    return game && game->statusSummaryOpen();
                }
                if (what == "nostatus") {
                    return game && !game->statusSummaryOpen();
                }
                return false;
            };
            inputScriptShots = inputScript.advance(SDL_GetTicks() - inputScriptStart, state.window, walkCondition);
            if (const InputScript::WaitResult waited = inputScript.takeWaitResult(); waited.fresh) {
                logInfo(concat("input script: waitfor ", waited.condition, waited.timedOut ? " TIMED OUT after " : " met after ",
                               waited.waitedMs, " ms"));
            }
            // The retail walk's override scripts (warps, spawns, removing Trask), run as retail's driver
            // runs them: no caller object (rs.py passes OBJECT_INVALID).
            for (const std::string& path : inputScript.takeScripts()) {
                std::ifstream ncs(path, std::ios::binary);
                const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(ncs)), std::istreambuf_iterator<char>());
                const bool ran = game && !bytes.empty() && game->runScriptNcsForTest(path, bytes, 0);
                logInfo(concat("input script: ran ", path, ran ? "" : " (FAILED)"));
            }
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (state.imguiReady) {
                ImGui_ImplSDL3_ProcessEvent(&event);
            }
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
                exitReason = "window closed";
            } else if (event.type == SDL_EVENT_MOUSE_MOTION && game && (mouseRelative || lookHeld) && !uiPausePrev) {
                mouseDx += event.motion.xrel;
                mouseDy += event.motion.yrel;
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT && game
                       && !uiPausePrev && !(state.guiSystem && state.guiSystem->active())
                       && !(state.imguiReady && ImGui::GetIO().WantCaptureMouse)) {
                lookHeld = true;
                SDL_SetWindowRelativeMouseMode(state.window, true);
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT && lookHeld) {
                lookHeld = false;
                SDL_SetWindowRelativeMouseMode(state.window, mouseRelative && !uiPausePrev);
            } else if (chargenGui && chargenNameEditing(*chargenGui)
                       && (event.type == SDL_EVENT_TEXT_INPUT
                           || (event.type == SDL_EVENT_KEY_DOWN
                               && (event.key.key == SDLK_BACKSPACE || event.key.key == SDLK_RETURN
                                   || event.key.key == SDLK_KP_ENTER)))) {
                // The name screen's text box: typed characters, Backspace, and Enter for Finish.
                if (event.type == SDL_EVENT_TEXT_INPUT) {
                    chargenNameInput(*chargenGui, event.text.text);
                } else if (event.key.key == SDLK_BACKSPACE) {
                    chargenNameBackspace(*chargenGui);
                } else if (state.guiSystem && game) {
                    handleChargenGuiAction("END_BTN", *chargenGui, *state.guiSystem, *game);
                }
            } else if (event.type == SDL_EVENT_KEY_UP && event.key.key == SDLK_T && hud) {
                hud->setInstantToolTips(false); // ToolTips released: 005f4be0(0), 00685710 puts the combat line back
            } else if (event.type == SDL_EVENT_KEY_DOWN
                       && (!state.imguiReady || !ImGui::GetIO().WantCaptureKeyboard)) {
                if (event.key.key == SDLK_ESCAPE) {
                    if (game && game->moviePlaying()) {
                        // Escape cancels a skippable movie (nwscript 769); a movie queued as
                        // not skippable plays on, and neither case closes the program.
                        const std::string skipped = game->movieName();
                        if (game->skipMovie()) {
                            logInfo(concat("Movie skipped: ", skipped));
                        }
                    } else if (state.guiSystem && state.guiSystem->active()) {
                        // A shipped screen: Escape is its Back button, not the exit key.
                        if (gameScreens && gameScreens->back()) {
                            // An in-game screen closed with its panel.
                        } else if (!guiBack(*state.guiSystem) && state.guiSystem->screenName() == "optionsingame") {
                            state.guiSystem->close(); // the pause menu resumes the game
                        }
                    } else if (game && game->dialogue().active) {
                        game->endDialogue();
                        logInfo("Dialogue closed");
                    } else if (frontEnd && state.guiSystem && state.imguiReady && game && !game->miniGameActive()
                               && !game->pazaakActive() && state.guiSystem->open("optionsingame")) {
                        // The in-game options menu pauses the world and frees the mouse. The
                        // game frame from just before it opened is what a save made from it
                        // stores as Screen.tga (retail captures the world, not the menu).
                        {
                            CapturedFrame saved;
                            if (state.backend->captureLatest(saved) && !saved.rgba.empty()) {
                                game->setSaveScreenshot(kotor::encodeSaveScreenshot(saved.rgba.data(), saved.width, saved.height));
                            }
                        }
                        pauseMenuOpen = true;
                        SDL_SetWindowRelativeMouseMode(state.window, false);
                    } else {
                        running = false;
                        exitReason = "escape pressed";
                    }
                } else if (game && worldKeys() && event.key.key == SDLK_Q && !event.key.repeat) {
                    game->cycleCombatTarget(-1); // keymap.2da row 204 SelectPrev
                } else if (game && worldKeys() && event.key.key == SDLK_E && !event.key.repeat) {
                    game->cycleCombatTarget(1); // keymap.2da row 205 SelectNext
                } else if (game && worldKeys() && event.key.key == SDLK_G && !event.key.repeat) {
                    // keymap.2da row 264 STEALTH.
                    if (game->stealthActive()) {
                        game->leaveStealth();
                    } else {
                        game->enterStealth();
                    }
                } else if (game && worldKeys() && hud && event.key.key == SDLK_T && !event.key.repeat) {
                    hud->setInstantToolTips(true); // keymap.2da row 225 ToolTips, retail 00621210 case 0xe1
                } else if (game && worldKeys() && event.key.key == SDLK_V && !event.key.repeat) {
                    game->setSoloMode(!game->soloMode()); // keymap.2da row 207, strref 48035 Solo Mode
                } else if (game && worldKeys() && event.key.key == SDLK_Y && !event.key.repeat) {
                    if (hud) {
                        hud->activate("BTN_CLEARONE"); // keymap.2da row 245 ClearOneAction
                    }
                } else if (game && worldKeys() && hud && !event.key.repeat && event.key.key >= SDLK_1 && event.key.key <= SDLK_7) {
                    // keymap.2da rows 226-238 by their character column. Retail's slots (0068b9d0): 1 to 3 the target's
                    // actions, then 4 power (row 32), 5 medical (34), 6 other item (38), 7 mine (36). The middle and
                    // right target slots are not filled yet.
                    static const char* const kSlotKeys[] = {"BTN_TARGET0", "BTN_TARGET1", "BTN_TARGET2", "BTN_ACTION0", "BTN_ACTION1", "BTN_ACTION2", "BTN_ACTION3"};
                    hud->activate(kSlotKeys[event.key.key - SDLK_1]);
                } else if (game && worldKeys() && event.key.key == SDLK_R && !event.key.repeat) {
                    // keymap.2da row 239 DefaultAction: what the cursor's target offers, which here is the nearest
                    // thing to use, else the nearest enemy to fight.
                    const std::string before = game->module();
                    if (game->activateSelected()) {
                        logInfo("Default action on the selected object");
                    } else if (game->interact()) {
                        if (game->module() != before) {
                            state.backend->setScene(&game->scene());
                            state.backend->setCamera(game->camera());
                            logInfo(concat("Transition to ", game->module(), " / ", game->scene().area));
                        } else if (game->dialogue().active) {
                            logInfo(concat("Dialogue: ", game->dialogue().speaker));
                        } else {
                            logInfo("Interaction");
                        }
                    } else {
                        game->attackNearestHostile();
                    }
                } else if (game && event.key.key == SDLK_TAB) {
                    // keymap.2da ChangeChar: the next party member takes the controls.
                    if (game->cycleLeader()) {
                        logInfo(concat("Controlling ", game->partyLeaderTag()));
                    }
                } else if (game && worldKeys() && event.key.key == SDLK_SPACE && !event.key.repeat) {
                    userPause = !userPause; // retail's pause key; the HUD's TB_PAUSE shows it
                    pauseReason = 4;
                } else if (game && worldKeys() && event.key.key == SDLK_F && !event.key.repeat) {
                    game->cancelCombat(); // keymap.2da row 240 CancleCombat
                } else if (game && game->dialogue().active && dialogueScreen && !event.key.repeat
                           && (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)
                           && dialogueScreen->cancelFeed()) {
                    // computercamera's "Press 'Enter' to Cancel Live Feed" (006a7230 inputs 0x27 / 0x2d).
                    dialogueEnterConsumed = true;
                } else if (game && game->dialogue().active
                           && event.key.key >= SDLK_1 && event.key.key <= SDLK_9) {
                    // 006a7230: a number picks a reply only from the list on screen (keys 0xfe.. below
                    // the reply count); while the line is spoken there is none.
                    if (!dialogueScreen || dialogueScreen->repliesShown()) {
                        game->chooseDialogueOption(static_cast<std::size_t>(event.key.key - SDLK_1));
                    }
                } else if (game && worldKeys() && event.key.key == SDLK_F4) { // keymap.2da row 218 Quicksave
                    logInfo(game->save(savePath) ? concat("Saved player state to ", savePath)
                                                 : concat("Could not save to ", savePath));
                } else if (game && worldKeys() && event.key.key == SDLK_F5) { // keymap.2da row 263 QUICKLOAD
                    if (game->load(savePath)) {
                        state.backend->setScene(&game->scene());
                        state.backend->setCamera(game->camera());
                        logInfo(concat("Loaded player state from ", savePath));
                    } else {
                        logWarn(concat("Could not load player state from ", savePath));
                    }
                } else if (event.key.key == SDLK_F11 && applyDisplay && state.window != nullptr) {
                    // ENGINE CHOICE: borderless fullscreen toggle (retail had none).
                    const bool full = (SDL_GetWindowFlags(state.window) & SDL_WINDOW_FULLSCREEN) != 0;
                    applyDisplay(0, 0, full ? 0 : 1);
                    if (userSettings) {
                        userSettings->set("graphics.fullscreen", full ? 0.0f : 1.0f);
                    }
                } else if (game && event.key.key == SDLK_F1) {
                    mouseRelative = !mouseRelative;
                    SDL_SetWindowRelativeMouseMode(state.window, mouseRelative);
                    logInfo(mouseRelative ? "Mouse captured" : "Mouse released");
                } else if (game && !event.key.repeat && event.key.key == SDLK_U) {
                    pageKey(*game, state.guiSystem.get(), gameScreens.get(), UiPanel::Equipment);
                } else if (game && !event.key.repeat && event.key.key == SDLK_I) {
                    pageKey(*game, state.guiSystem.get(), gameScreens.get(), UiPanel::Inventory);
                } else if (game && !event.key.repeat && event.key.key == SDLK_P) {
                    pageKey(*game, state.guiSystem.get(), gameScreens.get(), UiPanel::Character);
                } else if (game && !event.key.repeat && event.key.key == SDLK_K) {
                    pageKey(*game, state.guiSystem.get(), gameScreens.get(), UiPanel::Abilities);
                } else if (game && !event.key.repeat && event.key.key == SDLK_J) {
                    pageKey(*game, state.guiSystem.get(), gameScreens.get(), UiPanel::Messages);
                } else if (game && !event.key.repeat && event.key.key == SDLK_L) {
                    pageKey(*game, state.guiSystem.get(), gameScreens.get(), UiPanel::Journal);
                } else if (game && !event.key.repeat && event.key.key == SDLK_M) {
                    pageKey(*game, state.guiSystem.get(), gameScreens.get(), UiPanel::Map);
                } else if (game && !event.key.repeat && event.key.key == SDLK_O && gameScreens) {
                    if (gameScreens->open()) {
                        gameScreens->requestOptions();
                    } else if (state.guiSystem && state.guiSystem->screenName() == "optionsingame"
                               && state.guiSystem->active()) {
                        state.guiSystem->close();
                    } else if (state.guiSystem && !state.guiSystem->active()) {
                        gameScreens->requestOptions();
                    }
                }
            }
        }

        if (game) {
            if (userSettings) {
                std::uint32_t feedback = 0;
                for (const FeedbackRow& row : FrontEnd::feedbackTable()) {
                    feedback |= userSettings->get(row.key) != 0.0f ? row.bit : 0u;
                }
                game->setFeedbackOptions(feedback);
                // Autopause (004e0ac0 reason 9 with option bit 0x4000, the Party Killed box; 005fa5a0
                // reason 1 with bit 0x1000, Enemy Sighted).
                if (const int reason = game->takeAutoPauseRequest(); reason != 0) {
                    const bool wanted = (reason == 9 && userSettings->get("autopause.partykilled") != 0.0f)
                        || (reason == 1 && userSettings->get("autopause.enemysighted") != 0.0f
                            // ENGINE CHOICE: scripted runs (gate, soak, fixed frames) would stop at every
                            // enemy; they pause for it only when a check asks for it.
                            && (!offlineRun || game->enemySightedPauseForTest()));
                    if (wanted && !userPause) {
                        userPause = true;
                        pauseReason = reason;
                    }
                }
            }
            if (offlineRun) {
                game->holdStatusSummaryForTest(!(frontEndGate && frontEndGate->statusSummaryLive()));
            }
            // Retail's clock here is the frame's own (DAT_0078e574), running through a pause.
            const std::uint64_t feedbackNow = SDL_GetTicks();
            game->stepFeedback(static_cast<float>(feedbackNow - feedbackTicks) * 0.001f, !worldKeys());
            feedbackTicks = feedbackNow;
        }

        if (!running) {
            break;
        }

        const std::uint64_t now = SDL_GetTicksNS();
        const double deltaMs = options.fixedDt > 0.0
            ? options.fixedDt * 1000.0
            : static_cast<double>(now - previousTicks) / 1'000'000.0;
        previousTicks = now;
        if (game && !audioOutput) {
            // No audio device (--no-audio or --headless): advance the mixer on the
            // frame clock so voices retire and the bus meters keep moving.
            noDeviceMixFrames += deltaMs * 48.0;
            const int whole = static_cast<int>(noDeviceMixFrames);
            noDeviceMixFrames -= whole;
            for (int left = whole; left > 0;) {
                const int chunk = std::min(left, 2048);
                noDeviceScratch.assign(static_cast<std::size_t>(chunk) * 2, 0.0f);
                game->audio().mixer().mix(noDeviceScratch.data(), chunk);
                left -= chunk;
            }
        }
        smoothedFrameMs = (smoothedFrameMs <= 0.0) ? deltaMs : (smoothedFrameMs * 0.9 + deltaMs * 0.1);
        const double elapsedSeconds = static_cast<double>(now - startTicks) / 1'000'000'000.0;

        const bool shippedChargen = chargenGui && chargenGui->active;
        if (game) {
            InputState input;
            if (!chargenActive && !shippedChargen) {
                // --input-script holds keys the same way a hand would (InputScript::keyboardState).
                const bool* keys = inputScript.active() ? inputScript.keyboardState() : SDL_GetKeyboardState(nullptr);
                input.forward = keys[SDL_SCANCODE_W] != 0;
                input.back = keys[SDL_SCANCODE_S] != 0;
                // keymap.2da: Z and C step sideways (rows 281a, 281b), A and D turn the camera (284a, 284b). The
                // turn's speed is an ENGINE CHOICE, spoken in the mouse's units.
                input.left = keys[SDL_SCANCODE_Z] != 0;
                input.right = keys[SDL_SCANCODE_C] != 0;
                const float keyTurn = static_cast<float>(keys[SDL_SCANCODE_D] != 0) - static_cast<float>(keys[SDL_SCANCODE_A] != 0);
                mouseDx += keyTurn * static_cast<float>(deltaMs) * 0.5f;
                input.run = keys[SDL_SCANCODE_LSHIFT] != 0 || keys[SDL_SCANCODE_RSHIFT] != 0;
                input.jump = false; // Space is the pause key (retail has no jump key)
                input.mouseDx = mouseDx;
                input.mouseDy = mouseDy;
                if (options.noMovies && game->moviePlaying()) {
                    game->stopMovie();
                }
                // PauseGame holds the world until the player acknowledges it with the
                // pause key; no pause panel is drawn yet.
                if (game->scriptPauseActive() && input.jump) {
                    game->clearScriptPause();
                }
                if (const MiniGameWorld* world = game->miniGame()) {
                    // Keys follow the retail layout's meaning (006227e0). Mouse sensitivity
                    // is this window's choice: retail's raw device scale was not recovered.
                    const float seconds = std::max(static_cast<float>(deltaMs / 1000.0), 0.001f);
                    const bool leftKey = keys[SDL_SCANCODE_A] != 0 || keys[SDL_SCANCODE_LEFT] != 0;
                    const bool rightKey = keys[SDL_SCANCODE_D] != 0 || keys[SDL_SCANCODE_RIGHT] != 0;
                    const bool upKey = keys[SDL_SCANCODE_W] != 0 || keys[SDL_SCANCODE_UP] != 0;
                    const bool downKey = keys[SDL_SCANCODE_S] != 0 || keys[SDL_SCANCODE_DOWN] != 0;
                    const bool fireKey = keys[SDL_SCANCODE_SPACE] != 0
                        || (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
                    MiniGameInput miniInput;
                    miniInput.axisX = (rightKey ? 1.0f : 0.0f) - (leftKey ? 1.0f : 0.0f);
                    if (world->data.type == 1) {
                        miniInput.fire = fireKey && !miniGameFireHeld; // One gear per press.
                    } else {
                        constexpr float kDegreesPerPixel = 0.1f;
                        const float toAxis = world->movementPerSec > 0.0f
                            ? kDegreesPerPixel / (seconds * world->movementPerSec)
                            : 0.0f;
                        miniInput.axisY = (downKey ? 1.0f : 0.0f) - (upKey ? 1.0f : 0.0f);
                        miniInput.mouseX = mouseDx * toAxis;
                        miniInput.mouseY = mouseDy * toAxis;
                        miniInput.fire = fireKey; // The bank's Rate_Of_Fire paces a held trigger.
                    }
                    miniGameFireHeld = fireKey;
                    game->miniGameSetInput(miniInput);
                }
                if (pauseMenuOpen && !(state.guiSystem && state.guiSystem->active())) {
                    pauseMenuOpen = false;
                }
                const bool uiPause = pauseMenuOpen || (gameScreens && gameScreens->open());
                // A shipped screen over the world pauses it; a conversation keeps the world
                // running but, like a screen, frees the mouse for the panel.
                const bool mouseFreed = uiPause || (dialogueScreen && game->dialogue().active);
                if (mouseFreed != uiPausePrev) {
                    SDL_SetWindowRelativeMouseMode(state.window, mouseFreed ? false : (mouseRelative || lookHeld));
                    uiPausePrev = mouseFreed;
                }
                if (!miniGameFrozen && !game->scriptPauseActive() && !uiPause && !userPause) {
                    game->update(static_cast<float>(deltaMs / 1000.0), input);
                } else {
                    // retail 004babb0 calls TempDeathUpdate (004b6da0) every frame, paused or not.
                    game->tickWhilePaused(static_cast<float>(deltaMs / 1000.0));
                }
                // retail 00602eb0 returns to the main menu. Interactive runs do; the gates that
                // play the ending to its end still stop here.
                if (game->gameEnded()) {
                    if (frontEnd && state.guiSystem && state.imguiReady && !options.headless && options.frames == 0
                        && options.seconds <= 0.0) {
                        logInfo("game ended: back to the main menu");
                        game->acknowledgeGameEnd();
                        pauseMenuOpen = false;
                        if (!menuScene) {
                            menuScene = std::make_unique<MenuScene>(*state.guiInstall);
                            if (menuScene->ready()) {
                                visuals.menu = menuScene.get();
                            }
                        }
                        state.guiSystem->open("mainmenu");
                    } else {
                        logInfo("game ended: leaving the session");
                        running = false;
                    }
                }
            }
            mouseDx = 0.0f;
            mouseDy = 0.0f;
            if (!options.movieShot.empty() && !movieShotSaved
                && game->movieFrame() != nullptr) {
                const kotor::BinkVideoFrame* frame = game->movieFrame();
                if (writeBmpTopDown(options.movieShot, frame->width, frame->height,
                                    frame->rgba)) {
                    movieShotSaved = true;
                    logInfo(concat("Movie frame saved: ", options.movieShot,
                                   " frame=", game->movieFrameIndex()));
                }
            }
            if (game->module() != renderedModule) {
                // A script's StartNewModule lands here, not through a key press.
                renderedModule = game->module();
                userPause = false; // retail 005f8290 clears the pause states with the module
                state.backend->setScene(&game->scene());
                renderedMeshKey = game->scene().entityMeshes.size() + game->scene().meshRevision;
                moduleArrivalTicks = SDL_GetTicksNS();
                moduleArrivalLogs = 0;
            }
            // What the frame loop is doing after a module change, every 2 s for 20 s: the screen
            // state that can hold a frame black (fade, movie, a GUI screen, minigame, conversation).
            if (moduleArrivalLogs < 10
                && SDL_GetTicksNS() - moduleArrivalTicks >= static_cast<std::uint64_t>(moduleArrivalLogs) * 2000000000ull) {
                ++moduleArrivalLogs;
                const SceneCamera arrivalCamera = game->camera();
                const kotor::Vec3 arrivalAt = game->controlledPosition();
                logInfo(concat("module arrival ", game->module(), " t=", (moduleArrivalLogs - 1) * 2, "s fade=",
                               static_cast<int>(game->fade().mode), "/", game->fade().opacity, " movie=",
                               game->moviePlaying() ? 1 : 0, " gui=",
                               state.guiSystem && state.guiSystem->active() ? state.guiSystem->screenName() : std::string{"-"},
                               " minigame=", game->miniGameActive() ? 1 : 0, " pazaak=", game->pazaakActive() ? 1 : 0,
                               " dialogue=", game->dialogue().active ? 1 : 0, " eye=", arrivalCamera.eye.x, ",",
                               arrivalCamera.eye.y, ",", arrivalCamera.eye.z, " forward=", arrivalCamera.forward.x, ",",
                               arrivalCamera.forward.y, ",", arrivalCamera.forward.z, " pc=", arrivalAt.x, ",", arrivalAt.y,
                               ",", arrivalAt.z, " rooms=", game->scene().rooms.size(), " meshes=",
                               game->scene().entityMeshes.size()));
            }
            if (!miniGameFrozen) {
                game->bakeMiniGame();
            }
            if (!options.spawnVfx.empty() && !vfxSpawned) {
                // --spawn-vfx id[:n]: a held visual effect on the nth creature (gallery shots).
                vfxSpawned = true;
                const std::size_t colon = options.spawnVfx.find(':');
                const int wanted = colon == std::string::npos ? 0 : std::atoi(options.spawnVfx.c_str() + colon + 1);
                // The same order --capture-camera creature:n uses: alive, nearest the entry first.
                const kotor::Scene& vfxScene = game->scene();
                std::vector<std::size_t> creatures;
                for (std::size_t i = 0; i < vfxScene.entities.size(); ++i) {
                    if (vfxScene.entities[i].kind == kotor::EntityKind::Creature && !vfxScene.entities[i].dead) {
                        creatures.push_back(i);
                    }
                }
                std::stable_sort(creatures.begin(), creatures.end(), [&](std::size_t a, std::size_t b) {
                    return kotor::length(vfxScene.entities[a].position - vfxScene.entry)
                        < kotor::length(vfxScene.entities[b].position - vfxScene.entry);
                });
                if (wanted >= 0 && static_cast<std::size_t>(wanted) < creatures.size()) {
                    EffectDirector::Spawn request;
                    request.vfx = std::atoi(options.spawnVfx.c_str());
                    request.target = creatures[static_cast<std::size_t>(wanted)];
                    request.targetObject = game->objectForEntityIndex(request.target);
                    request.permanent = true;
                    // id:n:s: a beam (--spawn-vfx) coming from the sth creature.
                    if (const std::size_t beam = options.spawnVfx.find(':', colon == std::string::npos ? colon : colon + 1);
                        beam != std::string::npos) {
                        const int source = std::atoi(options.spawnVfx.c_str() + beam + 1);
                        if (source >= 0 && static_cast<std::size_t>(source) < creatures.size()) {
                            request.beamSource = creatures[static_cast<std::size_t>(source)];
                        }
                    }
                    game->effects().spawn(request);
                }
            }
            {
                // Spawned creatures and changed weapons brought geometry the GPU has not seen.
                runCaptureActions(*game, *state.backend, options.captureAction, frames);
                const std::size_t meshKey = game->scene().entityMeshes.size() + game->scene().meshRevision;
                if (meshKey != renderedMeshKey && !chargenStage) {
                    renderedMeshKey = meshKey;
                    const auto syncStart = std::chrono::steady_clock::now();
                    state.backend->syncSceneMeshes(&game->scene());
                    logInfo(concat("scene sync: ", std::chrono::duration<double, std::milli>(
                                                       std::chrono::steady_clock::now() - syncStart).count(), " ms"));
                    lastAnimationRevision = game->animationRevision() - 1u;
                }
            }
            if (!wrapGate || wrapGate->finished()) {
                SceneCamera frameCamera = game->camera();
                if (inputScript.active() && inputScript.camera().on) {
                    // The walk's recorded retail camera (walk.tsv cam_eye/cam_fwd/cam_up/cam_fovy).
                    const InputScript::Camera& c = inputScript.camera();
                    frameCamera.eye = {c.eye[0], c.eye[1], c.eye[2]};
                    frameCamera.forward = {c.forward[0], c.forward[1], c.forward[2]};
                    frameCamera.up = {c.up[0], c.up[1], c.up[2]};
                    frameCamera.fovYDegrees = c.fovy;
                }
                if (!options.captureCamera.empty()) {
                    kotor::Vec3 target;
                    float fov = frameCamera.fovYDegrees;
                    if (kotor::captureCameraPose(game->scene(), options.captureCamera, frameCamera.eye, target, fov)) {
                        const kotor::Vec3 toTarget = target - frameCamera.eye;
                        const float toTargetLength = std::max(kotor::length(toTarget), 0.001f);
                        frameCamera.forward = {toTarget.x / toTargetLength, toTarget.y / toTargetLength,
                                               toTarget.z / toTargetLength};
                        frameCamera.up = {0.0f, 0.0f, 1.0f};
                        frameCamera.fovYDegrees = fov;
                        static bool loggedCaptureView = false;
                        if (!loggedCaptureView) {
                            loggedCaptureView = true;
                            const kotor::Scene& shown = game->scene();
                            // What the middle of the view looks at, nearest first (either winding).
                            {
                                const kotor::Vec3 ray = kotor::normalize(target - frameCamera.eye);
                                struct Hit {
                                    float distance;
                                    const kotor::TextureBatch* batch;
                                    std::uint32_t vertex;
                                    float u;
                                    float v;
                                };
                                std::vector<Hit> hits;
                                for (const kotor::TextureBatch& batch : shown.batches) {
                                    for (std::uint32_t i = 0; i + 2u < batch.indexCount; i += 3u) {
                                        const kotor::Vec3 v0 = shown.vertices[shown.indices[batch.firstIndex + i]].position;
                                        const kotor::Vec3 e1 = shown.vertices[shown.indices[batch.firstIndex + i + 1u]].position - v0;
                                        const kotor::Vec3 e2 = shown.vertices[shown.indices[batch.firstIndex + i + 2u]].position - v0;
                                        const kotor::Vec3 cp = kotor::cross(ray, e2);
                                        const float det = kotor::dot(e1, cp);
                                        if (std::abs(det) < 1e-7f) {
                                            continue;
                                        }
                                        const kotor::Vec3 t0 = frameCamera.eye - v0;
                                        const float u = kotor::dot(t0, cp) / det;
                                        const kotor::Vec3 q = kotor::cross(t0, e1);
                                        const float v = kotor::dot(ray, q) / det;
                                        const float d = kotor::dot(e2, q) / det;
                                        if (u >= 0.0f && v >= 0.0f && u + v <= 1.0f && d > 0.0f) {
                                            hits.push_back({det > 0.0f ? d : -d, &batch, batch.firstIndex + i, u, v});
                                        }
                                    }
                                }
                                std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
                                    return std::abs(a.distance) < std::abs(b.distance);
                                });
                                for (std::size_t i = 0; i < hits.size() && i < 4u; ++i) {
                                    const kotor::TextureBatch& batch = *hits[i].batch;
                                    const kotor::SceneVertex& a = shown.vertices[shown.indices[hits[i].vertex]];
                                    const kotor::SceneVertex& b = shown.vertices[shown.indices[hits[i].vertex + 1u]];
                                    const kotor::SceneVertex& c = shown.vertices[shown.indices[hits[i].vertex + 2u]];
                                    const float w = 1.0f - hits[i].u - hits[i].v;
                                    const float lu = a.uv1.x * w + b.uv1.x * hits[i].u + c.uv1.x * hits[i].v;
                                    const float lv = a.uv1.y * w + b.uv1.y * hits[i].u + c.uv1.y * hits[i].v;
                                    int texel = -1;
                                    if (batch.lightmap < shown.textures.size()) {
                                        const kotor::Image& image = shown.textures[batch.lightmap].image;
                                        const int px = std::clamp(static_cast<int>((lu - std::floor(lu)) * static_cast<float>(image.width)), 0, image.width - 1);
                                        const int py = std::clamp(static_cast<int>((lv - std::floor(lv)) * static_cast<float>(image.height)), 0, image.height - 1);
                                        texel = image.rgba[static_cast<std::size_t>(py * image.width + px) * 4u];
                                    }
                                    logInfo(concat("capture hit uv: lightmap uv ", lu, ",", lv, " texel red ", texel,
                                                   " corners ", a.uv1.x, ",", a.uv1.y, " ", b.uv1.x, ",", b.uv1.y));
                                    {
                                        // Why this pixel looks as it does: texels, self illumination, environment.
                                        const auto rgbaAt = [&](std::uint32_t index, float x, float y) {
                                            std::string out = "none";
                                            if (index < shown.textures.size() && !shown.textures[index].image.rgba.empty()) {
                                                const kotor::Image& image = shown.textures[index].image;
                                                const int px = std::clamp(static_cast<int>((x - std::floor(x)) * image.width), 0, image.width - 1);
                                                const int py = std::clamp(static_cast<int>((y - std::floor(y)) * image.height), 0, image.height - 1);
                                                const std::size_t at = static_cast<std::size_t>(py * image.width + px) * 4u;
                                                out = concat(int(image.rgba[at]), ",", int(image.rgba[at + 1]), ",", int(image.rgba[at + 2]), ",", int(image.rgba[at + 3]));
                                            }
                                            return out;
                                        };
                                        const float du = a.uv.x * w + b.uv.x * hits[i].u + c.uv.x * hits[i].v;
                                        const float dv = a.uv.y * w + b.uv.y * hits[i].u + c.uv.y * hits[i].v;
                                        logInfo(concat("capture hit material: diffuse rgba ", rgbaAt(batch.texture, du, dv),
                                                       " lightmap rgba ", rgbaAt(batch.lightmap, lu, lv),
                                                       " selfillum ", a.selfIllum.x, ",", a.selfIllum.y, ",", a.selfIllum.z,
                                                       " env ", batch.envmap < shown.textures.size() ? shown.textures[batch.envmap].name : std::string{"none"},
                                                       " envMask ", batch.envMask ? 1 : 0, " bump rgba ", rgbaAt(batch.bumpmap, du, dv), " normal ", a.normal.x, ",", a.normal.y, ",", a.normal.z));
                                    }
                                    logInfo(concat("capture hit: ", hits[i].distance, " ", shown.textures[batch.texture].name,
                                                   " lightmap ", batch.lightmap < shown.textures.size() ? shown.textures[batch.lightmap].name : std::string{},
                                                   " room ", batch.room, " baked ", batch.bakedLight ? 1 : 0, " ambient ", batch.baked.ambient.x,
                                                   " unlit ", batch.unlit ? 1 : 0, " blend ", static_cast<int>(batch.blend)));
                                }
                            }
                            for (const kotor::TextureBatch& batch : shown.batches) {
                                if (batch.blend != kotor::BlendMode::Opaque || batch.alphaRef >= 0.0f || batch.envMask) {
                                    if (kotor::length(batch.center - target) < 25.0f) {
                                        logInfo(concat("capture batch: ", shown.textures[batch.texture].name, " blend ",
                                                       static_cast<int>(batch.blend), " ref ", batch.alphaRef, " env ",
                                                       batch.envMask ? shown.textures[batch.envmap].name : std::string{},
                                                       " room ", batch.room, " hint ", batch.transparencyHint,
                                                       " tris ", batch.indexCount / 3u));
                                    }
                                }
                            }
                            for (const kotor::EntityMesh& mesh : shown.entityMeshes) {
                                const bool standIn = mesh.entity < shown.entities.size() && mesh.visible
                                    && kotor::length(shown.entities[mesh.entity].position) < 0.01f;
                                if (mesh.entity >= shown.entities.size()
                                    || (!standIn && kotor::length(shown.entities[mesh.entity].position - target) > 9.0f)) {
                                    continue;
                                }
                                std::string textures;
                                for (const kotor::TextureBatch& batch : mesh.batches) {
                                    textures += ' ' + shown.textures[batch.texture].name;
                                }
                                const kotor::Vec3 litAt = shown.entities[mesh.entity].position + shown.entities[mesh.entity].lightOffset;
                                const kotor::SampledLight lit = kotor::sampleSceneLight(shown, litAt);
                                logInfo(concat("capture light: ambient ", lit.ambient.x, ",", lit.ambient.y, ",", lit.ambient.z,
                                               " lights ", lit.lights[0].colour.x, " ", lit.lights[1].colour.x, " ", lit.lights[2].colour.x));
                                for (const kotor::SampledLight::Light& one : lit.lights) {
                                    logInfo(concat("capture light slot: at ", one.position.x, ",", one.position.y, ",", one.position.z,
                                                   " colour ", one.colour.x, ",", one.colour.y, ",", one.colour.z, " quadratic ", one.quadratic));
                                }
                                logInfo(concat("capture view: ", shown.entities[mesh.entity].tag, " at ", shown.entities[mesh.entity].position.x, ",", shown.entities[mesh.entity].position.y, ",", shown.entities[mesh.entity].position.z, " clip '", mesh.animation, "' t ", mesh.animationTime, " visible ", mesh.visible ? 1 : 0, " center ", mesh.center.x, ",", mesh.center.y, ",", mesh.center.z, " ",
                                               mesh.model ? mesh.model->name() : std::string{}, ":", textures));
                            }
                        }
                    }
                }
                kotor::Scene& worldScene = game->scene();
                // The Taris sky box reaches past the 500 a camera defaults to. Retail's far plane
                // (0045c540, camera +0x1e0) has a value that was not traced. A minigame keeps its Far_Clip.
                if (!game->miniGameActive()) {
                    frameCamera.farClip = std::max(frameCamera.farClip, worldScene.viewExtent);
                }
                state.backend->setCamera(frameCamera);
                kotor::updateVisibleRooms(worldScene, frameCamera.eye);
                if (options.captureCamera.rfind("novis:", 0) == 0) {
                    worldScene.visibleRooms.clear(); // Inspecting what the VIS table hides.
                }
                kotor::stepParticles(worldScene, std::min(static_cast<float>(deltaMs / 1000.0), 0.08f), frameCamera.eye);
                kotor::buildParticleMesh(worldScene, frameCamera.eye, frameCamera.forward, frameCamera.up);
                // Grass wind, once a drawn frame (retail 004a6d30 -> 004a68b0); the blades move in place.
                const auto grassMoved = kotor::stepGrassWind(worldScene, static_cast<float>(deltaMs / 1000.0), frameCamera.eye);
                if (grassMoved.second > grassMoved.first) {
                    state.backend->refreshSceneVertices(worldScene, grassMoved.first, grassMoved.second - grassMoved.first);
                }
            }
            if (game->animationRevision() != lastAnimationRevision) {
                lastAnimationRevision = game->animationRevision();
                state.backend->refreshEntityMeshes(game->scene());
            }
        }

        if (state.imguiReady) {
            state.backend->newFrame();
            ImGui_ImplSDL3_NewFrame();
            if (frontEndGate) {
                // The gate's pointer, not the real cursor the backend reads while focused.
                float gateX = 0.0f;
                float gateY = 0.0f;
                if (frontEndGate->pointer(gateX, gateY)) {
                    ImGui::GetIO().AddMousePosEvent(gateX, gateY);
                }
            }
            ImGui::NewFrame();
            if (visuals.menuStaged && game && state.guiSystem && !state.guiSystem->active()) {
                // The front end closed (resume, a script): the world takes the backend back
                // from the main menu's model scene.
                std::string stageError;
                state.backend->setPreviewViews({});
                state.backend->setPreviewScene(nullptr, stageError);
                visuals.menuStaged = false;
                visuals.uploadedRevision = UINT32_MAX;
                state.backend->setScene(&game->scene());
                state.backend->setCamera(game->camera());
            }
            if (gameScreens && game && !game->moviePlaying()) {
                gameScreens->update();
                if (gameScreens->takeOptionsRequest()) {
                    SDL_Event escape{};
                    escape.type = SDL_EVENT_KEY_DOWN;
                    escape.key.key = SDLK_ESCAPE;
                    escape.key.down = true;
                    SDL_PushEvent(&escape); // the pause menu, as the key opens it
                }
            }
            // In-game 3D frames: the party selection's LBL_3D and the character sheet's LBL_3DCHAR
            // show the member's body through the backend's preview pass, over the world.
            if (gameScreens && game && !chargenStage && state.backend->supportsPreview()) {
                const std::string openScreen = gameScreens->open() ? gameScreens->screen() : std::string{};
                int appearance = -1;
                kotor::BodyVariant body;
                // The level-up hub's MODEL_LBL is the same charrec scene (006e8ef0 -> 006100f0).
                if (openScreen == "character" || openScreen == "maincg_leveluppnl") {
                    appearance = static_cast<int>(game->playerAppearance());
                    body = game->wornBodyVariant();
                }
                const bool galaxyShown = openScreen == "galaxymap";
                if (appearance >= 0 || galaxyShown) {
                    if (!chargenPreview) {
                        chargenPreview = std::make_unique<ChargenPreview>(game->install(), game->rules());
                        if (!chargenPreview->ready()) {
                            logWarn(concat("in-game 3D frames unavailable: ", chargenPreview->error()));
                        }
                    }
                    if (chargenPreview && chargenPreview->ready()) {
                        visuals.preview = chargenPreview.get();
                        if (galaxyShown) {
                            chargenPreview->showGalaxy(gameScreens->previewPlanetModel());
                        } else {
                            chargenPreview->showSheet(appearance, body, game->character().alignment);
                        }
                        visuals.syncPreviewScene();
                        if (chargenPreview->advance(static_cast<float>(deltaMs / 1000.0))) {
                            state.backend->refreshPreviewMeshes();
                        }
                        gamePreviewShown = true;
                    }
                } else if (gamePreviewShown) {
                    std::string stageError;
                    state.backend->setPreviewViews({});
                    state.backend->setPreviewScene(nullptr, stageError);
                    visuals.preview = nullptr;
                    visuals.uploadedRevision = UINT32_MAX;
                    gamePreviewShown = false;
                }
            }
            const bool movieMode = game && game->moviePlaying();
            const bool guiMode = state.guiSystem && state.guiSystem->active();
            // Retail 0062ef90 adds the status summary as one more panel over the in-game HUD (0040bc70):
            // the minimap, menu bar, portraits and action bar stay drawn under it (paired walk 0132/0134).
            const bool summaryOverHud = guiMode && game && game->statusSummaryOpen()
                && state.guiSystem->screenName() == "statussummary_fit";
            const bool minigameMode = game && game->pazaakActive();
            if (chargenActive && chargen && game) {
                drawChargenUi(*chargen, *game, chargenActive);
            } else if (!shippedChargen) {
                if (!movieMode && !guiMode && !minigameMode && !(game && game->miniGameActive())
                    && (options.debugUi || !hud)) {
                    drawDebugUi(state, frames, smoothedFrameMs, elapsedSeconds, exitReason);
                }
                if (game) {
                    if (!movieMode && (!guiMode || summaryOverHud) && !minigameMode) {
                        drawGameUi(*game, smoothedFrameMs > 0.0 ? static_cast<float>(1000.0 / smoothedFrameMs) : 0.0f,
                                   game->interactionPrompt(), options.debugUi || !hud);
                        if (hud && hudGui && gGuiPainter != nullptr && !game->miniGameActive()
                            && !game->dialogue().active && options.captureCamera.empty()) {
                            // The shipped HUD over the world: no backdrop, the world is behind it.
                            const ImGuiIO& hudIo = ImGui::GetIO();
                            // ENGINE CHOICE: the HUD is not scaled to fit like a menu. The 800x600
                            // file is drawn at a scale of window height / 720 (at least 1), and each
                            // control keeps its distance from the window edge it is nearest to (party
                            // and portraits at the bottom left, action bar at the bottom right,
                            // minimap at the top left and menu bar at the top right).
                            float designW = 0.0f;
                            float designH = 0.0f;
                            const std::string wantedLayout = Hud::layoutFor(hudIo.DisplaySize.x, hudIo.DisplaySize.y, designW, designH);
                            traceHudLayout = wantedLayout;
                            if (hudGui->screenName() != wantedLayout) {
                                hudGui->open(wantedLayout);
                            }
                            hud->setView(hudIo.DisplaySize.x, hudIo.DisplaySize.y,
                                         Hud::layoutScale(hudIo.DisplaySize.x, hudIo.DisplaySize.y), true);
                            hud->update(ImGui::GetTime(), userPause);
                            GuiPaintOptions hudOptions;
                            hudOptions.opaqueBackdrop = false;
                            hudOptions.timeSeconds = ImGui::GetTime();
                            const float hudScale = Hud::layoutScale(hudIo.DisplaySize.x, hudIo.DisplaySize.y);
                            GuiView hudLayout;
                            hudLayout.scale = hudScale;
                            hudLayout.baseWidth = designW;
                            hudLayout.baseHeight = designH;
                            hudLayout.windowWidth = hudIo.DisplaySize.x;
                            hudLayout.windowHeight = hudIo.DisplaySize.y;
                            hudLayout.anchored = true;
                            hudOptions.view = hudLayout;
                            hudPickView = hudLayout;
                            // The pointer drives the HUD while it is free (look not captured).
                            if (!mouseRelative && !lookHeld && !summaryOverHud) {
                                // A press that lands on no HUD button is a click on the world (00620350).
                                if (worldKeys() && hudPickAt && ImGui::IsMousePosValid()
                                    && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                                    && hudPickAt(hudIo.MousePos.x, hudIo.MousePos.y).empty()
                                    && hudIo.DisplaySize.x > 0.0f && hudIo.DisplaySize.y > 0.0f) {
                                    game->clickWorld(hudIo.MousePos.x / hudIo.DisplaySize.x * 2.0f - 1.0f,
                                                     1.0f - hudIo.MousePos.y / hudIo.DisplaySize.y * 2.0f,
                                                     hudIo.DisplaySize.x / hudIo.DisplaySize.y);
                                }
                                pumpGuiPointer(*hudGui);
                                const std::string hudAction = hudGui->lastAction();
                                if (!hudAction.empty()) {
                                    hudGui->clearActions();
                                    switch (hud->activate(hudAction)) {
                                    case Hud::Request::Pause: {
                                        SDL_Event escape{};
                                        escape.type = SDL_EVENT_KEY_DOWN;
                                        escape.key.key = SDLK_ESCAPE;
                                        escape.key.down = true;
                                        SDL_PushEvent(&escape); // the pause menu, as the key opens it
                                        break;
                                    }
                                    case Hud::Request::TogglePause: userPause = !userPause; pauseReason = 4; break;
                                    case Hud::Request::Map: game->toggleUiPanel(UiPanel::Map); break;
                                    case Hud::Request::None: break;
                                    }
                                }
                            }
                            // The portraits are drawn after the screen: the frame (LBL_BACKn) is
                            // listed after the portrait control and would cover it.
                            struct HudPortrait {
                                std::uint64_t texture;
                                ImVec2 low;
                                ImVec2 high;
                            };
                            std::vector<HudPortrait> hudPortraits;
                            std::vector<HudPortrait> hudBadges; // drawn over the portraits
                            // The minimap: LBL_MAPVIEW is the window, LBL_MAP (512x512 in the
                            // file) the area's lbl_map<area> texture behind it: one texel per design
                            // pixel, centred on the leader and clipped to the window, as 0068ab10 does
                            // it (z = 1 / MapZoom, and MapZoom is 1 in all 117 shipped AREs).
                            // Retail tiles the 512x256 art and draws the fog of war.
                            // LBL_MAPVIEW comes after LBL_MAP in the shipped file, so its window is
                            // taken from where the painter will put it, not from the paint order.
                            std::array<float, 4> mapWindow = {0.0f, 0.0f, 0.0f, 0.0f};
                            if (const kotor::GuiControl* mapView = hudGui->control("LBL_MAPVIEW")) {
                                const GuiPixelRect box = guiControlPixels(hudLayout, *mapView);
                                mapWindow = {box.x0, box.y0, box.x1, box.y1};
                            }
                            hudOptions.controlHook = [&](const kotor::GuiControl& control, float x0, float y0,
                                                         float x1, float y1) {
                                if (control.tag == "LBL_ARROW") {
                                    // The leader's mark turns with the way the leader faces: the
                                    // arrow art points up, turned to the facing in map space.
                                    const GuiTexture* art = gGuiPainter->texture(control.border.fill);
                                    float u0 = 0.0f;
                                    float v0 = 0.0f;
                                    float u1 = 0.0f;
                                    float v1 = 0.0f;
                                    const kotor::Vec3 at = game->position();
                                    // Retail 00578ed0 turns the arrow by the camera's forward (the object
                                    // whose +0x28 also projects to the screen, 00688e10), not the leader's.
                                    // A walk's recorded camera (the `camera` step) is the one the arrow follows.
                                    kotor::Vec3 forward = game->camera().forward;
                                    if (inputScript.active() && inputScript.camera().on) {
                                        forward = kotor::Vec3{inputScript.camera().forward[0], inputScript.camera().forward[1],
                                                              inputScript.camera().forward[2]};
                                    }
                                    const float yaw = std::atan2(forward.y, forward.x);
                                    if (art != nullptr
                                        && game->scene().minimap.project(at.x, at.y, u0, v0)
                                        && game->scene().minimap.project(at.x + std::cos(yaw), at.y + std::sin(yaw), u1, v1)) {
                                        const float turn = std::atan2(v1 - v0, u1 - u0) + 1.5707963f; // texel space
                                        // The HUD constructor overrides the file: 32x32 at (44,44) in the
                                        // 120x120 LBL_MAPVIEW, its centre.
                                        const float cx = mapWindow[2] > mapWindow[0] ? 0.5f * (mapWindow[0] + mapWindow[2]) : 0.5f * (x0 + x1);
                                        const float cy = mapWindow[3] > mapWindow[1] ? 0.5f * (mapWindow[1] + mapWindow[3]) : 0.5f * (y0 + y1);
                                        const float hx = 0.5f * (x1 - x0);
                                        const float hy = 0.5f * (y1 - y0);
                                        const float c = std::cos(turn);
                                        const float sn = std::sin(turn);
                                        const auto corner = [&](float lx, float ly) {
                                            return ImVec2(cx + lx * c - ly * sn, cy + lx * sn + ly * c);
                                        };
                                        ImGui::GetBackgroundDrawList()->AddImageQuad(
                                            static_cast<ImTextureID>(art->id), corner(-hx, -hy), corner(hx, -hy),
                                            corner(hx, hy), corner(-hx, hy), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1),
                                            ImVec2(0, 1), IM_COL32_WHITE);
                                        return true;
                                    }
                                    return false;
                                }
                                if (control.tag == "LBL_MAP") {
                                    float u = 0.0f;
                                    float v = 0.0f;
                                    const kotor::Vec3 at = game->position();
                                    const GuiTexture* art = gGuiPainter->texture("lbl_map" + game->scene().area);
                                    if (art != nullptr && mapWindow[2] > mapWindow[0]
                                        && game->scene().minimap.project(at.x, at.y, u, v)) {
                                        ImDrawList* draw = ImGui::GetBackgroundDrawList();
                                        const float texel = hudScale;
                                        const float cx = 0.5f * (mapWindow[0] + mapWindow[2]);
                                        const float cy = 0.5f * (mapWindow[1] + mapWindow[3]);
                                        // (u, v) are texels of the map art (design pixels): the leader's texel sits
                                        // at the window's centre.
                                        const float left = cx - u * texel;
                                        const float top = cy - v * texel;
                                        draw->PushClipRect(ImVec2(mapWindow[0], mapWindow[1]),
                                                           ImVec2(mapWindow[2], mapWindow[3]), true);
                                        // ENGINE CHOICE: black under the art, so the window past the
                                        // texture's edge is not the world seen through; the shipped
                                        // lbl_map textures are black where nothing is drawn.
                                        draw->AddRectFilled(ImVec2(mapWindow[0], mapWindow[1]),
                                                            ImVec2(mapWindow[2], mapWindow[3]), IM_COL32(0, 0, 0, 255));
                                        // Retail 00687e40 clears the control's clamp bits (+0x5e8c): the art repeats past its edge.
                                        const float artW = static_cast<float>(art->width) * texel;
                                        const float artH = static_cast<float>(art->height) * texel;
                                        for (int tileY = -1; tileY <= 1; ++tileY) {
                                            for (int tileX = -1; tileX <= 1; ++tileX) {
                                                const ImVec2 low(left + static_cast<float>(tileX) * artW, top + static_cast<float>(tileY) * artH);
                                                draw->AddImage(static_cast<ImTextureID>(art->id), low, ImVec2(low.x + artW, low.y + artH));
                                            }
                                        }
                                        drawMapFog(*game, draw, left, top, texel);
                                        draw->PopClipRect();
                                    }
                                    return true; // the 512x512 control itself is never stretched
                                }
                                if (control.tag.rfind("LBL_LEVELUP", 0) == 0) {
                                    // The level-up mark goes over the portrait it belongs to.
                                    if (const GuiTexture* art = gGuiPainter->texture(control.border.fill)) {
                                        hudBadges.push_back({art->id, ImVec2(x0, y0), ImVec2(x1, y1)});
                                    }
                                    return true;
                                }
                                const std::string portrait = hud->portraitFor(control.tag);
                                if (portrait.empty()) {
                                    return false;
                                }
                                const std::uint64_t texture = visuals.portraitTexture(portrait);
                                if (texture != 0) {
                                    hudPortraits.push_back({texture, ImVec2(x0, y0), ImVec2(x1, y1)});
                                }
                                return true;
                            };
                            // The target reticle (0068a310's code-built control +0x5a2c, turned by +0x5aa4).
                            // NOT TRACED: its draw order against mipc2*.gui; it goes under the HUD here.
                            // ENGINE CHOICE: its retail pixel sizes are scaled with the HUD (1:1 at 1024x768).
                            if (const Hud::Reticle& reticle = hud->reticle(); reticle.visible) {
                                if (const GuiTexture* art = gGuiPainter->texture(reticle.art)) {
                                    ImDrawList* draw = ImGui::GetBackgroundDrawList();
                                    const float half = 0.5f * reticle.size * hudScale;
                                    const float cx = (reticle.x + 0.5f * reticle.size) * hudScale;
                                    const float cy = (reticle.y + 0.5f * reticle.size) * hudScale;
                                    const float turn = reticle.angleDegrees * 3.14159265f / 180.0f;
                                    const float c = std::cos(turn);
                                    const float s = std::sin(turn);
                                    const auto corner = [&](float ux, float uy) {
                                        return ImVec2(cx + ux * c - uy * s, cy + ux * s + uy * c);
                                    };
                                    gGuiPainter->beginArt(draw, *art);
                                    draw->AddImageQuad(static_cast<ImTextureID>(art->id), corner(-half, -half), corner(half, -half),
                                                       corner(half, half), corner(-half, half));
                                    gGuiPainter->endArt(draw, *art);
                                }
                            }
                            gGuiPainter->paint(*hudGui, ImGui::GetBackgroundDrawList(), hudIo.DisplaySize.x,
                                               hudIo.DisplaySize.y, hudOptions);
                            hudStats = gGuiPainter->stats();
                            hudViewUsed = gGuiPainter->view();
                            paintFloatingText(*game, ImGui::GetBackgroundDrawList(), hudIo.DisplaySize.x, hudIo.DisplaySize.y, hudScale);
                            for (const HudPortrait& portrait : hudPortraits) {
                                ImGui::GetBackgroundDrawList()->AddImage(static_cast<ImTextureID>(portrait.texture),
                                                                         portrait.low, portrait.high);
                            }
                            for (const HudPortrait& badge : hudBadges) {
                                ImGui::GetBackgroundDrawList()->AddImage(static_cast<ImTextureID>(badge.texture),
                                                                         badge.low, badge.high);
                            }
                            // The pause panel (CGuiInGame::SetPaused 0062def0 adds pause.gui for reasons 1, 4, 5,
                            // 7-11). CSWGuiInGamePause::SetPauseReason (006c00c0) picks the line: 1 48212 enemy
                            // sighted, 5 42432 end of round, 7 42482 menu used, 8 42481 target changed, 9 42397
                            // party member down, 10 48423 action queued, 11 49118 mine sighted, else 1508 PAUSED;
                            // LBL_PRESS (48384) shows except for 1 and 11. CGuiInGame::SetupPauseGuiExtent
                            // (0062b100 -> 00688db0) puts the panel 3 px from the right edge, 3 px under the
                            // menu bar (or 3 px from the top with the quick menu hidden).
                            if (userPause && pauseGui && pauseGui->screen() != nullptr) {
                                std::uint32_t reasonStrref = 1508;
                                switch (pauseReason) {
                                case 1: reasonStrref = 48212; break;
                                case 5: reasonStrref = 42432; break;
                                case 7: reasonStrref = 42482; break;
                                case 8: reasonStrref = 42481; break;
                                case 9: reasonStrref = 42397; break;
                                case 10: reasonStrref = 48423; break;
                                case 11: reasonStrref = 49118; break;
                                default: break;
                                }
                                pauseGui->setText("LBL_PAUSEREASON",
                                                  game->substituteTokens(game->localization().tlk().text(reasonStrref)));
                                const bool pressShown = pauseReason != 1 && pauseReason != 11;
                                pauseGui->setVisible("LBL_PRESS", pressShown);
                                // 006c00c0 sizes the panel to its text: the reason label as tall as its
                                // wrapped text, LBL_PRESS 2 below it as tall as its own, the panel 5 below
                                // the last one, BTN_UNPAUSE over the panel (the file's 2 px inset kept).
                                if (kotor::GuiScreen* pausePanel = pauseGui->editableScreen();
                                    pausePanel != nullptr && gGuiPainter != nullptr) {
                                    const GuiFont* pauseFont = gGuiPainter->font("fnt_d16x16");
                                    kotor::GuiControl* reasonLabel = nullptr;
                                    kotor::GuiControl* pressLabel = nullptr;
                                    kotor::GuiControl* unpause = nullptr;
                                    for (kotor::GuiControl& control : pausePanel->controls) {
                                        reasonLabel = control.tag == "LBL_PAUSEREASON" ? &control : reasonLabel;
                                        pressLabel = control.tag == "LBL_PRESS" ? &control : pressLabel;
                                        unpause = control.tag == "BTN_UNPAUSE" ? &control : unpause;
                                    }
                                    const auto textHeight = [&](const std::string& text, int width) {
                                        // Greedy word wrap at the label's width; each \n starts a line.
                                        int lines = 0;
                                        std::size_t start = 0;
                                        while (start <= text.size()) {
                                            const std::size_t end = std::min(text.find('\n', start), text.size());
                                            std::string line;
                                            int count = 1;
                                            std::size_t word = start;
                                            while (word < end) {
                                                std::size_t next = text.find(' ', word);
                                                next = next == std::string::npos || next > end ? end : next;
                                                const std::string piece = text.substr(word, next - word);
                                                const std::string tried = line.empty() ? piece : line + " " + piece;
                                                if (!line.empty() && pauseFont->textWidth(tried) > static_cast<float>(width)) {
                                                    ++count;
                                                    line = piece;
                                                } else {
                                                    line = tried;
                                                }
                                                word = next + 1;
                                            }
                                            lines += count;
                                            start = end + 1;
                                        }
                                        return static_cast<int>(std::ceil(static_cast<float>(lines) * pauseFont->height));
                                    };
                                    if (pauseFont != nullptr && reasonLabel != nullptr && pressLabel != nullptr) {
                                        reasonLabel->extent.height = textHeight(pauseGui->state("LBL_PAUSEREASON").text,
                                                                                reasonLabel->extent.width);
                                        int bottom = reasonLabel->extent.top + reasonLabel->extent.height;
                                        if (pressShown) {
                                            pressLabel->extent.top = bottom + 2;
                                            pressLabel->extent.height = textHeight(
                                                game->localization().tlk().text(48384), pressLabel->extent.width);
                                            bottom = pressLabel->extent.top + pressLabel->extent.height;
                                        }
                                        pausePanel->extent.height = bottom + 5;
                                        if (unpause != nullptr) {
                                            unpause->extent.height = pausePanel->extent.height - 2 * unpause->extent.top;
                                        }
                                    }
                                }
                                GuiView pauseView = gGuiPainter->layout(*pauseGui->screen(), hudIo.DisplaySize.x,
                                                                        hudIo.DisplaySize.y);
                                const float panelW = static_cast<float>(pauseGui->screen()->extent.width);
                                float top = 3.0f * hudScale;
                                if ((game->feedbackOptions() & 0x200u) == 0u) {
                                    if (const kotor::GuiControl* menuBar = hudGui->control("LBL_MENUBG")) {
                                        top = guiControlPixels(hudLayout, *menuBar).y1 + 2.0f * hudScale;
                                    }
                                }
                                pauseView.scale = hudScale;
                                pauseView.offsetX = hudIo.DisplaySize.x - (panelW + 3.0f) * hudScale - pauseView.panelLeft * hudScale;
                                pauseView.offsetY = top - pauseView.panelTop * hudScale;
                                if (!mouseRelative && !lookHeld) {
                                    pumpGuiInput(*pauseGui, pauseView);
                                }
                                GuiPaintOptions pauseOptions;
                                pauseOptions.view = pauseView;
                                pauseOptions.opaqueBackdrop = false;
                                pauseOptions.timeSeconds = ImGui::GetTime();
                                gGuiPainter->paint(*pauseGui, ImGui::GetBackgroundDrawList(), hudIo.DisplaySize.x,
                                                   hudIo.DisplaySize.y, pauseOptions);
                                if (pauseGui->lastAction() == "BTN_UNPAUSE") { // 006c0350 OnUnpause
                                    userPause = false;
                                }
                                pauseGui->clearActions();
                            }
                            // The pointer's box goes over every HUD panel, the pause panel included.
                            // NOT TRACED: retail's draw order for 00624af0 against pause.gui; a tooltip under
                            // a panel's text is unreadable, so it is drawn last.
                            if (!mouseRelative && !lookHeld) {
                                hudTooltipShown = hud->tooltip();
                                paintTooltip(tooltipGui.get(), hudTooltipShown, ImGui::GetBackgroundDrawList(), hudIo.DisplaySize.x, hudIo.DisplaySize.y, hudScale);
                            }
                            // What E would act on. Retail's HUD has no such line (the in-game panel 00686ba0 and
                            // its controls carry no prompt text), so it is drawn for the developer view only.
                            const std::string interact = options.debugUi ? game->interactionPrompt() : std::string{};
                            if (!interact.empty()) {
                                if (const GuiFont* promptFont = gGuiPainter->font("fnt_d16x16")) {
                                    gGuiPainter->drawText(ImGui::GetBackgroundDrawList(), *promptFont, interact,
                                                          hudIo.DisplaySize.x * 0.5f - 200.0f * hudScale,
                                                          hudIo.DisplaySize.y - 190.0f * hudScale,
                                                          hudIo.DisplaySize.x * 0.5f + 200.0f * hudScale,
                                                          hudIo.DisplaySize.y - 160.0f * hudScale, 18,
                                                          IM_COL32(255, 220, 90, 255), hudScale, true);
                                }
                            }
                        }
                        // BarkString's box, over the HUD and over the 3D minigames (the turret's
                        // "Incoming fighters!"). NOT TRACED: its draw order against the dialog screen.
                        if (hud && barkGui && gGuiPainter != nullptr && options.captureCamera.empty()) {
                            const ImGuiIO& barkIo = ImGui::GetIO();
                            hud->updateBark(ImGui::GetTime());
                            if (hud->bark().visible) {
                                paintBark(barkGui.get(), hud->bark().text, ImGui::GetBackgroundDrawList(), barkIo.DisplaySize.x,
                                          barkIo.DisplaySize.y, Hud::layoutScale(barkIo.DisplaySize.x, barkIo.DisplaySize.y));
                            }
                        }
                        if (dialogueScreen && dialogueGui && gGuiPainter != nullptr && game->dialogue().active
                            && !options.debugUi && options.captureCamera.empty()) {
                            // The conversation: the shipped dialog.gui between two letterbox bars.
                            dialogueScreen->update();
                            // A Computer conversation is the terminal screen, drawn whole over the world;
                            // any other is dialog.gui between two letterbox bars.
                            GuiSystem& panelGui = dialogueScreen->activeGui();
                            const bool terminal = dialogueScreen->computerShown();
                            const ImGuiIO& dialogueIo = ImGui::GetIO();
                            // The live feed (computercamera.gui) takes the window's size (006a95f0).
                            const bool liveFeed = dialogueScreen->cameraFeedShown();
                            if (liveFeed) {
                                dialogueScreen->fitFeed(static_cast<int>(dialogueIo.DisplaySize.x),
                                                        static_cast<int>(dialogueIo.DisplaySize.y));
                            }
                            GuiView dialogueView = gGuiPainter->layout(*panelGui.screen(),
                                                                       dialogueIo.DisplaySize.x,
                                                                       dialogueIo.DisplaySize.y);
                            // RETAIL-PROVEN (006a8b40, 006a78c0): the world shows in a band
                            // trunc(W*3/7) high, centred; the reply panel sits under it at x 48
                            // (00409ee0). Paired frame dlg_static: band 165..603 at 1024x768.
                            const float band = std::trunc(dialogueIo.DisplaySize.x * 3.0f / 7.0f);
                            const float barTop = std::floor((dialogueIo.DisplaySize.y - band) * 0.5f);
                            const float barBottom = std::floor((dialogueIo.DisplaySize.y + band) * 0.5f);
                            if (!terminal && dialogueView.scale == 1.0f) {
                                dialogueScreen->fitWidth(static_cast<int>(dialogueIo.DisplaySize.x));
                                dialogueView.offsetX = 48.0f - dialogueView.panelLeft;
                                dialogueView.offsetY = barBottom - dialogueView.panelTop;
                            }
                            if (!dialogueEnterConsumed) {
                                pumpGuiInput(panelGui, dialogueView);
                            }
                            dialogueEnterConsumed = false;
                            ImDrawList* dialogueDraw = ImGui::GetBackgroundDrawList();
                            const ImU32 black = IM_COL32(0, 0, 0, 255);
                            if (!terminal && dialogueView.scale == 1.0f) {
                                dialogueDraw->AddRectFilled(ImVec2(0.0f, 0.0f),
                                                            ImVec2(dialogueIo.DisplaySize.x, barTop), black);
                                dialogueDraw->AddRectFilled(ImVec2(0.0f, barBottom), dialogueIo.DisplaySize, black);
                            } else if (!terminal) {
                                dialogueDraw->AddRectFilled(ImVec2(0.0f, 0.0f),
                                                            ImVec2(dialogueIo.DisplaySize.x,
                                                                   dialogueView.offsetY
                                                                       + DialogueScreen::kBarHeight * dialogueView.scale),
                                                            black);
                                dialogueDraw->AddRectFilled(ImVec2(0.0f, dialogueView.offsetY
                                                                             + (dialogueView.baseHeight
                                                                                - DialogueScreen::kBarHeight)
                                                                                 * dialogueView.scale),
                                                            dialogueIo.DisplaySize, black);
                            }
                            GuiPaintOptions dialogueOptions;
                            if (!terminal) {
                                dialogueOptions.view = dialogueView;
                            } else if (!liveFeed) {
                                // 006a6e20: "<W>x<H>comp" plus the conversation's computer type
                                // (006a8400 via 00628670). Only type 0 is read here: the DLG's
                                // ComputerType field is not loaded, so an ancient terminal gets comp0.
                                dialogueOptions.backdropKind = "comp0";
                            }
                            dialogueOptions.opaqueBackdrop = false;
                            // The live feed is the world through the placeable's camera: no surround.
                            dialogueOptions.windowBackdrop = !liveFeed;
                            dialogueOptions.timeSeconds = ImGui::GetTime();
                            gGuiPainter->paint(panelGui, dialogueDraw, dialogueIo.DisplaySize.x,
                                               dialogueIo.DisplaySize.y, dialogueOptions);
                            if (!terminal && dialogueView.scale == 1.0f) {
                                dialogueScreen->paintTop(dialogueDraw, dialogueIo.DisplaySize.x, barTop);
                            }
                            // A click is the same skip as Enter: CSWGuiDialog::HandleInputEvent 006a7230 calls
                            // SkipPastDialog for the Enter keys (0x27, 0x2d) and for input 0x1f9 alike while the line
                            // plays; retail ccSc_timeline.json shows the feed gone 134 ms after the click mark.
                            if (liveFeed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                dialogueScreen->cancelFeed();
                            }
                            if (!panelGui.lastAction().empty()) {
                                const std::string dialogueAction = panelGui.lastAction();
                                panelGui.clearActions();
                                dialogueScreen->action(dialogueAction);
                            }
                        }
                        // The rig panel is a debugging view, not part of a conversation.
                        if (options.facialDemo >= 0 || (options.debugUi && game->lipSyncActive())) {
                            drawFacialRigOverlay(game->facialRig());
                        }
                    }
                }
            }
            if (game) {
                if (game->moviePlaying() && game->movieFrame() != nullptr) {
                    if (game->movieFrameIndex() != uploadedMovieFrame
                        || game->movieName() != uploadedMovieName) {
                        std::string movieError;
                        const kotor::BinkVideoFrame* frame = game->movieFrame();
                        if (state.backend->uploadMovieFrame(
                                frame->width, frame->height, frame->rgba.data(),
                                movieTexture, movieError)) {
                            uploadedMovieFrame = game->movieFrameIndex();
                            uploadedMovieName = game->movieName();
                        } else if (!movieUploadWarned) {
                            logWarn(concat("Movie overlay unavailable: ", movieError));
                            movieUploadWarned = true;
                        }
                    }
                    if (movieTexture != 0) {
                        drawMovieOverlay(*game->movieFrame(), movieTexture);
                    }
                }
            }
            if (game && !shippedChargen && !guiMode && !game->moviePlaying()) {
                // A movie owns the screen: the world's fade (m01aa starts from solid black)
                // must not be painted over it.
                // The module's own fade (m01aa starts from black) is not part of chargen,
                // which has no module behind it.
                drawWorldStateOverlay(*game);
            }
            if (game && gGuiPainter != nullptr && game->creditsActive() && !game->moviePlaying()
                && options.captureCamera.empty()) {
                // The credit sequence (credits.2da pages): one page at a time fading in and out
                // over the world, the last scrolling up. The text sits where credits.gui puts
                // its list (140,40, 358x400).
                const ImGuiIO& creditsIo = ImGui::GetIO();
                ImDrawList* creditsDraw = ImGui::GetForegroundDrawList();
                if (!game->creditsTransparent()) {
                    creditsDraw->AddRectFilled(ImVec2(0.0f, 0.0f), creditsIo.DisplaySize, IM_COL32(0, 0, 0, 255));
                }
                const float scale = std::min(creditsIo.DisplaySize.x / 640.0f, creditsIo.DisplaySize.y / 480.0f);
                const float offsetX = 0.5f * (creditsIo.DisplaySize.x - 640.0f * scale);
                const float offsetY = 0.5f * (creditsIo.DisplaySize.y - 480.0f * scale);
                const std::int32_t strref = game->creditsPageStrref();
                const GuiFont* creditsFont = gGuiPainter->font("fnt_d16x16");
                if (strref >= 0 && creditsFont != nullptr) {
                    const std::string page = game->localization().tlk().text(static_cast<std::uint32_t>(strref));
                    const unsigned alpha = static_cast<unsigned>(std::clamp(game->creditsOpacity(), 0.0f, 1.0f) * 255.0f);
                    float top = 40.0f;
                    float bottom = 440.0f;
                    if (game->creditsOnLastPage()) {
                        // The page rises from the bottom edge to past the top.
                        const float rise = game->creditsScrollProgress();
                        const float lines = std::max(1.0f, creditsFont->height * 40.0f);
                        top = 440.0f - rise * (400.0f + lines);
                        bottom = top + lines;
                    }
                    gGuiPainter->drawText(creditsDraw, *creditsFont, page, offsetX + 140.0f * scale,
                                          offsetY + top * scale, offsetX + 498.0f * scale,
                                          offsetY + bottom * scale, game->creditsOnLastPage() ? 9 : 18,
                                          IM_COL32(255, 255, 255, alpha == 0 ? 0 : alpha), scale, true);
                }
            }
            if (chargenStage && chargenGui && !chargenGui->active) {
                // The flow handed the character to the session: leave the stage.
                std::string stageError;
                state.backend->setPreviewViews({});
                state.backend->setPreviewScene(nullptr, stageError);
                visuals.preview = nullptr;
                state.backend->setScene(&game->scene());
                state.backend->setCamera(game->camera());
                chargenStage = false;
            }
            if (newGameFlow && chargenGui && !chargenGui->active) {
                newGameFlow = false;
                if (chargenGui->finished) {
                    // Retail enters end_m01aa straight after the name screen; no movie is
                    // played between chargen and the module (nothing in 0067ace0 or the
                    // chargen handlers queues one).
                    // A new game: the menu's backdrop module is not carried into it.
                    game->startNewGame("end_m01aa");
                } else if (state.guiSystem) {
                    // BTN_BACK on classsel returns to the main menu.
                    state.guiSystem->open("mainmenu");
                }
            }
            if (chargenStage && chargenGui && chargenGui->active && visuals.preview != nullptr) {
                const Chargen& chargenState = chargenGui->chargen;
                if (chargenGui->step == 0) {
                    visuals.preview->showClassSelection(game->rules());
                } else if (chargenGui->step == 1) {
                    visuals.preview->showHead(chargenState.appearanceId(), chargenState.gender());
                } else if (chargenGui->step == kChargenHub) {
                    // MAINCG's MODEL_LBL: the body in charrec_light (006eb420 -> 006100f0), as the sheet.
                    visuals.preview->showSheet(chargenState.appearanceId(), chargenBodyVariant(), chargenState.build().alignment);
                }
                visuals.syncPreviewScene();
                if (visuals.preview->advance(static_cast<float>(deltaMs / 1000.0))) {
                    state.backend->refreshPreviewMeshes();
                }
                visuals.portraitResref = game->rules().portraitResrefFor(
                    chargenState.portrait(), chargenState.build().alignment);
            }
            if (!movieMode && guiMode && game) {
                std::function<bool(const std::string&)> handler;
                if (chargenGui) {
                    bindChargenScreen(*state.guiSystem, *chargenGui, *game);
                    handler = [&](const std::string& action) {
                        return handleChargenGuiAction(action, *chargenGui,
                                                      *state.guiSystem, *game);
                    };
                }
                drawGuiScreenOverlay(*state.guiSystem, *game, state.requestChargen, handler,
                                     &visuals);
                if (chargenGui && state.window != nullptr) {
                    // The name screen is typed into: text input stays on while it is up.
                    const bool typing = chargenNameEditing(*chargenGui);
                    if (typing != SDL_TextInputActive(state.window)) {
                        if (typing) {
                            SDL_StartTextInput(state.window);
                        } else {
                            SDL_StopTextInput(state.window);
                        }
                    }
                }
            }
            if (frontEnd && game && state.guiSystem) {
                FrontEndOutcome outcome = frontEnd->takeOutcome();
                if (outcome.newGame) {
                    state.guiSystem->close();
                    state.requestChargen = true;
                }
                if (outcome.quit) {
                    exitReason = "GUI exit";
                    running = false;
                }
                if (!outcome.playMovie.empty() && game->playMovie(outcome.playMovie)) {
                    userSettings->seenMovies.insert(outcome.playMovie);
                    userSettings->dirty = true;
                }
                if (!outcome.loadFolder.empty()) {
                    if (game->loadOriginalSave(outcome.loadFolder, true)) {
                        // A loaded game is played as the PC: the avatar and its follow camera, as
                        // after chargen. A session started without --third-person (the walk's
                        // --module runs, the main menu) otherwise kept the free camera and no PC
                        // model (WALK_DIFFS W29/W38).
                        game->setThirdPerson(true);
                        state.guiSystem->close();
                        state.backend->setPreviewViews({});
                        std::string stageError;
                        state.backend->setPreviewScene(nullptr, stageError);
                        visuals.menuStaged = false;
                        state.backend->setScene(&game->scene());
                        state.backend->setCamera(game->camera());
                    } else {
                        logWarn(concat("load failed: ", outcome.loadFolder.string()));
                    }
                }
                if (outcome.resumeGame) {
                    state.guiSystem->close();
                }
                if ((outcome.displayWidth > 0 || outcome.fullscreen >= 0) && applyDisplay) {
                    applyDisplay(outcome.displayWidth, outcome.displayHeight, outcome.fullscreen);
                }
                if (!outcome.saveFolder.empty()) {
                    if (game->saveOriginalSave(outcome.saveFolder, outcome.saveName)) {
                        state.guiSystem->pop();
                    } else {
                        logWarn(concat("save failed: ", outcome.saveFolder.string()));
                    }
                }
                if (userSettings && userSettings->dirty) {
                    userSettings->save(settingsPath);
                    userSettings->dirty = false;
                }
            }
            if (state.requestChargen && game) {
                // BTN_NEWGAME: the shipped classsel flow, by real GuiSystem events.
                state.requestChargen = false;
                if (state.guiSystem && !(chargenGui && chargenGui->active)) {
                    chargenGui = std::make_unique<ChargenGuiState>(game->rules());
                    newGameFlow = true;
                    setUpChargenStage();
                    state.guiSystem->open("classsel");
                }
            }
            if (!movieMode && state.guiSystem && state.guiSystem->quitRequested()) {
                exitReason = "GUI exit";
                running = false;
            }
            if (frontEndGate && !frontEndGate->artProbe().empty() && state.guiPainter) {
                // The gate's art probe: a bright grey field over everything, then each texture as
                // the painter draws it, where FrontEndGate::probeCell says.
                const ImGuiIO& probeIo = ImGui::GetIO();
                ImDrawList* probeDraw = ImGui::GetForegroundDrawList();
                probeDraw->AddRectFilled(ImVec2(0.0f, 0.0f), probeIo.DisplaySize,
                                         IM_COL32(FrontEndGate::kProbeField, FrontEndGate::kProbeField,
                                                  FrontEndGate::kProbeField, 255));
                const std::vector<std::string>& probe = frontEndGate->artProbe();
                for (std::size_t i = 0; i < probe.size(); ++i) {
                    const GuiTexture* art = state.guiPainter->texture(probe[i]);
                    if (art == nullptr) {
                        continue;
                    }
                    float cx = 0.0f;
                    float cy = 0.0f;
                    FrontEndGate::probeCell(i, probeIo.DisplaySize.x, cx, cy);
                    const float fit = std::min(1.0f, FrontEndGate::kProbeBox
                                                         / static_cast<float>(std::max(art->width, art->height)));
                    state.guiPainter->drawProbe(probeDraw, probe[i], cx, cy, cx + static_cast<float>(art->width) * fit,
                                                cy + static_cast<float>(art->height) * fit);
                }
            }
            ImGui::Render();
            if (!options.captureCamera.empty() && ImGui::GetDrawData() != nullptr) {
                ImGui::GetDrawData()->Clear();
            }

            const ImDrawData* drawData = ImGui::GetDrawData();
            if (!loggedUiGeometry && drawData != nullptr && drawData->TotalVtxCount > 0) {
                logInfo(concat("ImGui draw data at frame ", frames, ": ",
                               drawData->CmdListsCount, " list(s), ", drawData->TotalVtxCount,
                               " vertices"));
                loggedUiGeometry = true;
            }
        }

        const std::uint64_t warmup = static_cast<std::uint64_t>(options.videoEffectWarmup);
        const bool inEffectSequence = frames >= warmup && frames - warmup < videoEffectSequence.size();
        const std::size_t sequenceIndex = inEffectSequence ? static_cast<std::size_t>(frames - warmup) : 0;
        if (game && inEffectSequence && !options.videoEffectFixtures.empty()
            && videoEffectSequence[sequenceIndex] != "keep") {
            const std::filesystem::path fixture = std::filesystem::path(options.videoEffectFixtures)
                / ("video_effect_" + videoEffectSequence[sequenceIndex] + ".ncs");
            std::ifstream fixtureFile(fixture, std::ios::binary | std::ios::ate);
            if (fixtureFile) {
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fixtureFile.tellg()));
                fixtureFile.seekg(0);
                fixtureFile.read(reinterpret_cast<char*>(bytes.data()),
                                 static_cast<std::streamsize>(bytes.size()));
                game->runScriptNcsForTest("video_effect_sequence", bytes, game->scriptWorld().pc);
                logInfo(concat("video effect: frame ", frames, " ran ", fixture.filename().string(),
                               " -> row ", game->videoEffect().row, " active ",
                               game->videoEffect().active() ? 1 : 0));
            } else {
                logWarn(concat("video effect fixture missing: ", fixture.string()));
            }
        }
        if (game) {
            VideoEffectState effect = game->videoEffect();
            // Options CB_FRAMEBUFF (0078d98c, set by 0061d750) gates every frame-buffer effect.
            effect.frameBuffer = !userSettings || userSettings->get("graphics.framebuffer") != 0.0f;
            // The same gate returns from FrameBufferModifications 00437520 before the video effect's
            // passes (00432ad0 under 007a6882 and under 007a6883; 00435bd0 on the other route): no effect with it off.
            if (!effect.frameBuffer) {
                effect.saturation = false;
                effect.scanNoise = false;
            }
            // One blur flag and ratio (007a6880, 0078d43c), set by ProgFX 1601 or by ACT_563 in a minigame.
            if (const MiniGameWorld* world = game->miniGame()) {
                effect.speedBlur = world->speedBlur;
                effect.speedBlurRatio = world->speedBlurRatio;
            } else {
                effect.speedBlur = game->effects().speedBlurOn();
                effect.speedBlurRatio = game->effects().speedBlurRatio();
            }
            effect.distortEntities = game->effects().distortionEntities();
            effect.distortPhase = game->effects().distortionPhase();
            // Options CB_SHADOWS: CClientOptions::SetShadows 0061d6b0 sets 0078e3a8, the gate on
            // Scene::RenderShadows in Scene::Render (00451750). The scene focus (scene +0xa8, set
            // through 00458ba0) is taken as the controlled creature's place: not proven, RETAIL row 46.
            effect.shadows = !userSettings || userSettings->get("graphics.shadows") != 0.0f;
            const kotor::Vec3 focus = game->controlledPosition();
            effect.shadowFocus = {focus.x, focus.y, focus.z};
            state.backend->setVideoEffect(effect);
        }
        error.clear();
        const FrameStatus status = state.backend->renderFrame(error);
        if (status == FrameStatus::Fatal) {
            logError(concat("Frame failed: ", error));
            if (state.backend) {
                state.backend->releaseMovieFrame(movieTexture);
            }
            destroyRuntime(state);
            audioOutput.reset();
            SDL_Quit();
            return 6;
        }
        if (shadowProbeFrame > 0 && state.backend) {
            // --shadow-probe: the stencil after the shadow volumes of one frame (00451a50).
            std::size_t covered = 0;
            std::size_t total = 0;
            if (shadowProbeAsked && state.backend->shadowProbe(covered, total)) {
                logInfo(concat("shadow probe: frame ", frames, " covered ", covered, " of ", total, " pixels"));
                shadowProbeAsked = false;
                shadowProbeFrame = 0;
            } else if (!shadowProbeAsked && frames + 1u >= static_cast<std::uint64_t>(shadowProbeFrame)) {
                state.backend->requestShadowProbe();
                shadowProbeAsked = true;
            }
        }
        if (inEffectSequence && !options.videoEffectShots.empty()) {
            const std::string shot = options.videoEffectShots + "-" + std::to_string(sequenceIndex) + ".bmp";
            if (!state.backend->screenshot(shot)) {
                logWarn(concat("video effect shot failed: ", shot));
            }
        }
        if (options.minigameRenderCheck && game && options.frames > 3) {
            // The same frozen view with and without the minigame's own meshes: what
            // differs is the shipped scene content, so an empty pass cannot satisfy this.
            const std::uint64_t limit = static_cast<std::uint64_t>(options.frames);
            if (frames + 3 == limit) {
                if (!game->miniGameActive() || !state.backend->captureFrame(renderCheckFull)) {
                    logError("minigame render check: no minigame or no capture");
                    renderCheckExit = 66;
                }
                miniGameFrozen = true;
                game->setMiniGameMeshesHidden(true);
            } else if (frames + 1 == limit && renderCheckExit == 0) {
                CapturedFrame hidden;
                renderCheckExit = 66;
                if (state.backend->captureFrame(hidden) && hidden.rgba.size() == renderCheckFull.rgba.size()
                    && !hidden.rgba.empty()) {
                    std::size_t different = 0;
                    std::set<std::uint32_t> colours;
                    for (std::size_t i = 0; i + 3 < hidden.rgba.size(); i += 4) {
                        const int delta = std::abs(hidden.rgba[i] - renderCheckFull.rgba[i])
                            + std::abs(hidden.rgba[i + 1] - renderCheckFull.rgba[i + 1])
                            + std::abs(hidden.rgba[i + 2] - renderCheckFull.rgba[i + 2]);
                        different += delta > 24 ? 1u : 0u;
                        colours.insert(static_cast<std::uint32_t>(renderCheckFull.rgba[i])
                                       | (static_cast<std::uint32_t>(renderCheckFull.rgba[i + 1]) << 8u)
                                       | (static_cast<std::uint32_t>(renderCheckFull.rgba[i + 2]) << 16u));
                    }
                    const std::size_t pixels = hidden.rgba.size() / 4;
                    const bool pass = different * 200 >= pixels && colours.size() >= 64;
                    logInfo(concat("minigame render check: module=", game->module(), " backend=",
                                   state.backend->info().name, " pixels=", pixels, " scene_pixels=",
                                   different, " colours=", colours.size(), pass ? " PASS" : " FAIL"));
                    renderCheckExit = pass ? 0 : 66;
                    renderCheckMeasured = true;
                } else {
                    logError("minigame render check: second capture failed");
                }
            }
        }
        if (wrapGate && game && !wrapGate->finished()) {
            wrapGate->step(*state.backend, *game);
            if (wrapGate->finished()) {
                gateExit = wrapGate->exitCode();
                exitReason = "texture wrap check finished";
                running = false;
            }
        }
        if (frontEndGate && frontEnd && state.guiSystem && game) {
            FrontEndGate::Context context;
            context.backend = state.backend.get();
            context.gui = state.guiSystem.get();
            context.front = frontEnd.get();
            context.screens = gameScreens.get();
            context.dialogueGui = dialogueScreen ? &dialogueScreen->activeGui() : dialogueGui.get();
            context.loadProbe = &loadScreenProbe;
            context.hudGui = hudGui.get();
            context.hudStats = &hudStats;
            context.hudTooltip = hudTooltipShown;
            context.hudReticle = hud && hud->reticle().visible ? hud->reticle().art : std::string{};
            context.hudBark = hud && hud->bark().visible ? hud->bark().text : std::string{};
            context.hudView = &hudViewUsed;
            context.painter = state.guiPainter.get();
            context.settings = userSettings.get();
            context.game = &*game;
            context.menuStaged = visuals.menuStaged;
            frontEndGate->step(context);
            if (frontEndGate->finished()) {
                gateExit = frontEndGate->exitCode();
                exitReason = "front end gate finished";
                running = false;
            }
        }
        if (previewGate && state.guiSystem && game) {
            previewGate->step(*state.backend, *state.guiSystem, chargenGui.get(), visuals, *game);
            if (previewGate->finished()) {
                gateExit = previewGate->exitCode();
                logInfo(gateExit == 0 ? "chargen_preview: PASS" : "chargen_preview: FAIL");
                exitReason = "chargen preview gate finished";
                running = false;
            }
        }
        if (stateTrace) {
            // The frame is drawn: sample it, then take the anchor frames that fell due (BMP).
            StateTraceFrame traceFrame;
            traceFrame.game = game ? &*game : nullptr;
            traceFrame.menuGui = state.guiSystem.get();
            traceFrame.dialogueGui = dialogueScreen ? &dialogueScreen->activeGui() : dialogueGui.get();
            traceFrame.hudGui = hudGui.get();
            traceFrame.hudLayout = traceHudLayout;
            traceFrame.userPause = userPause;
            traceFrame.uiPause = pauseMenuOpen || (gameScreens && gameScreens->open());
            if (state.window != nullptr) {
                SDL_GetWindowSizeInPixels(state.window, &traceFrame.windowWidth, &traceFrame.windowHeight);
            }
            stateTrace->step(traceFrame);
            traceHudLayout.clear();
            for (const std::filesystem::path& file : stateTrace->takeShots()) {
                CapturedFrame traceShot;
                if (!state.backend->captureFrame(traceShot) || traceShot.rgba.empty()
                    || !writeBmpTopDown(file, traceShot.width, traceShot.height, traceShot.rgba)) {
                    static bool warned = false;
                    if (!warned) {
                        warned = true;
                        logWarn(concat("state trace: screenshot failed (no capture on this backend?): ", file.generic_string()));
                    }
                }
            }
        }
        for (const std::string& shot : inputScriptShots) {
            // Verification tooling: the conversation state each frame shows, so paired walks can
            // match retail frames by line rather than by clock.
            if (game) {
                logInfo(concat("input script: shot ", shot, " t ", game->elapsedSeconds(), " dlg ",
                               game->dialogue().active ? 1 : 0, " entry ",
                               game->dialogue().entry, " replies ",
                               dialogueScreen && dialogueScreen->repliesShown() ? 1 : 0, " pc ", game->position().x, ",",
                               game->position().y, " fog ", game->mapFog() != nullptr ? game->mapFog()->seenCount() : 0));
            }
            CapturedFrame frame;
            if (!state.backend->captureFrame(frame) || frame.rgba.empty()
                || !writeBmpTopDown(shot, frame.width, frame.height, frame.rgba)) {
                logWarn(concat("input script: screenshot failed: ", shot));
            }
        }
        inputScriptShots.clear();
        if (inputScript.active() && inputScript.finished()) {
            logInfo("input script: finished");
            exitReason = "input script finished";
            running = false;
        }
        // shot:<path>@N in --capture-action: a burst of frames from one run, for paired timing.
        for (std::size_t start = options.captureAction.find("shot:"); start != std::string::npos;
             start = options.captureAction.find("shot:", start + 5u)) {
            const std::size_t at = options.captureAction.find('@', start);
            if (at != std::string::npos
                && std::strtoull(options.captureAction.c_str() + at + 1u, nullptr, 10) == frames + 1) {
                state.backend->screenshot(options.captureAction.substr(start + 5u, at - start - 5u));
            }
        }
        if (!options.screenshot.empty() && options.frames > 0
            && frames + 1 >= static_cast<std::uint64_t>(options.frames)) {
            if (state.backend->screenshot(options.screenshot)) {
                logInfo(concat("Screenshot: ", options.screenshot));
            } else {
                logWarn(concat("Screenshot failed: ", options.screenshot));
            }
        }

        ++frames;

        if (options.frames > 0 && frames >= static_cast<std::uint64_t>(options.frames)) {
            exitReason = concat("frame limit reached (", options.frames, ")");
            running = false;
        } else if (options.seconds > 0.0 && elapsedSeconds >= options.seconds) {
            exitReason = concat("time limit reached (", options.seconds, " s)");
            running = false;
        }

        if (soak && game) {
            soak->step(*game, state.guiSystem.get());
            if (soak->takeSceneRebuilt()) {
                state.backend->setScene(&game->scene());
                state.backend->setCamera(game->camera());
                renderedMeshKey = game->scene().entityMeshes.size() + game->scene().meshRevision;
            }
            if (soak->finished()) {
                gateExit = soak->exitCode();
                exitReason = "soak finished";
                running = false;
            }
        }

        if (running) {
            const SDL_WindowFlags windowFlags = windowShown ? SDL_GetWindowFlags(state.window) : SDL_WindowFlags{0};
            const bool background = windowShown
                && ((windowFlags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED)) != 0
                    || (windowFlags & SDL_WINDOW_INPUT_FOCUS) == 0);
            const double fps = pacedFps(options.maxFps, options.backgroundFps, offlineRun, background);
            if (fps > 0.0) {
                const std::uint64_t paceNow = SDL_GetTicksNS();
                paceDeadline = nextFrameDeadline(paceDeadline, paceNow,
                                                 static_cast<std::uint64_t>(1'000'000'000.0 / fps));
                if (paceDeadline > paceNow) {
                    SDL_DelayNS(paceDeadline - paceNow);
                    const std::uint64_t woke = SDL_GetTicksNS();
                    const std::uint64_t late = woke > paceDeadline ? woke - paceDeadline : 0u;
                    ++paceWaits;
                    paceLateTotal += late;
                    paceLateWorst = std::max(paceLateWorst, late);
                }
            } else if (state.activeBackend == BackendKind::Null && !soak) {
                SDL_Delay(1); // avoid a 100% CPU spin in the headless logical loop
            }
        }
    }

    const double totalSeconds = static_cast<double>(SDL_GetTicksNS() - startTicks) / 1'000'000'000.0;
    const double averageMs = frames > 0 ? (totalSeconds * 1000.0) / static_cast<double>(frames) : 0.0;

    logInfo(concat("Exit: ", exitReason));
    logInfo(concat("Frames: ", frames, " in ", totalSeconds, " s (avg ", averageMs, " ms/frame)"));
    if (paceWaits > 0) {
        logInfo(concat("Frame pacing: ", paceWaits, " waits, late by ",
                       static_cast<double>(paceLateTotal) / static_cast<double>(paceWaits) / 1000.0,
                       " us on average, worst ", static_cast<double>(paceLateWorst) / 1000.0, " us"));
    }
    logInfo(concat("Backend summary: ", state.backend->info().name, " | ",
                   state.backend->info().apiVersion, " | ", state.backend->info().swapchain));

    if (state.backend) {
        state.backend->releaseMovieFrame(movieTexture);
        visuals.release();
    }
    gVisuals = nullptr;
    destroyRuntime(state);
    audioOutput.reset();
    if (game) {
        game->audio().mixer().stopCapture();
    }
    SDL_Quit();
    if (options.minigameRenderCheck && !renderCheckMeasured && renderCheckExit == 0) {
        logError("minigame render check: the run ended before the frames were measured");
        renderCheckExit = 66;
    }
    return gateExit != 0 ? gateExit : renderCheckExit;
}

} // namespace ke
