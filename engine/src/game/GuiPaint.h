#pragma once

// Paints a shipped .gui screen from the GuiSystem state: panel and control fills
// (BORDER FILL), edges and corners, bitmap fonts with TLK text, hilight states, list
// boxes with their PROTOITEM, sliders, progress bars and scrollbars. Everything is
// drawn through ImGui draw lists with textures uploaded by the active backend
// (uploadUiImage), so GL and Vulkan produce the same screen.
//
// Layout: the screen's root extent (640x480, or 800x600 for the main menu) is the
// design space. It is scaled uniformly to fit the window and centred; the
// full-window background control (LBL_3DVIEW and similar) is stretched over the whole
// window. The scale rule is this engine's choice: retail's scaling was not recovered.

#include "game/GuiSystem.h"
#include "kotor/Formats.h"
#include "kotor/Gui.h"
#include "render/Backend.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ImDrawList;
struct ImVec2;

namespace ke {

struct GuiView {
    float scale = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    float baseWidth = 640.0f;
    float baseHeight = 480.0f;
    float windowWidth = 640.0f;
    float windowHeight = 480.0f;

    // A popup panel (confirm, tooltips) sits at its root extent's left/top inside the
    // 640x480 design space and its controls are relative to that corner.
    float panelLeft = 0.0f;
    float panelTop = 0.0f;

    // Edge-anchored layout (the HUD): the design space grows to the window's aspect at
    // `scale`, and each control keeps its distance from the nearest window edge (retail's
    // 800x600 HUD is the 640x480 one with the right and bottom controls moved to the new
    // edges). Controls in the middle third of an axis stay centred on it.
    bool anchored = false;

    float pxX(float x) const { return offsetX + (x + panelLeft) * scale; }
    float pxY(float y) const { return offsetY + (y + panelTop) * scale; }
    float guiX(float px) const { return (px - offsetX) / scale - panelLeft; }
    float guiY(float py) const { return (py - offsetY) / scale - panelTop; }
};

struct GuiPixelRect {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    bool contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

// How far an edge-anchored view moves a control, in design pixels (zero for a view that is
// not anchored): the control keeps its distance from the window edge it is nearest to.
void guiAnchorShift(const GuiView& view, const kotor::GuiControl& control, float& shiftX, float& shiftY);
// The window pixels a top-level control covers in `view`, anchoring and the screen's own
// offset (GuiControlState::offsetX/Y) included. What the painter draws and what a pointer hits.
GuiPixelRect guiControlPixels(const GuiView& view, const kotor::GuiControl& control, float offsetX = 0.0f,
                              float offsetY = 0.0f);

// A decoded TPC/TGA image uploaded as a backend UI image.
struct GuiTexture {
    int width = 0;
    int height = 0;
    std::uint64_t id = 0;
    bool flat = false; // every texel the same colour and alpha: a stretch cannot show
    // The art's TXI says `blending additive` (retail 0047abd0 / 0047af70: SRC_ALPHA, ONE). It is
    // drawn between GuiPainter::beginArt / endArt, which switch the backend to that blend.
    bool additive = false;
    // The backend has no additive blend, so the pixels were converted to the nearest straight-alpha
    // picture instead (see GuiPainter::texture) and draw normally.
    bool approximated = false;
};

// TXI glyph table of a shipped bitmap font (dialogfont16x16, fnt_d16x16, fnt_console).
struct GuiFont {
    GuiTexture texture;
    int numChars = 0;
    float height = 16.0f;     // pixels at design scale
    float spacingR = 0.0f;
    float spacingB = 0.0f;
    // Retail measures in TXI units times 100 (texturewidth * 100 per image width): image pixels for
    // the stock fonts, where the factor is 1.
    float measureScale = 1.0f;
    struct Glyph {
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0; // image space, v down
        float width = 0, height = 0;           // pixels
        // Rows of the glyph cell that carry visible pixels (alpha above a quarter, from the decoded
        // font image; the whole cell when the image has no alpha). The audit measures the ink, not
        // the cell: dialogfont16x16 cells are 16 px high and its capitals are shorter.
        float inkTop = 0, inkBottom = 0;
    };
    std::vector<Glyph> glyphs;

    bool ready() const { return texture.id != 0 && !glyphs.empty(); }
    float advance(unsigned char c) const;
    float textWidth(const std::string& text) const;
    // RETAIL-PROVEN (CAurGUIStringInternal::WrapStrings 0045a2f0): the wrap measure, in whole pixels.
    // A line's first character counts its glyph only, truncated; every later one adds glyph plus
    // spacingR, plus 0.25 (0073edf0), truncated (_ftol 006fae8c).
    int wrapAdvance(unsigned char c, bool lineStart) const;
};

struct GuiPaintOptions {
    // Fill the window with black first (front-end screens with no world behind).
    bool opaqueBackdrop = true;
    // A layout the caller computed (a panel that is not scaled to fit: the HUD bars) instead
    // of the uniform scale-to-fit one.
    std::optional<GuiView> view;
    // ENGINE CHOICE (modern resolutions): retail draws a window-sized backdrop picked by its
    // five 4:3 modes (800x600back ... 1600x1200back). Here the art keeps its aspect at the
    // full window height (or width) and the margins repeat its outermost pixels, so a 16:9,
    // 21:9 or 16:10 window is covered without stretching the art. Off: plain black.
    bool windowBackdrop = true;
    // Which surround art: retail names it "<W>x<H>" plus a per-panel kind (0040a900 with the table built
    // at 0073ad80: back, store, pazaak, map; 006a6e20: comp0 / comp1). "back" for most panels.
    std::string backdropKind = "back";
    // Called for every control before it is drawn; returning true means the caller
    // drew (or hides) it. Rectangles are window pixels.
    std::function<bool(const kotor::GuiControl&, float x0, float y0, float x1, float y1)> controlHook;
    // Window-pixel rectangles (x0, y0, x1, y1) left unfilled by the backdrop: 3D model
    // controls the backend draws underneath.
    std::vector<std::array<float, 4>> holes;
    // The panel's own BORDER fill is the screen background; a screen whose 3D view the
    // backend draws underneath must not cover it.
    bool skipPanelFill = false;
    // Replaces the panel's BORDER fill image (the loading screen's per-area art).
    std::string panelFill;
    double timeSeconds = 0.0;
};

// The checkbox ring's edge in design pixels (paired frames opt_gameplay, opt_feedback).
inline constexpr float kCheckboxEdge = 25.0f;

class GuiPainter {
public:
    using TlkLookup = std::function<std::string(std::uint32_t)>;

    GuiPainter(kotor::Install& install, IRenderBackend* backend, TlkLookup tlk);
    ~GuiPainter();
    GuiPainter(const GuiPainter&) = delete;
    GuiPainter& operator=(const GuiPainter&) = delete;

    // Releases every uploaded image (before the backend shuts down).
    void release();

    GuiView layout(const kotor::GuiScreen& screen, float windowWidth, float windowHeight) const;

    // Paints `gui`'s current screen into `draw` for a window of this size.
    void paint(const GuiSystem& gui, ImDrawList* draw, float windowWidth, float windowHeight,
               const GuiPaintOptions& options);
    const GuiView& view() const { return view_; }

    // Window pixel to design space of the last painted screen.
    void toGui(float px, float py, float& x, float& y) const;

    // Image and font access (screens that draw art themselves: save thumbnails, maps).
    const GuiTexture* texture(const std::string& resref);
    // CSWGuiBorder::GetInnerExtent 00415240: DIMENSION, else the edge art's thickness, in design pixels.
    float borderInnerInset(const kotor::GuiBorder& border);
    // Brackets draws of `tex` so additive art (TXI `blending additive`) is blended SRC_ALPHA / ONE
    // as retail does. Both are no-ops for every other texture.
    void beginArt(ImDrawList* draw, const GuiTexture& tex) const;
    void endArt(ImDrawList* draw, const GuiTexture& tex) const;
    // Draws one texture, as the painter draws it, into a rectangle (the gate's art probe).
    bool drawProbe(ImDrawList* draw, const std::string& resref, float x0, float y0, float x1, float y1);
    // Uploads raw RGBA pixels under `key` (replacing any previous upload).
    const GuiTexture* uploadImage(const std::string& key, const kotor::Image& image);
    void dropTexture(const std::string& key);
    const GuiFont* font(const std::string& resref);

    // Text with the control's font, alignment flags and wrapping; used by the screens
    // that draw text outside a control. Rectangle in window pixels.
    void drawText(ImDrawList* draw, const GuiFont& font, const std::string& text, float x0, float y0,
                  float x1, float y1, int align, unsigned color, float scale, bool wrap) const;

    // Height in design pixels of `text` wrapped to `width` in a font.
    float measureText(const std::string& fontResref, const std::string& text, float width);

    std::string controlText(const kotor::GuiControl& control, const GuiControlState& state) const;

    std::size_t texturesLoaded() const { return textures_.size(); }

    // What the last paint() drew: a screen that paints nothing is a failure the tests catch.
    struct Stats {
        int controls = 0;
        int glyphs = 0;
        int images = 0;
        int missingImages = 0;
        std::vector<std::string> missingNames; // which images were asked for and not found (first 16)
        // The smallest font cell drawn, in window pixels (legibility at high resolutions).
        float minTextPixels = 1.0e9f;
        // The far right and bottom edge of any control image drawn, in window pixels.
        float maxRight = 0.0f;
        float maxBottom = 0.0f;
        // Where art and text went, in window pixels (the resolution gates check these).
        struct Placed {
            std::string tag;
            float x0 = 0.0f;
            float y0 = 0.0f;
            float x1 = 0.0f;
            float y1 = 0.0f;
            int nativeWidth = 0;   // the art's own size
            int nativeHeight = 0;
            bool flat = false;
            unsigned tint = 0;     // ABGR as drawn
        };
        std::vector<Placed> fills;      // every control fill image
        std::vector<Placed> indicators; // checkbox indicator art
        std::vector<Placed> labels;     // glyph bounds of a checkbox label
        // The window backdrop that filled what the layout does not cover ("" when none).
        std::string backdrop;
        // Audits (see FrontEndGate): what was drawn wrong or not at all, per control tag.
        // kind: text_overflow (wrapped block or word wider/taller than its control rect),
        // text_clip (glyphs outside the clip rect they were drawn under), image_clip (art beyond
        // the clip rect or window), empty (a visible control that drew nothing), empty_list.
        struct Issue {
            std::string kind;
            std::string tag;
            std::string detail;
        };
        std::vector<Issue> issues;
        int textOverflow = 0;
        int borderArt = 0; // edge and corner pieces drawn (they are not counted in images)
        int textClipped = 0;
        int imageClipped = 0;
        int emptyControls = 0;
    };
    const Stats& stats() const { return stats_; }
    // x0, y0, x1, y1 of the glyphs the last drawText put down (inverted when none).
    const float* lastTextBounds() const { return textBounds_; }

private:
    struct Painting;
    void drawWindowBackdrop(ImDrawList* draw, float windowWidth, float windowHeight,
                            const std::vector<std::array<float, 4>>& open, const std::string& kind);
    void paintControl(Painting& p, const kotor::GuiControl& control);
    void paintBorder(Painting& p, const kotor::GuiBorder& border, const kotor::GuiControl& control,
                     float x0, float y0, float x1, float y1, float alpha, bool fillOnly = false);
    void paintImage(Painting& p, const std::string& resref, float x0, float y0, float x1, float y1,
                    unsigned color, bool flipV = false, bool flipU = false);
    void paintListBox(Painting& p, const kotor::GuiControl& control, const GuiControlState& state);
    void paintFeatRow(Painting& p, const GuiListItem& item, int picked, float x0, float y0);
    void paintItemEntry(Painting& p, const kotor::GuiControl& proto, const GuiListItem& item, bool lit, bool boxed,
                        const GuiFont* font, float x0, float y0, float x1, float rowHeight = 0.0f);
    void paintScrollbar(Painting& p, const kotor::GuiControl& control, const GuiControlState& state,
                        const GuiControlState* listState);
    std::vector<std::string> wrapLines(const GuiFont& font, const std::string& text, float width) const;
    void note(const char* kind, const std::string& detail) const;
    void noteMissingBorderArt(const std::string& resref);
    void auditEmpty(const kotor::GuiControl& control, const GuiControlState& state, int glyphs0, int images0,
                    int missing0);

    kotor::Install& install_;
    IRenderBackend* backend_;
    TlkLookup tlk_;
    GuiView view_;
    std::map<std::string, std::optional<GuiTexture>> textures_;
    std::map<std::string, std::unique_ptr<GuiFont>> fonts_;
    mutable Stats stats_;
    mutable std::string auditTag_; // the control being painted (audit notes)
    bool auditOn_ = false;         // true while controls paint (not the panel's hole bands)
    mutable float textBounds_[4] = {1.0e9f, 1.0e9f, -1.0e9f, -1.0e9f}; // glyphs of the last drawText
    // Set while a text view draws: its line under the last whole one is clipped on purpose (retail does).
    mutable bool clipExpected_ = false;
};

} // namespace ke
