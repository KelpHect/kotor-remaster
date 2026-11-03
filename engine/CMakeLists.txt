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
# KotOR data layer: archives, GFF, geometry, textures, scene construction.
# This is engine code, not a Python tool; the research tools remain oracles.
# ---------------------------------------------------------------------------
add_library(kotor_data STATIC
    src/kotor/Archive.cpp
    src/kotor/Entities.cpp
    src/kotor/Gff.cpp
    src/kotor/Geometry.cpp
    src/kotor/Install.cpp
    src/kotor/Scene.cpp
    src/kotor/Texture.cpp
    src/kotor/Tlk.cpp
    src/kotor/TwoDa.cpp)
target_include_directories(kotor_data PUBLIC src)
target_link_libraries(kotor_data PUBLIC kotor_engine_options)

# ---------------------------------------------------------------------------
# Engine executable: both GPU backends are built; selection is runtime.
# ---------------------------------------------------------------------------
add_executable(kotor_engine
    src/main.cpp
    src/app/Application.cpp
    src/app/Args.cpp
    src/core/Log.cpp
    src/game/GameSession.cpp
    src/render/BackendFactory.cpp
    src/render/Bmp.cpp
    src/render/NullBackend.cpp
    src/render/GlBackend.cpp
    src/render/GlScene.cpp
    src/render/VulkanBackend.cpp
    src/render/VulkanScene.cpp)

target_include_directories(kotor_engine PRIVATE src)
target_link_libraries(kotor_engine PRIVATE
    kotor_engine_options
    kotor_data
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
    target_link_libraries(kotor_tests PRIVATE kotor_engine_options kotor_data)
    add_test(NAME kotor_formats COMMAND kotor_tests)

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
                 COMMAND kotor_engine --module end_m01aa --scene-assert --null --headless)
        add_test(NAME kotor_door_transition
                 COMMAND kotor_engine --module tar_m02af --door-test --null --headless)
        add_test(NAME kotor_taris_transition
                 COMMAND kotor_engine --module tar_m02af --door-test --null --headless)
        add_test(NAME kotor_walk_collision
                 COMMAND kotor_engine --module end_m01aa --walk-test --null --headless)
        add_test(NAME kotor_dialogue
                 COMMAND kotor_engine --module end_m01aa --dialogue-test --null --headless)
        add_test(NAME kotor_combat
                 COMMAND kotor_engine --module end_m01aa --combat-test --null --headless)
        add_test(NAME kotor_playthrough
                 COMMAND kotor_engine --module end_m01aa --playthrough --null --headless)
        add_test(NAME kotor_landmark
                 COMMAND kotor_engine --module end_m01aa --landmark-test --null --headless)
        add_test(NAME kotor_save_load
                 COMMAND kotor_engine --module end_m01aa --walk-test --save "${CMAKE_BINARY_DIR}/engine-save.txt" --null --headless)
        set_tests_properties(kotor_scene_stats kotor_scene_assert kotor_door_transition
                             kotor_taris_transition kotor_walk_collision kotor_dialogue
                             kotor_combat kotor_playthrough kotor_landmark kotor_save_load
            PROPERTIES ENVIRONMENT "KOTOR_INSTALL=${KOTOR_TEST_INSTALL}" TIMEOUT 300)
    endif()
endif()
