#include "kotor/Formats.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    } else {
        std::cout << "ok: " << what << '\n';
    }
}

void testSha()
{
    const char* abc = "abc";
    expect(ke::kotor::sha256Bytes(reinterpret_cast<const std::uint8_t*>(abc), 3)
               == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "sha256 abc");
}

void testResref()
{
    expect(ke::kotor::normalizeResref("End_Door05") == "end_door05", "resref lowercases");
    expect(ke::kotor::normalizeResref(std::string("ABC\0tail", 8)) == "abc", "resref stops at NUL");
}

void testLyt()
{
    const std::string text =
        "#MAXLAYOUT\nbeginlayout\nroomcount 1\nm01aa_01a 1.0 2.0 3.0 0.0 0.0 0.0\n"
        "doorhookcount 1\nm01aa_01a door 0 4.0 5.0 6.0 1.0 0.0 0.0 0.0\n";
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    const ke::kotor::Lyt lyt = ke::kotor::parseLyt(bytes, "test.lyt");
    expect(lyt.rooms.size() == 1, "LYT room count");
    expect(lyt.rooms[0].room == "m01aa_01a", "LYT room name");
    expect(lyt.rooms[0].origin.x == 1.0f && lyt.rooms[0].origin.z == 3.0f, "LYT room origin");
    expect(lyt.hooks.size() == 1 && lyt.hooks[0].node == "door", "LYT door hook");
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

void putF32(std::vector<std::uint8_t>& bytes, std::size_t offset, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put32(bytes, offset, bits);
}

void testWok()
{
    // The BWM count table ends at 0x60 (96). Keep the vertex/face/surface data
    // after it; overlapping the table makes the parser read zero counts.
    const std::size_t header = 96;
    const std::size_t vertices = 3 * 12;
    const std::size_t faces = 12;
    const std::size_t surface = 4;
    std::vector<std::uint8_t> bytes(header + vertices + faces + surface, 0);
    std::memcpy(bytes.data(), "BWM V1.0", 8);
    put32(bytes, 8, 0); // no per-face normals: derive from winding
    put32(bytes, 72, 3); put32(bytes, 76, static_cast<std::uint32_t>(header));
    put32(bytes, 80, 1); put32(bytes, 84, static_cast<std::uint32_t>(header + vertices));
    put32(bytes, 88, static_cast<std::uint32_t>(header + vertices + faces));
    put32(bytes, 92, 0);
    const float xyz[3][3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            putF32(bytes, header + static_cast<std::size_t>(i) * 12 + static_cast<std::size_t>(j) * 4, xyz[i][j]);
        }
    }
    const std::size_t face = header + vertices;
    bytes[face + 0] = 0; bytes[face + 1] = 0;
    bytes[face + 2] = 1; bytes[face + 3] = 0;
    bytes[face + 4] = 2; bytes[face + 5] = 0;
    put32(bytes, header + vertices + faces, 7);
    const ke::kotor::Wok wok = ke::kotor::parseWok(bytes, "test.wok");
    expect(wok.vertices.size() == 3, "WOK vertices");
    expect(wok.faces.size() == 1 && wok.faces[0].floor, "WOK upward face is floor");
    expect(wok.faces[0].surface == 7, "WOK surface id");
}

} // namespace

int main()
{
    testSha();
    testResref();
    testLyt();
    testWok();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all kotor format tests passed\n";
    return 0;
}
