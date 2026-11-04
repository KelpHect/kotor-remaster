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
};

struct GuiScreen {
    std::string name;
    int type = 0;
    std::string tag;
    GuiExtent extent;
    std::vector<GuiControl> controls;
    std::size_t controlCount = 0;

    bool hasTag(const std::string& wanted) const;
};

GuiScreen parseGui(const std::vector<std::uint8_t>& bytes, const std::string& name);

const char* guiControlTypeName(int type);

} // namespace ke::kotor
