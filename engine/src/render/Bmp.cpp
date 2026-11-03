#include "render/Bmp.h"

#include <fstream>

namespace ke {

namespace {

void writeLe16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void writeLe32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

} // namespace

bool writeBmp(const std::filesystem::path& path, int width, int height, const std::vector<std::uint8_t>& rgba)
{
    if (width <= 0 || height <= 0 || rgba.size() < static_cast<std::size_t>(width) * height * 4u) {
        return false;
    }
    const std::uint32_t rowBytes = static_cast<std::uint32_t>(width) * 3u;
    const std::uint32_t padding = (4u - (rowBytes % 4u)) % 4u;
    const std::uint32_t imageBytes = (rowBytes + padding) * static_cast<std::uint32_t>(height);
    std::vector<std::uint8_t> bmp;
    bmp.reserve(54u + imageBytes);
    bmp.push_back('B');
    bmp.push_back('M');
    writeLe32(bmp, 54u + imageBytes);
    writeLe16(bmp, 0);
    writeLe16(bmp, 0);
    writeLe32(bmp, 54u);
    writeLe32(bmp, 40u);
    writeLe32(bmp, static_cast<std::uint32_t>(width));
    writeLe32(bmp, static_cast<std::uint32_t>(height));
    writeLe16(bmp, 1);
    writeLe16(bmp, 24);
    writeLe32(bmp, 0);
    writeLe32(bmp, imageBytes);
    writeLe32(bmp, 2835);
    writeLe32(bmp, 2835);
    writeLe32(bmp, 0);
    writeLe32(bmp, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * width + x) * 4u;
            bmp.push_back(rgba[o + 2]);
            bmp.push_back(rgba[o + 1]);
            bmp.push_back(rgba[o]);
        }
        for (std::uint32_t p = 0; p < padding; ++p) {
            bmp.push_back(0u);
        }
    }
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(bmp.data()), static_cast<std::streamsize>(bmp.size()));
    return static_cast<bool>(file);
}

} // namespace ke
