#include "game/GuiPaint.h"

#include "core/Log.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace ke {

namespace {

// RETAIL-PROVEN (UpdateGuiPulse 00414a50, UpdateGuiHilightPulse 00418300): both clocks start at 0.75 s in state 0, so the
// value runs from 1.0 down to `low` over 0.75 s, then back up over the next 0.75 s. `low` is 0.4 (0073e320) for the text
// and border pulse (0078d3d4) and 0.2 (0073e46c) for the hilight pulse (0078d3e4) that the list box scrollbar uses.
float guiPulse(double seconds, float low)
{
    const float phase = static_cast<float>(std::fmod(seconds, 1.5));
    return phase < 0.75f ? 1.0f - (1.0f - low) * (phase / 0.75f) : low + (1.0f - low) * ((phase - 0.75f) / 0.75f);
}

std::string lowerCase(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

ImU32 packColor(kotor::Vec3 color, float alpha)
{
    const auto channel = [](float value) {
        return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return IM_COL32(channel(color.x), channel(color.y), channel(color.z), channel(alpha));
}

// Parses the TXI block of a font texture (numchars, fontheight, texturewidth, spacingR,
// spacingB, upperleftcoords, lowerrightcoords). `data` is the raw TPC/TGA payload; the
// TXI trails the pixel data as text.
bool parseFontTxi(const std::vector<std::uint8_t>& data, int imageWidth, int imageHeight, GuiFont& font)
{
    const std::string text(data.begin(), data.end());
    const std::size_t start = text.find("numchars");
    if (start == std::string::npos) {
        return false;
    }
    std::istringstream stream(text.substr(start));
    std::string line;
    float textureWidth = 0.0f;
    float fontHeight = 0.0f;
    float spacingR = 0.0f;
    float spacingB = 0.0f;
    struct Pair { float u = 0, v = 0; };
    std::vector<Pair> upperLeft;
    std::vector<Pair> lowerRight;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        std::istringstream words(line);
        std::string key;
        words >> key;
        if (key == "numchars") {
            words >> font.numChars;
        } else if (key == "fontheight") {
            words >> fontHeight;
        } else if (key == "texturewidth") {
            words >> textureWidth;
        } else if (key == "spacingR") {
            words >> spacingR;
        } else if (key == "spacingB") {
            words >> spacingB;
        } else if (key == "upperleftcoords" || key == "lowerrightcoords") {
            int count = 0;
            words >> count;
            std::vector<Pair>& target = key == "upperleftcoords" ? upperLeft : lowerRight;
            for (int i = 0; i < count && std::getline(stream, line); ++i) {
                std::istringstream values(line);
                Pair pair;
                float depth = 0.0f;
                values >> pair.u >> pair.v >> depth;
                target.push_back(pair);
            }
        }
    }
    if (upperLeft.empty() || upperLeft.size() != lowerRight.size() || textureWidth <= 0.0f) {
        return false;
    }
    const float unitPx = static_cast<float>(imageWidth) / textureWidth;
    font.height = fontHeight * unitPx;
    font.spacingR = spacingR * unitPx;
    font.spacingB = spacingB * unitPx;
    font.measureScale = textureWidth * 100.0f / static_cast<float>(imageWidth);
    font.glyphs.resize(upperLeft.size());
    for (std::size_t i = 0; i < upperLeft.size(); ++i) {
        GuiFont::Glyph& glyph = font.glyphs[i];
        glyph.u0 = upperLeft[i].u;
        glyph.u1 = lowerRight[i].u;
        // TXI v runs up (1.0 is the top row); the decoded image is top-left origin.
        glyph.v0 = 1.0f - upperLeft[i].v;
        glyph.v1 = 1.0f - lowerRight[i].v;
        glyph.width = (glyph.u1 - glyph.u0) * static_cast<float>(imageWidth);
        glyph.height = (glyph.v1 - glyph.v0) * static_cast<float>(imageHeight);
    }
    return true;
}

// Fills GuiFont::Glyph::inkTop / inkBottom from the decoded font image. Rows are counted from the top
// of the glyph cell in image pixels, which are design pixels for the stock fonts (unitPx is 1).
void measureFontInk(const std::vector<std::uint8_t>& data, std::uint32_t type, const std::string& name, GuiFont& font)
{
    for (GuiFont::Glyph& glyph : font.glyphs) {
        glyph.inkTop = 0.0f;
        glyph.inkBottom = glyph.height;
    }
    kotor::Image image;
    try {
        image = kotor::decodeTexture(data, type, name);
    } catch (const kotor::FormatError&) {
        return;
    }
    if (image.width <= 0 || image.height <= 0 || image.rgba.size() < static_cast<std::size_t>(image.width) * image.height * 4) {
        return;
    }
    const bool bottomUp = type == static_cast<std::uint32_t>(kotor::ResType::Tpc);
    bool hasAlpha = false;
    for (std::size_t i = 3; i < image.rgba.size() && !hasAlpha; i += 4) {
        hasAlpha = image.rgba[i] != 255;
    }
    const auto lit = [&](int x, int y) {
        const int row = bottomUp ? image.height - 1 - y : y;
        const std::uint8_t* p = &image.rgba[(static_cast<std::size_t>(row) * image.width + x) * 4];
        return (hasAlpha ? p[3] : std::max(p[0], std::max(p[1], p[2]))) > 64;
    };
    for (GuiFont::Glyph& glyph : font.glyphs) {
        const int x0 = std::clamp(static_cast<int>(std::lround(glyph.u0 * image.width)), 0, image.width - 1);
        const int x1 = std::clamp(static_cast<int>(std::lround(glyph.u1 * image.width)), x0 + 1, image.width);
        const int y0 = std::clamp(static_cast<int>(std::lround(glyph.v0 * image.height)), 0, image.height - 1);
        const int y1 = std::clamp(static_cast<int>(std::lround(glyph.v1 * image.height)), y0 + 1, image.height);
        int top = -1;
        int bottom = -1;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (lit(x, y)) {
                    top = top < 0 ? y - y0 : top;
                    bottom = y - y0 + 1;
                    break;
                }
            }
        }
        if (top >= 0) {
            glyph.inkTop = static_cast<float>(top);
            glyph.inkBottom = static_cast<float>(bottom);
        }
    }
}

} // namespace

float GuiFont::advance(unsigned char c) const
{
    if (c >= glyphs.size()) {
        return 0.0f;
    }
    return std::max(0.0f, glyphs[c].width) + spacingR;
}

int GuiFont::wrapAdvance(unsigned char c, bool lineStart) const
{
    const double glyph = c < glyphs.size() ? static_cast<double>(glyphs[c].width) * measureScale : 0.0;
    if (lineStart) {
        return static_cast<int>(glyph);
    }
    return static_cast<int>(glyph + static_cast<double>(spacingR) * measureScale + 0.25);
}

float GuiFont::textWidth(const std::string& text) const
{
    float width = 0.0f;
    for (const unsigned char c : text) {
        width += advance(c);
    }
    return width;
}

// RETAIL-PROVEN (CSWGuiOptionsCheckbox::Draw 006de0e0): the options checkboxes ignore the file's colours and
// set the text (and so the ring) to one of three constants: highlighted (0.98, 1, 0) at 007a23c0, enabled
// (0, 0.66, 0.98) at 007a23b4, disabled (0, 0.33, 0.49) at 007a2408. Paired: walk 0003_options_gameplay.
static kotor::Vec3 optionsCheckboxColor(bool hovered, bool enabled)
{
    if (hovered) {
        return {0.98f, 1.0f, 0.0f};
    }
    return enabled ? kotor::Vec3{0.0f, 0.66f, 0.98f} : kotor::Vec3{0.0f, 0.33f, 0.49f};
}

struct GuiPainter::Painting {
    ImDrawList* draw = nullptr;
    const GuiSystem* gui = nullptr;
    const GuiPaintOptions* options = nullptr;
    const kotor::GuiScreen* screen = nullptr;
    bool anchorApplied = false;
    // Underlay layers paint from their saved states and never show hover.
    const std::map<std::string, GuiControlState>* layerStates = nullptr;
    GuiView view;

    GuiControlState stateOf(const kotor::GuiControl& control) const
    {
        if (layerStates == nullptr) {
            return gui->state(control.key);
        }
        const auto found = layerStates->find(control.key);
        if (found != layerStates->end()) {
            return found->second;
        }
        GuiControlState state;
        state.value = control.curValue;
        state.selected = control.selected || control.isSelected;
        return state;
    }
    bool hovered(const kotor::GuiControl& control) const
    {
        return layerStates == nullptr && (control.key == gui->hovered() || control.key == gui->focused());
    }
};

std::size_t listRows(const kotor::GuiControl& list, float rowHeight = 0.0f)
{
    if (list.proto.empty()) {
        return 0;
    }
    const kotor::GuiControl& proto = list.proto.front();
    const int row = std::max(1, static_cast<int>(rowHeight > 0.0f ? rowHeight : static_cast<float>(proto.extent.height))
                                    + list.padding);
    const int room = list.extent.top + list.extent.height - proto.extent.top + list.padding;
    return static_cast<std::size_t>(std::max(1, room / row));
}

GuiPainter::GuiPainter(kotor::Install& install, IRenderBackend* backend, TlkLookup tlk)
    : install_(install), backend_(backend), tlk_(std::move(tlk))
{
}

GuiPainter::~GuiPainter()
{
    release();
}

void GuiPainter::release()
{
    if (backend_ != nullptr) {
        for (auto& entry : textures_) {
            if (entry.second.has_value() && entry.second->id != 0) {
                backend_->releaseUiImage(entry.second->id);
            }
        }
    }
    textures_.clear();
    fonts_.clear();
}

const GuiTexture* GuiPainter::uploadImage(const std::string& key, const kotor::Image& image)
{
    if (backend_ == nullptr || image.width <= 0 || image.height <= 0) {
        return nullptr;
    }
    const std::string name = lowerCase(key);
    std::uint64_t id = 0;
    if (const auto found = textures_.find(name);
        found != textures_.end() && found->second.has_value()) {
        id = found->second->id; // re-upload into the same slot
    }
    std::string error;
    if (!backend_->uploadUiImage(image.width, image.height, image.rgba.data(), id, error)) {
        logWarn(concat("GUI image upload failed (", key, "): ", error));
        return nullptr;
    }
    GuiTexture texture;
    texture.width = image.width;
    texture.height = image.height;
    texture.id = id;
    textures_[name] = texture;
    return &*textures_[name];
}

void GuiPainter::dropTexture(const std::string& key)
{
    const auto found = textures_.find(lowerCase(key));
    if (found == textures_.end()) {
        return;
    }
    if (backend_ != nullptr && found->second.has_value() && found->second->id != 0) {
        backend_->releaseUiImage(found->second->id);
    }
    textures_.erase(found);
}

const GuiTexture* GuiPainter::texture(const std::string& resref)
{
    if (resref.empty() || backend_ == nullptr) {
        return nullptr;
    }
    const std::string name = lowerCase(resref);
    const auto cached = textures_.find(name);
    if (cached != textures_.end()) {
        return cached->second.has_value() ? &*cached->second : nullptr;
    }
    std::optional<GuiTexture> loaded;
    std::uint32_t type = 0;
    if (auto data = install_.findTexture(name, &type)) {
        try {
            kotor::Image image = kotor::decodeTexture(*data, type, name);
            if (type == static_cast<std::uint32_t>(kotor::ResType::Tpc)) {
                // TPC rows are stored bottom-up (the scene shaders flip V); UI images are
                // sampled top-down, as PortraitImages does.
                const std::size_t row = static_cast<std::size_t>(image.width) * 4u;
                for (int y = 0; y < image.height / 2; ++y) {
                    std::swap_ranges(image.rgba.begin() + static_cast<std::ptrdiff_t>(y * row),
                                     image.rgba.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                                     image.rgba.begin()
                                         + static_cast<std::ptrdiff_t>((image.height - 1 - y) * row));
                }
            }
            // `blending additive` in the art's TXI: retail draws it with glBlendFunc(SRC_ALPHA, ONE)
            // (TXI parser 0047abd0 sets the source/destination factors, 0047af70 applies them from
            // the table at 0073f270), so black adds nothing and shows what is behind. Both backends
            // provide that blend as a draw callback (IRenderBackend::uiAdditiveCallback) and the
            // art is drawn between beginArt / endArt with its own texels. Only a backend without
            // it gets the fallback below: the nearest straight-alpha picture, made from the pixels
            // (alpha = brightest channel times the art's own alpha, colour scaled back up). That
            // fallback is an approximation: a bright texel replaces the background, not adds to it.
            bool additive = false;
            bool approximated = false;
            if (type == static_cast<std::uint32_t>(kotor::ResType::Tpc)
                && kotor::parseTxi(kotor::tpcEmbeddedTxi(*data)).blending == kotor::TxiInfo::Blend::Additive) {
                additive = true;
                approximated = backend_->uiAdditiveCallback() == nullptr;
            }
            if (approximated) {
                for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
                    const unsigned r = image.rgba[i];
                    const unsigned g = image.rgba[i + 1];
                    const unsigned b = image.rgba[i + 2];
                    const unsigned a = image.rgba[i + 3];
                    const unsigned m = std::max(r, std::max(g, b));
                    if (m == 0 || a == 0) {
                        image.rgba[i] = image.rgba[i + 1] = image.rgba[i + 2] = image.rgba[i + 3] = 0;
                        continue;
                    }
                    image.rgba[i] = static_cast<std::uint8_t>(std::min(255u, (r * 255u + m / 2) / m));
                    image.rgba[i + 1] = static_cast<std::uint8_t>(std::min(255u, (g * 255u + m / 2) / m));
                    image.rgba[i + 2] = static_cast<std::uint8_t>(std::min(255u, (b * 255u + m / 2) / m));
                    image.rgba[i + 3] = static_cast<std::uint8_t>((m * a + 127u) / 255u);
                }
            }
            std::uint64_t id = 0;
            std::string error;
            if (backend_->uploadUiImage(image.width, image.height, image.rgba.data(), id, error)) {
                GuiTexture texture;
                texture.additive = additive;
                texture.approximated = approximated;
                texture.width = image.width;
                texture.height = image.height;
                texture.id = id;
                texture.flat = true;
                for (std::size_t i = 4; i + 3 < image.rgba.size() && texture.flat; i += 4) {
                    for (std::size_t c = 0; c < 4; ++c) {
                        if (image.rgba[i + c] != image.rgba[c]) {
                            texture.flat = false;
                            break;
                        }
                    }
                }
                loaded = texture;
            } else {
                logWarn(concat("GUI texture upload failed (", name, "): ", error));
            }
        } catch (const kotor::FormatError& error) {
            logWarn(concat("GUI texture decode failed (", name, "): ", error.what()));
        }
    } else {
        logWarn(concat("GUI texture not found: ", name));
    }
    textures_[name] = loaded;
    return loaded.has_value() ? &*textures_[name] : nullptr;
}

void GuiPainter::beginArt(ImDrawList* draw, const GuiTexture& tex) const
{
    if (tex.additive && !tex.approximated && backend_ != nullptr) {
        if (const UiDrawCallback callback = backend_->uiAdditiveCallback()) {
            draw->AddCallback(callback, nullptr);
        }
    }
}

void GuiPainter::endArt(ImDrawList* draw, const GuiTexture& tex) const
{
    if (tex.additive && !tex.approximated && backend_ != nullptr && backend_->uiAdditiveCallback() != nullptr) {
        draw->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
    }
}

bool GuiPainter::drawProbe(ImDrawList* draw, const std::string& resref, float x0, float y0, float x1, float y1)
{
    const GuiTexture* tex = texture(resref);
    if (tex == nullptr) {
        return false;
    }
    beginArt(draw, *tex);
    draw->AddImage(static_cast<ImTextureID>(tex->id), ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(0.0f, 0.0f),
                   ImVec2(1.0f, 1.0f), IM_COL32_WHITE);
    endArt(draw, *tex);
    return true;
}

const GuiFont* GuiPainter::font(const std::string& resref)
{
    if (resref.empty()) {
        return nullptr;
    }
    std::string name = lowerCase(resref);
    // OPEN: retail 00415d60 -> 0040b360 asks for "<font>a" below 1280 wide and "<font>b" from 1280, but
    // drawing the shipped dialogfont16x16a by its own TXI shrinks the main menu's text well below the
    // retail frame (paired\walk\out_fe, 2026-11-06), so the glyph sizing for those variants is not
    // understood yet. The base fonts stay in use until it is.
    // fnt_d16x16's embedded TXI does not describe its own image: it lists 32x8 cells of 16 px
    // ('A' is 3 px wide in a 10 px line) while the pixels are a 16-column grid of 32 px cells
    // (measured from the decoded alpha, install swpc_tex_gui.erf), so every glyph reads
    // from the wrong place and the text is specks. dialogfont16x16 carries the same 256
    // glyphs with a TXI that matches its image; text in this font uses it instead.
    // OPEN: retail draws fnt_d16x16a here (0040b360, below 1280 wide): mixed case, 18.75 px line
    // pitch in the retail options description (paired\walk\out_fe2 0003_options_gameplay), while
    // its TXI gives 14 px at 100 px per unit (the console path 0044cca0 / 004a1770 uses 100). The
    // GUI text scale that makes 14 into 18.75 is not found yet; drawn 1:1 it is far too small.
    // RETAIL-PROVEN: 0040b360 appends 'b' to every font but fnt_console when the option byte +0x80 is 2, and
    // 0061e4ec..0061e595 stores 2 there when swkotor.ini has "Use Small Fonts=0", which the shipped ini
    // sets. fnt_d16x16b is then a mixed-case 19 px line (fontheight 0.19 at 100 px per unit), the line
    // pitch of the retail 1024x768 frames (paired levelup_attrs LB_DESC).
    if (name != "fnt_console") {
        std::uint32_t variantType = 0;
        if (install_.findTexture(name + "b", &variantType)) {
            name += "b";
        } else if (name == "fnt_d16x16") {
            name = "dialogfont16x16";
        }
    }
    if (const auto found = fonts_.find(name); found != fonts_.end()) {
        return found->second && found->second->ready() ? found->second.get() : nullptr;
    }
    auto font = std::make_unique<GuiFont>();
    std::uint32_t type = 0;
    if (auto data = install_.findTexture(name, &type)) {
        if (const GuiTexture* texture = this->texture(name)) {
            font->texture = *texture;
            if (!parseFontTxi(*data, texture->width, texture->height, *font)) {
                logWarn(concat("font TXI not readable: ", name));
                font->glyphs.clear();
            } else {
                measureFontInk(*data, type, name, *font);
            }
        }
    } else {
        logWarn(concat("font not found: ", name));
    }
    const GuiFont* result = font->ready() ? font.get() : nullptr;
    fonts_[name] = std::move(font);
    return result;
}

GuiView GuiPainter::layout(const kotor::GuiScreen& screen, float windowWidth, float windowHeight) const
{
    // RETAIL-PROVEN: a panel is drawn 1:1. A 640x480 panel sits at ((W-640)/2, (H-480)/2) plus its
    // own LEFT/TOP (0040ba20 and the hit test 0040b690: integer halves of the window minus 640x480).
    // A panel retail centres on its own size (0040a600: the main menu's 800x600, tooltips) gets
    // ((W-w)/2, (H-h)/2); for a 640x480 panel both rules agree. Paired frame menu_main: the
    // 800x600 main menu at 112,84 in a 1024x768 window.
    GuiView view;
    const bool popup = screen.extent.left != 0 || screen.extent.top != 0 || screen.extent.width < 640
        || screen.extent.height < 480;
    view.baseWidth = static_cast<float>(!popup && screen.extent.width > 0 ? screen.extent.width : 640);
    view.baseHeight = static_cast<float>(!popup && screen.extent.height > 0 ? screen.extent.height : 480);
    if (popup) {
        view.panelLeft = static_cast<float>(screen.extent.left);
        view.panelTop = static_cast<float>(screen.extent.top);
        // RETAIL-PROVEN: the panels whose constructors call CSWGuiPanel::CenterPanel (0040a600) sit at
        // ((W - w) / 2, (H - h) / 2) of the window whatever their LEFT/TOP: the message box (00626df0,
        // and 006253a0 after it resizes), container (006b6dc0), savename (006cae70) and skillinfo
        // (006ce7c0). Paired walk 0070_cg17: skillinfo's frame at 347,270 at 1024x768 (file TOP 118
        // would put it 6 px higher).
        std::string name = screen.name; // the resource name, "skillinfo.gui"
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".gui") == 0) {
            name.resize(name.size() - 4);
        }
        if (name == "confirm" || name == "container" || name == "savename" || name == "skillinfo") {
            const float originX = windowWidth >= 640.0f ? std::floor(0.5f * (windowWidth - 640.0f)) : 0.0f;
            const float originY = windowHeight >= 480.0f ? std::floor(0.5f * (windowHeight - 480.0f)) : 0.0f;
            if (windowWidth >= 640.0f && windowHeight >= 480.0f) {
                view.panelLeft = static_cast<float>(static_cast<int>(windowWidth - static_cast<float>(screen.extent.width)) / 2) - originX;
                view.panelTop = static_cast<float>(static_cast<int>(windowHeight - static_cast<float>(screen.extent.height)) / 2) - originY;
            } else {
                view.panelLeft = static_cast<float>((640 - screen.extent.width) / 2);
                view.panelTop = static_cast<float>((480 - screen.extent.height) / 2);
            }
        }
    }
    view.windowWidth = windowWidth;
    view.windowHeight = windowHeight;
    view.scale = 1.0f;
    if (windowWidth < view.baseWidth || windowHeight < view.baseHeight) {
        // ENGINE CHOICE: retail never runs below 640x480 (its smallest mode); a smaller window shrinks
        // the panel to fit instead of cutting it off.
        view.scale = std::min(windowWidth / view.baseWidth, windowHeight / view.baseHeight);
        view.offsetX = 0.5f * (windowWidth - view.baseWidth * view.scale);
        view.offsetY = 0.5f * (windowHeight - view.baseHeight * view.scale);
        return view;
    }
    view.offsetX = std::floor(0.5f * (windowWidth - view.baseWidth));
    view.offsetY = std::floor(0.5f * (windowHeight - view.baseHeight));
    return view;
}

void guiAnchorShift(const GuiView& view, const kotor::GuiControl& control, float& shiftX, float& shiftY)
{
    shiftX = 0.0f;
    shiftY = 0.0f;
    if (!view.anchored) {
        return;
    }
    const float extraW = view.windowWidth / view.scale - view.baseWidth;
    const float extraH = view.windowHeight / view.scale - view.baseHeight;
    const float cx = static_cast<float>(control.extent.left) + 0.5f * static_cast<float>(control.extent.width);
    const float cy = static_cast<float>(control.extent.top) + 0.5f * static_cast<float>(control.extent.height);
    shiftX = cx < view.baseWidth / 3.0f ? 0.0f : (cx > view.baseWidth * 2.0f / 3.0f ? extraW : 0.5f * extraW);
    shiftY = cy < view.baseHeight / 3.0f ? 0.0f : (cy > view.baseHeight * 2.0f / 3.0f ? extraH : 0.5f * extraH);
}

GuiPixelRect guiControlPixels(const GuiView& view, const kotor::GuiControl& control, float offsetX, float offsetY)
{
    float shiftX = 0.0f;
    float shiftY = 0.0f;
    guiAnchorShift(view, control, shiftX, shiftY);
    GuiPixelRect r;
    r.x0 = view.pxX(static_cast<float>(control.extent.left) + shiftX + offsetX);
    r.y0 = view.pxY(static_cast<float>(control.extent.top) + shiftY + offsetY);
    r.x1 = view.pxX(static_cast<float>(control.extent.left + control.extent.width) + shiftX + offsetX);
    r.y1 = view.pxY(static_cast<float>(control.extent.top + control.extent.height) + shiftY + offsetY);
    return r;
}

void GuiPainter::toGui(float px, float py, float& x, float& y) const
{
    x = view_.guiX(px);
    y = view_.guiY(py);
}

float GuiPainter::measureText(const std::string& fontResref, const std::string& text, float width)
{
    const GuiFont* f = font(fontResref);
    if (f == nullptr) {
        return 0.0f;
    }
    return static_cast<float>(wrapLines(*f, text, width).size()) * (f->height + f->spacingB);
}

std::string GuiPainter::controlText(const kotor::GuiControl& control, const GuiControlState& state) const
{
    if (state.hasText) {
        return state.text;
    }
    if (control.strref != 0xFFFFFFFFu && tlk_) {
        return tlk_(control.strref);
    }
    return control.text;
}

void GuiPainter::note(const char* kind, const std::string& detail) const
{
    const std::string k = kind;
    if (k == "text_overflow") {
        ++stats_.textOverflow;
    } else if (k == "text_clip") {
        ++stats_.textClipped;
    } else if (k == "image_clip") {
        ++stats_.imageClipped;
    } else {
        ++stats_.emptyControls;
    }
    if (stats_.issues.size() < 256) {
        stats_.issues.push_back({k, auditTag_, detail});
    }
}

std::vector<std::string> GuiPainter::wrapLines(const GuiFont& font, const std::string& source, float width) const
{
    std::string text;
    text.reserve(source.size());
    for (const char c : source) {
        if (c != '\r') {
            text.push_back(c);
        }
    }
    std::vector<std::string> lines;
    if (width <= 0.0f) {
        std::size_t start = 0;
        while (true) {
            const std::size_t newline = text.find('\n', start);
            lines.push_back(text.substr(start, newline == std::string::npos ? std::string::npos : newline - start));
            if (newline == std::string::npos) {
                return lines;
            }
            start = newline + 1;
        }
    }
    // RETAIL-PROVEN: CAurGUIStringInternal::WrapStrings 0045a2f0, step for step. The limit is the
    // text extent's width as a whole number (CSWGuiText::SetExtent 00416280 hands it over; the
    // control's border inner extent, CSWGuiBorder::GetInnerExtent 00415240). Scanning adds one
    // character at a time and stops on the first one that brings the sum to the limit or past it;
    // the line then ends at the last space seen (dropped) or after the last hyphen between two
    // letters (kept). With neither, the word is cut one character early and a hyphen added. The
    // sum starts from the line's first character alone (wrapAdvance), and a run of newlines at a
    // line start gives one empty line each while the sum keeps the start character's width.
    if (text.empty()) {
        return lines;
    }
    const int limit = static_cast<int>(std::floor(width + 0.001f));
    const auto at = [&](std::size_t i) -> unsigned char {
        return i < text.size() ? static_cast<unsigned char>(text[i]) : 0;
    };
    std::size_t start = 0;
    int sum = font.wrapAdvance(at(0), true);
    while (true) {
        std::size_t p = start;
        while (at(p) == '\n') {
            lines.emplace_back();
            ++p;
            start = p;
        }
        std::size_t cut = std::string::npos;
        bool finished = at(p) == 0;
        if (!finished && sum < limit) {
            while (true) {
                if (at(p) == '-' && p > 0 && std::isalpha(at(p - 1)) && std::isalpha(at(p + 1))) {
                    cut = p;
                }
                ++p;
                const unsigned char c = at(p);
                if (c == 0 || c == '\n') {
                    finished = true;
                    break;
                }
                sum += font.wrapAdvance(c, false);
                if (c == ' ') {
                    cut = p;
                }
                if (sum >= limit) {
                    break;
                }
            }
        }
        std::size_t next = p;
        if (finished || at(p) == 0 || at(p) == '\n') {
            lines.push_back(text.substr(start, p - start));
            if (at(next) == '\n') {
                ++next;
            }
        } else if (cut == std::string::npos || cut < start) {
            if (p == 0) {
                return {}; // retail drops every line when the cut would land on the string's start
            }
            --p;
            if (p <= start) {
                // ENGINE CHOICE: retail does not guard a box narrower than one character; keep it.
                p = start + 1;
                lines.push_back(text.substr(start, 1));
            } else {
                lines.push_back(text.substr(start, p - start) + "-");
            }
            next = p;
        } else if (text[cut] == ' ') {
            lines.push_back(text.substr(start, cut - start));
            next = cut + 1;
        } else {
            lines.push_back(text.substr(start, cut + 1 - start));
            next = cut + 1;
        }
        start = next;
        sum = font.wrapAdvance(at(start), true);
        if (at(start) == 0) {
            return lines;
        }
    }
}

void GuiPainter::drawText(ImDrawList* draw, const GuiFont& font, const std::string& text, float x0, float y0,
                          float x1, float y1, int align, unsigned color, float scale, bool wrap) const
{
    if (text.empty() || !font.ready()) {
        return;
    }
    const std::vector<std::string> lines =
        wrap ? wrapLines(font, text, (x1 - x0) / scale) : std::vector<std::string>{text};
    const float lineStep = (font.height + font.spacingB) * scale;
    stats_.minTextPixels = std::min(stats_.minTextPixels, font.height * scale);
    const float block = lineStep * static_cast<float>(lines.size());
    textBounds_[0] = textBounds_[1] = 1.0e9f;
    textBounds_[2] = textBounds_[3] = -1.0e9f;
    float y = y0;
    if (align & 32) {
        y = y1 - block;
    } else if (align & 16) {
        y = y0 + 0.5f * (y1 - y0 - block);
    }
    if (!(align & (8 | 16 | 32))) {
        y = y0 + 0.5f * (y1 - y0 - block);
    }
    for (const std::string& line : lines) {
        const float width = font.textWidth(line) * scale;
        float x = x0;
        if (align & 4) {
            x = x1 - width;
        } else if (align & 1) {
            x = x0;
        } else {
            x = x0 + 0.5f * (x1 - x0 - width);
        }
        for (const unsigned char c : line) {
            if (c < font.glyphs.size() && c != ' ') { // a space only advances
                const GuiFont::Glyph& glyph = font.glyphs[c];
                if (glyph.width > 0.0f && glyph.height > 0.0f) {
                    ++stats_.glyphs;
                    textBounds_[0] = std::min(textBounds_[0], x);
                    textBounds_[1] = std::min(textBounds_[1], y + glyph.inkTop * scale);
                    textBounds_[2] = std::max(textBounds_[2], x + glyph.width * scale);
                    textBounds_[3] = std::max(textBounds_[3], y + glyph.inkBottom * scale);
                    draw->AddImage(static_cast<ImTextureID>(font.texture.id),
                                   ImVec2(x, y), ImVec2(x + glyph.width * scale, y + glyph.height * scale),
                                   ImVec2(glyph.u0, glyph.v0), ImVec2(glyph.u1, glyph.v1), color);
                }
            }
            x += font.advance(c) * scale;
        }
        y += lineStep;
    }
    if (auditOn_ && textBounds_[2] > textBounds_[0]) {
        const float slack = 2.0f * scale;
        float widest = 0.0f;
        for (const std::string& line : lines) {
            widest = std::max(widest, font.textWidth(line) * scale);
        }
        // The ink (glyph rows with pixels), not the line step: a 13 px button in a 16 px font is not
        // an overflow while its letters are 12 px tall.
        const float ink = textBounds_[3] - textBounds_[1];
        if (ink > (y1 - y0) + slack || widest > (x1 - x0) + slack) {
            note("text_overflow", concat("block ", static_cast<int>(widest), "x", static_cast<int>(ink), " in ",
                                         static_cast<int>(x1 - x0), "x", static_cast<int>(y1 - y0), " text '",
                                         text.substr(0, 28), "'"));
        }
        const ImVec2 cmin = draw->GetClipRectMin();
        const ImVec2 cmax = draw->GetClipRectMax();
        if (!clipExpected_
            && (textBounds_[0] < cmin.x - 1.0f || textBounds_[1] < cmin.y - 1.0f || textBounds_[2] > cmax.x + 1.0f
                || textBounds_[3] > cmax.y + 1.0f)) {
            note("text_clip", concat("glyphs ", static_cast<int>(textBounds_[0]), ",", static_cast<int>(textBounds_[1]),
                                     ",", static_cast<int>(textBounds_[2]), ",", static_cast<int>(textBounds_[3]),
                                     " clip ", static_cast<int>(cmin.x), ",", static_cast<int>(cmin.y), ",",
                                     static_cast<int>(cmax.x), ",", static_cast<int>(cmax.y), " text '",
                                     text.substr(0, 28), "'"));
        }
    }
}

void GuiPainter::paintImage(Painting& p, const std::string& resref, float x0, float y0, float x1, float y1,
                            unsigned color, bool flipV, bool flipU)
{
    const GuiTexture* tex = texture(resref);
    if (tex == nullptr) {
        ++stats_.missingImages;
        if (stats_.missingNames.size() < 16
            && std::find(stats_.missingNames.begin(), stats_.missingNames.end(), resref) == stats_.missingNames.end()) {
            stats_.missingNames.push_back(resref);
        }
        return;
    }
    ++stats_.images;
    if (auditOn_) {
        const ImVec2 cmin = p.draw->GetClipRectMin();
        const ImVec2 cmax = p.draw->GetClipRectMax();
        const float lo = 1.5f * p.view.scale;
        const float left = std::min(x0, x1);
        const float right = std::max(x0, x1);
        const float top = std::min(y0, y1);
        const float bottom = std::max(y0, y1);
        if (left < cmin.x - lo || top < cmin.y - lo || right > cmax.x + lo || bottom > cmax.y + lo) {
            note("image_clip", concat(resref, " ", static_cast<int>(left), ",", static_cast<int>(top), ",",
                                      static_cast<int>(right), ",", static_cast<int>(bottom), " clip ",
                                      static_cast<int>(cmin.x), ",", static_cast<int>(cmin.y), ",",
                                      static_cast<int>(cmax.x), ",", static_cast<int>(cmax.y)));
        }
    }
    stats_.maxRight = std::max(stats_.maxRight, std::max(x0, x1));
    stats_.maxBottom = std::max(stats_.maxBottom, std::max(y0, y1));
    beginArt(p.draw, *tex);
    p.draw->AddImage(static_cast<ImTextureID>(tex->id), ImVec2(x0, y0), ImVec2(x1, y1),
                     ImVec2(flipU ? 1.0f : 0.0f, flipV ? 1.0f : 0.0f),
                     ImVec2(flipU ? 0.0f : 1.0f, flipV ? 0.0f : 1.0f), color);
    endArt(p.draw, *tex);
}

void GuiPainter::paintBorder(Painting& p, const kotor::GuiBorder& border, const kotor::GuiControl& control,
                             float x0, float y0, float x1, float y1, float alpha, bool fillOnly)
{
    // RETAIL-PROVEN (CSWGuiBorder::Load 004153e0 hands PULSING to SetPulsingAlpha 00414d00; CSWGuiBorder::Draw 004168c0
    // swaps the border's alpha for the shared pulse 0078d3d4 while the flag is set): a PULSING border breathes with the
    // same triangle as a pulsing text. Paired walk 0041..0081: the lit step ring tracks the lit step's text frame by frame.
    if (border.pulsing && p.options != nullptr) {
        alpha *= guiPulse(p.options->timeSeconds, 0.4f);
    }
    const float scale = p.view.scale;
    const float dimension = static_cast<float>(border.dimension) * scale;
    if (!border.fill.empty()) {
        // The fill is tinted by the border's COLOR (blue idle and yellow hovered on the +/-
        // arrows, white on most art), as the edge and corner art below already are. DIMENSION
        // is the edge art's thickness: a fill is inset by it only when there is edge or corner
        // art to leave room for (the left difficulty arrow carries DIMENSION=32 with none, its
        // sibling carries 0, and both are the same 24x25 control).
        const bool framed = !border.edge.empty() || !border.corner.empty();
        const float inset = framed ? dimension : 0.0f;
        const unsigned fillTint = packColor(border.color, alpha);
        // ENGINE CHOICE: the shipped cyclearrow1/2 art is a left arrow only and both BTN_CHARLEFT and
        // BTN_CHARRIGHT name it (no FLIPSTYLE in the files, none read in the panel constructors):
        // the right button mirrors it, as the scrollbar's down arrow mirrors the up arrow.
        const bool mirror = control.tag == "BTN_CHARRIGHT";
        // RETAIL-PROVEN (0068b9d0 turns BTN_ACTIONDOWN0..3 by 180 degrees): the HUD's action-slot DOWN
        // arrows name the same lbl_miarr_1/2 art as the UP arrows and are drawn turned over.
        // The target block's BTN_TARGETDOWN0..2 come from the same Initialize (param_4 picks the "TARGET"
        // prefix), so they are turned too (paired slots_pair: retail's target DOWN arrow points down).
        const bool turned = control.tag.rfind("BTN_ACTIONDOWN", 0) == 0 || control.tag.rfind("BTN_TARGETDOWN", 0) == 0;
        paintImage(p, border.fill, x0 + inset, y0 + inset, x1 - inset, y1 - inset, fillTint, turned,
                   mirror != turned);
        if (const GuiTexture* art = texture(border.fill)) {
            Stats::Placed placed;
            placed.tag = control.tag;
            placed.x0 = x0 + inset;
            placed.y0 = y0 + inset;
            placed.x1 = x1 - inset;
            placed.y1 = y1 - inset;
            placed.nativeWidth = art->width;
            placed.nativeHeight = art->height;
            placed.flat = art->flat;
            placed.tint = fillTint;
            stats_.fills.push_back(std::move(placed));
        }
    }
    if (fillOnly || dimension <= 0.0f) {
        return;
    }
    const unsigned tint = packColor(border.color, alpha);
    if (!border.edge.empty()) {
        if (const GuiTexture* edge = texture(border.edge)) {
            const ImTextureID id = static_cast<ImTextureID>(edge->id);
            beginArt(p.draw, *edge);
            const float innerW = x1 - x0 - 2.0f * dimension;
            const float innerH = y1 - y0 - 2.0f * dimension;
            if (innerW > 0.0f) {
                // top: u along x, v=0 on the outside
                p.draw->AddImageQuad(id, ImVec2(x0 + dimension, y0), ImVec2(x1 - dimension, y0),
                                     ImVec2(x1 - dimension, y0 + dimension), ImVec2(x0 + dimension, y0 + dimension),
                                     ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), tint);
                // bottom: outside is down
                p.draw->AddImageQuad(id, ImVec2(x0 + dimension, y1 - dimension), ImVec2(x1 - dimension, y1 - dimension),
                                     ImVec2(x1 - dimension, y1), ImVec2(x0 + dimension, y1),
                                     ImVec2(0, 1), ImVec2(1, 1), ImVec2(1, 0), ImVec2(0, 0), tint);
            }
            if (innerH > 0.0f) {
                // left: u along y, v=0 on the outside (left)
                p.draw->AddImageQuad(id, ImVec2(x0, y0 + dimension), ImVec2(x0 + dimension, y0 + dimension),
                                     ImVec2(x0 + dimension, y1 - dimension), ImVec2(x0, y1 - dimension),
                                     ImVec2(0, 0), ImVec2(0, 1), ImVec2(1, 1), ImVec2(1, 0), tint);
                // right
                p.draw->AddImageQuad(id, ImVec2(x1 - dimension, y0 + dimension), ImVec2(x1, y0 + dimension),
                                     ImVec2(x1, y1 - dimension), ImVec2(x1 - dimension, y1 - dimension),
                                     ImVec2(0, 1), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), tint);
            }
            endArt(p.draw, *edge);
            ++stats_.borderArt;
        } else {
            noteMissingBorderArt(border.edge);
        }
    }
    if (!border.corner.empty()) {
        if (const GuiTexture* corner = texture(border.corner)) {
            const ImTextureID id = static_cast<ImTextureID>(corner->id);
            beginArt(p.draw, *corner);
            const auto put = [&](float cx0, float cy0, bool flipU, bool flipV) {
                p.draw->AddImage(id, ImVec2(cx0, cy0), ImVec2(cx0 + dimension, cy0 + dimension),
                                 ImVec2(flipU ? 1.0f : 0.0f, flipV ? 1.0f : 0.0f),
                                 ImVec2(flipU ? 0.0f : 1.0f, flipV ? 0.0f : 1.0f), tint);
            };
            put(x0, y0, false, false);
            put(x1 - dimension, y0, true, false);
            put(x0, y1 - dimension, false, true);
            put(x1 - dimension, y1 - dimension, true, true);
            endArt(p.draw, *corner);
            ++stats_.borderArt;
        } else {
            noteMissingBorderArt(border.corner);
        }
    }
}

// An edge or corner picture that does not resolve is as missing as a fill that does not.
void GuiPainter::noteMissingBorderArt(const std::string& resref)
{
    ++stats_.missingImages;
    if (stats_.missingNames.size() < 16
        && std::find(stats_.missingNames.begin(), stats_.missingNames.end(), resref) == stats_.missingNames.end()) {
        stats_.missingNames.push_back(resref);
    }
}

void GuiPainter::paintScrollbar(Painting& p, const kotor::GuiControl& control, const GuiControlState& state,
                                const GuiControlState* listState)
{
    (void)state;
    const GuiView& v = p.view;
    const float x0 = v.pxX(static_cast<float>(control.extent.left));
    const float y0 = v.pxY(static_cast<float>(control.extent.top));
    const float x1 = v.pxX(static_cast<float>(control.extent.left + control.extent.width));
    const float y1 = v.pxY(static_cast<float>(control.extent.top + control.extent.height));
    paintBorder(p, control.border, control, x0, y0, x1, y1, 1.0f);
    const float arrow = static_cast<float>(control.extent.width) * v.scale;
    if (!control.dir.image.empty()) {
        paintImage(p, control.dir.image, x0, y0, x1, y0 + arrow, IM_COL32_WHITE);
        paintImage(p, control.dir.image, x0, y1 - arrow, x1, y1, IM_COL32_WHITE, true);
    }
    (void)listState;
}

// Retail 006ccc40 draws, per used cell, the lbl_indent plate, the icon, then a border2d/border1d frame, and an
// lbl_skarr arrow in front of each used cell after the first. 006cce30 places them; 006ccfd0 sets each part's
// alpha and the frame's colour from the status. The colours (007a23c0, 007a23d8, 007a23e4) are filled at run time,
// so yellow, red and green here are read from the paired frame cg_feats and the use of the same constants for
// damage and healing.
void GuiPainter::paintFeatRow(Painting& p, const GuiListItem& item, int picked, float x0, float y0)
{
    const GuiView& v = p.view;
    const float h = kFeatRowHeight * v.scale;
    const float w = kFeatRowWidth * v.scale;
    constexpr float kArrow = 32.0f;
    kotor::GuiBorder frame;
    frame.corner = "border2d";
    frame.edge = "border1d";
    frame.dimension = 8;
    const kotor::GuiControl plain;
    for (std::size_t c = 0; c < item.cells.size() && c < 3; ++c) {
        const GuiListCell& cell = item.cells[c];
        if (cell.value < 0) {
            continue;
        }
        const bool chosen = static_cast<int>(c) == picked;
        float plate = 1.0f;
        float icon = 1.0f;
        float arrow = 1.0f;
        float edge = 1.0f;
        kotor::Vec3 colour{0.0f, 1.0f, 0.0f};
        switch (cell.status) {
        case 0:
            icon = 0.25f;
            colour = {1.0f, 1.0f, 0.0f};
            // The picked can-take-now cell's yellow frame pulses (retail walk 0056..0062: 120..203 over time, while the
            // green frames of owned feats stay at 127), the shared 0078d3d4 triangle.
            edge = chosen ? guiPulse(p.options->timeSeconds, 0.4f) : 0.0f;
            break;
        case 1:
        case 2:
            edge = chosen ? 1.0f : 0.5f;
            break;
        case 3:
            icon = 0.25f;
            plate = 0.25f;
            arrow = 0.0f;
            colour = {1.0f, 0.0f, 0.0f};
            edge = chosen ? 1.0f : 0.0f;
            break;
        default:
            break;
        }
        const float cx = x0 + static_cast<float>(c) * (w - h) * 0.5f;
        paintImage(p, "lbl_indent", cx, y0, cx + h, y0 + h, packColor({1.0f, 1.0f, 1.0f}, plate));
        const float inset = (kFeatRowHeight - kArrow) * 0.5f * v.scale;
        if (!cell.icon.empty()) {
            paintImage(p, cell.icon, cx + inset, y0 + inset, cx + h - inset, y0 + h - inset,
                       packColor({1.0f, 1.0f, 1.0f}, icon));
        }
        if (edge > 0.0f) {
            frame.color = colour;
            paintBorder(p, frame, plain, cx, y0, cx + h, y0 + h, edge);
        }
        if (c > 0 && arrow > 0.0f) {
            const float size = kArrow * v.scale;
            const float ax = x0 + (c == 1 ? (w + h) * 0.25f : (3.0f * w - h) * 0.25f) - size * 0.5f;
            const float ay = y0 + (h - size) * 0.5f;
            paintImage(p, "lbl_skarr", ax, ay, ax + size, ay + size, packColor({1.0f, 1.0f, 1.0f}, arrow));
        }
    }
}

// CSWGuiInGameItemEntry (Initialize 006b4db0, SetItem 006b6710). The entry is 56 tall. Its icon sits in a
// 56x56 hex frame at the left (lbl_hex_3; lbl_hex_6 under 100 of a stack, lbl_hex_7 from 100, with the count
// in fnt_d16x16 in the frame's gap), and its name is in a box of the PROTOITEM's border to the right. The
// row and the frame are tinted (0, 0.66, 0.98) at 007a23b4, or the hilight (0.98, 1, 0) at 007a23c0 when the
// row is selected or under the pointer. The box offsets (frame 0..56, name box 59..width, 2..54 down) and
// the count's place are measured on the retail frames ui_inventory and walk 0032 at 1024x768.
void GuiPainter::paintItemEntry(Painting& p, const kotor::GuiControl& proto, const GuiListItem& item, bool lit,
                                bool boxed, const GuiFont* font, float x0, float y0, float x1, float rowHeight)
{
    const float s = p.view.scale;
    const kotor::Vec3 tint = lit ? kotor::Vec3{0.98f, 1.0f, 0.0f} : kotor::Vec3{0.0f, 0.66f, 0.98f};
    // RETAIL-PROVEN (the skillinfo entry, vtable 0x755ed0: Initialize 006ac930 sizes it 42 tall, SetExtent
    // 006ab8e0 gives the lbl_hex_3 frame the 42x42 square at the left and the name box the rest at full height).
    const bool small = rowHeight > 0.0f && rowHeight < 56.0f;
    const float edge = (small ? rowHeight : 56.0f) * s;
    const float boxLeft = small ? x0 + edge : x0 + 59.0f * s;
    const float boxTop = small ? y0 : y0 + 2.0f * s;
    const float boxBottom = small ? y0 + edge : y0 + 54.0f * s;
    if (boxed) {
        // The paper doll of the equip screen leaves the name boxes out (walk 0032): 006b7680 keeps the
        // rows at alpha 0 there; the frame, icon and name still show.
        kotor::GuiBorder box = lit && proto.hilight.present ? proto.hilight : proto.border;
        box.color = tint;
        paintBorder(p, box, proto, boxLeft, boxTop, x1, boxBottom, 1.0f);
    }
    if (!item.icon.empty()) {
        float ix0 = x0;
        float iy0 = y0;
        float ix1 = x0 + edge;
        float iy1 = y0 + edge;
        // RETAIL-PROVEN: the skillinfo entry's icon (006acb40) sets its border's fill style to 1
        // (flags & ~2 | 1), and CSWGuiBorder::Draw 004168c0 draws style 1 with DrawFillCentered
        // 004157f0: along each axis an image smaller than the rectangle keeps its own size, placed
        // (rect - image) / 2 (integer) in; a larger one is stretched to the rectangle. Paired walk
        // 0070_cg17: the feat icon at its native size in the middle of the 42x42 lbl_hex_3 frame.
        if (small) {
            if (const GuiTexture* art = texture(item.icon); art != nullptr && art->width > 0 && art->height > 0) {
                const int box = static_cast<int>(rowHeight);
                if (art->width < box) {
                    ix0 = x0 + static_cast<float>((box - art->width) / 2) * s;
                    ix1 = ix0 + static_cast<float>(art->width) * s;
                }
                if (art->height < box) {
                    iy0 = y0 + static_cast<float>((box - art->height) / 2) * s;
                    iy1 = iy0 + static_cast<float>(art->height) * s;
                }
            }
        }
        paintImage(p, item.icon, ix0, iy0, ix1, iy1, IM_COL32_WHITE);
    }
    const char* frame = item.count < 2 ? "lbl_hex_3" : item.count < 100 ? "lbl_hex_6" : "lbl_hex_7";
    paintImage(p, frame, x0, y0, x0 + edge, y0 + edge, packColor(tint, 1.0f));
    if (item.count >= 2) {
        if (const GuiFont* digits = this->font("fnt_d16x16"); digits != nullptr && digits->ready()) {
            drawText(p.draw, *digits, std::to_string(item.count), x0 + 36.0f * s, y0 + 40.0f * s, x0 + 57.0f * s,
                     y0 + 56.0f * s, 2 | 16, packColor({0.0f, 0.66f, 0.98f}, 1.0f), s, false);
        }
    }
    if (font != nullptr) {
        const kotor::Vec3 color = item.hasColor ? item.color : lit ? tint : proto.textColor;
        const float inset = 3.0f * s;
        drawText(p.draw, *font, item.text, boxLeft + inset, boxTop, x1 - inset, boxBottom,
                 proto.textAlign != 0 ? proto.textAlign : 18, packColor(color, 1.0f), s, true);
    }
}

float GuiPainter::borderInnerInset(const kotor::GuiBorder& border)
{
    if (border.dimension != 0) {
        return static_cast<float>(border.dimension);
    }
    if (!border.edge.empty() && !border.corner.empty()) {
        if (const GuiTexture* edge = texture(border.edge)) {
            return static_cast<float>(edge->height);
        }
    }
    return 0.0f;
}

void GuiPainter::paintListBox(Painting& p, const kotor::GuiControl& control, const GuiControlState& state)
{
    const GuiView& v = p.view;
    if (control.proto.empty()) {
        return;
    }
    const kotor::GuiControl& proto = control.proto.front();
    const float lx0 = v.pxX(static_cast<float>(control.extent.left));
    const float ly0 = v.pxY(static_cast<float>(control.extent.top));
    const float lx1 = v.pxX(static_cast<float>(control.extent.left + control.extent.width));
    const float ly1 = v.pxY(static_cast<float>(control.extent.top + control.extent.height));
    const std::size_t rows = listRows(control, state.rowHeight);
    const float rowHeight = state.rowHeight > 0.0f ? state.rowHeight : static_cast<float>(proto.extent.height);
    const float rowStep = rowHeight + static_cast<float>(control.padding);
    const GuiFont* itemFont = font(proto.font);
    const bool hasPointer = p.layerStates == nullptr && !p.gui->hovered().empty();
    // A list of plain text taller than its box (a description) is a text view: its wrapped lines
    // scroll one at a time under the scrollbar instead of running past the bottom edge.
    struct ViewLine {
        std::string text;
        const GuiListItem* item = nullptr;
    };
    std::vector<ViewLine> viewLines;
    std::size_t viewVisible = 0;
    // RETAIL-PROVEN (CSWGuiBorder::GetInnerExtent 00415240): a border's inner extent is its rectangle
    // inset by DIMENSION, or with DIMENSION 0 by the edge art's thickness when there are edge and
    // corner art. Text sits in the inner extent (CSWGuiEditbox::Initialize 004183b0 and the text
    // controls hand it to CSWGuiText), and the scrollbar thumb in its track's (004193f0).
    const auto innerInset = [&](const kotor::GuiBorder& border) { return borderInnerInset(border); };
    const float textInset = innerInset(proto.border);
    bool textView = false;  // drawn as lines from the box's top edge
    bool scrolling = false; // and taller than the box
    // Entry boxes (the Messages lists, AddDialogMessages 00626b10): each entry is as tall as its
    // wrapped text plus twice the HILIGHT border's dimension, stacked without padding. The scroll counts
    // text lines (viewLines stands for them), the end of the list sits flush with the box's bottom.
    struct EntryBox {
        std::vector<std::string> lines;
        float top = 0.0f;
        float height = 0.0f;
    };
    std::vector<EntryBox> entryBoxes;
    float entryTotal = 0.0f;
    float entryStep = 0.0f;
    float entryDim = 0.0f;
    float entryRoom = 0.0f;
    std::size_t entryFirst = std::numeric_limits<std::size_t>::max();
    std::size_t entryLast = 0;
    const bool entries = state.entryList && itemFont != nullptr && itemFont->ready() && !state.items.empty();
    if (entries) {
        entryDim = proto.hilight.present ? static_cast<float>(proto.hilight.dimension) : 0.0f;
        entryStep = itemFont->height + itemFont->spacingB;
        entryRoom = static_cast<float>(control.extent.top + control.extent.height - proto.extent.top);
        const float width = static_cast<float>(proto.extent.width) - 2.0f * entryDim;
        for (const GuiListItem& item : state.items) {
            EntryBox box;
            box.lines = wrapLines(*itemFont, item.text, width);
            box.top = entryTotal;
            box.height = static_cast<float>(box.lines.size()) * entryStep + 2.0f * entryDim;
            entryTotal += box.height;
            entryBoxes.push_back(std::move(box));
        }
        // OrganizeUnequal 00419ee0: the scrollbar's value is the first entry shown (+0x2c8), so the
        // scroll counts whole entries. The last first-entry is the one from which every entry after it
        // fits (the end-anchored layout); the TextView reports lines = that + the entries then shown, so
        // GuiSystem's maximum scroll is that entry and a track click pages by the entries visible there.
        std::size_t endFirst = 0;
        for (std::size_t i = 0; i < entryBoxes.size(); ++i) {
            if (entryTotal - entryBoxes[i].top <= entryRoom) {
                endFirst = i;
                break;
            }
            endFirst = i + 1 < entryBoxes.size() ? i + 1 : i;
        }
        viewVisible = std::max<std::size_t>(1, entryBoxes.size() - endFirst);
        viewLines.assign(endFirst + viewVisible, ViewLine{});
        scrolling = endFirst > 0;
    }
    if (!entries && itemFont != nullptr && itemFont->ready() && state.rowHeight <= 0.0f && state.pixelScroll == 0.0f
        && !state.items.empty() && control.tag != "LB_CREDITS") {
        bool plain = true;
        for (const GuiListItem& item : state.items) {
            plain = plain && !item.selectable && item.icon.empty();
        }
        if (plain) {
            const float width = static_cast<float>(proto.extent.width) - 2.0f * textInset;
            const float step = itemFont->height + itemFont->spacingB;
            const float room = static_cast<float>(control.extent.top + control.extent.height - proto.extent.top);
            viewVisible = static_cast<std::size_t>(std::max(1.0f, std::floor(room / std::max(1.0f, step))));
            bool tallerThanRow = false;
            for (const GuiListItem& item : state.items) {
                std::vector<std::string> wrapped = wrapLines(*itemFont, item.text, width);
                tallerThanRow = tallerThanRow
                    || static_cast<float>(wrapped.size()) * step > static_cast<float>(proto.extent.height) + 2.0f;
                for (std::string& line : wrapped) {
                    viewLines.push_back({std::move(line), &item});
                }
            }
            textView = tallerThanRow;
            scrolling = textView && viewLines.size() > viewVisible;
        }
    }
    if (p.layerStates == nullptr) {
        p.gui->setTextView(control.key, scrolling ? viewLines.size() : 0, viewVisible);
    }
    const std::size_t scrollCount = scrolling ? viewLines.size() : state.items.size();
    const std::size_t scrollVisible = scrolling ? viewVisible : rows;
    const std::size_t scrollMax = entries ? viewLines.size() - viewVisible
                                          : (scrollCount > scrollVisible ? scrollCount - scrollVisible : 0);
    const std::size_t scrollTop = entries && state.pinEnd ? scrollMax : std::min(state.scroll, scrollMax);
    p.draw->PushClipRect(ImVec2(lx0, ly0), ImVec2(lx1 + 1.0f, ly1 + 1.0f), true);
    if (textView) {
        // RETAIL-PROVEN (CSWGuiListBox::OrganizeControls 0041b140 to OrganizeOversized): a text item
        // taller than the box is placed whole and the box clips it, so a part of the line below
        // the last whole one shows (paired frame levelup_skills).
        const float step = itemFont->height + itemFont->spacingB;
        const float inset = textInset * v.scale;
        const float room = static_cast<float>(control.extent.top + control.extent.height - proto.extent.top);
        for (std::size_t i = 0; static_cast<float>(i) * step < room && scrollTop + i < viewLines.size(); ++i) {
            const ViewLine& line = viewLines[scrollTop + i];
            const float x0 = v.pxX(static_cast<float>(proto.extent.left));
            const float x1 = v.pxX(static_cast<float>(proto.extent.left + proto.extent.width));
            const float y0 = v.pxY(static_cast<float>(proto.extent.top) + step * static_cast<float>(i));
            const float y1 = v.pxY(static_cast<float>(proto.extent.top) + step * static_cast<float>(i + 1));
            kotor::Vec3 color = line.item->hasColor ? line.item->color : proto.textColor;
            const int align = (proto.textAlign != 0 ? proto.textAlign : 17) & 7;
            clipExpected_ = static_cast<float>(i + 1) * step > room;
            drawText(p.draw, *itemFont, line.text, x0 + inset, y0, x1 - inset, y1, (align != 0 ? align : 1) | 16,
                     packColor(color, 1.0f), v.scale, false);
            clipExpected_ = false;
        }
    }
    if (entries) {
        // The list starts at its first shown entry (scrollTop, an entry index; at the end it is the first
        // entry from which all the rest fit, paired walk out_ig6 0036_menus_messages).
        const float offset = scrollTop < entryBoxes.size() ? entryBoxes[scrollTop].top : 0.0f;
        // RETAIL-PROVEN (CSWGuiListBox::OrganizeUnequal 00419ee0): the scrollbar of a list of unequal entries counts
        // entries: visible = the whole entries shown (first to last), max = count - visible + 1, cur = the first shown.
        for (std::size_t i = 0; i < entryBoxes.size(); ++i) {
            const EntryBox& box = entryBoxes[i];
            if (box.top >= offset && box.top + box.height - offset <= entryRoom) {
                entryFirst = std::min(entryFirst, i);
                entryLast = i;
            }
        }
        std::vector<GuiSystem::EntryRect> entryRects;
        for (std::size_t i = 0; i < entryBoxes.size(); ++i) {
            const EntryBox& box = entryBoxes[i];
            const float top = static_cast<float>(proto.extent.top) + box.top - offset;
            if (top + box.height <= static_cast<float>(proto.extent.top)
                || top >= static_cast<float>(proto.extent.top) + entryRoom) {
                continue;
            }
            entryRects.push_back({i, top, box.height});
            const float x0 = v.pxX(static_cast<float>(proto.extent.left));
            const float x1 = v.pxX(static_cast<float>(proto.extent.left + proto.extent.width));
            const bool picked = i == state.listSelection;
            // The picked entry is lit like a button's frame and text (0078d3c0: 0.98, 1, 0).
            const kotor::Vec3 lit{0.98f, 1.0f, 0.0f};
            if (picked && proto.hilight.present) {
                kotor::GuiBorder frame = proto.hilight;
                frame.color = lit;
                paintBorder(p, frame, proto, x0, v.pxY(top), x1, v.pxY(top + box.height), 1.0f);
            }
            kotor::Vec3 color = state.items[i].hasColor ? state.items[i].color : proto.textColor;
            if (picked) {
                color = lit;
            }
            const int align = (proto.textAlign != 0 ? proto.textAlign : 17) & 7;
            for (std::size_t line = 0; line < box.lines.size(); ++line) {
                const float y = top + entryDim + static_cast<float>(line) * entryStep;
                drawText(p.draw, *itemFont, box.lines[line], x0 + entryDim * v.scale, v.pxY(y),
                         x1 - entryDim * v.scale, v.pxY(y + entryStep), (align != 0 ? align : 1) | 16,
                         packColor(color, 1.0f), v.scale, false);
            }
        }
        if (p.layerStates == nullptr) {
            p.gui->setEntryRects(control.key, std::move(entryRects));
        }
    }
    for (std::size_t i = 0; !textView && !entries && i < rows && state.scroll + i < state.items.size(); ++i) {
        const std::size_t index = state.scroll + i;
        const GuiListItem& item = state.items[index];
        float rowOffset = rowStep * static_cast<float>(i) - state.pixelScroll;
        float slotHeight = rowHeight;
        if (state.pixelScroll == 0.0f) {
            const GuiRowSlot slot = guiListRowSlot(control, rowHeight, rows, i);
            rowOffset = slot.top;
            slotHeight = slot.height;
        }
        const float x0 = v.pxX(static_cast<float>(proto.extent.left));
        const float y0 = v.pxY(static_cast<float>(proto.extent.top) + rowOffset);
        const float x1 = v.pxX(static_cast<float>(proto.extent.left + proto.extent.width));
        const float y1 = v.pxY(static_cast<float>(proto.extent.top) + slotHeight + rowOffset);
        if (!item.cells.empty()) {
            paintFeatRow(p, item, index == state.listSelection ? state.listCell : -1, x0, y0);
            continue;
        }
        if (control.tag == "LST_EventList" && itemFont != nullptr) {
            // RETAIL-PROVEN (CSWGuiKeyMapButton::SetExtent 006ec250, Draw 006ec350; CreateOption 006ed610): a key row
            // is two boxes. The name box is the row's width less 0xc5 (197); the key box starts 2 further right and is
            // 0xc3 (195) wide, ending at the row's right edge. Both frame with the item style and place their text in
            // the border's inner extent (DIMENSION 14). Only the row under the pointer is lit (SetActive 00418e00:
            // HILIGHT frame, text 0078d3c0), and the key box is drawn apart from it. Paired: route_pairs
            // 0010_gameplay_keymap.
            const bool lit = hasPointer && p.gui->hovered() == control.key
                && p.gui->listItemAt(control.key, p.gui->pointerX(), p.gui->pointerY()) == static_cast<int>(index);
            const int nameWidth = proto.extent.width - 197;
            const float xName = v.pxX(static_cast<float>(proto.extent.left + nameWidth));
            const float xKey = v.pxX(static_cast<float>(proto.extent.left + nameWidth + 2));
            const kotor::GuiBorder& frame = lit && proto.hilight.present ? proto.hilight : proto.border;
            paintBorder(p, frame, proto, x0, y0, xName, y1, 1.0f);
            paintBorder(p, proto.border, proto, xKey, y0, x1, y1, 1.0f);
            const float dim = static_cast<float>(proto.border.dimension) * v.scale;
            const int keyAlign = proto.textAlign != 0 ? proto.textAlign : 17;
            const kotor::Vec3 nameColor = lit ? kotor::Vec3{0.98f, 1.0f, 0.0f} : proto.textColor;
            drawText(p.draw, *itemFont, item.text, x0 + dim, y0, xName - dim, y1, keyAlign, packColor(nameColor, 1.0f),
                     v.scale, true);
            drawText(p.draw, *itemFont, item.tag, xKey + dim, y0, x1 - dim, y1, keyAlign,
                     packColor(proto.textColor, 1.0f), v.scale, true);
            continue;
        }
        const bool checkRow = item.check >= 0;
        const bool selected = checkRow ? item.check == 1 : index == state.listSelection && item.selectable;
        bool hovered = checkRow && index == state.listSelection;
        if (hasPointer && p.gui->hovered() == control.key) {
            hovered = p.gui->listItemAt(control.key, p.gui->pointerX(), p.gui->pointerY())
                == static_cast<int>(index) || hovered;
        }
        const kotor::GuiBorder* border = &proto.border;
        if (selected && hovered && proto.hilightSelected.present) {
            border = &proto.hilightSelected;
        } else if (selected && proto.selectedBorder.present) {
            border = &proto.selectedBorder;
        } else if ((hovered || selected) && proto.hilight.present) {
            border = &proto.hilight;
        }
        // The save and load entries are buttons (CSWGuiSaveLoadEntry): a lit one takes the highlight
        // constant 0078d3c0 (0.98, 1, 0) for its frame and its text, whatever the file's HILIGHT colour
        // (white in saveload.gui); paired walk out_ig6 0000_load_list.
        // An idle entry's frame takes the text colour (0, 0.66, 0.98), not the file's white.
        const bool saveEntry = control.tag == "LB_GAMES";
        const bool litEntry = saveEntry && (hovered || selected) && proto.hilight.present;
        kotor::GuiBorder litBorder;
        if (saveEntry) {
            litBorder = *border;
            litBorder.color = litEntry ? kotor::Vec3{0.98f, 1.0f, 0.0f} : proto.textColor;
            border = &litBorder;
        }
        // A lit save or load entry is a lit button: SetActive 00418e00 starts the shared text pulse, which the
        // frame and the text both follow (retail frames: the lit Auto Save entry reads 0.69 to 0.86 of the
        // constant at different times, frame and text together).
        const float entryPulse = litEntry && p.options != nullptr ? guiPulse(p.options->timeSeconds, 0.4f) : 1.0f;
        if (item.itemEntry) {
            paintItemEntry(p, proto, item, state.enabled && (hovered || selected), state.enabled, itemFont, x0, y0, x1,
                           state.rowHeight);
            continue;
        }
        if (checkRow) {
            // The ring as the CB_ controls draw it: its size at the row's left, in the label's colour.
            const float edge = kCheckboxEdge * v.scale;
            const float top = 0.5f * (y0 + y1 - edge);
            kotor::Vec3 ring = proto.textColor;
            if (item.hasColor) {
                ring = item.color;
            } else if ((hovered || selected) && hovered && proto.hilight.present) {
                ring = proto.hilight.color;
            }
            paintImage(p, border->fill, x0, top, x0 + edge, top + edge, packColor(ring, 1.0f));
        } else {
            paintBorder(p, *border, proto, x0, y0, x1, y1, entryPulse);
        }
        float textX0 = checkRow ? x0 + kCheckboxEdge * v.scale : x0;
        if (!item.icon.empty()) {
            const float size = (y1 - y0);
            paintImage(p, item.icon, x0, y0, x0 + size, y1, IM_COL32_WHITE);
            textX0 = x0 + size + 2.0f * v.scale;
        }
        if (itemFont != nullptr) {
            kotor::Vec3 color = proto.textColor;
            if (item.hasColor) {
                color = item.hasHoverColor && hasPointer && p.gui->hovered() == control.key
                        && p.gui->listItemAt(control.key, p.gui->pointerX(), p.gui->pointerY()) == static_cast<int>(index)
                    ? item.hoverColor : item.color;
            } else if (checkRow ? hovered && proto.hilight.present : (hovered || selected) && proto.hilight.present) {
                color = litEntry ? kotor::Vec3{0.98f, 1.0f, 0.0f} : proto.hilight.color; // a ticked row keeps the blue label (paired frame opt_feedback)
            }
            const float inset = 3.0f * v.scale;
            drawText(p.draw, *itemFont, item.text, textX0 + inset, y0, x1 - inset, y1,
                     proto.textAlign != 0 ? proto.textAlign : 17, packColor(color, entryPulse), v.scale, true);
        }
    }
    p.draw->PopClipRect();
    // RETAIL-PROVEN: the scrollbar child is drawn whether or not the rows overflow. The list box
    // constructor 0041be40 sets flag 0x20 (in 0x183a) at +0x2bc and the draw 0041a3e0 draws the
    // scrollbar at +0x104 on that flag alone.
    // RETAIL-PROVEN (CSWGuiScrollBar::SetExtent 004193f0): the arrows take the ends (their height,
    // at most half the bar); the bar's BORDER frames only the track between them; the thumb fills
    // the track border's inner extent across, and down it is visible / (visible + max) of it,
    // truncated, at least 4, placed at (inner - thumb) * cur / (max - 1). With max 1 or less it
    // fills the inner extent. The list box sets the values (CSWGuiListBox::OrganizeControls
    // 0041b140): rows max = count - fit + 1 (at least 1) and visible = min(fit, count); a text item
    // taller than the box counts pixels, max = item height - box height + 1 and visible = box height.
    for (const kotor::GuiControl& child : control.children) {
        if (child.type == 9 && !state.hideScrollbar) {
            const float cx0 = v.pxX(static_cast<float>(child.extent.left));
            const float cy0 = v.pxY(static_cast<float>(child.extent.top));
            const float cx1 = v.pxX(static_cast<float>(child.extent.left + child.extent.width));
            const float cy1 = v.pxY(static_cast<float>(child.extent.top + child.extent.height));
            float arrowUnits = static_cast<float>(child.extent.width);
            if (2.0f * arrowUnits > static_cast<float>(child.extent.height)) {
                arrowUnits = std::floor(0.5f * static_cast<float>(child.extent.height));
            }
            const float arrow = arrowUnits * v.scale;
            if (!child.dir.image.empty()) {
                // RETAIL-PROVEN (CSWGuiListBox::Draw 0041a3e0 with its default flags 0x183a from 0041be40): the scrollbar's
                // arrows take the hilight pulse as alpha; the thumb and track stay. Paired walk 0057 (dim) and 0058 (bright).
                const ImU32 arrowColor = packColor({1.0f, 1.0f, 1.0f}, guiPulse(p.options->timeSeconds, 0.2f));
                paintImage(p, child.dir.image, cx0, cy0, cx1, cy0 + arrow, arrowColor);
                paintImage(p, child.dir.image, cx0, cy1 - arrow, cx1, cy1, arrowColor, true);
            }
            const float trackTop = cy0 + arrow;
            const float trackBottom = cy1 - arrow;
            paintBorder(p, child.border, child, cx0, trackTop, cx1, trackBottom, 1.0f);
            const float trackInset = innerInset(child.border);
            const int innerW = child.extent.width - 2 * static_cast<int>(trackInset);
            const int innerH = static_cast<int>(child.extent.height - 2.0f * arrowUnits) - 2 * static_cast<int>(trackInset);
            if (innerW <= 0 || innerH <= 0 || child.thumb.image.empty()) {
                continue;
            }
            long long maxValue = 1;
            long long visibleValue = 1;
            long long current = 0;
            if (entries && entryFirst != std::numeric_limits<std::size_t>::max()) {
                const long long count = static_cast<long long>(entryBoxes.size());
                visibleValue = static_cast<long long>(entryLast - entryFirst + 1);
                maxValue = std::max<long long>(1, count - visibleValue + 1);
                current = static_cast<long long>(entryFirst);
            } else if (scrolling) {
                const float step = itemFont->height + itemFont->spacingB;
                const long long box = control.extent.height - 2 * static_cast<int>(innerInset(control.border));
                const long long item = static_cast<long long>(static_cast<float>(viewLines.size()) * step);
                maxValue = std::max<long long>(1, item - box + 1);
                visibleValue = std::max<long long>(1, box);
                current = std::min<long long>(static_cast<long long>(static_cast<float>(scrollTop) * step), maxValue - 1);
            } else if (!textView) {
                const long long count = static_cast<long long>(state.items.size());
                const long long fit = static_cast<long long>(rows);
                maxValue = std::max<long long>(1, count - fit + 1);
                visibleValue = std::max<long long>(1, std::min(fit, count));
                current = static_cast<long long>(scrollTop);
            }
            int thumbH = innerH;
            int thumbY = 0;
            if (maxValue > 1) {
                thumbH = std::max(4, static_cast<int>(static_cast<double>(visibleValue)
                                                      / static_cast<double>(visibleValue + maxValue) * innerH));
                thumbY = static_cast<int>((innerH - thumbH) * current / (maxValue - 1));
            }
            const float tx0 = cx0 + trackInset * v.scale;
            const float ty0 = trackTop + (trackInset + static_cast<float>(thumbY)) * v.scale;
            paintImage(p, child.thumb.image, tx0, ty0, tx0 + static_cast<float>(innerW) * v.scale,
                       ty0 + static_cast<float>(thumbH) * v.scale, IM_COL32_WHITE);
        }
    }
}

void GuiPainter::paintControl(Painting& p, const kotor::GuiControl& control)
{
    const GuiControlState state = p.stateOf(control);
    if (!state.visible) {
        return;
    }
    if (p.view.anchored && !p.anchorApplied) {
        // ENGINE CHOICE for modern aspects, following how retail's HUD files anchor: the
        // control moves with the window edge it is nearest to.
        const GuiView saved = p.view;
        float shiftX = 0.0f;
        float shiftY = 0.0f;
        guiAnchorShift(saved, control, shiftX, shiftY);
        p.view.offsetX = saved.offsetX + shiftX * saved.scale;
        p.view.offsetY = saved.offsetY + shiftY * saved.scale;
        p.anchorApplied = true;
        paintControl(p, control);
        p.anchorApplied = false;
        p.view = saved;
        return;
    }
    if (control.clipped) {
        // A laid-in sub-panel's control: drawn inside the sub-panel's rectangle only.
        const GuiView& cv = p.view;
        p.draw->PushClipRect(ImVec2(cv.pxX(static_cast<float>(control.clip.left)), cv.pxY(static_cast<float>(control.clip.top))),
                             ImVec2(cv.pxX(static_cast<float>(control.clip.left + control.clip.width)),
                                    cv.pxY(static_cast<float>(control.clip.top + control.clip.height))),
                             true);
        kotor::GuiControl inner = control;
        inner.clipped = false;
        paintControl(p, inner);
        p.draw->PopClipRect();
        return;
    }
    const GuiView& v = p.view;
    float x0 = v.pxX(static_cast<float>(control.extent.left) + state.offsetX);
    float y0 = v.pxY(static_cast<float>(control.extent.top) + state.offsetY);
    float x1 = v.pxX(static_cast<float>(control.extent.left + control.extent.width) + state.offsetX);
    float y1 = v.pxY(static_cast<float>(control.extent.top + control.extent.height) + state.offsetY);
    if (p.options != nullptr && p.options->controlHook
        && p.options->controlHook(control, x0, y0, x1, y1)) {
        return;
    }
    ++stats_.controls;
    struct TagScope {
        std::string& slot;
        std::string saved;
        ~TagScope() { slot = saved; }
    } tagScope{auditTag_, auditTag_};
    auditTag_ = control.tag.empty() ? control.key : control.tag;
    const int glyphs0 = stats_.glyphs;
    const int images0 = stats_.images + stats_.borderArt;
    const int missing0 = stats_.missingImages;
    const bool hovered = p.hovered(control) || state.marked;
    const float alpha = control.alpha * state.alpha; // the screen's fade (the HUD combat line, 00686ba0)
    // A control spanning the whole panel (mainmenu LBL_3DVIEW, the chargen MAIN_PNL) is drawn at its
    // extent like any other: retail places every panel 1:1 (0040ba20, 0040b690) and draws nothing of a
    // panel outside it; the window around it is the backdrop art (see drawWindowBackdrop).
    kotor::GuiBorder border = control.border;
    if (control.type == 7 || state.selected) {
        if (state.selected && hovered && control.hilightSelected.present) {
            border = control.hilightSelected;
        } else if (state.selected && control.selectedBorder.present) {
            border = control.selectedBorder;
        } else if (hovered && control.hilight.present) {
            border = control.hilight;
        }
    } else if (hovered && control.hilight.present && control.type != 8) {
        border = control.hilight;
    }
    if (state.borderPulse) {
        border.pulsing = true; // see paintBorder: the shared pulse replaces the border's alpha
    }
    bool hilightOver = false;
    if (state.hasFill) {
        if (state.hilightOverFill && control.hilight.present) {
            hilightOver = hovered;
            border = control.border;
        }
        border.fill = state.fill;
    }
    if (state.hasFillColor) {
        border.color = state.fillColor;
    }

    switch (control.type) {
    case 11:
        paintBorder(p, border, control, x0, y0, x1, y1, alpha);
        paintListBox(p, control, state);
        auditEmpty(control, state, glyphs0, images0, missing0);
        return; // the scrollbar child is drawn by the list box
    case 9:
        // standalone scrollbars (not under a list box)
        paintScrollbar(p, control, state, nullptr);
        return;
    case 10: {
        paintBorder(p, control.border, control, x0, y0, x1, y1, alpha);
        const float ratio = control.maxValue > 0.0f ? std::clamp(state.value / control.maxValue, 0.0f, 1.0f) : 0.0f;
        // A set fill replaces the bar's PROGRESS art (retail 006859e0 swaps ENEMY_BAR / FRIEND_BAR in).
        const std::string& fill = state.hasFill ? state.fill : control.progress.fill;
        const float width = x1 - x0;
        const float bx0 = control.startFromLeft ? x0 : x1 - width * ratio;
        const float bx1 = control.startFromLeft ? x0 + width * ratio : x1;
        if (!fill.empty() && ratio > 0.0f) {
            paintImage(p, fill, bx0, y0, bx1, y1, IM_COL32_WHITE);
        }
        return;
    }
    case 8: {
        paintBorder(p, hovered && control.hilight.present ? control.hilight : control.border, control,
                    x0, y0, x1, y1, alpha);
        if (!control.thumb.image.empty()) {
            if (const GuiTexture* thumb = texture(control.thumb.image)) {
                const float ratio = control.maxValue > 0.0f
                    ? std::clamp(state.value / control.maxValue, 0.0f, 1.0f) : 0.0f;
                const float thumbW = static_cast<float>(thumb->width) * v.scale;
                const float thumbH = static_cast<float>(thumb->height) * v.scale;
                // A slider taller than wide (the character sheet's SLD_ALIGN) runs top to bottom: 006afda0 sets its
                // value with 00417ee0, which places the thumb down the track from the top (paired cs_al10, cs_al90).
                const bool vertical = control.extent.height > control.extent.width;
                const float cx = vertical ? 0.5f * (x0 + x1) : x0 + 0.5f * thumbW + ratio * (x1 - x0 - thumbW);
                const float cy = vertical ? y0 + 0.5f * thumbH + ratio * (y1 - y0 - thumbH) : 0.5f * (y0 + y1);
                beginArt(p.draw, *thumb);
                p.draw->AddImage(static_cast<ImTextureID>(thumb->id),
                                 ImVec2(cx - 0.5f * thumbW, cy - 0.5f * thumbH),
                                 ImVec2(cx + 0.5f * thumbW, cy + 0.5f * thumbH));
                endArt(p.draw, *thumb);
            }
        }
        return;
    }
    default:
        break;
    }

    float textLeft = x0;
    // A checkbox whose fill art is much narrower than the control is an indicator ring; one whose
    // art fills its box (the party selection's 85x85 portrait buttons are type 7 too) is drawn whole.
    const GuiTexture* indicatorArt = nullptr;
    if (control.type == 7 && !border.fill.empty()) {
        if (const GuiTexture* art = texture(border.fill);
            art != nullptr && control.extent.width >= 3 * std::max(1, art->width)) {
            indicatorArt = art;
        }
    }
    if (control.type == 7 && indicatorArt != nullptr) {
        // Paired frames opt_gameplay and opt_feedback (pixels of the retail frame at 1:1): the ring's opaque
        // part is 18 wide at 3 from the control's left and centred; the art's is 23 of 32, so the whole
        // art draws kCheckboxEdge square at the left. It takes the label's colour, blue, or the
        // highlight's yellow on the focused row; the art's own white never shows.
        if (!border.fill.empty()) {
            if (const GuiTexture* art = indicatorArt) {
                const float w = kCheckboxEdge * v.scale;
                const float h = w;
                const float iy = 0.5f * (y0 + y1 - h);
                kotor::Vec3 ring = optionsCheckboxColor(hovered, state.enabled);
                if (state.hasColor) {
                    ring = state.color;
                }
                const unsigned tint = packColor(ring, alpha);
                paintImage(p, border.fill, x0, iy, x0 + w, iy + h, tint);
                Stats::Placed placed;
                placed.tag = control.tag;
                placed.x0 = x0;
                placed.y0 = iy;
                placed.x1 = x0 + w;
                placed.y1 = iy + h;
                placed.nativeWidth = art->width;
                placed.nativeHeight = art->height;
                placed.flat = art->flat;
                placed.tint = tint;
                stats_.indicators.push_back(std::move(placed));
                textLeft = x0 + w + 4.0f * v.scale;
            }
        }
    } else {
        paintBorder(p, border, control, x0, y0, x1, y1, alpha);
        if (hilightOver) {
            paintBorder(p, control.hilight, control, x0, y0, x1, y1, alpha);
        }
    }

    const std::string text = controlText(control, state);
    if (!text.empty()) {
        if (const GuiFont* f = font(control.font)) {
            kotor::Vec3 color = control.hasTextColor ? control.textColor : kotor::Vec3{1.0f, 1.0f, 1.0f};
            if (state.hasColor) {
                color = state.color;
            } else if (indicatorArt != nullptr) {
                color = optionsCheckboxColor(hovered, state.enabled);
            } else if (hovered && control.type == 6) {
                // RETAIL-PROVEN: CSWGuiButton::SetActive 00418e00 sets the text colour to (0.98, 1, 0) at
                // 0078d3c0 while the button is lit, whatever the file's HILIGHT colour (confirm.gui's is
                // near white). Paired: minigame_pazaak_set_result (retail OK label yellow).
                color = {0.98f, 1.0f, 0.0f};
            } else if (hovered && control.hilight.present) {
                color = control.hilight.color;
            }
            float textAlpha = alpha;
            if (!state.enabled && indicatorArt == nullptr) {
                color = {color.x * 0.5f, color.y * 0.5f, color.z * 0.5f};
            }
            const bool litButton = hovered && control.type == 6 && state.enabled && !state.hasColor && indicatorArt == nullptr;
            if (state.textPulse || litButton) {
                // RETAIL-PROVEN (SetActive 00418e00 sets the text flag 0x40, CSWGuiText::Draw 00416240 then takes the
                // shared pulse as its alpha, so a lit button's label pulses whether the pointer or the panel's focus
                // lit it: retail New Game 0.52 / 0.93 / 0.79 alpha in main-menu startup, hover and chargen frames).
                // RETAIL-PROVEN (UpdateGuiPulse 00414a50, 00416240): one shared triangle, 1.0 down to 0.4 over 0.75 s and
                // back up over the next 0.75 s (constants 0073e320 = 0.4, 0073d764 = 1.0, 0073e31c = 0.75); a pulsing
                // text takes it as its alpha. Paired walk 0050/0052: the focused skill row dims and brightens.
                textAlpha = guiPulse(p.options->timeSeconds, 0.4f);
            } else if (control.pulsingText && hovered) {
                textAlpha *= 0.75f + 0.25f * std::sin(static_cast<float>(p.options->timeSeconds) * 6.0f);
            }
            // RETAIL-PROVEN (CSWGuiLabel text through CSWGuiText::SetExtent 00416280): a label's text sits
            // in its border's inner extent (00415240), inset by DIMENSION or the edge art, nothing more;
            // skillinfo's LBL_MESSAGE (DIMENSION 0, no edge) wraps at its full 310 (walk 0070_cg17). The
            // old fixed 2 px made it wrap a word early. Buttons keep their full rectangle (not yet
            // checked against the button path).
            const float inset = control.type == 6 ? 0.0f : borderInnerInset(control.border) * v.scale;
            const int textAlign = state.textAlign != 0 ? state.textAlign : control.textAlign != 0 ? control.textAlign : 18;
            // computercamera.gui's LBL_RETURN is the third CSWGuiBlackenedLabel (vtable 00753cc0 in its constructor
            // 006a95f0, "Press 'Enter' to Cancel Live Feed and Return to Interface.", strref 48226).
            const bool blackened = control.type == 4
                && (control.tag == "LBL_LIGHT" || control.tag == "LBL_DARK"
                    || (control.tag == "LBL_RETURN" && p.screen != nullptr && p.screen->name.rfind("computercamera", 0) == 0));
            if (blackened) {
                // RETAIL-PROVEN (CSWGuiBlackenedLabel::Draw 006aeac0, vtable 00753cc0, built for these two labels by the
                // character sheet 006b0e40 at 006b117b / 006b1193, and for LBL_RETURN by computercamera 006a95f0): the text colour is set to the black at 00833910 and
                // the text drawn four times with its extent moved (-1,+1), (+1,+1), (+1,-1), (-1,-1) at the same alpha,
                // then the colour and extent are put back and CSWGuiLabel::Draw 00417750 draws the real text on top. That
                // is the 1 px black outline (and the black under the strokes) on the retail Light / Dark labels.
                // OPEN: the black's value at 00833910 is a runtime global (0, 0, 0 by the retail frames); the same class
                // also serves the floating text (Application.cpp paintFloatingText) and computercamera's LBL_RETURN.
                const unsigned black = packColor({0.0f, 0.0f, 0.0f}, textAlpha);
                for (const std::pair<float, float>& shift : {std::pair{-1.0f, 1.0f}, std::pair{1.0f, 1.0f},
                                                              std::pair{1.0f, -1.0f}, std::pair{-1.0f, -1.0f}}) {
                    const float dx = shift.first * v.scale;
                    const float dy = shift.second * v.scale;
                    drawText(p.draw, *f, text, textLeft + inset + dx, y0 + inset + dy, x1 - inset + dx, y1 - inset + dy,
                             textAlign, black, v.scale, true);
                }
            }
            drawText(p.draw, *f, text, textLeft + inset, y0 + inset, x1 - inset, y1 - inset, textAlign,
                     packColor(color, textAlpha), v.scale, true);
            if (control.type == 7 && textBounds_[2] > textBounds_[0]) {
                Stats::Placed placed;
                placed.tag = control.tag;
                placed.x0 = textBounds_[0];
                placed.y0 = textBounds_[1];
                placed.x1 = textBounds_[2];
                placed.y1 = textBounds_[3];
                stats_.labels.push_back(std::move(placed));
            }
        }
    }
    auditEmpty(control, state, glyphs0, images0, missing0);
    for (const kotor::GuiControl& child : control.children) {
        paintControl(p, child);
    }
}

void GuiPainter::auditEmpty(const kotor::GuiControl& control, const GuiControlState& state, int glyphs0, int images0,
                            int missing0)
{
    if (!auditOn_ || control.tag.empty()) {
        return;
    }
    const std::string& tag = control.tag;
    const bool dataName = tag.rfind("LBL_", 0) == 0 || tag.rfind("BTN_", 0) == 0 || tag.rfind("LB_", 0) == 0
        || tag.rfind("CB_", 0) == 0;
    if (control.type == 11) {
        if (state.items.empty() && tag.rfind("LB_", 0) == 0) {
            note("empty_list", "list has no items");
        }
        return;
    }
    const bool drew = stats_.glyphs != glyphs0 || stats_.images + stats_.borderArt != images0 || stats_.missingImages != missing0;
    if (drew) {
        return;
    }
    const bool hasData = state.hasText || control.strref != 0xFFFFFFFFu || !control.text.empty()
        || !control.border.fill.empty() || !control.hilight.fill.empty() || state.hasFill;
    if (dataName || hasData) {
        note("empty", concat("type ", control.type, hasData ? " has data source" : " named like data", " text '",
                             controlText(control, state).substr(0, 20), "' strref ", control.strref, " fill '",
                             control.border.fill, "' font '", control.font, "'"));
    }
}

// The surround retail draws around a panel: "<W>x<H><kind>" for the window's mode (0040a3e0 names
// 1024x768, 1280x960, 1280x1024 and 1600x1200; every other size is 800x600), the window filled 1:1
// at those sizes (paired frame menu_main: 1024x768back). ENGINE CHOICE for a window that is none of
// retail's modes: the smallest art at least as large as the window, else the largest, drawn at the
// full height (full width for a window narrower than 4:3) with its outermost pixels repeated.
void GuiPainter::drawWindowBackdrop(ImDrawList* draw, float windowWidth, float windowHeight,
                                    const std::vector<std::array<float, 4>>& open, const std::string& kind)
{
    struct Candidate {
        const char* size;
        float width;
        float height;
    };
    static constexpr Candidate kBackdrops[] = {
        {"800x600", 800.0f, 600.0f},     {"1024x768", 1024.0f, 768.0f},   {"1280x960", 1280.0f, 960.0f},
        {"1280x1024", 1280.0f, 1024.0f}, {"1600x1200", 1600.0f, 1200.0f},
    };
    const Candidate* chosen = nullptr;
    for (const Candidate& candidate : kBackdrops) {
        if (candidate.width == windowWidth && candidate.height == windowHeight) {
            chosen = &candidate;
        }
    }
    if (chosen == nullptr) {
        chosen = &kBackdrops[4];
        for (const Candidate& candidate : kBackdrops) {
            if (candidate.width >= windowWidth && candidate.height >= windowHeight
                && candidate.width * 3.0f == candidate.height * 4.0f) {
                chosen = &candidate;
                break;
            }
        }
    }
    const std::string name = std::string(chosen->size) + (kind.empty() ? std::string("back") : kind);
    const GuiTexture* tex = texture(name);
    if (tex == nullptr || tex->width <= 0 || tex->height <= 0) {
        return;
    }
    stats_.backdrop = name;
    if (const bool exact = chosen->width == windowWidth && chosen->height == windowHeight; exact) {
        const ImTextureID id = static_cast<ImTextureID>(tex->id);
        for (const auto& rect : open) {
            draw->PushClipRect(ImVec2(rect[0], rect[1]), ImVec2(rect[2], rect[3]), true);
            draw->AddImage(id, ImVec2(0.0f, 0.0f), ImVec2(windowWidth, windowHeight));
            draw->PopClipRect();
        }
        return;
    }
    // The load screen art is a picture with no panel in it: it keeps the fit below.
    if (kind != "load" && chosen->width >= windowWidth && chosen->height >= windowHeight) {
        // The art is drawn for a panel at true size in the middle of its own resolution (0040ba20 puts
        // every panel there), and some of it carries that panel's surround (1280x960pazaak, the store,
        // the galaxy map). Scaling it to fit a window of another shape moves that surround off the
        // panel: at 1280x720 a 0.75 fit left the 640x480 lbl_pazaak fill as a grey square over a smaller
        // painted frame (MINIGAME pair mg_pairs\pazaak_table_pair_new.png). Unscaled and centred, it lines
        // up with the panel and is cropped at the window edge.
        const ImTextureID id = static_cast<ImTextureID>(tex->id);
        const float x0 = std::floor(0.5f * (windowWidth - chosen->width));
        const float y0 = std::floor(0.5f * (windowHeight - chosen->height));
        for (const auto& rect : open) {
            draw->PushClipRect(ImVec2(rect[0], rect[1]), ImVec2(rect[2], rect[3]), true);
            draw->AddImage(id, ImVec2(x0, y0), ImVec2(x0 + chosen->width, y0 + chosen->height));
            draw->PopClipRect();
        }
        return;
    }
    const ImTextureID id = static_cast<ImTextureID>(tex->id);
    const float aspect = static_cast<float>(tex->width) / static_cast<float>(tex->height);
    float cx0 = 0.0f;
    float cy0 = 0.0f;
    float cx1 = windowWidth;
    float cy1 = windowHeight;
    if (windowWidth / windowHeight >= aspect) {
        const float width = windowHeight * aspect;
        cx0 = 0.5f * (windowWidth - width);
        cx1 = cx0 + width;
    } else {
        const float height = windowWidth / aspect;
        cy0 = 0.5f * (windowHeight - height);
        cy1 = cy0 + height;
    }
    const float edgeU = 1.5f / static_cast<float>(tex->width);
    const float edgeV = 1.5f / static_cast<float>(tex->height);
    for (const auto& rect : open) {
        draw->PushClipRect(ImVec2(rect[0], rect[1]), ImVec2(rect[2], rect[3]), true);
        draw->AddImage(id, ImVec2(cx0, cy0), ImVec2(cx1, cy1), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
        if (cx0 > 0.0f) {
            draw->AddImage(id, ImVec2(0.0f, cy0), ImVec2(cx0, cy1), ImVec2(0.0f, 0.0f), ImVec2(edgeU, 1.0f));
            draw->AddImage(id, ImVec2(cx1, cy0), ImVec2(windowWidth, cy1), ImVec2(1.0f - edgeU, 0.0f),
                           ImVec2(1.0f, 1.0f));
        }
        if (cy0 > 0.0f) {
            draw->AddImage(id, ImVec2(cx0, 0.0f), ImVec2(cx1, cy0), ImVec2(0.0f, 0.0f), ImVec2(1.0f, edgeV));
            draw->AddImage(id, ImVec2(cx0, cy1), ImVec2(cx1, windowHeight), ImVec2(0.0f, 1.0f - edgeV),
                           ImVec2(1.0f, 1.0f));
        }
        draw->PopClipRect();
    }
}

void GuiPainter::paint(const GuiSystem& gui, ImDrawList* draw, float windowWidth, float windowHeight,
                       const GuiPaintOptions& options)
{
    const kotor::GuiScreen* screen = gui.screen();
    if (screen == nullptr || draw == nullptr) {
        return;
    }
    view_ = options.view.has_value() ? *options.view : layout(*screen, windowWidth, windowHeight);
    stats_ = Stats{};
    // A whole-window panel (not a popup, not a caller-placed layout such as the HUD) hides the world:
    // retail draws its surround around it in game too (paired frame ui_inventory).
    const auto whole = [](const GuiView& v) {
        return v.panelLeft == 0.0f && v.panelTop == 0.0f && v.baseWidth >= 640.0f && v.baseHeight >= 480.0f;
    };
    bool wholePanel = !options.view.has_value() && whole(view_);
    // A popup over a whole panel (the inventory's tutorial box) keeps that panel's surround: the
    // panel is still the screen below it (walk 0140_inventory, route pair).
    for (std::size_t layer = 0; !options.view.has_value() && layer < gui.underlayCount(); ++layer) {
        if (const kotor::GuiScreen* below = gui.underlayScreen(layer)) {
            wholePanel = wholePanel || whole(layout(*below, windowWidth, windowHeight));
        }
    }
    if (options.opaqueBackdrop || (wholePanel && options.windowBackdrop)) {
        // Horizontal bands keep the holes (3D model rectangles) open.
        std::vector<float> ys = {0.0f, windowHeight};
        for (const auto& hole : options.holes) {
            ys.push_back(std::clamp(hole[1], 0.0f, windowHeight));
            ys.push_back(std::clamp(hole[3], 0.0f, windowHeight));
        }
        std::sort(ys.begin(), ys.end());
        ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
        std::vector<std::array<float, 4>> open; // the rectangles no model hole covers
        for (std::size_t band = 0; band + 1 < ys.size(); ++band) {
            std::vector<std::pair<float, float>> cuts;
            for (const auto& hole : options.holes) {
                if (hole[1] < ys[band + 1] && hole[3] > ys[band]) {
                    cuts.emplace_back(std::clamp(hole[0], 0.0f, windowWidth), std::clamp(hole[2], 0.0f, windowWidth));
                }
            }
            std::sort(cuts.begin(), cuts.end());
            float x = 0.0f;
            for (const auto& cut : cuts) {
                if (cut.first > x) {
                    open.push_back({x, ys[band], cut.first, ys[band + 1]});
                }
                x = std::max(x, cut.second);
            }
            if (x < windowWidth) {
                open.push_back({x, ys[band], windowWidth, ys[band + 1]});
            }
        }
        for (const auto& rect : open) {
            draw->AddRectFilled(ImVec2(rect[0], rect[1]), ImVec2(rect[2], rect[3]), IM_COL32(0, 0, 0, 255));
        }
        if (options.windowBackdrop) {
            drawWindowBackdrop(draw, windowWidth, windowHeight, open, options.backdropKind);
        }
    }
    Painting painting;
    painting.draw = draw;
    painting.gui = &gui;
    painting.options = &options;
    painting.screen = screen;
    painting.view = view_;
    // Screens under a popup, drawn inert from their saved states.
    for (std::size_t layer = 0; layer < gui.underlayCount(); ++layer) {
        const kotor::GuiScreen* below = gui.underlayScreen(layer);
        if (below == nullptr) {
            continue;
        }
        Painting under = painting;
        under.screen = below;
        under.layerStates = &gui.underlayStates(layer);
        under.view = layout(*below, windowWidth, windowHeight);
        if (below->border.present) {
            kotor::GuiControl panel;
            const auto fill = [&] {
                paintBorder(under, below->border, panel, under.view.pxX(0.0f), under.view.pxY(0.0f),
                            under.view.pxX(static_cast<float>(below->extent.width)),
                            under.view.pxY(static_cast<float>(below->extent.height)), 1.0f);
            };
            // The model rectangles of the screen below stay open too, so its 3D frame keeps showing under the popup
            // (the level-up's granted feats over the sheet, req_levelup_granted): the panel art goes in the bands around them.
            std::vector<float> ys = {0.0f, windowHeight};
            for (const auto& hole : options.holes) {
                ys.push_back(std::clamp(hole[1], 0.0f, windowHeight));
                ys.push_back(std::clamp(hole[3], 0.0f, windowHeight));
            }
            std::sort(ys.begin(), ys.end());
            ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
            for (std::size_t band = 0; band + 1 < ys.size(); ++band) {
                std::vector<std::pair<float, float>> cuts;
                for (const auto& hole : options.holes) {
                    if (hole[1] < ys[band + 1] && hole[3] > ys[band]) {
                        cuts.emplace_back(std::clamp(hole[0], 0.0f, windowWidth), std::clamp(hole[2], 0.0f, windowWidth));
                    }
                }
                std::sort(cuts.begin(), cuts.end());
                const auto fillRect = [&](float left, float right) {
                    if (right > left) {
                        draw->PushClipRect(ImVec2(left, ys[band]), ImVec2(right, ys[band + 1]), true);
                        fill();
                        draw->PopClipRect();
                    }
                };
                float x = 0.0f;
                for (const auto& cut : cuts) {
                    fillRect(x, cut.first);
                    x = std::max(x, cut.second);
                }
                fillRect(x, windowWidth);
            }
        }
        for (const kotor::GuiControl& control : below->controls) {
            paintControl(under, control);
        }
    }
    // The panel's own BORDER is the screen background.
    if (screen->border.present && !options.skipPanelFill) {
        const float x0 = view_.pxX(0.0f);
        const float y0 = view_.pxY(0.0f);
        const float x1 = view_.pxX(static_cast<float>(screen->extent.width));
        const float y1 = view_.pxY(static_cast<float>(screen->extent.height));
        kotor::GuiControl panel;
        kotor::GuiBorder panelBorder = screen->border;
        if (!options.panelFill.empty()) {
            panelBorder.fill = options.panelFill;
        }
        // A popup's own box is drawn whole over the model of the screen below it (req_levelup_granted: the
        // skillinfo fill darkens the sheet's model where they overlap).
        if (options.holes.empty() || gui.underlayCount() > 0) {
            paintBorder(painting, panelBorder, panel, x0, y0, x1, y1, 1.0f);
        } else {
            // The 3D model rectangles stay open: the panel art is clipped to the bands around
            // them so the backend's models underneath are not painted over.
            std::vector<float> ys = {0.0f, windowHeight};
            for (const auto& hole : options.holes) {
                ys.push_back(std::clamp(hole[1], 0.0f, windowHeight));
                ys.push_back(std::clamp(hole[3], 0.0f, windowHeight));
            }
            std::sort(ys.begin(), ys.end());
            ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
            for (std::size_t band = 0; band + 1 < ys.size(); ++band) {
                std::vector<std::pair<float, float>> cuts;
                for (const auto& hole : options.holes) {
                    if (hole[1] < ys[band + 1] && hole[3] > ys[band]) {
                        cuts.emplace_back(std::clamp(hole[0], 0.0f, windowWidth),
                                          std::clamp(hole[2], 0.0f, windowWidth));
                    }
                }
                std::sort(cuts.begin(), cuts.end());
                const auto fillRect = [&](float left, float right) {
                    if (right <= left) {
                        return;
                    }
                    draw->PushClipRect(ImVec2(left, ys[band]), ImVec2(right, ys[band + 1]), true);
                    paintBorder(painting, panelBorder, panel, x0, y0, x1, y1, 1.0f);
                    draw->PopClipRect();
                };
                float x = 0.0f;
                for (const auto& cut : cuts) {
                    fillRect(x, cut.first);
                    x = std::max(x, cut.second);
                }
                fillRect(x, windowWidth);
            }
        }
    }
    auditOn_ = true;
    for (const kotor::GuiControl& control : screen->controls) {
        paintControl(painting, control);
    }
    auditOn_ = false;
}

} // namespace ke
