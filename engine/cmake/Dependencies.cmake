# Pinned third-party build inputs.
#
# Every dependency is fetched as an immutable commit archive and verified by
# SHA-256 at download time. Tags alone are not trusted because tags are mutable.
# Versions and checksums are recorded in docs/versions.md.

include(FetchContent)

set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# ---------------------------------------------------------------------------
# SDL3 3.4.18 (commit 829a65d769d935c4852f8159e964312c0957260a)
# ---------------------------------------------------------------------------
if(NOT TARGET SDL3::SDL3)
    set(SDL_SHARED OFF CACHE BOOL "Build SDL3 shared library" FORCE)
    set(SDL_STATIC ON CACHE BOOL "Build SDL3 static library" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "Build SDL3_test" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "Build SDL3 tests" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "Build SDL3 examples" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "Install SDL3" FORCE)
    set(SDL_INSTALL_DOCS OFF CACHE BOOL "Install SDL3 docs" FORCE)

    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/archive/829a65d769d935c4852f8159e964312c0957260a.tar.gz
        URL_HASH SHA256=48e15cf1a6928453d77afa955d1fdfbbca9169e2bc3d76d111acb9b8bb076d66
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(SDL3)
endif()

# ---------------------------------------------------------------------------
# Vulkan-Headers 1.4.363.0 (commit 6802bb4733b63ed5efd3adb308a6c885ef180ea1)
# ---------------------------------------------------------------------------
if(NOT TARGET Vulkan::Headers)
    set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "Vulkan-Headers tests" FORCE)
    set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "Vulkan-Headers install" FORCE)

    FetchContent_Declare(VulkanHeaders
        URL https://github.com/KhronosGroup/Vulkan-Headers/archive/6802bb4733b63ed5efd3adb308a6c885ef180ea1.tar.gz
        URL_HASH SHA256=9044fbb356160e43201dc2094acb44c8a906722a63a35f48fc0f051579db9bb6
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(VulkanHeaders)
endif()

# ---------------------------------------------------------------------------
# volk 1.4.363.0 (commit 54fc0d7a72887cbfc5b030d26897b2b40ac53fb8)
# ---------------------------------------------------------------------------
if(NOT TARGET volk::volk)
    set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "Let volk fetch Vulkan-Headers" FORCE)
    set(VOLK_HEADERS_ONLY OFF CACHE BOOL "volk headers only" FORCE)
    set(VOLK_INSTALL OFF CACHE BOOL "volk install" FORCE)

    FetchContent_Declare(volk
        URL https://github.com/zeux/volk/archive/54fc0d7a72887cbfc5b030d26897b2b40ac53fb8.tar.gz
        URL_HASH SHA256=4944450dc3943871a28ce993eaf995309eb29e7000e091309968dba4cd321648
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(volk)
endif()

# VOLK_PULL_IN_VULKAN=OFF means volk does not fetch its own copy of the
# headers; propagate the pinned Vulkan::Headers target to volk and to
# consumers of volk.h.
if(TARGET volk AND TARGET Vulkan::Headers)
    target_link_libraries(volk PUBLIC Vulkan::Headers)
endif()
if(TARGET volk_headers AND TARGET Vulkan::Headers)
    target_link_libraries(volk_headers INTERFACE Vulkan::Headers)
endif()

# ---------------------------------------------------------------------------
# Dear ImGui 1.93.0 (commit c1aaac65e896a45f81f47a4d68f9959e0f86c859)
# ImGui has no upstream CMakeLists; the engine target compiles it explicitly.
# ---------------------------------------------------------------------------
if(NOT DEFINED imgui_SOURCE_DIR)
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/c1aaac65e896a45f81f47a4d68f9959e0f86c859.tar.gz
        URL_HASH SHA256=b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(imgui)
endif()

if(NOT DEFINED imgui_SOURCE_DIR)
    message(FATAL_ERROR "ImGui source directory was not populated")
endif()
