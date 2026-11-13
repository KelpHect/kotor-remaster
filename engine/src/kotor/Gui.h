#pragma once

#include "kotor/Formats.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ke::kotor {

struct GuiExtent {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

// One BORDER / HILIGHT / SELECTED / PROGRESS struct of a control. Retail draws `fill`
// inside the border, `edge` on the four sides and `corner` at the four corners, each
// `dimension` pixels thick, tinted with `color`.
struct GuiBorder {
    bool present = false;
    std::string corner;
    std::string edge;
    std::string fill;
    int fillStyle = 0;
    int dimension = 0;
    int innerOffset = 0;
    Vec3 color{1.0f, 1.0f, 1.0f};
    bool pulsing = false;
};

// Scrollbar DIR arrow and THUMB image structs, slider THUMB.
struct GuiImage {
    std::string image;
    int drawStyle = 0;
    int flipStyle = 0;
    float rotate = 0.0f;
    int alignment = 0;
};

struct GuiControl {
    std::string key; // stable hierarchy path, e.g. "LB_DESC/SCROLLBAR"
    int type = 0;
    int id = -1;
    std::string tag;
    GuiExtent extent;
    Vec3 color{1.0f, 1.0f, 1.0f};
    float alpha = 1.0f;
    std::string text;
    std::string font;
    std::string fill;
    std::string edge;
    std::string corner;
    float maxValue = 1.0f;
    float curValue = 0.0f;
    bool startFromLeft = true;
    bool selected = false;
    bool isSelected = false;
    std::vector<GuiControl> children;

    // ---- full control data (everything the shipped GUI files carry) ----
    GuiBorder border;
    GuiBorder hilight;
    GuiBorder selectedBorder;       // SELECTED: toggle buttons
    GuiBorder hilightSelected;      // HILIGHTSELECTED
    GuiBorder progress;             // PROGRESS: progress bar fill
    std::uint32_t strref = 0xFFFFFFFFu;
    int textAlign = 0;              // bit flags: 1 left 2 hcenter 4 right 8 top 16 vcenter 32 bottom
    bool hasTextColor = false;
    Vec3 textColor{1.0f, 1.0f, 1.0f};
    bool pulsingText = false;
    int moveUp = -1;                // MOVETO: control ids reached by the arrow keys
    int moveDown = -1;
    int moveLeft = -1;
    int moveRight = -1;
    int padding = 0;                // listbox
    bool looping = false;
    bool leftScrollbar = false;
    float visibleValue = 1.0f;      // scrollbar
    int drawMode = 0;
    GuiImage dir;                   // scrollbar arrows
    GuiImage thumb;                 // scrollbar and slider thumb
    std::vector<GuiControl> proto;  // listbox PROTOITEM (zero or one entry)
    // A sub-panel's control laid into another screen (MAINCG's QUICKPNL, CUSTPNL, LEVELUPPNL): retail draws a
    // child panel's controls clipped to the child panel's extent (LBL_BG reaches past it). Design coordinates.
    bool clipped = false;
    GuiExtent clip;
};

struct GuiScreen {
    std::string name;
    int type = 0;
    std::string tag;
    GuiExtent extent;
    GuiBorder border;               // the panel's own background
    Vec3 color{0.0f, 0.0f, 0.0f};
    float alpha = 1.0f;
    std::vector<GuiControl> controls;
    std::size_t controlCount = 0;
    // A panel fitted by its own OnPanelAdded: the extent AddPanel logs before the fit (the state trace's open row).
    GuiExtent openExtent;
    bool hasOpenExtent = false;

    bool hasTag(const std::string& wanted) const;
};

GuiScreen parseGui(const std::vector<std::uint8_t>& bytes, const std::string& name);

const char* guiControlTypeName(int type);

} // namespace ke::kotor
