#include "game/EncounterPlanner.h"

#include <algorithm>
#include <cmath>

namespace ke {

namespace {

// The 0.001 margin of 00592280 (_DAT_0073d708) and the 10% tolerance of 00590f80 (_DAT_0073e798).
constexpr float kSplitMargin = 0.001f;
constexpr float kTolerance = 0.1f;

// 1.5^n, n = 0..50, filled when a CSWSEncounter is constructed (00193df0, stored as floats).
float tablePoints(int n)
{
    return static_cast<float>(std::pow(1.5, static_cast<double>(n)));
}

// 005919e0 at full precision: the table entries are floats, every other branch is the C library pow.
double pointsExact(float cr, const std::array<float, 4>& fractionalMin)
{
    if (cr <= 0.0f || cr >= 1.0f) {
        if (cr >= 0.0f && cr <= 50.0f) {
            return tablePoints(static_cast<int>(cr));
        }
        return std::pow(1.5, static_cast<double>(cr));
    }
    if (cr >= fractionalMin[0]) {
        return 1.0;
    }
    if (cr >= fractionalMin[1]) {
        return std::pow(1.5, -1.0);
    }
    if (cr >= fractionalMin[2]) {
        return std::pow(1.5, -2.0);
    }
    if (cr >= fractionalMin[3]) {
        return std::pow(1.5, -3.0);
    }
    return std::pow(1.5, -4.0);
}

} // namespace

float encounterPoints(float cr, const std::array<float, 4>& fractionalMin)
{
    return static_cast<float>(pointsExact(cr, fractionalMin));
}

int encounterLevel(float pool, int difficulty)
{
    const double level = std::log(static_cast<double>(pool) * 0.25) / std::log(1.5) + 0.5;
    return static_cast<int>(level) + difficulty; // ftol truncates toward zero
}

float encounterLevelPoints(int level, const std::array<float, 4>& fractionalMin)
{
    return encounterPoints(static_cast<float>(level - 1), fractionalMin);
}

void EncounterPlanner::prepare()
{
    for (EncounterListEntry& entry : list) {
        entry.points = encounterPoints(entry.cr, fractionalMin);
        entry.spawned = false;
        entry.visited = false;
    }
    std::stable_sort(list.begin(), list.end(),
                     [](const EncounterListEntry& a, const EncounterListEntry& b) { return a.cr > b.cr; });
    if (!list.empty()) {
        minPoints = list.back().points;
    }
}

void EncounterPlanner::add(std::vector<EncounterPick>& picks, const EncounterListEntry& entry) const
{
    picks.push_back({entry.resref, entry.cr, entry.points});
}

int EncounterPlanner::pick(int count)
{
    return count > 0 && rand ? rand() % count : 0;
}

// 00590f80. The list is walked strongest first; a lone creature that fits is taken again and again
// while it still fits, a run of equal-CR creatures is drawn from at random.
void EncounterPlanner::initial(std::vector<EncounterPick>& picks, int cap)
{
    const int count = static_cast<int>(list.size());
    float budget = pool - spent;
    const float tolerance = kTolerance * budget;
    for (int i = 0; i < count; ++i) {
        if (cap <= static_cast<int>(picks.size()) || budget <= 0.0f) {
            return;
        }
        EncounterListEntry& entry = list[static_cast<std::size_t>(i)];
        const float cr = entry.cr;
        const float points = entry.points;
        bool spawnedLocal = entry.spawned; // the retail locals are copies: a take here does not mark the entry
        int next = i;
        if (entry.singleSpawn && !spawnedLocal) {
            add(picks, entry); // a single-spawn creature comes in first, whatever the budget
            spawnedLocal = true;
        } else if (cr != 0.0f && points <= budget + tolerance) {
            if (i + 1 < count && list[static_cast<std::size_t>(i + 1)].cr == cr) {
                int groupSize = 1;
                int singles = 1;
                int visitedCount = 0;
                int j = i + 1;
                for (; j < count; ++j) {
                    EncounterListEntry& member = list[static_cast<std::size_t>(j)];
                    if (member.cr != cr) {
                        --j;
                        break;
                    }
                    member.visited = false;
                    if (member.singleSpawn) {
                        ++singles;
                    }
                    ++groupSize;
                }
                next = j;
                // 00590f80 compares the float 16 bytes before the creature array with the tolerance
                // band: a heap header, not a field (Q-0264). Taken as true.
                for (int guard = 0; static_cast<int>(picks.size()) < cap && guard < 100000; ++guard) {
                    EncounterListEntry& member = list[static_cast<std::size_t>(pick(groupSize) + i)];
                    if (!member.singleSpawn || !member.spawned) {
                        add(picks, member);
                        member.spawned = true;
                        budget -= member.points;
                    }
                    if (!member.visited) {
                        member.visited = true;
                        ++visitedCount;
                    }
                    if ((groupSize == singles && visitedCount == groupSize) || budget + tolerance < member.points) {
                        break;
                    }
                }
            } else {
                do {
                    if (static_cast<int>(picks.size()) >= cap || (entry.singleSpawn && spawnedLocal)) {
                        break;
                    }
                    add(picks, entry);
                    budget -= points;
                    spawnedLocal = true;
                } while (points <= budget + tolerance);
            }
        }
        i = next;
    }
}

// 00592280. One pick is dropped and its points spent again on weaker list entries, each worth
// at least 0.001 less than the pick. A pick that is already the cheapest creature stays.
void EncounterPlanner::split(std::vector<EncounterPick>& picks, int index)
{
    const double exact = pointsExact(picks[static_cast<std::size_t>(index)].cr, fractionalMin);
    float remaining = static_cast<float>(exact);
    if (exact == static_cast<double>(minPoints)) {
        return;
    }
    picks.erase(picks.begin() + index);
    const float limit = remaining - kSplitMargin;
    for (EncounterListEntry& entry : list) {
        if (static_cast<int>(picks.size()) > recCreatures - alive) {
            return;
        }
        if (entry.points > remaining) {
            continue;
        }
        do {
            if (entry.points > limit || static_cast<int>(picks.size()) >= maxCreatures
                || (entry.singleSpawn && entry.spawned)) {
                break;
            }
            add(picks, entry);
            entry.spawned = true;
            remaining -= entry.points;
        } while (entry.points <= remaining);
    }
}

// 00590b10. Up to RecCreatures: the cheapest CR group not used yet, drawn from at random, then the
// next cheapest.
void EncounterPlanner::padOut(std::vector<EncounterPick>& picks)
{
    const int count = static_cast<int>(list.size());
    int need = (recCreatures - alive) - static_cast<int>(picks.size());
    float lastCr = 0.0f;
    while (need > 0) {
        int index = count - 1;
        for (; index >= 0; --index) {
            const EncounterListEntry& entry = list[static_cast<std::size_t>(index)];
            if (lastCr < entry.cr && (!entry.singleSpawn || !entry.spawned)) {
                break;
            }
        }
        if (index < 0) {
            return;
        }
        lastCr = list[static_cast<std::size_t>(index)].cr;
        int groupStart = -1;
        int groupEnd = 0;
        int singles = 0;
        for (int k = 0; k < count; ++k) {
            EncounterListEntry& entry = list[static_cast<std::size_t>(k)];
            if (entry.cr == lastCr) {
                if (groupStart < 0) {
                    groupStart = k;
                }
                if (entry.singleSpawn) {
                    ++singles;
                }
                entry.visited = false;
                groupEnd = k;
            }
        }
        const int groupSize = groupEnd - groupStart + 1;
        int visitedCount = 0;
        for (int guard = 0; guard < 100000; ++guard) {
            EncounterListEntry& entry = list[static_cast<std::size_t>(pick(groupSize) + groupStart)];
            if (!entry.singleSpawn || !entry.spawned) {
                add(picks, entry);
                entry.spawned = true;
                --need;
            }
            if (!entry.visited) {
                ++visitedCount;
                entry.visited = true;
            }
            if (groupSize == singles && visitedCount == singles) {
                break;
            }
            if (need < 1) {
                return;
            }
        }
    }
}

// 005935d0.
std::vector<EncounterPick> EncounterPlanner::build()
{
    std::vector<EncounterPick> picks;
    initial(picks, maxCreatures - alive);
    const int cap = recCreatures - alive;
    if (static_cast<int>(picks.size()) < cap && minPoints < pool) {
        for (int index = static_cast<int>(picks.size()) - 1;
             index >= 0 && static_cast<int>(picks.size()) < cap; --index) {
            split(picks, index);
        }
    }
    if (static_cast<int>(picks.size()) < cap) {
        padOut(picks);
    }
    if (pool < minPoints) {
        pool = 0.0f;
    }
    return picks;
}

} // namespace ke
