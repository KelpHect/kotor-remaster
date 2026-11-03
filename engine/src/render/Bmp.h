#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ke {

// 24-bit BMP writer for the engine's frame captures. RGBA rows are top-down.
bool writeBmp(const std::filesystem::path& path, int width, int height, const std::vector<std::uint8_t>& rgba);

} // namespace ke
