#pragma once

// Encounter creature selection: the challenge-rating budget of CSWSEncounter, not RecCreatures.
// Pure rules, no scene access. Retail addresses (swkotor.exe 1.0.3.0):
//   005919e0 CalculatePointsFromCR     points a creature of a challenge rating is worth
//   005931d0 CalculateSpawnPool        party points -> encounter level -> budget
//   00590f80 BuildInitialSpawnList     first pass, strongest creatures that fit
//   00592280 AddCreaturesToSpawnList   split one strong pick into weaker ones
//   00590b10 PadOutCreatureCount       fill up to RecCreatures from the weakest CR group
//   005935d0 BuildSpawnList            the three passes in order
//   00592430 ReadEncounterFromGff      list sorted by CR descending, last entry's points = minimum

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ke {

struct EncounterListEntry {
    std::string resref;
    float cr = 0.0f;
    float points = 0.0f; // 005919e0 of cr
    bool singleSpawn = false;
    bool spawned = false; // entry +0x18
    bool visited = false; // entry +0x1c
};

struct EncounterPick {
    std::string resref;
    float cr = 0.0f;
    float points = 0.0f;
};

// 005919e0. `fractionalMin` is the Min column of fractionalcr.2da rows 0..3 (only those four are read).
float encounterPoints(float cr, const std::array<float, 4>& fractionalMin);

// 005931d0: ftol(ln(pool * 0.25) / ln(1.5) + 0.5) + difficulty, the encounter's level.
int encounterLevel(float pool, int difficulty);

// What a creature of `level` is worth in a pool (005919e0 on the integer level).
float encounterLevelPoints(int level, const std::array<float, 4>& fractionalMin);

struct EncounterPlanner {
    std::vector<EncounterListEntry> list; // as read from the template; sort() puts them in retail order
    std::int32_t recCreatures = 0;        // encounter +0x250
    std::int32_t maxCreatures = 0;        // encounter +0x254
    std::int32_t alive = 0;               // encounter +0x264
    float pool = 0.0f;                    // encounter +0x2d8
    float spent = 0.0f;                   // encounter +0x2dc
    float minPoints = 0.0f;               // encounter +0x2d0
    std::array<float, 4> fractionalMin{0.40f, 0.30f, 0.20f, 0.15f};
    std::function<int()> rand;            // the C library rand(): 0..32767

    // 00592430: points per entry, descending CR order (a stable exchange sort), minPoints from the last.
    void prepare();
    // 005935d0: the creatures to spawn, in spawn order.
    std::vector<EncounterPick> build();

private:
    void initial(std::vector<EncounterPick>& picks, int cap);
    void split(std::vector<EncounterPick>& picks, int index);
    void padOut(std::vector<EncounterPick>& picks);
    int pick(int count);
    void add(std::vector<EncounterPick>& picks, const EncounterListEntry& entry) const;
};

} // namespace ke
