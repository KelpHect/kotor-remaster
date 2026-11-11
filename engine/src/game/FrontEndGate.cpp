#include "game/FrontEndGate.h"

#include "core/Log.h"
#include "kotor/Formats.h"
#include "kotor/SaveGame.h"
#include "render/Bmp.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>

namespace ke {

namespace {

constexpr int kScreenTimeout = 90; // frames to wait for a screen to open

// ---- resolution checks (pure, so each can be proven against the behaviour it rejects) ----

// The layout must scale both axes alike: art keeps its aspect.
bool scalesMatch(float sx, float sy)
{
    return sx > 0.0f && std::fabs(sx - sy) / sx < 0.005f;
}

bool rectInside(float x0, float y0, float x1, float y1, float width, float height)
{
    return x0 >= -1.0f && y0 >= -1.0f && x1 <= width + 1.0f && y1 <= height + 1.0f;
}

float lumaAt(const CapturedFrame& frame, int x, int y)
{
    x = std::clamp(x, 0, frame.width - 1);
    y = std::clamp(y, 0, frame.height - 1);
    const std::uint8_t* p = &frame.rgba[(static_cast<std::size_t>(y) * frame.width + x) * 4];
    return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
}

// A full-screen menu leaves no window margin empty. The backdrop art is 4:3, so a margin
// exists only where the window is wider (left and right) or taller (top and bottom) than 4:3.
// Probes sit 4 px in from the window edge, inside each margin, at three positions along it;
// none may be black. A window that is exactly 4:3 has no margin: nothing to probe (the art's
// own corners are dark, which is not a gap).
bool frameCovered(const CapturedFrame& frame, float* worst = nullptr, bool* hasMargin = nullptr)
{
    if (frame.width < 32 || frame.height < 32 || frame.rgba.empty()) {
        return false;
    }
    const int w = frame.width;
    const int h = frame.height;
    const float marginX = 0.5f * (static_cast<float>(w) - static_cast<float>(h) * 4.0f / 3.0f);
    const float marginY = 0.5f * (static_cast<float>(h) - static_cast<float>(w) * 3.0f / 4.0f);
    float lowest = 255.0f;
    bool probed = false;
    if (marginX >= 8.0f) {
        probed = true;
        for (int i = 1; i <= 3; ++i) {
            lowest = std::min(lowest, lumaAt(frame, 4, h * i / 4));
            lowest = std::min(lowest, lumaAt(frame, w - 5, h * i / 4));
        }
    }
    if (marginY >= 8.0f) {
        probed = true;
        for (int i = 1; i <= 3; ++i) {
            lowest = std::min(lowest, lumaAt(frame, w * i / 4, 4));
            lowest = std::min(lowest, lumaAt(frame, w * i / 4, h - 5));
        }
    }
    if (worst != nullptr) {
        *worst = lowest;
    }
    if (hasMargin != nullptr) {
        *hasMargin = probed;
    }
    return lowest >= 5.0f;
}

// ---- widget art checks (pure, each proven against the behaviour it rejects) ----

using Placed = GuiPainter::Stats::Placed;

bool closeTo(float a, float b, float tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

bool within(const Placed& inner, float x0, float y0, float x1, float y1, float tolerance)
{
    return inner.x0 >= x0 - tolerance && inner.y0 >= y0 - tolerance && inner.x1 <= x1 + tolerance
        && inner.y1 <= y1 + tolerance;
}

// The indicator is the art at its own size (scaled by the layout), inside its control.
bool indicatorOk(const Placed& indicator, float expectW, float expectH, float cx0, float cy0, float cx1, float cy1)
{
    return closeTo(indicator.x1 - indicator.x0, expectW, 1.5f) && closeTo(indicator.y1 - indicator.y0, expectH, 1.5f)
        && within(indicator, cx0, cy0, cx1, cy1, 1.5f);
}

// The label starts to the right of the indicator and stays in its control.
bool labelClear(const Placed& label, const Placed& indicator, float cx0, float cx1)
{
    return label.x0 >= indicator.x1 - 0.5f && label.x0 >= cx0 - 1.5f && label.x1 <= cx1 + 1.5f;
}

// Art that is not flat and is drawn at 3x or more of its own size is a stretch.
bool stretchedArt(const Placed& fill, float scale)
{
    if (fill.flat || fill.nativeWidth <= 0 || fill.nativeHeight <= 0 || scale <= 0.0f) {
        return false;
    }
    const float w = fill.x1 - fill.x0;
    const float h = fill.y1 - fill.y0;
    const float sx = w / (static_cast<float>(fill.nativeWidth) * scale);
    const float sy = h / (static_cast<float>(fill.nativeHeight) * scale);
    return std::max(sx, sy) >= 3.0f && std::max(w, h) > 60.0f * scale;
}

// Share of pixels in a window rectangle that are near black / clearly bright. A drawn area map is
// mostly black with bright rooms; the world seen through an empty minimap box is neither.
void mapShares(const CapturedFrame& frame, int x0, int y0, int x1, int y1, float& dark, float& bright)
{
    int total = 0;
    int darkCount = 0;
    int brightCount = 0;
    for (int y = std::max(0, y0); y < std::min(frame.height, y1); ++y) {
        for (int x = std::max(0, x0); x < std::min(frame.width, x1); ++x) {
            const float luma = lumaAt(frame, x, y);
            ++total;
            darkCount += luma < 25.0f ? 1 : 0;
            brightCount += luma > 70.0f ? 1 : 0;
        }
    }
    dark = total > 0 ? static_cast<float>(darkCount) / static_cast<float>(total) : 0.0f;
    bright = total > 0 ? static_cast<float>(brightCount) / static_cast<float>(total) : 0.0f;
}

float meanLumaAround(const CapturedFrame& frame, float cx, float cy, int radius)
{
    float sum = 0.0f;
    int count = 0;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            sum += lumaAt(frame, static_cast<int>(cx) + dx, static_cast<int>(cy) + dy);
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<float>(count) : 0.0f;
}

// Mean colour of the bright pixels (the art, not the panel behind it) inside a rectangle.
bool brightMean(const CapturedFrame& frame, const Placed& rect, float& r, float& g, float& b)
{
    double sr = 0.0;
    double sg = 0.0;
    double sb = 0.0;
    int count = 0;
    for (int y = static_cast<int>(rect.y0); y < static_cast<int>(rect.y1); ++y) {
        for (int x = static_cast<int>(rect.x0); x < static_cast<int>(rect.x1); ++x) {
            if (x < 0 || y < 0 || x >= frame.width || y >= frame.height) {
                continue;
            }
            const std::uint8_t* p = &frame.rgba[(static_cast<std::size_t>(y) * frame.width + x) * 4];
            if (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2] > 80.0f) {
                sr += p[0];
                sg += p[1];
                sb += p[2];
                ++count;
            }
        }
    }
    if (count < 8) {
        return false;
    }
    r = static_cast<float>(sr / count);
    g = static_cast<float>(sg / count);
    b = static_cast<float>(sb / count);
    return true;
}

// The shipped +/- arrows are tinted (0, 0.64, 0.95): blue, not white.
bool blueTinted(float r, float g, float b)
{
    return r < 70.0f && b > 140.0f && b > 2.0f * r + 40.0f && g < b;
}

// A selected checkbox shows its dot (bright centre), an unselected one an empty ring.
bool stateDiffers(float onLuma, float offLuma)
{
    return onLuma - offLuma > 60.0f;
}

} // namespace

FrontEndGate::FrontEndGate(std::string outDir, std::filesystem::path savesDir, SDL_Window* window,
                           std::string scenario)
    : outDir_(std::move(outDir)), scenario_(std::move(scenario)), savesDir_(std::move(savesDir)), window_(window)
{
    if (scenario_ == "store") {
        buildStore();
    } else if (scenario_.rfind("maps", 0) == 0) {
        buildMaps();
    } else {
        build();
    }
}

// The HUD minimap in the running engine for modules of every planet (audit 4). Each module is
// entered through the session's own module change, then the frame is read back: the map window
// must hold drawn map art around the leader and the leader's arrow must sit at its centre. The
// data-side sweep over all 117 modules is engine/scripts/audit_map_modules.py.
void FrontEndGate::buildMaps()
{
    ops_.push_back(screenIs("mainmenu"));
    ops_.push_back(wait(8));
    ops_.push_back(expect("leave_menu", [](Context& context) {
        context.gui->close();
        context.game->setTutorialWindowsEnabledForTest(false); // a tutorial popup would hold the module change
        return true;
    }));
    ops_.push_back(wait(4));
    // First, last and (where there are many) middle modules of each world, plus one area whose Map
    // struct is empty (MapResX 0, the Ebon Hawk's own cockpit module).
    static const char* const kSamples[] = {
        "end_m01aa", "end_m01ab", "tar_m02aa", "tar_m03ab", "tar_m05aa", "tar_m09ab", "tar_m11ab",
        "danm13", "danm14ac", "danm16", "kas_m22aa", "kas_m23ac", "kas_m25aa", "korr_m33aa", "korr_m35aa",
        "korr_m39aa", "manm26aa", "manm27aa", "manm28ad", "tat_m17aa", "tat_m18ab", "tat_m20aa", "unk_m41aa",
        "unk_m42aa", "unk_m44ac", "sta_m45aa", "sta_m45ad", "lev_m40aa", "lev_m40ad", "ebo_m12aa", "ebo_m41aa",
        "liv_m99aa", "ebo_m40aa"};
    // "maps:a,b" runs just those modules (a debugging aid; the plain scenario runs the sample list).
    std::vector<std::string> modules(std::begin(kSamples), std::end(kSamples));
    if (scenario_.size() > 5 && scenario_[4] == ':') {
        modules.clear();
        std::string rest = scenario_.substr(5);
        for (std::size_t at = 0; at <= rest.size();) {
            const std::size_t comma = std::min(rest.find(',', at), rest.size());
            modules.push_back(rest.substr(at, comma - at));
            at = comma + 1;
        }
    }
    for (const std::string& module : modules) {
        ops_.push_back(expect("maps_request_" + module, [module](Context& context) {
            context.game->requestModule(module);
            return true;
        }));
        ops_.push_back(waitUntil("maps_entered_" + module,
                                 [module](Context& context) { return context.game->module() == module; }, 1800));
        ops_.push_back(wait(30));
        ops_.push_back(expect("maps_settled_" + module, [](Context& context) {
            if (context.game->dialogue().active) {
                context.game->endDialogue();
            }
            context.game->releaseControl(); // an entry script's fade or restrict mode would hide the HUD
            context.game->markMapAt(context.game->position(), -1); // this checks where the art sits, so none of it is fogged
            return true;
        }));
        ops_.push_back(wait(20));
        // A conversation an entry script starts after the settle step (end_m01ab's Carth call, tar_m11ab) covers the
        // HUD and hides the minimap this gate measures, so it is ended again just before the capture.
        ops_.push_back(expect("maps_settled_again_" + module, [](Context& context) {
            if (context.game->dialogue().active) {
                context.game->endDialogue();
            }
            return true;
        }));
        ops_.push_back(wait(4));
        ops_.push_back(capture("maps_" + module, 40));
        ops_.push_back(expect("maps_minimap_" + module, [this, module](Context& context) {
            if (context.hudView == nullptr || context.hudGui == nullptr || lastFrame_.rgba.empty()) {
                return false;
            }
            const auto& map = context.game->scene().minimap;
            const kotor::GuiControl* view = context.hudGui->control("LBL_MAPVIEW");
            if (view == nullptr) {
                return false;
            }
            const GuiPixelRect box = guiControlPixels(*context.hudView, *view);
            const float cx = 0.5f * (box.x0 + box.x1);
            const float cy = 0.5f * (box.y0 + box.y1);
            const float scale = context.hudView->scale;
            // Art: pixels with light in them that are not the arrow's blue; arrow: blue-dominant pixels.
            int art = 0;
            int arrow = 0;
            int total = 0;
            const int radius = static_cast<int>(18.0f * scale);
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int x = static_cast<int>(cx) + dx;
                    const int y = static_cast<int>(cy) + dy;
                    if (x < 0 || y < 0 || x >= lastFrame_.width || y >= lastFrame_.height) {
                        continue;
                    }
                    const std::uint8_t* p = &lastFrame_.rgba[(static_cast<std::size_t>(y) * lastFrame_.width + x) * 4];
                    const bool blue = p[2] > p[0] + 50 && p[2] > p[1] + 30;
                    ++total;
                    arrow += blue ? 1 : 0;
                    art += !blue && lumaAt(lastFrame_, x, y) > 45.0f ? 1 : 0;
                }
            }
            const bool mapped = map.present && map.resX > 0;
            float texelU = -1.0f;
            float texelV = -1.0f;
            map.project(context.game->position().x, context.game->position().y, texelU, texelV);
            logInfo(concat("front_end: maps ", module, " leader ", context.game->position().x, ",", context.game->position().y,
                           " entry ", context.game->scene().entry.x, ",", context.game->scene().entry.y, " texel ", texelU, ",",
                           texelV, " northAxis ", map.northAxis));
            logInfo(concat("front_end: maps ", module, " mapped=", mapped ? 1 : 0, " area=", context.game->scene().area,
                           " art=", art, "/", total, " arrow=", arrow, " window ", static_cast<int>(box.x1 - box.x0), "x",
                           static_cast<int>(box.y1 - box.y0)));
            if (!mapped) {
                return true; // retail draws no map for it; the capture is the record
            }
            const int minArrow = static_cast<int>(12.0f * scale * scale / 4.0f);
            return arrow >= std::max(8, minArrow) && art * 100 >= total * 2;
        }));
    }
}

// The shipped store screen over a module that has merchants (tar_m02ac), driven by the M key and
// real pointer events, then the loading screen a module change shows.
void FrontEndGate::buildStore()
{
    const auto painted = [this](const std::string& screen) {
        return expect(screen + "_painted", [screen](Context& context) {
            const GuiPainter::Stats& stats = context.painter->stats();
            int missing = 0;
            for (const std::string& name : stats.missingNames) {
                logInfo("front_end: " + screen + " missing image '" + name + "'");
                missing += name == "po_no" ? 0 : 1;
            }
            return stats.controls > 0 && stats.glyphs > 0 && missing == 0;
        });
    };
    const auto layout = [this](const std::string& screen) { layoutChecks(screen); };
    ops_.push_back(screenIs("mainmenu"));
    ops_.push_back(wait(8));
    ops_.push_back(expect("leave_menu", [](Context& context) {
        context.gui->close();
        context.game->setTutorialWindowsEnabledForTest(false); // the journal tutorial would cover the store
        return true;
    }));
    ops_.push_back(wait(4));
    for (int round = 0; round < 3; ++round) {
        ops_.push_back(expect("opening_conversation_settled", [](Context& context) {
            if (context.game->dialogue().active) {
                context.game->endDialogue();
            }
            return true;
        }));
        ops_.push_back(wait(20));
    }
    ops_.push_back(expect("store_module_has_a_merchant", [](Context& context) {
        return context.game->module() == "tar_m02ac" && !context.game->merchants().empty();
    }));

    // ---- the store: the path a store script takes opens the nearest merchant's shipped screen
    ops_.push_back(expect("store_panel_asked_for", [](Context& context) {
        context.game->setUiPanel(UiPanel::Merchant);
        return true;
    }));
    ops_.push_back(screenIs("store"));
    ops_.push_back(wait(3));
    ops_.push_back(painted("store"));
    layout("store");
    ops_.push_back(expect("store_lists_the_stock", [](Context& context) {
        const GuiControlState list = context.gui->state("LB_SHOPITEMS");
        const MerchantState& merchant = context.game->merchants()[static_cast<std::size_t>(context.game->activeMerchant())];
        std::size_t available = 0;
        for (const MerchantStockItem& stock : merchant.stock) {
            available += stock.infinite || stock.remaining > 0 ? 1 : 0;
        }
        return list.visible && !list.items.empty() && list.items.size() == available
            && !context.gui->state("LBL_COST_VALUE").text.empty()
            && context.gui->state("LBL_CREDITS_VALUE").text == std::to_string(context.game->gold());
    }));
    ops_.push_back(capture("store", 40));
    struct Purse {
        std::int32_t gold = 0;
        std::size_t items = 0;
        std::int32_t price = 0;
    };
    auto purse = std::make_shared<Purse>();
    ops_.push_back(expect("store_fixture_gold", [purse](Context& context) {
        context.game->setGoldForTest(100000);
        purse->gold = context.game->gold();
        purse->items = context.game->inventory().size();
        purse->price = context.game->buyPrice(0);
        return purse->gold == 100000;
    }));
    ops_.push_back(wait(4));
    ops_.push_back(clickRow("LB_SHOPITEMS", 0));
    ops_.push_back(wait(3));
    ops_.push_back(click("BTN_Accept"));
    ops_.push_back(wait(5));
    ops_.push_back(expect("store_buy_takes_gold_and_gives_the_item", [purse](Context& context) {
        return context.game->gold() == purse->gold - purse->price && purse->price > 0
            && context.game->inventory().size() >= purse->items;
    }));
    ops_.push_back(click("BTN_Examine"));
    ops_.push_back(wait(5));
    ops_.push_back(expect("store_sell_mode_lists_the_pack", [](Context& context) {
        const GuiControlState list = context.gui->state("LB_INVITEMS");
        return list.visible && !context.gui->state("LB_SHOPITEMS").visible && !list.items.empty()
            && context.gui->state("LBL_BUYSELL").text == context.game->localization().tlk().text(32130);
    }));
    ops_.push_back(capture("store_sell", 40));
    auto soldFor = std::make_shared<std::int32_t>(0);
    ops_.push_back(expect("store_sell_fixture", [soldFor](Context& context) {
        *soldFor = context.game->gold();
        return true;
    }));
    ops_.push_back(clickRow("LB_INVITEMS", 0));
    ops_.push_back(wait(3));
    ops_.push_back(click("BTN_Accept"));
    ops_.push_back(wait(5));
    ops_.push_back(expect("store_sell_pays_gold", [soldFor](Context& context) {
        return context.game->gold() > *soldFor;
    }));
    ops_.push_back(key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE));
    ops_.push_back(wait(4));
    ops_.push_back(expect("store_closes_with_escape", [](Context& context) {
        return !context.gui->active() && context.game->activeMerchant() < 0
            && context.game->activePanel() == UiPanel::None;
    }));

    // ---- the loading screen a module change shows (loadscreen.gui, the LoadScreenID picture, a hint)
    ops_.push_back(expect("loading_screen_module_requested", [](Context& context) {
        if (context.loadProbe == nullptr) {
            return false;
        }
        context.loadProbe->shown = 0;
        context.game->requestModule("tar_m02aa");
        return true;
    }));
    ops_.push_back(expect("loading_screen_shown", [](Context& context) { return context.loadProbe->shown > 0; }));
    ops_.push_back(expect("loading_screen_painted", [this](Context& context) {
        const LoadScreenProbe& probe = *context.loadProbe;
        int missing = 0;
        for (const std::string& name : probe.stats.missingNames) {
            logInfo("front_end: loading_screen missing image '" + name + "'");
            missing += 1;
        }
        logInfo(concat("front_end: loading_screen module ", probe.module, " image ", probe.image, " hint '",
                       probe.hint, "' controls ", probe.stats.controls, " glyphs ", probe.stats.glyphs, " images ",
                       probe.stats.images));
        if (probe.frame.rgba.empty()) {
            return false;
        }
        std::set<unsigned> colours;
        const std::size_t pixels = static_cast<std::size_t>(probe.frame.width) * probe.frame.height;
        const std::size_t stride = std::max<std::size_t>(1, pixels / 60000);
        for (std::size_t i = 0; i < pixels; i += stride) {
            const std::uint8_t* p = &probe.frame.rgba[i * 4];
            colours.insert((static_cast<unsigned>(p[0] >> 3) << 10) | (static_cast<unsigned>(p[1] >> 3) << 5)
                           | static_cast<unsigned>(p[2] >> 3));
        }
        if (!outDir_.empty()) {
            std::error_code ignored;
            std::filesystem::create_directories(outDir_, ignored);
            writeBmpTopDown(std::filesystem::path(outDir_) / "loadscreen.bmp", probe.frame.width, probe.frame.height,
                            probe.frame.rgba);
        }
        return probe.module == "tar_m02aa" && !probe.image.empty() && probe.stats.controls > 0
            && probe.stats.glyphs > 0 && missing == 0 && colours.size() >= 40;
    }));
    ops_.push_back(wait(40));
    ops_.push_back(expect("loading_screen_then_the_module", [](Context& context) {
        return context.game->module() == "tar_m02aa";
    }));
}

void FrontEndGate::check(bool condition, const std::string& label)
{
    logInfo(concat("front_end: ", label, "=", condition ? 1 : 0));
    if (!condition) {
        const SDL_WindowFlags flags = window_ != nullptr ? SDL_GetWindowFlags(window_) : SDL_WindowFlags{0};
        logInfo(concat("front_end: failed at frame ", gateFrames_, " ms ", SDL_GetTicks(), " focus=",
                       (flags & SDL_WINDOW_INPUT_FOCUS) != 0 ? 1 : 0, " occluded=", (flags & SDL_WINDOW_OCCLUDED) != 0 ? 1 : 0,
                       " minimized=", (flags & SDL_WINDOW_MINIMIZED) != 0 ? 1 : 0, " focus_changes=", focusChanges_));
    }
    ok_ = ok_ && condition;
}

bool FrontEndGate::controlPoint(Context& context, const std::string& tag, float fx, float fy, float& px,
                                float& py)
{
    const kotor::GuiControl* control = pointerGui(context).control(tag);
    if (control == nullptr) {
        return false;
    }
    if (pointToHud_ && context.hudView != nullptr) {
        // The HUD moves its controls with the window edges: aim where the painter put this one.
        const GuiControlState state = pointerGui(context).state(control->key);
        const GuiPixelRect rect = guiControlPixels(*context.hudView, *control, state.offsetX, state.offsetY);
        px = rect.x0 + (rect.x1 - rect.x0) * fx;
        py = rect.y0 + (rect.y1 - rect.y0) * fy;
        return true;
    }
    const GuiView& view = context.painter->view();
    px = view.pxX(static_cast<float>(control->extent.left) + static_cast<float>(control->extent.width) * fx);
    py = view.pxY(static_cast<float>(control->extent.top) + static_cast<float>(control->extent.height) * fy);
    return true;
}

bool FrontEndGate::rowPoint(Context& context, const std::string& tag, int row, float& px, float& py)
{
    const kotor::GuiControl* list = pointerGui(context).control(tag);
    if (list == nullptr || list->proto.empty()) {
        return false;
    }
    const kotor::GuiControl& proto = list->proto.front();
    const GuiView& view = context.painter->view();
    // A screen that sets its own row height (setRowHeight) is clicked at that height.
    const float shown = pointerGui(context).state(tag).rowHeight;
    const float rowHeight = shown > 0.0f ? shown : static_cast<float>(proto.extent.height);
    const float step = rowHeight + static_cast<float>(list->padding);
    // A feat row is three icons: aim at its first.
    const GuiControlState rowState = pointerGui(context).state(tag);
    const bool cells = static_cast<std::size_t>(row) < rowState.items.size() && !rowState.items[static_cast<std::size_t>(row)].cells.empty();
    px = view.pxX(static_cast<float>(proto.extent.left)
                  + (cells ? kFeatRowHeight * 0.5f : static_cast<float>(proto.extent.width) * 0.5f));
    // The row's slot as the list spreads them (guiListRowSlot), relative to the first shown row.
    const std::size_t fit = pointerGui(context).visibleRows(list->key);
    const std::size_t shownRow = static_cast<std::size_t>(std::max(0, row));
    float top = step * static_cast<float>(row);
    float height = rowHeight;
    if (shownRow < fit && rowState.pixelScroll == 0.0f) {
        const GuiRowSlot slot = guiListRowSlot(*list, rowHeight, fit, shownRow);
        top = slot.top;
        height = slot.height;
    }
    py = view.pxY(static_cast<float>(proto.extent.top) + height * 0.5f + top);
    return true;
}

void FrontEndGate::pushMotion(float x, float y)
{
    pointerX_ = x;
    pointerY_ = y;
    pointerSet_ = true;
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = SDL_GetWindowID(window_);
    event.motion.which = 1;
    event.motion.x = x;
    event.motion.y = y;
    SDL_PushEvent(&event);
}

void FrontEndGate::pushButton(float x, float y, bool down)
{
    pointerX_ = x;
    pointerY_ = y;
    pointerSet_ = true;
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.windowID = SDL_GetWindowID(window_);
    event.button.which = 1;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = down;
    event.button.clicks = 1;
    event.button.x = x;
    event.button.y = y;
    SDL_PushEvent(&event);
}

void FrontEndGate::pushWheel(float x, float y, float dy)
{
    pointerX_ = x;
    pointerY_ = y;
    pointerSet_ = true;
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.windowID = SDL_GetWindowID(window_);
    event.wheel.which = 1;
    event.wheel.y = dy;
    event.wheel.mouse_x = x;
    event.wheel.mouse_y = y;
    SDL_PushEvent(&event);
}

void FrontEndGate::pushKey(int scancode, unsigned keyCode)
{
    for (const bool down : {true, false}) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.windowID = SDL_GetWindowID(window_);
        event.key.which = 1;
        event.key.scancode = static_cast<SDL_Scancode>(scancode);
        event.key.key = static_cast<SDL_Keycode>(keyCode);
        event.key.down = down;
        SDL_PushEvent(&event);
    }
}

FrontEndGate::Op FrontEndGate::hover(const std::string& tag, float fx, float fy)
{
    return {"hover " + tag, [this, tag, fx, fy](Context& context) {
                if (age_ == 0) {
                    if (!controlPoint(context, tag, fx, fy, px_, py_)) {
                        check(false, "hover_target_" + tag);
                        return true;
                    }
                    pushMotion(px_, py_);
                }
                return age_ >= 3;
            }};
}

FrontEndGate::Op FrontEndGate::click(const std::string& tag, float fx, float fy)
{
    return {"click " + tag, [this, tag, fx, fy](Context& context) {
                if (age_ == 0) {
                    if (!controlPoint(context, tag, fx, fy, px_, py_)) {
                        check(false, "click_target_" + tag);
                        return true;
                    }
                    pushMotion(px_, py_);
                } else if (age_ == 2) {
                    pushButton(px_, py_, true);
                } else if (age_ == 4) {
                    pushButton(px_, py_, false);
                }
                return age_ >= 8;
            }};
}

FrontEndGate::Op FrontEndGate::clickRow(const std::string& tag, int row)
{
    return {"click row " + std::to_string(row) + " of " + tag, [this, tag, row](Context& context) {
                if (age_ == 0) {
                    if (!rowPoint(context, tag, row, px_, py_)) {
                        check(false, "row_target_" + tag);
                        return true;
                    }
                    pushMotion(px_, py_);
                } else if (age_ == 2) {
                    pushButton(px_, py_, true);
                } else if (age_ == 4) {
                    pushButton(px_, py_, false);
                }
                return age_ >= 8;
            }};
}

// The last row, scrolled into view first: retail lists saves in folder order (006cc160), so the newest
// save is at the bottom.
FrontEndGate::Op FrontEndGate::clickLastRow(const std::string& tag)
{
    return {"click the last row of " + tag, [this, tag](Context& context) {
                if (age_ == 0) {
                    const std::size_t count = context.gui->state(tag).items.size();
                    if (count == 0 || !context.gui->scrollTo(tag, count - 1)) {
                        check(false, "row_target_" + tag);
                        return true;
                    }
                    const std::size_t shown = count - 1 - context.gui->state(tag).scroll;
                    if (!rowPoint(context, tag, static_cast<int>(shown), px_, py_)) {
                        check(false, "row_target_" + tag);
                        return true;
                    }
                    pushMotion(px_, py_);
                } else if (age_ == 2) {
                    pushButton(px_, py_, true);
                } else if (age_ == 4) {
                    pushButton(px_, py_, false);
                }
                return age_ >= 8;
            }};
}

FrontEndGate::Op FrontEndGate::hoverRow(const std::string& tag, int row)
{
    return {"hover row " + std::to_string(row) + " of " + tag, [this, tag, row](Context& context) {
                if (age_ == 0) {
                    if (!rowPoint(context, tag, row, px_, py_)) {
                        check(false, "row_target_" + tag);
                        return true;
                    }
                    pushMotion(px_, py_);
                }
                return age_ >= 4;
            }};
}

void FrontEndGate::optionHelpProof(const std::string& screen)
{
    const std::string list = screen == "optautopause" ? "LB_DETAILS" : "LB_DESC";
    ops_.push_back(expect("option_help_on_open_" + screen, [list](Context& context) {
        const GuiControlState state = context.gui->state(list);
        return !state.items.empty() && !state.items.front().text.empty();
    }));
    for (const OptionHelpRow& row : FrontEnd::optionHelpTable()) {
        if (screen != row.screen) {
            continue;
        }
        const std::string tag = row.tag;
        const std::uint32_t strref = row.strref;
        ops_.push_back(hover(tag));
        ops_.push_back(expect("option_help_" + screen + "_" + tag, [list, tag, strref](Context& context) {
            const GuiControlState state = context.gui->state(list);
            if (!context.gui->state(tag).visible) {
                return true; // this screen hides it here; the row is still right where it shows
            }
            const bool shown = !state.items.empty() && state.items.front().text == context.game->localization().tlk().text(strref);
            if (!shown) {
                logInfo(concat("front_end: help for ", tag, " focus=", context.gui->focused(), " hovered=", context.gui->hovered(),
                               " pointer=", context.gui->pointerX(), ",", context.gui->pointerY(), " text=",
                               state.items.empty() ? std::string{} : state.items.front().text.substr(0, 40)));
            }
            return shown;
        }));
    }
}

FrontEndGate::Op FrontEndGate::wheel(const std::string& tag, float dy)
{
    return {"wheel " + tag, [this, tag, dy](Context& context) {
                if (age_ == 0) {
                    if (!controlPoint(context, tag, 0.5f, 0.5f, px_, py_)) {
                        check(false, "wheel_target_" + tag);
                        return true;
                    }
                    pushMotion(px_, py_);
                } else if (age_ == 2) {
                    pushWheel(px_, py_, dy);
                }
                return age_ >= 6;
            }};
}

FrontEndGate::Op FrontEndGate::key(int scancode, unsigned keyCode)
{
    return {"key " + std::to_string(scancode), [this, scancode, keyCode](Context&) {
                if (age_ == 0) {
                    pushKey(scancode, keyCode);
                }
                return age_ >= 6;
            }};
}

FrontEndGate::Op FrontEndGate::hitTest(const std::string& screen, std::size_t index)
{
    return {"hit test " + screen + " " + std::to_string(index), [this, screen, index](Context& context) {
                GuiSystem& gui = pointerGui(context);
                std::vector<const kotor::GuiControl*> targets;
                if (gui.screen() != nullptr) {
                    for (const kotor::GuiControl& control : gui.screen()->controls) {
                        const GuiControlState state = gui.state(control.key);
                        if ((control.type != 6 && control.type != 11) || !state.visible || !state.enabled
                            || control.extent.width < 8 || control.extent.height < 8) {
                            continue;
                        }
                        targets.push_back(&control);
                    }
                }
                // Controls that overlap another target are ambiguous: skip them.
                std::vector<const kotor::GuiControl*> clear;
                for (const kotor::GuiControl* a : targets) {
                    bool overlapped = false;
                    for (const kotor::GuiControl* b : targets) {
                        if (a != b && a->extent.left < b->extent.left + b->extent.width
                            && b->extent.left < a->extent.left + a->extent.width
                            && a->extent.top < b->extent.top + b->extent.height
                            && b->extent.top < a->extent.top + a->extent.height) {
                            overlapped = true;
                            break;
                        }
                    }
                    if (!overlapped) {
                        clear.push_back(a);
                    }
                }
                if (index >= clear.size()) {
                    return true;
                }
                const kotor::GuiControl& control = *clear[index];
                const GuiView& view = context.painter->view();
                float cx = view.pxX(static_cast<float>(control.extent.left) + 0.5f * static_cast<float>(control.extent.width));
                float cy = view.pxY(static_cast<float>(control.extent.top) + 0.5f * static_cast<float>(control.extent.height));
                // 12 design pixels left of the control's left edge: drawn outside it.
                float ox = view.pxX(static_cast<float>(control.extent.left) - 12.0f);
                if (pointToHud_ && context.hudView != nullptr) {
                    // The HUD's controls move with the window edges: the painter's own placement.
                    const GuiControlState placed = gui.state(control.key);
                    const GuiPixelRect rect = guiControlPixels(*context.hudView, control, placed.offsetX, placed.offsetY);
                    cx = 0.5f * (rect.x0 + rect.x1);
                    cy = 0.5f * (rect.y0 + rect.y1);
                    ox = rect.x0 - 12.0f * context.hudView->scale;
                }
                const std::string label = screen + "_hit_" + control.tag;
                if (age_ == 0) {
                    pushMotion(cx, cy);
                } else if (age_ == 3) {
                    if (gui.hovered() != control.key) {
                        logInfo(concat("front_end: ", label, " hovered='", gui.hovered(), "' wanted '", control.key,
                                       "' pointer ", pointerX_, ",", pointerY_, " centre ", cx, ",", cy));
                    }
                    check(gui.hovered() == control.key, label + "_inside");
                    pushMotion(ox, cy);
                } else if (age_ >= 6) {
                    check(gui.hovered() != control.key, label + "_outside");
                    return true;
                }
                return false;
            }};
}

void FrontEndGate::layoutChecks(const std::string& screen)
{
    ops_.push_back(expect(screen + "_art_aspect_uniform", [](Context& context) {
        const GuiView& v = context.painter->view();
        const float sx = (v.pxX(v.baseWidth) - v.pxX(0.0f)) / v.baseWidth;
        const float sy = (v.pxY(v.baseHeight) - v.pxY(0.0f)) / v.baseHeight;
        return scalesMatch(sx, sy);
    }));
    // RETAIL (0040ba20, paired frames ui_inventory, menu_main): a panel is drawn 1:1, centred on the
    // window by integer halves. Replaces the old legibility rule, which demanded the 1.6 stretch.
    ops_.push_back(expect(screen + "_true_size_centred", [](Context& context) {
        const GuiPainter::Stats& stats = context.painter->stats();
        const GuiView& v = context.painter->view();
        logInfo(concat("front_end: window ", v.windowWidth, "x", v.windowHeight, " scale=", v.scale,
                       " offset=", v.offsetX, ",", v.offsetY, " smallest text cell=", stats.minTextPixels, "px"));
        if (v.windowWidth < v.baseWidth || v.windowHeight < v.baseHeight) {
            return v.scale < 1.0f; // below retail's smallest mode the panel shrinks to fit (engine choice)
        }
        if (v.anchored) {
            // The HUD (mipc*) is not one centred panel: its controls keep to the window's corners and
            // edges. Only its size is checked here: 1:1.
            return v.scale == 1.0f;
        }
        return v.scale == 1.0f && v.offsetX == std::floor(0.5f * (v.windowWidth - v.baseWidth))
            && v.offsetY == std::floor(0.5f * (v.windowHeight - v.baseHeight));
    }));
    ops_.push_back(expect(screen + "_controls_inside_window", [this](Context& context) {
        const GuiView& v = context.painter->view();
        const GuiSystem& gui = pointerGui(context);
        const kotor::GuiScreen* screen = gui.screen();
        if (screen == nullptr) {
            return false;
        }
        for (const kotor::GuiControl& control : screen->controls) {
            if (!gui.state(control.key).visible || control.extent.width <= 0 || control.extent.height <= 0) {
                continue;
            }
            const bool background = control.extent.left <= 0 && control.extent.top <= 0
                && control.extent.width >= screen->extent.width && control.extent.height >= screen->extent.height;
            if (background) {
                continue;
            }
            const float x0 = v.pxX(static_cast<float>(control.extent.left));
            const float y0 = v.pxY(static_cast<float>(control.extent.top));
            const float x1 = v.pxX(static_cast<float>(control.extent.left + control.extent.width));
            const float y1 = v.pxY(static_cast<float>(control.extent.top + control.extent.height));
            // The shipped HUD frame LBL_MAPBORDER sits at (-2,-3): its own data, 3 design pixels out.
            const float slack = 3.5f * v.scale;
            if (!rectInside(x0 + slack, y0 + slack, x1 - slack, y1 - slack, v.windowWidth, v.windowHeight)) {
                logInfo(concat("front_end: control ", control.tag, " leaves the window"));
                return false;
            }
        }
        return true;
    }));
    for (std::size_t i = 0; i < 4; ++i) {
        ops_.push_back(hitTest(screen, i));
    }
}

// Checkbox indicators, the +/- arrows and stretched art, on the last capture and the painter state
// it was drawn with. `screen` names the capture.
void FrontEndGate::widgetChecks(const std::string& screen)
{
    ops_.push_back(expect(screen + "_checkbox_art_placed", [this, screen](Context& context) {
        const GuiSystem& gui = pointerGui(context);
        const kotor::GuiScreen* scr = gui.screen();
        if (scr == nullptr) {
            return false;
        }
        const GuiView& v = lastView_;
        std::vector<const kotor::GuiControl*> boxes;
        std::function<void(const kotor::GuiControl&)> walk = [&](const kotor::GuiControl& control) {
            if (control.type == 7 && gui.state(control.key).visible) {
                boxes.push_back(&control);
            }
            for (const kotor::GuiControl& child : control.children) {
                walk(child);
            }
        };
        for (const kotor::GuiControl& control : scr->controls) {
            walk(control);
        }
        bool ok = true;
        for (const kotor::GuiControl* box : boxes) {
            const Placed* indicator = nullptr;
            const Placed* label = nullptr;
            for (const Placed& item : lastStats_.indicators) {
                if (item.tag == box->tag) {
                    indicator = &item;
                }
            }
            for (const Placed& item : lastStats_.labels) {
                if (item.tag == box->tag) {
                    label = &item;
                }
            }
            if (indicator == nullptr) {
                logInfo(concat("front_end: ", screen, " checkbox ", box->tag, " drew no indicator"));
                ok = false;
                continue;
            }
            const float ew = kCheckboxEdge * v.scale; // fails on the old ring, 32 across
            const float eh = ew;
            // Fails on the art's own white: the ring wears the label's colour or the highlight's.
            const auto rgba = [](kotor::Vec3 c) {
                const auto n = [](float v) { return static_cast<unsigned>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
                return n(c.x) | (n(c.y) << 8) | (n(c.z) << 16) | (255u << 24);
            };
            const unsigned expectedBlue = rgba(box->hasTextColor ? box->textColor : kotor::Vec3{1.0f, 1.0f, 1.0f});
            const unsigned expectedLit = rgba(box->hilight.color);
            // Retail CSWGuiOptionsCheckbox::Draw 006de0e0 takes the colour from its own constants, not
            // the GUI file: hilighted 007a23c0, enabled 007a23b4, otherwise 007a2408.
            const unsigned retailLit = rgba({0.98f, 1.0f, 0.0f});
            const unsigned retailOn = rgba({0.0f, 0.66f, 0.98f});
            const unsigned retailOff = rgba({0.0f, 0.33f, 0.49f});
            if (indicator->tint != expectedBlue && indicator->tint != expectedLit && indicator->tint != retailLit
                && indicator->tint != retailOn && indicator->tint != retailOff) {
                logInfo(concat("front_end: ", screen, " checkbox ", box->tag, " ring tint ", indicator->tint));
                ok = false;
            }
            const float cx0 = v.pxX(static_cast<float>(box->extent.left));
            const float cy0 = v.pxY(static_cast<float>(box->extent.top));
            const float cx1 = v.pxX(static_cast<float>(box->extent.left + box->extent.width));
            const float cy1 = v.pxY(static_cast<float>(box->extent.top + box->extent.height));
            const bool placed = indicatorOk(*indicator, ew, eh, cx0, cy0, cx1, cy1);
            const bool clear = label == nullptr || labelClear(*label, *indicator, cx0, cx1);
            if (!placed || !clear) {
                logInfo(concat("front_end: ", screen, " checkbox ", box->tag, " indicator ", indicator->x1 - indicator->x0, "x",
                               indicator->y1 - indicator->y0, " expected ", ew, "x", eh, placed ? "" : " MISPLACED",
                               clear ? "" : " LABEL OVER INDICATOR"));
            }
            ok = ok && placed && clear;
        }
        logInfo(concat("front_end: ", screen, " checkboxes=", boxes.size(), " indicators drawn=",
                       lastStats_.indicators.size()));
        return ok;
    }));
    ops_.push_back(expect(screen + "_checkbox_state_visible", [this, screen](Context& context) {
        const GuiSystem& gui = pointerGui(context);
        float onMin = 1.0e9f;
        float offMax = -1.0e9f;
        for (const Placed& indicator : lastStats_.indicators) {
            const float luma = meanLumaAround(lastFrame_, 0.5f * (indicator.x0 + indicator.x1),
                                              0.5f * (indicator.y0 + indicator.y1), 2);
            if (gui.state(indicator.tag).selected) {
                onMin = std::min(onMin, luma);
            } else {
                offMax = std::max(offMax, luma);
            }
        }
        const bool haveOn = onMin < 1.0e8f;
        const bool haveOff = offMax > -1.0e8f;
        if (haveOn || haveOff) {
            logInfo(concat("front_end: ", screen, " checkbox centre luma on>=", haveOn ? onMin : -1.0f, " off<=",
                           haveOff ? offMax : -1.0f));
        }
        return (!haveOn || onMin > 90.0f) && (!haveOff || offMax < 70.0f)
            && (!(haveOn && haveOff) || stateDiffers(onMin, offMax));
    }));
    ops_.push_back(expect(screen + "_arrows_tinted_and_equal", [this, screen](Context& context) {
        const GuiSystem& gui = pointerGui(context);
        const Placed* left = nullptr;
        const Placed* right = nullptr;
        for (const Placed& item : lastStats_.fills) {
            if (item.tag == "BTN_DIFFLEFT") {
                left = &item;
            } else if (item.tag == "BTN_DIFFRIGHT") {
                right = &item;
            }
        }
        if (left == nullptr || right == nullptr) {
            return true; // this screen has no +/- arrows
        }
        const GuiView& v = lastView_;
        bool ok = closeTo(left->x1 - left->x0, right->x1 - right->x0, 1.5f) && closeTo(left->y1 - left->y0, right->y1 - right->y0, 1.5f);
        for (const char* tag : {"BTN_DIFFLEFT", "BTN_DIFFRIGHT"}) {
            const Placed& arrow = tag[8] == 'L' ? *left : *right;
            const kotor::GuiControl* control = gui.control(tag);
            if (control == nullptr) {
                return false;
            }
            const float ew = static_cast<float>(control->extent.width) * v.scale;
            const float eh = static_cast<float>(control->extent.height) * v.scale;
            ok = ok && closeTo(arrow.x1 - arrow.x0, ew, 1.5f) && closeTo(arrow.y1 - arrow.y0, eh, 1.5f);
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            const bool measured = brightMean(lastFrame_, arrow, r, g, b);
            logInfo(concat("front_end: ", screen, " ", tag, " ", arrow.x1 - arrow.x0, "x", arrow.y1 - arrow.y0, " px, art colour ",
                           measured ? r : -1.0f, ",", g, ",", b));
            ok = ok && measured && blueTinted(r, g, b);
        }
        return ok;
    }));
    ops_.push_back(expect(screen + "_art_not_stretched", [this, screen](Context&) {
        bool ok = true;
        for (const Placed& fill : lastStats_.fills) {
            if (stretchedArt(fill, lastView_.scale)) {
                logInfo(concat("front_end: ", screen, " control ", fill.tag, " stretches ", fill.nativeWidth, "x",
                               fill.nativeHeight, " art to ", fill.x1 - fill.x0, "x", fill.y1 - fill.y0, " px"));
                ok = false;
            }
        }
        return ok;
    }));
}

// Each widget check must reject the old behaviour: a checkbox ring stretched over its control, a
// label drawn through the ring, the same art for both states, white arrows, and the inset that
// made the left arrow a different size from the right.
void FrontEndGate::widgetProofs()
{
    ops_.push_back(expect("proof_stretched_checkbox_is_rejected", [this](Context& context) {
        const GuiSystem& gui = pointerGui(context);
        const kotor::GuiControl* box = gui.control("CB_LEVELUP");
        if (box == nullptr || lastStats_.indicators.empty()) {
            return false;
        }
        const GuiView& v = lastView_;
        const Placed& real = lastStats_.indicators.front();
        Placed old = real; // what the painter did before: the 32x32 ring over the whole 240x40 control
        old.x0 = v.pxX(static_cast<float>(box->extent.left));
        old.y0 = v.pxY(static_cast<float>(box->extent.top));
        old.x1 = v.pxX(static_cast<float>(box->extent.left + box->extent.width));
        old.y1 = v.pxY(static_cast<float>(box->extent.top + box->extent.height));
        const float ew = kCheckboxEdge * v.scale;
        const float eh = ew;
        return !indicatorOk(old, ew, eh, old.x0, old.y0, old.x1, old.y1) && stretchedArt(old, v.scale)
            && indicatorOk(real, ew, eh, real.x0 - 1.0f, real.y0 - 1.0f, real.x1 + 1.0f, real.y1 + 1.0f);
    }));
    ops_.push_back(expect("proof_label_over_indicator_is_rejected", [this](Context&) {
        if (lastStats_.indicators.empty() || lastStats_.labels.empty()) {
            return false;
        }
        const Placed& indicator = lastStats_.indicators.front();
        Placed label = lastStats_.labels.front();
        const float width = label.x1 - label.x0;
        label.x0 = indicator.x0 + 2.0f; // the label drawn through the ring, as the centred text was
        label.x1 = label.x0 + width;
        return !labelClear(label, indicator, indicator.x0, indicator.x1 + 400.0f);
    }));
    ops_.push_back(expect("proof_identical_state_art_is_rejected", [](Context&) {
        return !stateDiffers(100.0f, 100.0f) && !stateDiffers(30.0f, 30.0f) && stateDiffers(200.0f, 20.0f);
    }));
    ops_.push_back(expect("proof_white_arrows_are_rejected", [](Context&) {
        return !blueTinted(255.0f, 255.0f, 255.0f) && blueTinted(0.0f, 164.0f, 243.0f);
    }));
    ops_.push_back(expect("proof_inset_arrow_is_rejected", [this](Context&) {
        const Placed* right = nullptr;
        for (const Placed& item : lastStats_.fills) {
            if (item.tag == "BTN_DIFFRIGHT") {
                right = &item;
            }
        }
        if (right == nullptr) {
            return false;
        }
        // The left arrow before: inset by DIMENSION 32 on a 24-wide control.
        const float oldWidth = (24.0f - 2.0f * 32.0f) * lastView_.scale;
        return !closeTo(oldWidth, right->x1 - right->x0, 1.5f);
    }));
}

// Each check must reject the behaviour it exists for: stretch-to-fill art, unscaled retail
// text at a high resolution, a control off the window, an empty window margin.
void FrontEndGate::layoutProofs()
{
    ops_.push_back(expect("proof_stretch_to_fill_is_rejected", [](Context& context) {
        const GuiView& v = context.painter->view();
        const float sx = v.windowWidth / v.baseWidth;
        const float sy = v.windowHeight / v.baseHeight;
        if (scalesMatch(sx, sy) && std::fabs(v.windowWidth / v.windowHeight - v.baseWidth / v.baseHeight) > 0.01f) {
            return false; // old behaviour would have passed
        }
        logInfo(concat("front_end: stretch proof ", scalesMatch(sx, sy) ? "n/a (window aspect equals the design's)"
                                                                       : "rejected"));
        return true;
    }));
    ops_.push_back(expect("proof_stretched_panel_is_rejected", [](Context& context) {
        // The old layout scaled a 640x480 panel to fit (1.6 at 1024x768); retail draws it 1:1.
        GuiView stretched = context.painter->view();
        if (stretched.windowWidth < stretched.baseWidth || stretched.windowHeight < stretched.baseHeight) {
            return true;
        }
        stretched.scale = std::min(stretched.windowWidth / stretched.baseWidth, stretched.windowHeight / stretched.baseHeight);
        return stretched.scale == 1.0f || stretched.scale != context.painter->view().scale;
    }));
    ops_.push_back(expect("proof_control_off_window_is_rejected", [](Context& context) {
        const GuiView& v = context.painter->view();
        return !rectInside(-40.0f, 10.0f, 100.0f, 50.0f, v.windowWidth, v.windowHeight)
            && rectInside(10.0f, 10.0f, 100.0f, 50.0f, v.windowWidth, v.windowHeight);
    }));
    ops_.push_back(expect("proof_black_margin_is_rejected", [](Context&) {
        CapturedFrame black;
        black.width = 1600; // a fixed 16:9 window, so the proof holds at every size under test
        black.height = 900;
        black.rgba.assign(static_cast<std::size_t>(black.width) * black.height * 4, 0);
        return !frameCovered(black);
    }));
}

FrontEndGate::Op FrontEndGate::windowIs(const std::string& label, std::function<bool(int, int)> accept,
                                        int fullscreen)
{
    return {"window " + label, [this, label, accept, fullscreen](Context& context) {
                int w = 0;
                int h = 0;
                bool full = false;
                if (window_ != nullptr) {
                    SDL_GetWindowSizeInPixels(window_, &w, &h);
                    full = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
                }
                const GuiView& v = context.painter->view();
                const bool follows = static_cast<int>(v.windowWidth) == w && static_cast<int>(v.windowHeight) == h;
                const bool wanted = accept(w, h) && (fullscreen < 0 || full == (fullscreen == 1));
                if (wanted && follows && age_ >= 6) {
                    logInfo(concat("front_end: window now ", w, "x", h, full ? " fullscreen" : " windowed",
                                   ", the layout follows it"));
                    check(true, label);
                    return true;
                }
                if (age_ >= 240) {
                    logInfo(concat("front_end: window ", w, "x", h, " painter ", v.windowWidth, "x", v.windowHeight,
                                   full ? " fullscreen" : " windowed"));
                    check(false, label);
                    return true;
                }
                return false;
            }};
}

FrontEndGate::Op FrontEndGate::setWindow(int width, int height, bool fullscreen)
{
    return {"set window " + std::to_string(width) + "x" + std::to_string(height), [this, width, height, fullscreen](Context&) {
                if (age_ == 0 && window_ != nullptr) {
                    SDL_SetWindowFullscreen(window_, fullscreen);
                    if (!fullscreen) {
                        SDL_SetWindowSize(window_, width, height);
                    }
                }
                return age_ >= 2;
            }};
}

FrontEndGate::Op FrontEndGate::selectResolution(const std::string& text)
{
    return {"select resolution " + text, [this, text](Context& context) {
                if (age_ == 0) {
                    const std::vector<GuiListItem> items = context.gui->state("LB_RESOLUTIONS").items;
                    int row = -1;
                    for (std::size_t i = 0; i < items.size(); ++i) {
                        if (items[i].text == text) {
                            row = static_cast<int>(i);
                        }
                    }
                    if (row < 0 || !rowPoint(context, "LB_RESOLUTIONS", row, px_, py_)) {
                        check(false, "resolution_row_" + text);
                        return true;
                    }
                    pushMotion(px_, py_);
                } else if (age_ == 2) {
                    pushButton(px_, py_, true);
                } else if (age_ == 4) {
                    pushButton(px_, py_, false);
                }
                return age_ >= 8;
            }};
}

FrontEndGate::Op FrontEndGate::waitUntil(const std::string& label, std::function<bool(Context&)> test, int maxFrames)
{
    return {label, [this, label, test, maxFrames](Context& context) {
                if (test(context)) {
                    check(true, label);
                    return true;
                }
                if (age_ >= maxFrames) {
                    check(false, label);
                    return true;
                }
                return false;
            }};
}

FrontEndGate::Op FrontEndGate::wait(int frames)
{
    return {"wait", [this, frames](Context&) { return age_ >= frames; }};
}

FrontEndGate::Op FrontEndGate::expect(const std::string& label, std::function<bool(Context&)> test)
{
    return {label, [this, label, test](Context& context) {
                check(test(context), label);
                return true;
            }};
}

FrontEndGate::Op FrontEndGate::screenIs(const std::string& name)
{
    return {"screen " + name, [this, name](Context& context) {
                if (context.gui->screenName() == name && age_ >= 4
                    && (context.front->screen() == name
                        || (context.screens != nullptr && context.screens->screen() == name))) {
                    check(true, "screen_" + name);
                    return true;
                }
                if (age_ >= kScreenTimeout) {
                    check(false, "screen_" + name + "_got_" + context.gui->screenName());
                    return true;
                }
                return false;
            }};
}

namespace {

// Audit findings that are not defects, each with the evidence. Keep this short.
struct ExpectedIssue {
    const char* screen; // "" matches any
    const char* kind;
    const char* tag;
    const char* reason;
};
constexpr ExpectedIssue kExpectedIssues[] = {
    {"", "empty", "BTN_EQU", "top.gui type 6, BORDER FILL '', no TEXT: the tab's click area, its LBLH_EQU icon draws above it"},
    {"", "empty", "BTN_INV", "top.gui type 6, BORDER FILL '', no TEXT: the tab's click area, its LBLH_INV icon draws above it"},
    {"", "empty", "BTN_CHAR", "top.gui type 6, BORDER FILL '', no TEXT: the tab's click area, its LBLH_CHAR icon draws above it"},
    {"", "empty", "BTN_ABI", "top.gui type 6, BORDER FILL '', no TEXT: the tab's click area, its LBLH_ABI icon draws above it"},
    {"", "empty", "BTN_MSG", "top.gui type 6, BORDER FILL '', no TEXT: the tab's click area, its LBLH_MSG icon draws above it"},
    {"", "empty", "BTN_JOU", "top.gui type 6, BORDER FILL '', no TEXT: the tab's click area, its LBLH_JOU icon draws above it"},
    {"", "empty", "BTN_MAP", "top.gui type 6, BORDER FILL '', no TEXT: the tab's click area, its LBLH_MAP icon draws above it"},
    {"", "empty", "BTN_OPT", "top.gui type 6, BORDER FILL '', no TEXT: the tab's click area, its LBLH_OPT icon draws above it"},
    // Shipped data (mipc2*.gui, every size): frame art placed outside the window origin, a spacer, the
    // minimap window the map is painted into, and hit areas with no art or text of their own.
    {"", "image_clip", "LBL_MAPBORDER", "mipc2*.gui LBL_MAPBORDER EXTENT (-2,-3,136x137), fill lbl_minimap: the art overhangs the window edge in the file"},
    {"", "empty", "LBL_ARROW_MARGIN", "mipc2*.gui type 4, BORDER FILL '', STRREF 0xFFFFFFFF, no TEXT: a spacer rectangle"},
    {"", "empty", "LBL_MAPVIEW", "mipc2*.gui type 4 at (6,6,120x120), BORDER FILL '', no TEXT: the window the engine paints the map into (Application.cpp HUD hook)"},
    {"", "empty", "BTN_MINIMAP", "mipc2*.gui type 6 at (8,8,118x118), BORDER and HILIGHT FILL '', no TEXT: hit area over the minimap"},
    {"", "empty", "BTN_CHAR1", "mipc2*.gui type 6, BORDER and HILIGHT FILL '', no TEXT: hit area over the leader portrait (portrait drawn by the HUD hook)"},
    {"", "empty", "BTN_CHAR2", "mipc2*.gui type 6, BORDER and HILIGHT FILL '', no TEXT: hit area over the second portrait (drawn by the HUD hook)"},
    {"", "empty", "BTN_CHAR3", "mipc2*.gui type 6, BORDER and HILIGHT FILL '', no TEXT: hit area over the third portrait (drawn by the HUD hook)"},
    {"", "empty", "BTN_CLEARONE", "mipc2*.gui type 6, BORDER and HILIGHT FILL '', no TEXT: hit area over the target block's clear button"},
    {"", "empty", "BTN_CLEARONE2", "mipc2*.gui type 6 (524,679,51x53 at 1024x768), BORDER and HILIGHT FILL '', no TEXT: hit area over the first queue cell, shown with the queue (ShowActionQueue 006874c0, +0x6e94)"},
    {"", "empty", "LBL_CMBTMSGBG", "mipc28x6.gui type 4 (198,6,345x50), BORDER FILL '', TEXT '', STRREF 0xFFFFFFFF: shown with the combat line (Hud.cpp, retail +0x749c) but carries nothing of its own"},
    {"hud_paused", "empty", "BTN_UNPAUSE", "pause.gui type 6 (2,2,247x68), BORDER and HILIGHT FILL '', TEXT '', STRREF 0xFFFFFFFF: the click area over the pause panel (006c03b0, OnUnpause 006c0350)"},
    {"hud_enemy_sighted", "empty", "BTN_UNPAUSE", "pause.gui type 6, BORDER and HILIGHT FILL '', TEXT '', STRREF 0xFFFFFFFF: the click area over the pause panel, sized with it (006c00c0)"},
    {"character", "empty", "BTN_3DCHAR", "character.gui type 6 (55,110,258x294), BORDER and HILIGHT CORNER/EDGE/FILL '': hit area over the 3D model frame the backend draws"},
    // A sub-panel laid into MAINCG is clipped to its own extent, as retail draws a child panel's controls.
    {"levelup_main", "image_clip", "LBL_BG", "leveluppnl.gui LBL_BG (lbl_lvluppnl) reaches below the panel's EXTENT; retail clips a child panel to it (paired frame levelup_main)"},
    {"levelup_granted_hub_open", "image_clip", "LBL_BG", "the same level-up hub as levelup_main: leveluppnl.gui LBL_BG clipped to its panel"},
    {"levelup_granted_after_ok", "empty", "BTN_3DCHAR", "the character sheet, as capture character: BTN_3DCHAR is the hit area over the 3D model frame"},
    {"levelup_main_attrs", "image_clip", "LBL_BG", "as levelup_main"},
    // Retail 006b9970 writes an empty string (0x73d71c) to both left-hand labels while the left weapon
    // slot is empty; only the right column shows the bare-hand values (paired walk 0032 menus_equip).
    {"equip", "empty", "LBL_ATKL", "equip.gui LBL_ATKL: blank with nothing in the left hand (006b9970, walk 0032 pair)"},
    {"equip", "empty", "LBL_TOHITL", "equip.gui LBL_TOHITL: blank with nothing in the left hand (006b9970, walk 0032 pair)"},
    {"equip", "text_overflow", "LBL_VITALITY",
     "fnt_d16x16b (0040b360, the shipped Use Small Fonts=0): retail's '22/22' spans the whole 52 px label (paired "
     "ui_equip), so a wider '24/27' passes it by 2 px as retail's text does"},
    // Text shown only after a choice the gate frame has not made.
    {"map", "empty", "LBL_MapNote", "map.gui type 4, STRREF 0xFFFFFFFF: retail 006929b0 writes an empty string while no pin is selected; an area without an enabled note shows none (map_pins check)"},
};
constexpr bool kEnforceAudits = true;

// Items whose icon cannot resolve, by resref. baseitems.2da: row 51 (g_i_recordrod01) is blank; rows 82/83/84
// (creature pierce, bite, hide) name item classes w_crpierce / w_crbite / w_crhide whose art ships only as
// iw_crhide_002..004 (the hide variations that exist); row 63 (torch) names I_Torch with no ii_torch_* art.
// Their defaulticon cells (iw_creature, ii_device) are no resource of any type in the five passes either.
constexpr const char* kIconlessItems[] = {
    "g_i_crhide001", "g_i_crhide002", "g_i_crhide003", "g_i_crhide004", "g_i_crhide005", "g_i_crhide006",
    "g_i_crhide007", "g_i_crhide008", "g_i_crhide009", "g_i_crhide010", "g_i_crhide011", "g_i_crhide012",
    "g_i_crhide013", "g_i_recordrod01", "g_i_torch01", "g_w_crgore001", "g_w_crgore002", "g_w_crslprc001",
    "g_w_crslprc002", "g_w_crslprc003", "g_w_crslprc004", "g_w_crslprc005", "w_blhvy001", "w_lghtsbr001", "w_null"};

} // namespace

int FrontEndGate::auditFrame(const std::string& name, const GuiPainter::Stats& stats)
{
    int unexpected = 0;
    for (const GuiPainter::Stats::Issue& issue : stats.issues) {
        bool allowed = false;
        for (const ExpectedIssue& entry : kExpectedIssues) {
            allowed = allowed
                || (issue.kind == entry.kind && issue.tag == entry.tag
                    && (entry.screen[0] == 0 || name.rfind(entry.screen, 0) == 0));
        }
        if (allowed) {
            logInfo(concat("front_end: ", name, " allowed ", issue.kind, " ", issue.tag, " ", issue.detail));
            continue;
        }
        ++unexpected;
        logInfo(concat("front_end: ", name, " audit ", issue.kind, " ", issue.tag, " ", issue.detail));
    }
    if (kEnforceAudits) {
        check(unexpected == 0, name + "_audit_clean");
    }
    return unexpected;
}

FrontEndGate::Op FrontEndGate::capture(const std::string& name, int minColours, bool fullScreen)
{
    return {"capture " + name, [this, name, minColours, fullScreen](Context& context) {
                if (age_ < 3) {
                    if (age_ == 0) {
                        CapturedFrame warm;
                        context.backend->captureFrame(warm); // GL arms its pre-swap read here
                    }
                    return false;
                }
                CapturedFrame frame;
                if (!context.backend->captureFrame(frame) || frame.rgba.empty()) {
                    check(false, "capture_" + name);
                    return true;
                }
                lastFrame_ = frame;
                lastStats_ = context.painter->stats();
                logInfo(concat("front_end: captured ", name, " hovered='", context.gui->hovered(), "' focused='",
                               context.gui->focused(), "' pointer ", pointerX_, ",", pointerY_));
                lastView_ = context.painter->view();
                std::set<unsigned> colours;
                const std::size_t pixels = static_cast<std::size_t>(frame.width) * frame.height;
                const std::size_t stride = std::max<std::size_t>(1, pixels / 60000);
                for (std::size_t i = 0; i < pixels; i += stride) {
                    const std::uint8_t* p = &frame.rgba[i * 4];
                    colours.insert((static_cast<unsigned>(p[0] >> 3) << 10) | (static_cast<unsigned>(p[1] >> 3) << 5)
                                   | static_cast<unsigned>(p[2] >> 3));
                }
                const int unexpected = auditFrame(name, context.painter->stats());
                logInfo(concat("front_end: ", name, " colours=", colours.size(), " glyphs=",
                               context.painter->stats().glyphs, " images=", context.painter->stats().images,
                               " missing=", context.painter->stats().missingImages + unexpected, " (images ",
                               context.painter->stats().missingImages, " text_overflow ",
                               context.painter->stats().textOverflow, " text_clip ",
                               context.painter->stats().textClipped, " image_clip ",
                               context.painter->stats().imageClipped, " empty ",
                               context.painter->stats().emptyControls, " unexpected ", unexpected, ")"));
                check(static_cast<int>(colours.size()) >= minColours, name + "_not_blank");
                if (fullScreen) {
                    float worst = 0.0f;
                    bool margin = false;
                    const bool covered = frameCovered(frame, &worst, &margin);
                    logInfo(concat("front_end: ", name, " window ", frame.width, "x", frame.height,
                                   margin ? " darkest margin probe luma=" : " no margin (4:3), luma=", worst,
                                   " backdrop=", context.painter->stats().backdrop));
                    check(covered && !context.painter->stats().backdrop.empty(), name + "_window_covered");
                }
                if (!outDir_.empty()) {
                    std::error_code ignored;
                    std::filesystem::create_directories(outDir_, ignored);
                    writeBmpTopDown(std::filesystem::path(outDir_) / (name + ".bmp"), frame.width, frame.height,
                                    frame.rgba);
                }
                return true;
            }};
}

void FrontEndGate::build()
{
    const auto painted = [this](const std::string& screen) {
        return expect(screen + "_painted", [screen](Context& context) {
            const GuiPainter::Stats& stats = context.painter->stats();
            // 'po_no' is named by partyselection.gui's own LBL_CHARn fills and exists nowhere in the
            // install (not in the BIFs, patch, ERFs or texture packs): retail finds no art there too.
            int missing = 0;
            for (const std::string& name : stats.missingNames) {
                logInfo("front_end: " + screen + " missing image '" + name + "'");
                missing += name == "po_no" ? 0 : 1;
            }
            const bool onlyShippedGap = stats.missingNames.size() < 16 && missing == 0;
            return stats.controls > 0 && stats.glyphs > 0 && (stats.missingImages == 0 || onlyShippedGap);
        });
    };
    const auto saves = savesDir_;
    const auto layout = [this](const std::string& screen) { layoutChecks(screen); };
    const auto widgets = [this](const std::string& screen) { widgetChecks(screen); };
    // Audit 5: every item, feat, power, skill and class icon resolves to a texture. What does not is
    // listed with its evidence: creature natural weapons and test items carry no icon (no ii_* art
    // for item classes w_cr*, and no UTI template icon), special/item/monster abilities have no
    // iconresref in spells.2da and are not asked, the class icons IR_* ship in no archive (all five passes scanned).
    ops_.push_back(expect("icons_resolve", [](Context& context) {
        if (context.screens == nullptr) {
            return false;
        }
        int unexpected = 0;
        int total = 0;
        int expectedGaps = 0;
        for (const GameScreens::IconGroup& group : context.screens->iconAudit()) {
            int groupUnexpected = 0;
            for (const std::string& failure : group.failures) {
                // The exact 25 items (evidence per item: scratchpad icon_gaps.tsv): each one's baseitems row
                // names an item class with no art in any archive, or has no item class at all.
                bool itemGap = false;
                for (const char* resref : kIconlessItems) {
                    itemGap = itemGap || failure.rfind(std::string(resref) + " ", 0) == 0;
                }
                const bool gap = group.name == "classes" || (group.name == "items" && itemGap)
                    // feat.2da row 93 PROFICIENCY_ALL: the grant-all feat has no icon and no description cell.
                    || (group.name == "feat" && failure.rfind("93 ", 0) == 0);
                if (!gap) {
                    ++groupUnexpected;
                }
            }
            const bool spellsGap = false;
            total += group.total;
            expectedGaps += group.missing - groupUnexpected;
            logInfo(concat("front_end: icons ", group.name, " total=", group.total, " missing=", group.missing,
                           " unexpected=", spellsGap ? 0 : groupUnexpected));
            for (const std::string& failure : group.failures) {
                logInfo(concat("front_end: icons ", group.name, " unresolved ", failure));
            }
            unexpected += spellsGap ? 0 : groupUnexpected;
        }
        logInfo(concat("front_end: icons total=", total, " expected_gaps=", expectedGaps, " unexpected=", unexpected));
        return unexpected == 0;
    }));
    // A save to list: the gate writes one through the real save path when the folder is empty.
    ops_.push_back(expect("save_fixture", [saves](Context& context) {
        context.game->setTutorialWindowsEnabledForTest(false);
        if (!FrontEnd::listSaves(saves).empty()) {
            return true;
        }
        return context.game->saveOriginalSave(saves / "000001 - Gate", "Gate Save");
    }));

    // ---- main menu
    ops_.push_back(screenIs("mainmenu"));
    ops_.push_back(wait(8));
    ops_.push_back(painted("mainmenu"));
    layout("mainmenu");
    layoutProofs();
    ops_.push_back(expect("mainmenu_debug_controls_hidden", [](Context& context) {
        return !context.gui->state("BTN_WARP").visible && !context.gui->state("LB_MODULES").visible;
    }));
    ops_.push_back(expect("mainmenu_3d_scene_staged", [](Context& context) { return context.menuStaged; }));
    ops_.push_back(capture("mainmenu", 60, true));
    widgets("mainmenu");
    ops_.push_back(hover("BTN_OPTIONS"));
    ops_.push_back(expect("mainmenu_hover_state", [](Context& context) {
        const kotor::GuiControl* options = context.gui->control("BTN_OPTIONS");
        const kotor::GuiControl* newGame = context.gui->control("BTN_NEWGAME");
        return options != nullptr && context.gui->hovered() == options->key
            && (newGame == nullptr || context.gui->hovered() != newGame->key);
    }));
    ops_.push_back(capture("mainmenu_hover", 60, true));

    // ---- options and its sub-screens
    ops_.push_back(click("BTN_OPTIONS"));
    ops_.push_back(screenIs("optionsmain"));
    ops_.push_back(painted("optionsmain"));
    layout("optionsmain");
    ops_.push_back(capture("optionsmain", 40, true));
    widgets("optionsmain");
    optionHelpProof("optionsmain");
    ops_.push_back(click("BTN_GAMEPLAY"));
    ops_.push_back(screenIs("optgameplay"));
    ops_.push_back(painted("optgameplay"));
    layout("optgameplay");
    ops_.push_back(expect("difficulty_label", [](Context& context) {
        const GuiControlState state = context.gui->state("BTN_DIFFICULTY");
        return state.hasText && !state.text.empty();
    }));
    ops_.push_back(capture("optgameplay", 40, true));
    widgets("optgameplay");
    widgetProofs();
    optionHelpProof("optgameplay");
    // A control with no event 0 handler (BTN_DIFFLEFT, retail 006e69a0) leaves the text as it is.
    auto helpText = std::make_shared<std::string>();
    ops_.push_back(hover("CB_AUTOSAVE"));
    ops_.push_back(expect("option_help_kept_before", [helpText](Context& context) {
        *helpText = context.gui->state("LB_DESC").items.front().text;
        return !helpText->empty();
    }));
    ops_.push_back(hover("BTN_DIFFLEFT"));
    ops_.push_back(expect("option_help_kept_on_a_control_with_none", [helpText](Context& context) {
        return context.gui->state("LB_DESC").items.front().text == *helpText;
    }));
    ops_.push_back(expect("combat_movement_default_is_on", [](Context& context) {
        // swkotor.ini "Combat Movement=1" (paired frame opt_gameplay shows it on).
        context.settings->resetGroup("gameplay.");
        return context.settings->get("gameplay.disablemouse") != 0.0f;
    }));
    ops_.push_back(click("BTN_MOUSE"));
    ops_.push_back(screenIs("optmouse"));
    ops_.push_back(painted("optmouse"));
    optionHelpProof("optmouse");
    ops_.push_back(click("BTN_BACK"));
    ops_.push_back(screenIs("optgameplay"));
    auto before = std::make_shared<float>(0.0f);
    ops_.push_back(expect("autosave_before", [before](Context& context) {
        *before = context.settings->get("gameplay.autosave");
        return context.gui->state("CB_AUTOSAVE").selected == (*before != 0.0f);
    }));
    ops_.push_back(click("CB_AUTOSAVE"));
    ops_.push_back(expect("autosave_toggled", [before](Context& context) {
        return context.settings->get("gameplay.autosave") != *before
            && context.gui->state("CB_AUTOSAVE").selected == (context.settings->get("gameplay.autosave") != 0.0f);
    }));
    auto difficulty = std::make_shared<std::string>();
    ops_.push_back(expect("difficulty_before", [difficulty](Context& context) {
        *difficulty = context.gui->state("BTN_DIFFICULTY").text;
        return true;
    }));
    ops_.push_back(click("BTN_DIFFRIGHT"));
    ops_.push_back(click("BTN_DIFFLEFT"));
    ops_.push_back(expect("difficulty_cycles_back", [difficulty](Context& context) {
        return context.gui->state("BTN_DIFFICULTY").text == *difficulty;
    }));
    ops_.push_back(click("BTN_BACK"));
    ops_.push_back(screenIs("optionsmain"));

    ops_.push_back(click("BTN_SOUND"));
    ops_.push_back(screenIs("optsound"));
    ops_.push_back(painted("optsound"));
    layout("optsound");
    ops_.push_back(click("SLI_MUSIC", 0.25f, 0.5f));
    ops_.push_back(wait(3));
    ops_.push_back(expect("music_slider_value", [](Context& context) {
        const float value = context.gui->value("SLI_MUSIC");
        return value > 10.0f && value < 45.0f;
    }));
    ops_.push_back(expect("music_setting_follows_slider", [](Context& context) {
        return std::abs(context.settings->get("sound.music") - context.gui->value("SLI_MUSIC")) < 1.0f;
    }));
    ops_.push_back(expect("music_volume_applied", [](Context& context) {
        return std::abs(context.game->audio().userVolumes().music * 100.0f - context.settings->get("sound.music"))
            < 1.0f;
    }));
    ops_.push_back(capture("optsound", 40, true));
    widgets("optsound");
    optionHelpProof("optsound");
    ops_.push_back(click("BTN_ADVANCED"));
    ops_.push_back(screenIs("optsoundadv"));
    optionHelpProof("optsoundadv");
    ops_.push_back(click("BTN_BACK"));
    ops_.push_back(screenIs("optsound"));
    ops_.push_back(click("BTN_BACK"));
    ops_.push_back(screenIs("optionsmain"));
    ops_.push_back(click("BTN_GRAPHICS"));
    ops_.push_back(screenIs("optgraphics"));
    ops_.push_back(painted("optgraphics"));
    layout("optgraphics");
    ops_.push_back(capture("optgraphics", 40, true));
    widgets("optgraphics");
    optionHelpProof("optgraphics");
    ops_.push_back(click("BTN_ADVANCED"));
    ops_.push_back(screenIs("optgraphicsadv"));
    optionHelpProof("optgraphicsadv");
    ops_.push_back(click("BTN_BACK"));
    ops_.push_back(screenIs("optgraphics"));
    ops_.push_back(click("BTN_BACK"));
    ops_.push_back(screenIs("optionsmain"));
    ops_.push_back(click("BTN_AUTOPAUSE"));
    ops_.push_back(screenIs("optautopause"));
    ops_.push_back(painted("optautopause"));
    optionHelpProof("optautopause");
    ops_.push_back(click("BTN_BACK"));
    ops_.push_back(screenIs("optionsmain"));
    ops_.push_back(click("BTN_FEEDBACK"));
    ops_.push_back(screenIs("optfeedback"));
    ops_.push_back(painted("optfeedback"));
    layout("optfeedback");
    // Retail 006e1300: nine checkbox rows, each with its own help (006de9b0) and setting (006de550 ..).
    ops_.push_back(expect("feedback_lists_the_rows", [](Context& context) {
        const GuiControlState list = context.gui->state("LB_OPTIONS");
        const auto rows = FrontEnd::feedbackTable();
        if (list.items.size() != rows.size()) {
            return false;
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (list.items[i].text != context.game->localization().tlk().text(rows[i].name)
                || list.items[i].check != (context.settings->get(rows[i].key) != 0.0f ? 1 : 0)) {
                return false;
            }
        }
        return true;
    }));
    ops_.push_back(expect("feedback_first_row_help_on_open", [](Context& context) {
        const GuiControlState help = context.gui->state("LB_DESC");
        return !help.items.empty() && !help.items.front().text.empty();
    }));
    ops_.push_back(capture("optfeedback", 40, true));
    for (int row = 0; row < 3; ++row) {
        const std::uint32_t strref = FrontEnd::feedbackTable()[static_cast<std::size_t>(row)].help;
        ops_.push_back(hoverRow("LB_OPTIONS", row));
        ops_.push_back(expect("feedback_row_help_" + std::to_string(row), [strref](Context& context) {
            const GuiControlState help = context.gui->state("LB_DESC");
            return !help.items.empty() && help.items.front().text == context.game->localization().tlk().text(strref);
        }));
    }
    auto minimapBefore = std::make_shared<float>(0.0f);
    ops_.push_back(expect("feedback_toggle_before", [minimapBefore](Context& context) {
        *minimapBefore = context.settings->get("feedback.minimap");
        return *minimapBefore != 0.0f;
    }));
    ops_.push_back(clickRow("LB_OPTIONS", 3));
    ops_.push_back(wait(2));
    ops_.push_back(expect("feedback_click_toggles_and_shows", [](Context& context) {
        return context.settings->get("feedback.minimap") == 0.0f && context.gui->state("LB_OPTIONS").items[3].check == 0;
    }));
    ops_.push_back(click("BTN_DEFAULT"));
    ops_.push_back(wait(2));
    ops_.push_back(expect("feedback_default_restores_the_reset_values", [](Context& context) {
        for (const FeedbackRow& row : FrontEnd::feedbackTable()) {
            // 0061d950: word & 0xfffffcbe | 0x4be
            const bool retail = ((0x4beu & row.bit) != 0u);
            if ((context.settings->get(row.key) != 0.0f) != retail) {
                return false;
            }
        }
        return true;
    }));
    ops_.push_back(click("BTN_BACK"));
    ops_.push_back(screenIs("optionsmain"));

    // ---- keyboard: arrows move the focus, Enter activates, Escape goes back
    ops_.push_back(key(SDL_SCANCODE_DOWN, SDLK_DOWN));
    ops_.push_back(expect("key_focus_set", [](Context& context) { return !context.gui->focused().empty(); }));
    auto focus = std::make_shared<std::string>();
    ops_.push_back(expect("key_focus_before", [focus](Context& context) {
        *focus = context.gui->focused();
        return true;
    }));
    ops_.push_back(key(SDL_SCANCODE_DOWN, SDLK_DOWN));
    ops_.push_back(expect("key_focus_moves", [focus](Context& context) {
        return context.gui->focused() != *focus && !context.gui->focused().empty();
    }));
    ops_.push_back(key(SDL_SCANCODE_RETURN, SDLK_RETURN));
    ops_.push_back(wait(4));
    ops_.push_back(expect("key_enter_opens_screen", [](Context& context) {
        return context.gui->screenName() != "optionsmain" && context.gui->screenName().rfind("opt", 0) == 0;
    }));
    ops_.push_back(key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE));
    ops_.push_back(screenIs("optionsmain"));
    ops_.push_back(click("BTN_BACK"));
    ops_.push_back(screenIs("mainmenu"));

    // ---- load game: the real listing of the saves folder
    ops_.push_back(click("BTN_LOADGAME"));
    ops_.push_back(screenIs("saveload"));
    ops_.push_back(painted("saveload"));
    layout("saveload");
    ops_.push_back(expect("saves_listed", [saves](Context& context) {
        return context.gui->state("LB_GAMES").items.size() == FrontEnd::listSaves(saves).size()
            && !context.gui->state("LB_GAMES").items.empty();
    }));
    // The newest save, at the bottom (006cc160 keeps folder order); the gate's fixture, first, has no area.
    ops_.push_back(clickLastRow("LB_GAMES"));
    ops_.push_back(wait(4));
    ops_.push_back(expect("save_selected", [](Context& context) {
        const std::size_t count = context.gui->state("LB_GAMES").items.size();
        return count > 0 && context.gui->listSelection("LB_GAMES") == count - 1
            && context.gui->state("BTN_SAVELOAD").enabled;
    }));
    ops_.push_back(expect("save_details_shown", [](Context& context) {
        return !context.gui->state("LBL_PLANETNAME").text.empty();
    }));
    ops_.push_back(expect("save_thumbnail_uploaded", [](Context& context) {
        const GuiControlState state = context.gui->state("LBL_SCREENSHOT");
        return state.visible && state.hasFill && context.painter->texture(state.fill) != nullptr;
    }));
    ops_.push_back(capture("saveload", 50, true));
    widgets("saveload");
    ops_.push_back(click("BTN_BACK"));
    ops_.push_back(screenIs("mainmenu"));

    // ---- movies
    ops_.push_back(click("BTN_MOVIES"));
    ops_.push_back(screenIs("titlemovie"));
    ops_.push_back(painted("titlemovie"));
    layout("titlemovie");
    ops_.push_back(expect("movies_listed", [](Context& context) {
        return !context.gui->state("LB_MOVIES").items.empty();
    }));
    ops_.push_back(hover("LB_MOVIES"));
    ops_.push_back(capture("titlemovie", 40, true));
    widgets("titlemovie");
    ops_.push_back(click("BTN_BACK"));
    ops_.push_back(screenIs("mainmenu"));

    // ---- the screens over the world: opened by real key presses, driven by real clicks
    ops_.push_back(expect("leave_menu", [](Context& context) {
        context.gui->close();
        return true;
    }));
    ops_.push_back(wait(4));
    // The HUD over the world, once the module's opening conversation is out of the way. It is let play to its
    // first reply list first: entry 54 holds the talk while Trask crosses the room (ActionPauseConversation,
    // ActionMoveToObject, ActionResumeConversation), so ending it at a fixed frame cut it before any line was said
    // on a slow backend and left the Messages list empty and the later conversation at its first entry.
    ops_.push_back(waitUntil("opening_conversation_reaches_the_replies", [](Context& context) {
        const DialogueView& view = context.game->dialogue();
        if (!view.active) {
            return true;
        }
        bool sayable = false;
        for (const DialogueOption& option : view.options) {
            sayable = sayable || (option.reply != UINT32_MAX && option.text != "(Continue)");
        }
        return sayable && !context.game->dialogueLineSpeaking();
    }, 2400));
    ops_.push_back(expect("opening_conversation_ended", [](Context& context) {
        if (context.game->dialogue().active) {
            context.game->endDialogue();
        }
        return true;
    }));
    ops_.push_back(wait(30));
    // The conversation's owner can restart it from its script; end it again until it stays out.
    for (int round = 0; round < 3; ++round) {
        ops_.push_back(expect("opening_conversation_settled", [](Context& context) {
            if (context.game->dialogue().active) {
                context.game->endDialogue();
            }
            return true;
        }));
        ops_.push_back(wait(20));
    }
    ops_.push_back(expect("hud_without_conversation", [](Context& context) {
        return !context.game->dialogue().active;
    }));
    // The HUD has no text until something is targeted: images and bars are what it paints.
    ops_.push_back(expect("hud_painted", [](Context& context) {
        const GuiPainter::Stats& stats = context.painter->stats();
        return stats.controls > 0 && stats.images > 0 && stats.missingImages == 0;
    }));
    ops_.push_back(capture("hud", 60));
    // The HUD keeps retail's edge anchoring at any aspect: its right-most and bottom-most art
    // sits at the window's far edges, not where a centred 4:3 layout would leave it.
    ops_.push_back(expect("hud_edge_anchored", [](Context& context) {
        if (context.hudStats == nullptr || context.hudView == nullptr) {
            return false;
        }
        const GuiPainter::Stats& s = *context.hudStats;
        const GuiView& v = *context.hudView;
        logInfo(concat("front_end: hud ", v.windowWidth, "x", v.windowHeight, " scale=", v.scale,
                       " right-most art ", s.maxRight, " bottom-most art ", s.maxBottom));
        return s.maxRight >= 0.9f * v.windowWidth && s.maxBottom >= 0.9f * v.windowHeight;
    }));
    // The minimap box (LBL_MAPVIEW, design 6,6 120x120, top-left anchored) holds the area's map,
    // not the world seen through an empty frame.
    ops_.push_back(expect("hud_minimap_shows_the_map", [this](Context& context) {
        if (context.hudView == nullptr || context.hudGui == nullptr || lastFrame_.rgba.empty()) {
            return false;
        }
        const GuiView& v = *context.hudView;
        const kotor::GuiControl* view = context.hudGui->control("LBL_MAPVIEW");
        if (view == nullptr) {
            return false;
        }
        const GuiPixelRect box = guiControlPixels(v, *view);
        const int x0 = static_cast<int>(box.x0) + 3;
        const int y0 = static_cast<int>(box.y0) + 3;
        const int x1 = static_cast<int>(box.x1) - 3;
        const int y1 = static_cast<int>(box.y1) - 3;
        float dark = 0.0f;
        float bright = 0.0f;
        mapShares(lastFrame_, x0, y0, x1, y1, dark, bright);
        // The same measurement over world-only pixels (the same-sized box at the left of the
        // frame, nothing of the HUD there) must be rejected: that is what the empty box looked like.
        float worldDark = 0.0f;
        float worldBright = 0.0f;
        const int wx = static_cast<int>(0.12f * v.windowWidth);
        const int wy = static_cast<int>(0.45f * v.windowHeight);
        mapShares(lastFrame_, wx, wy, wx + (x1 - x0), wy + (y1 - y0), worldDark, worldBright);
        logInfo(concat("front_end: minimap box dark=", dark, " bright=", bright, "; world-only box dark=", worldDark,
                       " bright=", worldBright));
        // Mostly dark but not black: the unseen cells' blackdot grid (00688100) covers the bright art until walked over.
        const auto looksLikeMap = [](float d, float) { return d >= 0.35f && d <= 0.98f; };
        check(!looksLikeMap(worldDark, worldBright), "proof_world_only_box_is_rejected");
        return looksLikeMap(dark, bright);
    }));
    ops_.push_back(expect("proof_unanchored_hud_is_rejected", [](Context& context) {
        if (context.hudStats == nullptr || context.hudView == nullptr) {
            return false;
        }
        const GuiPainter::Stats& s = *context.hudStats;
        const GuiView& v = *context.hudView;
        const float extraW = v.windowWidth / v.scale - v.baseWidth;
        const float extraH = v.windowHeight / v.scale - v.baseHeight;
        // Without the anchoring shift the same art would end this far from the window edges.
        const float rightUnanchored = s.maxRight - extraW * v.scale;
        const float bottomUnanchored = s.maxBottom - extraH * v.scale;
        const bool rejectedRight = extraW * v.scale < 8.0f || rightUnanchored < 0.9f * v.windowWidth;
        const bool rejectedBottom = extraH * v.scale < 8.0f || bottomUnanchored < 0.9f * v.windowHeight;
        return rejectedRight && rejectedBottom;
    }));
    // The pointer in play: free (so the bar below answers it with no key press), captured for camera
    // look only while the right button is held, free again on release. The probe is the window's
    // real relative-mouse state; it reads true while the button is held, so a pass of the free
    // reading is not the probe being stuck.
    {
        const auto rightButton = [this](bool down) {
            return Op{down ? "right button down" : "right button up", [this, down](Context&) {
                          if (age_ == 0) {
                              SDL_Event event{};
                              event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                              event.button.windowID = SDL_GetWindowID(window_);
                              event.button.which = 1;
                              event.button.button = SDL_BUTTON_RIGHT;
                              event.button.down = down;
                              event.button.clicks = 1;
                              event.button.x = pointerX_;
                              event.button.y = pointerY_;
                              SDL_PushEvent(&event);
                          }
                          return age_ >= 4;
                      }};
        };
        ops_.push_back(expect("mouse_free_in_play", [this](Context&) { return !SDL_GetWindowRelativeMouseMode(window_); }));
        ops_.push_back(rightButton(true));
        ops_.push_back(expect("look_captured_while_right_held", [this](Context&) { return SDL_GetWindowRelativeMouseMode(window_); }));
        ops_.push_back(rightButton(false));
        ops_.push_back(expect("look_released_with_right_button", [this](Context&) { return !SDL_GetWindowRelativeMouseMode(window_); }));
    }
    // The menu bar across the top, by real pointer events (the cursor is free in play; camera look
    // is taken only while the right button is held).
    ops_.push_back(wait(4));
    ops_.push_back(expect("hud_top_bar_present", [this](Context& context) {
        pointToHud_ = true;
        return context.hudGui != nullptr && context.hudGui->screen() != nullptr
            && context.hudGui->control("BTN_INV") != nullptr;
    }));
    ops_.push_back(wait(2));
    layout("hud_top");
    ops_.push_back(click("BTN_INV"));
    ops_.push_back(wait(4));
    ops_.push_back(expect("hud_top_bar_opens_inventory", [this](Context& context) {
        pointToHud_ = false;
        return context.game->activePanel() == UiPanel::Inventory;
    }));
    // The shipped inventory screen covers the HUD: it opens, and Escape closes it.
    ops_.push_back(screenIs("inventory"));
    ops_.push_back(key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE));
    ops_.push_back(wait(4));
    ops_.push_back(expect("hud_top_bar_inventory_closes", [](Context& context) {
        return context.game->activePanel() == UiPanel::None && !context.gui->active();
    }));
    ops_.push_back(wait(2));
    // ---- the rest of the HUD (mipc2<size>.gui), by real pointer events: the pause, solo and action
    // controls; each answer is read from the session or the control, and each has a proof that the
    // reading is not satisfied by nothing happening.
    {
        const auto hudOn = [this](bool on) {
            return expect(on ? "hud_pointer_on" : "hud_pointer_off", [this, on](Context&) {
                pointToHud_ = on;
                return true;
            });
        };
        ops_.push_back(expect("hud_layout_is_the_pc_interface", [](Context& context) {
            return context.hudGui != nullptr && context.hudGui->screenName().rfind("mipc2", 0) == 0
                && context.hudGui->control("BTN_ACTION0") != nullptr && context.hudGui->control("TB_PAUSE") != nullptr;
        }));
        ops_.push_back(hudOn(true));
        auto clock = std::make_shared<float>(0.0f);

        // Pause: the world waits and the toggle shows it; a second press lets it run.
        ops_.push_back(click("TB_PAUSE"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("hud_pause_toggle_is_selected", [](Context& context) {
            return context.hudGui->state("TB_PAUSE").selected;
        }));
        ops_.push_back(expect("hud_pause_mark", [clock](Context& context) {
            *clock = context.game->elapsedSeconds();
            return true;
        }));
        ops_.push_back(wait(24));
        ops_.push_back(expect("hud_pause_freezes_world", [clock](Context& context) {
            return std::fabs(context.game->elapsedSeconds() - *clock) < 1.0e-4f;
        }));
        ops_.push_back(capture("hud_paused", 6)); // the pause.gui panel (006c03b0) at the top right
        ops_.push_back(click("TB_PAUSE"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("hud_pause_toggle_is_released", [clock](Context& context) {
            *clock = context.game->elapsedSeconds();
            return !context.hudGui->state("TB_PAUSE").selected;
        }));
        ops_.push_back(wait(24));
        ops_.push_back(expect("proof_hud_world_runs_after_unpause", [clock](Context& context) {
            return context.game->elapsedSeconds() > *clock + 0.05f;
        }));

        // Solo mode. Retail shows the toggle only while the party table has a member (00686ba0, the
        // paired HUD with no party has no such icon), so one recruit joins for this check.
        ops_.push_back(expect("hud_solo_fixture_member", [](Context& context) {
            kotor::Vec3 at = context.game->position();
            at.x += 1.5f;
            context.game->addAvailableNpcByTemplate(kNpcCarth, "p_carth");
            const std::uint32_t carth = context.game->spawnAvailableNpc(kNpcCarth, at, 0.0f);
            context.game->addPartyNpc(kNpcCarth, carth);
            return !context.game->partyTable().members.empty();
        }));
        ops_.push_back(wait(8));
        ops_.push_back(click("TB_SOLO"));
        ops_.push_back(wait(8)); // a 4K frame can take long enough for the release to land a frame late
        ops_.push_back(expect("hud_solo_toggle_sets_solo_mode", [](Context& context) {
            return context.game->soloMode() && context.hudGui->state("TB_SOLO").selected;
        }));
        ops_.push_back(click("TB_SOLO"));
        ops_.push_back(wait(8));
        ops_.push_back(expect("proof_hud_solo_toggle_clears_solo_mode", [](Context& context) {
            return !context.game->soloMode() && !context.hudGui->state("TB_SOLO").selected;
        }));
        ops_.push_back(expect("hud_solo_fixture_member_leaves", [](Context& context) {
            context.game->removePartyNpc(kNpcCarth);
            return context.game->partyTable().members.empty();
        }));
        ops_.push_back(wait(8));

        // The action bar as retail's 0068b9d0 numbers it: 0 Force powers, 1 medical, 2 other items,
        // 3 mines; Attack sits in the target's left slot. Retail keeps an empty slot's frame and drops
        // only its icon (paired/hud_sword_retail.png). Fails on the old bar, whose first slot was Attack.
        ops_.push_back(expect("proof_hud_power_slot_is_the_first", [](Context& context) {
            const GuiControlState icon = context.hudGui->state("LBL_ACTION0");
            return context.hudGui->state("BTN_ACTION0").visible && icon.fill != "i_attack"
                && (!icon.visible || !context.game->character().powers.empty())
                && !context.hudGui->state("BTN_ACTIONDOWN0").visible;
        }));
        ops_.push_back(expect("hud_empty_slots_keep_their_frames", [](Context& context) {
            return context.hudGui->state("BTN_ACTION1").visible && context.hudGui->state("BTN_ACTION2").visible
                && context.hudGui->state("BTN_ACTION3").visible;
        }));
        auto firstIcon = std::make_shared<std::string>();
        // spells.2da rows 10 Cure (forcefriendly 0, priority 0), 17 Force Armor (forcefriendly 1),
        // 28 Heal (forcefriendly 0, priority 1), 45 Slow (forcehostile 3). Slot 0 (00616230 through
        // 0064a870) keeps one power per forcefriendly value, the highest priority, by value: Heal,
        // then Force Armor. Slow goes to the target's middle slot.
        ops_.push_back(expect("hud_learn_powers", [](Context& context) {
            int learned = 0;
            for (const std::int32_t id : {10, 17, 28, 45}) {
                if (context.game->rules().forcePower(id).has_value()) {
                    context.game->character().powers.insert(id);
                    ++learned;
                }
            }
            return learned == 4;
        }));
        ops_.push_back(wait(24)); // the slots rebuild four times a second
        ops_.push_back(expect("hud_power_slot_appears_with_arrows", [firstIcon](Context& context) {
            const GuiControlState icon = context.hudGui->state("LBL_ACTION0");
            *firstIcon = icon.fill;
            return context.hudGui->state("BTN_ACTION0").visible && icon.visible && !icon.fill.empty()
                && context.hudGui->state("BTN_ACTIONDOWN0").visible && context.hudGui->state("BTN_ACTIONUP0").visible;
        }));
        ops_.push_back(click("BTN_ACTIONDOWN0"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("hud_down_arrow_shows_the_next_power", [firstIcon](Context& context) {
            const std::string now = context.hudGui->state("LBL_ACTION0").fill;
            return !now.empty() && now != *firstIcon;
        }));
        ops_.push_back(click("BTN_ACTIONUP0"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("proof_hud_up_arrow_returns_to_the_first", [firstIcon](Context& context) {
            return context.hudGui->state("LBL_ACTION0").fill == *firstIcon;
        }));
        // Fails on the old slot, which listed every known power in id order (Cure first, Slow in it).
        ops_.push_back(expect("proof_hud_power_slot_keeps_the_best_friendly_power_per_value", [](Context& context) {
            return context.hudGui->state("LBL_ACTION0").fill == "ip_heal";
        }));
        ops_.push_back(click("BTN_ACTIONDOWN0"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("hud_power_slot_second_value", [](Context& context) {
            return context.hudGui->state("LBL_ACTION0").fill == "ip_armor";
        }));
        ops_.push_back(click("BTN_ACTIONDOWN0"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("proof_hud_power_slot_holds_two", [](Context& context) {
            return context.hudGui->state("LBL_ACTION0").fill == "ip_heal";
        }));
        // The frame paired with retail's req_hud_slot_down_hover: the pointer on a two-entry slot's DOWN arrow.
        ops_.push_back(hover("BTN_ACTIONDOWN0"));
        ops_.push_back(hover("BTN_ACTIONDOWN0"));
        ops_.push_back(wait(30));
        ops_.push_back(capture("hud_slot_down_hover", 6));
        ops_.push_back(expect("proof_hud_slot_arrow_lights_the_slot_and_names_itself", [](Context& context) {
            logInfo(concat("front_end: slot arrow tooltip '", context.hudTooltip, "' slot marked ",
                           context.hudGui->state("BTN_ACTION0").marked ? 1 : 0));
            return context.hudGui->state("BTN_ACTION0").marked
                && context.hudTooltip == context.game->localization().tlk().text(48466);
        }));
        ops_.push_back(hover("BTN_ACTION0"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("hud_hover_names_the_action", [](Context& context) {
            const GuiControlState desc = context.hudGui->state("LBL_ACTIONDESC");
            return desc.visible && desc.hasText && !desc.text.empty();
        }));
        ops_.push_back(hover("BTN_EQU"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("proof_hud_description_hides_off_the_slot", [](Context& context) {
            return !context.hudGui->state("LBL_ACTIONDESC").visible;
        }));
        ops_.push_back(capture("hud_actions", 60));
        ops_.push_back(expect("hud_unlearn_powers", [](Context& context) {
            context.game->character().powers.clear();
            return true;
        }));
        // The target's left slot holds Attack and the combat modes (00619c20); its arrows step
        // through them. The fixture gives the character a Power Attack feat so there are two.
        // Q and E choose among the enemies in reach.
        auto targetHome = std::make_shared<kotor::Vec3>();
        auto firstTarget = std::make_shared<std::uint32_t>(UINT32_MAX);
        auto addedFeats = std::make_shared<std::vector<std::int32_t>>();
        auto addedGrenade = std::make_shared<std::uint32_t>(script::kObjectInvalid);
        auto firstMode = std::make_shared<std::string>();
        ops_.push_back(expect("hud_target_fixture", [targetHome, firstTarget, addedFeats, addedGrenade](Context& context) {
            GameSession& game = *context.game;
            game.character().powers.insert(45); // Slow, forcehostile 3: the middle slot (006191f0)
            game.setEnemySightedPauseForTest(true); // 005fa5a0: the first hostile in sight autopauses
            *addedGrenade = game.giveItem("g_w_fraggren01", game.scriptWorld().pc, 1); // itemtype 6: the right slot
            // feat.2da 8 CRITICAL_STRIKE and 31 SNIPER_SHOT: a combat mode for the left slot whichever
            // weapon the PC holds (00619950 offers the melee or the ranged set).
            for (const std::int32_t id : {8, 31}) {
                if (game.character().feats.insert(id).second) {
                    addedFeats->push_back(id);
                }
            }
            *targetHome = game.controlledPosition();
            std::size_t bestCount = 0;
            kotor::Vec3 bestAt = *targetHome;
            for (const kotor::Entity& entity : game.scene().entities) {
                if (entity.kind != kotor::EntityKind::Creature || entity.dead || entity.partyAvatar) {
                    continue;
                }
                game.setPositionForTest(entity.position);
                const std::size_t count = game.hostilesInReach();
                if (count > bestCount) {
                    bestCount = count;
                    bestAt = entity.position;
                }
            }
            game.setPositionForTest(bestAt);
            logInfo(concat("front_end: hud_target_fixture hostiles in reach ", bestCount));
            return bestCount >= 2;
        }));
        ops_.push_back(wait(3));
        // 005fa5a0 (005faa6d..005faaa5): the first hostile that comes into sight becomes the selected
        // object, with no attack ordered, so its nameplate and reticle are up in the Enemy Sighted frame
        // (retail paired/retail/raw/armour_end_from_taris.png). Fails on the old engine, which selected
        // only what the player clicked or attacked.
        ops_.push_back(expect("proof_enemy_sighted_selects_the_hostile", [](Context& context) {
            return context.game->combatTarget() == UINT32_MAX && context.game->hudTarget() != UINT32_MAX
                && context.game->hudTargetHostile();
        }));
        ops_.push_back(expect("hud_target_fixture_attack", [firstTarget](Context& context) {
            context.game->attackNearestHostile();
            *firstTarget = context.game->combatTarget();
            return *firstTarget != UINT32_MAX;
        }));
        ops_.push_back(wait(24));
        // 005fa5a0 -> 005f3f10 reason 1 with the Enemy Sighted option (on by default): the pause panel
        // says 48212 with the pause key named. Fails on the old engine, which never paused for it.
        ops_.push_back(expect("proof_enemy_sighted_autopauses", [](Context& context) {
            return context.hudGui->state("TB_PAUSE").selected;
        }));
        ops_.push_back(capture("hud_enemy_sighted", 6));
        // 0068a310 marks the selected hostile: combatreticle in combat, hostilereticle2 out of it, or
        // hostilearrow at the edge it is past. Fails on the old engine, which drew no reticle.
        ops_.push_back(expect("proof_hud_target_reticle_marks_the_target", [](Context& context) {
            logInfo(concat("front_end: target reticle '", context.hudReticle, "'"));
            return context.hudReticle == "combatreticle" || context.hudReticle == "hostilereticle2"
                || context.hudReticle == "hostilearrow";
        }));
        ops_.push_back(click("TB_PAUSE"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("enemy_sighted_unpaused", [](Context& context) {
            context.game->setEnemySightedPauseForTest(false);
            return !context.hudGui->state("TB_PAUSE").selected;
        }));
        ops_.push_back(expect("hud_target_block_shows_arrows", [](Context& context) {
            return context.hudGui->state("LBL_NAME").visible && !context.hudGui->state("LBL_NAME").text.empty()
                && context.hudGui->state("BTN_TARGETUP0").visible && context.hudGui->state("BTN_TARGETDOWN0").visible
                && context.hudGui->state("BTN_TARGET1").visible && context.hudGui->state("BTN_TARGET2").visible;
        }));
        // GetTargetActions 00619c20: the middle slot the hostile powers (006191f0), the right one the
        // grenades (006198e0). Fails on the old block, whose middle and right slots stayed empty.
        ops_.push_back(expect("proof_hud_target_middle_and_right_slots_filled", [addedGrenade](Context& context) {
            const GuiControlState power = context.hudGui->state("LBL_TARGET1");
            const GuiControlState item = context.hudGui->state("LBL_TARGET2");
            logInfo(concat("front_end: target slots ", power.visible ? 1 : 0, " ", power.fill, " ", item.visible ? 1 : 0,
                           " ", item.fill, " grenade ", *addedGrenade));
            return power.visible && power.fill == "ip_slow" && item.visible && !item.fill.empty()
                && *addedGrenade != script::kObjectInvalid;
        }));
        // 006859e0: a hostile target's bar is ENEMY_BAR and its slot frames lbl_miscroll_h. Fails on
        // the old block, which drew the file's own bar and frames.
        ops_.push_back(expect("proof_hud_target_block_is_hostile_red", [](Context& context) {
            return context.hudGui->state("PB_HEALTH").fill == "enemy_bar"
                && context.hudGui->state("BTN_TARGET0").fill == "lbl_miscroll_h"
                && context.hudGui->state("BTN_TARGET2").fill == "lbl_miscroll_h";
        }));
        // 00686090: the block is centred on the target's projected point and sits above it, kept
        // inside the window. Fails on the old block, which stayed at the top centre.
        ops_.push_back(expect("proof_hud_target_block_floats_on_the_target", [](Context& context) {
            GameSession& game = *context.game;
            const kotor::GuiControl* nameBg = context.hudGui->control("LBL_NAMEBG");
            const GuiView* view = context.hudView;
            kotor::Vec3 at;
            if (nameBg == nullptr || view == nullptr || game.combatTarget() == UINT32_MAX
                || !game.floatingAnchor(game.objectForEntityIndex(game.combatTarget()), at)) {
                return false;
            }
            const SceneCamera camera = game.camera();
            const auto dot = [](const kotor::Vec3& a, const kotor::Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
            const auto unit = [&](kotor::Vec3 v) {
                const float size = std::sqrt(dot(v, v));
                return size > 0.0f ? kotor::Vec3{v.x / size, v.y / size, v.z / size} : v;
            };
            const kotor::Vec3 forward = unit(camera.forward);
            const kotor::Vec3 right = unit({forward.y * camera.up.z - forward.z * camera.up.y,
                                            forward.z * camera.up.x - forward.x * camera.up.z,
                                            forward.x * camera.up.y - forward.y * camera.up.x});
            const kotor::Vec3 d{at.x - camera.eye.x, at.y - camera.eye.y, at.z - camera.eye.z};
            const float depth = dot(d, forward);
            const float focal = (view->windowHeight * 0.5f) / std::tan(camera.fovYDegrees * 0.5f * 3.14159265f / 180.0f);
            const float px = view->windowWidth * 0.5f + dot(d, right) / std::max(depth, camera.nearClip) * focal;
            const GuiPixelRect box = guiControlPixels(*view, *nameBg, context.hudGui->state("LBL_NAMEBG").offsetX,
                                                      context.hudGui->state("LBL_NAMEBG").offsetY);
            const float centre = 0.5f * (box.x0 + box.x1);
            const bool atEdge = box.x0 <= 1.0f || box.x1 >= view->windowWidth - 1.0f;
            logInfo(concat("front_end: target block centre ", centre, " target x ", px, " depth ", depth, " box ", box.x0, ",",
                           box.y0, "-", box.x1, ",", box.y1));
            // Behind the camera the block goes to the side away from the point.
            const bool placed = depth > camera.nearClip ? (std::fabs(centre - px) <= 2.0f * view->scale + 1.0f || atEdge) : atEdge;
            return placed && box.y0 >= 0.0f && box.y1 <= view->windowHeight;
        }));
        ops_.push_back(capture("hud_target", 60));
        // 00619950 lists the combat modes first and 00619b10 adds Attack after them. Fails on the old
        // list, which started on Attack.
        ops_.push_back(expect("proof_hud_target_slot_starts_on_a_combat_mode", [firstMode](Context& context) {
            *firstMode = context.hudGui->state("LBL_TARGET0").fill;
            logInfo(concat("front_end: target slot 0 first icon ", *firstMode));
            return firstMode->rfind("i_", 0) == 0 && *firstMode != "i_attack";
        }));
        // 00686090 holds the block still while the pointer is over it; the pointer goes there first, as a
        // player's does, so the moving target cannot pull the arrow away between the press and the release.
        ops_.push_back(hover("BTN_TARGETDOWN0"));
        ops_.push_back(hover("BTN_TARGETDOWN0"));
        ops_.push_back(click("BTN_TARGETDOWN0"));
        ops_.push_back(wait(6));
        // Fails on the old arrows, which changed the target and left the slot on Attack.
        ops_.push_back(expect("proof_hud_target_down_arrow_shows_the_next_action", [firstTarget, firstMode](Context& context) {
            const std::string fill = context.hudGui->state("LBL_TARGET0").fill;
            return !fill.empty() && fill != *firstMode && context.game->combatTarget() == *firstTarget;
        }));
        // 00686090 holds the block still while the pointer is over it; the pointer goes there first, as a
        // player's does, so the moving target cannot pull the arrow away between the press and the release.
        ops_.push_back(hover("BTN_TARGETUP0"));
        ops_.push_back(hover("BTN_TARGETUP0"));
        ops_.push_back(click("BTN_TARGETUP0"));
        ops_.push_back(wait(6));
        ops_.push_back(expect("hud_target_up_arrow_returns_to_the_first_action", [firstMode](Context& context) {
            return context.hudGui->state("LBL_TARGET0").fill == *firstMode;
        }));
        // keymap.2da rows 205, 204, 240: E the next target, Q the one before, F cancels combat. 005fb050 (the
        // handler of E and Q) only moves the selected object along the angular list; it orders no attack, so the
        // checks read the selection (the target block) and not the attack target.
        // 00687700: 47859 is a transient message, 2.5 s, after which Update 00686ba0 puts 48208 back.
        // Fails on the old line, which always said 48208.
        ops_.push_back(expect("hud_combat_message_fixture", [](Context& context) {
            context.game->raiseCombatMessage(47859);
            return context.game->controlledQueueSize() > 0;
        }));
        ops_.push_back(wait(3));
        ops_.push_back(expect("proof_hud_combat_line_shows_the_raised_message", [](Context& context) {
            const GuiControlState line = context.hudGui->state("LBL_CMBTMODEMSG");
            return line.visible && line.text == context.game->localization().tlk().text(47859);
        }));
        // 00686ba0 fades the transient line through the label's alpha (+0x746c), not its colour.
        // Fails on the old line, which darkened the colour at full alpha.
        ops_.push_back(waitUntil("proof_hud_combat_line_fades_by_alpha", [](Context& context) {
            const GuiControlState line = context.hudGui->state("LBL_CMBTMODEMSG");
            return line.text == context.game->localization().tlk().text(47859) && line.alpha > 0.0f && line.alpha < 0.9f
                && std::max({line.color.x, line.color.y, line.color.z}) > 0.94f;
        }, 600));
        ops_.push_back(waitUntil("hud_combat_line_returns_to_engaged", [](Context& context) {
            return context.hudGui->state("LBL_CMBTMODEMSG").text == context.game->localization().tlk().text(48208);
        }, 600));
        ops_.push_back(key(SDL_SCANCODE_F, SDLK_F));
        ops_.push_back(wait(6));
        ops_.push_back(expect("key_f_cancels_combat", [](Context& context) {
            return context.game->combatTarget() == UINT32_MAX;
        }));
        // E and Q after F: the PC stops walking to its attack target, so the angular list (005fa5a0, turned by the
        // facing) stays put between the two presses.
        ops_.push_back(wait(12));
        // The attacked creature may already be dead by now (how soon depends on the frame rate), so the checks
        // are relative to the selection when E is pressed: E moves it to another creature, Q returns to it
        // (or, if it had gone, to the one before E's pick, which is not E's pick).
        auto selectedBefore = std::make_shared<std::uint32_t>(UINT32_MAX);
        auto selectedByE = std::make_shared<std::uint32_t>(UINT32_MAX);
        ops_.push_back(expect("key_e_fixture_selection", [selectedBefore](Context& context) {
            *selectedBefore = context.game->hudTarget();
            return true;
        }));
        ops_.push_back(key(SDL_SCANCODE_E, SDLK_E));
        ops_.push_back(wait(6));
        ops_.push_back(expect("key_e_picks_the_next_target", [selectedBefore, selectedByE](Context& context) {
            *selectedByE = context.game->hudTarget();
            logInfo(concat("front_end: key e selection ", *selectedBefore, " -> ", *selectedByE));
            return *selectedByE != UINT32_MAX && *selectedByE != *selectedBefore;
        }));
        ops_.push_back(key(SDL_SCANCODE_Q, SDLK_Q));
        ops_.push_back(wait(6));
        ops_.push_back(expect("key_q_picks_the_one_before", [selectedBefore, selectedByE](Context& context) {
            const std::uint32_t now = context.game->hudTarget();
            logInfo(concat("front_end: key q selection ", *selectedByE, " -> ", now));
            return *selectedBefore != UINT32_MAX ? now == *selectedBefore : now != UINT32_MAX && now != *selectedByE;
        }));
        ops_.push_back(key(SDL_SCANCODE_G, SDLK_G));
        ops_.push_back(wait(6));
        auto stealthSeen = std::make_shared<bool>(false);
        ops_.push_back(expect("key_g_is_the_stealth_toggle", [stealthSeen](Context& context) {
            *stealthSeen = context.game->stealthActive();
            logInfo(concat("front_end: stealth after G ", *stealthSeen ? 1 : 0));
            return true;
        }));
        ops_.push_back(expect("key_g_put_back", [stealthSeen](Context& context) {
            if (*stealthSeen) {
                context.game->leaveStealth();
            }
            return true;
        }));
        ops_.push_back(key(SDL_SCANCODE_V, SDLK_V));
        ops_.push_back(wait(6));
        ops_.push_back(expect("key_v_toggles_solo_mode", [](Context& context) {
            const bool solo = context.game->soloMode();
            context.game->setSoloMode(!solo);
            return solo;
        }));
        ops_.push_back(expect("hud_target_fixture_cleared", [targetHome, addedFeats, addedGrenade](Context& context) {
            for (const std::int32_t id : *addedFeats) {
                context.game->character().feats.erase(id);
            }
            context.game->character().powers.erase(45);
            if (*addedGrenade != script::kObjectInvalid) {
                context.game->destroyItem(*addedGrenade);
            }
            context.game->cancelCombat();
            context.game->clearSelection(); // Clear leaves the selection (006887d0); deselect for "no target"
            context.game->setPositionForTest(*targetHome);
            return context.game->combatTarget() == UINT32_MAX;
        }));
        ops_.push_back(wait(24));
        ops_.push_back(expect("proof_hud_target_arrows_hidden_without_a_target", [](Context& context) {
            return !context.hudGui->state("BTN_TARGETUP0").visible && !context.hudGui->state("BTN_TARGETDOWN0").visible
                && !context.hudGui->state("LBL_NAME").visible;
        }));
        // The level-up badge marks the portrait while points wait, and only then.
        auto savedPoints = std::make_shared<std::int32_t>(0);
        const auto badgeShown = [](Context& context) {
            for (const char* tag : {"LBL_LEVELUP1", "LBL_LEVELUP2", "LBL_LEVELUP3"}) {
                if (context.hudGui->state(tag).visible) {
                    return true;
                }
            }
            return false;
        };
        ops_.push_back(expect("proof_hud_no_level_up_badge_without_points", [badgeShown, savedPoints](Context& context) {
            *savedPoints = context.game->character().skillPoints;
            context.game->character().skillPoints = 0;
            context.game->character().attributePoints = 0;
            context.game->character().featPoints = 0;
            return !context.game->canLevelUp();
        }));
        ops_.push_back(wait(24));
        ops_.push_back(expect("hud_level_up_badge_hidden_without_points", [badgeShown](Context& context) {
            return !badgeShown(context);
        }));
        ops_.push_back(expect("hud_give_skill_points", [](Context& context) {
            context.game->character().skillPoints = 3;
            return context.game->canLevelUp();
        }));
        ops_.push_back(wait(24));
        ops_.push_back(expect("hud_level_up_badge_shows_with_points", [badgeShown](Context& context) {
            return badgeShown(context);
        }));
        ops_.push_back(capture("hud_level_up", 40));
        ops_.push_back(expect("hud_take_skill_points", [savedPoints](Context& context) {
            context.game->character().skillPoints = *savedPoints;
            return true;
        }));
        ops_.push_back(wait(24));

        // Option 0x10, retail 006027c0: experience floats as "XP n" for 3 s, and not at all with the option off.
        // Fails on the old engine, which had no floating text.
        ops_.push_back(expect("floating_xp_line_on", [](Context& context) {
            context.game->giveXp(1);
            const std::vector<FloatingText>& lines = context.game->floatingTexts();
            kotor::Vec3 anchor;
            const bool anchored = !lines.empty() && context.game->floatingAnchor(lines.back().object, anchor);
            const SceneCamera camera = context.game->camera();
            logInfo(concat("front_end: floating anchored ", anchored ? 1 : 0, " at ", anchor.x, ",", anchor.y, ",", anchor.z,
                           " eye ", camera.eye.x, ",", camera.eye.y, ",", camera.eye.z,
                           " forward ", camera.forward.x, ",", camera.forward.y, ",", camera.forward.z));
            const bool ok = !lines.empty() && lines.back().seconds == 3.0f
                && lines.back().text == context.game->localization().tlk().text(38551) + " 1";
            // The gate's camera sits on the PC, so its own line is out of view. For the frame paired with
            // retail's kill burst (XP 300 over a trooper), the same line goes over the nearest creature in view.
            std::uint32_t inView = 0xFFFFFFFFu;
            float nearest = 1.0e9f;
            const std::vector<kotor::Entity>& entities = context.game->scene().entities;
            // The nearest creature inside the view (or, with none in view, the nearest door or placeable),
            // within half the vertical field of view of the camera's axis on both sides.
            const float slope = std::tan(camera.fovYDegrees * 0.5f * 3.14159265f / 180.0f);
            bool creatureFound = false;
            for (std::size_t i = 0; i < entities.size(); ++i) {
                if (entities[i].partyAvatar || (entities[i].kind != kotor::EntityKind::Creature
                                                 && entities[i].kind != kotor::EntityKind::Door
                                                 && entities[i].kind != kotor::EntityKind::Placeable)) {
                    continue;
                }
                const bool creature = entities[i].kind == kotor::EntityKind::Creature;
                if (creatureFound && !creature) {
                    continue;
                }
                const kotor::Vec3 d{entities[i].position.x - camera.eye.x, entities[i].position.y - camera.eye.y,
                                    entities[i].position.z - camera.eye.z};
                const float depth = d.x * camera.forward.x + d.y * camera.forward.y + d.z * camera.forward.z;
                const float off2 = d.x * d.x + d.y * d.y + d.z * d.z - depth * depth;
                if (depth <= 2.0f || off2 > depth * slope * depth * slope) {
                    continue;
                }
                if (creature && !creatureFound) {
                    creatureFound = true;
                    nearest = 1.0e9f;
                }
                if (depth < nearest) {
                    nearest = depth;
                    inView = context.game->objectForEntityIndex(i);
                }
            }
            if (inView != 0xFFFFFFFFu) {
                context.game->floatingText(inView, 3, 300);
            }
            return ok;
        }));
        ops_.push_back(capture("hud_floating_xp", 6));
        ops_.push_back(expect("floating_option_off_fixture", [](Context& context) {
            context.settings->set("feedback.floatingnumbers", 0.0f);
            return true;
        }));
        ops_.push_back(wait(3));
        ops_.push_back(expect("floating_option_off_says_nothing", [](Context& context) {
            const std::size_t before = context.game->floatingTexts().size();
            context.game->giveXp(1);
            const bool same = context.game->floatingTexts().size() == before;
            context.settings->set("feedback.floatingnumbers", 1.0f);
            return same;
        }));
        // BarkString(OBJECT_INVALID, 37107) as k_pebo_skybox calls it: the state ACT_671 leaves
        // (ScriptWorldState barkString), then the bark bubble across the top of the screen. Paired
        // with retail's turret burst raw/burst/tur_003091_0084865 ("Incoming fighters!").
        ops_.push_back(expect("bark_banner_fixture", [](Context& context) {
            PresentationState& shown = ScriptApi{*context.game}.sim().presentation;
            shown.lastBarkSpeaker = script::kObjectInvalid;
            shown.lastBarkStrref = 37107;
            ++shown.barkSerial;
            return true;
        }));
        // The bubble lives on the frame clock: CSWGuiBarkBubble::Draw 006a9ce0 takes the draw delta off its time
        // left every frame (006a9ecb) and drops the box once the sound that played has ended (006a9efa), here the
        // line's 1.15 s of voice. Whether a fixed number of frames fits inside that depends on how long the frames
        // take, so the proof reads the line on the first frame the box is up and the capture comes after.
        ops_.push_back(waitUntil("bark_banner_up", [](Context& context) { return !context.hudBark.empty(); }, 600));
        ops_.push_back(expect("proof_bark_banner_shows_the_line", [](Context& context) {
            logInfo(concat("front_end: bark banner \"", context.hudBark, "\""));
            return !context.hudBark.empty() && context.hudBark == context.game->localization().tlk().text(37107);
        }));
        ops_.push_back(capture("hud_bark_banner", 6));
        // Option 0x400, retail 0040b490 and 00418a90: the slot's name and its key from keymap.2da.
        ops_.push_back(hover("BTN_ACTION0"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("tooltip_names_the_slot_and_its_key", [](Context& context) {
            logInfo(concat("front_end: tooltip '", context.hudTooltip, "'"));
            return context.hudTooltip == context.game->localization().tlk().text(48486) + " : 4";
        }));
        ops_.push_back(capture("hud_tooltip", 6));
        ops_.push_back(expect("tooltip_option_off_fixture", [](Context& context) {
            context.settings->set("feedback.tooltips", 0.0f);
            return true;
        }));
        ops_.push_back(wait(3));
        ops_.push_back(expect("tooltip_option_off_shows_none", [](Context& context) {
            const bool none = context.hudTooltip.empty();
            context.settings->set("feedback.tooltips", 1.0f);
            return none;
        }));
        // Option 0x20, retail 0062ef90 and 00625c60: what was gathered pops up a quarter second after the world is free,
        // only its own rows, and leaves the icons up four seconds. Off, only the icons.
        ops_.push_back({"status_summary_fixture", [this](Context& context) {
                            statusSummaryLive_ = true;
                            context.game->statusFeedback(1, 10);
                            context.game->statusFeedback(2, 25);
                            return true;
                        }});
        ops_.push_back(wait(2));
        ops_.push_back(waitUntil("status_summary_opens", [](Context& context) {
            return context.game->statusSummaryOpen();
        }, 2000));
        ops_.push_back(hudOn(false));
        ops_.push_back(screenIs("statussummary_fit"));
        ops_.push_back(wait(3));
        ops_.push_back(painted("statussummary_fit"));
        ops_.push_back(capture("status_summary", 10));
        ops_.push_back(expect("status_summary_shows_only_its_rows", [](Context& context) {
            const GuiSystem& gui = *context.gui;
            const std::string credits = gui.state("LBL_CREDITS_DESC").text;
            const std::string xp = gui.state("LBL_XP_DESC").text;
            logInfo(concat("front_end: summary '", credits, "' '", xp, "'"));
            return gui.state("LBL_CREDITS").visible && gui.state("LBL_XP").visible && !gui.state("LBL_JOURNAL").visible
                && !gui.state("LBL_LOST").visible && credits.find("10") != std::string::npos
                && xp.find("25") != std::string::npos;
        }));
        ops_.push_back(click("BTN_OK"));
        ops_.push_back(wait(4));
        ops_.push_back(hudOn(true));
        ops_.push_back({"status_summary_answered", [this](Context& context) {
                            statusSummaryLive_ = false;
                            check(!context.game->statusSummaryOpen() && !context.gui->active(), "status_summary_answered");
                            return true;
                        }});
        ops_.push_back(wait(4));
        ops_.push_back(expect("status_summary_leaves_the_icons", [](Context& context) {
            return context.game->feedbackIconSeconds(2) > 0.0f && context.hudGui->state("LBL_PLOTXP").visible
                && context.hudGui->state("LBL_CASH").visible && !context.hudGui->state("LBL_JOURNAL").visible;
        }));
        ops_.push_back(capture("hud_feedback_icons", 4));
        ops_.push_back(hudOn(false));

        // The map: the minimap button and the menu bar's Map open the shipped map panel; its
        // Party Selection button is refused with retail's text while the area is Unescapable
        // (00693650 builds the refusal, 0062dd20 stops the UI path) and opens the party screen
        // when it is not.
        ops_.push_back(hudOn(true));
        // Retail 004b4e80, 00579210, 00578fa0: walking uncovers the cells round the leader and the rest stay covered
        // until RevealMap. Fails on the old engine, which had no explored cells.
        ops_.push_back(expect("map_fog_uncovers_only_near_the_leader", [](Context& context) {
            const MapFog* fog = context.game->mapFog();
            if (fog == nullptr) {
                logInfo("front_end: this area has no map to fog");
                return true;
            }
            logInfo(concat("front_end: map fog ", fog->cols, "x", fog->rows, " seen ", fog->seenCount(), " of ", fog->seen.size()));
            return context.game->mapPointSeen(context.game->position()) && fog->seenCount() > 0 && fog->seenCount() < fog->seen.size();
        }));
        ops_.push_back(capture("hud_minimap_fog", 4));
        // The map screen fogs as the minimap does (006943d0); the frame compares with retail.
        ops_.push_back(click("BTN_MINIMAP"));
        ops_.push_back(wait(4));
        ops_.push_back(hudOn(false));
        ops_.push_back(screenIs("map"));
        ops_.push_back(wait(3));
        ops_.push_back(capture("map_fogged", 20));
        ops_.push_back(click("BTN_EXIT"));
        ops_.push_back(wait(4));
        ops_.push_back(hudOn(true));
        ops_.push_back(expect("map_reveal_uncovers_all", [](Context& context) {
            context.game->markMapAt(context.game->position(), -1);
            const MapFog* fog = context.game->mapFog();
            return fog == nullptr || fog->seenCount() == fog->seen.size();
        }));
        ops_.push_back(click("BTN_MINIMAP"));
        ops_.push_back(wait(4));
        ops_.push_back(hudOn(false));
        ops_.push_back(screenIs("map"));
        ops_.push_back(wait(3));
        ops_.push_back(painted("map"));
        layout("map");
        ops_.push_back(capture("map", 40));
        widgets("map");
        ops_.push_back(expect("map_names_the_area", [](Context& context) {
            const GuiControlState area = context.gui->state("LBL_Area");
            return area.hasText && !area.text.empty();
        }));
        // The Return button: hidden until a script shows it (SetReturnStrref, action 152); its click asks the
        // script's question in a popup (retail 00694d50 -> 00693bc0 -> 00692bc0), Cancel leaves the map as it was.
        // OK would run k_sup_gohawk (00692b00), which is not driven here: it moves the party.
        // Paired frame ui_map: without a script the button stays up, dim; a click gets the reason (00692bc0). Fails on the old hidden one.
        ops_.push_back(expect("map_return_dim_until_a_script_shows_it", [](Context& context) {
            const GuiControlState button = context.gui->state("BTN_RETURN");
            return button.visible && button.hasColor && !context.game->presentation().returnOption
                && button.text == context.game->localization().tlk().text(38550);
        }));
        // Retail 00692c70, 006929b0, 006927b0: pins are enabled noted waypoints, BTN_DOWN walks them and the
        // note line follows. Two made here so the walk has somewhere to go; removed after.
        ops_.push_back(expect("map_pin_notes_walk", [](Context& context) {
            kotor::Scene& scene = context.game->scene();
            for (int i = 0; i < 2; ++i) {
                kotor::Entity pin;
                pin.kind = kotor::EntityKind::Waypoint;
                pin.tag = "gate_pin_" + std::to_string(i);
                pin.position = context.game->position();
                pin.hasMapNote = true;
                pin.mapNoteEnabled = true;
                pin.mapNote = "gate note " + std::to_string(i);
                scene.entities.push_back(std::move(pin));
            }
            return context.game->mapPins().size() >= 2;
        }));
        ops_.push_back(wait(3));
        auto firstNote = std::make_shared<std::string>();
        ops_.push_back(expect("map_pin_note_shown", [firstNote](Context& context) {
            *firstNote = context.gui->state("LBL_MapNote").text;
            return firstNote->find(context.game->localization().tlk().text(349) + ": ") == 0;
        }));
        ops_.push_back(click("BTN_DOWN"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("map_pin_down_changes_note", [firstNote](Context& context) {
            const std::string now = context.gui->state("LBL_MapNote").text;
            return !now.empty() && now != *firstNote;
        }));
        ops_.push_back(expect("map_pins_removed", [](Context& context) {
            std::vector<kotor::Entity>& entities = context.game->scene().entities;
            entities.resize(entities.size() - 2);
            return true;
        }));
        ops_.push_back(wait(3));
        ops_.push_back(expect("map_return_shown_by_action_152", [](Context& context) {
            const std::uint32_t pc = context.game->scriptWorld().pc;
            const auto result = context.game->actionAsForTest(
                pc, 152, {{script::Value::makeInt(1)}, {script::Value::makeInt(32179)}, {script::Value::makeInt(32322)}});
            return result.has_value() && context.game->presentation().returnOption;
        }));
        ops_.push_back(wait(4));
        ops_.push_back(expect("map_return_button_text", [](Context& context) {
            const GuiControlState button = context.gui->state("BTN_RETURN");
            return button.visible && button.text == context.game->localization().tlk().text(32179);
        }));
        ops_.push_back(capture("map_return", 40));
        ops_.push_back(click("BTN_RETURN"));
        ops_.push_back(waitUntil("map_return_asks_the_scripts_question", [](Context& context) {
            const auto items = context.gui->state("LB_MESSAGE").items;
            return context.gui->modalActive() && !items.empty()
                && items[0].text == context.game->localization().tlk().text(32322);
        }, 60));
        ops_.push_back(wait(4));
        layout("pazaak_ask");
        ops_.push_back(capture("map_return_question", 40));
        ops_.push_back(click("BTN_CANCEL"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("map_return_cancel_keeps_the_map", [](Context& context) {
            return !context.gui->modalActive() && context.gui->screenName() == "map"
                && context.game->activePanel() == UiPanel::Map;
        }));
        ops_.push_back(expect("map_return_hidden_again", [](Context& context) {
            const std::uint32_t pc = context.game->scriptWorld().pc;
            const auto result = context.game->actionAsForTest(
                pc, 152, {{script::Value::makeInt(0)}, {script::Value::makeInt(0)}, {script::Value::makeInt(0)}});
            return result.has_value() && !context.game->presentation().returnOption;
        }));
        ops_.push_back(expect("map_unescapable_on", [](Context& context) {
            context.game->setAreaUnescapableForTest(true);
            return true;
        }));
        ops_.push_back(click("BTN_PRTYSLCT"));
        ops_.push_back(wait(4));
        // Retail 00692bc0: a message box with the reason. Fails on the old line on the map's note.
        ops_.push_back(expect("map_party_selection_refused_when_unescapable", [](Context& context) {
            const std::vector<GuiListItem> message = context.gui->state("LB_MESSAGE").items;
            return !context.game->partySelection().active && context.game->activePanel() == UiPanel::Map
                && context.gui->modalActive() && !message.empty()
                && message.front().text == context.game->localization().tlk().text(38451)
                && !context.gui->state("BTN_CANCEL").visible;
        }));
        ops_.push_back(capture("map_unescapable", 40));
        ops_.push_back(click("BTN_OK"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("map_unescapable_off", [](Context& context) {
            context.game->setAreaUnescapableForTest(false);
            return true;
        }));
        ops_.push_back(click("BTN_PRTYSLCT"));
        ops_.push_back(wait(5));
        ops_.push_back(expect("proof_map_party_selection_opens_when_escapable", [](Context& context) {
            return context.game->partySelection().active;
        }));
        ops_.push_back(expect("map_party_selection_cancel", [](Context& context) {
            context.game->cancelPartySelection();
            context.game->setUiPanel(UiPanel::None);
            return true;
        }));
        ops_.push_back(wait(4));
        ops_.push_back(expect("map_closed_after_party_selection", [](Context& context) {
            return !context.gui->active() && context.game->activePanel() == UiPanel::None;
        }));
        // The menu bar's Map opens the same panel.
        ops_.push_back(hudOn(true));
        ops_.push_back(click("BTN_MAP"));
        ops_.push_back(wait(4));
        ops_.push_back(hudOn(false));
        ops_.push_back(screenIs("map"));
        ops_.push_back(click("BTN_EXIT"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("map_closes", [](Context& context) {
            return !context.gui->active() && context.game->activePanel() == UiPanel::None;
        }));

        // The map's button is the same UI path, not a roster list: refused while the area is Unescapable
        // (the reason lands in the feedback log, there is no map panel to show it), otherwise the
        // shipped party selection screen.
        ops_.push_back(expect("l_key_unescapable_on", [](Context& context) {
            context.game->setAreaUnescapableForTest(true);
            return true;
        }));
        ops_.push_back(expect("party_panel_asked_for", [](Context& context) {
            context.game->setUiPanel(UiPanel::Party);
            return true;
        }));
        ops_.push_back(wait(5));
        ops_.push_back(expect("l_key_party_selection_refused_when_unescapable", [](Context& context) {
            const auto& log = context.game->feedbackLog();
            return !context.game->partySelection().active && !context.gui->active()
                && context.game->activePanel() == UiPanel::None && !log.empty()
                && log.back() == context.game->localization().tlk().text(38451);
        }));
        ops_.push_back(expect("l_key_unescapable_off", [](Context& context) {
            context.game->setAreaUnescapableForTest(false);
            return true;
        }));
        ops_.push_back(expect("party_panel_asked_for", [](Context& context) {
            context.game->setUiPanel(UiPanel::Party);
            return true;
        }));
        ops_.push_back(wait(5));
        ops_.push_back(screenIs("partyselection"));
        ops_.push_back(wait(3));
        ops_.push_back(painted("partyselection_l_key"));
        ops_.push_back(expect("proof_l_key_opens_party_selection_when_escapable", [](Context& context) {
            return context.game->partySelection().active && context.game->activePanel() == UiPanel::None;
        }));
        ops_.push_back(expect("l_key_party_selection_cancel", [](Context& context) {
            context.game->cancelPartySelection();
            return true;
        }));
        ops_.push_back(wait(4));
    }
    ops_.push_back(expect("inventory_fixture", [](Context& context) {
        const std::uint32_t pc = context.game->scriptWorld().pc;
        std::size_t given = 0;
        for (const char* resref : {"g_w_blstrpstl001", "g_w_lghtsbr01", "g_a_class4001", "g_i_adrnaline001", "g_i_belt001"}) {
            given += context.game->giveItem(resref, pc, 1) != script::kObjectInvalid ? 1 : 0;
        }
        return given >= 3;
    }));
    ops_.push_back(key(SDL_SCANCODE_I, SDLK_I));
    ops_.push_back(screenIs("inventory"));
    ops_.push_back(wait(3));
    ops_.push_back(painted("inventory"));
    layout("inventory");
    ops_.push_back(expect("inventory_listed", [](Context& context) {
        const GuiControlState list = context.gui->state("LB_ITEMS");
        std::size_t icons = 0;
        for (const GuiListItem& item : list.items) {
            icons += item.icon.empty() ? 0 : 1;
        }
        return list.items.size() >= 3 && icons >= 3;
    }));
    ops_.push_back(expect("inventory_describes_first", [](Context& context) {
        const GuiControlState description = context.gui->state("LB_DESCRIPTION");
        return !description.items.empty() && !description.items[0].text.empty();
    }));
    ops_.push_back(capture("inventory", 60));
    widgets("inventory");
    auto shownItem = std::make_shared<std::string>();
    auto rowCount = std::make_shared<std::size_t>(0);
    auto pickRow = std::make_shared<int>(1);
    ops_.push_back(expect("inventory_before", [shownItem, rowCount, pickRow](Context& context) {
        const GuiControlState shown = context.gui->state("LB_DESCRIPTION");
        *shownItem = shown.items.empty() ? std::string{} : shown.items[0].text;
        const GuiControlState list = context.gui->state("LB_ITEMS");
        *rowCount = list.items.size();
        // The belt: an item any sheet can wear; never the row already selected.
        for (std::size_t i = 1; i < list.items.size(); ++i) {
            const std::size_t index = static_cast<std::size_t>(list.items[i].value);
            if (index < context.game->inventory().size() && context.game->inventory()[index].resref == "g_i_belt001") {
                *pickRow = static_cast<int>(i);
            }
        }
        return true;
    }));
    ops_.push_back({"click the weapon row", [this, pickRow](Context& context) {
                        return clickRow("LB_ITEMS", *pickRow).run(context);
                    }});
    ops_.push_back(wait(3));
    ops_.push_back(expect("inventory_row_changes_description", [shownItem, pickRow](Context& context) {
        return context.gui->listSelection("LB_ITEMS") == static_cast<std::size_t>(*pickRow)
            && !context.gui->state("LB_DESCRIPTION").items.empty()
            && context.gui->state("LB_DESCRIPTION").items[0].text != *shownItem;
    }));
    ops_.push_back({"click the weapon row again", [this, pickRow](Context& context) {
                        return clickRow("LB_ITEMS", *pickRow).run(context); // equips it
                    }});
    ops_.push_back(wait(3));
    // Paired frame ui_inventory: the worn belt stays listed, first and marked. Fails on the old list,
    // which dropped worn rows, on the old title and on the old button.
    ops_.push_back(expect("inventory_equip_marks_item", [rowCount](Context& context) {
        const std::string mark = "(" + context.game->localization().tlk().text(32346) + ")";
        const GuiControlState list = context.gui->state("LB_ITEMS");
        bool sawBelt = false;
        bool sawLoose = false;
        for (const GuiListItem& row : list.items) {
            const std::size_t index = static_cast<std::size_t>(row.value);
            const bool marked = row.text.find(mark) != std::string::npos;
            if (index >= context.game->inventory().size() || marked != context.game->inventory()[index].equipped) {
                return false;
            }
            sawBelt = sawBelt || (marked && context.game->inventory()[index].resref == "g_i_belt001");
            if (marked && sawLoose) {
                return false; // worn rows come first
            }
            sawLoose = sawLoose || !marked;
        }
        return sawBelt && list.items.size() == *rowCount;
    }));
    ops_.push_back(expect("inventory_title_names_list", [](Context& context) {
        const auto& tlk = context.game->localization().tlk();
        return context.gui->state("LBL_INV").text == tlk.text(32171) + " - " + tlk.text(41822)
            && context.gui->state("BTN_QUESTITEMS").text == tlk.text(42359) + " " + tlk.text(42165);
    }));
    // Retail 006b2c90 case 1, 0055d330: the New list holds what just arrived. Fails on the old engine's empty list.
    auto newArrival = std::make_shared<std::uint32_t>(0);
    ops_.push_back(expect("inventory_new_item_fixture", [newArrival](Context& context) {
        context.game->clearNewItems();
        *newArrival = context.game->giveItem("g_i_medeqpmnt01", context.game->scriptWorld().pc, 1, script::kObjectInvalid);
        return context.game->isNewItem(*newArrival);
    }));
    ops_.push_back(click("BTN_QUESTITEMS"));
    ops_.push_back(wait(3));
    ops_.push_back(expect("inventory_new_list_holds_the_arrival", [newArrival](Context& context) {
        const GuiControlState list = context.gui->state("LB_ITEMS");
        logInfo(concat("front_end: new list rows ", list.items.size(), " first ", list.items.empty() ? std::string{} : list.items.front().text,
                       " value ", list.items.empty() ? -1 : list.items.front().value, " arrival ", *newArrival));
        return list.items.size() == 1 && list.items.front().value >= 0
            && context.game->inventory()[static_cast<std::size_t>(list.items.front().value)].object == *newArrival;
    }));
    ops_.push_back(click("BTN_QUESTITEMS"));
    ops_.push_back(wait(3));
    ops_.push_back(click("BTN_QUESTITEMS"));
    ops_.push_back(wait(3));
    ops_.push_back(expect("inventory_equippable_list", [](Context& context) {
        const auto& tlk = context.game->localization().tlk();
        const GuiControlState list = context.gui->state("LB_ITEMS");
        for (const GuiListItem& row : list.items) {
            if (context.game->rules().equipMask(context.game->inventory()[static_cast<std::size_t>(row.value)].baseItem) == 0) {
                return false;
            }
        }
        return !list.items.empty() && context.gui->state("LBL_INV").text == tlk.text(32171) + " - " + tlk.text(41821)
            && context.gui->state("BTN_QUESTITEMS").text == tlk.text(42359) + " " + tlk.text(41819);
    }));
    ops_.push_back(click("BTN_QUESTITEMS"));
    ops_.push_back(wait(3));
    ops_.push_back(click("BTN_QUESTITEMS"));
    ops_.push_back(wait(3));
    ops_.push_back(click("BTN_QUESTITEMS"));
    ops_.push_back(wait(3));
    ops_.push_back(expect("inventory_cycle_wraps", [](Context& context) {
        const auto& tlk = context.game->localization().tlk();
        return context.gui->state("LBL_INV").text == tlk.text(32171) + " - " + tlk.text(41822);
    }));
    // A second arrival whose row is never shown (the selection stays on the first, worn row).
    auto unseenArrival = std::make_shared<std::uint32_t>(0);
    ops_.push_back(expect("inventory_unseen_item_fixture", [unseenArrival](Context& context) {
        context.gui->setListSelection("LB_ITEMS", 0); // the first row is worn, never new
        // An item the inventory does not hold yet, so it cannot stack onto a row already shown.
        for (const char* resref : {"g_i_progspike01", "g_i_secspike01", "g_w_vbroswrd01", "g_i_cmbtshot01"}) {
            const bool held = std::any_of(context.game->inventory().begin(), context.game->inventory().end(),
                                          [&](const InventoryItem& item) { return item.resref == resref; });
            if (!held) {
                *unseenArrival = context.game->giveItem(resref, context.game->scriptWorld().pc, 1, script::kObjectInvalid);
                break;
            }
        }
        return *unseenArrival != 0 && context.game->isNewItem(*unseenArrival);
    }));
    ops_.push_back(wait(3));
    ops_.push_back(click("BTN_EXIT"));
    ops_.push_back(wait(3));
    ops_.push_back(expect("inventory_closes", [](Context& context) {
        return !context.gui->active() && context.game->activePanel() == UiPanel::None;
    }));
    // 006b4430 -> 00556050 clears NewItem only on the items whose description showed (006b3d10 adds
    // them to +0x1dcc). Fails on the old close, which cleared every item.
    ops_.push_back(expect("proof_inventory_close_clears_only_seen_new_items", [newArrival, unseenArrival](Context& context) {
        const bool seenCleared = !context.game->isNewItem(*newArrival);
        const bool unseenKept = context.game->isNewItem(*unseenArrival);
        logInfo(concat("front_end: new items after close seen_cleared=", seenCleared ? 1 : 0, " unseen_kept=", unseenKept ? 1 : 0));
        return seenCleared && unseenKept && *unseenArrival != *newArrival;
    }));

    // ---- equipment, abilities and messages, opened by the top bar's real pointer events and driven
    // by clicks; each screen is the shipped GUI (equip, abilities, messages), not a debug panel.
    const auto topBar = [this](const char* button) {
        ops_.push_back(expect(std::string("topbar_aims_") + button, [this](Context& context) {
            pointToHud_ = true;
            return context.hudGui != nullptr && context.hudGui->screen() != nullptr;
        }));
        ops_.push_back(wait(2));
        ops_.push_back(click(button));
        ops_.push_back(wait(4));
        ops_.push_back(expect(std::string("topbar_done_") + button, [this](Context&) {
            pointToHud_ = false;
            return true;
        }));
    };
    {
        topBar("BTN_EQU");
        ops_.push_back(screenIs("equip"));
        ops_.push_back(wait(3));
        ops_.push_back(painted("equip"));
        layout("equip");
        ops_.push_back(capture("equip", 40));
        widgets("equip");
        // Retail 00627980: top.gui's tabs stay over the pages, the page on show lit.
        const auto tabsLit = [](const char* lit) {
            return [lit](Context& context) {
                const char* const labels[8] = {"LBLH_EQU", "LBLH_INV", "LBLH_CHA", "LBLH_ABI", "LBLH_MSG", "LBLH_JOU", "LBLH_MAP", "LBLH_OPT"};
                for (const char* label : labels) {
                    const GuiControlState state = context.gui->state(label);
                    const bool on = state.hasFill && !state.fill.empty() && state.fill.back() == '2';
                    if (!state.visible || on != (std::string(label) == lit)) {
                        return false;
                    }
                }
                return true;
            };
        };
        ops_.push_back(hover("LB_ITEMS")); // the top bar button it was opened with lies under a tab
        ops_.push_back(wait(2));
        ops_.push_back(expect("tabs_equip_lit", tabsLit("LBLH_EQU")));
        ops_.push_back(click("BTN_INV"));
        ops_.push_back(screenIs("inventory"));
        ops_.push_back(wait(2));
        ops_.push_back(expect("tabs_click_goes_to_inventory", tabsLit("LBLH_INV")));
        ops_.push_back(capture("tabs_inventory", 40));
        // Retail 00624c00/30: the tab under the pointer lights too. Fails while hovered state goes unread.
        ops_.push_back(hover("BTN_MAP"));
        ops_.push_back(wait(2));
        ops_.push_back(expect("tabs_hover_lights_the_tab", [](Context& context) {
            const std::string map = context.gui->state("LBLH_MAP").fill;
            return !map.empty() && map.back() == '2';
        }));
        ops_.push_back(hover("LBL_INV")); // off the tabs and off where the equip screen has slots
        ops_.push_back(wait(2));
        // keymap.2da rows 209..216 (retail 00621210): M Map, L Quests, J Messages, K Abilities, P Character.
        ops_.push_back(key(SDL_SCANCODE_M, SDLK_M));
        ops_.push_back(screenIs("map"));
        ops_.push_back(wait(2));
        ops_.push_back(expect("tabs_key_m_is_the_map", tabsLit("LBLH_MAP")));
        ops_.push_back(key(SDL_SCANCODE_L, SDLK_L));
        ops_.push_back(screenIs("journal"));
        ops_.push_back(key(SDL_SCANCODE_J, SDLK_J));
        ops_.push_back(screenIs("messages"));
        ops_.push_back(key(SDL_SCANCODE_K, SDLK_K));
        ops_.push_back(screenIs("abilities"));
        ops_.push_back(key(SDL_SCANCODE_P, SDLK_P));
        ops_.push_back(screenIs("character"));
        ops_.push_back(key(SDL_SCANCODE_U, SDLK_U));
        ops_.push_back(screenIs("equip"));
        ops_.push_back(wait(2));
        // Paired frame ui_equip: the Body slot, lit, and its list: None, then what is worn and carried.
        ops_.push_back(expect("equip_opens_on_body_with_its_list", [](Context& context) {
            const GuiControlState list = context.gui->state("LB_ITEMS");
            return context.gui->state("BTN_INV_BODY").marked && list.visible && !list.items.empty()
                && list.items.front().text == context.game->localization().tlk().text(363)
                && context.gui->state("LBL_SLOTNAME").text == context.game->localization().tlk().text(31380);
        }));
        // Option 1, retail 006b9470: off (retail's default), what this member cannot wear is listed red; on, it is
        // gone. Taking the feats away makes the fixture's armour unwearable. Fails on the old list, never red or filtered.
        auto keptFeats = std::make_shared<std::set<std::int32_t>>();
        auto rowsAll = std::make_shared<std::size_t>(0);
        ops_.push_back(expect("equip_unwearable_fixture", [keptFeats](Context& context) {
            *keptFeats = context.game->character().feats;
            context.game->character().feats.clear();
            return true;
        }));
        ops_.push_back(wait(3));
        ops_.push_back(expect("equip_unwearable_listed_red", [rowsAll](Context& context) {
            const std::vector<GuiListItem> items = context.gui->state("LB_ITEMS").items;
            std::size_t red = 0;
            for (const GuiListItem& row : items) {
                red += row.hasColor ? 1u : 0u;
            }
            *rowsAll = items.size();
            logInfo(concat("front_end: equip rows ", items.size(), " red ", red));
            context.settings->set("feedback.hideunequippable", 1.0f);
            return red > 0;
        }));
        ops_.push_back(wait(3));
        ops_.push_back(expect("equip_option_hides_the_unwearable", [rowsAll, keptFeats](Context& context) {
            const std::vector<GuiListItem> items = context.gui->state("LB_ITEMS").items;
            bool anyRed = false;
            for (const GuiListItem& row : items) {
                anyRed = anyRed || row.hasColor;
            }
            context.settings->set("feedback.hideunequippable", 0.0f);
            context.game->character().feats = *keptFeats;
            return !anyRed && items.size() < *rowsAll;
        }));
        ops_.push_back(wait(3));
        ops_.push_back(hover("BTN_INV_HEAD"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("equip_hover_moves_the_slot", [](Context& context) {
            return context.gui->state("BTN_INV_HEAD").marked && !context.gui->state("BTN_INV_BODY").marked
                && context.gui->state("LBL_SLOTNAME").text == context.game->localization().tlk().text(31375);
        }));
        ops_.push_back(expect("equip_list_stays_up_on_the_paper_doll", [](Context& context) {
            return context.gui->state("LB_ITEMS").visible;
        }));
        // The shipped equip.gui lays LB_DESC (62,112 253x180) over the slot buttons (157..321 x
        // 132..296): the two cannot be on screen together, and the screen shows one then the other.
        const auto overlapping = [](Context& context, const char* a, const char* b) {
            const kotor::GuiControl* ca = context.gui->control(a);
            const kotor::GuiControl* cb = context.gui->control(b);
            if (ca == nullptr || cb == nullptr) {
                return false;
            }
            return ca->extent.left < cb->extent.left + cb->extent.width && cb->extent.left < ca->extent.left + ca->extent.width
                && ca->extent.top < cb->extent.top + cb->extent.height && cb->extent.top < ca->extent.top + ca->extent.height;
        };
        ops_.push_back(expect("proof_layout_overlap_is_detected", [overlapping](Context& context) {
            return overlapping(context, "LB_DESC", "BTN_INV_WEAP_R"); // the file's own overlap
        }));
        const auto separate = [overlapping](Context& context) {
            for (const char* slot : {"BTN_INV_HEAD", "BTN_INV_BODY", "BTN_INV_WEAP_R", "BTN_INV_BELT"}) {
                if (overlapping(context, "LB_DESC", slot) && context.gui->state("LB_DESC").visible
                    && context.gui->state(slot).visible) {
                    return false;
                }
            }
            return !(context.gui->state("LB_ITEMS").visible && context.gui->state("BTN_INV_HEAD").visible
                     && overlapping(context, "LB_ITEMS", "BTN_INV_HEAD"));
        };
        // equip.gui draws LBL_INV_WEAP_R (a label) over BTN_INV_WEAP_R in the same rectangle, after it
        // in the file: a hit test that returned the topmost control would give the label.
        ops_.push_back(expect("proof_label_shadows_button_by_geometry", [overlapping](Context& context) {
            const kotor::GuiControl* label = context.gui->control("LBL_INV_WEAP_R");
            const kotor::GuiControl* button = context.gui->control("BTN_INV_WEAP_R");
            return overlapping(context, "LBL_INV_WEAP_R", "BTN_INV_WEAP_R") && label != nullptr && button != nullptr
                && label->extent.left == button->extent.left && label->extent.top == button->extent.top;
        }));
        ops_.push_back(expect("pointer_on_slot_hits_the_button_not_the_label", [](Context& context) {
            const kotor::GuiControl* button = context.gui->control("BTN_INV_WEAP_R");
            if (button == nullptr) {
                return false;
            }
            const std::string hit = context.gui->controlAt(
                static_cast<float>(button->extent.left) + 0.5f * static_cast<float>(button->extent.width),
                static_cast<float>(button->extent.top) + 0.5f * static_cast<float>(button->extent.height));
            return hit == button->key;
        }));
        ops_.push_back(expect("equip_slots_mode_has_no_overlap", [separate](Context& context) {
            return context.gui->state("BTN_INV_WEAP_R").visible && !context.gui->state("LB_DESC").visible && separate(context);
        }));
        ops_.push_back(click("BTN_INV_WEAP_R"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("equip_slot_click_lists_weapons", [separate](Context& context) {
            const GuiControlState list = context.gui->state("LB_ITEMS");
            return list.visible && !list.items.empty() && context.gui->state("LB_DESC").visible
                && !context.gui->state("LB_DESC").items.empty() && !context.gui->state("BTN_INV_WEAP_R").visible
                && context.gui->state("BTN_EQUIP").visible && separate(context);
        }));
        ops_.push_back(capture("equip_slot", 40));
        ops_.push_back(click("BTN_BACK"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("equip_cancel_returns_to_slots", [separate](Context& context) {
            return context.gui->active() && context.gui->screenName() == "equip"
                && context.gui->state("BTN_INV_WEAP_R").visible && !context.gui->state("LB_DESC").visible && separate(context);
        }));
        ops_.push_back(click("BTN_INV_WEAP_R"));
        ops_.push_back(wait(3));
        ops_.push_back(click("BTN_EQUIP"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("equip_button_wields_it", [](Context& context) {
            const std::uint32_t pc = context.game->scriptWorld().pc;
            return context.game->wornItemObject(pc, 4) != 0 && context.gui->state("BTN_INV_WEAP_R").visible;
        }));
        ops_.push_back(capture("equip_after", 40));
        ops_.push_back(click("BTN_BACK"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("equip_closes", [](Context& context) {
            return !context.gui->active() && context.game->activePanel() == UiPanel::None;
        }));

        topBar("BTN_ABI");
        ops_.push_back(screenIs("abilities"));
        ops_.push_back(wait(3));
        ops_.push_back(painted("abilities"));
        layout("abilities");
        auto firstSkill = std::make_shared<std::string>();
        ops_.push_back(expect("abilities_lists_skills", [firstSkill](Context& context) {
            const GuiControlState list = context.gui->state("LB_ABILITY");
            *firstSkill = list.items.empty() ? std::string{} : list.items[0].text;
            return !list.items.empty() && !firstSkill->empty();
        }));
        // Retail 006ad560 and paired frame ui_abilities: the skill page's own heads and the skill icons.
        // Fails on the file's power-page strrefs and on rows with no icon.
        ops_.push_back(expect("abilities_skill_heads_and_icons", [](Context& context) {
            const auto& tlk = context.game->localization().tlk();
            const GuiControlState list = context.gui->state("LB_ABILITY");
            return context.gui->state("LBL_SKILLRANK").text == tlk.text(1579)
                && context.gui->state("LBL_BONUS").text == tlk.text(32129)
                && context.gui->state("LBL_TOTAL").text == tlk.text(41904)
                && !list.items.empty() && !list.items[0].icon.empty();
        }));
        ops_.push_back(hover("LB_DESC")); // the layout checks leave the focus on another tab
        ops_.push_back(hover("BTN_SKILLS"));
        ops_.push_back(wait(2));
        ops_.push_back(capture("abilities", 40));
        // Paired frame ui_abilities: the page's own tab is the lit one. Fails on the old selected-only state.
        ops_.push_back(expect("abilities_skills_tab_is_the_lit_one", [](Context& context) {
            return context.gui->state("BTN_SKILLS").marked && !context.gui->state("BTN_FEATS").marked;
        }));
        ops_.push_back(click("BTN_FEATS"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("abilities_feats_tab_changes_list", [firstSkill](Context& context) {
            const GuiControlState list = context.gui->state("LB_ABILITY");
            return !list.items.empty() && list.items[0].text != *firstSkill;
        }));
        ops_.push_back(expect("abilities_feats_are_rows_of_icons", [](Context& context) {
            const std::vector<GuiListItem> items = context.gui->state("LB_ABILITY").items;
            return !items.empty() && !items[0].cells.empty() && items[0].cells[0].value >= 0
                && !context.gui->state("LBL_NAME").text.empty();
        }));
        ops_.push_back(capture("abilities_feats", 40));
        ops_.push_back(click("BTN_EXIT"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("abilities_closes", [](Context& context) { return !context.gui->active(); }));

        // Party members: two recruits join, the HUD shows their three portraits, and the equip and
        // abilities screens switch to them with the shipped BTN_CHANGE1/2 and BTN_CHARLEFT/RIGHT
        // controls (what is shown there is that member's sheet and what that member wears).
        ops_.push_back(expect("party_members_fixture", [](Context& context) {
            kotor::Vec3 at = context.game->position();
            at.x += 1.5f;
            context.game->addAvailableNpcByTemplate(kNpcBastila, "p_bastilla");
            context.game->addAvailableNpcByTemplate(kNpcCarth, "p_carth");
            // The member arrows draw only with more than two available NPCs (006b5b60, 006abec0, 006aee30).
            context.game->addAvailableNpcByTemplate(kNpcMission, "p_mission");
            const std::uint32_t bastila = context.game->spawnAvailableNpc(kNpcBastila, at, 0.0f);
            at.x += 1.5f;
            const std::uint32_t carth = context.game->spawnAvailableNpc(kNpcCarth, at, 0.0f);
            context.game->addPartyNpc(kNpcBastila, bastila);
            context.game->addPartyNpc(kNpcCarth, carth);
            return context.game->partyTable().members.size() == 2 && context.game->partyObjects().size() == 3;
        }));
        ops_.push_back(wait(20));
        ops_.push_back(capture("hud_party", 60));
        ops_.push_back(expect("hud_party_three_portraits", [](Context& context) {
            const CharacterSheet& pc = context.game->character();
            const std::int32_t row = pc.portraitRow >= 0
                ? pc.portraitRow
                : context.game->rules().portraitRowForAppearance(static_cast<std::int32_t>(context.game->playerAppearance()));
            logInfo(concat("front_end: pc portrait row ", pc.portraitRow, " appearance ", context.game->playerAppearance(),
                           " shown row ", row, " art '",
                           context.game->rules().portraitResrefFor(context.game->rules().portraitRow(row), pc.alignment), "'"));
            for (const char* tag : {"LBL_CHAR1", "LBL_CHAR2", "LBL_CHAR3"}) {
                if (!context.hudGui->state(tag).visible) {
                    return false;
                }
            }
            return true;
        }));
        topBar("BTN_EQU");
        ops_.push_back(screenIs("equip"));
        ops_.push_back(wait(3));
        ops_.push_back(painted("equip_party"));
        auto ownPortrait = std::make_shared<std::string>();
        ops_.push_back(expect("equip_party_switch_controls_shown", [ownPortrait](Context& context) {
            *ownPortrait = context.gui->state("LBL_PORTRAIT").fill;
            return context.gui->state("BTN_CHANGE1").visible && context.gui->state("BTN_CHANGE2").visible
                && context.gui->state("BTN_CHARLEFT").visible && context.gui->state("BTN_CHARRIGHT").visible
                && !context.gui->state("BTN_CHANGE1").fill.empty() && !ownPortrait->empty();
        }));
        ops_.push_back(capture("equip_party", 40));
        ops_.push_back(click("BTN_CHANGE1"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("equip_party_shows_the_member", [ownPortrait](Context& context) {
            return context.gui->state("LBL_PORTRAIT").fill != *ownPortrait
                && !context.gui->state("LBL_PORTRAIT").fill.empty();
        }));
        ops_.push_back(capture("equip_member", 40));
        ops_.push_back(click("BTN_CHARRIGHT"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("equip_party_arrow_steps_on", [ownPortrait](Context& context) {
            return context.gui->state("LBL_PORTRAIT").fill != *ownPortrait
                && !context.gui->state("LBL_PORTRAIT").fill.empty();
        }));
        ops_.push_back(click("BTN_BACK"));
        ops_.push_back(wait(3));
        topBar("BTN_ABI");
        ops_.push_back(screenIs("abilities"));
        ops_.push_back(wait(3));
        ops_.push_back(painted("abilities_party"));
        ops_.push_back(click("BTN_CHANGE2"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("abilities_party_shows_the_member", [ownPortrait](Context& context) {
            return context.gui->state("LBL_PORTRAIT").fill != *ownPortrait
                && !context.gui->state("LB_ABILITY").items.empty();
        }));
        ops_.push_back(capture("abilities_member", 40));
        ops_.push_back(click("BTN_EXIT"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("party_members_leave", [](Context& context) {
            context.game->removePartyNpc(kNpcBastila);
            context.game->removePartyNpc(kNpcCarth);
            return context.game->partyTable().members.empty();
        }));

        // The feedback the session gave the player is listed in the messages screen; a line only
        // appears once something happens (the proof: the list is the log's size before, one more after).
        auto feedbackBefore = std::make_shared<std::size_t>(0);
        ops_.push_back(expect("messages_feedback_mark", [feedbackBefore](Context& context) {
            *feedbackBefore = context.game->feedbackLog().size();
            context.game->giveXp(37);
            return context.game->feedbackLog().size() == *feedbackBefore + 1
                && context.game->feedbackLog().back().find("37") != std::string::npos;
        }));
        topBar("BTN_MSG");
        ops_.push_back(screenIs("messages"));
        ops_.push_back(wait(3));
        ops_.push_back(painted("messages"));
        layout("messages");
        auto feedbackShown = std::make_shared<bool>(false);
        // Paired frame ui_messages, retail 00624df0. Fails on the old screen, which opened on the feedback.
        ops_.push_back(expect("messages_opens_on_the_dialog_log", [](Context& context) {
            const auto& tlk = context.game->localization().tlk();
            return context.gui->state("LB_DIALOG").visible && !context.gui->state("LB_MESSAGES").visible
                && context.gui->state("BTN_SHOW").text == tlk.text(42142)
                && context.gui->state("LBL_MESSAGES").text == tlk.text(1563) + " - " + tlk.text(371);
        }));
        ops_.push_back(expect("messages_before", [feedbackShown](Context& context) {
            *feedbackShown = context.gui->state("LB_MESSAGES").visible;
            return true;
        }));
        ops_.push_back(capture("messages", 40));
        ops_.push_back(expect("messages_lists_the_feedback", [](Context& context) {
            const GuiControlState list = context.gui->state("LB_MESSAGES");
            const auto& log = context.game->feedbackLog();
            return !log.empty() && list.items.size() == log.size()
                && list.items.back().text.find("37") != std::string::npos;
        }));
        ops_.push_back(click("BTN_SHOW"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("messages_toggle_switches_list", [feedbackShown](Context& context) {
            return context.gui->state("LB_MESSAGES").visible != *feedbackShown
                && context.gui->state("LB_DIALOG").visible == *feedbackShown;
        }));
        ops_.push_back(click("BTN_EXIT"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("messages_closes", [](Context& context) { return !context.gui->active(); }));

        // The galaxy map: a script opens it (ShowGalaxyMap), the pointer picks a planet.
        ops_.push_back(expect("galaxy_fixture", [](Context& context) {
            const auto planets = context.game->galaxyPlanetNames();
            for (const auto& [index, name] : planets) {
                const bool used = index >= 1 && index <= 10;
                context.game->setPlanet(static_cast<std::size_t>(index), used, used);
            }
            context.game->showGalaxyMap(1);
            return !planets.empty();
        }));
        ops_.push_back(screenIs("galaxymap"));
        ops_.push_back(wait(3));
        ops_.push_back(painted("galaxymap"));
        // Retail 00694b40: the first available and selectable planet when nothing is highlighted, so the
        // description is filled on open. Module list: BTN_WARP/LB_MODULES are mainmenu's (0067ace0), not here.
        ops_.push_back(click("BTN_BACK"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("galaxy_reopen_with_nothing_highlighted", [](Context& context) {
            context.game->showGalaxyMap(-1);
            return true;
        }));
        ops_.push_back(screenIs("galaxymap"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("galaxy_description_default_on_open", [](Context& context) {
            return !context.gui->state("LBL_DESC").text.empty() && !context.gui->state("LBL_PLANETNAME").text.empty()
                && context.gui->control("LB_MODULES") == nullptr;
        }));
        layout("galaxymap");
        ops_.push_back(capture("galaxymap", 40));
        auto planetLabel = std::make_shared<std::string>();
        ops_.push_back(expect("galaxy_shows_planet_buttons", [planetLabel](Context& context) {
            std::size_t shown = 0;
            for (const char* tag : {"LBL_Planet_Taris", "LBL_Planet_Dantooine", "LBL_Planet_Tatooine", "LBL_Planet_Kashyyyk"}) {
                shown += context.gui->state(tag).visible ? 1 : 0;
            }
            *planetLabel = context.gui->state("LBL_PLANETNAME").text;
            return shown >= 3;
        }));
        ops_.push_back(click("LBL_Planet_Tatooine"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("galaxy_click_selects_planet", [planetLabel](Context& context) {
            const std::string name = context.gui->state("LBL_PLANETNAME").text;
            return !name.empty() && name != *planetLabel && context.gui->state("BTN_ACCEPT").enabled
                && !context.gui->state("LBL_DESC").text.empty();
        }));
        ops_.push_back(capture("galaxymap_picked", 40));
        // Retail 00695180 / 006933a0: galaxy in 3D_PlanetDisplay, the picked planet in 3D_PlanetModel. Without a
        // model each rectangle is its flat fill, so a varied picture is what tells them apart. The camera rides
        // the zoomin clip, so the planet grows: a later frame differs from this one.
        const auto varied = [this](const char* tag, const char* name) {
            return expect(name, [this, tag, name](Context& context) {
                const kotor::GuiControl* control = context.gui->control(tag);
                if (control == nullptr || lastFrame_.rgba.empty()) {
                    return false;
                }
                const GuiView view = context.painter->layout(*context.gui->screen(), static_cast<float>(lastFrame_.width),
                                                             static_cast<float>(lastFrame_.height));
                const GuiPixelRect box = guiControlPixels(view, *control);
                std::set<int> levels;
                std::size_t lit = 0;
                std::size_t total = 0;
                for (int y = static_cast<int>(box.y0) + 8; y < static_cast<int>(box.y1) - 8; y += 2) {
                    for (int x = static_cast<int>(box.x0) + 8; x < static_cast<int>(box.x1) - 8; x += 2) {
                        const float luma = lumaAt(lastFrame_, x, y);
                        levels.insert(static_cast<int>(luma));
                        lit += luma > 40.0f ? 1u : 0u;
                        ++total;
                    }
                }
                logInfo(concat("front_end: ", name, " levels=", levels.size(), " lit=", lit, "/", total));
                // The galaxy model is emitters: without them the box stays black and these few levels fail.
                return levels.size() >= 24 && lit > 0;
            });
        };
        ops_.push_back(expect("galaxy_model_is_the_rows_own", [](Context& context) {
            return context.screens->previewPlanetModel().rfind("planet_", 0) == 0;
        }));
        ops_.push_back(varied("3D_PlanetDisplay", "galaxy_display_draws_the_model"));
        ops_.push_back(varied("3D_PlanetModel", "galaxy_planet_draws_the_model"));
        // The panel art is clipped around the 3D rectangles: no seam of world along their edges. Fails on the
        // fractional edges, where a scissor lost one row and the world showed through it.
        ops_.push_back(expect("galaxy_no_seam_at_the_3d_edges", [this](Context& context) {
            const kotor::GuiControl* control = context.gui->control("3D_PlanetDisplay");
            if (control == nullptr || lastFrame_.rgba.empty()) {
                return false;
            }
            const GuiView view = context.painter->layout(*context.gui->screen(), static_cast<float>(lastFrame_.width),
                                                         static_cast<float>(lastFrame_.height));
            const GuiPixelRect box = guiControlPixels(view, *control);
            const int edge = static_cast<int>(std::lround(box.y1));
            const int from = static_cast<int>(view.pxX(0.0f)) + 4;
            const int to = static_cast<int>(box.x0) - 4;
            int odd = 0;
            int total = 0;
            for (int y = edge - 1; y <= edge + 1; ++y) {
                int rowOdd = 0;
                int rowTotal = 0;
                for (int x = from; x < to; ++x) {
                    const float above = lumaAt(lastFrame_, x, y - 1);
                    const float here = lumaAt(lastFrame_, x, y);
                    const float below = lumaAt(lastFrame_, x, y + 1);
                    rowOdd += std::abs(above - below) < 3.0f && std::abs(here - 0.5f * (above + below)) > 4.0f ? 1 : 0;
                    ++rowTotal;
                }
                total = rowTotal;
                odd = std::max(odd, rowOdd);
            }
            logInfo(concat("front_end: galaxy seam columns ", odd, "/", total, " at row ", edge));
            return total > 0 && odd * 4 < total;
        }));
        // Shipped planet models: zoomin ends 1.47 from a planet of diameter 1, and with retail's field of view that
        // fills the box. Fails where the camera went back to its start when rotate took over: about 1 percent lit.
        auto zoomStart = std::make_shared<std::uint64_t>(0);
        ops_.push_back(expect("galaxy_zoom_timer", [zoomStart](Context&) {
            *zoomStart = SDL_GetTicks();
            return true;
        }));
        ops_.push_back(waitUntil("galaxy_zoom_time_passed", [zoomStart](Context&) {
            return SDL_GetTicks() - *zoomStart > 4500;
        }, 100000));
        ops_.push_back(capture("galaxymap_zoomed", 40));
        ops_.push_back(expect("galaxy_planet_fills_after_the_zoom", [this](Context& context) {
            const kotor::GuiControl* control = context.gui->control("3D_PlanetModel");
            if (control == nullptr || lastFrame_.rgba.empty()) {
                return false;
            }
            const GuiView view = context.painter->layout(*context.gui->screen(), static_cast<float>(lastFrame_.width),
                                                         static_cast<float>(lastFrame_.height));
            const GuiPixelRect box = guiControlPixels(view, *control);
            std::size_t lit = 0;
            std::size_t total = 0;
            for (int y = static_cast<int>(box.y0) + 2; y < static_cast<int>(box.y1) - 2; y += 2) {
                for (int x = static_cast<int>(box.x0) + 2; x < static_cast<int>(box.x1) - 2; x += 2) {
                    lit += lumaAt(lastFrame_, x, y) > 20.0f ? 1u : 0u;
                    ++total;
                }
            }
            logInfo(concat("front_end: galaxy planet lit after the zoom ", lit, "/", total));
            return total > 0 && lit * 2 > total;
        }));
        auto modelBefore = std::make_shared<std::string>();
        ops_.push_back(expect("galaxy_other_planet_before", [modelBefore](Context& context) {
            *modelBefore = context.screens->previewPlanetModel();
            return true;
        }));
        ops_.push_back(click("LBL_Planet_Taris"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("galaxy_other_planet_other_model", [modelBefore](Context& context) {
            return context.screens->previewPlanetModel() != *modelBefore;
        }));
        ops_.push_back(capture("galaxymap_planet2", 40));
        ops_.push_back(varied("3D_PlanetModel", "galaxy_planet2_draws_the_model"));
        ops_.push_back(click("BTN_BACK"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("galaxy_back_closes", [](Context& context) {
            return !context.gui->active() && !context.game->galaxyMapOpen();
        }));

        // Party selection: companions made available, then toggled by clicks.
        ops_.push_back(expect("party_selection_fixture", [](Context& context) {
            context.game->addAvailableNpcByTemplate(kNpcBastila, "p_bastilla");
            context.game->addAvailableNpcByTemplate(kNpcCarth, "p_carth");
            context.game->addAvailableNpcByTemplate(kNpcCanderous, "p_canderous");
            return context.game->isAvailableNpc(kNpcBastila) && context.game->openPartySelection("", -1, -1);
        }));
        ops_.push_back(screenIs("partyselection"));
        ops_.push_back(wait(3));
        ops_.push_back(painted("partyselection"));
        layout("partyselection");
        ops_.push_back(capture("partyselection", 40));
        auto slotsBefore = std::make_shared<std::string>();
        ops_.push_back(expect("party_slots_before", [slotsBefore](Context& context) {
            *slotsBefore = context.gui->state("LBL_COUNT").text;
            return !slotsBefore->empty();
        }));
        ops_.push_back(click("BTN_NPC0"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("party_pick_uses_a_slot", [slotsBefore](Context& context) {
            return context.gui->state("LBL_COUNT").text != *slotsBefore;
        }));
        ops_.push_back(expect("party_click_toggles_member", [](Context& context) {
            const std::vector<int>& chosen = context.game->partySelection().chosen;
            return std::find(chosen.begin(), chosen.end(), kNpcBastila) != chosen.end();
        }));
        // Retail 006becc0 from the click handler 006bf2a0: a picked member's button reads Remove (38456).
        ops_.push_back(expect("party_button_says_remove_for_a_picked_member", [](Context& context) {
            return context.gui->state("BTN_ACCEPT").text == context.game->localization().tlk().text(38456);
        }));
        // Retail 006bf5b0: the box is a label whose fill is the member's portrait name plus "3", not a model.
        // Fails on the 3D preview, which left this fill empty and cut the body's feet off.
        ops_.push_back(expect("party_box_is_the_shipped_full_body_picture", [](Context& context) {
            const GuiControlState box = context.gui->state("LBL_3D");
            logInfo(concat("front_end: party box fill '", box.fill, "' level '", context.gui->state("LBL_NPC_LEVEL").text, "'"));
            return box.visible && box.fill == "po_pbastila3";
        }));
        ops_.push_back(expect("party_level_line_names_level_and_class", [](Context& context) {
            const std::string line = context.gui->state("LBL_NPC_LEVEL").text;
            const std::string word = context.game->localization().tlk().text(32154);
            return line.rfind(word + " ", 0) == 0 && line.size() > word.size() + 3;
        }));
        // The fixture's Canderous slot stays unavailable here, so the third slot (Carth) is the one to point at. An
        // empty member hides the box (006bf5b0); that is the first check.
        ops_.push_back(hover("BTN_NPC1"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("party_box_hidden_on_an_empty_member", [](Context& context) {
            return context.gui->state("BTN_NPC1").enabled || !context.gui->state("LBL_3D").visible;
        }));
        ops_.push_back(hover("BTN_NPC2"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("party_box_follows_the_pointer", [](Context& context) {
            const GuiControlState box = context.gui->state("LBL_3D");
            logInfo(concat("front_end: party box under the pointer '", box.fill, "' hovered '", context.gui->hovered(), "'"));
            return box.visible && box.fill != "po_pbastila3" && !box.fill.empty();
        }));
        ops_.push_back(hover("BTN_NPC0"));
        ops_.push_back(capture("partyselection_picked", 40));
        ops_.push_back(click("BTN_NPC0"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("party_second_click_clears_member", [](Context& context) {
            const std::vector<int>& chosen = context.game->partySelection().chosen;
            return std::find(chosen.begin(), chosen.end(), kNpcBastila) == chosen.end();
        }));
        ops_.push_back(expect("party_button_says_add_after_removal", [](Context& context) {
            return context.gui->state("BTN_ACCEPT").text == context.game->localization().tlk().text(38455);
        }));
        ops_.push_back(click("BTN_BACK"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("party_back_closes", [](Context& context) {
            return !context.gui->active() && !context.game->partySelection().active;
        }));
    }

    ops_.push_back(expect("journal_fixture", [](Context& context) {
        for (const std::int32_t state : {10, 1, 20, 30, 40}) {
            if (!context.game->journalEntryText("end_attack", state).empty()) {
                return context.game->addJournalEntry("end_attack", state);
            }
        }
        return false;
    }));
    ops_.push_back(key(SDL_SCANCODE_L, SDLK_L)); // keymap.2da row 214: Quests
    ops_.push_back(screenIs("journal"));
    ops_.push_back(wait(3));
    ops_.push_back(painted("journal"));
    layout("journal");
    ops_.push_back(expect("journal_lists_quest", [](Context& context) {
        const GuiControlState list = context.gui->state("LB_ITEMS");
        const GuiControlState entry = context.gui->state("LBL_ITEM_DESCRIPTION");
        return !list.items.empty() && !list.items[0].text.empty() && !entry.items.empty()
            && !entry.items[0].text.empty();
    }));
    ops_.push_back(capture("journal", 50));
    ops_.push_back(click("BTN_EXIT"));
    ops_.push_back(wait(3));
    ops_.push_back(expect("journal_closes", [](Context& context) { return !context.gui->active(); }));

    ops_.push_back(key(SDL_SCANCODE_P, SDLK_P));
    ops_.push_back(screenIs("character"));
    ops_.push_back(wait(3));
    ops_.push_back(painted("character"));
    layout("character");
    // Retail 006afda0: both buttons show only with a level to take (005a6810). What was on them in the older frames
    // was this gate's own fixture: logged here, then taken away so the frame shows a sheet with nothing to spend.
    ops_.push_back(expect("character_level_buttons_show_with_points", [](Context& context) {
        const CharacterSheet& sheet = context.game->character();
        logInfo(concat("front_end: character points attr ", sheet.attributePoints, " skill ", sheet.skillPoints, " feat ",
                       sheet.featPoints, " power ", sheet.powerPoints));
        return context.gui->state("BTN_LEVELUP").visible == context.game->canLevelUp();
    }));
    auto pointsKept = std::make_shared<std::array<std::int32_t, 4>>();
    ops_.push_back(expect("character_points_set_aside", [pointsKept](Context& context) {
        CharacterSheet& sheet = context.game->character();
        *pointsKept = {sheet.attributePoints, sheet.skillPoints, sheet.featPoints, sheet.powerPoints};
        sheet.attributePoints = sheet.skillPoints = sheet.featPoints = sheet.powerPoints = 0;
        return true;
    }));
    ops_.push_back(wait(3));
    ops_.push_back(expect("character_no_level_buttons_without_points", [](Context& context) {
        return !context.gui->state("BTN_LEVELUP").visible && !context.gui->state("BTN_AUTO").visible;
    }));
    ops_.push_back(hover("LBL_EXPERIENCE_STAT"));
    ops_.push_back(wait(300)); // the backdrop is emitters, which fill in from nothing
    ops_.push_back(capture("character_nothing_to_spend", 6));
    ops_.push_back(expect("character_shows_sheet", [](Context& context) {
        return context.gui->state("LBL_NAME").text == context.game->character().name
            && !context.gui->state("LBL_STR").text.empty() && !context.gui->state("LBL_VITALITY_STAT").text.empty();
    }));
    ops_.push_back(capture("character", 60));
    // The retail sheet at alignment 10 and 90 (paired retail cs_al10, cs_al90): the slider and the backdrop move,
    // the body keeps its framing.
    auto alignmentKept = std::make_shared<std::int32_t>(50);
    ops_.push_back(expect("character_alignment_dark", [alignmentKept](Context& context) {
        *alignmentKept = context.game->alignment();
        context.game->setAlignment(10);
        return context.game->alignment() == 10;
    }));
    ops_.push_back(wait(300));
    // 006afda0: 00417ee0(100 - alignment); the arrow follows the alignment while the sheet is open.
    ops_.push_back(expect("character_al10_slider", [](Context& context) {
        return std::abs(context.gui->value("SLD_ALIGN") - 90.0f) < 0.5f;
    }));
    ops_.push_back(capture("character_al10", 6));
    ops_.push_back(expect("character_alignment_light", [](Context& context) {
        context.game->setAlignment(90);
        return context.game->alignment() == 90;
    }));
    ops_.push_back(wait(300));
    // 006afda0: 00417ee0(100 - alignment); the arrow follows the alignment while the sheet is open.
    ops_.push_back(expect("character_al90_slider", [](Context& context) {
        return std::abs(context.gui->value("SLD_ALIGN") - 10.0f) < 0.5f;
    }));
    ops_.push_back(capture("character_al90", 6));
    ops_.push_back(expect("character_alignment_put_back", [alignmentKept](Context& context) {
        context.game->setAlignment(*alignmentKept);
        return context.game->alignment() == *alignmentKept;
    }));
    ops_.push_back(wait(3));
    ops_.push_back(expect("character_points_put_back", [pointsKept](Context& context) {
        CharacterSheet& sheet = context.game->character();
        sheet.attributePoints = (*pointsKept)[0];
        sheet.skillPoints = (*pointsKept)[1];
        sheet.featPoints = (*pointsKept)[2];
        sheet.powerPoints = (*pointsKept)[3];
        return true;
    }));
    ops_.push_back(key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE));
    ops_.push_back(wait(3));
    ops_.push_back(expect("escape_closes_character", [](Context& context) {
        return !context.gui->active() && context.game->activePanel() == UiPanel::None;
    }));

    // ---- art whose TXI says `blending additive` (retail draws it SRC_ALPHA / ONE, so its black adds
    // nothing): over a bright grey field every opaque-black texel of each must leave the field
    // as it was, the art's own bright colour must still brighten it, and art with no such TXI keeps
    // its black.
    {
        static const char* const additiveArt[] = {
        "lbl_miabi",
        "lbl_miabi2",
        "lbl_mibox00",
        "lbl_mibox00a",
        "lbl_micha",
        "lbl_micha2",
        "lbl_midialog",
        "lbl_miequ",
        "lbl_miequ2",
        "lbl_miindpc01f",
        "lbl_miinv",
        "lbl_miinv2",
        "lbl_mileft",
        "lbl_mileftbk",
        "lbl_mimap",
        "lbl_mimap2",
        "lbl_mimsg",
        "lbl_mimsg2",
        "lbl_minimap",
        "lbl_miopt",
        "lbl_miopt2",
        "lbl_miport1",
        "lbl_miport2",
        "lbl_miport3",
        "lbl_mique",
        "lbl_mique2",
        "lbl_miright",
        "lbl_mirightbot",
        "lbl_static"};
        static const char* const solidControl = "lbl_party"; // 68% opaque near-black in the shipped data, no such TXI
        constexpr std::size_t kAdditiveCount = sizeof(additiveArt) / sizeof(additiveArt[0]);
        ops_.push_back(expect("alpha_probe_on", [this](Context&) {
            artProbe_.clear();
            for (const char* name : additiveArt) {
                artProbe_.emplace_back(name);
            }
            artProbe_.emplace_back(solidControl);
            return true;
        }));
        ops_.push_back(wait(3));
        ops_.push_back(capture("alpha_probe", 3));
        ops_.push_back(expect("alpha_probe_measure", [this](Context& context) {
            bool all = true;
            // The original texels (the decoder the painter uses, rows turned over as it does).
            const auto load = [&](const std::string& name, kotor::Image& image) {
                std::uint32_t type = 0;
                auto data = context.game->install().findTexture(name, &type);
                if (!data.has_value()) {
                    return false;
                }
                image = kotor::decodeTexture(*data, type, name);
                if (type == static_cast<std::uint32_t>(kotor::ResType::Tpc)) {
                    const std::size_t row = static_cast<std::size_t>(image.width) * 4u;
                    for (int y = 0; y < image.height / 2; ++y) {
                        std::swap_ranges(image.rgba.begin() + static_cast<std::ptrdiff_t>(y * row),
                                         image.rgba.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                                         image.rgba.begin() + static_cast<std::ptrdiff_t>((image.height - 1 - y) * row));
                    }
                }
                return true;
            };
            for (std::size_t index = 0; index < artProbe_.size(); ++index) {
                const std::string name = artProbe_[index];
                const bool control = index == kAdditiveCount;
                kotor::Image image;
                const GuiTexture* texture = context.painter->texture(name);
                if (texture == nullptr || !load(name, image) || lastFrame_.rgba.empty()) {
                    logInfo(concat("front_end: alpha_probe ", name, " unreadable"));
                    all = false;
                    continue;
                }
                float cx = 0.0f;
                float cy = 0.0f;
                probeCell(index, static_cast<float>(lastFrame_.width), cx, cy);
                const float fit = std::min(1.0f, kProbeBox / static_cast<float>(std::max(image.width, image.height)));
                const int radius = static_cast<int>(std::ceil(1.0f / fit)) + 1;
                const auto texel = [&](int x, int y) {
                    const std::uint8_t* p = &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
                    return std::array<int, 4>{p[0], p[1], p[2], p[3]};
                };
                int blackTested = 0;
                int blackClear = 0;
                int artTested = 0;
                int artShown = 0;
                const int stepX = std::max(1, image.width / 24);
                const int stepY = std::max(1, image.height / 24);
                for (int y = radius; y < image.height - radius; y += stepY) {
                    for (int x = radius; x < image.width - radius; x += stepX) {
                        const auto center = texel(x, y);
                        const int top = std::max(center[0], std::max(center[1], center[2]));
                        const int low = std::min(center[0], std::min(center[1], center[2]));
                        bool blackArea = center[3] >= 250 && top <= 23;
                        bool artArea = center[3] >= 250 && top >= 160 && top - low >= 100;
                        for (int dy = -radius; dy <= radius && (blackArea || artArea); ++dy) {
                            for (int dx = -radius; dx <= radius; ++dx) {
                                const auto n = texel(x + dx, y + dy);
                                const int nTop = std::max(n[0], std::max(n[1], n[2]));
                                if (n[3] < 250 || nTop > 23) {
                                    blackArea = false;
                                }
                                if (n[3] < 250 || nTop < 120) {
                                    artArea = false;
                                }
                            }
                        }
                        const int fx = static_cast<int>(cx + (static_cast<float>(x) + 0.5f) * fit);
                        const int fy = static_cast<int>(cy + (static_cast<float>(y) + 0.5f) * fit);
                        if (fx < 0 || fy < 0 || fx >= lastFrame_.width || fy >= lastFrame_.height) {
                            continue;
                        }
                        const std::uint8_t* shot = &lastFrame_.rgba[(static_cast<std::size_t>(fy) * lastFrame_.width + fx) * 4];
                        if (blackArea) {
                            ++blackTested;
                            const auto onField = [](int value) {
                                return std::abs(value - FrontEndGate::kProbeField) <= 30;
                            };
                            const bool clear = control ? (shot[0] < 60 && shot[1] < 60 && shot[2] < 60)
                                                       : (onField(shot[0]) && onField(shot[1]) && onField(shot[2]));
                            blackClear += clear ? 1 : 0;
                        } else if (artArea && !control) {
                            ++artTested;
                            const int lift = FrontEndGate::kProbeField + 40;
                            artShown += (shot[0] > lift || shot[1] > lift || shot[2] > lift) ? 1 : 0;
                        }
                    }
                }
                // A texture with no opaque-black area has nothing to test for the background; the
                // control must have some or it proves nothing.
                const bool black = blackTested == 0 ? !control : blackClear * 100 >= blackTested * 97;
                const bool art = artTested == 0 || artShown * 100 >= artTested * 90;
                const bool flagged = control ? !texture->additive : texture->additive;
                logInfo(concat("front_end: alpha_probe ", name, control ? " (control)" : "", " black ",
                               blackClear, "/", blackTested, " art ", artShown, "/", artTested,
                               flagged ? "" : " NOT-FLAGGED"));
                all = all && black && art && flagged;
            }
            return all;
        }));
        ops_.push_back(expect("alpha_probe_off", [this](Context&) {
            artProbe_.clear();
            return true;
        }));
        ops_.push_back(wait(3));
    }

    // ---- level up as the retail frames show it (paired levelup_main, levelup_skills, levelup_feats): a level 3
    // Soldier named Kran Katarn (portrait po_pmhc4) in armour. 006ee7d0 reads the creature after its class level
    // was raised (006e8ef0 -> 0064a2a0 on the original) and shows attributes only when that level is a multiple
    // of 4, so level 3 has skills, feats and Accept, numbered 1..3. Lighting a row resets its picks (006ee0e0) and
    // only the lit row opens. The PC is kept for the block below and restored after it.
    auto levelSaved = std::make_shared<CharacterSheet>();
    auto levelSavedLook = std::make_shared<std::uint32_t>(0);
    auto levelArmour = std::make_shared<std::uint32_t>(script::kObjectInvalid);
    {
        const auto hubLit = [](Context& context, int row) {
            for (int i = 1; i <= 5; ++i) {
                if (context.gui->state("LBL_" + std::to_string(i)).marked != (i == row)) {
                    return false;
                }
            }
            return true;
        };
        ops_.push_back(expect("levelup_kran_fixture", [levelSaved, levelSavedLook, levelArmour](Context& context) {
            GameSession& game = *context.game;
            const Rules& rules = game.rules();
            *levelSaved = game.character();
            *levelSavedLook = game.playerAppearance();
            CharacterSheet sheet = game.character();
            std::int32_t portrait = -1;
            for (const std::int32_t row : rules.pcPortraitRows(0)) {
                std::string resref = rules.portraitRow(row).resref;
                std::transform(resref.begin(), resref.end(), resref.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                portrait = resref == "po_pmhc4" ? row : portrait;
            }
            sheet.name = "Kran Katarn";
            sheet.classId = 0;
            sheet.level = 3;
            sheet.gender = 0;
            sheet.portraitRow = portrait;
            sheet.str = 15;
            sheet.dex = 14;
            sheet.con = 12;
            sheet.intel = 8;
            sheet.wis = 16;
            sheet.cha = 10;
            sheet.maxHp = rules.maxHitPoints(0, 3, sheet.con);
            sheet.hp = sheet.maxHp;
            for (auto& skill : sheet.skills) {
                skill.second = 0;
            }
            sheet.skills[rules.skillName(4)] = 1; // the retail frame's Persuade and Treat Injury ranks
            sheet.skills[rules.skillName(7)] = 3;
            sheet.feats.insert(3);  // the retail frame's taken Two-Weapon Fighting (feat.2da row 3)
            sheet.feats.insert(11); // and Flurry (row 11)
            sheet.attributePoints = 0;
            sheet.skillPoints = rules.skillPointsForLevel(0, sheet.intel);
            sheet.featPoints = 1;
            sheet.powerPoints = 0;
            game.applyCharacter(sheet);
            *levelArmour = game.giveItem("g_a_class4001", game.scriptWorld().pc, 1);
            const bool worn = *levelArmour != script::kObjectInvalid
                && game.equipItemObject(game.scriptWorld().pc, *levelArmour);
            logInfo(concat("front_end: kran portrait row ", portrait, " appearance ", game.playerAppearance(), " armour ", worn,
                           " skill points ", sheet.skillPoints, " max hp ", sheet.maxHp));
            return portrait >= 0 && worn && game.canLevelUp();
        }));
        ops_.push_back(key(SDL_SCANCODE_P, SDLK_P));
        ops_.push_back(screenIs("character"));
        ops_.push_back(wait(3));
        ops_.push_back(click("BTN_LEVELUP"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("maincg_leveluppnl"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("levelup_level3_rows", [hubLit](Context& context) {
            const auto shown = [&](const char* tag) { return context.gui->state(tag).visible; };
            const auto text = [&](const char* tag) { return context.gui->state(tag).text; };
            logInfo(concat("front_end: level 3 hub name '", text("LBL_NAME"), "' level '", text("LBL_LEVEL_VAL"), "' rows ",
                           shown("BTN_STEPNAME1"), shown("BTN_STEPNAME2"), shown("BTN_STEPNAME3"), shown("BTN_STEPNAME4"),
                           shown("BTN_STEPNAME5"), " numbers ", text("LBL_NUM2"), text("LBL_NUM3"), text("LBL_NUM5")));
            return text("LBL_NAME") == "Kran Katarn" && text("LBL_LEVEL_VAL") == "3" && !shown("BTN_STEPNAME1")
                && !shown("LBL_NUM1") && shown("BTN_STEPNAME2") && shown("BTN_STEPNAME3") && !shown("BTN_STEPNAME4")
                && text("LBL_NUM2") == "1" && text("LBL_NUM3") == "2" && text("LBL_NUM5") == "3" && hubLit(context, 2)
                && context.gui->state("BTN_BACK").color.y < 0.4f; // 007a2408, Back off on the first row
        }));
        ops_.push_back(hover("MAIN_TITLE_LBL"));
        ops_.push_back(wait(30)); // the sheet's smoke and clip run a moment, as in the retail frame
        ops_.push_back(capture("levelup_main", 40));
        // 006ee0e0 disables every row's button but the lit one's.
        ops_.push_back(click("BTN_STEPNAME3"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("levelup_unlit_row_stays_shut", [hubLit](Context& context) {
            return context.gui->screenName() == "maincg_leveluppnl" && hubLit(context, 2)
                && context.game->activePanel() == UiPanel::LevelUp;
        }));
        ops_.push_back(click("BTN_STEPNAME2"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("skchrgen"));
        ops_.push_back(hover("COMPUTER_USE_LBL"));
        ops_.push_back(wait(2));
        ops_.push_back(hover("MAIN_TITLE_LBL"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("levelup_level3_skills", [](Context& context) {
            logInfo(concat("front_end: level 3 skills remaining '", context.gui->state("REMAINING_SELECTIONS_LBL").text,
                           "' persuade '", context.gui->state("PERSUADE_POINTS_BTN").text, "' treat '",
                           context.gui->state("TREAT_INJURY_POINTS_BTN").text, "'"));
            return context.gui->state("REMAINING_SELECTIONS_LBL").text == "1"
                && context.gui->state("PERSUADE_POINTS_BTN").text == "1"
                && context.gui->state("TREAT_INJURY_POINTS_BTN").text == "3";
        }));
        ops_.push_back(capture("levelup_skills", 40));
        // A step's own Back drops its picks (006ee580 lights the row again, 006e8730 restores the ranks).
        ops_.push_back(click("TRE_PLUS_BTN"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("levelup_skill_spent_before_back", [](Context& context) {
            return context.game->character().skillPoints == 0;
        }));
        ops_.push_back(click("BTN_BACK"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("maincg_leveluppnl"));
        ops_.push_back(expect("levelup_step_back_resets_row", [hubLit](Context& context) {
            const CharacterSheet& sheet = context.game->character();
            const auto rank = sheet.skills.find(context.game->rules().skillName(7));
            return sheet.skillPoints == 1 && rank != sheet.skills.end() && rank->second == 3 && hubLit(context, 2);
        }));
        ops_.push_back(click("BTN_STEPNAME2"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("skchrgen"));
        ops_.push_back(click("BTN_ACCEPT"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("maincg_leveluppnl"));
        ops_.push_back(expect("levelup_level3_lights_feats", [hubLit](Context& context) { return hubLit(context, 3); }));
        ops_.push_back(click("BTN_STEPNAME3"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("ftchrgen"));
        ops_.push_back(hover("MAIN_TITLE_LBL"));
        ops_.push_back(wait(3));
        ops_.push_back(capture("levelup_feats", 40));
        ops_.push_back(click("BTN_ACCEPT"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("maincg_leveluppnl"));
        ops_.push_back(expect("levelup_level3_lights_accept", [hubLit](Context& context) { return hubLit(context, 5); }));
        ops_.push_back(click("BTN_STEPNAME5"));
        ops_.push_back(wait(5));
        ops_.push_back(expect("levelup_level3_accept_ends", [](Context& context) {
            return context.game->activePanel() == UiPanel::None && !context.gui->active();
        }));
    }

    // ---- level up: the character screen's Level Up button opens MAINCG with LEVELUPPNL laid in (retail
    // 006e8ef0, 006ee7d0): name, class, level, portrait, scores and the numbered rows for what is waiting.
    // Each row opens its shipped screen, whose Accept comes back with the next row lit; Accept ends it.
    // Fails on the old flow, which went straight to abchrgen and stepped through the screens by itself.
    {
        auto levelBefore = std::make_shared<std::array<std::int32_t, 6>>();
        const auto hubLit = [](Context& context, int row) {
            for (int i = 1; i <= 5; ++i) {
                const std::string n = std::to_string(i);
                if (context.gui->state("LBL_" + n).marked != (i == row)) {
                    return false;
                }
            }
            return true;
        };
        ops_.push_back(expect("levelup_fixture", [levelBefore](Context& context) {
            CharacterSheet& sheet = context.game->character();
            sheet.level = 4; // the attributes row: a level that is a multiple of 4 (006ee7d0)
            sheet.attributePoints = 1;
            sheet.skillPoints = 8;
            sheet.featPoints = 1;
            (*levelBefore)[0] = sheet.str;
            (*levelBefore)[1] = sheet.skillPoints;
            (*levelBefore)[2] = static_cast<std::int32_t>(sheet.feats.size());
            return context.game->canLevelUp();
        }));
        ops_.push_back(key(SDL_SCANCODE_P, SDLK_P));
        ops_.push_back(screenIs("character"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("character_offers_level_up", [](Context& context) {
            return context.gui->state("BTN_LEVELUP").visible;
        }));
        ops_.push_back(click("BTN_LEVELUP"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("maincg_leveluppnl"));
        ops_.push_back(wait(3));
        ops_.push_back(painted("maincg_leveluppnl"));
        ops_.push_back(expect("levelup_hub_fills_maincg", [](Context& context) {
            const CharacterSheet& sheet = context.game->character();
            const auto text = [&](const char* tag) { return context.gui->state(tag).text; };
            return context.game->activePanel() == UiPanel::LevelUp
                && text("MAIN_TITLE_LBL") == context.game->localization().tlk().text(1071)
                && text("LBL_NAME") == sheet.name && text("LBL_CLASS") == context.game->rules().className(sheet.classId)
                && text("LBL_LEVEL_VAL") == std::to_string(sheet.level) && context.gui->state("LBL_LEVEL_VAL").visible
                && text("STR_AB_LBL") == std::to_string(sheet.str) && text("LBL_VIT") == std::to_string(sheet.maxHp)
                && !text("NEW_FORT_LBL").empty() && context.gui->state("PORTRAIT_LBL").hasFill
                && !context.gui->state("OLD_FORT_LBL").visible;
        }));
        // 006ee7d0: attributes, skills, feats shown and numbered 1..3, powers hidden, Accept numbered 4.
        ops_.push_back(expect("levelup_hub_rows", [](Context& context) {
            const auto shown = [&](const char* tag) { return context.gui->state(tag).visible; };
            return shown("BTN_STEPNAME1") && shown("BTN_STEPNAME2") && shown("BTN_STEPNAME3") && !shown("BTN_STEPNAME4")
                && !shown("LBL_NUM4") && !shown("LBL_4") && shown("BTN_STEPNAME5")
                && context.gui->state("LBL_NUM3").text == "3" && context.gui->state("LBL_NUM5").text == "4";
        }));
        ops_.push_back(expect("levelup_hub_first_row_lit_back_off", [hubLit](Context& context) {
            for (int i = 1; i <= 5; ++i) {
                logInfo(concat("front_end: levelup hub LBL_", i, " marked ", context.gui->state("LBL_" + std::to_string(i)).marked));
            }
            logInfo(concat("front_end: levelup hub back enabled ", context.gui->state("BTN_BACK").enabled, " cancel visible ",
                           context.gui->state("BTN_CANCEL").visible));
            return hubLit(context, 1) && context.gui->state("BTN_BACK").color.y < 0.4f
                && (context.gui->control("BTN_CANCEL") == nullptr || !context.gui->state("BTN_CANCEL").visible);
        }));
        ops_.push_back(capture("levelup_main_attrs", 40));
        ops_.push_back(click("BTN_STEPNAME1"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("abchrgen"));
        ops_.push_back(wait(3));
        ops_.push_back(painted("abchrgen"));
        layout("abchrgen");
        ops_.push_back(hover("STR_LBL")); // the first row focused, as at open, and the pointer parked off the rows
        ops_.push_back(wait(2));
        ops_.push_back(hover("LB_DESC")); // takes the keyboard focus off the OK the last click left it on
        ops_.push_back(wait(2));
        ops_.push_back(hover("MAIN_TITLE_LBL"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("levelup_abilities_show_points", [levelBefore](Context& context) {
            return context.game->activePanel() == UiPanel::LevelUp
                && context.gui->state("REMAINING_SELECTIONS_LBL").text == "1"
                && context.gui->state("STR_POINTS_BTN").text == std::to_string((*levelBefore)[0])
                && !context.gui->state("STR_LBL").text.empty() && !context.gui->state("MAIN_TITLE_LBL").text.empty();
        }));
        // 006f7600 / 006f70e0: "Attributes", a point costs 1, the first row's modifier shown.
        ops_.push_back(expect("levelup_abilities_title_cost_modifier", [](Context& context) {
            // The focused row (yellow, 007a23c0) follows the pointer; its score gives the modifier.
            const CharacterSheet& sheet = context.game->character();
            const std::int32_t scores[6] = {sheet.str, sheet.dex, sheet.con, sheet.intel, sheet.wis, sheet.cha};
            static constexpr const char* rows[6] = {"STR_LBL", "DEX_LBL", "CON_LBL", "INT_LBL", "WIS_LBL", "CHA_LBL"};
            int focused = -1;
            for (int i = 0; i < 6; ++i) {
                const GuiControlState row = context.gui->state(rows[i]);
                if (row.hasColor && row.color.x > 0.9f && row.color.y > 0.9f) {
                    focused = focused < 0 ? i : 99;
                }
            }
            if (focused < 0 || focused > 5) {
                return false;
            }
            const int mod = scores[focused] / 2 - 5;
            const std::string want = mod == 0 ? std::string("-") : (mod > 0 ? "+" : "") + std::to_string(mod);
            logInfo(concat("front_end: levelup abilities sub '", context.gui->state("SUB_TITLE_LBL").text, "' cost ",
                           context.gui->state("COST_POINTS_LBL").visible, " '", context.gui->state("COST_POINTS_LBL").text,
                           "' mod ", context.gui->state("LBL_ABILITY_MOD").visible, " '",
                           context.gui->state("LBL_ABILITY_MOD").text, "' want '", want, "'"));
            return context.gui->state("SUB_TITLE_LBL").text == context.game->localization().tlk().text(209)
                && context.gui->state("COST_POINTS_LBL").visible && context.gui->state("COST_POINTS_LBL").text == "1"
                && context.gui->state("LBL_ABILITY_MOD").visible && context.gui->state("LBL_ABILITY_MOD").text == want;
        }));
        ops_.push_back(capture("levelup_abilities", 40));
        ops_.push_back(click("STR_PLUS_BTN"));
        ops_.push_back(wait(5));
        ops_.push_back(expect("levelup_ability_point_placed", [levelBefore](Context& context) {
            return context.game->character().str == (*levelBefore)[0] + 1 && context.game->character().attributePoints == 0;
        }));
        ops_.push_back(screenIs("abchrgen")); // no step of its own: the screen waits for Accept
        ops_.push_back(click("BTN_ACCEPT"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("maincg_leveluppnl"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("levelup_hub_lights_skills", [hubLit](Context& context) {
            return hubLit(context, 2) && context.gui->state("BTN_BACK").color.y > 0.6f;
        }));
        ops_.push_back(click("BTN_STEPNAME2"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("skchrgen"));
        ops_.push_back(wait(3));
        ops_.push_back(painted("skchrgen"));
        layout("skchrgen");
        ops_.push_back(expect("levelup_skills_show_points", [levelBefore](Context& context) {
            return context.gui->state("REMAINING_SELECTIONS_LBL").text == std::to_string((*levelBefore)[1])
                && !context.gui->state("COMPUTER_USE_LBL").text.empty()
                && context.gui->state("COM_PLUS_BTN").visible;
        }));
        // 006f4bf0: the first skill's cost and Class Skill / Cross-class Skill.
        ops_.push_back(expect("levelup_skills_cost_and_class", [](Context& context) {
            const CharacterSheet& sheet = context.game->character();
            const bool classSkill = context.game->rules().skillIsClassSkill(sheet.classId, 0);
            return context.gui->state("CLASSSKL_LBL").visible
                && context.gui->state("CLASSSKL_LBL").text == context.game->localization().tlk().text(classSkill ? 38152 : 38153)
                && context.gui->state("COST_POINTS_LBL").text == std::to_string(classSkill ? 1 : 2);
        }));
        ops_.push_back(capture("levelup_skills_more", 40));
        ops_.push_back(click("COM_PLUS_BTN"));
        ops_.push_back(wait(5));
        ops_.push_back(expect("levelup_skill_rank_placed", [levelBefore](Context& context) {
            return context.game->character().skillPoints < (*levelBefore)[1]
                && context.gui->state("REMAINING_SELECTIONS_LBL").text
                    == std::to_string(context.game->character().skillPoints);
        }));
        ops_.push_back(click("BTN_ACCEPT"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("maincg_leveluppnl"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("levelup_hub_lights_feats", [hubLit](Context& context) { return hubLit(context, 3); }));
        ops_.push_back(click("BTN_STEPNAME3"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("ftchrgen"));
        ops_.push_back(wait(3));
        ops_.push_back(painted("ftchrgen"));
        layout("ftchrgen");
        // Retail 006ce570, 006cda80: rows of three icons, not lines of text; a base feat's followers sit in its
        // row. Fails on the old list of names, whose items had no cells.
        ops_.push_back(expect("levelup_feats_are_rows_of_icons", [](Context& context) {
            const std::vector<GuiListItem> items = context.gui->state("LB_FEATS").items;
            std::size_t withFollower = 0;
            std::size_t withIcon = 0;
            for (const GuiListItem& row : items) {
                withFollower += row.cells.size() == 3 && row.cells[1].value >= 0 ? 1u : 0u;
                withIcon += !row.cells.empty() && !row.cells[0].icon.empty() ? 1u : 0u;
            }
            logInfo(concat("front_end: feat rows ", items.size(), " with a follower ", withFollower, " with an icon ", withIcon));
            return !items.empty() && withIcon == items.size() && withFollower > 0;
        }));
        ops_.push_back(expect("levelup_feats_listed", [](Context& context) {
            return !context.gui->state("LB_FEATS").items.empty()
                && context.gui->state("STD_REMAINING_SELECTIONS_LBL").text == "1";
        }));
        auto firstFeat = std::make_shared<std::string>();
        ops_.push_back(expect("levelup_feat_first_row_describes", [firstFeat](Context& context) {
            // The first cell is picked at open whether owned or not (paired cg_feats, levelup_feats); an owned one
            // offers "Remove Feat" (38456), one to take "Add Feat" (38455).
            *firstFeat = context.gui->state("LBL_NAME").text;
            const GuiControlState list = context.gui->state("LB_FEATS");
            const bool owned = !list.items.empty() && !list.items[0].cells.empty() && list.items[0].cells[0].status == 1;
            return list.listSelection == 0 && list.listCell == 0 && context.gui->state("BTN_SELECT").visible
                && context.gui->state("DESC_LBL").visible && !firstFeat->empty()
                && context.gui->state("DESC_LBL").text == context.game->localization().tlk().text(217)
                && context.gui->state("BTN_SELECT").text
                       == context.game->localization().tlk().text(owned ? 38456 : 38455) + " " + context.game->localization().tlk().text(42487);
        }));
        ops_.push_back(clickRow("LB_FEATS", 1));
        ops_.push_back(wait(5));
        ops_.push_back(expect("levelup_feat_second_row_describes_another", [firstFeat](Context& context) {
            const std::string now = context.gui->state("LBL_NAME").text;
            return !now.empty() && now != *firstFeat && context.gui->state("LB_FEATS").listSelection == 1
                && context.gui->listCellValue("LB_FEATS") >= 0;
        }));
        ops_.push_back(capture("levelup_feats_more", 40));
        auto takeableRow = std::make_shared<int>(-1);
        ops_.push_back(expect("levelup_feat_takeable_row", [takeableRow](Context& context) {
            const std::vector<GuiListItem> items = context.gui->state("LB_FEATS").items;
            for (std::size_t i = 0; i < items.size() && *takeableRow < 0; ++i) {
                if (!items[i].cells.empty() && items[i].cells[0].status == 0) {
                    *takeableRow = static_cast<int>(i);
                }
            }
            return *takeableRow >= 0;
        }));
        ops_.push_back({"levelup_feat_pick_takeable", [this, takeableRow](Context& context) {
                            return clickRow("LB_FEATS", *takeableRow).run(context);
                        }});
        ops_.push_back(wait(5));
        ops_.push_back(click("BTN_SELECT"));
        ops_.push_back(wait(5));
        ops_.push_back(expect("levelup_feat_learned", [levelBefore](Context& context) {
            const CharacterSheet& sheet = context.game->character();
            return static_cast<std::int32_t>(sheet.feats.size()) == (*levelBefore)[2] + 1 && sheet.featPoints == 0;
        }));
        ops_.push_back(click("BTN_ACCEPT"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("maincg_leveluppnl"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("levelup_hub_lights_accept", [hubLit](Context& context) { return hubLit(context, 5); }));
        ops_.push_back(click("BTN_BACK"));
        ops_.push_back(wait(4));
        ops_.push_back(expect("levelup_hub_back_steps_a_row", [hubLit](Context& context) { return hubLit(context, 3); }));
        // Lighting feats again resets them (006ee0e0 -> 006e8db0): the pick is gone and the point is back.
        ops_.push_back(expect("levelup_back_resets_feats", [levelBefore](Context& context) {
            const CharacterSheet& sheet = context.game->character();
            return static_cast<std::int32_t>(sheet.feats.size()) == (*levelBefore)[2] && sheet.featPoints == 1;
        }));
        ops_.push_back(click("BTN_STEPNAME3"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("ftchrgen"));
        ops_.push_back(click("BTN_ACCEPT"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("maincg_leveluppnl"));
        ops_.push_back(expect("levelup_hub_relights_accept", [hubLit](Context& context) { return hubLit(context, 5); }));
        ops_.push_back(click("BTN_STEPNAME5"));
        ops_.push_back(wait(5));
        ops_.push_back(expect("levelup_accept_ends", [](Context& context) {
            return context.game->activePanel() == UiPanel::None && !context.gui->active();
        }));
        ops_.push_back(expect("levelup_clear_fixture", [](Context& context) {
            context.game->character().skillPoints = 0;
            context.game->character().attributePoints = 0;
            context.game->character().featPoints = 0;
            return !context.game->canLevelUp();
        }));
        // Recommended (retail 006f7390, 006f4e20/006491a0, 006f3a80/0064a770): the class's primary ability
        // and the ranked skills, and a popup listing the recommended feats (skillinfo, 42256).
        auto recoBefore = std::make_shared<std::array<std::int32_t, 3>>();
        ops_.push_back(expect("reco_fixture", [recoBefore](Context& context) {
            CharacterSheet& sheet = context.game->character();
            sheet.attributePoints = 1;
            sheet.skillPoints = 4;
            sheet.featPoints = 1;
            (*recoBefore)[0] = sheet.str + sheet.dex + sheet.con + sheet.intel + sheet.wis + sheet.cha;
            (*recoBefore)[1] = static_cast<std::int32_t>(sheet.feats.size());
            return context.game->canLevelUp();
        }));
        ops_.push_back(key(SDL_SCANCODE_P, SDLK_P));
        ops_.push_back(screenIs("character"));
        ops_.push_back(wait(3));
        ops_.push_back(click("BTN_LEVELUP"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("maincg_leveluppnl"));
        ops_.push_back(wait(3));
        ops_.push_back(click("BTN_STEPNAME1"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("abchrgen"));
        ops_.push_back(wait(3));
        ops_.push_back(expect("reco_abilities_help_is_filled", [](Context& context) {
            const GuiControlState help = context.gui->state("LB_DESC");
            return !help.items.empty() && !help.items.front().text.empty();
        }));
        ops_.push_back(click("BTN_RECOMMENDED"));
        ops_.push_back(wait(5));
        ops_.push_back(expect("reco_ability_goes_to_the_primary", [recoBefore](Context& context) {
            const CharacterSheet& sheet = context.game->character();
            return sheet.attributePoints == 0
                && sheet.str + sheet.dex + sheet.con + sheet.intel + sheet.wis + sheet.cha == (*recoBefore)[0] + 1;
        }));
        ops_.push_back(click("BTN_ACCEPT"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("maincg_leveluppnl"));
        ops_.push_back(wait(3));
        ops_.push_back(click("BTN_STEPNAME2"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("skchrgen"));
        ops_.push_back(wait(3));
        ops_.push_back(click("BTN_RECOMMENDED"));
        ops_.push_back(wait(5));
        ops_.push_back(expect("reco_skills_spend_the_points", [](Context& context) {
            return context.game->character().skillPoints == 0;
        }));
        ops_.push_back(click("BTN_ACCEPT"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("maincg_leveluppnl"));
        ops_.push_back(wait(3));
        ops_.push_back(click("BTN_STEPNAME3"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("ftchrgen"));
        ops_.push_back(wait(3));
        ops_.push_back(click("BTN_RECOMMENDED"));
        ops_.push_back(wait(5));
        ops_.push_back(expect("reco_feat_learned_and_listed", [recoBefore](Context& context) {
            const CharacterSheet& sheet = context.game->character();
            return static_cast<std::int32_t>(sheet.feats.size()) == (*recoBefore)[1] + 1 && context.gui->modalActive()
                && context.gui->state("LBL_MESSAGE").text == context.game->localization().tlk().text(42256)
                && context.gui->state("LB_SKILLS").items.size() == 1;
        }));
        ops_.push_back(capture("levelup_feats_recommended", 40));
        ops_.push_back(click("BTN_OK"));
        ops_.push_back(wait(5));
        ops_.push_back(expect("reco_popup_closes", [](Context& context) { return !context.gui->modalActive(); }));
        ops_.push_back(click("BTN_ACCEPT"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("maincg_leveluppnl"));
        ops_.push_back(wait(3));
        ops_.push_back(click("BTN_STEPNAME5"));
        ops_.push_back(wait(5));
        ops_.push_back(expect("reco_flow_ends", [](Context& context) {
            return context.game->activePanel() == UiPanel::None && !context.gui->active();
        }));
        ops_.push_back(expect("reco_clear_fixture", [levelSaved, levelSavedLook, levelArmour](Context& context) {
            GameSession& game = *context.game;
            if (*levelArmour != script::kObjectInvalid) {
                game.unequipItemObject(game.scriptWorld().pc, *levelArmour);
                game.destroyItem(*levelArmour);
            }
            CharacterSheet sheet = *levelSaved; // the PC as before the level-up blocks, with the inventory as it is now
            sheet.inventory = game.character().inventory;
            sheet.skillPoints = 0;
            sheet.attributePoints = 0;
            sheet.featPoints = 0;
            sheet.powerPoints = 0;
            game.applyCharacter(sheet);
            game.setPlayerAppearance(*levelSavedLook);
            return !game.canLevelUp();
        }));
    }

    // ---- the granted-feats box on level-up (006ee7d0: FUN_00649950 gives every feat whose <class>_list is 3 and
    // <class>_granted is the new level, then skillinfo opens with 0xa512 over the hub). A level 2 Scoundrel given the
    // experience for level 3 is granted Sneak Attack 2d6 (feat.2da scd_granted 3). RETAIL_REQUESTS: levelup_granted.
    {
        auto grantSaved = std::make_shared<CharacterSheet>();
        auto grantFeat = std::make_shared<std::int32_t>(-1);
        ops_.push_back(expect("levelup_granted_scoundrel_fixture", [grantSaved, grantFeat](Context& context) {
            GameSession& game = *context.game;
            const Rules& rules = game.rules();
            *grantSaved = game.character();
            std::int32_t scoundrel = -1;
            for (std::int32_t id = 0; id < 6; ++id) {
                scoundrel = rules.classColumnPrefix(id) == "scd" ? id : scoundrel;
            }
            for (std::int32_t id = 0; id < rules.featCount(); ++id) {
                *grantFeat = rules.featName(id) == "SNEAK_ATTACK_2D6" ? id : *grantFeat;
            }
            if (scoundrel < 0 || *grantFeat < 0) {
                return false;
            }
            CharacterSheet sheet = rules.createCharacter(scoundrel, 2, rules.xpForLevel(2));
            sheet.name = grantSaved->name;
            sheet.gender = grantSaved->gender;
            sheet.portraitRow = grantSaved->portraitRow;
            sheet.inventory = grantSaved->inventory;
            sheet.attributePoints = 0;
            sheet.skillPoints = 0;
            sheet.featPoints = 0;
            sheet.powerPoints = 0;
            game.applyCharacter(sheet);
            game.giveXp(rules.xpForLevel(3) - game.character().xp);
            const CharacterSheet& now = game.character();
            logInfo(concat("front_end: scoundrel class ", scoundrel, " level ", now.level, " xp ", now.xp, " sneak 2d6 feat ",
                           *grantFeat, " owned ", now.feats.contains(*grantFeat)));
            return now.level == 3 && game.canLevelUp() && !now.feats.contains(*grantFeat);
        }));
        ops_.push_back(key(SDL_SCANCODE_P, SDLK_P));
        ops_.push_back(screenIs("character"));
        ops_.push_back(wait(3));
        ops_.push_back(click("BTN_LEVELUP"));
        ops_.push_back(wait(6));
        // Retail (req_levelup_l3_hub_open, req_levelup_l3_hub_open_10ms): the level 3 hub opens with no box; its rows
        // are Skills and Accept.
        ops_.push_back(expect("levelup_granted_no_box_at_open", [grantFeat](Context& context) {
            return !context.gui->modalActive() && context.gui->screenName() == "maincg_leveluppnl"
                && !context.game->character().feats.contains(*grantFeat);
        }));
        ops_.push_back(capture("levelup_granted_hub_open", 40));
        ops_.push_back(click("BTN_STEPNAME2"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("skchrgen"));
        // Retail took the Skills step with Recommended, then OK (RETAIL_REQUESTS row 22).
        ops_.push_back(click("BTN_RECOMMENDED"));
        ops_.push_back(wait(4));
        ops_.push_back(click("BTN_ACCEPT"));
        ops_.push_back(wait(4));
        ops_.push_back(screenIs("maincg_leveluppnl"));
        ops_.push_back(click("BTN_STEPNAME5"));
        ops_.push_back(wait(8));
        // Retail (req_levelup_granted): after Accept the sheet is back, showing level 3, under the box (0xa512).
        ops_.push_back(expect("levelup_granted_box_after_accept", [grantFeat](Context& context) {
            const std::vector<GuiListItem> rows = context.gui->state("LB_SKILLS").items;
            logInfo(concat("front_end: granted box screen ", context.gui->screenName(), " panel ",
                           uiPanelName(context.game->activePanel()), " rows ", rows.size(), " first '",
                           rows.empty() ? std::string{} : rows.front().text, "'"));
            return context.gui->modalActive() && context.gui->screenName() == "skillinfo"
                && context.game->activePanel() == UiPanel::Character
                && context.gui->state("LBL_MESSAGE").text == context.game->localization().tlk().text(0xa512)
                && rows.size() == 1 && rows.front().itemEntry
                && context.game->character().feats.contains(*grantFeat);
        }));
        ops_.push_back(hover("LBL_MESSAGE"));
        ops_.push_back(wait(30)); // the sheet's smoke runs a moment under the box
        ops_.push_back(capture("levelup_granted", 40));
        ops_.push_back(click("BTN_OK"));
        ops_.push_back(wait(4));
        // req_levelup_l3_after_ok: the level is taken, so 006afda0 hides Level Up and Auto Level Up (005a6810).
        ops_.push_back(expect("levelup_granted_ok_leaves_the_sheet", [](Context& context) {
            const CharacterSheet& sheet = context.game->character();
            logInfo(concat("front_end: after the granted box: points attr ", sheet.attributePoints, " skill ",
                           sheet.skillPoints, " feat ", sheet.featPoints, " power ", sheet.powerPoints, " reflex '",
                           context.gui->state("LBL_REFLEX_STAT").text, "' will '", context.gui->state("LBL_WILL_STAT").text,
                           "'"));
            return !context.gui->modalActive() && context.gui->screenName() == "character"
                && !context.gui->state("BTN_LEVELUP").visible && !context.gui->state("BTN_AUTO").visible;
        }));
        ops_.push_back(capture("levelup_granted_after_ok", 40));
        ops_.push_back(expect("levelup_granted_clear_fixture", [grantSaved](Context& context) {
            GameSession& game = *context.game;
            game.setUiPanel(UiPanel::None);
            CharacterSheet sheet = *grantSaved;
            sheet.inventory = game.character().inventory;
            game.applyCharacter(sheet);
            return !game.canLevelUp();
        }));
        ops_.push_back(wait(4));
    }

    ops_.push_back(expect("container_opens", [](Context& context) {
        const std::size_t locker = context.game->findEntity("end_locker01");
        return locker != static_cast<std::size_t>(-1) && context.game->openLoot(locker);
    }));
    ops_.push_back(screenIs("container"));
    ops_.push_back(wait(3));
    ops_.push_back(painted("container"));
    layout("container");
    ops_.push_back(expect("container_lists_loot", [](Context& context) {
        return context.gui->state("LB_ITEMS").items.size() == context.game->loot().size()
            && !context.game->loot().empty();
    }));
    ops_.push_back(capture("container", 40));
    ops_.push_back(click("BTN_OK"));
    ops_.push_back(wait(3));
    ops_.push_back(expect("container_take_all", [](Context& context) {
        return context.game->loot().empty() && !context.gui->active();
    }));

    // ---- a conversation: dialog.gui between the letterbox bars, answered by pointer
    ops_.push_back(expect("dialogue_starts", [](Context& context) { return context.game->startDialogue("end_trask"); }));
    ops_.push_back(wait(12));
    ops_.push_back(expect("dialogue_panel_bound", [this](Context& context) {
        pointToDialogue_ = true;
        const DialogueView& view = context.game->dialogue();
        if (!view.active || context.dialogueGui == nullptr) {
            return false;
        }
        // The list is up whenever a reply can be said (SetReplies 006a86a0), one reply included, and the line is
        // shown instead while it is spoken; which of the two is up depends on the moment.
        if (context.dialogueGui->state("LB_REPLIES").visible) {
            return context.dialogueGui->state("LB_REPLIES").items.size() == view.options.size();
        }
        return context.dialogueGui->state("LBL_MESSAGE").visible
            && context.dialogueGui->state("LBL_MESSAGE").text == view.text && !view.text.empty();
    }));
    ops_.push_back(capture("dialogue", 40));
    // Option 4, retail 00631d80, 006a70a0: off, a voiced line shows no text; the replies stay. The gate has no audio,
    // so the voice is told. Fails on the old screen, which always showed the line.
    ops_.push_back(expect("subtitles_off_fixture", [](Context& context) {
        context.settings->set("feedback.subtitles", 0.0f);
        context.game->setDialogueVoicedForTest(true);
        return true;
    }));
    ops_.push_back(wait(4));
    ops_.push_back(expect("subtitles_off_hides_a_voiced_line", [](Context& context) {
        const DialogueView& view = context.game->dialogue();
        const bool hidden = view.options.size() > 1
            ? !context.dialogueGui->state("LB_REPLIES").items.empty() && !context.game->dialogueLineShown()
            : context.dialogueGui->state("LBL_MESSAGE").text.empty();
        context.settings->set("feedback.subtitles", 1.0f);
        context.game->setDialogueVoicedForTest(false);
        return hidden;
    }));
    ops_.push_back(wait(4));
    // end_trask01's entry 54 pauses the conversation while Trask crosses the room (ActionPauseConversation,
    // ActionMoveToObject, ActionResumeConversation): a paused conversation takes no answer, so the click waits
    // for the resume, as a player would.
    ops_.push_back(waitUntil("dialogue_resumed", [](Context& context) {
        return !context.game->dialogue().active || !context.game->dialoguePaused();
    }));
    // The click answers a reply, so it waits until the reply list is up, the spoken line over: a click while it is spoken only skips it
    // (006a7230 input 0x27). Without the wait the click raced the entries' hold times, which run on real frame time
    // (the Vulkan run, whose frames are slower, reached entry 45 before the click and only skipped its line).
    ops_.push_back(waitUntil("dialogue_replies_up", [](Context& context) {
        return !context.game->dialogue().active
            || (context.dialogueGui != nullptr && context.dialogueGui->state("LB_REPLIES").visible
                && !context.game->dialogueLineSpeaking());
    }));
    ops_.push_back(expect("dialogue_before", [](Context& context) {
        return context.game->dialogue().active;
    }));
    auto entryBefore = std::make_shared<std::uint32_t>(0);
    ops_.push_back(expect("dialogue_entry_before", [entryBefore](Context& context) {
        *entryBefore = context.game->dialogue().entry;
        return true;
    }));
    ops_.push_back({"dialogue_click_answers", [this](Context& context) {
                        // The reply list takes the click when it is up (a single reply included), else the line.
                        const bool replies = context.dialogueGui != nullptr && context.dialogueGui->state("LB_REPLIES").visible;
                        return (replies ? clickRow("LB_REPLIES", 0) : click("LBL_MESSAGE")).run(context);
                    }});
    ops_.push_back(wait(6));
    ops_.push_back(expect("dialogue_click_advances", [this, entryBefore](Context& context) {
        pointToDialogue_ = false;
        return !context.game->dialogue().active || context.game->dialogue().entry != *entryBefore;
    }));
    ops_.push_back(expect("dialogue_ends", [](Context& context) {
        for (int guard = 0; guard < 64 && context.game->dialogue().active; ++guard) {
            context.game->chooseDialogueOption(0);
        }
        return !context.game->dialogue().active;
    }));

    // ---- a terminal: a Computer conversation (DLG ConversationType 1) is computer.gui, not the
    // dialog panel; the line and every reply are listed together and a reply is clicked.
    ops_.push_back(expect("computer_conversation_starts", [](Context& context) {
        kotor::Dlg dlg;
        dlg.conversationType = 1;
        kotor::DlgEntry first;
        first.text = "Maintenance terminal ready. Select a file to open.";
        first.replies = {0, 1};
        kotor::DlgEntry closed;
        closed.text = "The log is empty.";
        dlg.entries = {first, closed};
        kotor::DlgReply open;
        open.text = "Open the maintenance log.";
        open.entries = {1};
        kotor::DlgReply leave;
        leave.text = "Log off.";
        dlg.replies = {open, leave};
        context.game->setDialogueForTest(dlg, "gate_terminal", "Terminal");
        return context.game->dialogue().active && context.game->dialogue().computer;
    }));
    ops_.push_back(wait(12));
    ops_.push_back(expect("computer_screen_shown", [this](Context& context) {
        pointToDialogue_ = true;
        return context.dialogueGui != nullptr && context.dialogueGui->screenName() == "computer";
    }));
    ops_.push_back(painted("computer"));
    layout("computer");
    ops_.push_back(expect("computer_panel_bound", [this](Context& context) {
        pointToDialogue_ = true;
        const DialogueView& view = context.game->dialogue();
        if (!view.active || context.dialogueGui == nullptr) {
            return false;
        }
        const GuiSystem& panel = *context.dialogueGui;
        const std::string spikesLabel = context.game->localization().tlk().text(42414);
        return panel.state("LB_MESSAGE").items.size() == 1 && panel.state("LB_MESSAGE").items[0].text == view.text
            && panel.state("LB_REPLIES").items.size() == view.options.size() && view.options.size() == 2
            && !panel.state("LBL_COMP_SKILL").text.empty() && panel.state("LBL_COMP_SPIKES").text == spikesLabel
            && !panel.state("LBL_COMP_SPIKES_VAL").text.empty()
            && panel.state("LBL_REP_UNITS").text == context.game->localization().tlk().text(42415)
            && !panel.state("LBL_REP_UNITS_VAL").text.empty();
    }));
    ops_.push_back(capture("computer", 40));
    auto terminalEntry = std::make_shared<std::uint32_t>(0);
    ops_.push_back(expect("computer_entry_before", [terminalEntry](Context& context) {
        *terminalEntry = context.game->dialogue().entry;
        return true;
    }));
    ops_.push_back(clickRow("LB_REPLIES", 0));
    ops_.push_back(wait(6));
    ops_.push_back(expect("computer_reply_click_advances", [this, terminalEntry](Context& context) {
        pointToDialogue_ = false;
        return context.game->dialogue().active && context.game->dialogue().entry != *terminalEntry
            && context.game->dialogue().text == "The log is empty.";
    }));
    ops_.push_back(expect("computer_ends", [](Context& context) {
        for (int guard = 0; guard < 8 && context.game->dialogue().active; ++guard) {
            context.game->chooseDialogueOption(0);
        }
        if (context.game->dialogue().active) {
            context.game->endDialogue();
        }
        return !context.game->dialogue().active;
    }));

    // ---- a terminal's live camera feed: a Computer conversation's node with CameraAngle 6 swaps the
    // terminal for computercamera.gui (ctor 006a95f0; SetComputerCameraShot 0062c030), whose only control is
    // LBL_RETURN (strref 48226); Enter cancels the feed (006a7230) and the terminal comes back.
    ops_.push_back(expect("computer_camera_starts", [](Context& context) {
        if (context.game->scene().cameras.empty()) {
            return false;
        }
        kotor::Dlg dlg;
        dlg.conversationType = 1;
        kotor::DlgEntry feed;
        feed.cameraAngle = 6;
        feed.cameraId = static_cast<std::int32_t>(context.game->scene().cameras.front().id);
        feed.delay = 60; // seconds: longer than the gate, so only Enter ends it
        feed.replies = {0};
        kotor::DlgEntry back;
        back.text = "Feed ended. Terminal ready.";
        back.replies = {1};
        dlg.entries = {feed, back};
        kotor::DlgReply link;
        link.entries = {1};
        kotor::DlgReply leave;
        leave.text = "Log off.";
        dlg.replies = {link, leave};
        context.game->setDialogueForTest(dlg, "gate_feed", "Terminal");
        return context.game->dialogue().active && context.game->dialogue().computer;
    }));
    ops_.push_back(wait(12));
    ops_.push_back(expect("computer_camera_shown", [this](Context& context) {
        pointToDialogue_ = true;
        const DialogueView& view = context.game->dialogue();
        return view.active && view.cameraAngle == 6 && view.cameraId >= 0 && context.dialogueGui != nullptr
            && context.dialogueGui->screenName() == "computercamera"
            && context.dialogueGui->state("LBL_RETURN").text == context.game->localization().tlk().text(48226);
    }));
    ops_.push_back(painted("computercamera"));
    // 006a95f0: LBL_RETURN keeps its width and sits at x = (W - width) / 2, y = H - 75 of the window.
    ops_.push_back(expect("computercamera_return_placed", [](Context& context) {
        const GuiSystem& gui = *context.dialogueGui;
        const kotor::GuiControl* label = gui.control("LBL_RETURN");
        if (label == nullptr) {
            return false;
        }
        const GuiView& v = context.painter->view();
        const GuiPixelRect rect = guiControlPixels(v, *label, 0.0f, 0.0f);
        const float wantX = static_cast<float>((static_cast<int>(v.windowWidth) - label->extent.width) / 2);
        const float wantY = v.windowHeight - 75.0f;
        logInfo(concat("front_end: computercamera LBL_RETURN at ", rect.x0, ",", rect.y0, " want ", wantX, ",", wantY));
        return std::fabs(rect.x0 - wantX) < 0.5f && std::fabs(rect.y0 - wantY) < 0.5f;
    }));
    ops_.push_back(capture("computer_camera", 40));
    ops_.push_back(key(SDL_SCANCODE_RETURN, SDLK_RETURN));
    ops_.push_back(wait(12));
    ops_.push_back(expect("computer_camera_enter_returns", [this](Context& context) {
        pointToDialogue_ = true;
        const DialogueView& view = context.game->dialogue();
        logInfo(concat("front_end: computer_camera after Enter: active=", view.active, " entry=", view.entry, " text='",
                       view.text, "' screen=", context.dialogueGui != nullptr ? context.dialogueGui->screenName() : std::string("none")));
        return view.active && view.text == "Feed ended. Terminal ready." && context.dialogueGui != nullptr
            && context.dialogueGui->screenName() == "computer";
    }));
    ops_.push_back(expect("computer_camera_ends", [this](Context& context) {
        pointToDialogue_ = false;
        for (int guard = 0; guard < 8 && context.game->dialogue().active; ++guard) {
            context.game->chooseDialogueOption(0);
        }
        if (context.game->dialogue().active) {
            context.game->endDialogue();
        }
        return !context.game->dialogue().active;
    }));

    // ---- the upgrade bench: ShowUpgradeScreen (script action 354, retail 00543990 -> 0062e760) opens
    // upgradesel, upgradeitems and upgrade; Assemble installs the chosen upgrade (spending the upgrade
    // item and setting the item's bit, as the save stores it) and an emptied slot returns it.
    const auto packItem = [](Context& context, const char* resref) -> const InventoryItem* {
        for (const InventoryItem& item : context.game->inventory()) {
            if (item.resref.size() == std::strlen(resref)
                && std::equal(item.resref.begin(), item.resref.end(), resref,
                              [](char a, char b) { return std::tolower(static_cast<unsigned char>(a))
                                                       == std::tolower(static_cast<unsigned char>(b)); })) {
                return &item;
            }
        }
        return nullptr;
    };
    ops_.push_back(expect("upgrade_fixture", [](Context& context) {
        GameSession& game = *context.game;
        const auto give = [&](const char* resref, int stack) {
            const kotor::ItemTemplate* templ = game.upgradeTemplate(resref);
            return templ != nullptr && game.addInventoryItem(resref, templ->baseItem, stack, templ->cost);
        };
        return give("g1_w_lghtsbr01", 1) && give("g_w_sbrcrstl01", 1) && give("g_w_sbrcrstl02", 1)
            && give("g_w_blstrpstl005", 1) && give("g_i_upgrade001", 2) && give("g_i_upgrade003", 1)
            && give("g1_w_vbroswrd01", 1) && give("g_i_upgrade007", 1)
            && give("g_a_class8005", 1) && give("g_i_upgrade005", 1);
    }));
    ops_.push_back(expect("upgrade_slots_come_from_the_item_template", [packItem](Context& context) {
        GameSession& game = *context.game;
        const InventoryItem* saber = packItem(context, "g1_w_lghtsbr01");
        const InventoryItem* pistol = packItem(context, "g_w_blstrpstl005");
        const InventoryItem* sword = packItem(context, "g1_w_vbroswrd01");
        const InventoryItem* armor = packItem(context, "g_a_class8005");
        return saber != nullptr && pistol != nullptr && sword != nullptr && armor != nullptr
            && game.upgradeSlots(*saber).size() == 1 && game.upgradeSlots(*pistol).size() == 4
            && game.upgradeSlots(*sword).size() == 3 && game.upgradeSlots(*armor).size() == 2
            && game.upgradeCategory(*saber) == 0 && game.upgradeCategory(*pistol) == 1
            && game.upgradeCategory(*sword) == 2 && game.upgradeCategory(*armor) == 3;
    }));
    ops_.push_back(expect("upgrade_bench_opens_through_action_354", [](Context& context) {
        const auto result = context.game->actionAsForTest(context.game->scriptWorld().pc, 354, {});
        return result.has_value() && context.game->upgradeScreenOpen();
    }));
    ops_.push_back(screenIs("upgradesel"));
    ops_.push_back(wait(3));
    ops_.push_back(painted("upgradesel"));
    layout("upgradesel");
    ops_.push_back(expect("upgradesel_kinds_enabled_and_next_waits_for_a_pick", [](Context& context) {
        return context.gui->state("BTN_LIGHTSABER").enabled && context.gui->state("BTN_RANGED").enabled
            && context.gui->state("BTN_MELEE").enabled && context.gui->state("BTN_ARMOR").enabled
            && !context.gui->state("BTN_UPGRADEITEMS").enabled;
    }));
    ops_.push_back(capture("upgrade_kinds", 20));
    ops_.push_back(click("BTN_RANGED"));
    ops_.push_back(wait(3));
    ops_.push_back(expect("upgradesel_pick_enables_upgrade_items", [](Context& context) {
        return context.gui->state("BTN_RANGED").selected && context.gui->state("BTN_UPGRADEITEMS").enabled;
    }));
    ops_.push_back(click("BTN_UPGRADEITEMS"));
    ops_.push_back(screenIs("upgradeitems"));
    ops_.push_back(wait(3));
    ops_.push_back(painted("upgradeitems"));
    layout("upgradeitems");
    ops_.push_back(expect("upgradeitems_lists_the_ranged_weapons_with_a_description", [](Context& context) {
        const GuiControlState list = context.gui->state("LB_ITEMS");
        const auto description = context.gui->state("LB_DESCRIPTION").items;
        return list.items.size() == 1 && !description.empty() && !description[0].text.empty()
            && context.gui->state("BTN_UPGRADEITEM").enabled;
    }));
    ops_.push_back(capture("upgrade_items", 20));
    ops_.push_back(clickRow("LB_ITEMS", 0));
    ops_.push_back(wait(3));
    ops_.push_back(click("BTN_UPGRADEITEM"));
    ops_.push_back(screenIs("upgrade_ranged"));
    ops_.push_back(wait(3));
    ops_.push_back(painted("upgrade_ranged"));
    layout("upgrade_ranged");
    ops_.push_back(expect("upgrade_ranged_shows_four_slots_and_the_fitting_upgrades", [](Context& context) {
        const GuiControlState list = context.gui->state("LB_ITEMS");
        return context.gui->state("BTN_UPGRADE41").visible && context.gui->state("BTN_UPGRADE44").visible
            && !context.gui->state("BTN_UPGRADE31").visible && context.gui->state("BTN_UPGRADE41").selected
            && list.items.size() == 2 // None and the scope
            && !context.gui->state("BTN_ASSEMBLE").enabled;
    }));
    ops_.push_back(capture("upgrade_slots", 20));
    ops_.push_back(clickRow("LB_ITEMS", 1));
    ops_.push_back(wait(3));
    ops_.push_back(expect("upgrade_pick_is_pending_until_assemble", [packItem](Context& context) {
        const InventoryItem* pistol = packItem(context, "g_w_blstrpstl005");
        const InventoryItem* scope = packItem(context, "g_i_upgrade001");
        return pistol != nullptr && pistol->upgrades == 0 && scope != nullptr && scope->stack == 2
            && context.gui->state("BTN_ASSEMBLE").enabled && context.gui->state("LBL_UPGRADE_COUNT").text == "1";
    }));
    ops_.push_back(capture("upgrade_pending", 20));
    ops_.push_back(click("BTN_ASSEMBLE"));
    ops_.push_back(wait(4));
    ops_.push_back(expect("upgrade_assemble_installs_and_spends_the_upgrade_item", [packItem](Context& context) {
        const InventoryItem* pistol = packItem(context, "g_w_blstrpstl005");
        const InventoryItem* scope = packItem(context, "g_i_upgrade001");
        return pistol != nullptr && pistol->upgrades == (1u << 16) && scope != nullptr && scope->stack == 1
            && !context.gui->state("BTN_ASSEMBLE").enabled;
    }));
    ops_.push_back(click("BTN_UPGRADE43"));
    ops_.push_back(wait(3));
    ops_.push_back(clickRow("LB_ITEMS", 1));
    ops_.push_back(wait(3));
    ops_.push_back(click("BTN_ASSEMBLE"));
    ops_.push_back(wait(4));
    ops_.push_back(expect("upgrade_second_slot_installs_its_own_bit", [packItem](Context& context) {
        const InventoryItem* pistol = packItem(context, "g_w_blstrpstl005");
        return pistol != nullptr && pistol->upgrades == ((1u << 16) | (1u << 18))
            && packItem(context, "g_i_upgrade003") == nullptr
            && context.gui->state("LBL_UPGRADE_COUNT").text == "2";
    }));
    ops_.push_back(capture("upgrade_installed", 20));
    ops_.push_back(click("BTN_UPGRADE41"));
    ops_.push_back(wait(3));
    ops_.push_back(clickRow("LB_ITEMS", 0)); // None: the scope comes out
    ops_.push_back(wait(3));
    ops_.push_back(click("BTN_ASSEMBLE"));
    ops_.push_back(wait(4));
    ops_.push_back(expect("upgrade_emptying_a_slot_returns_the_upgrade_item", [packItem](Context& context) {
        const InventoryItem* pistol = packItem(context, "g_w_blstrpstl005");
        const InventoryItem* scope = packItem(context, "g_i_upgrade001");
        int scopes = 0;
        for (const InventoryItem& item : context.game->inventory()) {
            scopes += item.resref == "g_i_upgrade001" || item.resref == "G_I_UPGRADE001" ? item.stack : 0;
        }
        return pistol != nullptr && pistol->upgrades == (1u << 18) && scope != nullptr && scopes == 2;
    }));
    // Back goes one screen at a time.
    ops_.push_back(click("BTN_BACK"));
    ops_.push_back(screenIs("upgradeitems"));
    ops_.push_back(click("BTN_BACK"));
    ops_.push_back(screenIs("upgradesel"));
    // The lightsaber has its own layout (one crystal slot, the model frame, LB_DESC_LS).
    ops_.push_back(click("BTN_LIGHTSABER"));
    ops_.push_back(wait(3));
    ops_.push_back(click("BTN_UPGRADEITEMS"));
    ops_.push_back(screenIs("upgradeitems"));
    ops_.push_back(wait(3));
    ops_.push_back(clickRow("LB_ITEMS", 0));
    ops_.push_back(wait(3));
    ops_.push_back(click("BTN_UPGRADEITEM"));
    ops_.push_back(screenIs("upgrade_saber"));
    ops_.push_back(wait(3));
    ops_.push_back(painted("upgrade_saber"));
    layout("upgrade_saber");
    ops_.push_back(expect("upgrade_saber_lists_the_crystals", [](Context& context) {
        const GuiControlState list = context.gui->state("LB_ITEMS");
        return list.items.size() == 3 && context.gui->state("LB_DESC_LS").visible
            && !context.gui->state("LB_DESC").visible && !context.gui->state("BTN_UPGRADE41").visible
            && !context.gui->state("LBL_LSSLOTNAME").text.empty();
    }));
    ops_.push_back(capture("upgrade_saber", 20));
    // The list shows two rows of its three (None, crystal 01, crystal 02) at this size; the visible second
    // row is crystal 01 (the third is reached by scrolling).
    ops_.push_back(clickRow("LB_ITEMS", 1));
    ops_.push_back(wait(3));
    ops_.push_back(click("BTN_ASSEMBLE"));
    ops_.push_back(wait(4));
    ops_.push_back(expect("upgrade_saber_crystal_installs", [packItem](Context& context) {
        // The bench opens on the first saber of the pack (the bench's own choice, not the fixture's): its
        // crystal slot now holds crystal 01's row and no other row, and the pack lost that crystal.
        bool installed = false;
        for (const InventoryItem& row : context.game->inventory()) {
            if (context.game->upgradeCategory(row) != 0 || row.upgrades == 0) {
                continue;
            }
            const auto slots = context.game->upgradeSlots(row);
            const int slotRow = slots.empty() ? -1 : context.game->upgradeInstalled(row, slots.front().type);
            const auto& rows = context.game->upgradeRows();
            std::string name = slotRow >= 0 && static_cast<std::size_t>(slotRow) < rows.size()
                ? rows[static_cast<std::size_t>(slotRow)].templ : std::string{};
            for (char& c : name) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            logInfo(concat("front_end: upgrade_saber: ", row.resref, " bits ", row.upgrades, " slot row ", slotRow,
                           " template '", name, "'"));
            installed = installed || (name == "g_w_sbrcrstl01" && (row.upgrades & (row.upgrades - 1u)) == 0);
        }
        return installed && packItem(context, "g_w_sbrcrstl01") == nullptr && packItem(context, "g_w_sbrcrstl02") != nullptr;
    }));
    ops_.push_back(capture("upgrade_saber_installed", 20));
    // Escape steps back like Back; armor has two slots.
    ops_.push_back(key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE));
    ops_.push_back(screenIs("upgradeitems"));
    ops_.push_back(key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE));
    ops_.push_back(screenIs("upgradesel"));
    ops_.push_back(click("BTN_ARMOR"));
    ops_.push_back(wait(3));
    ops_.push_back(click("BTN_UPGRADEITEMS"));
    ops_.push_back(screenIs("upgradeitems"));
    ops_.push_back(wait(3));
    ops_.push_back(clickRow("LB_ITEMS", 0));
    ops_.push_back(click("BTN_UPGRADEITEM"));
    ops_.push_back(screenIs("upgrade_armor"));
    ops_.push_back(wait(3));
    ops_.push_back(expect("upgrade_armor_has_two_slots", [](Context& context) {
        return context.gui->state("BTN_UPGRADE31").visible && context.gui->state("BTN_UPGRADE32").visible
            && !context.gui->state("BTN_UPGRADE33").visible && context.gui->state("LB_ITEMS").items.size() == 2;
    }));
    ops_.push_back(capture("upgrade_armor", 20));
    ops_.push_back(key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE));
    ops_.push_back(screenIs("upgradeitems"));
    ops_.push_back(key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE));
    ops_.push_back(screenIs("upgradesel"));
    ops_.push_back(click("BTN_BACK"));
    ops_.push_back(wait(4));
    ops_.push_back(expect("upgrade_bench_closes_from_the_first_screen", [](Context& context) {
        return !context.gui->active() && !context.game->upgradeScreenOpen();
    }));

    // ---- tutorial popups: ShowTutorialWindow(1) is tutorial.2da row 40 (retail 00547340): three
    // pages (message0..2), "Continue" on the first two and "OK" on the last, shown once.
    const auto tutorialAction = [](Context& context, int window) {
        const auto result = context.game->actionAsForTest(context.game->scriptWorld().pc, 517,
                                                          {{script::Value::makeInt(window)}});
        return result.has_value();
    };
    ops_.push_back(expect("tutorial_windows_on", [](Context& context) {
        context.game->setTutorialWindowsEnabledForTest(true);
        return true;
    }));
    ops_.push_back(expect("proof_tutorial_window_5_does_nothing", [tutorialAction](Context& context) {
        return tutorialAction(context, 5) && context.game->pendingTutorialWindow() == -1;
    }));
    ops_.push_back(expect("tutorial_window_1_queues_row_40", [tutorialAction](Context& context) {
        return tutorialAction(context, 1) && context.game->pendingTutorialWindow() == 40;
    }));
    ops_.push_back(screenIs("confirm_box"));
    ops_.push_back(wait(3));
    ops_.push_back(painted("tutorial_page1"));
    layout("tutorial_page1");
    ops_.push_back(expect("tutorial_page1_text", [](Context& context) {
        const kotor::Tlk& tlk = context.game->localization().tlk();
        const auto items = context.gui->state("LB_MESSAGE").items;
        return !items.empty() && items[0].text == tlk.text(42622)
            && context.gui->state("BTN_OK").text == tlk.text(38623) && !context.gui->state("BTN_CANCEL").visible;
    }));
    ops_.push_back(expect("tutorial_pauses_the_world", [](Context& context) {
        return context.game->pendingTutorialWindow() == 40;
    }));
    ops_.push_back(capture("tutorial_page1", 40));
    ops_.push_back(click("BTN_OK"));
    ops_.push_back(wait(3));
    ops_.push_back(expect("tutorial_page2_text", [](Context& context) {
        const kotor::Tlk& tlk = context.game->localization().tlk();
        const auto items = context.gui->state("LB_MESSAGE").items;
        return !items.empty() && items[0].text == tlk.text(42623)
            && context.gui->state("BTN_OK").text == tlk.text(38623) && context.game->pendingTutorialWindow() == 40;
    }));
    ops_.push_back(click("BTN_OK"));
    ops_.push_back(wait(3));
    ops_.push_back(expect("tutorial_page3_text_and_ok", [](Context& context) {
        const kotor::Tlk& tlk = context.game->localization().tlk();
        const auto items = context.gui->state("LB_MESSAGE").items;
        return !items.empty() && items[0].text == tlk.text(42624)
            && context.gui->state("BTN_OK").text == tlk.text(1580);
    }));
    ops_.push_back(capture("tutorial_page3", 40));
    ops_.push_back(click("BTN_OK"));
    ops_.push_back(wait(4));
    ops_.push_back(expect("tutorial_closes_after_the_last_page", [](Context& context) {
        return !context.gui->active() && context.game->pendingTutorialWindow() == -1;
    }));
    ops_.push_back(expect("proof_tutorial_shown_once", [tutorialAction](Context& context) {
        return tutorialAction(context, 1) && context.game->pendingTutorialWindow() == -1;
    }));
    // The events raise the others: the status summary queues row 14 as it opens (retail 00625c60)
    // and stays on top until its OK; then the tutorial shows, and Escape answers it with its OK.
    ops_.push_back({"tutorial_row_14_on_a_journal_entry", [this](Context& context) {
                        statusSummaryLive_ = true;
                        context.game->addJournalEntry("gate_tutorial_plot", 1);
                        check(context.game->pendingTutorialWindow() == -1, "tutorial_row_14_waits_for_the_summary");
                        return true;
                    }});
    ops_.push_back(waitUntil("tutorial_row_14_summary_opens", [](Context& context) {
        return context.game->statusSummaryOpen();
    }, 2000));
    ops_.push_back(screenIs("statussummary_fit"));
    ops_.push_back(expect("tutorial_row_14_queued_under_the_summary", [](Context& context) {
        return context.game->pendingTutorialWindow() == 14;
    }));
    ops_.push_back(click("BTN_OK"));
    ops_.push_back(wait(4));
    ops_.push_back({"tutorial_row_14_summary_answered", [this](Context& context) {
                        statusSummaryLive_ = false;
                        check(!context.game->statusSummaryOpen(), "tutorial_row_14_summary_answered");
                        return true;
                    }});
    ops_.push_back(screenIs("confirm_box"));
    ops_.push_back(wait(3));
    ops_.push_back(expect("tutorial_row_14_text", [](Context& context) {
        const auto items = context.gui->state("LB_MESSAGE").items;
        return !items.empty() && items[0].text == context.game->localization().tlk().text(41882)
            && context.gui->state("BTN_OK").text == context.game->localization().tlk().text(1580);
    }));
    ops_.push_back(key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE));
    ops_.push_back(wait(4));
    ops_.push_back(expect("tutorial_escape_answers_ok", [](Context& context) {
        return !context.gui->active() && context.game->pendingTutorialWindow() == -1;
    }));
    // A panel's first opening raises its tutorial (messages: row 12, retail 00626d90) over the panel,
    // which stays drawn below it; OK returns to the panel.
    ops_.push_back(expect("tutorial_messages_panel_opens", [](Context& context) {
        context.game->setUiPanel(UiPanel::Messages);
        return true;
    }));
    ops_.push_back(wait(6));
    ops_.push_back(expect("proof_tutorial_sits_over_its_panel", [](Context& context) {
        return context.gui->screenName() == "confirm_box" && context.gui->underlayCount() == 1
            && context.game->pendingTutorialWindow() == 12;
    }));
    ops_.push_back(key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE));
    ops_.push_back(wait(4));
    ops_.push_back(expect("tutorial_closes_to_its_panel", [](Context& context) {
        const bool back = context.gui->active() && context.gui->screenName() == "messages"
            && !context.gui->modalActive() && context.game->pendingTutorialWindow() == -1;
        if (back) {
            context.game->setTutorialWindowsEnabledForTest(false);
            context.game->setUiPanel(UiPanel::None);
        }
        return back;
    }));
    ops_.push_back(wait(4));

    // Escape with nothing open is the pause menu: the in-game options. The conversation that
    // just ended can restart from its owner's script; wait for it to settle.
    ops_.push_back(expect("conversation_settled", [](Context& context) {
        if (context.game->dialogue().active) {
            context.game->endDialogue();
        }
        return true;
    }));
    ops_.push_back(wait(20));
    ops_.push_back(expect("no_conversation_before_escape", [](Context& context) {
        if (context.game->dialogue().active) {
            context.game->endDialogue();
        }
        return !context.game->dialogue().active;
    }));
    ops_.push_back(capture("world_before_pause", 40));
    ops_.push_back(expect("world_frame_kept", [this](Context&) {
        worldFrame_ = lastFrame_;
        return !worldFrame_.rgba.empty();
    }));
    // The world clock runs while play is on, and stands still while the pause menu is up.
    auto clock = std::make_shared<float>(0.0f);
    ops_.push_back(expect("world_clock_mark", [clock](Context& context) {
        *clock = context.game->elapsedSeconds();
        return true;
    }));
    // Frames are not seconds: 12 frames were 90 ms at the usual pace and under the 50 ms below when frames ran fast.
    auto clockStart = std::make_shared<std::uint64_t>(0);
    ops_.push_back(expect("world_clock_timer", [clockStart](Context&) {
        *clockStart = SDL_GetTicks();
        return true;
    }));
    ops_.push_back(waitUntil("world_clock_waited", [clockStart](Context&) { return SDL_GetTicks() - *clockStart > 200; },
                             100000));
    ops_.push_back(expect("proof_world_clock_runs_in_play", [clock](Context& context) {
        const bool runs = context.game->elapsedSeconds() > *clock + 0.05f;
        *clock = context.game->elapsedSeconds();
        return runs;
    }));
    ops_.push_back(key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE));
    ops_.push_back(screenIs("optionsingame"));
    ops_.push_back(painted("optionsingame"));
    layout("optionsingame");
    ops_.push_back(capture("optionsingame", 40));
    ops_.push_back(expect("pause_mark", [clock](Context& context) {
        *clock = context.game->elapsedSeconds();
        return true;
    }));
    ops_.push_back(wait(30));
    ops_.push_back(expect("pause_menu_freezes_world", [clock](Context& context) {
        return std::fabs(context.game->elapsedSeconds() - *clock) < 1.0e-4f && context.gui->screenName() == "optionsingame";
    }));
    widgets("optionsingame");
    {
        // A save made from the pause menu stores the world's frame as its Screen.tga (the
        // fixture save above has only the synthetic gradient thumbnail), the screen shows
        // "Planet" and "Area" on two lines, and the three party boxes are drawn.
        ops_.push_back(click("BTN_SAVEGAME"));
        ops_.push_back(screenIs("saveload"));
        ops_.push_back(clickRow("LB_GAMES", 0)); // New Slot
        ops_.push_back(wait(2));
        ops_.push_back(click("BTN_SAVELOAD"));
        ops_.push_back(wait(8));
        ops_.push_back(screenIs("optionsingame"));
        ops_.push_back(expect("save_screenshot_is_the_world", [this](Context& context) {
            std::filesystem::path folder;
            for (const FrontEnd::SaveSlot& slot : context.front->listSaves(savesDir_)) {
                if (slot.name.rfind("Game", 0) == 0 && slot.name != "Gate Save") {
                    folder = slot.folder; // the last one: folder order, newest at the end (006cc160)
                }
            }
            if (folder.empty() || worldFrame_.rgba.empty()) {
                logInfo("front_end: no in-game save slot or no world frame");
                return false;
            }
            std::ifstream file(folder / "Screen.tga", std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                                  std::istreambuf_iterator<char>());
            const auto meanDiff = [](const kotor::Image& a, const kotor::Image& b) {
                if (a.width != b.width || a.height != b.height || a.rgba.empty()) {
                    return 255.0;
                }
                double total = 0.0;
                std::size_t count = 0;
                for (std::size_t i = 0; i + 3 < a.rgba.size(); i += 4) {
                    for (std::size_t c = 0; c < 3; ++c) {
                        total += std::abs(static_cast<int>(a.rgba[i + c]) - static_cast<int>(b.rgba[i + c]));
                        ++count;
                    }
                }
                return total / static_cast<double>(std::max<std::size_t>(1, count));
            };
            try {
                const kotor::Image saved = kotor::decodeTga(bytes, "Screen.tga");
                const kotor::Image expected = kotor::decodeTga(
                    kotor::encodeSaveScreenshot(worldFrame_.rgba.data(), worldFrame_.width, worldFrame_.height), "world");
                const kotor::Image synthetic = kotor::decodeTga(kotor::makeSaveThumbnail(), "synthetic");
                const double toWorld = meanDiff(saved, expected);
                const double worldToSynthetic = meanDiff(expected, synthetic);
                const double toSynthetic = meanDiff(saved, synthetic);
                logInfo(concat("front_end: Screen.tga ", saved.width, "x", saved.height, " mean diff to the world frame ",
                               toWorld, ", to the synthetic gradient ", toSynthetic));
                check(worldToSynthetic > 25.0, "proof_synthetic_thumbnail_is_distinguishable");
                return saved.width == 256 && saved.height == 256 && toWorld < 12.0 && toSynthetic > 25.0;
            } catch (const kotor::FormatError&) {
                return false;
            }
        }));
        ops_.push_back(click("BTN_LOADGAME"));
        ops_.push_back(screenIs("saveload"));
        ops_.push_back(clickLastRow("LB_GAMES")); // the save just made: newest, last (006cc160)
        ops_.push_back(wait(4));
        ops_.push_back(painted("saveload_real"));
        ops_.push_back(expect("save_planet_and_area_split", [](Context& context) {
            const std::string planet = context.gui->state("LBL_PLANETNAME").text;
            const std::string area = context.gui->state("LBL_AREANAME").text;
            logInfo("front_end: save labels planet='" + planet + "' area='" + area + "'");
            return !planet.empty() && !area.empty() && planet.find(" - ") == std::string::npos
                && area.find(" - ") == std::string::npos;
        }));
        ops_.push_back(expect("save_party_boxes_drawn", [](Context& context) {
            return context.gui->state("LBL_PM1").visible && context.gui->state("LBL_PM2").visible
                && context.gui->state("LBL_PM3").visible && context.gui->state("LBL_SCREENSHOT").visible;
        }));
        ops_.push_back(capture("saveload_real", 50, true));
        ops_.push_back(click("BTN_BACK"));
        ops_.push_back(screenIs("optionsingame"));
    }
    {
        // Live window resize: the same screen at a different size and aspect, then back.
        int launchW = 0;
        int launchH = 0;
        if (window_ != nullptr) {
            SDL_GetWindowSizeInPixels(window_, &launchW, &launchH);
        }
        const bool wide = launchH > 0 && static_cast<float>(launchW) / static_cast<float>(launchH) > 1.6f;
        const int altW = wide ? 1280 : 1920;
        const int altH = wide ? 1024 : 1080;
        ops_.push_back(setWindow(altW, altH, false));
        ops_.push_back(windowIs("live_resize_follows", [altW, altH](int w, int h) { return w == altW && h == altH; }));
        ops_.push_back(painted("optionsingame_resized"));
        layout("optionsingame_resized");
        ops_.push_back(capture("optionsingame_resized", 40));
        ops_.push_back(setWindow(launchW, launchH, false));
        ops_.push_back(windowIs("live_resize_back", [launchW, launchH](int w, int h) { return w == launchW && h == launchH; }));
        ops_.push_back(painted("optionsingame_restored"));
        layout("optionsingame_restored");

        // The options screen applies a resolution and fullscreen through real clicks.
        ops_.push_back(click("BTN_GRAPHICS"));
        ops_.push_back(screenIs("optgraphics"));
        ops_.push_back(click("BTN_RESOLUTION"));
        ops_.push_back(screenIs("optresolution"));
        ops_.push_back(painted("optresolution"));
        layout("optresolution");
        ops_.push_back(expect("optresolution_lists_modes", [](Context& context) {
            return context.gui->state("LB_RESOLUTIONS").items.size() >= 4;
        }));
        ops_.push_back(capture("optresolution", 40, true));
    widgets("optresolution");
        ops_.push_back(selectResolution("800x600"));
        ops_.push_back(click("BTN_OK"));
        ops_.push_back(screenIs("optgraphics"));
        ops_.push_back(windowIs("options_applies_800x600", [](int w, int h) { return w == 800 && h == 600; }));
        layout("optgraphics_800x600");
        ops_.push_back(capture("optgraphics_800x600", 40, true));
        ops_.push_back(click("BTN_RESOLUTION"));
        ops_.push_back(screenIs("optresolution"));
        ops_.push_back(selectResolution("Fullscreen"));
        ops_.push_back(click("BTN_OK"));
        ops_.push_back(screenIs("optgraphics"));
        ops_.push_back(windowIs("options_applies_fullscreen", [](int, int) { return true; }, 1));
        layout("optgraphics_fullscreen");
        ops_.push_back(capture("optgraphics_fullscreen", 40, true));
        ops_.push_back(setWindow(launchW, launchH, false));
        ops_.push_back(windowIs("window_restored", [launchW, launchH](int w, int h) { return w == launchW && h == launchH; }, 0));
        ops_.push_back(click("BTN_BACK"));
        ops_.push_back(screenIs("optionsingame"));
    }
    ops_.push_back(click("BTN_EXIT"));
    ops_.push_back(wait(3));
    ops_.push_back(expect("pause_menu_resumes", [](Context& context) { return !context.gui->active(); }));

    // The settings the screens changed were written.
    ops_.push_back(expect("settings_dirty_flushed", [](Context& context) { return !context.settings->dirty; }));

    // ---- Pazaak: PlayPazaak (action 364) opens the shipped pazaakwager, pazaaksetup and pazaakgame
    // screens in turn (GameScreensPazaak.cpp). Every step below is a pointer click on a control of
    // those screens; a whole match is played that way, result popups included.
    ops_.push_back(expect("pazaak_fixture", [](Context& context) {
        GameSession& game = *context.game;
        game.setGoldForTest(500);
        // A named PC: LBL_PLRNAME is the full name (OnPanelAdded 0067ffa0, 005a4c60), not a class.
        CharacterSheet named = game.character();
        named.name = "Triff Avery";
        game.setCharacterForTest(named);
        // A new party owns two each of +1..+5 (PARTYTABLE ClearTable 00563200). Card items given to the
        // party become counts (AddItem 0055d330): -1 x2, +/-2, +/-3.
        const std::pair<const char*, int> cards[] = {{"g_i_pazcard_007", 2}, {"g_i_pazcard_014", 1},
                                                     {"g_i_pazcard_015", 1}};
        bool all = game.pazaakCardsOwned(0) == 2 && game.pazaakCardsOwned(4) == 2 && game.pazaakCardsOwned(6) == 0;
        for (const auto& [resref, count] : cards) {
            all = all && game.absorbPazaakCard(resref, 86, count);
        }
        context.screens->setPazaakOpponentDelay(6);
        return all && game.pazaakCardsOwned(6) == 2 && game.pazaakCardsOwned(13) == 1 && game.pazaakCardsOwned(14) == 1;
    }));
    ops_.push_back(expect("pazaak_play_action_364_opens_the_wager_screen", [](Context& context) {
        const std::uint32_t pc = context.game->scriptWorld().pc;
        const auto result = context.game->actionAsForTest(
            pc, 364,
            {{script::Value::makeInt(2)}, {script::Value::makeString("")}, {script::Value::makeInt(50)},
             {script::Value::makeInt(0)}, {script::Value::makeObject(pc)}});
        return result.has_value() && context.game->pazaakStage() == GameSession::PazaakStage::Wager;
    }));
    // The wager popup is over the side deck panel (OnPanelAdded 0067f750 adds it, 0067f795), not a
    // screen of its own (paired frame req_pazaak_wager_open).
    ops_.push_back(waitUntil("pazaak_wager_popup_over_the_side_deck", [](Context& context) {
        return context.gui->screenName() == "pazaakwager" && context.gui->modalActive()
            && context.screens != nullptr && context.screens->screen() == "pazaaksetup";
    }, 120));
    ops_.push_back(wait(3));
    ops_.push_back(painted("pazaakwager"));
    layout("pazaakwager");
    // 0067f000: the wager opens at the maximum; LBL_MAXIMUM is 0x7e41 max, a new line, 0x96c8 gold.
    ops_.push_back(expect("pazaak_wager_text", [](Context& context) {
        const auto& tlk = context.game->localization().tlk();
        return context.gui->state("LBL_MAXIMUM").text == tlk.text(32321) + " 50\n" + tlk.text(38600) + " 500"
            && context.gui->state("LBL_WAGERVAL").text == "50";
    }));
    ops_.push_back(capture("pazaak_wager", 20));
    ops_.push_back(click("BTN_MORE"));
    ops_.push_back(wait(3));
    ops_.push_back(expect("pazaak_wager_stops_at_the_maximum", [](Context& context) {
        return context.game->pazaakWager() == 50 && context.gui->state("LBL_WAGERVAL").text == "50";
    }));
    ops_.push_back(click("BTN_LESS"));
    ops_.push_back(wait(3));
    // 0067e150: one at a time.
    ops_.push_back(expect("pazaak_wager_less_steps_by_one", [](Context& context) {
        return context.game->pazaakWager() == 49 && context.gui->state("LBL_WAGERVAL").text == "49";
    }));
    ops_.push_back(click("BTN_WAGER"));
    ops_.push_back(screenIs("pazaaksetup"));
    ops_.push_back(wait(3));
    ops_.push_back(painted("pazaaksetup"));
    layout("pazaaksetup");
    // OnPanelAdded 0067f750: only owned kinds show; BTN_YTEXT is never bound; Play needs ten.
    ops_.push_back(expect("pazaak_setup_lists_the_pack", [](Context& context) {
        return context.gui->state("BTN_AVAIL00").visible && context.gui->state("LBL_AVAILNUM00").text == "2"
            && context.gui->state("BTN_AVAIL10").visible && context.gui->state("BTN_AVAIL21").visible
            && !context.gui->state("BTN_AVAIL05").visible && !context.gui->state("BTN_AVAIL15").visible
            && !context.gui->state("BTN_AVAIL20").visible && !context.gui->state("BTN_YTEXT").visible
            && !context.gui->state("BTN_ATEXT").enabled;
    }));
    ops_.push_back(capture("pazaak_setup_empty", 20));
    // HandleMoveCard 006807e0: a click moves the card.
    const auto addCard = [this](const char* avail) {
        ops_.push_back(click(avail));
        ops_.push_back(wait(2));
    };
    addCard("BTN_AVAIL00");
    addCard("BTN_AVAIL00");
    addCard("BTN_AVAIL01");
    ops_.push_back(wait(3));
    ops_.push_back(expect("pazaak_setup_moves_cards_to_the_deck", [](Context& context) {
        const std::vector<int>& chosen = context.game->pazaakChosen();
        return chosen == std::vector<int>{0, 0, 1} && context.gui->state("BTN_AVAIL00").visible
            && !context.gui->state("BTN_AVAIL00").enabled && context.gui->state("LBL_AVAILNUM00").text == "0"
            && context.gui->state("BTN_AVAIL01").visible && context.gui->state("LBL_AVAILNUM01").text == "1"
            && context.gui->state("LBL_CHOSEN0").text == "+1";
    }));
    ops_.push_back(capture("pazaak_setup", 20));
    addCard("BTN_AVAIL01");
    addCard("BTN_AVAIL02");
    addCard("BTN_AVAIL03");
    addCard("BTN_AVAIL10");
    addCard("BTN_AVAIL10");
    addCard("BTN_AVAIL21");
    addCard("BTN_AVAIL22");
    ops_.push_back(wait(3));
    ops_.push_back(expect("pazaak_setup_deck_is_ten", [](Context& context) {
        return context.game->pazaakChosen().size() == 10 && context.gui->state("BTN_ATEXT").enabled;
    }));
    ops_.push_back(click("BTN_CHOSEN9"));
    ops_.push_back(wait(3));
    ops_.push_back(expect("pazaak_setup_chosen_card_goes_back", [](Context& context) {
        return context.game->pazaakChosen().size() == 9 && context.gui->state("BTN_AVAIL22").enabled
            && !context.gui->state("BTN_ATEXT").enabled;
    }));
    addCard("BTN_AVAIL22");
    ops_.push_back(wait(3));
    ops_.push_back(expect("pazaak_setup_deck_is_ten_again", [](Context& context) {
        return context.game->pazaakChosen().size() == 10;
    }));
    ops_.push_back(capture("pazaak_setup_full", 20));
    ops_.push_back(click("BTN_ATEXT"));
    ops_.push_back(waitUntil("pazaak_deck_question_popup", [](Context& context) {
        const auto items = context.gui->state("LB_MESSAGE").items;
        return context.gui->modalActive() && !items.empty()
            && items[0].text == context.game->localization().tlk().text(32322);
    }, 60));
    ops_.push_back(wait(4));
    layout("pazaak_ask");
    ops_.push_back(capture("pazaak_deck_question", 20));
    ops_.push_back(click("BTN_OK"));
    ops_.push_back(screenIs("pazaakgame"));
    ops_.push_back(wait(3));
    ops_.push_back(painted("pazaakgame"));
    layout("pazaakgame");
    // DoGameSequence 00680030: the player's turn opens the match after the 0.4 s start-turn wait.
    ops_.push_back(waitUntil("pazaak_first_draw", [](Context& context) {
        const PazaakGame* game = context.game->pazaak();
        return game != nullptr && game->awaitingPlayer() && game->player().tableCount() == 1;
    }, 240));
    ops_.push_back(wait(3));
    ops_.push_back(expect("pazaak_table_starts_a_player_turn", [](Context& context) {
        const PazaakGame* game = context.game->pazaak();
        const std::string drawn = game != nullptr ? std::to_string(game->player().table[0].value()) : std::string{};
        return game != nullptr && game->opponent().tableCount() == 0
            && game->player().handCount() == 4 && game->opponent().handCount() == 4
            && context.gui->state("BTN_PLRSIDE0").visible && context.gui->state("BTN_NPCSIDE3").visible
            && context.gui->state("BTN_PLR0").visible && !context.gui->state("BTN_PLR1").visible
            && context.gui->state("LBL_PLR0").text == drawn // SetCard 0067cd30: a main card has no sign
            && context.gui->state("LBL_PLRTOTAL").text == std::to_string(game->playerTotal())
            && context.gui->state("LBL_PLRNAME").text == "Triff Avery"
            && context.gui->state("LBL_PLRTURN").visible && !context.gui->state("LBL_NPCTURN").visible;
    }));
    ops_.push_back(capture("pazaak_game", 40));
    // Escape at the table asks 0xa5b9 (0067e8f0); Cancel goes back to the same turn (paired frame
    // req_pazaak_match_esc).
    ops_.push_back(key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE));
    ops_.push_back(waitUntil("pazaak_table_escape_asks_0xa5b9", [](Context& context) {
        const auto items = context.gui->state("LB_MESSAGE").items;
        return context.gui->modalActive() && !items.empty()
            && items[0].text == context.game->localization().tlk().text(42425);
    }));
    ops_.push_back(wait(4));
    layout("pazaak_ask");
    ops_.push_back(capture("pazaak_match_esc", 40));
    ops_.push_back(click("BTN_CANCEL"));
    ops_.push_back(waitUntil("pazaak_table_escape_cancel_resumes", [](Context& context) {
        const PazaakGame* game = context.game->pazaak();
        return !context.gui->modalActive() && context.gui->screenName() == "pazaakgame" && game != nullptr
            && game->awaitingPlayer() && game->player().tableCount() == 1;
    }));
    ops_.push_back(wait(3));
    ops_.push_back(click("BTN_PLRSIDE0"));
    ops_.push_back(wait(3));
    ops_.push_back(expect("pazaak_hand_card_click_plays_it", [](Context& context) {
        const PazaakGame* game = context.game->pazaak();
        return game != nullptr && game->player().tableCount() == 2 && game->player().handCount() == 3
            && game->handCardPlayed() && context.gui->state("BTN_PLR1").visible
            && !context.gui->state("BTN_PLRSIDE0").visible && !context.gui->state("BTN_PLRSIDE1").enabled;
    }));
    ops_.push_back(capture("pazaak_hand_card_played", 40));

    // A +/- card in the hand: its flip button (pazflip) changes the card's sign. The hand is the seeded
    // deal, so the card is looked up; with none dealt this step is skipped and says so.
    auto flipped = std::make_shared<std::array<int, 3>>(); // index, value before, found
    ops_.push_back({"pazaak_flip_button_flips_the_card", [this, flipped](Context& context) {
                        const PazaakGame* game = context.game->pazaak();
                        if (age_ == 0) {
                            *flipped = {-1, 0, 0};
                            for (std::size_t i = 0; game != nullptr && i < game->player().hand.size(); ++i) {
                                if (game->player().hand[i].flip() && (*flipped)[0] < 0) {
                                    *flipped = {static_cast<int>(i), game->player().hand[i].value(), 1};
                                }
                            }
                            if ((*flipped)[0] < 0) {
                                logInfo("front_end: pazaak: no +/- card in the dealt hand, flip click skipped");
                                return true;
                            }
                            if (!controlPoint(context, "BTN_FLIP" + std::to_string((*flipped)[0]), 0.5f, 0.5f, px_, py_)) {
                                check(false, "pazaak_flip_target");
                                return true;
                            }
                            pushMotion(px_, py_);
                        } else if (age_ == 2) {
                            pushButton(px_, py_, true);
                        } else if (age_ == 4) {
                            pushButton(px_, py_, false);
                        } else if (age_ >= 8) {
                            check(game != nullptr && game->player().hand[static_cast<std::size_t>((*flipped)[0])].value()
                                      == -(*flipped)[1],
                                  "pazaak_flip_button_flips_the_card");
                            return true;
                        }
                        return false;
                    }});
    ops_.push_back(capture("pazaak_flipped", 40));

    // The match: a player who stands at 17 or more, plays a hand card that lands on 17..20 (flipping
    // a +/- card first when its other sign does), and otherwise ends the turn; popups answered by OK.
    // Each click is a pointer move, press and release on the control.
    const auto pickTag = [](Context& context) -> std::string {
        if (context.gui->modalActive()) {
            return "BTN_OK";
        }
        const PazaakGame* game = context.game->pazaak();
        if (context.gui->screenName() != "pazaakgame" || game == nullptr || !game->awaitingPlayer()) {
            return {};
        }
        const PazaakGame::Side& me = game->player();
        const int total = me.total();
        const bool played = game->handCardPlayed();
        const auto lands = [&](int value) { return total + value >= 17 && total + value <= 20; };
        if (!played && total < 17) {
            for (std::size_t i = 0; i < me.hand.size(); ++i) {
                if (!me.hand[i].present()) {
                    continue;
                }
                if (lands(me.hand[i].value())) {
                    return "BTN_PLRSIDE" + std::to_string(i);
                }
                if (me.hand[i].flip() && lands(-me.hand[i].value())) {
                    return "BTN_FLIP" + std::to_string(i);
                }
            }
        }
        if (!played && total > 20) {
            for (std::size_t i = 0; i < me.hand.size(); ++i) {
                if (!me.hand[i].present()) {
                    continue;
                }
                if (total + me.hand[i].value() <= 20) {
                    return "BTN_PLRSIDE" + std::to_string(i);
                }
                if (me.hand[i].flip() && total - me.hand[i].value() <= 20) {
                    return "BTN_FLIP" + std::to_string(i);
                }
            }
        }
        return total >= 17 && total <= 20 ? "BTN_YTEXT" : "BTN_XTEXT";
    };
    struct Clicker {
        std::string tag;
        int frame = 0;
        int clicks = 0;
        std::map<std::string, int> byTag;
    };
    auto clicker = std::make_shared<Clicker>();
    const auto playUntil = [this, pickTag, clicker](const std::string& label, std::function<bool(Context&)> stop) {
        ops_.push_back({label, [this, label, pickTag, clicker, stop](Context& context) {
                            if (clicker->tag.empty() && stop(context)) {
                                check(true, label);
                                return true;
                            }
                            if (age_ >= 6000) {
                                logInfo(concat("front_end: pazaak stuck: phase ",
                                               context.game->pazaak() != nullptr
                                                   ? static_cast<int>(context.game->pazaak()->phase()) : -1,
                                               " screen ", context.gui->screenName()));
                                check(false, label);
                                return true;
                            }
                            if (clicker->tag.empty()) {
                                clicker->tag = pickTag(context);
                                clicker->frame = 0;
                                if (clicker->tag.empty()) {
                                    return false; // the opponent's turn: nothing to click yet
                                }
                            }
                            if (clicker->frame == 0) {
                                if (!controlPoint(context, clicker->tag, 0.5f, 0.5f, px_, py_)) {
                                    check(false, label + "_target_" + clicker->tag);
                                    return true;
                                }
                                pushMotion(px_, py_);
                            } else if (clicker->frame == 2) {
                                pushButton(px_, py_, true);
                            } else if (clicker->frame == 4) {
                                pushButton(px_, py_, false);
                            } else if (clicker->frame >= 8) {
                                ++clicker->clicks;
                                ++clicker->byTag[clicker->tag.rfind("BTN_PLRSIDE", 0) == 0 ? "hand" : clicker->tag];
                                clicker->tag.clear();
                                return false;
                            }
                            ++clicker->frame;
                            return false;
                        }});
    };
    playUntil("pazaak_first_set_ends_by_clicks", [](Context& context) {
        const PazaakGame* game = context.game->pazaak();
        return game != nullptr && game->phase() == PazaakGame::Phase::SetOver && context.gui->modalActive();
    });
    ops_.push_back(wait(4));
    ops_.push_back(expect("pazaak_set_result_text", [](Context& context) {
        const auto& tlk = context.game->localization().tlk();
        const auto items = context.gui->state("LB_MESSAGE").items;
        const PazaakGame* game = context.game->pazaak();
        const std::uint32_t want = game->lastSet() == PazaakGame::SetResult::Player ? 32334
            : (game->lastSet() == PazaakGame::SetResult::Opponent ? 32335 : 32338);
        return !items.empty() && items[0].text == tlk.text(want);
    }));
    // FixMessageLabel 006253a0 grows the box a line while the text does not fit at the PROTOITEM width
    // (258): "The opponent wins the set." (261 wide) gets 290x79 at y 200, a one-line result 290x63
    // (paired frames req_pazaak_set_result, req_pazaak_match_result).
    ops_.push_back(expect("pazaak_set_result_box_fitted_to_its_text", [](Context& context) {
        const kotor::GuiScreen* box = context.gui->screen();
        const PazaakGame* game = context.game->pazaak();
        const bool twoLines = game->lastSet() == PazaakGame::SetResult::Opponent;
        const kotor::GuiControl* list = context.gui->control("LB_MESSAGE");
        const bool noScrollbar = list != nullptr
            && std::none_of(list->children.begin(), list->children.end(),
                            [](const kotor::GuiControl& child) { return child.type == 9; });
        return box != nullptr && box->extent.width == 290 && box->extent.height == (twoLines ? 79 : 63)
            && box->extent.top == (twoLines ? 200 : 208) && noScrollbar;
    }));
    layout("pazaak_notice");
    ops_.push_back(capture("pazaak_set_result", 40));
    playUntil("pazaak_match_ends_by_clicks", [](Context& context) {
        const PazaakGame* game = context.game->pazaak();
        return game != nullptr && game->phase() == PazaakGame::Phase::MatchOver && context.gui->modalActive();
    });
    ops_.push_back(wait(4));
    auto matchSnapshot = std::make_shared<std::array<int, 4>>(); // player sets, opponent sets, won, gold
    ops_.push_back(expect("pazaak_match_result_text", [matchSnapshot](Context& context) {
        const auto& tlk = context.game->localization().tlk();
        const auto items = context.gui->state("LB_MESSAGE").items;
        const PazaakGame* game = context.game->pazaak();
        *matchSnapshot = {game->playerSets(), game->opponentSets(), game->playerWonMatch() ? 1 : 0,
                          static_cast<int>(context.game->gold())};
        return !items.empty() && items[0].text == tlk.text(game->playerWonMatch() ? 32336 : 32337)
            && (game->playerSets() == 3) != (game->opponentSets() == 3);
    }));
    ops_.push_back(expect("pazaak_match_result_box_one_line", [](Context& context) {
        // Only the defeat box has a retail frame; a win's longer text grows as the rule says.
        const kotor::GuiScreen* box = context.gui->screen();
        if (context.game->pazaak()->playerWonMatch()) {
            return box != nullptr && box->extent.width == 290 && box->extent.height >= 63;
        }
        return box != nullptr && box->extent.width == 290 && box->extent.height == 63 && box->extent.top == 208;
    }));
    ops_.push_back(capture("pazaak_match_result", 40));
    playUntil("pazaak_screens_close_after_the_match", [](Context& context) {
        return !context.game->pazaakActive() && !context.gui->active();
    });
    ops_.push_back(expect("pazaak_wager_paid_and_result_kept", [matchSnapshot](Context& context) {
        const int won = (*matchSnapshot)[2];
        return context.game->lastPazaakResult() == won && context.game->gold() == (*matchSnapshot)[3] + (won ? 49 : -49);
    }));
    ops_.push_back(expect("pazaak_played_only_by_clicks", [clicker](Context&) {
        logInfo(concat("front_end: pazaak clicks=", clicker->clicks, " end turn=", clicker->byTag["BTN_XTEXT"],
                       " stand=", clicker->byTag["BTN_YTEXT"], " hand=", clicker->byTag["hand"],
                       " flip=", clicker->byTag["BTN_FLIP0"] + clicker->byTag["BTN_FLIP1"] + clicker->byTag["BTN_FLIP2"]
                           + clicker->byTag["BTN_FLIP3"],
                       " ok=", clicker->byTag["BTN_OK"]));
        return clicker->clicks >= 10 && clicker->byTag["BTN_OK"] >= 3;
    }));
    // A second PlayPazaak, left by Escape at the wager: HandleQuitDialog 0067d3d0 asks 0xa5b8 and
    // its OK leaves without a match (paired frame req_pazaak_wager_esc).
    ops_.push_back(expect("pazaak_again_opens_the_wager_screen", [](Context& context) {
        const std::uint32_t pc = context.game->scriptWorld().pc;
        const auto result = context.game->actionAsForTest(
            pc, 364,
            {{script::Value::makeInt(2)}, {script::Value::makeString("")}, {script::Value::makeInt(50)},
             {script::Value::makeInt(0)}, {script::Value::makeObject(pc)}});
        return result.has_value() && context.game->pazaakStage() == GameSession::PazaakStage::Wager;
    }));
    ops_.push_back(waitUntil("pazaak_wager_popup_again", [](Context& context) {
        return context.gui->screenName() == "pazaakwager" && context.gui->modalActive();
    }));
    ops_.push_back(wait(3));
    ops_.push_back(key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE));
    ops_.push_back(waitUntil("pazaak_wager_escape_asks_0xa5b8", [](Context& context) {
        const auto items = context.gui->state("LB_MESSAGE").items;
        return context.gui->modalActive() && !items.empty()
            && items[0].text == context.game->localization().tlk().text(42424);
    }));
    ops_.push_back(wait(4));
    layout("pazaak_ask");
    ops_.push_back(capture("pazaak_wager_esc", 40));
    // 0067d3d0 adds the box over the wager popup; Cancel leaves the popup where it was.
    ops_.push_back(click("BTN_CANCEL"));
    ops_.push_back(waitUntil("pazaak_wager_escape_cancel_back_to_the_wager", [](Context& context) {
        return context.gui->screenName() == "pazaakwager" && context.gui->modalActive()
            && context.game->pazaakStage() == GameSession::PazaakStage::Wager;
    }));
    ops_.push_back(wait(3));
    ops_.push_back(key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE));
    ops_.push_back(waitUntil("pazaak_wager_escape_asks_again", [](Context& context) {
        const auto items = context.gui->state("LB_MESSAGE").items;
        return context.gui->modalActive() && !items.empty()
            && items[0].text == context.game->localization().tlk().text(42424);
    }));
    ops_.push_back(wait(3));
    ops_.push_back(click("BTN_OK"));
    ops_.push_back(waitUntil("pazaak_wager_escape_ok_leaves", [](Context& context) {
        return !context.game->pazaakActive() && !context.gui->active();
    }));

    // ---- game over: the whole party down. After kPartyWipeSeconds of play a message box says so
    // (ENGINE CHOICE for text and timing, see GameSession::updatePartyWipe); its OK ends the game as
    // EndGame does and the UI is back at the main menu, where Load Game is.
    ops_.push_back(expect("party_wipe_fixture", [](Context& context) {
        context.game->killPartyForTest();
        return context.game->partyWiped() && !context.game->gameOverMessage();
    }));
    // kPartyWipeSeconds is a time; the frames allowed here must cover it at any pace (the window can run uncapped).
    ops_.push_back(waitUntil("game_over_message_after_the_wipe_time", [](Context& context) {
        return context.game->gameOverMessage();
    }, 20000));
    ops_.push_back(screenIs("confirm_box"));
    ops_.push_back(wait(3));
    ops_.push_back(painted("game_over"));
    layout("game_over");
    ops_.push_back(expect("game_over_text", [](Context& context) {
        const auto items = context.gui->state("LB_MESSAGE").items;
        return !items.empty() && items[0].text == context.game->localization().tlk().text(42351);
    }));
    ops_.push_back(capture("game_over", 40));
    ops_.push_back(key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE));
    ops_.push_back(wait(4));
    ops_.push_back(expect("proof_game_over_cannot_be_escaped", [](Context& context) {
        return context.gui->active() && context.gui->screenName() == "confirm_box" && context.game->gameOverMessage();
    }));
    ops_.push_back(click("BTN_OK"));
    ops_.push_back(screenIs("mainmenu"));
    ops_.push_back(wait(6));
    ops_.push_back(expect("game_over_returns_to_the_main_menu", [](Context& context) {
        return !context.game->gameOverMessage() && context.gui->screenName().rfind("mainmenu", 0) == 0
            && context.gui->state("BTN_LOADGAME").visible;
    }));
}

void FrontEndGate::probeCell(std::size_t index, float windowWidth, float& x, float& y)
{
    const std::size_t columns = std::max<std::size_t>(1, static_cast<std::size_t>((windowWidth - 32.0f) / kProbeCell));
    x = 16.0f + static_cast<float>(index % columns) * kProbeCell;
    y = 16.0f + static_cast<float>(index / columns) * kProbeCell;
}

void FrontEndGate::step(Context& context)
{
    if (finished()) {
        return;
    }
    if (!armed_) {
        armed_ = true;
        age_ = 0;
    }
    ++gateFrames_;
    if (window_ != nullptr) {
        const bool focus = (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0;
        if (focusSeen_ && focus != focusWas_) {
            ++focusChanges_;
            logInfo(concat("front_end: window focus ", focus ? 1 : 0, " at frame ", gateFrames_, " during ",
                           index_ < ops_.size() ? ops_[index_].label : std::string{}));
        }
        focusSeen_ = true;
        focusWas_ = focus;
    }
    Op& op = ops_[index_];
    if (op.run(context)) {
        ++index_;
        age_ = 0;
        if (finished()) {
            logInfo(ok_ ? "front_end: PASS" : "front_end: FAIL");
        }
        return;
    }
    ++age_;
}

} // namespace ke
