#include "kotor/Gui.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace ke::kotor {

namespace {

const GffField* field(const Gff& gff, std::uint32_t structure, const char* label)
{
    return gff.field(structure, label);
}

int number(const Gff& gff, std::uint32_t structure, const char* label, int fallback = 0)
{
    const GffField* f = field(gff, structure, label);
    return f != nullptr ? gff.integer(*f) : fallback;
}

std::string text(const Gff& gff, std::uint32_t structure, const char* label)
{
    const GffField* f = field(gff, structure, label);
    return f != nullptr ? gff.text(*f) : std::string{};
}

GuiExtent extent(const Gff& gff, std::uint32_t structure)
{
    GuiExtent out;
    const GffField* f = field(gff, structure, "EXTENT");
    if (f == nullptr || f->type != GffType::Struct) {
        return out;
    }
    const std::uint32_t e = gff.structIndex(*f);
    out.left = number(gff, e, "LEFT", number(gff, e, "left", 0));
    out.top = number(gff, e, "TOP", number(gff, e, "top", 0));
    out.width = number(gff, e, "WIDTH", number(gff, e, "width", 0));
    out.height = number(gff, e, "HEIGHT", number(gff, e, "height", 0));
    return out;
}

std::string resrefField(const Gff& gff, std::uint32_t structure, const char* sub,
                        const char* label)
{
    const GffField* f = field(gff, structure, sub);
    if (f == nullptr || f->type != GffType::Struct) {
        return {};
    }
    return text(gff, gff.structIndex(*f), label);
}

GuiBorder parseBorder(const Gff& gff, std::uint32_t structure, const char* sub)
{
    GuiBorder out;
    const GffField* f = field(gff, structure, sub);
    if (f == nullptr || f->type != GffType::Struct) {
        return out;
    }
    const std::uint32_t b = gff.structIndex(*f);
    out.present = true;
    out.corner = text(gff, b, "CORNER");
    out.edge = text(gff, b, "EDGE");
    out.fill = text(gff, b, "FILL");
    out.fillStyle = number(gff, b, "FILLSTYLE", 0);
    out.dimension = number(gff, b, "DIMENSION", 0);
    out.innerOffset = number(gff, b, "INNEROFFSET", 0);
    out.pulsing = number(gff, b, "PULSING", 0) != 0;
    if (const GffField* color = field(gff, b, "COLOR");
        color != nullptr && color->type == GffType::Vector3) {
        out.color = gff.vector3(*color);
    }
    return out;
}

GuiImage parseImage(const Gff& gff, std::uint32_t structure, const char* sub)
{
    GuiImage out;
    const GffField* f = field(gff, structure, sub);
    if (f == nullptr || f->type != GffType::Struct) {
        return out;
    }
    const std::uint32_t b = gff.structIndex(*f);
    out.image = text(gff, b, "IMAGE");
    out.drawStyle = number(gff, b, "DRAWSTYLE", 0);
    out.flipStyle = number(gff, b, "FLIPSTYLE", 0);
    out.alignment = number(gff, b, "ALIGNMENT", 0);
    if (const GffField* rotate = field(gff, b, "ROTATE")) {
        out.rotate = gff.number(*rotate);
    }
    return out;
}

GuiControl parseControl(const Gff& gff, std::uint32_t structure, int depth,
                        const std::string& parentKey)
{
    GuiControl control;
    control.type = number(gff, structure, "CONTROLTYPE", 0);
    control.id = number(gff, structure, "ID", -1);
    control.tag = text(gff, structure, "TAG");
    control.key = parentKey + "/"
        + (control.tag.empty()
               ? "#" + std::to_string(control.id) + ":" + std::to_string(control.type)
               : control.tag);
    control.extent = extent(gff, structure);
    if (const GffField* color = field(gff, structure, "COLOR");
        color != nullptr && color->type == GffType::Vector3) {
        control.color = gff.vector3(*color);
    }
    if (const GffField* alpha = field(gff, structure, "ALPHA")) {
        control.alpha = gff.number(*alpha);
    }
    control.text = text(gff, structure, "TEXT");
    if (control.text.empty()) {
        // TEXT is a struct holding TEXT, FONT, STRREF, ALIGNMENT and COLOR.
        control.text = resrefField(gff, structure, "TEXT", "TEXT");
    }
    control.font = resrefField(gff, structure, "TEXT", "FONT");
    control.fill = resrefField(gff, structure, "BORDER", "FILL");
    control.edge = resrefField(gff, structure, "BORDER", "EDGE");
    control.corner = resrefField(gff, structure, "BORDER", "CORNER");
    if (const GffField* maxValue = field(gff, structure, "MAXVALUE")) {
        control.maxValue = gff.number(*maxValue);
    }
    if (const GffField* curValue = field(gff, structure, "CURVALUE")) {
        control.curValue = gff.number(*curValue);
    }
    if (const GffField* start = field(gff, structure, "STARTFROMLEFT")) {
        control.startFromLeft = gff.integer(*start) != 0;
    }
    if (const GffField* selected = field(gff, structure, "ISSELECTED")) {
        control.isSelected = gff.integer(*selected) != 0;
    }
    if (const GffField* selected = field(gff, structure, "SELECTED")) {
        control.selected = gff.integer(*selected) != 0;
    }
    if (control.maxValue <= 0.0f) {
        control.maxValue = 1.0f;
    }
    control.border = parseBorder(gff, structure, "BORDER");
    control.hilight = parseBorder(gff, structure, "HILIGHT");
    control.selectedBorder = parseBorder(gff, structure, "SELECTED");
    control.hilightSelected = parseBorder(gff, structure, "HILIGHTSELECTED");
    control.progress = parseBorder(gff, structure, "PROGRESS");
    if (const GffField* t = field(gff, structure, "TEXT"); t != nullptr && t->type == GffType::Struct) {
        const std::uint32_t ts = gff.structIndex(*t);
        if (const GffField* ref = field(gff, ts, "STRREF")) {
            // A DWORD held in the field: Gff::integer goes through a float and turns 0xFFFFFFFF (no strref)
            // into 0x80000000, which then hid the file's own TEXT (CUSTPNL's LBL_NUMn "1".."6").
            control.strref = ref->type == GffType::UInt32 ? ref->data : static_cast<std::uint32_t>(gff.integer(*ref));
        }
        control.textAlign = number(gff, ts, "ALIGNMENT", 0);
        control.pulsingText = number(gff, ts, "PULSING", 0) != 0;
        if (const GffField* color = field(gff, ts, "COLOR");
            color != nullptr && color->type == GffType::Vector3) {
            control.textColor = gff.vector3(*color);
            control.hasTextColor = true;
        }
    }
    if (const GffField* m = field(gff, structure, "MOVETO"); m != nullptr && m->type == GffType::Struct) {
        const std::uint32_t ms = gff.structIndex(*m);
        control.moveUp = number(gff, ms, "UP", -1);
        control.moveDown = number(gff, ms, "DOWN", -1);
        control.moveLeft = number(gff, ms, "LEFT", -1);
        control.moveRight = number(gff, ms, "RIGHT", -1);
    }
    control.padding = number(gff, structure, "PADDING", 0);
    control.looping = number(gff, structure, "LOOPING", 0) != 0;
    // RETAIL-PROVEN (0041d5b0, CSWGuiListBox loader): LEFTSCROLLBAR defaults to 1 when absent. Every
    // shipped listbox sets it, and every shipped SCROLLBAR extent already sits where 004181f0 would put it.
    control.leftScrollbar = number(gff, structure, "LEFTSCROLLBAR", 1) != 0;
    control.drawMode = number(gff, structure, "DRAWMODE", 0);
    if (const GffField* visible = field(gff, structure, "VISIBLEVALUE")) {
        control.visibleValue = gff.number(*visible);
    }
    control.dir = parseImage(gff, structure, "DIR");
    control.thumb = parseImage(gff, structure, "THUMB");
    if (depth < 64) {
        if (const GffField* children = field(gff, structure, "CONTROLS");
            children != nullptr && children->type == GffType::List) {
            for (const std::uint32_t child : gff.list(*children)) {
                control.children.push_back(parseControl(gff, child, depth + 1,
                                                        control.key));
            }
        }
        if (control.type == 11) {
            if (const GffField* proto = field(gff, structure, "PROTOITEM");
                proto != nullptr && proto->type == GffType::Struct) {
                control.proto.push_back(parseControl(gff, gff.structIndex(*proto), depth + 1,
                                                     control.key + "/PROTOITEM"));
            }
            if (const GffField* scroll = field(gff, structure, "SCROLLBAR");
                scroll != nullptr && scroll->type == GffType::Struct) {
                control.children.push_back(parseControl(
                    gff, gff.structIndex(*scroll), depth + 1, control.key));
            }
        }
    }
    return control;
}

std::size_t countControls(const GuiControl& control)
{
    std::size_t count = 1;
    for (const GuiControl& child : control.children) {
        count += countControls(child);
    }
    return count;
}

bool hasTag(const GuiControl& control, const std::string& wanted)
{
    if (control.tag == wanted) {
        return true;
    }
    for (const GuiControl& child : control.children) {
        if (hasTag(child, wanted)) {
            return true;
        }
    }
    return false;
}

} // namespace

GuiScreen parseGui(const std::vector<std::uint8_t>& bytes, const std::string& name)
{
    if (bytes.size() < 8 || std::memcmp(bytes.data(), "GUI ", 4) != 0
        || std::memcmp(bytes.data() + 4, "V3.2", 4) != 0) {
        throw FormatError(name + ": expected GUI V3.2");
    }
    Gff gff = Gff::parse(bytes, name);
    GuiScreen screen;
    screen.name = name;
    screen.type = number(gff, gff.rootIndex(), "CONTROLTYPE", 0);
    screen.tag = text(gff, gff.rootIndex(), "TAG");
    screen.extent = extent(gff, gff.rootIndex());
    screen.border = parseBorder(gff, gff.rootIndex(), "BORDER");
    if (const GffField* color = field(gff, gff.rootIndex(), "COLOR");
        color != nullptr && color->type == GffType::Vector3) {
        screen.color = gff.vector3(*color);
    }
    if (const GffField* alpha = field(gff, gff.rootIndex(), "ALPHA")) {
        screen.alpha = gff.number(*alpha);
    }
    const GffField* controls = field(gff, gff.rootIndex(), "CONTROLS");
    if (controls == nullptr || controls->type != GffType::List) {
        throw FormatError(name + ": GUI root has no CONTROLS list");
    }
    for (const std::uint32_t structure : gff.list(*controls)) {
        screen.controls.push_back(parseControl(gff, structure, 1,
                                               screen.tag.empty() ? name : screen.tag));
    }
    screen.controlCount = 1;
    for (const GuiControl& control : screen.controls) {
        screen.controlCount += countControls(control);
    }
    return screen;
}

bool GuiScreen::hasTag(const std::string& wanted) const
{
    if (tag == wanted) {
        return true;
    }
    for (const GuiControl& control : controls) {
        if (ke::kotor::hasTag(control, wanted)) {
            return true;
        }
    }
    return false;
}

const char* guiControlTypeName(int type)
{
    switch (type) {
    case 0: return "control";
    case 2: return "panel";
    // RETAIL-PROVEN (0041d3e0, PROTOITEM factory): 4 builds a label (ctor 0041acd0) and 5 a
    // highlighting label (ctor 0041b5c0).
    case 4: return "label";
    case 5: return "label_hilight";
    case 6: return "button";
    case 7: return "checkbox";
    case 8: return "slider";
    case 9: return "scrollbar";
    case 10: return "progress";
    case 11: return "listbox";
    default: return "unknown";
    }
}

} // namespace ke::kotor
