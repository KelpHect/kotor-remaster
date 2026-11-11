#include "kotor/Formats.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

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
                // The three-colour DXT1 mode: index 3 is black. TPC encoding 2 is the RGB format (no
                // alpha channel), so no texel is transparent. (This block used to clear texel 15's
                // alpha whatever its index was, which punched a hole in the corner of every such block.)
                for (int ch = 0; ch < 3; ++ch) {
                    colors[2][ch] = static_cast<std::uint8_t>((colors[0][ch] + colors[1][ch]) / 2);
                    colors[3][ch] = 0;
                }
                if (!dxt5) {
                    for (int i = 0; i < 16; ++i) {
                        alpha[i] = 255;
                    }
                }
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
    } else if (encoding == 2
               && (dataSize == static_cast<std::uint32_t>(w * h * 3) || dataSize == 0)) {
        // dataSize 0 marks an uncompressed TPC (the portraits.2da art is 64x64 RGB);
        // a compressed one carries its payload size and is DXT1.
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

namespace {

// Bytes of one mip level in a TPC pixel region; 0 for an encoding this reader does not know.
std::size_t tpcLevelBytes(int w, int h, std::uint8_t encoding, bool compressed)
{
    const std::size_t blocksW = static_cast<std::size_t>(std::max(1, (w + 3) / 4));
    const std::size_t blocksH = static_cast<std::size_t>(std::max(1, (h + 3) / 4));
    const std::size_t pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    switch (encoding) {
    case 1:
        return pixels;
    case 0x0C:
        return pixels * 4u;
    case 2:
        return compressed ? blocksW * blocksH * 8u : pixels * 3u;
    case 3:
    case 4:
        return compressed ? blocksW * blocksH * 16u : pixels * 4u;
    default:
        return 0;
    }
}

std::size_t tpcChainBytes(int w, int h, std::uint8_t encoding, int mips, bool compressed)
{
    std::size_t total = 0;
    for (int level = 0; level < mips; ++level) {
        total += tpcLevelBytes(w, h, encoding, compressed);
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }
    return total;
}

bool looksLikeTxi(const std::uint8_t* bytes, std::size_t size)
{
    if (size == 0) {
        return false;
    }
    bool sawWord = false;
    for (std::size_t i = 0; i < size; ++i) {
        const std::uint8_t c = bytes[i];
        const bool text = (c >= 0x20 && c < 0x7F) || c == '\r' || c == '\n' || c == '\t';
        if (!text) {
            return false;
        }
        sawWord = sawWord || std::isalnum(c) != 0;
    }
    return sawWord;
}

} // namespace

std::string tpcEmbeddedTxi(const std::vector<std::uint8_t>& data)
{
    if (data.size() <= 128) {
        return {};
    }
    const std::uint32_t dataSize = readU32(data.data(), data.size(), 0, "TPC data size");
    const int w = readU16(data.data(), data.size(), 8, "TPC width");
    const int h = readU16(data.data(), data.size(), 10, "TPC height");
    const std::uint8_t encoding = data[12];
    const int mips = std::max<int>(1, data[13]);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || tpcLevelBytes(w, h, encoding, true) == 0) {
        return {};
    }
    // The pixel region is the stored mip-0 size, the whole compressed chain or the whole raw
    // chain (docs/recon/18); the candidate whose remainder is empty or text is the right one.
    std::vector<std::size_t> candidates;
    if (h == w * 6) {
        // A cube map: six faces, each with its own mip chain, so `h` is not one image's height.
        // Every shipped cm_* map ends in "cube 1" here; reading past it left them as flat strips.
        const std::size_t perFace = tpcChainBytes(w, w, encoding, mips, dataSize != 0);
        candidates.push_back(perFace * 6u);
    }
    if (dataSize != 0) {
        candidates.push_back(dataSize);
    }
    candidates.push_back(tpcChainBytes(w, h, encoding, mips, true));
    candidates.push_back(tpcChainBytes(w, h, encoding, mips, false));
    for (const std::size_t pixelBytes : candidates) {
        if (128u + pixelBytes > data.size()) {
            continue;
        }
        const std::size_t tail = data.size() - 128u - pixelBytes;
        if (tail == 0) {
            return {};
        }
        if (looksLikeTxi(data.data() + 128u + pixelBytes, tail)) {
            return std::string(reinterpret_cast<const char*>(data.data()) + 128u + pixelBytes, tail);
        }
    }
    return {};
}

std::uint8_t txiClampFlags(const std::string& txi)
{
    std::uint8_t flags = 0;
    std::size_t at = 0;
    while (at < txi.size()) {
        std::size_t end = txi.find('\n', at);
        if (end == std::string::npos) {
            end = txi.size();
        }
        std::size_t i = at;
        while (i < end && (txi[i] == ' ' || txi[i] == '\t' || txi[i] == '\r')) {
            ++i;
        }
        std::size_t wordEnd = i;
        while (wordEnd < end && txi[wordEnd] != ' ' && txi[wordEnd] != '\t' && txi[wordEnd] != '\r') {
            ++wordEnd;
        }
        if (wordEnd - i == 5) {
            bool match = true;
            for (std::size_t k = 0; k < 5; ++k) {
                match = match && std::tolower(static_cast<unsigned char>(txi[i + k])) == "clamp"[k];
            }
            if (match) {
                // Each matching line overwrites the field, so the last one wins (retail
                // 00422390 hands the rest of the line to sscanf("%d")).
                const std::string rest = txi.substr(wordEnd, end - wordEnd);
                flags = static_cast<std::uint8_t>(std::strtol(rest.c_str(), nullptr, 10) & 3L);
            }
        }
        at = end + 1;
    }
    return flags;
}

std::uint8_t textureClampFlags(const std::vector<std::uint8_t>& data, std::uint32_t type,
                               const std::vector<std::uint8_t>* separateTxi)
{
    if (type == resType(ResType::Tpc)) {
        const std::uint8_t embedded = txiClampFlags(tpcEmbeddedTxi(data));
        if (embedded != 0) {
            return embedded;
        }
    }
    if (separateTxi != nullptr && !separateTxi->empty()) {
        return txiClampFlags(std::string(separateTxi->begin(), separateTxi->end()));
    }
    return 0;
}

Image decodeCubeLatLong(const std::vector<std::uint8_t>& data, const std::string& name)
{
    if (data.size() < 128) {
        return {};
    }
    const std::uint32_t dataSize = readU32(data.data(), data.size(), 0, "TPC data size");
    const int w = readU16(data.data(), data.size(), 8, "TPC width");
    const int h = readU16(data.data(), data.size(), 10, "TPC height");
    const std::uint8_t encoding = data[12];
    const int mips = std::max<int>(1, data[13]);
    if (w <= 0 || h != w * 6) {
        return {};
    }
    const bool compressed = dataSize != 0;
    const std::size_t stride = tpcChainBytes(w, w, encoding, mips, compressed);
    if (stride == 0 || 128u + stride * 6u > data.size()) {
        return {};
    }
    // Six faces, each with its own mip chain, in file order (placed below).
    std::array<Image, 6> faces;
    for (int face = 0; face < 6; ++face) {
        Image& image = faces[static_cast<std::size_t>(face)];
        image.width = w;
        image.height = w;
        image.rgba.assign(static_cast<std::size_t>(w) * w * 4u, 0u);
        const std::size_t payload = 128u + stride * static_cast<std::size_t>(face);
        if (compressed && (encoding == 2 || encoding == 4)) {
            decodeDxt(data, payload, w, w, image, encoding == 4);
        } else if (encoding == 2) {
            for (int i = 0; i < w * w; ++i) {
                const std::size_t o = payload + static_cast<std::size_t>(i) * 3u;
                put(image, i % w, i / w, data[o], data[o + 1], data[o + 2], 255);
            }
        } else if (encoding == 4) {
            std::memcpy(image.rgba.data(), data.data() + payload, image.rgba.size());
        } else {
            return {};
        }
    }
    (void)name;
    // Retail 00423f90 uploads file face i to GL face 0x8515 + {1,0,2,3,4,5}[i] (0x78db50), so the
    // first two are swapped, after turning it {3,1,0,2,2,0}[i] quarter turns (0x78db38) with
    // 0041fbd0: 1 sends source (x, y) to (h-1-y, x), 2 to (w-1-x, h-1-y), 3 to (y, w-1-x).
    {
        constexpr int kTarget[6] = {1, 0, 2, 3, 4, 5};
        constexpr int kTurns[6] = {3, 1, 0, 2, 2, 0};
        std::array<Image, 6> placed;
        for (int face = 0; face < 6; ++face) {
            const Image& source = faces[static_cast<std::size_t>(face)];
            Image& target = placed[static_cast<std::size_t>(kTarget[face])];
            target.width = w;
            target.height = w;
            target.rgba.assign(source.rgba.size(), 0u);
            for (int y = 0; y < w; ++y) {
                for (int x = 0; x < w; ++x) {
                    int row = y;
                    int column = x;
                    switch (kTurns[face]) {
                    case 1: row = x; column = w - 1 - y; break;
                    case 2: row = w - 1 - y; column = w - 1 - x; break;
                    case 3: row = w - 1 - x; column = y; break;
                    default: break;
                    }
                    std::memcpy(target.rgba.data() + (static_cast<std::size_t>(row) * w + column) * 4u,
                                source.rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4u, 4u);
                }
            }
        }
        faces = std::move(placed);
    }
    // Resampled so one sampler2D serves both backends, faces now in GL order and orientation.
    Image out;
    out.width = w * 4;
    out.height = w * 2;
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4u, 0u);
    const float pi = 3.14159265358979f;
    for (int y = 0; y < out.height; ++y) {
        const float polar = (static_cast<float>(y) + 0.5f) / static_cast<float>(out.height) * pi;
        for (int x = 0; x < out.width; ++x) {
            const float azimuth = ((static_cast<float>(x) + 0.5f) / static_cast<float>(out.width) - 0.5f) * 2.0f * pi;
            const float dx = std::sin(polar) * std::cos(azimuth);
            const float dy = std::sin(polar) * std::sin(azimuth);
            const float dz = std::cos(polar);
            const float ax = std::abs(dx);
            const float ay = std::abs(dy);
            const float az = std::abs(dz);
            int face = 0;
            float sc = 0.0f;
            float tc = 0.0f;
            float ma = 1.0f;
            if (ax >= ay && ax >= az) {
                face = dx > 0.0f ? 0 : 1;
                ma = ax;
                sc = dx > 0.0f ? -dz : dz;
                tc = -dy;
            } else if (ay >= az) {
                face = dy > 0.0f ? 2 : 3;
                ma = ay;
                sc = dx;
                tc = dy > 0.0f ? dz : -dz;
            } else {
                face = dz > 0.0f ? 4 : 5;
                ma = az;
                sc = dz > 0.0f ? dx : -dx;
                tc = -dy;
            }
            const int fx = std::clamp(static_cast<int>((sc / ma * 0.5f + 0.5f) * static_cast<float>(w)), 0, w - 1);
            const int fy = std::clamp(static_cast<int>((tc / ma * 0.5f + 0.5f) * static_cast<float>(w)), 0, w - 1);
            const std::uint8_t* source = faces[static_cast<std::size_t>(face)].rgba.data()
                + (static_cast<std::size_t>(fy) * w + fx) * 4u;
            std::uint8_t* target = out.rgba.data() + (static_cast<std::size_t>(y) * out.width + x) * 4u;
            std::memcpy(target, source, 4u);
        }
    }
    return out;
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
