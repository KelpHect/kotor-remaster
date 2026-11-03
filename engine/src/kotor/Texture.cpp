#include "kotor/Formats.h"

#include <algorithm>
#include <cstring>

namespace ke::kotor {

namespace {

std::uint16_t rgb565(std::uint16_t c, int channel)
{
    const std::uint16_t r = static_cast<std::uint16_t>((c >> 11) & 0x1Fu);
    const std::uint16_t g = static_cast<std::uint16_t>((c >> 5) & 0x3Fu);
    const std::uint16_t b = static_cast<std::uint16_t>(c & 0x1Fu);
    if (channel == 0) {
        return static_cast<std::uint16_t>((r * 255u + 15u) / 31u);
    }
    if (channel == 1) {
        return static_cast<std::uint16_t>((g * 255u + 31u) / 63u);
    }
    return static_cast<std::uint16_t>((b * 255u + 15u) / 31u);
}

void put(Image& image, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
{
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
        return;
    }
    const std::size_t o = (static_cast<std::size_t>(y) * image.width + x) * 4u;
    image.rgba[o] = r;
    image.rgba[o + 1] = g;
    image.rgba[o + 2] = b;
    image.rgba[o + 3] = a;
}

void decodeDxt(const std::vector<std::uint8_t>& data, std::size_t offset, int w, int h, Image& image, bool dxt5)
{
    const int blocksX = (w + 3) / 4;
    const int blocksY = (h + 3) / 4;
    const std::size_t blockBytes = dxt5 ? 16u : 8u;
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            const std::size_t bo = offset + (static_cast<std::size_t>(by) * blocksX + bx) * blockBytes;
            requireRange(data.size(), bo, blockBytes, "DXT block");
            int colorOffset = 0;
            std::uint8_t alpha[16]{};
            if (dxt5) {
                const std::uint8_t a0 = data[bo];
                const std::uint8_t a1 = data[bo + 1];
                std::uint8_t table[8] = {a0, a1, 0, 0, 0, 0, 0, 255};
                if (a0 > a1) {
                    for (int i = 1; i <= 6; ++i) {
                        table[i + 1] = static_cast<std::uint8_t>(((7 - i) * a0 + i * a1) / 7);
                    }
                } else {
                    for (int i = 1; i <= 4; ++i) {
                        table[i + 1] = static_cast<std::uint8_t>(((5 - i) * a0 + i * a1) / 5);
                    }
                }
                const std::uint64_t bits = static_cast<std::uint64_t>(data[bo + 2])
                    | (static_cast<std::uint64_t>(data[bo + 3]) << 8)
                    | (static_cast<std::uint64_t>(data[bo + 4]) << 16)
                    | (static_cast<std::uint64_t>(data[bo + 5]) << 24)
                    | (static_cast<std::uint64_t>(data[bo + 6]) << 32)
                    | (static_cast<std::uint64_t>(data[bo + 7]) << 40);
                for (int i = 0; i < 16; ++i) {
                    alpha[i] = table[(bits >> (3 * i)) & 7u];
                }
                colorOffset = 8;
            }
            const std::uint16_t c0 = static_cast<std::uint16_t>(data[bo + colorOffset] | (data[bo + colorOffset + 1] << 8));
            const std::uint16_t c1 = static_cast<std::uint16_t>(data[bo + colorOffset + 2] | (data[bo + colorOffset + 3] << 8));
            std::uint8_t colors[4][3]{};
            colors[0][0] = static_cast<std::uint8_t>(rgb565(c0, 0));
            colors[0][1] = static_cast<std::uint8_t>(rgb565(c0, 1));
            colors[0][2] = static_cast<std::uint8_t>(rgb565(c0, 2));
            colors[1][0] = static_cast<std::uint8_t>(rgb565(c1, 0));
            colors[1][1] = static_cast<std::uint8_t>(rgb565(c1, 1));
            colors[1][2] = static_cast<std::uint8_t>(rgb565(c1, 2));
            if (c0 > c1 || dxt5) {
                for (int ch = 0; ch < 3; ++ch) {
                    colors[2][ch] = static_cast<std::uint8_t>((2 * colors[0][ch] + colors[1][ch]) / 3);
                    colors[3][ch] = static_cast<std::uint8_t>((colors[0][ch] + 2 * colors[1][ch]) / 3);
                }
                if (!dxt5) {
                    alpha[0] = alpha[1] = alpha[2] = alpha[3] = 255;
                    alpha[4] = alpha[5] = alpha[6] = alpha[7] = 255;
                    alpha[8] = alpha[9] = alpha[10] = alpha[11] = 255;
                    alpha[12] = alpha[13] = alpha[14] = alpha[15] = 255;
                }
            } else {
                for (int ch = 0; ch < 3; ++ch) {
                    colors[2][ch] = static_cast<std::uint8_t>((colors[0][ch] + colors[1][ch]) / 2);
                    colors[3][ch] = 0;
                }
                for (int i = 0; i < 16; ++i) {
                    alpha[i] = 255;
                }
                alpha[15] = 0;
            }
            const std::uint32_t bits = static_cast<std::uint32_t>(data[bo + colorOffset + 4])
                | (static_cast<std::uint32_t>(data[bo + colorOffset + 5]) << 8)
                | (static_cast<std::uint32_t>(data[bo + colorOffset + 6]) << 16)
                | (static_cast<std::uint32_t>(data[bo + colorOffset + 7]) << 24);
            for (int i = 0; i < 16; ++i) {
                const int px = bx * 4 + (i & 3);
                const int py = by * 4 + (i >> 2);
                if (px >= w || py >= h) {
                    continue;
                }
                const int ci = static_cast<int>((bits >> (2 * i)) & 3u);
                put(image, px, py, colors[ci][0], colors[ci][1], colors[ci][2], alpha[i]);
            }
        }
    }
}

} // namespace

Image decodeTpc(const std::vector<std::uint8_t>& data, const std::string& name)
{
    if (data.size() < 128) {
        throw FormatError(name + ": TPC shorter than 128-byte header");
    }
    const std::uint32_t dataSize = readU32(data.data(), data.size(), 0, "TPC data size");
    const int w = readU16(data.data(), data.size(), 8, "TPC width");
    const int h = readU16(data.data(), data.size(), 10, "TPC height");
    const std::uint8_t encoding = data[12];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        throw FormatError(name + ": implausible TPC dimensions");
    }
    Image image;
    image.width = w;
    image.height = h;
    image.rgba.assign(static_cast<std::size_t>(w) * h * 4u, 0u);
    const std::size_t payload = 128;
    if (encoding == 1) {
        requireRange(data.size(), payload, static_cast<std::size_t>(w) * h, "TPC gray");
        for (int i = 0; i < w * h; ++i) {
            const std::uint8_t g = data[payload + static_cast<std::size_t>(i)];
            put(image, i % w, i / w, g, g, g, 255);
        }
    } else if (encoding == 2 && dataSize == static_cast<std::uint32_t>(w * h * 3)) {
        requireRange(data.size(), payload, static_cast<std::size_t>(w) * h * 3u, "TPC rgb");
        for (int i = 0; i < w * h; ++i) {
            const std::size_t o = payload + static_cast<std::size_t>(i) * 3u;
            put(image, i % w, i / w, data[o], data[o + 1], data[o + 2], 255);
        }
    } else if (encoding == 4 && (dataSize == static_cast<std::uint32_t>(w * h * 4) || dataSize == 0)) {
        requireRange(data.size(), payload, static_cast<std::size_t>(w) * h * 4u, "TPC rgba");
        std::memcpy(image.rgba.data(), data.data() + payload, static_cast<std::size_t>(w) * h * 4u);
    } else if (encoding == 0x0C) {
        requireRange(data.size(), payload, static_cast<std::size_t>(w) * h * 4u, "TPC bgra");
        for (int i = 0; i < w * h; ++i) {
            const std::size_t o = payload + static_cast<std::size_t>(i) * 4u;
            put(image, i % w, i / w, data[o + 2], data[o + 1], data[o], data[o + 3]);
        }
    } else if (encoding == 2) {
        decodeDxt(data, payload, w, h, image, false);
    } else if (encoding == 4) {
        decodeDxt(data, payload, w, h, image, true);
    } else {
        throw FormatError(name + ": unsupported TPC encoding " + std::to_string(encoding));
    }
    return image;
}

Image decodeTga(const std::vector<std::uint8_t>& data, const std::string& name)
{
    if (data.size() < 18) {
        throw FormatError(name + ": TGA shorter than 18-byte header");
    }
    const std::uint8_t idLength = data[0];
    const std::uint8_t colorMapType = data[1];
    const std::uint8_t imageType = data[2];
    const int w = readU16(data.data(), data.size(), 12, "TGA width");
    const int h = readU16(data.data(), data.size(), 14, "TGA height");
    const std::uint8_t bpp = data[16];
    const std::uint8_t descriptor = data[17];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        throw FormatError(name + ": implausible TGA dimensions");
    }
    if (imageType != 1 && imageType != 2 && imageType != 3 && imageType != 9 && imageType != 10 && imageType != 11) {
        throw FormatError(name + ": unsupported TGA image type");
    }
    if (bpp != 8 && bpp != 24 && bpp != 32) {
        throw FormatError(name + ": unsupported TGA bpp");
    }
    const bool rle = imageType >= 9;
    const int baseType = imageType - (rle ? 8 : 0);
    std::size_t pos = 18u + idLength;
    std::vector<std::uint8_t> colorMap;
    int colorMapEntry = 0;
    if (colorMapType == 1) {
        const int mapLength = readU16(data.data(), data.size(), 5, "TGA map length");
        colorMapEntry = readU16(data.data(), data.size(), 7, "TGA map entry") / 8;
        if (colorMapEntry != 16 && colorMapEntry != 24 && colorMapEntry != 32) {
            throw FormatError(name + ": unsupported TGA color map");
        }
        const std::size_t bytes = static_cast<std::size_t>(mapLength) * (colorMapEntry / 8);
        requireRange(data.size(), pos, bytes, "TGA color map");
        colorMap.assign(data.begin() + pos, data.begin() + pos + bytes);
        pos += bytes;
    }
    Image image;
    image.width = w;
    image.height = h;
    image.rgba.assign(static_cast<std::size_t>(w) * h * 4u, 0u);
    const bool topDown = (descriptor & 0x20u) != 0;
    const bool rightLeft = (descriptor & 0x10u) != 0;
    const auto store = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
        if (rightLeft) {
            x = w - 1 - x;
        }
        if (!topDown) {
            y = h - 1 - y;
        }
        put(image, x, y, r, g, b, a);
    };
    const auto readPixel = [&](std::uint8_t& r, std::uint8_t& g, std::uint8_t& b, std::uint8_t& a) {
        if (baseType == 3 || (baseType == 1 && colorMapEntry == 8)) {
            requireRange(data.size(), pos, 1, "TGA gray");
            r = g = b = data[pos++];
            a = 255;
            return;
        }
        if (baseType == 1) {
            const std::size_t step = static_cast<std::size_t>(colorMapEntry / 8);
            requireRange(data.size(), pos, step, "TGA map index");
            const std::uint16_t index = colorMapEntry > 8
                ? static_cast<std::uint16_t>(data[pos] | (data[pos + 1] << 8)) : data[pos];
            pos += step;
            const std::size_t o = static_cast<std::size_t>(index) * step;
            if (o + step > colorMap.size()) {
                throw FormatError(name + ": TGA map index out of range");
            }
            if (colorMapEntry == 16) {
                const std::uint16_t c = static_cast<std::uint16_t>(colorMap[o] | (colorMap[o + 1] << 8));
                r = static_cast<std::uint8_t>(rgb565(c, 0));
                g = static_cast<std::uint8_t>(rgb565(c, 1));
                b = static_cast<std::uint8_t>(rgb565(c, 2));
                a = 255;
            } else {
                b = colorMap[o];
                g = colorMap[o + 1];
                r = colorMap[o + 2];
                a = colorMapEntry == 32 ? colorMap[o + 3] : 255;
            }
            return;
        }
        requireRange(data.size(), pos, static_cast<std::size_t>(bpp / 8), "TGA pixel");
        if (bpp == 24) {
            b = data[pos];
            g = data[pos + 1];
            r = data[pos + 2];
            a = 255;
        } else {
            b = data[pos];
            g = data[pos + 1];
            r = data[pos + 2];
            a = data[pos + 3];
        }
        pos += static_cast<std::size_t>(bpp / 8);
    };
    const int total = w * h;
    int written = 0;
    while (written < total) {
        int count = 1;
        bool repeat = false;
        if (rle) {
            requireRange(data.size(), pos, 1, "TGA RLE header");
            const std::uint8_t header = data[pos++];
            count = (header & 0x7Fu) + 1;
            repeat = (header & 0x80u) != 0;
        }
        if (repeat) {
            std::uint8_t r = 0, g = 0, b = 0, a = 255;
            readPixel(r, g, b, a);
            for (int i = 0; i < count && written < total; ++i) {
                store(written % w, written / w, r, g, b, a);
                ++written;
            }
        } else {
            for (int i = 0; i < count && written < total; ++i) {
                std::uint8_t r = 0, g = 0, b = 0, a = 255;
                readPixel(r, g, b, a);
                store(written % w, written / w, r, g, b, a);
                ++written;
            }
        }
    }
    return image;
}

Image decodeTexture(const std::vector<std::uint8_t>& data, std::uint32_t type, const std::string& name)
{
    if (type == resType(ResType::Tpc)) {
        return decodeTpc(data, name);
    }
    if (type == resType(ResType::Tga)) {
        return decodeTga(data, name);
    }
    if (data.size() >= 128) {
        const int w = readU16(data.data(), data.size(), 8, "texture width");
        const int h = readU16(data.data(), data.size(), 10, "texture height");
        const std::uint8_t enc = data[12];
        if ((enc == 1 || enc == 2 || enc == 4 || enc == 0x0C) && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
            try {
                return decodeTpc(data, name);
            } catch (const FormatError&) {
                // fall through to TGA
            }
        }
    }
    return decodeTga(data, name);
}

} // namespace ke::kotor
