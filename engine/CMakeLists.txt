cmake_minimum_required(VERSION 3.28)

project(kotor_engine
    VERSION 0.1.0
    DESCRIPTION "KotOR Remaster engine scaffold (Lane 08): C++23 + SDL3 + Vulkan 1.4 / OpenGL 4.5 + ImGui"
    LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(KOTOR_ENGINE_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(KOTOR_ENGINE_BUILD_TESTS "Register headless CTest smoke tests" ON)

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus)
endif()

include(cmake/Dependencies.cmake)

# ---------------------------------------------------------------------------
# First-party warning/toolchain interface
# ---------------------------------------------------------------------------
add_library(kotor_engine_options INTERFACE)
target_compile_features(kotor_engine_options INTERFACE cxx_std_23)
if(MSVC)
    target_compile_options(kotor_engine_options INTERFACE /W4 /EHsc)
    if(KOTOR_ENGINE_WARNINGS_AS_ERRORS)
        target_compile_options(kotor_engine_options INTERFACE /WX)
    endif()
else()
    target_compile_options(kotor_engine_options INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(KOTOR_ENGINE_WARNINGS_AS_ERRORS)
        target_compile_options(kotor_engine_options INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# ImGui 1.93.0 (no upstream CMakeLists; compile the core and pinned backends)
# ---------------------------------------------------------------------------
add_library(kotor_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_compile_features(kotor_imgui PUBLIC cxx_std_17)
target_include_directories(kotor_imgui
    PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${volk_SOURCE_DIR})
target_link_libraries(kotor_imgui PUBLIC SDL3::SDL3-static)
target_compile_definitions(kotor_imgui PRIVATE IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(kotor_imgui PRIVATE Vulkan::Headers volk::volk_headers)
if(MSVC)
    target_compile_options(kotor_imgui PRIVATE /W3)
endif()

# ---------------------------------------------------------------------------
# dr_mp3 (public domain/MIT-0): MP3 stream decode for KotOR audio.
# ---------------------------------------------------------------------------
add_library(kotor_dr_mp3 STATIC third_party/dr_libs/dr_mp3_impl.cpp)
target_include_directories(kotor_dr_mp3 PUBLIC third_party/dr_libs)
if(MSVC)
    target_compile_options(kotor_dr_mp3 PRIVATE /W0)
else()
    target_compile_options(kotor_dr_mp3 PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# KotOR data layer: archives, GFF, geometry, textures, scene construction.
# This is engine code, not a Python tool; the research tools remain oracles.
# ---------------------------------------------------------------------------
add_library(kotor_data STATIC
    src/kotor/MiniGameData.cpp
    src/kotor/Archive.cpp
    src/kotor/BinkDecoder.cpp
    src/kotor/Entities.cpp
    src/kotor/Gff.cpp
    src/kotor/GffWrite.cpp
    src/kotor/Geometry.cpp
    src/kotor/Gui.cpp
    src/kotor/Install.cpp
    src/kotor/ResourceMisses.cpp
    src/kotor/Localization.cpp
    src/kotor/Media.cpp
    src/kotor/Profile.cpp
    src/kotor/SaveGame.cpp
    src/kotor/Scene.cpp
    src/kotor/Texture.cpp
    src/kotor/Material.cpp
    src/kotor/Particles.cpp
    src/kotor/Tlk.cpp
    src/kotor/TwoDa.cpp)
target_include_directories(kotor_data PUBLIC src)
target_link_libraries(kotor_data PUBLIC kotor_engine_options)
if(TARGET kotor_ffmpeg)
    target_link_libraries(kotor_data PUBLIC kotor_ffmpeg)
endif()

# ---------------------------------------------------------------------------
# Audio: WAV/MP3 decode, deterministic buses and EAX-style reverb.
# ---------------------------------------------------------------------------
add_library(kotor_audio STATIC
    src/audio/Audio.cpp
    src/audio/AudioReference.cpp
    src/audio/Output.cpp
    src/audio/Reverb.cpp
    src/audio/ReverbGate.cpp)
target_include_directories(kotor_audio PUBLIC src)
target_link_libraries(kotor_audio PUBLIC kotor_engine_options kotor_data kotor_dr_mp3
                                              SDL3::SDL3-static)

# ---------------------------------------------------------------------------
# Script runtime: native NCS VM, generated action table and action model.
# ---------------------------------------------------------------------------
add_library(kotor_script STATIC
    src/script/ActionTable.cpp
    src/script/ActionRuntime.cpp
    src/script/Corpus.cpp
    src/script/Ncs.cpp
    src/script/Random.cpp
    src/script/Vm.cpp
    src/script/World.cpp)
target_include_directories(kotor_script PUBLIC src)
target_link_libraries(kotor_script PUBLIC kotor_engine_options)

# ---------------------------------------------------------------------------
# Engine executable: both GPU backends are built; selection is runtime.
# ---------------------------------------------------------------------------
add_executable(kotor_engine
    src/main.cpp
    src/app/Application.cpp
    src/app/Args.cpp
    src/app/InputScript.cpp
    src/app/Soak.cpp
    src/app/WalkTools.cpp
    src/core/Log.cpp
    src/game/Ambience.cpp
    src/game/AmbienceFamily.cpp
    src/game/WorldFamily.cpp
    src/game/TextureAudit.cpp
    src/game/AnimationFamily.cpp
    src/game/AudioSystem.cpp
    src/game/Chargen.cpp
    src/game/WalkSupport.cpp
    src/game/ChargenGui.cpp
    src/game/ChargenPreview.cpp
    src/game/FacialRig.cpp
    src/game/Factions.cpp
    src/game/FrontEnd.cpp
    src/game/FrontEndGate.cpp
    src/game/GameSession.cpp
    src/game/GuiFrame.cpp
    src/game/GuiPaint.cpp
    src/game/GuiSystem.cpp
    src/game/Hud.cpp
    src/game/GameScreens.cpp
    src/game/GameScreensMore.cpp
    src/game/GameScreensExtra.cpp
    src/game/GameScreensPazaak.cpp
    src/game/UpgradeBench.cpp
    src/game/DialogueScreen.cpp
    src/game/MenuScene.cpp
    src/game/Minigames.cpp
    src/game/MiniGame3D.cpp
    src/game/ScriptMiniGame.cpp
    src/game/Rules.cpp
    src/game/CinemaFamily.cpp
    src/game/AudioQualityFamily.cpp
    src/game/Effects.cpp
    src/game/EffectsFamily.cpp
    src/game/Pathfinding.cpp
    src/game/QuestGraph.cpp
    src/game/QuestTrace.cpp
    src/game/QuestFlow.cpp
    src/game/ScriptActions.cpp
    src/game/ScriptCombat.cpp
    src/game/ScriptEffects.cpp
    src/game/ScriptEvents.cpp
    src/game/ScriptHost.cpp
    src/game/ScriptItems.cpp
    src/game/ScriptObjects.cpp
    src/game/ScriptParty.cpp
    src/game/PartyControl.cpp
    src/game/ScriptAmbience.cpp
    src/game/ScriptVideoEffect.cpp
    src/game/ScriptWorldState.cpp
    src/game/EncounterPlanner.cpp
    src/game/VideoEffectController.cpp
    src/game/WorldPersistence.cpp
    src/render/BackendFactory.cpp
    src/render/Bmp.cpp
    src/render/NullBackend.cpp
    src/render/GlBackend.cpp
    src/render/GlScene.cpp
    src/render/VideoEffect.cpp
    src/render/VulkanBackend.cpp
    src/render/VulkanPost.cpp
    src/render/VulkanScene.cpp)

target_include_directories(kotor_engine PRIVATE src)
target_link_libraries(kotor_engine PRIVATE
    kotor_engine_options
    kotor_data
    kotor_audio
    kotor_script
    kotor_imgui
    SDL3::SDL3-static
    Vulkan::Headers
    volk::volk)
target_compile_definitions(kotor_engine PRIVATE
    VK_NO_PROTOTYPES
    KE_ENABLE_VULKAN=1
    KE_ENABLE_OPENGL=1)

if(WIN32)
    target_link_libraries(kotor_engine PRIVATE opengl32)
    set_target_properties(kotor_engine PROPERTIES WIN32_EXECUTABLE OFF)
endif()

if(TARGET kotor_ffmpeg)
    file(GLOB KOTOR_FFMPEG_DLLS
        "${KOTOR_FFMPEG_ROOT}/bin/avcodec-*.dll"
        "${KOTOR_FFMPEG_ROOT}/bin/avformat-*.dll"
        "${KOTOR_FFMPEG_ROOT}/bin/avutil-*.dll"
        "${KOTOR_FFMPEG_ROOT}/bin/swresample-*.dll"
        "${KOTOR_FFMPEG_ROOT}/bin/swscale-*.dll")
    add_custom_command(TARGET kotor_engine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${KOTOR_FFMPEG_DLLS} "$<TARGET_FILE_DIR:kotor_engine>"
        COMMENT "Copying FFmpeg runtime DLLs")
endif()

# Keep the shader folder beside the executable in sync on every build; the
# SPIR-V is committed, so a stale configure-time copy would silently run the
# wrong push-constant layout.
add_custom_command(TARGET kotor_engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders
            ${CMAKE_BINARY_DIR}/shaders
    COMMENT "Copying scene shaders")

# ---------------------------------------------------------------------------
# Tests (headless; no GPU required)
# ---------------------------------------------------------------------------
if(KOTOR_ENGINE_BUILD_TESTS)
    enable_testing()
    add_executable(kotor_tests tests/test_kotor.cpp)
    target_link_libraries(kotor_tests PRIVATE kotor_engine_options kotor_data kotor_script)
    add_test(NAME kotor_formats COMMAND kotor_tests)
    add_executable(kotor_vm_conformance tests/vm_conformance.cpp)
    target_link_libraries(kotor_vm_conformance PRIVATE kotor_engine_options kotor_script)
    add_test(NAME kotor_vm_conformance COMMAND kotor_vm_conformance)
    # Negative check: with every expected result perturbed, every case must fail.
    add_test(NAME kotor_vm_conformance_negative COMMAND kotor_vm_conformance --negative)
    set_tests_properties(kotor_vm_conformance_negative PROPERTIES
                         PASS_REGULAR_EXPRESSION "VM_CONFORMANCE_NEGATIVE_PASS")

    add_test(NAME smoke_null_headless
             COMMAND kotor_engine --headless --frames 5 --seconds 20)
    set_tests_properties(smoke_null_headless PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy"
        TIMEOUT 30)
    add_test(NAME smoke_imgui_selftest COMMAND kotor_engine --self-test)
    set_tests_properties(smoke_imgui_selftest PROPERTIES TIMEOUT 30)

    # Module integration tests use the user's read-only install. The path can
    # be overridden with KOTOR_INSTALL at configure time; absent installs skip.
    set(KOTOR_TEST_INSTALL "$ENV{KOTOR_INSTALL}")
    if(NOT KOTOR_TEST_INSTALL)
        set(KOTOR_TEST_INSTALL "E:/SteamLibrary/steamapps/common/swkotor")
    endif()
    if(EXISTS "${KOTOR_TEST_INSTALL}/chitin.key")
        add_test(NAME kotor_scene_stats
                 COMMAND kotor_engine --module end_m01aa --scene-stats --null --headless)
        add_test(NAME kotor_scene_assert
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_scene_assert/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_scene_assert/settings.ini"
                          --module end_m01aa --scene-assert --null --headless)
        add_test(NAME kotor_door_transition
                 COMMAND kotor_engine --module tar_m02af --door-test --null --headless)
        add_test(NAME kotor_taris_transition
                 COMMAND kotor_engine --module tar_m02af --door-test --null --headless)
        add_test(NAME kotor_walk_collision
                 COMMAND kotor_engine --module end_m01aa --walk-test --null --headless)
        # Walk tooling (app/WalkTools.h, tests/walk/STEPS.md): a script plays the opening through the player's own
        # paths. The script holds the part of the route that passes today; it only grows. The full route to the
        # next stop is tests/walk/opening_full.walk (not a test until it passes).
        add_test(NAME kotor_walk_opening
                 COMMAND kotor_engine --new-game --input-script "${CMAKE_CURRENT_SOURCE_DIR}/tests/walk/opening.walk"
                         --null --headless --fixed-dt 0.0166667 --no-audio --no-movies
                         --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_walk_opening/saves"
                         --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_walk_opening/settings.ini")
        set_tests_properties(kotor_walk_opening PROPERTIES
            ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}"
            PASS_REGULAR_EXPRESSION "WALK RESULT PASS"
            TIMEOUT 300)
        add_test(NAME kotor_dialogue
                 COMMAND kotor_engine --module end_m01aa --dialogue-test --null --headless)
        add_test(NAME kotor_family_pathfinding
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_pathfinding/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_pathfinding/settings.ini"
                          --module end_m01aa --family-test pathfinding --null --headless)
        add_test(NAME kotor_family_worldstate
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_worldstate/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_worldstate/settings.ini"
                          --module end_m01aa --family-test worldstate --null --headless)
        set_tests_properties(kotor_family_worldstate PROPERTIES TIMEOUT 900)
        add_test(NAME kotor_family_items
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_items/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_items/settings.ini"
                          --module end_m01aa --family-test items --null --headless)
        add_test(NAME kotor_family_party
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_party/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_party/settings.ini"
                          --module end_m01aa --family-test party --null --headless)
        add_test(NAME kotor_family_companion
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_companion/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_companion/settings.ini"
                          --module end_m01aa --family-test companion --null --headless)
        add_test(NAME kotor_family_animation
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_animation/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_animation/settings.ini"
                          --module end_m01aa --family-test animation --null --headless)
        set_tests_properties(kotor_family_animation PROPERTIES TIMEOUT 900)
        add_test(NAME kotor_family_combat
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_combat/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_combat/settings.ini"
                          --module end_m01aa --family-test combat --null --headless)
        set_tests_properties(kotor_family_combat PROPERTIES TIMEOUT 900)
        add_test(NAME kotor_family_objects
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_objects/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_objects/settings.ini"
                          --module end_m01aa --family-test objects --null --headless)
        add_test(NAME kotor_family_videoeffect
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_videoeffect/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_videoeffect/settings.ini"
                          --module end_m01aa --family-test videoeffect
                         --video-effect-fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
                         --null --headless)
        add_test(NAME kotor_family_endgame
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_endgame/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_endgame/settings.ini"
                          --module end_m01aa --family-test endgame --no-movies --null --headless)
        set_tests_properties(kotor_family_endgame PROPERTIES TIMEOUT 600)
        add_test(NAME kotor_family_world3d
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_world3d/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_world3d/settings.ini"
                          --module end_m01aa --family-test world3d --null --headless)
        set_tests_properties(kotor_family_world3d PROPERTIES TIMEOUT 900
                             ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}")
        add_test(NAME kotor_family_worldposes
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_worldposes/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_worldposes/settings.ini"
                          --module end_m01aa --family-test worldposes --null --headless)
        set_tests_properties(kotor_family_worldposes PROPERTIES TIMEOUT 1800
                             ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}")
        foreach(backend gl vulkan)
            add_test(NAME kotor_scene_sync_${backend}
                     COMMAND kotor_engine --${backend} --no-fallback --module end_m01aa --frames 40 --no-movies
                             --capture-camera creature:0 --capture-action equip:0:w_vbroswrd_001@20)
            set_tests_properties(kotor_scene_sync_${backend} PROPERTIES TIMEOUT 300
                                 ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}"
                                 PASS_REGULAR_EXPRESSION "scene sync: meshes reused [1-9][0-9]* uploaded [1-9]"
                                 FAIL_REGULAR_EXPRESSION "\\[ERROR\\]")
        endforeach()
        # The GL pixel format is fixed at window creation: asking later left a 16-bit depth buffer.
        add_test(NAME kotor_gl_depth_bits
                 COMMAND kotor_engine --opengl --no-fallback --module end_m01aa --frames 3 --no-movies)
        set_tests_properties(kotor_gl_depth_bits PROPERTIES TIMEOUT 120
                             ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}"
                             PASS_REGULAR_EXPRESSION "GL depth buffer bits: (24|32)")
        add_test(NAME kotor_family_materials
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_materials/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_materials/settings.ini"
                          --module end_m01aa --family-test materials --null --headless)
        set_tests_properties(kotor_family_materials PROPERTIES TIMEOUT 600
                             ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}")
        add_test(NAME kotor_family_worldtextures
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_worldtextures/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_worldtextures/settings.ini"
                          --module end_m01aa --family-test worldtextures --null --headless)
        set_tests_properties(kotor_family_worldtextures PROPERTIES TIMEOUT 1800
                             ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}")
        add_test(NAME kotor_family_ambience
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_ambience/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_ambience/settings.ini"
                          --module end_m01aa --family-test ambience --null --headless)
        set_tests_properties(kotor_family_ambience PROPERTIES TIMEOUT 900
                             ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}")
        add_test(NAME kotor_family_audioquality
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_audioquality/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_audioquality/settings.ini"
                          --module end_m01aa --family-test audioquality --null --headless)
        set_tests_properties(kotor_family_audioquality PROPERTIES TIMEOUT 1800
                             ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}")
        add_test(NAME kotor_family_cinema
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_cinema/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_cinema/settings.ini"
                          --module end_m01aa --family-test cinema --null --headless)
        set_tests_properties(kotor_family_cinema PROPERTIES TIMEOUT 900
                             ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}")
        add_test(NAME kotor_family_sfx
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_sfx/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_sfx/settings.ini"
                          --module end_m01aa --family-test sfx --null --headless)
        set_tests_properties(kotor_family_sfx PROPERTIES TIMEOUT 900
                             ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}")
        add_test(NAME kotor_family_actions
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_actions/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_actions/settings.ini"
                          --module end_m01aa --family-test actions --null --headless)
        add_test(NAME kotor_family_minigame3d
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_minigame3d/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_minigame3d/settings.ini"
                          --module end_m01aa --family-test minigame3d --null --headless)
        set_tests_properties(kotor_family_minigame3d PROPERTIES TIMEOUT 900)
        foreach(backend gl vulkan)
            foreach(kind swoop turret)
                add_test(NAME kotor_family_minigame3d_${kind}_${backend}
                         COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_family_minigame3d_${kind}_${backend}/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_family_minigame3d_${kind}_${backend}/settings.ini"
                                  --${backend} --no-fallback --start-minigame ${kind} --no-movies
                                 --minigame-render-check --frames 120)
                set_tests_properties(kotor_family_minigame3d_${kind}_${backend} PROPERTIES TIMEOUT 300)
            endforeach()
        endforeach()
        # The quest tracer: one process, about 15 minutes. docs/recon/64_QUEST_TRACE.md.
        add_test(NAME kotor_quest_trace
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_quest_trace/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_quest_trace/settings.ini"
                         --module end_m01aa --quest-trace "${CMAKE_CURRENT_BINARY_DIR}/quest_trace"
                         --quest-trace-baseline "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/quest_trace_baseline.tsv"
                         --no-movies --null --headless)
        set_tests_properties(kotor_quest_trace PROPERTIES TIMEOUT 1200
                             ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}")
        # Negative check: a test-only fault must make the gate fail on engine edges.
        add_test(NAME kotor_quest_trace_negative
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_quest_trace_negative/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_quest_trace_negative/settings.ini"
                         --module end_m01aa --quest-trace "${CMAKE_CURRENT_BINARY_DIR}/quest_trace_negative"
                         --quest-trace-baseline "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/quest_trace_baseline.tsv"
                         --quest-trace-modules manm27aa --quest-trace-break journal
                         --no-movies --null --headless)
        set_tests_properties(kotor_quest_trace_negative PROPERTIES TIMEOUT 300
                             ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}"
                             PASS_REGULAR_EXPRESSION "quest trace gate: FAIL engine_fail=[1-9]")
        # Sequence proofs on one session: docs/recon/64_QUEST_TRACE.md section 9.
        add_test(NAME kotor_conversation_flow
                 COMMAND kotor_engine --module end_m01aa --conversation-flow "${CMAKE_CURRENT_BINARY_DIR}/conversation_flow"
                         --quest-trace-baseline "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/conversation_flow_baseline.tsv"
                         --no-movies --null --headless)
        set_tests_properties(kotor_conversation_flow PROPERTIES TIMEOUT 900
                             ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}")
        # Negative check: global writes dropped must make the walk see missing effects.
        add_test(NAME kotor_conversation_flow_negative
                 COMMAND kotor_engine --module end_m01aa --conversation-flow "${CMAKE_CURRENT_BINARY_DIR}/conversation_flow_negative"
                         --quest-trace-baseline "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/conversation_flow_baseline.tsv"
                         --quest-trace-modules end_m01aa,end_m01ab --quest-trace-break globals
                         --no-movies --null --headless)
        set_tests_properties(kotor_conversation_flow_negative PROPERTIES TIMEOUT 300
                             ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}"
                             PASS_REGULAR_EXPRESSION "conversation flow gate: FAIL fail=[1-9]")
        add_test(NAME kotor_plot_replay
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_plot_replay/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_plot_replay/settings.ini"
                         --module end_m01aa --plot-replay "${CMAKE_CURRENT_BINARY_DIR}/plot_replay"
                         --quest-trace-baseline "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/plot_replay_baseline.tsv"
                         --no-movies --null --headless)
        set_tests_properties(kotor_plot_replay PROPERTIES TIMEOUT 900
                             ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}")
        # Negative check: a setter the plot needs is never run, so a later step's real condition fails.
        add_test(NAME kotor_plot_replay_negative
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_plot_replay_negative/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_plot_replay_negative/settings.ini"
                         --module end_m01aa --plot-replay "${CMAKE_CURRENT_BINARY_DIR}/plot_replay_negative"
                         --quest-trace-baseline "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/plot_replay_baseline.tsv"
                         --quest-trace-break K_CAPTURED_LEV
                         --no-movies --null --headless)
        set_tests_properties(kotor_plot_replay_negative PROPERTIES TIMEOUT 900
                             ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}"
                             PASS_REGULAR_EXPRESSION "plot replay gate: FAIL fail=[1-9]")
        add_test(NAME kotor_world_sim
                 COMMAND kotor_engine --module end_m01aa --world-sim-test --null --headless)
        add_test(NAME kotor_combat
                 COMMAND kotor_engine --module end_m01aa --combat-test --null --headless)
        add_test(NAME kotor_script
                 COMMAND kotor_engine --module end_m01aa --script-test --null --headless)
        add_test(NAME kotor_rules
                 COMMAND kotor_engine --module end_m01aa --rules-test --null --headless)
        add_test(NAME kotor_systems
                 COMMAND kotor_engine --module end_m01aa --systems-test --null --headless)
        add_test(NAME kotor_merchant
                 COMMAND kotor_engine --module tar_m02ac --merchant-test --null --headless)
        add_test(NAME kotor_encounter
                 COMMAND kotor_engine --module tar_m03aa --encounter-test --null --headless)
        add_test(NAME kotor_encounter_reset
                 COMMAND kotor_engine --module tar_m03ab --encounter-reset-test --null --headless)
        add_test(NAME kotor_party
                 COMMAND kotor_engine --module end_m01aa --party-test --null --headless)
        add_test(NAME kotor_ui
                 COMMAND kotor_engine --module end_m01aa --ui-test --null --headless)
        add_test(NAME kotor_loot
                 COMMAND kotor_engine --module danm13 --loot-test --null --headless)
        add_test(NAME kotor_delayed_functors
                 COMMAND kotor_engine --module end_m01aa
                         --delayed-test "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/delay_test.ncs"
                         --null --headless)
        add_test(NAME kotor_combat_force
                 COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_combat_force/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_combat_force/settings.ini"
                         --module end_m01aa --combat-force-test
                         --combat-force-script "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/force_script_test.ncs"
                         --null --headless)
        add_test(NAME kotor_animation
                 COMMAND kotor_engine --module end_m01aa --animation-test
                         --animation-script "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/anim_script_test.ncs"
                         --null --headless)
        add_test(NAME kotor_audio
                 COMMAND kotor_engine --module end_m01aa --audio-test
                         --audio-script "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/audio_script_test.ncs"
                         --null --headless)
        add_test(NAME kotor_media
                 COMMAND kotor_engine --module end_m01aa --media-test --null --headless)
        add_test(NAME kotor_gui
                 COMMAND kotor_engine --module end_m01aa --gui-test --null --headless)
        add_test(NAME kotor_minigame
                 COMMAND kotor_engine --module end_m01aa --minigame-test
                         --minigame-script "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/minigame_script_test.ncs"
                         --null --headless)
        add_test(NAME kotor_localization
                 COMMAND kotor_engine --module end_m01aa --localization-test --null --headless)
        add_test(NAME kotor_faction
                 COMMAND kotor_engine --module end_m01aa --faction-test
                         --faction-script "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/faction_script_test.ncs"
                         --null --headless)
        add_test(NAME kotor_vfs
                 COMMAND kotor_engine --module end_m01aa --vfs-test
                         --vfs-fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/vfs"
                         --null --headless)
        add_test(NAME kotor_playthrough
                 COMMAND kotor_engine --module end_m01aa --playthrough --null --headless)
        add_test(NAME kotor_landmark
                 COMMAND kotor_engine --module end_m01aa --landmark-test --null --headless)
        add_test(NAME kotor_save_load
                 COMMAND kotor_engine --module end_m01aa --walk-test --save "${CMAKE_BINARY_DIR}/engine-save.txt" --null --headless)
        set(KOTOR_SAVE_FIXTURE "${CMAKE_SOURCE_DIR}/../artifacts/lanes/06-dynamic/profile/saves/000000 - QUICKSAVE")
        if(EXISTS "${KOTOR_SAVE_FIXTURE}/SAVEGAME.sav")
            add_test(NAME kotor_original_save_load
                     COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_original_save_load/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_original_save_load/settings.ini"
                         --original-save-load "${KOTOR_SAVE_FIXTURE}" --null --headless)
            add_test(NAME kotor_original_save_roundtrip
                     COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_original_save_roundtrip/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_original_save_roundtrip/settings.ini"
                         --original-save-roundtrip "${KOTOR_SAVE_FIXTURE}"
                             --original-save-out "${CMAKE_BINARY_DIR}/000009 - ENGINE" --null --headless)
            set_tests_properties(kotor_original_save_load kotor_original_save_roundtrip
                                 PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 300)
        endif()
        add_test(NAME kotor_original_save_create
                 COMMAND kotor_engine --module end_m01aa
                         --original-save-create "${CMAKE_BINARY_DIR}/000010 - ENGINE" --null --headless)
        set(KOTOR_SAVE_PARTY_FIXTURE "${CMAKE_SOURCE_DIR}/../artifacts/lanes/06-dynamic/profile/saves/000006 - TARISLC")
        if(EXISTS "${KOTOR_SAVE_PARTY_FIXTURE}/SAVEGAME.sav")
            add_test(NAME kotor_original_save_party_roundtrip
                     COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_original_save_party_roundtrip/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_original_save_party_roundtrip/settings.ini"
                         --original-save-roundtrip "${KOTOR_SAVE_PARTY_FIXTURE}"
                             --original-save-out "${CMAKE_BINARY_DIR}/000011 - ENGINE" --null --headless)
            set_tests_properties(kotor_original_save_party_roundtrip
                                 PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 300)
        endif()
        set(KOTOR_SAVE_MATRIX_DIR "${CMAKE_SOURCE_DIR}/../artifacts/lanes/06-dynamic/profile/saves")
        if(EXISTS "${KOTOR_SAVE_MATRIX_DIR}")
            add_test(NAME kotor_save_matrix
                     COMMAND kotor_engine --module end_m01aa --save-matrix-test
                             --save-fixtures "${KOTOR_SAVE_MATRIX_DIR}" --null --headless)
            set_tests_properties(kotor_save_matrix
                                 PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 300)
            add_test(NAME kotor_differential
                     COMMAND kotor_engine --module end_m01aa --differential-test
                             --save-fixtures "${KOTOR_SAVE_MATRIX_DIR}" --null --headless)
            set_tests_properties(kotor_differential
                                 PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 900)
        endif()
        add_test(NAME kotor_dialogue_corpus
                 COMMAND kotor_engine --module end_m01aa --dialogue-corpus-test --null --headless)
        set_tests_properties(kotor_dialogue_corpus
                             PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 300)
        add_test(NAME kotor_bug_parity
                 COMMAND kotor_engine --module end_m01aa --bug-parity-test --null --headless)
        set_tests_properties(kotor_bug_parity
                             PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 120)
        add_test(NAME kotor_chargen
                 COMMAND kotor_engine --module end_m01aa --chargen-test --null --headless
                         --saves-dir "${CMAKE_BINARY_DIR}/chargen/saves" --settings-file "${CMAKE_BINARY_DIR}/chargen/settings.ini")
        set_tests_properties(kotor_chargen
                             PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 120)
        set(KOTOR_CAMPAIGN_SAVES "${CMAKE_SOURCE_DIR}/../artifacts/lanes/06-dynamic/profile/saves")
        if(EXISTS "${KOTOR_CAMPAIGN_SAVES}")
            add_test(NAME kotor_campaign
                     COMMAND kotor_engine --module end_m01aa --campaign-test
                             --save-fixtures "${KOTOR_CAMPAIGN_SAVES}" --null --headless)
            set_tests_properties(kotor_campaign
                                 PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 600)
        endif()
        add_test(NAME kotor_avatar
                 COMMAND kotor_engine --module end_m01aa --avatar-test --null --headless)
        set_tests_properties(kotor_avatar
                             PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 180)
        add_test(NAME kotor_chargen_gui
                 COMMAND kotor_engine --module end_m01aa --chargen-gui-test --null --headless
                         --saves-dir "${CMAKE_BINARY_DIR}/chargen_gui/saves" --settings-file "${CMAKE_BINARY_DIR}/chargen_gui/settings.ini")
        set_tests_properties(kotor_chargen_gui
                             PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 180)
        # Pixel gate for the 3D chargen controls: needs a real GL / Vulkan device and fails
        # (never skips) when the requested backend cannot start. The window stays visible:
        # a hidden window presents nothing to read back on these drivers.
        add_test(NAME kotor_chargen_preview_gl
                 COMMAND kotor_engine --module end_m01aa --chargen-preview-test --opengl
                         --no-fallback --width 1280 --height 960
                         --chargen-preview-out "${CMAKE_BINARY_DIR}/chargen_preview/gl"
                         --saves-dir "${CMAKE_BINARY_DIR}/chargen_preview/gl/saves" --settings-file "${CMAKE_BINARY_DIR}/chargen_preview/gl/settings.ini")
        set_tests_properties(kotor_chargen_preview_gl
                             PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 300)
        add_test(NAME kotor_chargen_preview_vulkan
                 COMMAND kotor_engine --module end_m01aa --chargen-preview-test --vulkan
                         --no-fallback --width 1280 --height 960
                         --chargen-preview-out "${CMAKE_BINARY_DIR}/chargen_preview/vulkan"
                         --saves-dir "${CMAKE_BINARY_DIR}/chargen_preview/vulkan/saves" --settings-file "${CMAKE_BINARY_DIR}/chargen_preview/vulkan/settings.ini")
        set_tests_properties(kotor_chargen_preview_vulkan
                             PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 300)
        # Pixel gate for texture wrap: a shipped room triangle whose UVs run past 1 is drawn
        # with a probe texture under each wrap setting and the frame is measured. Needs a
        # real GL / Vulkan device; fails (never skips) when the backend cannot start.
        # Leak gate: sustained growth per cycle after warm-up, this process only (app/Soak.h).
        set(KOTOR_SOAK_ARGS --module tar_m03aa --fixed-dt 0.0166667 --no-audio
            --soak-modules danm13,kas_m25aa,sta_m45aa,end_m01aa
            --soak-out "${CMAKE_CURRENT_BINARY_DIR}/soak")
        add_test(NAME kotor_soak_idle COMMAND kotor_engine ${KOTOR_SOAK_ARGS} --soak idle --null --headless)
        add_test(NAME kotor_soak_modules COMMAND kotor_engine ${KOTOR_SOAK_ARGS} --soak modules --null --headless)
        add_test(NAME kotor_soak_ui COMMAND kotor_engine ${KOTOR_SOAK_ARGS} --soak ui --gui-screen mainmenu --null --headless)
        set_tests_properties(kotor_soak_idle kotor_soak_modules kotor_soak_ui
                             PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 600)
        foreach(backend gl vulkan)
            add_test(NAME kotor_texture_wrap_${backend}
                     COMMAND kotor_engine --saves-dir "${CMAKE_BINARY_DIR}/test_profile/kotor_texture_wrap_${backend}/saves" --settings-file "${CMAKE_BINARY_DIR}/test_profile/kotor_texture_wrap_${backend}/settings.ini"
                              --module end_m01aa --texture-wrap-check --${backend}
                             --no-fallback --width 1280 --height 720 --no-movies --frames 3500)
            set_tests_properties(kotor_texture_wrap_${backend}
                                 PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 300)
        endforeach()
        # New Game from the main menu through the shipped chargen to end_m01aa, driven by
        # pointer events (starts in end_m01ab so the module change is observable).
        foreach(backend gl vulkan)
            add_test(NAME kotor_new_game_flow_${backend}
                     COMMAND kotor_engine --module end_m01ab --new-game-flow-test --${backend}
                             --no-fallback --width 1280 --height 960
                             --chargen-preview-out "${CMAKE_BINARY_DIR}/new_game_flow/${backend}"
                             --saves-dir "${CMAKE_BINARY_DIR}/new_game_flow/${backend}/saves"
                             --settings-file "${CMAKE_BINARY_DIR}/new_game_flow/${backend}/settings.ini")
            set_tests_properties(kotor_new_game_flow_${backend}
                                 PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 300)
        endforeach()
        # The shipped front end (main menu, options, load game, movies) driven by real SDL
        # pointer, wheel and key events; saves and settings go to the build folder.
        foreach(backend gl vulkan)
            add_test(NAME kotor_front_end_${backend}
                     COMMAND kotor_engine --module end_m01aa --front-end-test --${backend}
                             --no-fallback --width 1280 --height 720 --no-audio --no-movies --hidden
                             --saves-dir "${CMAKE_BINARY_DIR}/front_end/${backend}/saves"
                             --settings-file "${CMAKE_BINARY_DIR}/front_end/${backend}/settings.ini"
                             --front-end-out "${CMAKE_BINARY_DIR}/front_end/${backend}/frames")
            set_tests_properties(kotor_front_end_${backend}
                                 PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 300)
            # The shipped store screen (merchants on tar_m02ac) and the loading screen of a module change.
            add_test(NAME kotor_front_end_store_${backend}
                     COMMAND kotor_engine --module tar_m02ac --front-end-test --front-end-scenario store --${backend}
                             --no-fallback --width 1280 --height 720 --no-audio --no-movies --hidden
                             --saves-dir "${CMAKE_BINARY_DIR}/front_end_store/${backend}/saves"
                             --settings-file "${CMAKE_BINARY_DIR}/front_end_store/${backend}/settings.ini"
                             --front-end-out "${CMAKE_BINARY_DIR}/front_end_store/${backend}/frames")
            set_tests_properties(kotor_front_end_store_${backend}
                                 PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 300)
            # The HUD minimap in the running engine over a sample of every planet's modules.
            add_test(NAME kotor_front_end_maps_${backend}
                     COMMAND kotor_engine --module end_m01aa --front-end-test --front-end-scenario maps --${backend}
                             --no-fallback --width 1280 --height 720 --no-audio --no-movies --hidden
                             --saves-dir "${CMAKE_BINARY_DIR}/front_end_maps/${backend}/saves"
                             --settings-file "${CMAKE_BINARY_DIR}/front_end_maps/${backend}/settings.ini"
                             --front-end-out "${CMAKE_BINARY_DIR}/front_end_maps/${backend}/frames")
            set_tests_properties(kotor_front_end_maps_${backend}
                                 PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 1200)
        endforeach()
        add_test(NAME kotor_profile_k1
                 COMMAND kotor_engine --profile-info "${KOTOR_TEST_INSTALL}" --null --headless)
        set(KOTOR2_TEST_INSTALL "$ENV{KOTOR2_INSTALL}")
        if(NOT KOTOR2_TEST_INSTALL)
            set(KOTOR2_TEST_INSTALL "E:/SteamLibrary/steamapps/common/Knights of the Old Republic II")
        endif()
        if(EXISTS "${KOTOR2_TEST_INSTALL}/swkotor2.exe")
            add_test(NAME kotor_profile_k2
                     COMMAND kotor_engine --profile-info "${KOTOR2_TEST_INSTALL}" --null --headless)
            add_test(NAME kotor_world_k2
                     COMMAND kotor_engine --install "${KOTOR2_TEST_INSTALL}" --module 001EBO
                             --k2-test --null --headless
                             --saves-dir "${CMAKE_BINARY_DIR}/world_k2/saves"
                             --settings-file "${CMAKE_BINARY_DIR}/world_k2/settings.ini")
            set_tests_properties(kotor_world_k2 PROPERTIES TIMEOUT 300)
        endif()
        set_tests_properties(kotor_scene_stats kotor_scene_assert kotor_door_transition
                             kotor_taris_transition kotor_walk_collision kotor_dialogue kotor_world_sim
                             kotor_combat kotor_script kotor_rules kotor_systems
                             kotor_family_items kotor_family_party kotor_family_endgame kotor_family_actions kotor_family_companion kotor_family_objects kotor_family_combat kotor_merchant kotor_encounter kotor_encounter_reset kotor_party kotor_ui kotor_loot
                             kotor_delayed_functors kotor_combat_force kotor_animation kotor_audio kotor_media kotor_gui kotor_minigame kotor_localization kotor_vfs kotor_faction
                             kotor_playthrough kotor_landmark kotor_save_load kotor_original_save_create kotor_profile_k1
            PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 300)
        if(EXISTS "${KOTOR2_TEST_INSTALL}/swkotor2.exe")
            set_tests_properties(kotor_profile_k2 PROPERTIES TIMEOUT 30)
        endif()
        # Native VM parity against the verified Python oracle. The corpus and
        # oracles live under artifacts/ (not shipped); skip the test when a
        # checkout has no research artifacts.
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME kotor_original_save_validate_create
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/../tools/resources/kotor_save.py
                             validate "${CMAKE_BINARY_DIR}/000010 - ENGINE")
            set_tests_properties(kotor_original_save_validate_create PROPERTIES
                                 DEPENDS kotor_original_save_create TIMEOUT 120)
            if(EXISTS "${KOTOR_SAVE_FIXTURE}/SAVEGAME.sav")
                add_test(NAME kotor_original_save_validate_roundtrip
                         COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/../tools/resources/kotor_save.py
                                 validate "${CMAKE_BINARY_DIR}/000009 - ENGINE")
                set_tests_properties(kotor_original_save_validate_roundtrip PROPERTIES
                                     DEPENDS kotor_original_save_roundtrip TIMEOUT 120)
            endif()
            if(EXISTS "${KOTOR_SAVE_PARTY_FIXTURE}/SAVEGAME.sav")
                add_test(NAME kotor_original_save_validate_party
                         COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/../tools/resources/kotor_save.py
                                 validate "${CMAKE_BINARY_DIR}/000011 - ENGINE")
                set_tests_properties(kotor_original_save_validate_party PROPERTIES
                                     DEPENDS kotor_original_save_party_roundtrip TIMEOUT 120)
            endif()
        endif()
        set(KOTOR_VM_CORPUS "${CMAKE_SOURCE_DIR}/../artifacts/lanes/04-ncs/extracted/unique")
        set(KOTOR_VM_DEFAULT_ORACLE "${CMAKE_SOURCE_DIR}/../artifacts/lanes/s6-01-vm/default_oracle_s6.json")
        if(EXISTS "${KOTOR_VM_CORPUS}")
            add_test(NAME kotor_action_coverage
                     COMMAND kotor_engine --module end_m01aa
                             --action-coverage "${CMAKE_BINARY_DIR}/action_coverage.tsv"
                             --action-corpus "${KOTOR_VM_CORPUS}" --null --headless)
            set_tests_properties(kotor_action_coverage PROPERTIES
                                 ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 300)
        endif()
        set(KOTOR_VM_FUNCTIONAL_ORACLE "${CMAKE_SOURCE_DIR}/../artifacts/lanes/s6-02-actions/functional_oracle_s6.json")
        if(Python3_Interpreter_FOUND AND EXISTS "${KOTOR_VM_CORPUS}" AND EXISTS "${KOTOR_VM_DEFAULT_ORACLE}")
            add_test(NAME kotor_vm_corpus
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_vm_corpus.py
                             --engine $<TARGET_FILE:kotor_engine>
                             --corpus "${KOTOR_VM_CORPUS}"
                             --default-oracle "${KOTOR_VM_DEFAULT_ORACLE}"
                             --functional-oracle "${KOTOR_VM_FUNCTIONAL_ORACLE}"
                             --out "${CMAKE_BINARY_DIR}/s6-vm-parity.json")
            set_tests_properties(kotor_vm_corpus PROPERTIES TIMEOUT 600)
        endif()

        if(Python3_Interpreter_FOUND)
            # GL and Vulkan draw the same world. Fails when either backend cannot start.
            add_test(NAME kotor_backend_parity_vulkan
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_backend_parity.py
                             --exe $<TARGET_FILE:kotor_engine> --install "${KOTOR_TEST_INSTALL}"
                             --saves-dir "${CMAKE_BINARY_DIR}/backend_parity/saves"
                             --settings-file "${CMAKE_BINARY_DIR}/backend_parity/settings.ini")
            set_tests_properties(kotor_backend_parity_vulkan PROPERTIES TIMEOUT 600)
            # The mouth on screen changes with the lip shape. Fails when the backend cannot start.
            foreach(_talk_backend gl vulkan)
                add_test(NAME kotor_talk_pixels_${_talk_backend}
                         COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_talk_pixels.py
                                 --exe $<TARGET_FILE:kotor_engine> --backend ${_talk_backend}
                                 --install "${KOTOR_TEST_INSTALL}"
                                 --out "${CMAKE_BINARY_DIR}/talk_pixels"
                                 --saves-dir "${CMAKE_BINARY_DIR}/talk_pixels/${_talk_backend}/saves"
                                 --settings-file "${CMAKE_BINARY_DIR}/talk_pixels/${_talk_backend}/settings.ini")
                set_tests_properties(kotor_talk_pixels_${_talk_backend} PROPERTIES TIMEOUT 300)
            endforeach()
            add_test(NAME kotor_all_modules
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/sweep_modules.py
                             --exe $<TARGET_FILE:kotor_engine>
                             --install "${KOTOR_TEST_INSTALL}"
                             --out "${CMAKE_BINARY_DIR}/all-modules.json")
            set_tests_properties(kotor_all_modules PROPERTIES TIMEOUT 900)
            set(KOTOR_RETAIL_TRACE_DIR "${CMAKE_SOURCE_DIR}/../artifacts/lanes/s4-trace")
            if(EXISTS "${KOTOR_RETAIL_TRACE_DIR}")
                add_test(NAME kotor_retail_trace
                         COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_retail_traces.py
                                 --trace-dir "${KOTOR_RETAIL_TRACE_DIR}"
                                 --actions "${CMAKE_SOURCE_DIR}/src/script/ActionTable.inc"
                                 --out "${CMAKE_BINARY_DIR}/retail-trace-coverage.json")
                set_tests_properties(kotor_retail_trace PROPERTIES TIMEOUT 300)
                add_test(NAME kotor_retail_differential
                         COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/run_retail_differential.py
                                 --engine $<TARGET_FILE:kotor_engine>
                                 --install "${KOTOR_TEST_INSTALL}"
                                 --corpus "${CMAKE_SOURCE_DIR}/../artifacts/lanes/04-ncs/extracted/unique"
                                 --manifest "${CMAKE_SOURCE_DIR}/tests/fixtures/retail_traces.json"
                                 --repo-root "${CMAKE_SOURCE_DIR}/.."
                                 --module-usage "${CMAKE_SOURCE_DIR}/../artifacts/lanes/s1-04-scripts/module_action_usage.csv"
                                 --action-table "${CMAKE_SOURCE_DIR}/src/script/ActionTable.inc"
                                 --out-dir "${CMAKE_BINARY_DIR}/retail-differential"
                                 --saves-dir "${CMAKE_BINARY_DIR}/retail-differential/saves"
                                 --settings-file "${CMAKE_BINARY_DIR}/retail-differential/settings.ini")
                set_tests_properties(kotor_retail_differential PROPERTIES TIMEOUT 2400)
            endif()
            add_test(NAME kotor_p6_content
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_p6_content.py
                             --exe $<TARGET_FILE:kotor_engine>
                             --install "${KOTOR_TEST_INSTALL}"
                             --baseline "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/p6_content_baseline.json"
                             --out "${CMAKE_BINARY_DIR}/p6-content.json"
                             --saves-dir "${CMAKE_BINARY_DIR}/p6_content/saves"
                             --settings-file "${CMAKE_BINARY_DIR}/p6_content/settings.ini")
            set_tests_properties(kotor_p6_content PROPERTIES TIMEOUT 900)
        endif()
    endif()
endif()
