#include "script/Random.h"

#include <algorithm>

namespace ke::script {

PythonRandom::PythonRandom(std::uint64_t seed)
{
    // CPython random_seed: build 32-bit limbs of abs(seed), least significant
    // first, then init_by_array. A zero seed uses the single limb [0].
    std::vector<std::uint32_t> key;
    std::uint64_t n = seed;
    while (n != 0) {
        key.push_back(static_cast<std::uint32_t>(n & 0xFFFFFFFFULL));
        n >>= 32;
    }
    if (key.empty()) {
        key.push_back(0);
    }
    initByArray(key);
}

void PythonRandom::initGenrand(std::uint32_t seed)
{
    mt_[0] = seed;
    for (std::size_t i = 1; i < mt_.size(); ++i) {
        mt_[i] = 1812433253u * (mt_[i - 1] ^ (mt_[i - 1] >> 30)) + static_cast<std::uint32_t>(i);
    }
    index_ = mt_.size();
}

void PythonRandom::initByArray(const std::vector<std::uint32_t>& key)
{
    initGenrand(19650218u);
    std::size_t i = 1;
    std::size_t j = 0;
    std::size_t k = std::max(mt_.size(), key.size());
    for (; k != 0; --k) {
        mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525u))
            + key[j] + static_cast<std::uint32_t>(j);
        ++i;
        ++j;
        if (i >= mt_.size()) {
            mt_[0] = mt_[mt_.size() - 1];
            i = 1;
        }
        if (j >= key.size()) {
            j = 0;
        }
    }
    for (k = mt_.size() - 1; k != 0; --k) {
        mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941u))
            - static_cast<std::uint32_t>(i);
        ++i;
        if (i >= mt_.size()) {
            mt_[0] = mt_[mt_.size() - 1];
            i = 1;
        }
    }
    mt_[0] = 0x80000000u;
}

std::uint32_t PythonRandom::genrand()
{
    if (index_ >= mt_.size()) {
        for (std::size_t i = 0; i < mt_.size(); ++i) {
            const std::uint32_t y = (mt_[i] & 0x80000000u)
                | (mt_[(i + 1) % mt_.size()] & 0x7FFFFFFFu);
            mt_[i] = mt_[(i + 397) % mt_.size()] ^ (y >> 1);
            if ((y & 1u) != 0) {
                mt_[i] ^= 0x9908B0DFu;
            }
        }
        index_ = 0;
    }
    std::uint32_t y = mt_[index_++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9D2C5680u;
    y ^= (y << 15) & 0xEFC60000u;
    y ^= y >> 18;
    return y;
}

std::uint32_t PythonRandom::getrandbits(int bits)
{
    if (bits <= 0) {
        return 0;
    }
    if (bits <= 32) {
        return genrand() >> static_cast<std::uint32_t>(32 - bits);
    }
    std::uint32_t value = 0;
    int remaining = bits;
    int shift = 0;
    while (remaining > 0) {
        const int take = std::min(remaining, 32);
        const std::uint32_t word = genrand() >> static_cast<std::uint32_t>(32 - take);
        value |= word << shift;
        shift += take;
        remaining -= take;
    }
    return value;
}

std::uint32_t PythonRandom::randrange(std::uint32_t stop)
{
    if (stop == 0) {
        return 0;
    }
    int bits = 0;
    for (std::uint32_t n = stop; n != 0; n >>= 1) {
        ++bits;
    }
    std::uint32_t value = getrandbits(bits);
    while (value >= stop) {
        value = getrandbits(bits);
    }
    return value;
}

std::int32_t PythonRandom::randint(std::int32_t low, std::int32_t high)
{
    if (high < low) {
        return low;
    }
    const std::uint32_t span = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(high) - static_cast<std::int64_t>(low) + 1);
    return low + static_cast<std::int32_t>(randrange(span));
}

} // namespace ke::script
