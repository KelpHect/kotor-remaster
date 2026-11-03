#pragma once

// CPython-compatible Mersenne Twister, used only by the deterministic action
// runtime. The corpus oracle seeds World with 0x4B4F544F52; matching the
// generator keeps script traces reproducible against the Python reference.

#include <array>
#include <cstdint>
#include <vector>

namespace ke::script {

class PythonRandom {
public:
    explicit PythonRandom(std::uint64_t seed = 0x4B4F544F52ULL);

    std::uint32_t getrandbits(int bits);
    std::uint32_t randrange(std::uint32_t stop);
    std::int32_t randint(std::int32_t low, std::int32_t high);

private:
    void initGenrand(std::uint32_t seed);
    void initByArray(const std::vector<std::uint32_t>& key);
    std::uint32_t genrand();

    std::array<std::uint32_t, 624> mt_{};
    std::size_t index_ = 624;
};

} // namespace ke::script
