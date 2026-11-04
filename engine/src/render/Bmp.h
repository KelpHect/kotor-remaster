#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ke {

// 24-bit BMP writer for the engine's frame captures. `writeBmp` expects the
// caller's first row to be the image bottom (GL/Vulkan readback convention);
// `writeBmpTopDown` accepts top-down RGBA such as decoded movie frames.
bool writeBmp(const std::filesystem::path& path, int width, int height, const std::vector<std::uint8_t>& rgba);
bool writeBmpTopDown(const std::filesystem::path& path, int width, int height, const std::vector<std::uint8_t>& rgba);

} // namespace ke
