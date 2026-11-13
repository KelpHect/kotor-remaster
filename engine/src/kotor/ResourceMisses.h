#pragma once

// Walk tooling: a count of every resource lookup that found nothing, kept only while recording is on
// (--input-script runs with walk steps). Off, Install::find costs one branch and records nothing.

#include <cstdint>
#include <string>
#include <vector>

namespace ke::kotor {

struct ResourceMiss {
    std::string kind;   // file extension of the resource type ("tex" for a texture asked as tpc then tga)
    std::string resref; // lower case
    std::uint64_t count = 0;
};

void setResourceMissRecording(bool on);
bool resourceMissRecording();
// Every miss recorded so far, most frequent first; the counters are kept.
std::vector<ResourceMiss> resourceMisses();
// Called by Install::find and findTexture.
void recordResourceMiss(std::uint32_t type, const std::string& resref);
void recordTextureMiss(const std::string& resref);
// While a texture is probed as tpc then tga, the inner finds are not misses on their own.
void setTextureProbe(bool on);

} // namespace ke::kotor
