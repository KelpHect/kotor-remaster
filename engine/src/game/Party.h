#pragma once

// Party table: the engine-side model of retail's PARTYTABLE. Nine "available
// NPC" slots indexed by the nwscript NPC_* constants, a member list (NPCs only;
// the PC is never a member and is NPC_PLAYER = -1), the leader, the controlled
// NPC (SwitchPlayerCharacter), solo mode and the shared XP pool. Behaviour and
// the evidence for it live in ScriptParty.cpp.

#include "game/Rules.h"
#include "script/World.h"

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace ke {

// nwscript.nss NPC_* constants. The index is the retail party-table slot and the
// npc.2da row.
constexpr int kNpcPlayer = -1;
constexpr int kNpcBastila = 0;
constexpr int kNpcCanderous = 1;
constexpr int kNpcCarth = 2;
constexpr int kNpcHk47 = 3;
constexpr int kNpcJolee = 4;
constexpr int kNpcJuhani = 5;
constexpr int kNpcMission = 6;
constexpr int kNpcT3m4 = 7;
constexpr int kNpcZaalbar = 8;
constexpr int kNpcCount = 9;
// Retail's member list holds at most two NPCs (the PC is the third party slot).
constexpr int kMaxPartyNpcs = 2;

// One available-NPC slot: retail keeps flags, a creature object id and a stored
// creature (AVAILNPC<n>) per NPC. The stored creature is `sheet` plus the
// template it came from.
struct PartyNpc {
    bool available = false;
    // The PT_NPC_SELECT byte. Nonzero is selectable in the party GUI. The default
    // for a freshly added NPC is not recovered from the binary (retail's loader
    // default is 0; scripts set it right after adding); engine default is 1.
    std::int32_t select = 1;
    bool selectable() const { return select != 0; }
    std::string templateResref;
    std::string tag;
    std::string name;
    CharacterSheet sheet;
    bool hasSheet = false;
    // Retail's creature field at +0x22c, saved as JoiningXP: the XP a creature
    // is credited beyond its share of the party pool when it joins.
    std::int32_t joiningXp = 0;
    // Script object of the live creature, or kObjectInvalid while it is only
    // stored. Kept across module changes.
    std::uint32_t object = script::kObjectInvalid;
    // Party death state (engine rule, not recovered from the binary).
    bool downed = false;
    // HP bookkeeping so a change made on either side (entity or sheet) wins.
    std::int32_t lastEntityHp = -1;
    std::int32_t lastSheetHp = -1;
};

struct PartyTable {
    std::array<PartyNpc, kNpcCount> npcs;
    std::vector<int> members;     // NPC ids in join order, at most kMaxPartyNpcs.
    int leader = kNpcPlayer;      // PT_IS_LEADER; kNpcPlayer when the PC leads.
    int controlled = kNpcPlayer;  // PT_CONTROLLED_NP.
    bool solo = false;            // PT_SOLOMODE.
    std::int32_t xpPool = 0;      // PT_XP_POOL.
    // SetLockHeadFollowInDialog: objects whose head must not track the speaker.
    std::set<std::uint32_t> headFollowLocked;
    // Lazy caches of static data (npc.2da PercentXP, template per NPC).
    bool tablesLoaded = false;
    std::array<std::int32_t, kNpcCount> xpPercent{80, 80, 80, 80, 80, 80, 80, 80, 80};
    std::array<std::string, kNpcCount> npcTag;
    std::array<std::string, kNpcCount> defaultTemplate;
};

class GameSession;
// The party family gate, behind --family-test party.
int runPartyFamilyTest(GameSession& game);
// The companion-control gate, behind --family-test companion.
int runCompanionFamilyTest(GameSession& game);

// A party table read back from an engine save (the party_* lines).
struct PartyLoad {
    bool present = false;
    PartyTable table;
    // Where the controlled companion stood (the PC's own position is the save's
    // x/y/z). Engine save only: a retail save keeps no creature positions here.
    bool hasControlledPosition = false;
    float controlledPosition[3] = {0.0f, 0.0f, 0.0f};
    float controlledBearing = 0.0f;
};

// One save line. Returns false when the key is not a party line.
bool parsePartyLine(const std::string& key, const std::string& value, PartyLoad& out);

// ShowPartySelectionGUI state, shared by the UI and the bot. `chosen` is the
// tentative member list; nothing changes in the party until accept.
struct PartySelectionState {
    bool active = false;
    std::string exitScript;
    int forced[2] = {kNpcPlayer, kNpcPlayer};
    std::vector<int> chosen;
};

} // namespace ke
