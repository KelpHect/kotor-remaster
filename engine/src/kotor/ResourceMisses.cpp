#include "kotor/ResourceMisses.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>

namespace ke::kotor {

namespace {

std::mutex gLock;
bool gOn = false;
thread_local bool gTextureProbe = false;
std::map<std::pair<std::string, std::string>, std::uint64_t> gMisses;

std::string lowerCopy(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string kindOf(std::uint32_t type)
{
    switch (type) {
    case 3: return "tga";
    case 4: return "wav";
    case 8: return "mp3";
    case 2002: return "mdl";
    case 2009: return "nss";
    case 2010: return "ncs";
    case 2012: return "are";
    case 2014: return "ifo";
    case 2015: return "bic";
    case 2016: return "wok";
    case 2017: return "2da";
    case 2018: return "tlk";
    case 2022: return "txi";
    case 2023: return "git";
    case 2025: return "uti";
    case 2027: return "utc";
    case 2029: return "dlg";
    case 2032: return "utt";
    case 2035: return "uts";
    case 2038: return "fac";
    case 2040: return "ute";
    case 2042: return "utd";
    case 2044: return "utp";
    case 2047: return "gui";
    case 2051: return "utm";
    case 2052: return "dwk";
    case 2053: return "pwk";
    case 2056: return "jrl";
    case 2058: return "utw";
    case 2060: return "ssf";
    case 2063: return "bik";
    case 3000: return "lyt";
    case 3001: return "vis";
    case 3003: return "pth";
    case 3004: return "lip";
    case 3007: return "tpc";
    case 3008: return "mdx";
    default: return "type" + std::to_string(type);
    }
}

} // namespace

void setResourceMissRecording(bool on)
{
    std::scoped_lock lock(gLock);
    gOn = on;
}

bool resourceMissRecording()
{
    std::scoped_lock lock(gLock);
    return gOn;
}

void setTextureProbe(bool on) { gTextureProbe = on; }

void recordResourceMiss(std::uint32_t type, const std::string& resref)
{
    if (gTextureProbe) {
        return;
    }
    std::scoped_lock lock(gLock);
    if (gOn) {
        ++gMisses[{kindOf(type), lowerCopy(resref)}];
    }
}

void recordTextureMiss(const std::string& resref)
{
    std::scoped_lock lock(gLock);
    if (gOn) {
        ++gMisses[{"tex", lowerCopy(resref)}];
    }
}

std::vector<ResourceMiss> resourceMisses()
{
    std::vector<ResourceMiss> out;
    {
        std::scoped_lock lock(gLock);
        for (const auto& [key, count] : gMisses) {
            out.push_back({key.first, key.second, count});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const ResourceMiss& a, const ResourceMiss& b) {
        return a.count != b.count ? a.count > b.count : (a.kind != b.kind ? a.kind < b.kind : a.resref < b.resref);
    });
    return out;
}

} // namespace ke::kotor
