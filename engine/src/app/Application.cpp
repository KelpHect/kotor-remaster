#include "app/Application.h"

#include "core/Log.h"
#include "game/GameSession.h"
#include "kotor/Formats.h"
#include "render/Backend.h"
#include "script/Corpus.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ke {

namespace {

constexpr const char* kEngineVersion = "0.1.0";

struct RuntimeState {
    SDL_Window* window = nullptr;
    std::unique_ptr<IRenderBackend> backend;
    bool imguiReady = false;
    BackendKind activeBackend = BackendKind::Null;
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

    const std::filesystem::path savePath =
        std::filesystem::temp_directory_path() / "kotor_engine_s6_systems.save";
    game.scriptWorld().setLocal(game.scriptWorld().pc, "s6_local",
                                script::Value::makeInt(7));
    check(game.save(savePath), "save_written");
    GameSession restored(game.installPath(), game.module());
    check(restored.load(savePath), "save_loaded");
    check(restored.character().xp == game.character().xp, "xp_roundtrip");
    check(restored.character().level == game.character().level, "level_roundtrip");
    check(restored.inventory().size() == game.inventory().size(), "inventory_roundtrip");
    check(restored.partyTags().size() == game.partyTags().size(), "party_roundtrip");
    check(restored.module() == game.module(), "module_roundtrip");
    const script::Value local = restored.scriptWorld().getLocal(
        restored.scriptWorld().pc, "s6_local", script::ValueType::Int);
    check(local.integer == 7, "local_roundtrip");

    logInfo(ok ? "systems test: PASS" : "systems test: FAIL");
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

int runApplication(const Options& options)
{
    if (!options.vmCorpus.empty()) {
        return runVmCorpus(options);
    }
    if (options.selfTest) {
        return runImGuiSelfTest();
    }

    std::optional<GameSession> game;
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
            game.emplace(installPath, options.module);
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
                           ", waypoint ", countKind(ke::kotor::EntityKind::Waypoint), ")"));
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
                    && loaded.vertices.size() == 235038
                    && loaded.indices.size() == 235038
                    && loaded.batches.size() == 34
                    && loaded.textures.size() == 53
                    && loaded.collisionVertices.size() == 850
                    && loaded.collision.size() == 1365
                    && loaded.entities.size() == 218
                    && loaded.entityMeshes.size() == 101
                    && loaded.triggers.size() == 13
                    && loaded.warnings.empty();
                logInfo(concat("scene assert: rooms=", loaded.rooms.size(),
                               " vertices=", loaded.vertices.size(),
                               " batches=", loaded.batches.size(),
                               " textures=", loaded.textures.size(),
                               " collision=", loaded.collision.size(),
                               " entities=", loaded.entities.size(),
                               " meshes=", loaded.entityMeshes.size(),
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
            state.backend->setCamera(game->camera());
        }

        if (state.imguiReady) {
            state.backend->newFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            drawDebugUi(state, frames, smoothedFrameMs, elapsedSeconds, exitReason);
            if (game) {
                drawGameUi(*game, smoothedFrameMs > 0.0 ? static_cast<float>(1000.0 / smoothedFrameMs) : 0.0f,
                           game->interactionPrompt());
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
            destroyRuntime(state);
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

    destroyRuntime(state);
    SDL_Quit();
    return 0;
}

} // namespace ke
