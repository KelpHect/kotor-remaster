#pragma once

// Deterministic models of the documented KotOR 1 bug-parity glitches.
//
// Classic mode reproduces the retail behavior; fixed mode refuses it. The
// models are intentionally small: each one captures the observable state
// transition that the save/load or transition code would perform, so the
// classic-vs-fixed fixture can assert both the mutation and the opt-out.

#include "kotor/Formats.h"

#include <map>
#include <string>
#include <vector>

namespace ke {

struct BugParityLevel {
    int level = 1;
    int xp = 0;
};

struct BugParityItems {
    std::vector<std::string> inventory;
    std::vector<std::string> equipped;
};

struct BugParityHotshotInput {
    bool destinationSnapshot = false;
    int destinationLevel = 1;
    int destinationXp = 0;
};

struct BugParityHotshotState {
    int level = 1;
    int xp = 0;
    std::vector<std::string> inventory;
    std::map<std::string, int> quests;
};

struct BugParityRoutineResult {
    bool menu = false;
    bool hotshot = false;
    bool spawnWarp = false;
    bool fakeLevel = false;
    bool dupe = false;
    bool completed = false;
};

namespace BugParity {

// Spawn/GP warp: party members keep their pre-transition coordinates in
// classic mode; fixed mode repositions them at the destination entry.
inline kotor::Vec3 spawnWarp(bool classic, kotor::Vec3 partyPosition,
                             kotor::Vec3 destinationEntry)
{
    return classic ? partyPosition : destinationEntry;
}

// Hotshot: warp to a module using a quicksave/hardsave pair. The destination
// snapshot restores the main character's level/XP, while inventory and quest
// state persist. Fixed mode refuses the warp.
inline bool hotshot(bool classic, const BugParityHotshotInput& input,
                    BugParityHotshotState& state)
{
    if (!classic || !input.destinationSnapshot) {
        return false;
    }
    state.level = input.destinationLevel;
    state.xp = input.destinationXp;
    return true;
}

// Fake Level Up: the level advances while XP stays unchanged. Fixed mode
// refuses the level-up.
inline bool fakeLevelUp(bool classic, BugParityLevel& state)
{
    if (!classic || state.level >= 20) {
        return false;
    }
    ++state.level;
    return true;
}

// Manaan airlock duplication: equipped companion items are copied into the
// inventory when the party cycles through the airlock. Fixed mode does not
// copy them.
inline bool airlockDupe(bool classic, BugParityItems& state)
{
    if (!classic || state.equipped.empty()) {
        return false;
    }
    state.inventory.insert(state.inventory.end(), state.equipped.begin(),
                           state.equipped.end());
    return true;
}

// "The Routine" is documented as a string of other glitches. Reproduce the
// chain (menu glitch -> hotshot -> spawn warp -> fake level -> duping) only in
// classic mode.
inline BugParityRoutineResult routine(bool classic,
                                      const BugParityHotshotInput& hotshotInput,
                                      BugParityHotshotState& hotshotState,
                                      BugParityLevel& levelState,
                                      BugParityItems& itemState,
                                      kotor::Vec3 partyPosition,
                                      kotor::Vec3 destinationEntry)
{
    BugParityRoutineResult result;
    result.menu = classic;
    result.hotshot = hotshot(classic, hotshotInput, hotshotState);
    result.spawnWarp = spawnWarp(classic, partyPosition, destinationEntry).x
        == partyPosition.x;
    result.fakeLevel = fakeLevelUp(classic, levelState);
    result.dupe = airlockDupe(classic, itemState);
    result.completed = result.menu && result.hotshot && result.spawnWarp
        && result.fakeLevel && result.dupe;
    return result;
}

} // namespace BugParity
} // namespace ke
