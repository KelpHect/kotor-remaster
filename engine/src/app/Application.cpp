#include "app/Application.h"

#include "audio/Output.h"
#include "core/Log.h"
#include "game/AudioSystem.h"
#include "game/Factions.h"
#include "game/GameSession.h"
#include "game/GuiSystem.h"
#include "game/Minigames.h"
#include "kotor/BinkDecoder.h"
#include "kotor/Formats.h"
#include "kotor/Gui.h"
#include "kotor/SaveGame.h"
#include "render/Backend.h"
#include "render/Bmp.h"
#include "script/Corpus.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace ke {

namespace {

constexpr const char* kEngineVersion = "0.1.0";

struct RuntimeState {
    SDL_Window* window = nullptr;
    std::unique_ptr<IRenderBackend> backend;
    bool imguiReady = false;
    BackendKind activeBackend = BackendKind::Null;
    std::unique_ptr<kotor::Install> guiInstall;
    std::unique_ptr<GuiSystem> guiSystem;
};

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
    if (options.hidden || options.headless || kind == BackendKind::Null) {
        flags |= SDL_WINDOW_HIDDEN;
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
        ImGui::Text("%s  %s %d", sheet.name.c_str(), game.rules().className(sheet.classId).c_str(),
                    sheet.level);
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
        ImGui::Text("AC %d  Attack +%d  Skill points %d  Feat points %d",
                    game.rules().armorClass(sheet), game.rules().attackBonus(sheet),
                    sheet.skillPoints, sheet.featPoints);
        ImGui::Separator();
        ImGui::TextUnformatted("Skills");
        for (const auto& [name, rank] : sheet.skills) {
            ImGui::Text("%s: %d", name.c_str(), rank);
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
    case UiPanel::Party:
        ImGui::Text("Party roster: %zu", game.companions().size());
        ImGui::Separator();
        for (std::size_t i = 0; i < game.companions().size(); ++i) {
            const CompanionState& companion = game.companions()[i];
            ImGui::Text("%s  %s %d  HP %d/%d  XP %d", companion.tag.c_str(),
                        companion.sheet.name.c_str(), companion.sheet.level,
                        companion.sheet.hp, companion.sheet.maxHp, companion.sheet.xp);
            ImGui::SameLine();
            if (ImGui::Button((std::string("Select##") + std::to_string(i)).c_str())) {
                game.selectCompanion(static_cast<int>(i));
            }
        }
        if (game.companions().empty()) {
            ImGui::TextUnformatted("No companions in the party.");
        }
        break;
    case UiPanel::Journal:
        ImGui::Text("Journal entries: %zu", game.journal().size());
        ImGui::Separator();
        for (const auto& [plot, state] : game.journal()) {
            ImGui::Text("%s = %d", plot.c_str(), state);
        }
        break;
    case UiPanel::Merchant:
        ImGui::Text("Gold: %d", game.gold());
        ImGui::Separator();
        for (std::size_t i = 0; i < game.merchants().size(); ++i) {
            const MerchantState& merchant = game.merchants()[i];
            ImGui::Text("%s (%s)  stock=%zu", merchant.name.c_str(), merchant.resref.c_str(),
                        merchant.stock.size());
            ImGui::SameLine();
            if (ImGui::Button((std::string("Open##") + std::to_string(i)).c_str())) {
                game.openStore(merchant.entity);
            }
        }
        if (game.activeMerchant() >= 0) {
            const MerchantState& merchant =
                game.merchants()[static_cast<std::size_t>(game.activeMerchant())];
            ImGui::Separator();
            ImGui::Text("Buy from %s  (store gold %d)", merchant.resref.c_str(),
                        merchant.gold);
            for (std::size_t i = 0; i < merchant.stock.size(); ++i) {
                const MerchantStockItem& stock = merchant.stock[i];
                ImGui::Text("%s  %d gp  %s", stock.item.resref.c_str(), game.buyPrice(i),
                            stock.infinite || stock.remaining > 0 ? "" : "sold");
                ImGui::SameLine();
                if (ImGui::Button((std::string("Buy##") + std::to_string(i)).c_str())) {
                    game.buyMerchantItem(i);
                }
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Sell");
            for (std::size_t i = 0; i < game.inventory().size(); ++i) {
                const InventoryItem& item = game.inventory()[i];
                ImGui::Text("%s  %d gp%s", item.resref.c_str(), game.sellPrice(i),
                            item.identified ? "" : "  unidentified");
                ImGui::SameLine();
                if (ImGui::Button((std::string("Sell##") + std::to_string(i)).c_str())) {
                    game.sellInventoryItem(i);
                }
                if (!item.identified) {
                    ImGui::SameLine();
                    if (ImGui::Button((std::string("Identify##") + std::to_string(i)).c_str())) {
                        game.identifyInventoryItem(i);
                    }
                }
            }
        }
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

void drawMovieOverlay(const kotor::BinkVideoFrame& frame, std::uint64_t texture)
{
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("Movie", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                     | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground
                     | ImGuiWindowFlags_NoBringToFrontOnFocus);
    const float scale = std::min(io.DisplaySize.x / static_cast<float>(frame.width),
                                 io.DisplaySize.y / static_cast<float>(frame.height));
    const ImVec2 size(static_cast<float>(frame.width) * scale,
                      static_cast<float>(frame.height) * scale);
    ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - size.x) * 0.5f,
                               (io.DisplaySize.y - size.y) * 0.5f));
    ImGui::Image(static_cast<ImTextureID>(texture), size);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void drawFacialRigOverlay(const FacialRig& rig)
{
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 360.0f, 60.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340.0f, 310.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Lip sync rig")) {
        ImGui::End();
        return;
    }
    const VisemePose& pose = rig.currentPose();
    ImGui::Text("viseme %d: %s", rig.viseme(), rig.visemeName(rig.viseme()).c_str());
    ImGui::Text("jaw %.1f deg  wide %.2f  round %.2f  brow %.2f",
                rig.jawAngle(rig.viseme()) * 57.29578f, pose.mouthWide,
                pose.lipRound, pose.browRaise);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center(origin.x + 165.0f, origin.y + 120.0f);
    draw->AddCircleFilled(center, 78.0f, IM_COL32(210, 188, 158, 255));
    draw->AddCircle(center, 78.0f, IM_COL32(86, 70, 54, 255), 0, 2.0f);
    const float brow = pose.browRaise * 8.0f;
    draw->AddLine(ImVec2(center.x - 42.0f, center.y - 38.0f - brow),
                  ImVec2(center.x - 14.0f, center.y - 44.0f - brow),
                  IM_COL32(60, 45, 35, 255), 3.0f);
    draw->AddLine(ImVec2(center.x + 14.0f, center.y - 44.0f - brow),
                  ImVec2(center.x + 42.0f, center.y - 38.0f - brow),
                  IM_COL32(60, 45, 35, 255), 3.0f);
    draw->AddCircleFilled(ImVec2(center.x - 28.0f, center.y - 16.0f), 7.0f,
                          IM_COL32(40, 30, 24, 255));
    draw->AddCircleFilled(ImVec2(center.x + 28.0f, center.y - 16.0f), 7.0f,
                          IM_COL32(40, 30, 24, 255));
    const float mouthWidth = std::max(10.0f, 30.0f + pose.mouthWide * 26.0f);
    const float mouthHeight = std::max(2.0f, 3.0f + pose.jawOpen * 34.0f);
    draw->AddEllipseFilled(ImVec2(center.x, center.y + 38.0f),
                           ImVec2(mouthWidth, mouthHeight),
                           IM_COL32(74, 26, 26, 255));
    draw->AddEllipseFilled(ImVec2(center.x, center.y + 38.0f),
                           ImVec2(std::max(6.0f, mouthWidth - 4.0f),
                                  std::max(1.0f, mouthHeight * 0.4f)),
                           IM_COL32(242, 222, 216, 255));
    ImGui::Dummy(ImVec2(320.0f, 220.0f));
    ImGui::End();
}

void drawGuiScreenOverlay(GuiSystem& gui, GameSession& game)
{
    const kotor::GuiScreen* screen = gui.screen();
    if (screen == nullptr) {
        return;
    }
    ImGui::SetNextWindowFocus();
    ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(700.0f, 620.0f), ImGuiCond_Once);
    if (!ImGui::Begin("GUI screen", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%s  controls=%zu  hover=%s  action=%s", gui.screenName().c_str(),
                screen->controlCount,
                gui.hovered().empty() ? "-" : gui.hovered().c_str(),
                gui.lastAction().empty() ? "-" : gui.lastAction().c_str());
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImGui::GetWindowPos(),
                        ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                               ImGui::GetWindowPos().y + ImGui::GetWindowSize().y),
                        IM_COL32(20, 20, 24, 255));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    constexpr float kScale = 1.0f;
    ImGui::InvisibleButton("gui_canvas", ImVec2(640.0f * kScale, 480.0f * kScale));
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float localX = (mouse.x - origin.x) / kScale;
    const float localY = (mouse.y - origin.y) / kScale;
    if (ImGui::IsItemHovered()) {
        gui.pointerMove(localX, localY);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            gui.pointerDown(localX, localY);
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        gui.pointerUp(localX, localY);
    }
    std::function<void(const kotor::GuiControl&)> drawControl =
        [&](const kotor::GuiControl& control) {
            const GuiControlState state = gui.state(control.key);
            if (!state.visible) {
                return;
            }
            const ImVec2 low(origin.x + static_cast<float>(control.extent.left) * kScale,
                             origin.y + static_cast<float>(control.extent.top) * kScale);
            const ImVec2 high(low.x + static_cast<float>(control.extent.width) * kScale,
                              low.y + static_cast<float>(control.extent.height) * kScale);
            ImU32 fill = IM_COL32(40, 46, 58, 180);
            if (control.type == 6) {
                fill = state.pressed ? IM_COL32(110, 140, 180, 240)
                                     : IM_COL32(70, 90, 120, 220);
            } else if (control.type == 7) {
                fill = state.selected ? IM_COL32(80, 150, 100, 230)
                                      : IM_COL32(60, 70, 80, 200);
            } else if (control.type == 8 || control.type == 9 || control.type == 10) {
                fill = IM_COL32(50, 60, 72, 200);
            } else if (control.type == 5 || control.type == 4) {
                fill = IM_COL32(30, 34, 40, 80);
            } else if (control.type == 11) {
                fill = IM_COL32(50, 60, 80, 160);
            }
            if (control.key == gui.hovered()) {
                fill = IM_COL32(120, 150, 190, 230);
            }
            draw->AddRectFilled(low, high, fill);
            draw->AddRect(low, high, state.enabled ? IM_COL32(160, 170, 190, 255)
                                                    : IM_COL32(90, 90, 90, 255));
            if (control.type == 8 || control.type == 9 || control.type == 10) {
                const float ratio = control.maxValue > 0.0f
                    ? std::clamp(state.value / control.maxValue, 0.0f, 1.0f) : 0.0f;
                const ImVec2 barHigh(control.startFromLeft
                        ? low.x + (high.x - low.x) * ratio : high.x,
                    high.y);
                const ImVec2 barLow(control.startFromLeft ? low.x : low.x + (high.x - low.x) * ratio,
                    low.y);
                draw->AddRectFilled(barLow, barHigh, IM_COL32(100, 170, 220, 220));
            }
            const std::string label = !control.text.empty() ? control.text : control.tag;
            if (!label.empty() && control.extent.width > 8 && control.extent.height > 8) {
                draw->AddText(ImVec2(low.x + 3.0f, low.y + 2.0f),
                              IM_COL32(235, 235, 240, 255), label.c_str());
            }
            for (const kotor::GuiControl& child : control.children) {
                drawControl(child);
            }
        };
    for (const kotor::GuiControl& control : screen->controls) {
        drawControl(control);
    }
    if (!gui.lastAction().empty()) {
        const std::string action = gui.lastAction();
        gui.clearActions();
        if (!game.applyGuiAction(action)) {
            if (action == "BTN_LOADGAME") {
                gui.push("loadscreen");
            } else if (action == "BTN_NEWGAME") {
                gui.close();
            } else if (action == "BTN_MOVIES") {
                gui.push("credits");
            }
        }
    }
    ImGui::End();
}

void drawGameUi(GameSession& game, float fps, const std::string& prompt)
{
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(470.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("KotOR Remaster - Playable Slice", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("Module: %s  Area: %s", game.module().c_str(), game.scene().area.c_str());
    ImGui::Text("Position: %.2f %.2f %.2f  %s", game.position().x, game.position().y, game.position().z,
                game.grounded() ? "grounded" : "airborne");
    ImGui::Text("FPS: %.1f", fps);
    ImGui::Text("HP: %d / %d", game.playerHp(), game.playerMaxHp());
    ImGui::ProgressBar(static_cast<float>(game.playerHp()) / static_cast<float>(std::max(1, game.playerMaxHp())),
                       ImVec2(-1.0f, 0.0f));
    if (game.combatTarget() != UINT32_MAX) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "Target: %s", game.combatTargetName().c_str());
    }
    if (!prompt.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "%s", prompt.c_str());
    }
    ImGui::Separator();
    ImGui::TextUnformatted("WASD move, mouse look, Shift run, Space jump");
    ImGui::TextUnformatted("E interact, F attack, F5 save, F9 load, F1 mouse, Esc quit");
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

    if (game.dialogue().active) {
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
    if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
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
        game.update(1.0f / 60.0f, InputState{});
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

int runPlaythrough(GameSession& game)
{
    bool ok = true;
    const auto teleportOrWalk = [&](kotor::Vec3 target, float seconds) {
        if (!game.walkTo(target, seconds)) {
            logWarn("playthrough: walk blocked, using scripted spawn");
            game.setPositionForTest(target);
        }
    };

    const std::size_t trask = game.findEntity("end_trask");
    if (trask == static_cast<std::size_t>(-1)) {
        logError("playthrough: end_trask missing");
        ok = false;
    } else {
        teleportOrWalk(game.scene().entities[trask].position, 20.0f);
        if (!game.startDialogue(trask)) {
            logError("playthrough: Trask dialogue did not start");
            ok = false;
        } else {
            int lines = 0;
            while (game.dialogue().active && lines < 24) {
                logInfo(concat("  dialogue: ", game.dialogue().speaker, ": ", game.dialogue().text));
                game.chooseDialogueOption(0);
                ++lines;
            }
            logInfo(concat("playthrough: dialogue lines=", lines));
            ok = ok && lines > 0;
        }
    }

    const std::size_t triggerEntity = game.findEntity("end_trig02");
    if (triggerEntity == static_cast<std::size_t>(-1)) {
        logWarn("playthrough: end_trig02 missing");
        ok = false;
    } else {
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
        teleportOrWalk(centre, 20.0f);
        game.update(1.0f / 60.0f, InputState{});
        const bool fired = hasEvent(game, "trigger end_trig02: enter");
        logInfo(concat("playthrough: trigger end_trig02 fired=", fired ? 1 : 0));
        ok = ok && fired;
    }

    const std::size_t locker = game.findEntity("end_locker01");
    if (locker == static_cast<std::size_t>(-1)) {
        logWarn("playthrough: end_locker01 missing");
        ok = false;
    } else {
        teleportOrWalk(game.scene().entities[locker].position, 10.0f);
        const bool used = game.usePlaceable(locker);
        const bool logged = hasEvent(game, "use end_locker01");
        logInfo(concat("playthrough: placeable end_locker01 used=", used ? 1 : 0,
                       " logged=", logged ? 1 : 0));
        ok = ok && used && logged;
    }

    const std::size_t hostile = firstHostile(game);
    if (hostile == static_cast<std::size_t>(-1)) {
        logWarn("playthrough: no hostile creature");
        ok = false;
    } else {
        teleportOrWalk(game.scene().entities[hostile].position, 30.0f);
        game.attackNearestHostile();
        int steps = 0;
        while (steps < 3600 && !game.playerDead() && !game.scene().entities[hostile].dead) {
            game.update(1.0f / 60.0f, InputState{});
            ++steps;
        }
        const bool dead = game.scene().entities[hostile].dead;
        logInfo(concat("playthrough: combat target ", game.scene().entities[hostile].tag,
                       " dead=", dead ? 1 : 0, " playerHp=", game.playerHp(),
                       "/", game.playerMaxHp(), " steps=", steps));
        if (!dead) {
            const std::size_t total = game.combatLog().size();
            const std::size_t first = total > 40 ? total - 40 : 0;
            for (std::size_t i = first; i < total; ++i) {
                logInfo(concat("playthrough event: ", game.combatLog()[i]));
            }
        }
        ok = ok && dead;
    }

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
        game.attackNearestHostile();
        int steps = 0;
        while (steps < 3600 && !game.playerDead() && !game.scene().entities[hostile].dead) {
            game.update(1.0f / 60.0f, InputState{});
            ++steps;
        }
        check(game.scene().entities[hostile].dead, "hostile_died");
        check(scriptLogHas(game, "hook ScriptDeath"), "death_hook_ran");
    }

    // Dialogue conditions: the shipped DLG has 19 StartingList.Active entries.
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
        if (line.rfind("dialogue start end_trask01", 0) == 0) {
            selected = line;
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
    check(soldier.skillPoints == 2, "skill_points_2");
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
    const std::int32_t storeGoldBefore = game.merchants()[merchantIndex].gold;
    const std::size_t inventoryBefore = game.inventory().size();
    check(game.buyMerchantItem(stockIndex), "buy_item");
    check(game.gold() == goldBefore - buyPrice, "buy_gold");
    check(game.merchants()[merchantIndex].gold == storeGoldBefore + buyPrice, "store_gold_buy");
    check(game.inventory().size() == inventoryBefore + 1, "buy_inventory");
    game.character().inventory.back().identified = false;
    const std::int32_t goldBeforeIdentify = game.gold();
    check(game.identifyInventoryItem(game.inventory().size() - 1), "identify_item");
    check(game.character().inventory.back().identified, "identified");
    check(game.gold() < goldBeforeIdentify, "identify_gold");
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
        bool merchantGoldMatch = false;
        for (const MerchantState& restoredMerchant : restored.merchants()) {
            if (restoredMerchant.resref == merchant.resref
                && restoredMerchant.gold == game.merchants()[merchantIndex].gold) {
                merchantGoldMatch = true;
            }
        }
        check(merchantGoldMatch, "merchant_gold_roundtrip");
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
    std::int32_t total = 0;
    std::size_t firstEntity = static_cast<std::size_t>(-1);
    for (const EncounterState& state : game.encounters()) {
        total += state.spawnedCount;
        if (firstEntity == static_cast<std::size_t>(-1) && !state.entities.empty()) {
            firstEntity = state.entities.front();
        }
    }
    logInfo(concat("encounter test: spawned=", total));
    check(total > 0, "creatures_spawned");
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
        if (state.spawned && state.resetTime > 0.0f && state.respawnsRemaining != 0
            && !state.entities.empty()) {
            target = i;
            break;
        }
    }
    if (target == static_cast<std::size_t>(-1)) {
        logError("encounter reset test: no resettable encounter");
        return 9;
    }
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
    game.giveXp(1000);
    check(game.character().xp == xpBefore + 1000, "pc_full_xp");
    if (!game.companions().empty()) {
        logInfo(concat("party test: companion xp=", game.companions().front().sheet.xp));
        check(game.companions().front().sheet.xp == 800, "companion_80_percent");
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
            for (int i = 0; i < 600 && !hasEvent(game, "Carth hits"); ++i) {
                game.update(0.05f, InputState{});
            }
            check(hasEvent(game, "Carth hits"), "companion_attacks");
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
        check(restored.companions().front().sheet.xp == 800, "companion_xp_roundtrip");
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
        if (game.scene().entities[i].kind == kotor::EntityKind::Placeable
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
            static_cast<std::size_t>(options.vmLimit), trace);
        logInfo(concat("vm corpus: files=", run.files.size(), " returned=", run.returned,
                       " blocked=", run.blocked, " budget=", run.budget,
                       " decode_errors=", run.decodeErrors, " vm_errors=", run.vmErrors,
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
    check(game.scriptWorld().getLocal(pc, "10").asInt() == 0, "script_before");
    check(game.scriptWorld().getLocal(pc, "11").asInt() == 25, "script_after");
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

    logInfo(concat("faction test: count=", factions.count(),
                   " relations=", factions.relations(),
                   " player/hostile1=", factions.reputation(0, 1),
                   " player/friendly1=", factions.reputation(0, 2),
                   " player/endar=", factions.reputation(0, 14)));
    logInfo(ok ? "faction test: PASS" : "faction test: FAIL");
    return ok ? 0 : 67;
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
    check(scene.entities.size() == 175, "entities");
    check(scene.entityMeshes.size() == 70, "entity_meshes");
    check(scene.triggers.size() == 17, "triggers");
    check(scene.warnings.empty(), "warnings");
    check(scene.vertices.size() > 200'000, "vertices");
    check(scene.batches.size() == 55, "batches");
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

    // Pazaak: deterministic deck, side cards and first-to-two-sets match.
    const auto playPazaak = [](std::uint32_t seed) {
        PazaakGame game;
        game.reset(seed);
        std::string transcript;
        for (int step = 0; step < 256
             && game.phase() != PazaakGame::Phase::MatchOver; ++step) {
            if (game.phase() == PazaakGame::Phase::PlayerTurn) {
                if (game.playerTotal() < 17) {
                    game.playerDraw();
                } else {
                    game.playerStand();
                }
            } else if (game.phase() == PazaakGame::Phase::OpponentTurn) {
                game.opponentTurn();
            } else {
                break;
            }
            transcript += std::to_string(static_cast<int>(game.phase())) + "|"
                + std::to_string(game.playerTotal()) + "|"
                + std::to_string(game.opponentTotal()) + "|"
                + std::to_string(game.playerSets()) + "|"
                + std::to_string(game.opponentSets()) + "|"
                + std::to_string(game.deckRemaining()) + ";";
        }
        return std::make_tuple(transcript, game.phase(), game.playerSets(),
                               game.opponentSets());
    };
    const auto pazaakA = playPazaak(0x13572468u);
    const auto pazaakB = playPazaak(0x13572468u);
    const auto pazaakC = playPazaak(0x24681357u);
    check(std::get<0>(pazaakA) == std::get<0>(pazaakB), "pazaak_deterministic");
    check(std::get<0>(pazaakA) != std::get<0>(pazaakC), "pazaak_seed_varies");
    check(std::get<1>(pazaakA) == PazaakGame::Phase::MatchOver, "pazaak_match_over");
    check(std::get<2>(pazaakA) == 2 || std::get<3>(pazaakA) == 2,
          "pazaak_two_sets");
    check(std::get<0>(pazaakA).size() > 32, "pazaak_turns");

    PazaakGame pazaak;
    pazaak.reset(7u);
    check(pazaak.deckRemaining() == 40, "pazaak_deck");
    check(pazaak.playerSide().size() == 4 && pazaak.playerSide()[0] == -1
              && pazaak.playerSide()[1] == -2 && pazaak.playerSide()[2] == 3
              && pazaak.playerSide()[3] == 4,
          "pazaak_side_cards");
    check(!pazaak.playerStand(), "pazaak_stand_requires_total");
    check(!pazaak.playerPlaySide(9), "pazaak_side_bounds");
    check(pazaak.playerPlaySide(3), "pazaak_side_play");
    check(pazaak.playerTotal() == 4 && pazaak.playerSide().size() == 3,
          "pazaak_side_total");
    check(pazaak.playerDraw() && pazaak.deckRemaining() == 39, "pazaak_draw");
    check(pazaak.playerTotal() >= 5 && pazaak.playerTotal() <= 14,
          "pazaak_draw_range");
    check(pazaak.phase() == PazaakGame::Phase::OpponentTurn, "pazaak_turn_order");
    check(!pazaak.playerDraw(), "pazaak_draw_wrong_phase");
    check(pazaak.opponentTurn(), "pazaak_opponent");
    check(pazaak.playerSets() + pazaak.opponentSets() == 1, "pazaak_set_scored");

    // Swoop: throttle/boost/brake and three laps of 3,000 m.
    const auto runSwoop = []() {
        SwoopRace race;
        race.reset();
        for (int frame = 0; frame < 60 * 150; ++frame) {
            const float t = static_cast<float>(frame) / 60.0f;
            race.update(1.0f / 60.0f, 1.0f, t > 5.0f, false);
        }
        return race.state();
    };
    const SwoopState swoopA = runSwoop();
    const SwoopState swoopB = runSwoop();
    check(swoopA.finished, "swoop_finished");
    check(swoopA.lap == SwoopRace::kLaps, "swoop_laps");
    check(swoopA.distance >= SwoopRace::kTrackLength * SwoopRace::kLaps,
          "swoop_distance");
    check(swoopA.speed > 100.0f && swoopA.time > 0.0f, "swoop_speed_time");
    check(swoopA.distance == swoopB.distance && swoopA.speed == swoopB.speed
              && swoopA.time == swoopB.time,
          "swoop_deterministic");
    SwoopRace braked;
    braked.reset();
    for (int frame = 0; frame < 60; ++frame) {
        braked.update(1.0f / 60.0f, 1.0f, false, true);
    }
    check(braked.state().speed < swoopA.speed, "swoop_brake_slower");

    // Turret: five approaching targets, hits/score and damage on arrival.
    TurretGame turret;
    turret.reset();
    check(turret.targets().size() == 5, "turret_targets");
    check(turret.fireAt(0), "turret_fire_hit");
    check(turret.state().hits == 1 && turret.state().shots == 1
              && turret.state().score == 100,
          "turret_score");
    check(!turret.fireAt(0), "turret_dead_target");
    check(turret.state().shots == 1, "turret_shot_count");
    turret.update(1.0f);
    check(turret.state().hitPoints == 100, "turret_approach");
    turret.update(10.0f);
    check(!turret.targets()[1].alive && !turret.targets()[4].alive,
          "turret_arrival");
    check(turret.state().hitPoints == 20, "turret_damage");

    TurretGame turretA;
    TurretGame turretB;
    turretA.reset();
    turretB.reset();
    for (TurretGame* sim : {&turretA, &turretB}) {
        sim->update(0.5f);
        sim->fireAt(2);
        sim->update(2.0f);
        sim->fireAt(3);
    }
    check(turretA.state().score == turretB.state().score
              && turretA.state().hitPoints == turretB.state().hitPoints
              && turretA.state().shots == 2 && turretA.state().hits == 2,
          "turret_deterministic");

    // Script bridge: the compiled fixture calls PlayPazaak(1, "", 0, FALSE,
    // OBJECT_SELF); drive the match with the same stand-at-17 policy.
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
        check(game.pazaakActive(), "pazaak_script_start");
        if (game.pazaakActive()) {
            for (int step = 0; step < 256
                 && game.pazaak()->phase() != PazaakGame::Phase::MatchOver; ++step) {
                if (game.pazaak()->phase() == PazaakGame::Phase::PlayerTurn) {
                    if (game.pazaak()->playerTotal() < 17) {
                        game.pazaak()->playerDraw();
                    } else {
                        game.pazaak()->playerStand();
                    }
                } else if (game.pazaak()->phase() == PazaakGame::Phase::OpponentTurn) {
                    game.pazaak()->opponentTurn();
                } else {
                    break;
                }
            }
            check(game.pazaak()->phase() == PazaakGame::Phase::MatchOver,
                  "pazaak_script_match_over");
            const bool playerWon =
                game.pazaak()->playerSets() > game.pazaak()->opponentSets();
            game.resolvePazaakIfOver();
            check(!game.pazaakActive(), "pazaak_script_resolved");
            check(game.lastPazaakResult() == (playerWon ? 1 : 0),
                  "pazaak_script_result");
        }
    }

    // Wager settlement through GameSession.
    const std::int32_t goldBefore = game.gold();
    check(game.startPazaak(2, "", 25, false, 0), "pazaak_session_start");
    if (game.pazaakActive()) {
        for (int step = 0; step < 256
             && game.pazaak()->phase() != PazaakGame::Phase::MatchOver; ++step) {
            if (game.pazaak()->phase() == PazaakGame::Phase::PlayerTurn) {
                if (game.pazaak()->playerTotal() < 17) {
                    game.pazaak()->playerDraw();
                } else {
                    game.pazaak()->playerStand();
                }
            } else if (game.pazaak()->phase() == PazaakGame::Phase::OpponentTurn) {
                game.pazaak()->opponentTurn();
            } else {
                break;
            }
        }
        check(game.pazaak() != nullptr
                  && game.pazaak()->phase() == PazaakGame::Phase::MatchOver,
              "pazaak_session_match_over");
        const bool playerWon =
            game.pazaak()->playerSets() > game.pazaak()->opponentSets();
        game.resolvePazaakIfOver();
        check(game.lastPazaakResult() == (playerWon ? 1 : 0),
              "pazaak_session_result");
        const std::int32_t expected = std::max<std::int32_t>(
            0, goldBefore + (playerWon ? 25 : -25));
        check(game.gold() == expected, "pazaak_wager");
    }

    logInfo(concat("minigame test: pazaak sets=", std::get<2>(pazaakA), "-",
                   std::get<3>(pazaakA), " turns=", std::get<0>(pazaakA).size(),
                   " swoop distance=", swoopA.distance, " time=", swoopA.time,
                   " turret score=", turret.state().score,
                   " hp=", turret.state().hitPoints));
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

    // Procedural facial rig: visemes.2da channels drive the shipped head bones.
    const FacialRig& rig = game.facialRig();
    check(rig.loaded(), "facial_rig");
    check(rig.visemeCount() == 16, "facial_visemes");
    check(rig.visemeName(0) == "Silence" && rig.visemeName(13) == "Roar",
          "facial_names");
    check(rig.pose(0).jawOpen == 0.0f && rig.pose(11).jawOpen == 0.0f,
          "facial_closed");
    check(rig.pose(13).jawOpen > 0.8f, "facial_roar");
    check(std::fabs(rig.jawAngle(13)) > std::fabs(rig.jawAngle(10)),
          "facial_jaw_order");
    const auto roarRotations = rig.rotations(13);
    check(roarRotations.count("f_jaw_g") == 1
              && roarRotations.count("f_lmc_g") == 1
              && roarRotations.count("f_rmc_g") == 1
              && roarRotations.count("f_lbrw_g") == 1,
          "facial_bones");
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
            const std::vector<kotor::MdlMeshTriangle> roar =
                head.posedTrianglesRigged({}, 0.0f, -1.0f, roarRotations);
            const std::vector<kotor::MdlMeshTriangle> silence =
                head.posedTrianglesRigged({}, 0.0f, -1.0f, rig.rotations(0));
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
            check(roarDelta > 1.0e-3, "facial_geometry");
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
        check(game.facialRig().currentPose().jawOpen
                  == game.facialRig().pose(game.facialRig().viseme()).jawOpen,
              "facial_pose_drive");
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

    check(audio.playFootstep(0, 4, {0.0f, 0.0f, 0.0f}), "footstep_stone");
    check(audio.playFootstep(1, 10, {0.0f, 0.0f, 0.0f}), "footstep_metal");
    check(audio.playFootstep(2, 5, {0.0f, 0.0f, 0.0f}), "footstep_wood");
    check(audio.mixer().activeVoices(audio::Bus::Effects) > 0, "footstep_voices");

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
    check(mixer.activeVoices() == 0 && mixer.lastRms() == 0.0f, "stop_all");

    std::vector<float> loopMix(960 * 2);
    auto shortTone = std::make_shared<audio::SoundData>(*tone);
    shortTone->samples.resize(100 * 2);
    const int loopVoice = mixer.play(shortTone, audio::Bus::Effects, 1.0f, true);
    mixer.mix(loopMix.data(), 960);
    check(loopVoice > 0 && mixer.activeVoices() == 1, "loop_wrap");
    mixer.stopAll();

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
        check(std::fabs(audio.mixer().busVolume(audio::Bus::Ambient) - 0.4f) < 0.01f,
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
        const std::vector<kotor::MdlMeshTriangle> fx0 = fxModel->fxTriangles({}, 0.0f);
        const std::vector<kotor::MdlMeshTriangle> fx1 = fxModel->fxTriangles({}, 1.0f);
        check(!fx0.empty() && fx0.size() == fx1.size(), "fx_geometry");
        bool fxFinite = true;
        std::size_t fxMoved = 0;
        for (std::size_t i = 0; i < fx0.size() && i < fx1.size(); ++i) {
            for (int v = 0; v < 3; ++v) {
                const kotor::Vec3 a = fx0[i].v[v].position;
                const kotor::Vec3 b = fx1[i].v[v].position;
                fxFinite = fxFinite && std::isfinite(b.x) && std::isfinite(b.y)
                    && std::isfinite(b.z);
                if (std::fabs(a.x - b.x) > 0.0001f || std::fabs(a.y - b.y) > 0.0001f
                    || std::fabs(a.z - b.z) > 0.0001f) {
                    ++fxMoved;
                }
            }
        }
        check(fxFinite, "fx_finite");
        check(fxMoved > 0, "fx_animated");
        bool additive = false;
        bool alpha = false;
        for (const kotor::MdlMeshTriangle& triangle : fx0) {
            additive = additive
                || triangle.blend == kotor::BlendMode::Additive;
            alpha = alpha || triangle.blend == kotor::BlendMode::Alpha;
        }
        check(additive, "fx_additive_batches");
        check(alpha, "fx_alpha_batches");
        bool parameterTextures = !fx0.empty();
        for (const kotor::MdlMeshTriangle& triangle : fx0) {
            parameterTextures = parameterTextures
                && triangle.texture.rfind("__fx_", 0) == 0
                && triangle.texture.size() == 13;
        }
        check(parameterTextures, "fx_parameter_textures");
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
    std::size_t entityAlpha = 0;
    for (const kotor::EntityMesh& mesh : game.scene().entityMeshes) {
        if (!mesh.fx) {
            continue;
        }
        for (const kotor::TextureBatch& batch : mesh.batches) {
            if (batch.blend == kotor::BlendMode::Alpha) {
                ++entityAlpha;
            }
        }
    }
    logInfo(concat("animation test: blend batches room additive=", sceneAdditive,
                   " alpha=", sceneAlpha, " opaque=", sceneOpaque,
                   " entity alpha=", entityAlpha));
    check(sceneAdditive > 0, "scene_additive_batches");
    check(sceneOpaque > 0, "scene_opaque_batches");
    check(entityAlpha > 0, "entity_alpha_batches");
    const std::optional<kotor::Mdl> roomFx = loadModel("m01aa_01a");
    check(roomFx.has_value(), "room_light_loaded");
    if (roomFx.has_value()) {
        check(roomFx->hasFxNodes(), "room_light_nodes");
        const std::set<std::int32_t> roomTypes = roomFx->fxControllerTypes();
        check(roomTypes.contains(76) && roomTypes.contains(88) && roomTypes.contains(140),
              "room_light_controllers");
        check(!roomFx->fxTriangles({}, 0.0f).empty(), "room_light_geometry");
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
        check(weaponMesh->attachNode == "Rhand_g" || weaponMesh->attachNode == "Lhand_g",
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
        check(std::fabs(cinematic.eye.z - (definition.position.z + definition.height)) < 0.001f,
              "camera_height");
        check(std::fabs(kotor::length(cinematic.forward) - 1.0f) < 0.01f, "camera_forward");
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
    passive.feats.insert(104); // SCOUNDRELS_LUCK
    check(game.rules().armorClass(passive) == baseAc + 2, "scoundrel_luck_2");
    passive.feats.insert(105); // IMPROVED_SCOUNDRELS_LUCK
    check(game.rules().armorClass(passive) == baseAc + 4, "scoundrel_luck_4");
    passive.feats.insert(106); // MASTER_SCOUNDRELS_LUCK
    check(game.rules().armorClass(passive) == baseAc + 6, "scoundrel_luck_6");
    passive.feats.insert(107); // JEDI_SENSE
    check(game.rules().armorClass(passive) == baseAc + 8, "jedi_sense_2");
    passive.feats.insert(108); // KNIGHT_SENSE
    check(game.rules().armorClass(passive) == baseAc + 10, "knight_sense_4");
    passive.feats.insert(109); // MASTER_SENSE
    check(game.rules().armorClass(passive) == baseAc + 12, "master_sense_6");
    const std::int32_t baseDc = game.rules().forcePowerSaveDC(passive);
    passive.feats.insert(88); // FORCE_FOCUS
    check(game.rules().forcePowerSaveDC(passive) == baseDc + 1, "force_focus_1");
    passive.feats.insert(89); // FORCE_FOCUS_ADVANCED
    check(game.rules().forcePowerSaveDC(passive) == baseDc + 2, "force_focus_2");
    passive.feats.insert(90); // FORCE_FOCUS_MASTERY
    check(game.rules().forcePowerSaveDC(passive) == baseDc + 4, "force_focus_4");
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

    const std::int32_t fpBefore = game.forcePoints();
    const std::int32_t hpBefore = target.hitPoints;
    check(game.useForcePower(35, targetObject), "lightning_used");
    check(game.forcePoints() == fpBefore - 20, "lightning_cost");
    check(target.hitPoints < hpBefore, "lightning_damage");
    check(game.alignment() == 48, "lightning_dark_side");

    check(game.useForcePower(28, 0), "heal_used");
    check(game.character().hp > 20, "heal_amount");
    check(game.alignment() == 50, "heal_light_side");

    check(game.useForcePower(17, 0), "armor_used");
    check(game.forceArmorBonus() > 0, "armor_buff");
    check(game.alignment() == 52, "armor_light_side");

    check(game.useForcePower(46, targetObject), "stun_used");
    check(target.stunTimer > 0.0f, "stun_applied");
    check(game.alignment() == 54, "stun_light_side");

    check(game.useForcePower(20, targetObject), "immunity_used");
    const auto targetHasImmunity = [&]() {
        return std::any_of(target.effects.begin(), target.effects.end(),
                           [](const kotor::ActiveEffect& effect) {
                               return effect.remaining > 0.0f
                                   && effect.label.find("FORCE_IMMUNITY") != std::string::npos;
                           });
    };
    check(targetHasImmunity(), "immunity_effect");
    const std::int32_t fpBeforeImmuneBlock = game.forcePoints();
    check(!game.useForcePower(35, targetObject), "immunity_blocks_power");
    check(game.forcePoints() == fpBeforeImmuneBlock, "immunity_no_cost");
    check(game.useForcePower(19, targetObject), "breach_used");
    check(!targetHasImmunity(), "breach_removes_immunity");
    check(game.useForcePower(35, targetObject), "lightning_after_breach");

    check(game.useForcePower(17, 0), "armor_before_suppress");
    check(game.forceArmorBonus() > 0, "armor_before_suppress_effect");
    check(game.useForcePower(24, 0), "shield_before_breach");
    check(game.forceShield() > 0, "shield_effect");
    check(game.useForcePower(19, 0), "breach_self");
    check(game.forceArmorBonus() == 0 && game.forceShield() == 0,
          "breach_removes_buffs");
    check(game.useForcePower(48, 0), "suppress_used");
    check(game.forceSuppressed(), "suppress_effect");
    const std::int32_t fpBeforeSuppressBlock = game.forcePoints();
    check(!game.useForcePower(35, targetObject), "suppress_blocks_power");
    check(game.forcePoints() == fpBeforeSuppressBlock, "suppress_no_cost");
    InputState idle;
    for (int i = 0; i < 200; ++i) {
        game.update(0.08f, idle);
    }
    check(!game.forceSuppressed(), "suppress_expires");
    check(game.useForcePower(35, targetObject), "lightning_after_suppress");

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
            return game.scriptWorld().getLocal(game.scriptWorld().pc, name,
                                               script::ValueType::Int).asInt();
        };
        check(scriptLocal("0") == 77, "script_alignment_read");
        check(scriptLocal("1") == 1, "script_has_feat");
        check(scriptLocal("2") == 1, "script_has_spell");
        check(scriptLocal("3") == fpBeforeScript, "script_force_points");
        check(scriptLocal("4") == game.maxForcePoints(), "script_max_force_points");
        check(scriptLocal("5") == 1, "script_talent_type");
        check(scriptLocal("6") == 35, "script_talent_id");
        check(game.combatMode() == CombatMode::PowerAttack, "script_action_feat");
        check(game.forceArmorBonus() > 0, "script_action_spell");
        check(game.alignment() == 81, "script_alignment_action");
    }

    check(game.useForcePower(17, 0), "persist_effect_added");
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
    ok = ok && local("0") == 1 && local("1") == 0 && local("2") == 0
        && local("5") == 1 && local("6") == 0;
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
    std::unique_ptr<audio::Output> audioOutput;
    if (!options.module.empty()) {
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
            game.emplace(installPath, options.module, options.language);
            logInfo(concat("Language: ", game->localization().languageName(),
                           " (", game->languageId(), ") tlk entries=",
                           game->localization().entryCount()));
            game->setAnimationEnabled(!options.headless && options.backend != BackendKind::Null);
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
            logInfo(concat("  entity meshes: ", loaded.entityMeshes.size(), " triggers: ",
                           loaded.triggers.size(), " warnings: ", loaded.warnings.size()));
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
                const bool ok = loaded.rooms.size() == 15
                    && loaded.vertices.size() == 237594
                    && loaded.indices.size() == 237594
                    && loaded.batches.size() == 49
                    && loaded.textures.size() == 84
                    && loaded.collisionVertices.size() == 850
                    && loaded.collision.size() == 1365
                    && loaded.entities.size() == 218
                    && loaded.entityMeshes.size() == 142
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
                               " cameras=", loaded.cameras.size(),
                               " triggers=", loaded.triggers.size(),
                               " warnings=", loaded.warnings.size(),
                               " ok=", ok ? 1 : 0));
                return ok ? 0 : 9;
            }
            if (options.dialogueTest) {
                return runDialogueTest(*game);
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
                    GameSession restored(installPath, options.module);
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
            if (options.demoDialogue) {
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

    if (game && !options.headless) {
        audioOutput = std::make_unique<audio::Output>(game->audio().mixer());
        if (audioOutput->valid()) {
            logInfo(concat("Audio device: ", audioOutput->device()));
        } else {
            logWarn("Audio device unavailable; continuing silent");
            audioOutput.reset();
        }
    }
    if (game && !options.guiScreen.empty()) {
        game->stopMovie();
        try {
            state.guiInstall = std::make_unique<kotor::Install>(game->installPath());
            state.guiInstall->bindModule(game->module());
            state.guiInstall->addPacks();
            state.guiSystem = std::make_unique<GuiSystem>(*state.guiInstall);
            if (state.guiSystem->open(options.guiScreen)) {
                logInfo(concat("GUI screen loaded: ", state.guiSystem->screenName(),
                               " controls=", state.guiSystem->screen()->controlCount));
                if (!options.guiAction.empty()
                    && !state.guiSystem->activate(options.guiAction)) {
                    logWarn(concat("GUI action not found: ", options.guiAction));
                }
            } else {
                logWarn(concat("GUI screen not found: ", options.guiScreen));
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
        if (!options.headless) {
            SDL_SetWindowRelativeMouseMode(state.window, true);
        }
    }

    std::uint64_t frames = 0;
    bool loggedUiGeometry = false;
    std::uint32_t lastAnimationRevision = UINT32_MAX;
    std::uint64_t movieTexture = 0;
    int uploadedMovieFrame = -1;
    bool movieShotSaved = false;
    bool movieUploadWarned = false;
    bool running = true;
    bool mouseRelative = game && !options.headless;
    float mouseDx = 0.0f;
    float mouseDy = 0.0f;
    std::string exitReason = "loop exited";
    double smoothedFrameMs = 0.0;
    const std::uint64_t startTicks = SDL_GetTicksNS();
    std::uint64_t previousTicks = startTicks;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (state.imguiReady) {
                ImGui_ImplSDL3_ProcessEvent(&event);
            }
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
                exitReason = "window closed";
            } else if (event.type == SDL_EVENT_MOUSE_MOTION && game && mouseRelative) {
                mouseDx += event.motion.xrel;
                mouseDy += event.motion.yrel;
            } else if (event.type == SDL_EVENT_KEY_DOWN
                       && (!state.imguiReady || !ImGui::GetIO().WantCaptureKeyboard)) {
                if (event.key.key == SDLK_ESCAPE) {
                    if (game && game->dialogue().active) {
                        game->endDialogue();
                        logInfo("Dialogue closed");
                    } else {
                        running = false;
                        exitReason = "escape pressed";
                    }
                } else if (game && event.key.key == SDLK_E) {
                    const std::string before = game->module();
                    if (game->interact()) {
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
                        logInfo("Nothing to interact with");
                    }
                } else if (game && event.key.key == SDLK_F) {
                    game->attackNearestHostile();
                } else if (game && game->dialogue().active
                           && event.key.key >= SDLK_1 && event.key.key <= SDLK_9) {
                    game->chooseDialogueOption(static_cast<std::size_t>(event.key.key - SDLK_1));
                } else if (game && event.key.key == SDLK_F5) {
                    logInfo(game->save(savePath) ? concat("Saved player state to ", savePath)
                                                 : concat("Could not save to ", savePath));
                } else if (game && event.key.key == SDLK_F9) {
                    if (game->load(savePath)) {
                        state.backend->setScene(&game->scene());
                        state.backend->setCamera(game->camera());
                        logInfo(concat("Loaded player state from ", savePath));
                    } else {
                        logWarn(concat("Could not load player state from ", savePath));
                    }
                } else if (game && event.key.key == SDLK_F1) {
                    mouseRelative = !mouseRelative;
                    SDL_SetWindowRelativeMouseMode(state.window, mouseRelative);
                    logInfo(mouseRelative ? "Mouse captured" : "Mouse released");
                } else if (game && event.key.key == SDLK_C) {
                    game->toggleUiPanel(UiPanel::Character);
                } else if (game && event.key.key == SDLK_I) {
                    game->toggleUiPanel(UiPanel::Inventory);
                } else if (game && event.key.key == SDLK_P) {
                    game->toggleUiPanel(UiPanel::Party);
                } else if (game && event.key.key == SDLK_J) {
                    game->toggleUiPanel(UiPanel::Journal);
                } else if (game && event.key.key == SDLK_M) {
                    game->toggleUiPanel(UiPanel::Merchant);
                }
            }
        }

        if (!running) {
            break;
        }

        const std::uint64_t now = SDL_GetTicksNS();
        const double deltaMs = static_cast<double>(now - previousTicks) / 1'000'000.0;
        previousTicks = now;
        smoothedFrameMs = (smoothedFrameMs <= 0.0) ? deltaMs : (smoothedFrameMs * 0.9 + deltaMs * 0.1);
        const double elapsedSeconds = static_cast<double>(now - startTicks) / 1'000'000'000.0;

        if (game) {
            InputState input;
            const bool* keys = SDL_GetKeyboardState(nullptr);
            input.forward = keys[SDL_SCANCODE_W] != 0;
            input.back = keys[SDL_SCANCODE_S] != 0;
            input.left = keys[SDL_SCANCODE_A] != 0;
            input.right = keys[SDL_SCANCODE_D] != 0;
            input.run = keys[SDL_SCANCODE_LSHIFT] != 0 || keys[SDL_SCANCODE_RSHIFT] != 0;
            input.jump = keys[SDL_SCANCODE_SPACE] != 0;
            input.mouseDx = mouseDx;
            input.mouseDy = mouseDy;
            mouseDx = 0.0f;
            mouseDy = 0.0f;
            game->update(static_cast<float>(deltaMs / 1000.0), input);
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
            state.backend->setCamera(game->camera());
            if (game->animationRevision() != lastAnimationRevision) {
                lastAnimationRevision = game->animationRevision();
                state.backend->refreshEntityMeshes(game->scene());
            }
        }

        if (state.imguiReady) {
            state.backend->newFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            const bool movieMode = game && game->moviePlaying();
            const bool guiMode = state.guiSystem && state.guiSystem->active();
            if (!movieMode && !guiMode) {
                drawDebugUi(state, frames, smoothedFrameMs, elapsedSeconds, exitReason);
            }
            if (game) {
                if (!movieMode && !guiMode) {
                    drawGameUi(*game, smoothedFrameMs > 0.0 ? static_cast<float>(1000.0 / smoothedFrameMs) : 0.0f,
                               game->interactionPrompt());
                    if (options.facialDemo >= 0 || game->lipSyncActive()) {
                        drawFacialRigOverlay(game->facialRig());
                    }
                }
                if (game->movieFrame() != nullptr) {
                    if (game->movieFrameIndex() != uploadedMovieFrame) {
                        std::string movieError;
                        const kotor::BinkVideoFrame* frame = game->movieFrame();
                        if (state.backend->uploadMovieFrame(
                                frame->width, frame->height, frame->rgba.data(),
                                movieTexture, movieError)) {
                            uploadedMovieFrame = game->movieFrameIndex();
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
            if (!movieMode && guiMode && game) {
                drawGuiScreenOverlay(*state.guiSystem, *game);
            }
            if (!movieMode && state.guiSystem && state.guiSystem->quitRequested()) {
                exitReason = "GUI exit";
                running = false;
            }
            ImGui::Render();

            const ImDrawData* drawData = ImGui::GetDrawData();
            if (!loggedUiGeometry && drawData != nullptr && drawData->TotalVtxCount > 0) {
                logInfo(concat("ImGui draw data at frame ", frames, ": ",
                               drawData->CmdListsCount, " list(s), ", drawData->TotalVtxCount,
                               " vertices"));
                loggedUiGeometry = true;
            }
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

        if (state.activeBackend == BackendKind::Null && running) {
            SDL_Delay(1); // avoid a 100% CPU spin in the headless logical loop
        }
    }

    const double totalSeconds = static_cast<double>(SDL_GetTicksNS() - startTicks) / 1'000'000'000.0;
    const double averageMs = frames > 0 ? (totalSeconds * 1000.0) / static_cast<double>(frames) : 0.0;

    logInfo(concat("Exit: ", exitReason));
    logInfo(concat("Frames: ", frames, " in ", totalSeconds, " s (avg ", averageMs, " ms/frame)"));
    logInfo(concat("Backend summary: ", state.backend->info().name, " | ",
                   state.backend->info().apiVersion, " | ", state.backend->info().swapchain));

    if (state.backend) {
        state.backend->releaseMovieFrame(movieTexture);
    }
    destroyRuntime(state);
    audioOutput.reset();
    SDL_Quit();
    return 0;
}

} // namespace ke
