#pragma once

#include "kotor/Formats.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ke {

// Faction reputation from the shipped `repute.2da` matrix plus the save FAC
// (`REPUTE`) payload. Matrix cell [a][b] is faction a's standing toward b:
// 0 hostile, 50 neutral, 100 friendly.
class FactionSystem {
public:
    bool load(kotor::Install& install, std::string& error);
    bool loaded() const { return !names_.empty(); }

    std::size_t count() const { return names_.size(); }
    const std::string& name(int faction) const;
    int parent(int faction) const;
    std::size_t relations() const { return relations_; }

    int reputation(int factionA, int factionB) const;
    bool setReputation(int factionA, int factionB, int value);
    bool adjustReputation(int factionA, int factionB, int delta);
    static bool hostileStanding(int reputation) { return reputation < 50; }
    bool hostile(int factionA, int factionB) const {
        return hostileStanding(reputation(factionA, factionB));
    }

    // Retail saves store every pair whose value is not 100; missing pairs are
    // friendly (100).
    bool loadFac(const std::vector<std::uint8_t>& bytes, std::string& error);
    std::vector<std::uint8_t> saveFac() const;

private:
    void resize(std::size_t count);

    std::vector<std::string> names_;
    std::vector<int> parents_;
    std::vector<int> matrix_;
    std::size_t relations_ = 0;
};

} // namespace ke
