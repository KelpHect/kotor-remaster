#include "app/WalkTools.h"

#include "core/Log.h"
#include "game/Chargen.h"
#include "game/GameSession.h"
#include "game/ScriptHost.h"
#include "kotor/ResourceMisses.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace ke {

namespace {

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

std::string lowerText(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::vector<std::string> splitWords(const std::string& line)
{
    std::vector<std::string> out;
    std::istringstream stream(line);
    for (std::string word; stream >> word;) {
        out.push_back(word);
    }
    return out;
}

std::string fixed(double value, int digits = 2)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(digits) << value;
    return stream.str();
}

float dist2D(kotor::Vec3 a, kotor::Vec3 b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

const char* useOutcomeName(int outcome)
{
    switch (outcome) {
    case 0: return "invalid";
    case 1: return "out-of-range";
    case 2: return "refused";
    case 3: return "locked";
    case 4: return "opened";
    case 5: return "closed";
    case 6: return "transition";
    case 7: return "looted";
    case 8: return "used";
    default: return "none";
    }
}

const char* kindName(kotor::EntityKind kind)
{
    switch (kind) {
    case kotor::EntityKind::Door: return "door";
    case kotor::EntityKind::Placeable: return "placeable";
    case kotor::EntityKind::Creature: return "creature";
    case kotor::EntityKind::Waypoint: return "waypoint";
    case kotor::EntityKind::Trigger: return "trigger";
    case kotor::EntityKind::Encounter: return "encounter";
    case kotor::EntityKind::Sound: return "sound";
    case kotor::EntityKind::Store: return "store";
    default: return "unknown";
    }
}

// The keys a step line may carry as key=value; everything else stays positional.
bool isOptionKey(const std::string& key)
{
    return key == "timeout" || key == "stop" || key == "range" || key == "kinds";
}

bool isFlagWord(const std::string& word)
{
    return word == "walk" || word == "noskip" || word == "accept";
}

bool compare(double left, const std::string& op, double right)
{
    if (op == "=" || op == "==") return left == right;
    if (op == "!=") return left != right;
    if (op == "<") return left < right;
    if (op == ">") return left > right;
    if (op == "<=") return left <= right;
    if (op == ">=") return left >= right;
    return false;
}

} // namespace

WalkDriver::WalkDriver(bool godmode, double fixedDt)
    : godmode_(godmode)
    , fixedDt_(fixedDt)
{
}

std::string WalkDriver::where(GameSession& game) const
{
    const kotor::Vec3 at = game.controlledPosition();
    return concat("module=", game.module(), " pos=", fixed(at.x), ",", fixed(at.y), ",", fixed(at.z));
}

double WalkDriver::option(const std::string& key, double fallback) const
{
    const auto it = opts_.find(key);
    return it == opts_.end() ? fallback : std::atof(it->second.c_str());
}

bool WalkDriver::flag(const std::string& key) const
{
    return std::find(words_.begin() + 1, words_.end(), key) != words_.end();
}

std::vector<std::string> WalkDriver::positional() const
{
    std::vector<std::string> out;
    for (std::size_t i = 1; i < words_.size(); ++i) {
        const std::size_t eq = words_[i].find('=');
        if ((eq != std::string::npos && isOptionKey(words_[i].substr(0, eq))) || isFlagWord(words_[i])) {
            continue;
        }
        out.push_back(words_[i]);
    }
    return out;
}

std::string WalkDriver::blockReason(GameSession& game) const
{
    if (game.dialogue().active) return "dialogue";
    if (game.moviePlaying()) return "movie";
    if (game.miniGameActive()) return "minigame";
    if (game.pazaakActive()) return "pazaak";
    if (game.partySelection().active) return "party-selection";
    if (game.activePanel() != UiPanel::None) return "panel";
    if (game.galaxyMapOpen()) return "galaxy-map";
    if (game.controlLocked()) return "control-locked";
    if (game.controlledDead()) return "dead";
    if (game.scriptPauseActive()) return "script-pause";
    if (game.creditsActive()) return "credits";
    return {};
}

// ---------------------------------------------------------------------------------------------- per frame

void WalkDriver::tick(GameSession& game)
{
    const auto now = std::chrono::steady_clock::now();
    double wallMs = 0.0;
    if (haveLastTick_) {
        wallMs = std::chrono::duration<double, std::milli>(now - lastTick_).count();
        frameMs_.push_back(static_cast<float>(wallMs));
    }
    lastTick_ = now;
    haveLastTick_ = true;
    sim_ += fixedDt_ > 0.0 ? fixedDt_ : std::min(wallMs / 1000.0, 0.25);

    if (game.module() != lastModule_) {
        if (!lastModule_.empty()) {
            // The frame that ran the load carries its wall time.
            loads_.emplace_back(game.module(), wallMs);
            logInfo(concat("WALK load step=", stepNo_, " from=", lastModule_, " module=", game.module(), " ms=", fixed(wallMs, 0)));
        }
        lastModule_ = game.module();
    }
    scanScripts(game);
    if (godmode_) {
        godmode(game);
    }
    if (stepPolicyOn_) {
        serviceDialogue(game, stepPolicy_);
    } else if (backgroundOn_) {
        serviceDialogue(game, background_);
    }
}

void WalkDriver::godmode(GameSession& game)
{
    // Only keeps the party from dying (route discovery): a member at 60 percent of its hit points or less is
    // healed to full through the same heal path a medpac uses, and each member carries the SetMinOneHP flag so a
    // single hit larger than its whole hit point pool (a critical at level 1 does 14 against 12) leaves it at 1
    // instead of dead. Damage and kills are the game's own.
    ScriptApi api{game};
    for (const std::uint32_t object : game.partyObjects()) {
        std::int32_t hp = 0;
        std::int32_t maxHp = 0;
        if (object == game.controlledObject()) {
            hp = game.controlledHp();
            maxHp = game.controlledMaxHp();
        } else {
            const std::size_t index = game.bodyOfObject(object);
            if (index == kNone || index >= game.scene().entities.size()) {
                continue;
            }
            hp = game.scene().entities[index].hitPoints;
            maxHp = game.scene().entities[index].maxHitPoints;
        }
        if (object == api.pc()) {
            api.actor(object).combat.min1Hp = true;
        } else if (const std::size_t body = game.bodyOfObject(object); body != kNone && body < game.scene().entities.size()) {
            game.scene().entities[body].min1Hp = true;
        }
        if (maxHp > 0 && hp > 0 && hp * 10 <= maxHp * 6) {
            healCreature(api, object, maxHp - hp);
        }
    }
}

void WalkDriver::scanScripts(GameSession& game)
{
    const std::vector<std::string>& log = game.scriptLog();
    const std::size_t count = log.size();
    if (count < scriptSeen_) {
        // The session trimmed its log: continue after the last line seen.
        scriptSeen_ = 0;
        for (std::size_t i = count; i-- > 0;) {
            if (log[i] == scriptLastLine_) {
                scriptSeen_ = i + 1;
                break;
            }
        }
    }
    for (std::size_t i = scriptSeen_; i < count; ++i) {
        const std::string& entry = log[i];
        std::string kind;
        std::string name;
        if (entry.rfind("missing script ", 0) == 0) {
            kind = "missing";
            name = entry.substr(15);
        } else if (entry.rfind("decode ", 0) == 0) {
            kind = "decode";
            name = entry.substr(7, entry.find(':') == std::string::npos ? std::string::npos : entry.find(':') - 7);
        } else if (entry.find(" refused") != std::string::npos) {
            kind = "refused";
            const std::vector<std::string> words = splitWords(entry);
            name = words.size() > 1 ? words[1] : entry;
        } else if (const std::size_t at = entry.find(" status="); at != std::string::npos
                   && (entry.rfind("script ", 0) == 0 || entry.rfind("functor ", 0) == 0)) {
            const std::size_t from = at + 8;
            const std::string status = entry.substr(from, entry.find(' ', from) == std::string::npos ? std::string::npos : entry.find(' ', from) - from);
            if (status == "returned" || status == "blocked") {
                continue;
            }
            kind = status;
            const std::vector<std::string> words = splitWords(entry);
            name = words.size() > 1 ? words[1] : entry;
        } else {
            continue;
        }
        const std::string key = kind + " " + name;
        ++scriptFailTotal_;
        const int times = ++scriptFailKinds_[key];
        if (times == 1) {
            scriptFailExample_[key] = concat("module=", game.module(), " step=", stepNo_, " ", entry);
            logInfo(concat("WALK script-fail step=", stepNo_, " module=", game.module(), " kind=", kind, " script=", name,
                           " line=", entry));
        }
    }
    scriptSeen_ = count;
    if (count > 0) {
        scriptLastLine_ = log[count - 1];
    }
}

// ---------------------------------------------------------------------------------------------- dialogue

bool WalkDriver::parsePolicy(const std::vector<std::string>& words, std::size_t from, Policy& out, std::string& error) const
{
    out = Policy{};
    std::vector<std::string> rest;
    for (std::size_t i = from; i < words.size(); ++i) {
        const std::size_t eq = words[i].find('=');
        if (words[i] == "noskip") {
            out.noSkip = true;
        } else if (eq != std::string::npos && isOptionKey(words[i].substr(0, eq))) {
            continue;
        } else {
            rest.push_back(words[i]);
        }
    }
    if (rest.empty() || rest[0] == "first") {
        out.kind = Policy::Kind::First;
        out.text = "first";
        return true;
    }
    const auto addHint = [&](const std::string& spec) {
        // <dlg>:<entry|*>=<option index>[,<dlg>:<entry>=<n>...]
        std::istringstream stream(spec);
        for (std::string part; std::getline(stream, part, ',');) {
            const std::size_t colon = part.find(':');
            const std::size_t eq = part.find('=');
            if (colon == std::string::npos || eq == std::string::npos || eq < colon) {
                error = "bad hint '" + part + "' (want <dlg>:<entry|*>=<option>)";
                return false;
            }
            out.hints[lowerText(part.substr(0, colon)) + ":" + part.substr(colon + 1, eq - colon - 1)] =
                std::atoi(part.c_str() + eq + 1);
        }
        return true;
    };
    if (rest[0] == "index") {
        out.kind = Policy::Kind::Index;
        for (std::size_t i = 1; i < rest.size(); ++i) {
            std::istringstream stream(rest[i]);
            for (std::string part; std::getline(stream, part, ',');) {
                out.indices.push_back(std::atoi(part.c_str()));
            }
        }
        out.text = "index";
        return true;
    }
    if (rest[0] == "hint") {
        out.kind = Policy::Kind::Hint;
        for (std::size_t i = 1; i < rest.size(); ++i) {
            if (!addHint(rest[i])) {
                return false;
            }
        }
        out.text = "hint";
        return true;
    }
    if (rest[0].rfind("hintfile=", 0) == 0) {
        out.kind = Policy::Kind::Hint;
        std::ifstream in(rest[0].substr(9));
        if (!in) {
            error = "cannot read " + rest[0].substr(9);
            return false;
        }
        // One hint per line: <dlg> <entry|*> <option>
        for (std::string line; std::getline(in, line);) {
            if (const std::size_t hash = line.find('#'); hash != std::string::npos) {
                line.erase(hash);
            }
            const std::vector<std::string> parts = splitWords(line);
            if (parts.size() == 3) {
                out.hints[lowerText(parts[0]) + ":" + parts[1]] = std::atoi(parts[2].c_str());
            }
        }
        out.text = "hintfile";
        return true;
    }
    error = "unknown dialogue policy '" + rest[0] + "' (first | index | hint | hintfile=)";
    return false;
}

void WalkDriver::serviceDialogue(GameSession& game, Policy& policy)
{
    const DialogueView& view = game.dialogue();
    if (!view.active) {
        seen_ = DialogueSeen{};
        dialogueStalled_ = false;
        visited_.clear();
        conversationNodes_ = 0;
        return;
    }
    const std::string resref = game.dialogueResref();
    const int optionCount = static_cast<int>(view.options.size());
    if (seen_.resref != resref || seen_.entry != view.entry || seen_.options != optionCount) {
        seen_ = DialogueSeen{resref, view.entry, optionCount, sim_, false};
        dialogueStalled_ = false;
    } else if (sim_ - seen_.since > 30.0) {
        dialogueStalled_ = true;
        if (!seen_.stalledLogged) {
            seen_.stalledLogged = true;
            logWarn(concat("WALK dialogue-stalled step=", stepNo_, " dlg=", resref, " entry=", view.entry,
                           " options=", optionCount, " paused=", game.dialoguePaused() ? 1 : 0));
        }
    }
    if (game.dialoguePaused()) {
        return;
    }
    if (game.dialogueLineSpeaking()) {
        if (!policy.noSkip) {
            game.skipDialogueLine(); // a click on the line, as DialogueScreen sends it
        }
        return;
    }
    std::vector<std::size_t> sayable;
    for (std::size_t i = 0; i < view.options.size(); ++i) {
        if (view.options[i].reply != UINT32_MAX && view.options[i].text != "(Continue)") {
            sayable.push_back(i);
        }
    }
    std::size_t pick = 0;
    const char* via = "continue";
    if (++conversationNodes_ > 300 && !dialogueStalled_) {
        dialogueStalled_ = true; // a hub that never lets go: the step reports it
        logWarn(concat("WALK dialogue-loop step=", stepNo_, " dlg=", resref, " nodes=", conversationNodes_));
    }
    if (!sayable.empty()) {
        // First reply not said yet in this conversation, else the last one (a hub's way out is its last reply).
        pick = sayable.back();
        via = "last";
        for (const std::size_t option : sayable) {
            if (visited_.count(lowerText(resref) + ":" + std::to_string(view.options[option].reply)) == 0) {
                pick = option;
                via = "first";
                break;
            }
        }
        if (policy.kind == Policy::Kind::Index && policy.cursor < policy.indices.size()) {
            const int wanted = policy.indices[policy.cursor++];
            if (wanted >= 0 && static_cast<std::size_t>(wanted) < view.options.size()) {
                pick = static_cast<std::size_t>(wanted);
                via = "index";
            } else {
                via = "index-out-of-range";
            }
        } else if (policy.kind == Policy::Kind::Hint) {
            auto it = policy.hints.find(lowerText(resref) + ":" + std::to_string(view.entry));
            if (it == policy.hints.end()) {
                it = policy.hints.find(lowerText(resref) + ":*");
            }
            if (it != policy.hints.end() && it->second >= 0 && static_cast<std::size_t>(it->second) < view.options.size()) {
                pick = static_cast<std::size_t>(it->second);
                via = "hint";
            }
        }
    }
    const std::uint32_t reply = view.options[pick].reply;
    logInfo(concat("WALK dlg step=", stepNo_, " resref=", resref, " entry=", view.entry, " options=", optionCount,
                   " sayable=", sayable.size(), " pick=", pick, " reply=",
                   reply == UINT32_MAX ? std::string{"end"} : std::to_string(reply), " via=", via));
    ++dialogueNodes_;
    visited_.insert(lowerText(resref) + ":" + std::to_string(reply));
    game.chooseDialogueOption(pick);
}


// ---------------------------------------------------------------------------------------------- steps

int WalkDriver::step(GameSession& game, const std::string& line, bool first)
{
    if (first) {
        const int result = begin(game, line);
        if (result != 0) {
            return result;
        }
    }
    return poll(game);
}

int WalkDriver::done(GameSession& game, bool ok, const std::string& detail)
{
    if (ok) {
        ++passed_;
    } else {
        ++failed_;
        failedLine_ = line_;
    }
    std::string text = concat("WALK step ", stepNo_, ok ? " OK   " : " FAIL ", line_, " | t=", fixed(sim_ - stepStart_, 1), "s ",
                              where(game));
    if (!detail.empty()) {
        text += " " + detail;
    }
    if (ok) {
        logInfo(text);
    } else {
        logError(text);
    }
    stepPolicyOn_ = false;
    return ok ? 1 : -1;
}

int WalkDriver::begin(GameSession& game, const std::string& line)
{
    line_ = line;
    words_ = splitWords(line);
    opts_.clear();
    ++stepNo_;
    stepStart_ = sim_;
    phase_ = 0;
    retries_ = 0;
    lastBlock_.clear();
    settledSince_ = -1.0;
    stepPolicyOn_ = false;
    verb_ = words_.empty() ? std::string{} : words_[0];
    for (std::size_t i = 1; i < words_.size(); ++i) {
        const std::size_t eq = words_[i].find('=');
        if (eq != std::string::npos && isOptionKey(words_[i].substr(0, eq))) {
            opts_[words_[i].substr(0, eq)] = words_[i].substr(eq + 1);
        }
    }
    if (verb_ == "goto") {
        timeout_ = option("timeout", 90.0);
        return beginGoto(game);
    }
    if (verb_ == "use" || verb_ == "talk") {
        timeout_ = option("timeout", 90.0);
        return beginUse(game, verb_ == "talk");
    }
    if (verb_ == "dialogue") {
        timeout_ = option("timeout", 120.0);
        return beginDialogue(game);
    }
    if (verb_ == "autodialogue") {
        std::string error;
        if (words_.size() > 1 && words_[1] == "off") {
            backgroundOn_ = false;
            return done(game, true, "policy=off");
        }
        if (!parsePolicy(words_, 1, background_, error)) {
            return done(game, false, "reason=" + error);
        }
        backgroundOn_ = true;
        return done(game, true, "policy=" + background_.text);
    }
    if (verb_ == "fight") {
        timeout_ = option("timeout", 180.0);
        calm_ = 0.0;
        lastAttack_ = -10.0;
        fightSeen_.clear();
        fightKilled_ = 0;
        return 0;
    }
    if (verb_ == "module") {
        timeout_ = option("timeout", 60.0);
        if (positional().empty()) {
            return done(game, false, "reason=usage: module <name>");
        }
        return 0;
    }
    if (verb_ == "expect") {
        timeout_ = option("timeout", 0.0); // with a timeout the expectation is waited for
        return doExpect(game);
    }
    if (verb_ == "idle") {
        const std::vector<std::string> args = positional();
        idleUntil_ = sim_ + (args.empty() ? 1.0 : std::atof(args[0].c_str()));
        return 0;
    }
    if (verb_ == "partyselect") {
        timeout_ = option("timeout", 30.0);
        return 0;
    }
    if (verb_ == "note") {
        return done(game, true, "");
    }
    if (verb_ == "list") {
        return doList(game);
    }
    if (verb_ == "loot") {
        timeout_ = option("timeout", 5.0);
        return 0;
    }
    if (verb_ == "equip") {
        // The inventory screen's Equip on everything not worn.
        std::vector<std::string> gear;
        for (const InventoryItem& item : game.inventory()) {
            if (!item.equipped) {
                gear.push_back(item.resref);
            }
        }
        int worn = 0;
        for (const std::string& resref : gear) {
            const bool ok = game.equipItem(resref);
            worn += ok ? 1 : 0;
            logInfo(concat("WALK equip step=", stepNo_, " item=", resref, " ok=", ok ? 1 : 0));
        }
        return done(game, true, concat("worn=", worn, " of=", gear.size()));
    }
    if (verb_ == "leader") {
        // The party screen's / Tab key's choice of who is controlled: an NPC slot number or `player`.
        const std::vector<std::string> args = positional();
        if (args.empty()) {
            return done(game, false, "reason=usage: leader <player|0..8>");
        }
        const int npc = args[0] == "player" ? kNpcPlayer : std::atoi(args[0].c_str());
        const bool ok = game.selectLeaderNpc(npc);
        return done(game, ok, ok ? "" : concat("reason=refused npc=", npc, " control_locked=", game.controlLocked() ? 1 : 0));
    }
    if (verb_ == "opendoor") {
        // ActionOpenDoor as a script queues it on the controlled creature (what the retail recording issued): a
        // locked door runs its OnFailToOpen hook, an unlocked one opens. For doors the mouse cannot target.
        const std::vector<std::string> args = positional();
        const std::size_t index = args.empty() ? kNone : game.findEntity(args[0]);
        if (index == kNone) {
            return done(game, false, "reason=unknown-target");
        }
        if (!game.inReach(index, 3.0f)) {
            return done(game, false, "reason=out-of-reach tag=" + args[0]);
        }
        const UseOutcome outcome = game.openDoor(index);
        const bool must = args.size() > 1 && args[1] == "must"; // `must`: only an opened door counts
        return done(game, outcome == UseOutcome::Opened || (outcome == UseOutcome::Locked && !must),
                    concat("tag=", args[0], " outcome=", outcome == UseOutcome::Opened ? "opened" : "locked-hook"));
    }
    if (verb_ == "unlock") {
        // The Security skill on a locked door in reach, as the HUD's unlock action does.
        const std::vector<std::string> args = positional();
        const std::size_t index = args.empty() ? kNone : game.findEntity(args[0]);
        if (index == kNone) {
            return done(game, false, "reason=unknown-target");
        }
        if (!game.inReach(index, 3.0f)) {
            return done(game, false, "reason=out-of-reach tag=" + args[0]);
        }
        // A failed Security roll is tried again, as a player clicks again (up to 25 tries; the key is tried first).
        bool ok = false;
        int tries = 0;
        while (!ok && tries < 25) {
            ok = game.unlockDoor(index);
            ++tries;
        }
        return done(game, ok, concat("tag=", args[0], " tries=", tries, " locked=", game.scene().entities[index].locked ? 1 : 0));
    }
    return done(game, false, "reason=unknown verb");
}

int WalkDriver::poll(GameSession& game)
{
    if (verb_ == "goto") return pollGoto(game);
    if (verb_ == "use" || verb_ == "talk") return pollUse(game);
    if (verb_ == "dialogue") return pollDialogue(game);
    if (verb_ == "fight") return pollFight(game);
    if (verb_ == "module") return pollModule(game);
    if (verb_ == "expect") return doExpect(game);
    if (verb_ == "idle") {
        return sim_ >= idleUntil_ ? done(game, true, "") : 0;
    }
    if (verb_ == "partyselect") return pollPartySelect(game);
    if (verb_ == "loot") {
        if (game.activePanel() == UiPanel::Loot || !game.loot().empty()) {
            const std::size_t items = game.loot().size();
            std::string listed;
            for (const InventoryItem& item : game.loot()) {
                listed += concat(listed.empty() ? "" : ",", item.resref, "x", item.stack);
            }
            game.takeAllLoot(); // the container screen's Take All, then its Close
            game.closeLoot();
            game.setUiPanel(UiPanel::None);
            return done(game, true, concat("items=", items, " [", listed, "]"));
        }
        return sim_ - stepStart_ > timeout_ ? done(game, false, "reason=no-container-open") : 0;
    }
    return done(game, false, "reason=unknown verb");
}

// goto ---------------------------------------------------------------------------------------------

int WalkDriver::beginGoto(GameSession& game)
{
    const std::vector<std::string> args = positional();
    if (args.empty()) {
        return done(game, false, "reason=usage: goto <waypoint|tag|x,y>");
    }
    targetText_ = args[0];
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float defaultStop = 0.5f;
    int numbers = 0;
    {
        // "x,y" or "x,y,z"; anything else is a tag.
        std::istringstream point(targetText_);
        char comma = 0;
        if (point >> x >> comma >> y && comma == ',') {
            numbers = 2;
            if (point >> comma >> z && comma == ',') {
                numbers = 3;
            }
        }
    }
    if (numbers >= 2) {
        targetX_ = x;
        targetY_ = y;
        targetZ_ = numbers >= 3 ? z : game.floorHeightAt(x, y);
    } else {
        // A tag: waypoints win over other objects of the same tag; among equals the nearest to the leader.
        const std::string wanted = lowerText(targetText_);
        const kotor::Vec3 from = game.controlledPosition();
        std::size_t best = kNone;
        for (int pass = 0; pass < 2 && best == kNone; ++pass) {
            float bestDistance = 1.0e9f;
            for (std::size_t i = 0; i < game.scene().entities.size(); ++i) {
                const kotor::Entity& entity = game.scene().entities[i];
                if (lowerText(entity.tag) != wanted || (pass == 0 && entity.kind != kotor::EntityKind::Waypoint)) {
                    continue;
                }
                const float distance = dist2D(from, entity.position);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = i;
                }
            }
        }
        if (best == kNone) {
            return done(game, false, "reason=unknown-target tag=" + targetText_);
        }
        const kotor::Entity& entity = game.scene().entities[best];
        targetX_ = entity.position.x;
        targetY_ = entity.position.y;
        targetZ_ = entity.position.z;
        switch (entity.kind) {
        case kotor::EntityKind::Waypoint: defaultStop = 0.4f; break;
        case kotor::EntityKind::Trigger: defaultStop = 0.3f; break;
        case kotor::EntityKind::Door: defaultStop = 1.8f; break;
        default: defaultStop = 1.5f; break;
        }
        targetText_ += concat("(", kindName(entity.kind), ")");
    }
    stop_ = static_cast<float>(option("stop", defaultStop));
    doorClicks_ = 0;
    gotoModule_ = game.module();
    return 0;
}

int WalkDriver::pollGoto(GameSession& game)
{
    // `through`: the walk goes through a linked door, and the step is done when the module changes under it.
    if (flag("through") && game.module() != gotoModule_) {
        return done(game, true, concat("left=", gotoModule_, " target=", targetText_));
    }
    if (sim_ - stepStart_ > timeout_) {
        const kotor::Vec3 at = game.controlledPosition();
        return done(game, false, concat("reason=timeout phase=", phase_ == 0 ? "waiting-for-control" : "moving", " block=",
                                        lastBlock_.empty() ? "-" : lastBlock_, " target=", targetText_, " at=", fixed(targetX_),
                                        ",", fixed(targetY_), " remaining=", fixed(dist2D(at, {targetX_, targetY_, 0.0f}))));
    }
    if (phase_ == 0) {
        lastBlock_ = blockReason(game);
        if (!lastBlock_.empty()) {
            return 0;
        }
        const kotor::Vec3 target{targetX_, targetY_, targetZ_};
        if (!game.moveTo(target, stop_, !flag("walk"))) {
            std::size_t door = kNone;
            game.pathExists(game.controlledPosition(), target, &door);
            std::string blocker = "none";
            if (door != kNone && door < game.scene().entities.size()) {
                const kotor::Entity& entity = game.scene().entities[door];
                blocker = concat("door:", entity.tag, " open=", entity.open ? 1 : 0, " locked=", entity.locked ? 1 : 0);
            }
            return done(game, false, concat("reason=unreachable target=", targetText_, " at=", fixed(targetX_), ",", fixed(targetY_),
                                            " blocker=", blocker));
        }
        phase_ = 1;
        const kotor::Vec3 at = game.controlledPosition();
        lastProgressX_ = at.x;
        lastProgressY_ = at.y;
        lastProgressAt_ = sim_;
        return 0;
    }
    const kotor::Vec3 at = game.controlledPosition();
    if (phase_ == 2) {
        // The click on the door is walking to it; when it has opened (or not), the walk is issued again.
        if (game.usePending() || game.movePending()) {
            return 0;
        }
        const int outcome = game.takeLastUseOutcome();
        if (outcome == 3 || outcome == 2 || outcome == 1 || outcome == 0) {
            return done(game, false, concat("reason=door-", useOutcomeName(outcome), " target=", targetText_));
        }
        phase_ = 0;
        return 0;
    }
    if (game.movePending()) {
        if (dist2D(at, {lastProgressX_, lastProgressY_, 0.0f}) > 0.3f) {
            lastProgressX_ = at.x;
            lastProgressY_ = at.y;
            lastProgressAt_ = sim_;
        } else if (sim_ - lastProgressAt_ > 6.0) {
            return done(game, false, concat("reason=stuck target=", targetText_, " remaining=",
                                            fixed(dist2D(at, {targetX_, targetY_, 0.0f}))));
        }
        return 0;
    }
    const float remaining = dist2D(at, {targetX_, targetY_, 0.0f});
    if (remaining <= std::max(stop_, 0.5f) + 0.6f) {
        return done(game, true, concat("target=", targetText_, " remaining=", fixed(remaining)));
    }
    if (game.moveOutcome() == GameSession::MoveOutcome::DoorBlocked) {
        const std::size_t door = game.moveBlockingDoor();
        // A closed door that is not locked: the player clicks it, then walks on.
        if (door != kNone && door < game.scene().entities.size() && !game.scene().entities[door].locked
            && !game.scene().entities[door].open && doorClicks_ < 8 && blockReason(game).empty()) {
            ++doorClicks_;
            game.takeLastUseOutcome();
            if (game.performDefaultAction(door)) {
                logInfo(concat("WALK note step=", stepNo_, " goto opens door=", game.scene().entities[door].tag));
                phase_ = 2;
                return 0;
            }
        }
        std::string blocker = "door";
        if (door != kNone && door < game.scene().entities.size()) {
            const kotor::Entity& entity = game.scene().entities[door];
            blocker = concat("door:", entity.tag, " open=", entity.open ? 1 : 0, " locked=", entity.locked ? 1 : 0,
                             " key=", entity.keyName.empty() ? "-" : entity.keyName);
        }
        return done(game, false, concat("reason=blocked target=", targetText_, " remaining=", fixed(remaining), " blocker=", blocker));
    }
    if (game.moveOutcome() == GameSession::MoveOutcome::Unreachable) {
        return done(game, false, concat("reason=unreachable target=", targetText_, " remaining=", fixed(remaining)));
    }
    // The walk ended short without a verdict: a scene or script took the control (the player would click again).
    const std::string block = blockReason(game);
    if (retries_ < 6) {
        ++retries_;
        logInfo(concat("WALK note step=", stepNo_, " goto interrupted block=", block.empty() ? "-" : block, " remaining=",
                       fixed(remaining), " retry=", retries_));
        phase_ = 0;
        return 0;
    }
    return done(game, false, concat("reason=ended-short target=", targetText_, " remaining=", fixed(remaining), " block=",
                                    block.empty() ? "-" : block));
}

// use / talk ---------------------------------------------------------------------------------------

int WalkDriver::beginUse(GameSession& game, bool talk)
{
    talk_ = talk;
    const std::vector<std::string> args = positional();
    if (args.empty()) {
        return done(game, false, "reason=usage: use|talk <tag>");
    }
    useTag_ = args[0];
    const std::string wanted = lowerText(useTag_);
    const kotor::Vec3 from = game.controlledPosition();
    useIndex_ = kNone;
    float bestDistance = 1.0e9f;
    for (std::size_t i = 0; i < game.scene().entities.size(); ++i) {
        const kotor::Entity& entity = game.scene().entities[i];
        if (lowerText(entity.tag) != wanted || entity.dead) {
            continue;
        }
        const bool kindOk = talk ? entity.kind == kotor::EntityKind::Creature
                                 : (entity.kind == kotor::EntityKind::Door || entity.kind == kotor::EntityKind::Placeable
                                    || entity.kind == kotor::EntityKind::Store);
        // The mouse selects only what can be used: a static prop that shares a tag with a container is not a target.
        const bool selectable = talk || entity.kind == kotor::EntityKind::Door || entity.useable;
        const float distance = dist2D(from, entity.position) + (selectable ? 0.0f : 1.0e6f);
        if (kindOk && distance < bestDistance) {
            bestDistance = distance;
            useIndex_ = i;
        }
    }
    if (useIndex_ == kNone) {
        return done(game, false, "reason=unknown-target tag=" + useTag_ + " (alive " + (talk ? "creature" : "door/placeable") + ")");
    }
    return 0;
}

int WalkDriver::pollUse(GameSession& game)
{
    if (sim_ - stepStart_ > timeout_) {
        return done(game, false, concat("reason=timeout phase=", phase_ == 0 ? "waiting-for-control" : "using", " block=",
                                        lastBlock_.empty() ? "-" : lastBlock_, " tag=", useTag_));
    }
    if (useIndex_ >= game.scene().entities.size()) {
        return done(game, false, "reason=target-gone tag=" + useTag_);
    }
    const kotor::Entity& entity = game.scene().entities[useIndex_];
    if (phase_ == 0) {
        lastBlock_ = blockReason(game);
        if (!lastBlock_.empty()) {
            return 0;
        }
        if (talk_ && entity.hostile) {
            return done(game, false, "reason=hostile (the default action is an attack) tag=" + useTag_);
        }
        game.takeLastUseOutcome();
        if (!game.performDefaultAction(useIndex_)) {
            return done(game, false, concat("reason=not-usable tag=", useTag_, " kind=", kindName(entity.kind), " dead=", entity.dead ? 1 : 0,
                                            " useable=", entity.useable ? 1 : 0));
        }
        phase_ = 1;
        lastProgressAt_ = sim_;
        const kotor::Vec3 at = game.controlledPosition();
        lastProgressX_ = at.x;
        lastProgressY_ = at.y;
        return 0;
    }
    const kotor::Vec3 at = game.controlledPosition();
    if (game.usePending() || game.movePending()) {
        if (dist2D(at, {lastProgressX_, lastProgressY_, 0.0f}) > 0.3f) {
            lastProgressX_ = at.x;
            lastProgressY_ = at.y;
            lastProgressAt_ = sim_;
        } else if (sim_ - lastProgressAt_ > 6.0) {
            return done(game, false, concat("reason=stuck tag=", useTag_, " distance=", fixed(dist2D(at, entity.position))));
        }
        return 0;
    }
    if (talk_) {
        if (game.dialogue().active) {
            return done(game, true, concat("tag=", useTag_, " dlg=", game.dialogueResref()));
        }
    } else {
        const int outcome = game.takeLastUseOutcome();
        if (outcome == 4 || outcome == 5 || outcome == 6 || outcome == 7 || outcome == 8) {
            return done(game, true, concat("tag=", useTag_, " outcome=", useOutcomeName(outcome)));
        }
        if (outcome >= 0) {
            return done(game, false, concat("reason=", useOutcomeName(outcome), " tag=", useTag_, " locked=", entity.locked ? 1 : 0,
                                            " key=", entity.keyName.empty() ? "-" : entity.keyName, " distance=",
                                            fixed(dist2D(at, entity.position))));
        }
    }
    // Settled with nothing to show for it yet: a script reply (OnDialogue, OnUsed) gets two seconds.
    if (settledSince_ < 0.0) {
        settledSince_ = sim_;
    }
    if (sim_ - settledSince_ < 2.0) {
        return 0;
    }
    if (talk_) {
        // What the creature's OnDialogue hook did: the session's last script lines.
        const std::vector<std::string>& lines = game.scriptLog();
        for (std::size_t i = lines.size() > 40 ? lines.size() - 40 : 0; i < lines.size(); ++i) {
            logInfo(concat("WALK script-log ", lines[i]));
        }
        const std::vector<std::string>& combatLog = game.combatLog();
        for (std::size_t i = combatLog.size() > 16 ? combatLog.size() - 16 : 0; i < combatLog.size(); ++i) {
            logInfo(concat("WALK combat-log ", combatLog[i]));
        }
    }
    std::string blocker;
    if (game.moveOutcome() == GameSession::MoveOutcome::DoorBlocked && game.moveBlockingDoor() < game.scene().entities.size()) {
        blocker = " blocker=door:" + game.scene().entities[game.moveBlockingDoor()].tag;
    }
    return done(game, false, concat("reason=", talk_ ? "no-conversation" : "walk-ended-short", " tag=", useTag_, " distance=",
                                    fixed(dist2D(at, entity.position)), " move=", static_cast<int>(game.moveOutcome()), blocker));
}

// dialogue -----------------------------------------------------------------------------------------

int WalkDriver::beginDialogue(GameSession& game)
{
    std::string error;
    if (!parsePolicy(words_, 1, stepPolicy_, error)) {
        return done(game, false, "reason=" + error);
    }
    stepPolicyOn_ = true;
    sawDialogue_ = game.dialogue().active;
    dialogueNodes_ = 0;
    return 0;
}

int WalkDriver::pollDialogue(GameSession& game)
{
    stepPolicyOn_ = true;
    if (game.dialogue().active) {
        sawDialogue_ = true;
        if (dialogueStalled_) {
            return done(game, false, concat("reason=stalled dlg=", seen_.resref, " entry=", seen_.entry, " options=", seen_.options));
        }
    } else if (sawDialogue_) {
        return done(game, true, concat("nodes=", dialogueNodes_, " policy=", stepPolicy_.text));
    }
    if (sim_ - stepStart_ > timeout_ && !sawDialogue_ && std::find(words_.begin(), words_.end(), "optional") != words_.end()) {
        return done(game, true, "nodes=0 none-started (optional)");
    }
    if (sim_ - stepStart_ > timeout_) {
        return done(game, false, concat("reason=timeout ", sawDialogue_ ? "dialogue-never-ended" : "no-dialogue-started", " nodes=",
                                        dialogueNodes_));
    }
    return 0;
}

// fight --------------------------------------------------------------------------------------------

int WalkDriver::pollFight(GameSession& game)
{
    if (game.partyAllDown() || game.gameOverMessage()) {
        // The last combat log lines say what landed (player hp lines carry the hit sizes).
        const std::vector<std::string>& combatLog = game.combatLog();
        for (std::size_t i = combatLog.size() > 14 ? combatLog.size() - 14 : 0; i < combatLog.size(); ++i) {
            logInfo(concat("WALK combat-log ", combatLog[i]));
        }
        return done(game, false, concat("reason=party-wiped hp=", game.controlledHp(), "/", game.controlledMaxHp(), " player_dead=",
                                        game.playerDead() ? 1 : 0, " godmode=", godmode_ ? 1 : 0));
    }
    const float range = static_cast<float>(option("range", 20.0));
    const kotor::Vec3 at = game.controlledPosition();
    ScriptApi api{game};
    std::size_t nearest = kNone;
    float nearestDistance = 1.0e9f;
    std::string alive;
    int count = 0;
    const std::uint32_t self = game.controlledObject();
    for (std::size_t i = 0; i < game.scene().entities.size(); ++i) {
        const kotor::Entity& entity = game.scene().entities[i];
        if (entity.kind != kotor::EntityKind::Creature || entity.partyAvatar) {
            continue;
        }
        const std::uint32_t object = game.objectForEntityIndex(i);
        if (object == script::kObjectInvalid || !isEnemy(api, self, object)) {
            continue;
        }
        const float distance = dist2D(at, entity.position);
        if (distance > range) {
            if (entity.dead && fightSeen_.erase(entity.tag + "#" + std::to_string(i)) > 0) {
                ++fightKilled_;
            }
            continue;
        }
        const std::string key = entity.tag + "#" + std::to_string(i);
        if (entity.dead) {
            if (fightSeen_.erase(key) > 0) {
                ++fightKilled_;
            }
            continue;
        }
        fightSeen_.insert(key);
        ++count;
        if (alive.size() < 200) {
            alive += (alive.empty() ? "" : ",") + entity.tag + "@" + fixed(distance, 1) + "/hp" + std::to_string(entity.hitPoints);
        }
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = i;
        }
    }
    if (sim_ - stepStart_ > timeout_) {
        return done(game, false, concat("reason=timeout hostiles=", count, " [", alive, "] killed=", fightKilled_, " player_hp=",
                                        game.controlledHp(), "/", game.controlledMaxHp()));
    }
    const std::string block = blockReason(game);
    if (count == 0) {
        calm_ += fixedDt_ > 0.0 ? fixedDt_ : 0.016;
        if (block.empty() && sim_ - stepStart_ >= 1.0 && calm_ >= 1.5 && game.controlledQueueSize() == 0) {
            return done(game, true, concat("killed=", fightKilled_, " hp=", game.controlledHp(), "/", game.controlledMaxHp()));
        }
        return 0;
    }
    calm_ = 0.0;
    if (!block.empty() && block != "dialogue") {
        return 0;
    }
    if (block.empty() && sim_ - lastAttack_ >= 0.75 && nearest != kNone
        && (game.controlledQueueSize() == 0 || game.combatTarget() == UINT32_MAX
            || game.combatTarget() >= game.scene().entities.size() || game.scene().entities[game.combatTarget()].dead)) {
        lastAttack_ = sim_;
        game.attackNearestHostile();
    }
    return 0;
}

// module -------------------------------------------------------------------------------------------

int WalkDriver::pollModule(GameSession& game)
{
    const std::string wanted = positional().front();
    if (lowerText(game.module()) == lowerText(wanted)) {
        return done(game, true, "");
    }
    if (sim_ - stepStart_ > timeout_) {
        return done(game, false, concat("reason=timeout wanted=", wanted, " block=", blockReason(game).empty() ? "-" : blockReason(game)));
    }
    return 0;
}

// expect -------------------------------------------------------------------------------------------

int WalkDriver::doExpect(GameSession& game)
{
    // `done` for an expectation: a false one keeps waiting while the step's timeout (timeout=<s>) has not run out.
    const auto verdict = [&](bool ok, const std::string& detail) {
        if (!ok && timeout_ > 0.0 && sim_ - stepStart_ < timeout_) {
            return 0;
        }
        return done(game, ok, detail);
    };
    const std::vector<std::string> args = positional();
    if (args.empty()) {
        return done(game, false, "reason=usage: expect module|global|journal|alive|dead|near|credits|party ...");
    }
    const std::string& what = args[0];
    if (what == "module" && args.size() >= 2) {
        const bool ok = lowerText(game.module()) == lowerText(args[1]);
        return verdict(ok, ok ? "" : "reason=module-is-" + game.module());
    }
    if (what == "global" && args.size() >= 4) {
        const std::int64_t value = game.scriptWorld().getGlobal(args[1]).asInt();
        const bool ok = compare(static_cast<double>(value), args[2], std::atof(args[3].c_str()));
        return verdict(ok, concat("value=", value));
    }
    if (what == "journal" && args.size() >= 4) {
        const std::int32_t value = game.journalState(args[1]);
        const bool ok = compare(value, args[2], std::atof(args[3].c_str()));
        return verdict(ok, concat("state=", value));
    }
    if ((what == "alive" || what == "dead") && args.size() >= 2) {
        const std::string wanted = lowerText(args[1]);
        int found = 0;
        int dead = 0;
        for (const kotor::Entity& entity : game.scene().entities) {
            if (entity.kind == kotor::EntityKind::Creature && lowerText(entity.tag) == wanted) {
                ++found;
                dead += entity.dead ? 1 : 0;
            }
        }
        const bool ok = found > 0 && (what == "dead" ? dead == found : dead < found);
        return verdict(ok, concat("found=", found, " dead=", dead));
    }
    if (what == "near" && args.size() >= 3) {
        const std::size_t index = game.findEntity(args[1]);
        if (index == kNone) {
            return done(game, false, "reason=unknown-target tag=" + args[1]);
        }
        const float distance = dist2D(game.controlledPosition(), game.scene().entities[index].position);
        return verdict(distance <= std::atof(args[2].c_str()), concat("distance=", fixed(distance)));
    }
    if (what == "credits") {
        return verdict(game.creditsActive() || game.gameEnded(), "");
    }
    if (what == "party" && args.size() >= 2) {
        bool found = false;
        for (const CompanionState& companion : game.companions()) {
            found = found || lowerText(companion.tag) == lowerText(args[1]);
        }
        return verdict(found, "");
    }
    return done(game, false, "reason=unknown expectation '" + what + "'");
}

// list ---------------------------------------------------------------------------------------------

// Route authoring: what is around the leader (tags, kinds, positions; resrefs, never text).
int WalkDriver::doList(GameSession& game)
{
    const auto kindsIt = opts_.find("kinds");
    const std::string kinds = kindsIt == opts_.end() ? std::string{} : "," + lowerText(kindsIt->second) + ",";
    const float range = static_cast<float>(option("range", 1.0e6));
    const kotor::Vec3 from = game.controlledPosition();
    int printed = 0;
    for (std::size_t i = 0; i < game.scene().entities.size() && printed < 400; ++i) {
        const kotor::Entity& entity = game.scene().entities[i];
        const float distance = dist2D(from, entity.position);
        if (distance > range || (!kinds.empty() && kinds.find("," + std::string(kindName(entity.kind)) + ",") == std::string::npos)) {
            continue;
        }
        ++printed;
        std::string extra;
        if (!entity.conversation.empty()) extra += " conv=" + entity.conversation;
        if (!entity.linkedModule.empty()) extra += " link=" + entity.linkedModule + "/" + entity.linkedTo;
        if (entity.kind == kotor::EntityKind::Door || entity.kind == kotor::EntityKind::Placeable) {
            extra += concat(" open=", entity.open ? 1 : 0, " locked=", entity.locked ? 1 : 0);
            if (!entity.keyName.empty()) extra += " key=" + entity.keyName;
        }
        if (entity.kind == kotor::EntityKind::Creature) {
            extra += concat(" hostile=", entity.hostile ? 1 : 0, " dead=", entity.dead ? 1 : 0, " hp=", entity.hitPoints);
        }
        logInfo(concat("WALK entity #", i, " obj=", game.objectForEntityIndex(i), " tag=", entity.tag, " kind=", kindName(entity.kind), " pos=", fixed(entity.position.x), ",",
                       fixed(entity.position.y), ",", fixed(entity.position.z), " dist=", fixed(distance, 1), extra));
    }
    return done(game, true, concat("entities=", printed));
}

// partyselect --------------------------------------------------------------------------------------

int WalkDriver::pollPartySelect(GameSession& game)
{
    if (game.partySelection().active) {
        // The screen's own Accept: the exit script runs as it does for the player.
        const std::size_t chosen = game.partySelection().chosen.size();
        const bool ok = game.acceptPartySelection();
        return done(game, ok, concat("chosen=", chosen));
    }
    if (sim_ - stepStart_ > timeout_) {
        return done(game, false, "reason=timeout no-party-selection");
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------- report

int WalkDriver::finish(GameSession* game, bool scriptFinished, const std::string& exitReason)
{
    std::vector<float> sorted = frameMs_;
    std::sort(sorted.begin(), sorted.end());
    double total = 0.0;
    for (const float value : sorted) {
        total += value;
    }
    const auto at = [&](double fraction) {
        return sorted.empty() ? 0.0f : sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(sorted.size())))];
    };
    int stalls = 0;
    double stallMs = 0.0;
    for (const float value : sorted) {
        if (value > 250.0f) {
            ++stalls;
            stallMs += value;
        }
    }
    logInfo(concat("WALK frames count=", sorted.size(), " avg_ms=", fixed(sorted.empty() ? 0.0 : total / static_cast<double>(sorted.size())),
                   " p50_ms=", fixed(at(0.5)), " p99_ms=", fixed(at(0.99)), " max_ms=", fixed(sorted.empty() ? 0.0 : sorted.back()),
                   " stalls_over_250ms=", stalls, " stall_total_ms=", fixed(stallMs, 0)));
    for (const auto& [module, ms] : loads_) {
        logInfo(concat("WALK load-time module=", module, " ms=", fixed(ms, 0)));
    }
    // .txi files are optional companions of a texture: the engine asks for every one and most do not exist, so
    // they are counted apart and not listed.
    std::vector<kotor::ResourceMiss> misses;
    std::uint64_t missTotal = 0;
    std::uint64_t txiDistinct = 0;
    for (const kotor::ResourceMiss& miss : kotor::resourceMisses()) {
        if (miss.kind == "txi") {
            ++txiDistinct;
            continue;
        }
        misses.push_back(miss);
        missTotal += miss.count;
    }
    logInfo(concat("WALK resource-misses distinct=", misses.size(), " total=", missTotal, " optional_txi_distinct=", txiDistinct));
    for (std::size_t i = 0; i < misses.size() && i < 60; ++i) {
        logInfo(concat("WALK miss kind=", misses[i].kind, " resref=", misses[i].resref, " count=", misses[i].count));
    }
    logInfo(concat("WALK script-failures total=", scriptFailTotal_, " distinct=", scriptFailKinds_.size()));
    for (const auto& [key, count] : scriptFailKinds_) {
        logInfo(concat("WALK script-fail-count key=\"", key, "\" count=", count, " first: ", scriptFailExample_[key]));
    }
    const bool pass = failed_ == 0 && scriptFinished && stepNo_ > 0;
    logInfo(concat("WALK summary steps=", stepNo_, " ok=", passed_, " fail=", failed_, " finished=", scriptFinished ? 1 : 0,
                   " exit=\"", exitReason, "\" sim_s=", fixed(sim_, 1), " last_module=", game != nullptr ? game->module() : std::string{"-"}));
    if (!scriptFinished && failed_ == 0) {
        logError(concat("WALK step ", stepNo_, " FAIL ", line_, " | reason=run-ended-before-step-finished exit=\"", exitReason, "\""));
    }
    if (pass) {
        logInfo("WALK RESULT PASS");
    } else {
        logError(concat("WALK RESULT FAIL first_failed=\"", failedLine_.empty() ? line_ : failedLine_, "\""));
    }
    return pass ? 0 : 40;
}

// ---------------------------------------------------------------------------------------------- start state

bool walkStartNewGame(GameSession& game, int classSlot)
{
    Chargen chargen(game.rules());
    chargen.selectSlot(classSlot); // 2: male Soldier, the default of the class screen
    chargen.applyRecommended(Chargen::Step::Abilities);
    chargen.applyRecommended(Chargen::Step::Skills);
    chargen.applyRecommended(Chargen::Step::Feats);
    chargen.setName("Player");
    const CharacterSheet sheet = chargen.build();
    game.applyCharacter(sheet);
    game.setPlayerAppearance(static_cast<std::uint32_t>(std::max(0, chargen.appearanceId())), chargen.name());
    game.setThirdPerson(true);
    game.startNewGame("end_m01aa");
    logInfo(concat("WALK start new-game class=", chargen.classId(), " name=", chargen.name()));
    return true;
}

bool walkStartAtWaypoint(GameSession& game, const std::string& tag)
{
    const std::string wanted = lowerText(tag);
    for (const kotor::Entity& entity : game.scene().entities) {
        if (entity.kind == kotor::EntityKind::Waypoint && lowerText(entity.tag) == wanted) {
            const bool ok = game.walkPlaceLeader(entity.position, entity.bearing);
            logInfo(concat("WALK start waypoint=", tag, " ok=", ok ? 1 : 0, " pos=", fixed(entity.position.x), ",", fixed(entity.position.y),
                           ",", fixed(entity.position.z)));
            return ok;
        }
    }
    logError(concat("WALK start waypoint=", tag, " not found in ", game.module()));
    return false;
}

bool walkApplySeedFile(GameSession& game, const std::string& path)
{
    std::ifstream in(path);
    if (!in) {
        logError(concat("WALK seed cannot read ", path));
        return false;
    }
    bool ok = true;
    int lineNo = 0;
    for (std::string line; std::getline(in, line);) {
        ++lineNo;
        if (const std::size_t hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        for (char& ch : line) {
            if (ch == '=') {
                ch = ' ';
            }
        }
        const std::vector<std::string> words = splitWords(line);
        if (words.empty()) {
            continue;
        }
        if (words[0] == "global" && words.size() >= 3) {
            game.scriptWorld().setGlobal(words[1], script::Value::makeInt(std::atoll(words[2].c_str())));
        } else if (words[0] == "journal" && words.size() >= 3) {
            ok = game.addJournalEntry(words[1], std::atoi(words[2].c_str())) && ok;
        } else if (words[0] == "party" && words.size() >= 2) {
            std::istringstream names(words[1]);
            for (std::string name; std::getline(names, name, ',');) {
                ok = game.addPartyMember(name) && ok;
            }
        } else if (words[0] == "gold" && words.size() >= 2) {
            game.setGoldForTest(std::atoi(words[1].c_str()));
        } else if (words[0] == "xp" && words.size() >= 2) {
            game.giveXp(std::atoi(words[1].c_str()));
        } else {
            logError(concat("WALK seed ", path, ":", lineNo, ": cannot read '", line, "'"));
            ok = false;
        }
    }
    logInfo(concat("WALK start seed=", path, " ok=", ok ? 1 : 0));
    return ok;
}

} // namespace ke
