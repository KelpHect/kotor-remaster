#pragma once

// Walk tooling: plays the engine through the player's own paths from an --input-script, so "playable end to
// end" is a test. Test tooling only; with no walk step in the script nothing here runs, and nothing here
// teleports or writes quest state in a continuous run (the start-state helpers below are for per-module
// probing and are separate flags).
//
// A walk step is one script line (see engine/tests/walk/STEPS.md):
//   goto <waypoint|tag|x,y> [stop=<m>] [walk] [timeout=<s>]
//   use <tag> | talk <tag> [timeout=<s>]
//   dialogue <policy> [timeout=<s>]        run the conversation that is or starts next to its end
//   autodialogue <policy>|off              answer every conversation that starts, whatever step is running
//   fight [range=<m>] [timeout=<s>]
//   module <name> [timeout=<s>]            wait until the game is in this module
//   expect <what> ...                      assertions on the state
//   idle <seconds> | partyselect [accept] | note <text>
// Policies: first | index <i> <i> ... | hint <dlg>:<entry>=<reply>,... | hintfile=<path>, each with an optional
// `noskip` (let lines finish instead of clicking through them).
//
// Every step prints one line, `WALK step <n> OK|FAIL <line> ...`; the run ends with `WALK RESULT PASS|FAIL`.

#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ke {

class GameSession;

class WalkDriver {
public:
    WalkDriver(bool godmode, double fixedDt);

    // Once per frame, before the session updates: background dialogue, godmode, script-failure and
    // frame-time bookkeeping.
    void tick(GameSession& game);
    // InputScript::WalkHook: 0 pending, 1 done, -1 failed.
    int step(GameSession& game, const std::string& line, bool first);
    // The end-of-run report. Returns the process exit code (0 when every step passed and the script finished).
    int finish(GameSession* game, bool scriptFinished, const std::string& exitReason);

    int failures() const { return failed_; }

private:
    struct Policy {
        enum class Kind { First, Index, Hint } kind = Kind::First;
        std::vector<int> indices;
        std::size_t cursor = 0;
        std::map<std::string, int> hints; // "<dlg>:<entry>" or "<dlg>:*" -> option index
        bool noSkip = false;
        std::string text;
    };
    struct DialogueSeen {
        std::string resref;
        std::uint32_t entry = 0;
        int options = 0;
        double since = 0.0;
        bool stalledLogged = false;
    };

    int begin(GameSession& game, const std::string& line);
    int poll(GameSession& game);
    int done(GameSession& game, bool ok, const std::string& detail);

    int beginGoto(GameSession& game);
    int pollGoto(GameSession& game);
    int beginUse(GameSession& game, bool talk);
    int pollUse(GameSession& game);
    int beginDialogue(GameSession& game);
    int pollDialogue(GameSession& game);
    int pollFight(GameSession& game);
    int pollModule(GameSession& game);
    int doExpect(GameSession& game);
    int doList(GameSession& game);
    int pollPartySelect(GameSession& game);

    std::string blockReason(GameSession& game) const;
    bool parsePolicy(const std::vector<std::string>& words, std::size_t from, Policy& out, std::string& error) const;
    void serviceDialogue(GameSession& game, Policy& policy);
    void scanScripts(GameSession& game);
    void godmode(GameSession& game);
    std::string where(GameSession& game) const;
    double option(const std::string& key, double fallback) const;
    bool flag(const std::string& key) const;
    std::vector<std::string> positional() const;

    bool godmode_ = false;
    double fixedDt_ = 0.0;
    double sim_ = 0.0;
    std::chrono::steady_clock::time_point lastTick_{};
    bool haveLastTick_ = false;
    std::vector<float> frameMs_;
    std::string lastModule_;
    std::vector<std::pair<std::string, double>> loads_;

    // The step in progress.
    int stepNo_ = 0;
    int passed_ = 0;
    int failed_ = 0;
    std::string line_;
    std::vector<std::string> words_;
    std::map<std::string, std::string> opts_;
    std::string verb_;
    double stepStart_ = 0.0;
    double timeout_ = 60.0;
    int phase_ = 0;
    int retries_ = 0;
    std::string lastBlock_;
    // goto
    float targetX_ = 0.0f;
    float targetY_ = 0.0f;
    float targetZ_ = 0.0f;
    float stop_ = 0.5f;
    std::string targetText_;
    int doorClicks_ = 0;
    std::string gotoModule_; // goto ... through: the module the walk started in
    float lastProgressX_ = 0.0f;
    float lastProgressY_ = 0.0f;
    double lastProgressAt_ = 0.0;
    // use / talk
    std::size_t useIndex_ = 0;
    std::string useTag_;
    bool talk_ = false;
    double settledSince_ = -1.0;
    // dialogue
    Policy stepPolicy_;
    bool stepPolicyOn_ = false;
    bool sawDialogue_ = false;
    int dialogueNodes_ = 0;
    // fight
    double calm_ = 0.0;
    double lastAttack_ = 0.0;
    std::set<std::string> fightSeen_;
    int fightKilled_ = 0;
    // idle / partyselect
    double idleUntil_ = 0.0;

    Policy background_;
    bool backgroundOn_ = false;
    DialogueSeen seen_;
    bool dialogueStalled_ = false;
    std::set<std::string> visited_;
    int conversationNodes_ = 0;

    // script failures
    std::size_t scriptSeen_ = 0;
    std::string scriptLastLine_;
    std::map<std::string, int> scriptFailKinds_;
    std::map<std::string, std::string> scriptFailExample_;
    int scriptFailTotal_ = 0;
    std::string failedLine_;
};

// Start-state helpers for per-module probing (separate flags; never part of the continuous run).
// New game: the quick-built default character, then the same entry the front end's New Game makes.
bool walkStartNewGame(GameSession& game, int classSlot = 2);
// --start-wp: the leader and party stand on this waypoint.
bool walkStartAtWaypoint(GameSession& game, const std::string& tag);
// --seed-file: `global NAME = N`, `journal TAG = N`, `party NAME[,NAME]`, `gold N`, `xp N`, one per line.
bool walkApplySeedFile(GameSession& game, const std::string& path);

} // namespace ke
