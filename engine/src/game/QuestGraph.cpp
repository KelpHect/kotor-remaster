// Stage 1 of the quest tracer: the static graph. docs/recon/64_QUEST_TRACE.md.
//
// Each NCS script is walked by a small symbolic stack machine. It keeps constants and
// plot reads on the stack, follows JSR with the real arguments, explores both sides of a
// branch it cannot decide and records, at every interesting action, the constant
// arguments and the conditions that led there. Nothing is executed and no world exists.
// An argument it cannot resolve is reported, never guessed.
#include "game/QuestGraph.h"

#include "script/ActionTable.h"
#include "script/Ncs.h"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <optional>
#include <queue>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace ke::quest {

namespace {

using script::Instruction;
using script::Op;

constexpr std::uint32_t kNcs = 2010;
constexpr std::uint32_t kAre = 2012;
constexpr std::uint32_t kIfo = 2014;
constexpr std::uint32_t kGit = 2023;
constexpr std::uint32_t kUtc = 2027;
constexpr std::uint32_t kDlg = 2029;
constexpr std::uint32_t kUtt = 2032;
constexpr std::uint32_t kUte = 2040;
constexpr std::uint32_t kUtd = 2042;
constexpr std::uint32_t kUtp = 2044;
constexpr std::uint32_t kUtm = 2051;
constexpr std::uint32_t kJrl = 2056;
constexpr std::uint32_t kUtw = 2058;

std::string lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

// ---- symbolic values -------------------------------------------------------------------

enum class SymKind : std::uint8_t { Unknown, Int, Str, Obj, Read, Call, Cmp, Not, And, Or };
enum class ReadKind : std::int64_t { GlobalBool, GlobalNum, GlobalStr, Journal, LocalBool, LocalNum, Item, Exists };
enum class CmpOp : std::int64_t { Eq, Ne, Gt, Ge, Lt, Le };

struct Sym {
    SymKind kind = SymKind::Unknown;
    std::int64_t number = 0; // Int value, ReadKind or CmpOp.
    std::string text;        // Str value, object name, read or call name.
    std::string owner;       // Read: local owner or item holder.
    const Sym* a = nullptr;
    const Sym* b = nullptr;
    int depth = 0;
};

class Interner {
public:
    const Sym* make(Sym sym)
    {
        std::string key;
        key.reserve(sym.text.size() + sym.owner.size() + 40);
        key += static_cast<char>('A' + static_cast<int>(sym.kind));
        key += std::to_string(sym.number);
        key += '|';
        key += sym.text;
        key += '|';
        key += sym.owner;
        key += '|';
        key += std::to_string(reinterpret_cast<std::uintptr_t>(sym.a));
        key += '|';
        key += std::to_string(reinterpret_cast<std::uintptr_t>(sym.b));
        const auto found = map_.find(key);
        if (found != map_.end()) {
            return found->second;
        }
        sym.depth = 1 + std::max(sym.a != nullptr ? sym.a->depth : 0, sym.b != nullptr ? sym.b->depth : 0);
        store_.push_back(std::move(sym));
        const Sym* out = &store_.back();
        map_.emplace(std::move(key), out);
        return out;
    }

private:
    std::deque<Sym> store_;
    std::unordered_map<std::string, const Sym*> map_;
};

const char* cmpName(CmpOp op)
{
    switch (op) {
    case CmpOp::Eq: return "eq";
    case CmpOp::Ne: return "ne";
    case CmpOp::Gt: return "gt";
    case CmpOp::Ge: return "ge";
    case CmpOp::Lt: return "lt";
    case CmpOp::Le: return "le";
    }
    return "eq";
}

CmpOp cmpInvert(CmpOp op)
{
    switch (op) {
    case CmpOp::Eq: return CmpOp::Ne;
    case CmpOp::Ne: return CmpOp::Eq;
    case CmpOp::Gt: return CmpOp::Le;
    case CmpOp::Ge: return CmpOp::Lt;
    case CmpOp::Lt: return CmpOp::Ge;
    case CmpOp::Le: return CmpOp::Gt;
    }
    return CmpOp::Eq;
}

CmpOp cmpSwap(CmpOp op)
{
    switch (op) {
    case CmpOp::Gt: return CmpOp::Lt;
    case CmpOp::Ge: return CmpOp::Le;
    case CmpOp::Lt: return CmpOp::Gt;
    case CmpOp::Le: return CmpOp::Ge;
    default: return op;
    }
}

void readInto(const Sym& read, Req& req)
{
    switch (static_cast<ReadKind>(read.number)) {
    case ReadKind::GlobalBool:
        req.boolRead = true;
        req.kind = "global";
        break;
    case ReadKind::GlobalNum:
    case ReadKind::GlobalStr: req.kind = "global"; break;
    case ReadKind::Journal: req.kind = "journal"; break;
    case ReadKind::LocalBool:
    case ReadKind::LocalNum: req.kind = "local"; break;
    case ReadKind::Item: req.kind = "item"; break;
    case ReadKind::Exists: req.kind = "exists"; break;
    }
    req.name = read.text;
    req.owner = read.owner;
}

// A sufficient condition for `sym` to have the wanted truth. A disjunction takes its
// first arm: the graph records one way through, and the harness tests that way.
void requirementsOf(const Sym* sym, bool truth, ReqSet& out)
{
    if (sym == nullptr) {
        return;
    }
    switch (sym->kind) {
    case SymKind::Read: {
        Req req;
        readInto(*sym, req);
        req.op = truth ? "true" : "false";
        out.push_back(std::move(req));
        return;
    }
    case SymKind::Call: {
        Req req;
        req.kind = "call";
        req.name = sym->text;
        req.op = truth ? "true" : "false";
        out.push_back(std::move(req));
        return;
    }
    case SymKind::Not:
        requirementsOf(sym->a, !truth, out);
        return;
    case SymKind::And:
        requirementsOf(sym->a, truth, out);
        if (truth) {
            requirementsOf(sym->b, true, out);
        }
        return;
    case SymKind::Or:
        requirementsOf(sym->a, truth, out);
        if (!truth) {
            requirementsOf(sym->b, false, out);
        }
        return;
    case SymKind::Cmp: {
        const CmpOp op = truth ? static_cast<CmpOp>(sym->number) : cmpInvert(static_cast<CmpOp>(sym->number));
        Req req;
        req.op = cmpName(op);
        if (sym->a->kind == SymKind::Read) {
            readInto(*sym->a, req);
        } else if (sym->a->kind == SymKind::Call) {
            req.kind = "call";
            req.name = sym->a->text;
        } else {
            req.kind = "unknown";
        }
        if (sym->b->kind == SymKind::Int) {
            req.value = sym->b->number;
        } else if (sym->b->kind == SymKind::Str) {
            req.text = sym->b->text;
        } else if (sym->b->kind == SymKind::Call || sym->b->kind == SymKind::Obj) {
            // A comparison of two engine answers: GetEnteringObject() == GetFirstPC().
            req.kind = "call";
            req.name = (sym->a->kind == SymKind::Call || sym->a->kind == SymKind::Obj ? sym->a->text
                                                                                      : std::string("value")) + "~"
                + sym->b->text;
        } else {
            req.kind = "unknown";
        }
        out.push_back(std::move(req));
        return;
    }
    case SymKind::Int:
    case SymKind::Str:
    case SymKind::Obj:
        return;
    case SymKind::Unknown: {
        Req req;
        req.kind = "unknown";
        req.op = truth ? "true" : "false";
        out.push_back(std::move(req));
        return;
    }
    }
}

// ---- interesting actions ---------------------------------------------------------------

enum class Act : std::uint8_t {
    None, AddJournal, RemoveJournal, GetJournal, SetGlobalBool, SetGlobalNum, SetGlobalStr,
    GetGlobalBool, GetGlobalNum, GetGlobalStr, SetLocalBool, SetLocalNum, GetLocalBool,
    GetLocalNum, CreateObject, StartNewModule, GiveXp, GivePlotXp, CreateItem, GiveItem, TakeItem,
    PossessedBy, IsValid, ByTag, NearestByTag, FirstPc, Module, Area, ExecuteScript,
    StartConversation, EndGame, AddAvailable, AddParty, SetLocked, OpenDoor, UnlockDoor,
    PlanetAvailable, PlanetSelectable, ShowGalaxy, PartyGui, Pazaak,
};

const std::array<Act, 772>& actionClasses()
{
    static const std::array<Act, 772> table = [] {
        std::array<Act, 772> out{};
        const std::map<std::string, Act> names = {
            {"AddJournalQuestEntry", Act::AddJournal},
            {"RemoveJournalQuestEntry", Act::RemoveJournal},
            {"GetJournalEntry", Act::GetJournal},
            {"SetGlobalBoolean", Act::SetGlobalBool},
            {"SetGlobalNumber", Act::SetGlobalNum},
            {"SetGlobalString", Act::SetGlobalStr},
            {"GetGlobalBoolean", Act::GetGlobalBool},
            {"GetGlobalNumber", Act::GetGlobalNum},
            {"GetGlobalString", Act::GetGlobalStr},
            {"SetLocalBoolean", Act::SetLocalBool},
            {"SetLocalNumber", Act::SetLocalNum},
            {"GetLocalBoolean", Act::GetLocalBool},
            {"GetLocalNumber", Act::GetLocalNum},
            {"CreateObject", Act::CreateObject},
            {"StartNewModule", Act::StartNewModule},
            {"GiveXPToCreature", Act::GiveXp},
            {"GivePlotXP", Act::GivePlotXp},
            {"CreateItemOnObject", Act::CreateItem},
            {"ActionGiveItem", Act::GiveItem},
            {"ActionTakeItem", Act::TakeItem},
            {"GetItemPossessedBy", Act::PossessedBy},
            {"GetIsObjectValid", Act::IsValid},
            {"GetObjectByTag", Act::ByTag},
            {"GetWaypointByTag", Act::ByTag},
            {"GetNearestObjectByTag", Act::NearestByTag},
            {"GetFirstPC", Act::FirstPc},
            {"GetPCSpeaker", Act::FirstPc},
            {"GetPartyLeader", Act::FirstPc},
            {"GetModule", Act::Module},
            {"GetArea", Act::Area},
            {"ExecuteScript", Act::ExecuteScript},
            {"ActionStartConversation", Act::StartConversation},
            {"BeginConversation", Act::StartConversation},
            {"EndGame", Act::EndGame},
            {"AddAvailableNPCByTemplate", Act::AddAvailable},
            {"AddAvailableNPCByObject", Act::AddAvailable},
            {"AddPartyMember", Act::AddParty},
            {"SetLocked", Act::SetLocked},
            {"ActionOpenDoor", Act::OpenDoor},
            {"ActionUnlockObject", Act::UnlockDoor},
            {"SetPlanetAvailable", Act::PlanetAvailable},
            {"SetPlanetSelectable", Act::PlanetSelectable},
            {"ShowGalaxyMap", Act::ShowGalaxy},
            {"ShowPartySelectionGUI", Act::PartyGui},
            {"PlayPazaak", Act::Pazaak},
        };
        for (const script::ActionSpec& spec : script::actionTable()) {
            const auto it = names.find(spec.name);
            if (it != names.end() && spec.id < out.size()) {
                out[spec.id] = it->second;
            }
        }
        return out;
    }();
    return table;
}

// ---- a path's conditions, one way of meeting them ------------------------------------------

bool holdsFor(std::int64_t value, const Req& req)
{
    if (req.op == "true") return value != 0;
    if (req.op == "false") return value == 0;
    if (req.op == "eq") return value == req.value;
    if (req.op == "ne") return value != req.value;
    if (req.op == "gt") return value > req.value;
    if (req.op == "ge") return value >= req.value;
    if (req.op == "lt") return value < req.value;
    if (req.op == "le") return value <= req.value;
    return true;
}

// False when these conditions on stored state cannot all hold. Engine answers (`call`, `unknown`)
// may differ from one asking to the next, and strings are not compared: both count as possible.
bool jointlyPossible(const ReqSet& reqs, std::string* clash = nullptr)
{
    std::map<std::string, std::vector<const Req*>> byVariable;
    for (const Req& req : reqs) {
        if (!req.opaque() && req.text.empty()) {
            byVariable[req.kind + "|" + req.owner + "|" + req.name].push_back(&req);
        }
    }
    for (const auto& [variable, group] : byVariable) {
        std::set<std::int64_t> candidates{0, 1, -1};
        for (const Req* req : group) {
            candidates.insert({req->value - 1, req->value, req->value + 1});
        }
        const bool any = std::any_of(candidates.begin(), candidates.end(), [&](std::int64_t value) {
            return std::all_of(group.begin(), group.end(), [&](const Req* req) { return holdsFor(value, *req); });
        });
        if (!any) {
            if (clash != nullptr) {
                *clash = variable;
            }
            return false;
        }
    }
    return true;
}

// Every way a condition can have this truth. `!(a && b)` holds by !a or by !b, and keeping only the
// first would invent a precondition: a later `a` then looks impossible though !b was the way through.
std::vector<ReqSet> alternativesOf(const Sym* sym, bool truth)
{
    constexpr std::size_t kMostArms = 8;
    if (sym == nullptr) {
        return {ReqSet{}};
    }
    if (sym->kind == SymKind::Not) {
        return alternativesOf(sym->a, !truth);
    }
    if (sym->kind == SymKind::And || sym->kind == SymKind::Or) {
        const bool both = (sym->kind == SymKind::And) == truth;
        const std::vector<ReqSet> left = alternativesOf(sym->a, truth);
        const std::vector<ReqSet> right = alternativesOf(sym->b, truth);
        std::vector<ReqSet> out;
        if (both) {
            for (const ReqSet& one : left) {
                for (const ReqSet& other : right) {
                    if (out.size() >= kMostArms) {
                        return out;
                    }
                    ReqSet joined = one;
                    joined.insert(joined.end(), other.begin(), other.end());
                    out.push_back(std::move(joined));
                }
            }
            return out;
        }
        out = left;
        for (const ReqSet& other : right) {
            if (out.size() < kMostArms) {
                out.push_back(other);
            }
        }
        return out;
    }
    ReqSet single;
    requirementsOf(sym, truth, single);
    return {std::move(single)};
}

// ---- the symbolic walk -----------------------------------------------------------------

struct Guard {
    const Sym* condition = nullptr;
    bool truth = false;
};

struct State {
    std::size_t ip = 0;
    std::vector<const Sym*> stack;
    std::int64_t bp = 0;
    std::vector<std::size_t> calls;
    std::vector<Guard> guards;
    bool deferred = false;
    bool wrote = false; // A plot write on this path: earlier guards may no longer hold.
    // Globals this path wrote: a later read sees the value, not a new precondition.
    std::vector<std::pair<std::string, const Sym*>> written;
};

constexpr std::size_t kGuardCap = 64;
constexpr int kExprDepthCap = 8;
constexpr int kVisitsPerState = 3;

class Walker {
public:
    Walker(const std::vector<Instruction>& code, std::uint64_t budget)
        : code_(code)
        , budget_(budget)
    {
        byOffset_.reserve(code.size() * 2);
        for (std::size_t i = 0; i < code.size(); ++i) {
            byOffset_.emplace(code[i].offset, i);
        }
        for (const Instruction& ins : code) {
            if (ins.op == Op::JMP || ins.op == Op::JZ || ins.op == Op::JNZ || ins.op == Op::JSR) {
                const auto to = byOffset_.find(static_cast<std::uint32_t>(static_cast<std::int32_t>(ins.offset) + ins.operand0));
                if (to != byOffset_.end()) {
                    jumpTargets_.insert(to->second);
                }
            }
        }
        unknown_ = intern_.make(Sym{});
        Sym zero;
        zero.kind = SymKind::Int;
        zero_ = intern_.make(zero);
        Sym one = zero;
        one.number = 1;
        one_ = intern_.make(one);
    }


    ScriptFacts run()
    {
        State start;
        worklist_.push_back(std::move(start));
        while (!worklist_.empty() && steps_ < budget_) {
            State state = std::move(worklist_.back());
            worklist_.pop_back();
            walk(std::move(state));
        }
        if (!worklist_.empty() || steps_ >= budget_) {
            facts_.truncated = true; facts_.cutLine = __LINE__;
        }
        facts_.steps = steps_;
        for (const auto& [key, read] : reads_) {
            facts_.reads.push_back(read);
        }
        findValidGuards();
        return std::move(facts_);
    }

private:
    // From the code alone, so it holds where the walk stopped early: no branch, call or saved state
    // from outside the stretch lands inside it.
    void findValidGuards()
    {
        for (std::size_t i = 1; i < code_.size(); ++i) {
            if (code_[i].op != Op::JZ || code_[i - 1].op != Op::ACTION
                || std::string_view(script::actionSpec(code_[i - 1].actionId).name) != "GetIsObjectValid") {
                continue;
            }
            const std::int32_t end = code_[i].branchTarget();
            if (end <= static_cast<std::int32_t>(code_[i].offset)) {
                continue;
            }
            const std::uint32_t last = static_cast<std::uint32_t>(end);
            const bool sealed = std::none_of(code_.begin(), code_.end(), [&](const Instruction& ins) {
                const bool branches = ins.op == Op::JMP || ins.op == Op::JSR || ins.op == Op::JZ || ins.op == Op::JNZ || ins.op == Op::STORESTATE;
                const std::int32_t target = branches ? ins.branchTarget() : -1;
                const bool inside = ins.offset > code_[i].offset && ins.offset < last;
                return !inside && target > static_cast<std::int32_t>(code_[i].offset) && target < end;
            });
            if (sealed) {
                facts_.validGuards.emplace_back(code_[i].offset, last);
            }
        }
    }

    const Sym* intSym(std::int64_t value)
    {
        const auto found = ints_.find(value);
        if (found != ints_.end()) {
            return found->second;
        }
        Sym sym;
        sym.kind = SymKind::Int;
        sym.number = value;
        const Sym* out = intern_.make(std::move(sym));
        ints_.emplace(value, out);
        return out;
    }
    const Sym* strSym(std::string value)
    {
        Sym sym;
        sym.kind = SymKind::Str;
        sym.text = std::move(value);
        return intern_.make(std::move(sym));
    }
    const Sym* objSym(const std::string& name)
    {
        const auto found = objects_.find(name);
        if (found != objects_.end()) {
            return found->second;
        }
        Sym sym;
        sym.kind = SymKind::Obj;
        sym.text = name;
        const Sym* out = intern_.make(std::move(sym));
        objects_.emplace(name, out);
        return out;
    }
    const Sym* constString(std::size_t ip, std::string value)
    {
        const auto found = strings_.find(ip);
        if (found != strings_.end()) {
            return found->second;
        }
        const Sym* out = strSym(std::move(value));
        strings_.emplace(ip, out);
        return out;
    }
    const Sym* callSym(std::string name)
    {
        Sym sym;
        sym.kind = SymKind::Call;
        sym.text = std::move(name);
        return intern_.make(std::move(sym));
    }
    const Sym* readSym(ReadKind kind, std::string name, std::string owner = {})
    {
        Sym sym;
        sym.kind = SymKind::Read;
        sym.number = static_cast<std::int64_t>(kind);
        sym.text = std::move(name);
        sym.owner = std::move(owner);
        const Sym* out = intern_.make(std::move(sym));
        Read read;
        switch (kind) {
        case ReadKind::GlobalBool:
        case ReadKind::GlobalNum:
        case ReadKind::GlobalStr: read.kind = "global"; break;
        case ReadKind::Journal: read.kind = "journal"; break;
        case ReadKind::LocalBool:
        case ReadKind::LocalNum: read.kind = "local"; break;
        case ReadKind::Item: read.kind = "item"; break;
        case ReadKind::Exists: read.kind = "exists"; break;
        }
        read.name = out->text;
        read.owner = out->owner;
        reads_.emplace(read.kind + "|" + read.name + "|" + read.owner, read);
        return out;
    }
    const Sym* binary(SymKind kind, std::int64_t number, const Sym* a, const Sym* b)
    {
        Sym sym;
        sym.kind = kind;
        sym.number = number;
        sym.a = a;
        sym.b = b;
        const int depth = 1 + std::max(a != nullptr ? a->depth : 0, b != nullptr ? b->depth : 0);
        return depth > kExprDepthCap ? unknown_ : intern_.make(std::move(sym));
    }
    const Sym* notOf(const Sym* a)
    {
        if (a->kind == SymKind::Int) {
            return a->number != 0 ? zero_ : one_;
        }
        if (a->kind == SymKind::Not) {
            return a->a;
        }
        return binary(SymKind::Not, 0, a, nullptr);
    }
    // A condition over plot state only reads the same twice on one path. An engine
    // answer (a call, an unknown) may not.
    static bool stableCondition(const Sym* sym)
    {
        switch (sym->kind) {
        case SymKind::Read:
            return true;
        case SymKind::Cmp:
            return sym->a->kind == SymKind::Read && (sym->b->kind == SymKind::Int || sym->b->kind == SymKind::Str);
        case SymKind::Not:
            return stableCondition(sym->a);
        case SymKind::And:
        case SymKind::Or:
            return stableCondition(sym->a) && stableCondition(sym->b);
        default:
            return false;
        }
    }
    static bool isValueLike(const Sym* sym)
    {
        return sym->kind == SymKind::Read || sym->kind == SymKind::Call || sym->kind == SymKind::Obj;
    }
    const Sym* compare(CmpOp op, const Sym* a, const Sym* b)
    {
        if (a->kind == SymKind::Int && b->kind == SymKind::Int) {
            bool value = false;
            switch (op) {
            case CmpOp::Eq: value = a->number == b->number; break;
            case CmpOp::Ne: value = a->number != b->number; break;
            case CmpOp::Gt: value = a->number > b->number; break;
            case CmpOp::Ge: value = a->number >= b->number; break;
            case CmpOp::Lt: value = a->number < b->number; break;
            case CmpOp::Le: value = a->number <= b->number; break;
            }
            return value ? one_ : zero_;
        }
        if (a->kind == SymKind::Str && b->kind == SymKind::Str && (op == CmpOp::Eq || op == CmpOp::Ne)) {
            const bool equal = lowered(a->text) == lowered(b->text);
            return (op == CmpOp::Eq) == equal ? one_ : zero_;
        }
        if (a->kind == SymKind::Obj && b->kind == SymKind::Obj && a == b
            && (op == CmpOp::Eq || op == CmpOp::Ne)) {
            return op == CmpOp::Eq ? one_ : zero_;
        }
        // x == OBJECT_INVALID and x != OBJECT_INVALID are validity tests.
        if ((op == CmpOp::Eq || op == CmpOp::Ne) && b->kind == SymKind::Obj && b->text == "invalid") {
            const Sym* valid = validity(a);
            return op == CmpOp::Ne ? valid : notOf(valid);
        }
        if ((op == CmpOp::Eq || op == CmpOp::Ne) && a->kind == SymKind::Obj && a->text == "invalid") {
            const Sym* valid = validity(b);
            return op == CmpOp::Ne ? valid : notOf(valid);
        }
        if (!isValueLike(a) && isValueLike(b)) {
            return compare(cmpSwap(op), b, a);
        }
        if (isValueLike(a) && (b->kind == SymKind::Int || b->kind == SymKind::Str || isValueLike(b))) {
            return binary(SymKind::Cmp, static_cast<std::int64_t>(op), a, b);
        }
        return unknown_;
    }
    const Sym* validity(const Sym* object)
    {
        if (object->kind == SymKind::Read) {
            return object; // A possessed item: valid means held.
        }
        if (object->kind == SymKind::Obj) {
            if (object->text == "invalid") {
                return zero_;
            }
            if (object->text == "self" || object->text == "pc" || object->text == "module"
                || object->text == "area") {
                return one_;
            }
            if (object->text.rfind("tag:", 0) == 0) {
                return readSym(ReadKind::Exists, object->text.substr(4));
            }
        }
        return callSym("GetIsObjectValid");
    }
    static std::string ownerName(const Sym* object)
    {
        if (object == nullptr) {
            return "self";
        }
        if (object->kind == SymKind::Obj) {
            return object->text;
        }
        if (object->kind == SymKind::Call) {
            return "call:" + object->text;
        }
        return "unknown";
    }
    const Sym* cell(std::vector<const Sym*>& stack, std::int64_t index) const
    {
        if (index < 0 || index >= static_cast<std::int64_t>(stack.size())) {
            return nullptr;
        }
        return stack[static_cast<std::size_t>(index)];
    }

    static std::uint64_t mix(std::uint64_t seed, std::uint64_t value)
    {
        return seed ^ (value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2));
    }

    // True when this state has been seen often enough. The same stack may be reached
    // under a few different guard sets so a site keeps alternative preconditions.
    bool saturated(const State& state)
    {
        std::uint64_t key = state.ip;
        for (const Sym* sym : state.stack) {
            key = mix(key, reinterpret_cast<std::uintptr_t>(sym));
        }
        key = mix(key, static_cast<std::uint64_t>(state.bp));
        for (const std::size_t call : state.calls) {
            key = mix(key, call);
        }
        std::uint64_t guardKey = 0;
        for (const Guard& guard : state.guards) {
            guardKey = mix(guardKey, reinterpret_cast<std::uintptr_t>(guard.condition) + (guard.truth ? 1u : 0u));
        }
        auto& seen = visits_[key];
        if (std::find(seen.begin(), seen.end(), guardKey) != seen.end()) {
            return true;
        }
        if (seen.size() >= static_cast<std::size_t>(kVisitsPerState)) {
            facts_.pathsDropped = true;
            return true;
        }
        seen.push_back(guardKey);
        return false;
    }

    // A comparison an earlier guard already settles: after `x == 5`, `x == 7` is false. A
    // path that ignored this would ask for two values at once and no run could take it.
    static std::optional<bool> impliedByGuards(const State& state, const Sym* condition)
    {
        if (condition->kind != SymKind::Cmp || condition->a->kind != SymKind::Read || condition->b->kind != SymKind::Int) {
            return std::nullopt;
        }
        const CmpOp op = static_cast<CmpOp>(condition->number);
        const std::int64_t wanted = condition->b->number;
        for (const Guard& guard : state.guards) {
            const Sym* known = guard.condition;
            if (known->kind != SymKind::Cmp || known->a != condition->a || known->b->kind != SymKind::Int) {
                continue;
            }
            const CmpOp held = guard.truth ? static_cast<CmpOp>(known->number) : cmpInvert(static_cast<CmpOp>(known->number));
            const std::int64_t value = known->b->number;
            if (held == CmpOp::Eq) {
                switch (op) {
                case CmpOp::Eq: return value == wanted;
                case CmpOp::Ne: return value != wanted;
                case CmpOp::Gt: return value > wanted;
                case CmpOp::Ge: return value >= wanted;
                case CmpOp::Lt: return value < wanted;
                case CmpOp::Le: return value <= wanted;
                }
            }
            if (held == CmpOp::Ne && value == wanted && (op == CmpOp::Eq || op == CmpOp::Ne)) {
                return op == CmpOp::Ne;
            }
        }
        return std::nullopt;
    }

    static ReqSet distinct(const ReqSet& raw)
    {
        ReqSet out;
        std::set<std::string> seen;
        for (const Req& req : raw) {
            if (seen.insert(req.key()).second) {
                out.push_back(req);
            }
        }
        return out;
    }

    // Up to `most` ways of meeting every guard of this path that do not contradict themselves.
    // None found: the first arms, which the harness then reports as an infeasible gate. `capped`
    // says there may be more ways than were returned.
    std::vector<ReqSet> requirementSets(const State& state, std::size_t most, bool& capped) const
    {
        std::vector<std::vector<ReqSet>> arms;
        for (const Guard& guard : state.guards) {
            arms.push_back(alternativesOf(guard.condition, guard.truth));
        }
        std::vector<ReqSet> found;
        int visits = 0;
        constexpr int kMostVisits = 4000;
        ReqSet chosen;
        const std::function<void(std::size_t)> choose = [&](std::size_t index) {
            if (found.size() >= most || visits >= kMostVisits) {
                capped = true;
                return;
            }
            if (index == arms.size()) {
                found.push_back(distinct(chosen));
                return;
            }
            for (const ReqSet& arm : arms[index]) {
                ++visits;
                const std::size_t mark = chosen.size();
                chosen.insert(chosen.end(), arm.begin(), arm.end());
                if (jointlyPossible(chosen)) {
                    choose(index + 1);
                }
                chosen.resize(mark);
            }
        };
        choose(0);
        if (found.empty()) {
            ReqSet raw;
            for (const Guard& guard : state.guards) {
                requirementsOf(guard.condition, guard.truth, raw);
            }
            found.push_back(distinct(raw));
        }
        return found;
    }

    ReqSet requirements(const State& state) const
    {
        bool capped = false;
        return requirementSets(state, 1, capped).front();
    }

    void record(const State& state, const Instruction& ins, Effect effect)
    {
        const std::string key = std::to_string(ins.offset) + "|" + effect.key();
        const auto found = siteIndex_.find(key);
        Site* site = nullptr;
        if (found == siteIndex_.end()) {
            Site fresh;
            fresh.offset = ins.offset;
            fresh.action = script::actionSpec(ins.actionId).name;
            fresh.effect = std::move(effect);
            fresh.deferred = state.deferred;
            facts_.sites.push_back(std::move(fresh));
            siteIndex_.emplace(key, facts_.sites.size() - 1);
            site = &facts_.sites.back();
        } else {
            site = &facts_.sites[found->second];
        }
        if (site->paths.size() >= 6) {
            site->pathsCapped = true;
            return;
        }
        bool capped = false;
        for (ReqSet& reqs : requirementSets(state, 3, capped)) {
            const auto same = [&](const ReqSet& other) {
                if (other.size() != reqs.size()) {
                    return false;
                }
                for (std::size_t i = 0; i < reqs.size(); ++i) {
                    if (other[i].key() != reqs[i].key()) {
                        return false;
                    }
                }
                return true;
            };
            if (std::none_of(site->paths.begin(), site->paths.end(), same) && site->paths.size() < 6) {
                site->paths.push_back(std::move(reqs));
            }
        }
        site->pathsCapped = site->pathsCapped || capped;
    }

    static std::string textOf(const Sym* sym)
    {
        return sym != nullptr && sym->kind == SymKind::Str ? sym->text : std::string{};
    }
    static void remember(State& state, const std::string& name, const Sym* value)
    {
        const std::string key = lowered(name);
        for (auto& [known, held] : state.written) {
            if (known == key) {
                held = value;
                return;
            }
        }
        state.written.emplace_back(key, value);
    }
    static const Sym* writtenOr(const State& state, const std::string& name, const Sym* read)
    {
        const std::string key = lowered(name);
        for (const auto& [known, held] : state.written) {
            if (known == key) {
                return held;
            }
        }
        return read;
    }
    static bool isStr(const Sym* sym) { return sym != nullptr && sym->kind == SymKind::Str; }
    static bool isInt(const Sym* sym) { return sym != nullptr && sym->kind == SymKind::Int; }

    // Pops the arguments, records the effect and pushes the result. False on underflow.
    bool doAction(State& state, const Instruction& ins)
    {
        const script::ActionSpec& spec = script::actionSpec(ins.actionId);
        std::vector<const Sym*> args(ins.actionArgc, nullptr);
        for (std::uint8_t index = 0; index < ins.actionArgc; ++index) {
            const script::ParamKind kind = index < spec.paramCount ? spec.params[index] : script::ParamKind::Int;
            if (kind == script::ParamKind::Action) {
                continue;
            }
            const int width = std::max(0, script::parameterCells(kind));
            if (state.stack.size() < static_cast<std::size_t>(width)) {
                return false;
            }
            if (width >= 1) {
                args[index] = state.stack[state.stack.size() - static_cast<std::size_t>(width)];
            }
            state.stack.resize(state.stack.size() - static_cast<std::size_t>(width));
        }
        const auto arg = [&](std::size_t index) -> const Sym* {
            return index < args.size() ? args[index] : nullptr;
        };
        const Sym* result = nullptr;
        Effect effect;
        if (std::string_view(spec.name) == "GetTag" && ownerName(arg(0)) != "self") {
            facts_.tagOfOther = true;
        }
        switch (actionClasses()[ins.actionId]) {
        case Act::AddJournal:
            effect.kind = "journal_set";
            effect.name = textOf(arg(0));
            effect.nameKnown = isStr(arg(0));
            effect.valueKnown = isInt(arg(1));
            effect.value = effect.valueKnown ? arg(1)->number : 0;
            effect.flag = isInt(arg(2)) && arg(2)->number != 0;
            break;
        case Act::RemoveJournal:
            effect.kind = "journal_remove";
            effect.name = textOf(arg(0));
            effect.nameKnown = isStr(arg(0));
            effect.valueKnown = true;
            break;
        case Act::GetJournal:
            result = isStr(arg(0)) ? readSym(ReadKind::Journal, arg(0)->text) : callSym("GetJournalEntry");
            break;
        case Act::SetGlobalBool:
        case Act::SetGlobalNum:
            effect.kind = "global_set";
            effect.name = textOf(arg(0));
            effect.nameKnown = isStr(arg(0));
            effect.valueKnown = isInt(arg(1));
            effect.value = effect.valueKnown ? arg(1)->number : 0;
            // A boolean stores truth: one shipped script passes 50.
            if (actionClasses()[ins.actionId] == Act::SetGlobalBool && effect.value != 0) {
                effect.value = 1;
            }
            if (effect.nameKnown && arg(1) != nullptr) {
                remember(state, effect.name, arg(1));
            }
            break;
        case Act::SetGlobalStr:
            effect.kind = "global_set";
            effect.name = textOf(arg(0));
            effect.nameKnown = isStr(arg(0));
            effect.aux = isStr(arg(1)) ? "str:" + arg(1)->text : "str";
            effect.valueKnown = isStr(arg(1));
            break;
        case Act::GetGlobalBool:
            result = isStr(arg(0)) ? writtenOr(state, arg(0)->text, readSym(ReadKind::GlobalBool, arg(0)->text)) : callSym("GetGlobalBoolean");
            break;
        case Act::GetGlobalNum:
            result = isStr(arg(0)) ? writtenOr(state, arg(0)->text, readSym(ReadKind::GlobalNum, arg(0)->text)) : callSym("GetGlobalNumber");
            break;
        case Act::GetGlobalStr:
            result = isStr(arg(0)) ? readSym(ReadKind::GlobalStr, arg(0)->text) : callSym("GetGlobalString");
            break;
        case Act::SetLocalBool:
        case Act::SetLocalNum:
            effect.kind = "local_set";
            effect.name = isInt(arg(1)) ? std::to_string(arg(1)->number) : std::string{};
            effect.nameKnown = isInt(arg(1));
            effect.aux = ownerName(arg(0));
            effect.valueKnown = isInt(arg(2));
            effect.value = effect.valueKnown ? arg(2)->number : 0;
            if (effect.nameKnown && arg(2) != nullptr) {
                remember(state, "local|" + effect.aux + "|" + effect.name, arg(2));
            }
            break;
        case Act::GetLocalBool:
        case Act::GetLocalNum:
            if (isInt(arg(1))) {
                const std::string slot = std::to_string(arg(1)->number);
                result = writtenOr(state, "local|" + ownerName(arg(0)) + "|" + slot,
                                   readSym(actionClasses()[ins.actionId] == Act::GetLocalBool ? ReadKind::LocalBool
                                                                                               : ReadKind::LocalNum,
                                           slot, ownerName(arg(0))));
            } else {
                result = callSym(spec.name);
            }
            break;
        case Act::CreateObject:
            effect.kind = "create_object";
            effect.name = textOf(arg(1));
            effect.nameKnown = isStr(arg(1));
            effect.value = isInt(arg(0)) ? arg(0)->number : 0;
            effect.valueKnown = isInt(arg(0));
            result = effect.nameKnown ? callSym("CreateObject:" + effect.name) : callSym("CreateObject");
            break;
        case Act::StartNewModule:
            effect.kind = "start_module";
            effect.name = textOf(arg(0));
            effect.nameKnown = isStr(arg(0));
            effect.aux = textOf(arg(1));
            effect.valueKnown = true;
            break;
        case Act::GiveXp:
            effect.kind = "xp";
            effect.name = ownerName(arg(0));
            effect.nameKnown = true;
            effect.valueKnown = isInt(arg(1));
            effect.value = effect.valueKnown ? arg(1)->number : 0;
            break;
        case Act::GivePlotXp:
            effect.kind = "plot_xp";
            effect.name = textOf(arg(0));
            effect.nameKnown = isStr(arg(0));
            effect.valueKnown = isInt(arg(1));
            effect.value = effect.valueKnown ? arg(1)->number : 0;
            break;
        case Act::CreateItem:
            effect.kind = "item_create";
            effect.name = textOf(arg(0));
            effect.nameKnown = isStr(arg(0));
            effect.aux = ownerName(arg(1));
            effect.valueKnown = !args.empty() && (args.size() < 3 || isInt(arg(2)));
            effect.value = isInt(arg(2)) ? arg(2)->number : 1;
            result = effect.nameKnown ? callSym("CreateItemOnObject:" + effect.name) : callSym("CreateItemOnObject");
            break;
        case Act::GiveItem:
        case Act::TakeItem:
            effect.kind = actionClasses()[ins.actionId] == Act::GiveItem ? "item_give" : "item_take";
            effect.name = arg(0) != nullptr && arg(0)->kind == SymKind::Read ? arg(0)->text : std::string{};
            effect.nameKnown = !effect.name.empty();
            effect.aux = ownerName(arg(1));
            effect.valueKnown = true;
            break;
        case Act::PossessedBy:
            if (isStr(arg(1))) {
                result = readSym(ReadKind::Item, arg(1)->text, ownerName(arg(0)));
            } else {
                result = callSym("GetItemPossessedBy");
            }
            break;
        case Act::IsValid:
            result = arg(0) != nullptr ? validity(arg(0)) : callSym("GetIsObjectValid");
            break;
        case Act::ByTag:
        case Act::NearestByTag:
            result = isStr(arg(0)) ? objSym("tag:" + arg(0)->text) : callSym(spec.name);
            if (isStr(arg(0)) && std::string_view(spec.name) == "GetWaypointByTag") {
                const std::string tag = lowered(arg(0)->text);
                if (std::find(facts_.waypointTags.begin(), facts_.waypointTags.end(), tag) == facts_.waypointTags.end()) {
                    facts_.waypointTags.push_back(tag);
                }
            }
            break;
        case Act::FirstPc:
            result = objSym("pc");
            break;
        case Act::Module:
            result = objSym("module");
            break;
        case Act::Area:
            result = objSym("area");
            break;
        case Act::ExecuteScript:
            effect.kind = "exec_script";
            effect.name = textOf(arg(0));
            effect.nameKnown = isStr(arg(0));
            effect.aux = ownerName(arg(1));
            effect.valueKnown = true;
            // The third argument, when constant, is what GetRunScriptVar answers (00535b70).
            effect.flag = isInt(arg(2));
            effect.value = effect.flag ? arg(2)->number : -1;
            // Retail stores whatever was popped, -1 included, with no compare (00535c13).
            effect.valueKnown = effect.flag || ins.actionArgc < 3;
            break;
        // A script named to a screen runs when the screen closes, on the PC (ScriptParty.cpp,
        // GameSession.cpp). It is a call all the same, later.
        case Act::PartyGui:
        case Act::Pazaak: {
            const Sym* name = arg(actionClasses()[ins.actionId] == Act::PartyGui ? 0 : 1);
            effect.kind = "exec_script";
            effect.name = textOf(name);
            effect.nameKnown = isStr(name);
            effect.aux = "pc";
            effect.valueKnown = true;
            effect.flag = false;
            effect.value = -1;
            break;
        }
        case Act::StartConversation:
            effect.kind = "start_conv";
            effect.name = textOf(arg(1));
            effect.nameKnown = isStr(arg(1));
            effect.aux = ownerName(arg(0));
            effect.valueKnown = true;
            break;
        case Act::EndGame:
            effect.kind = "end_game";
            effect.name = "end";
            effect.nameKnown = true;
            effect.valueKnown = isInt(arg(0));
            effect.value = effect.valueKnown ? arg(0)->number : 0;
            break;
        case Act::AddAvailable:
        case Act::AddParty:
            effect.kind = "party_add";
            effect.name = isInt(arg(0)) ? std::to_string(arg(0)->number) : std::string{};
            effect.nameKnown = isInt(arg(0));
            effect.aux = textOf(arg(1));
            effect.valueKnown = true;
            result = callSym(spec.name);
            break;
        case Act::SetLocked:
            effect.kind = "door_op";
            effect.name = arg(0) != nullptr && arg(0)->kind == SymKind::Obj ? arg(0)->text : std::string{};
            effect.nameKnown = effect.name.rfind("tag:", 0) == 0;
            effect.aux = "SetLocked";
            effect.valueKnown = isInt(arg(1));
            effect.value = effect.valueKnown ? arg(1)->number : 0;
            break;
        case Act::OpenDoor:
        case Act::UnlockDoor:
            effect.kind = "door_op";
            effect.name = arg(0) != nullptr && arg(0)->kind == SymKind::Obj ? arg(0)->text : std::string{};
            effect.nameKnown = effect.name.rfind("tag:", 0) == 0;
            effect.aux = spec.name;
            effect.valueKnown = true;
            break;
        case Act::PlanetAvailable:
        case Act::PlanetSelectable:
            effect.kind = "planet";
            effect.name = isInt(arg(0)) ? std::to_string(arg(0)->number) : std::string{};
            effect.nameKnown = isInt(arg(0));
            effect.aux = spec.name;
            effect.valueKnown = isInt(arg(1));
            effect.value = effect.valueKnown ? arg(1)->number : 0;
            break;
        case Act::ShowGalaxy:
        case Act::None:
            break;
        }
        if (!effect.kind.empty()) {
            if (effect.kind == "global_set" || effect.kind == "local_set" || effect.kind == "journal_set"
                || effect.kind == "journal_remove") {
                state.wrote = true;
            }
            record(state, ins, std::move(effect));
        }
        // The result cell, as Vm::doAction leaves it.
        switch (spec.returnKind) {
        case script::ReturnKind::Void:
            break;
        case script::ReturnKind::Vector:
            state.stack.insert(state.stack.end(), 3, unknown_);
            break;
        case script::ReturnKind::Float:
        case script::ReturnKind::Engine:
            state.stack.push_back(unknown_);
            break;
        default:
            // retail 00543840: ShipBuild pushes 1, always, so its debug branches never run.
            if (std::string_view(spec.name) == "ShipBuild") {
                result = one_;
            }
            if (result == nullptr) {
                const Sym*& cached = calls_[ins.actionId];
                if (cached == nullptr) {
                    cached = callSym(spec.name);
                }
                result = cached;
            }
            state.stack.push_back(result);
            break;
        }
        return true;
    }

    // A switch over one value leaves one "not this case" per case and fills the guard list. Once
    // the value is pinned those say nothing more, and what follows them must still be recorded.
    static void pushGuard(State& state, const Sym* condition, bool truth)
    {
        const bool pins = condition->kind == SymKind::Cmp && condition->b->kind == SymKind::Int
            && static_cast<CmpOp>(condition->number) == (truth ? CmpOp::Eq : CmpOp::Ne);
        if (pins) {
            std::erase_if(state.guards, [&](const Guard& guard) {
                return guard.condition->kind == SymKind::Cmp && guard.condition->a == condition->a
                    && guard.condition->b->kind == SymKind::Int;
            });
        }
        if (state.guards.size() < kGuardCap) {
            state.guards.push_back({condition, truth});
        }
    }

    // Runs one path until it ends. Undecided branches fork onto the worklist.
    void walk(State state)
    {
        while (steps_ < budget_) {
            ++steps_;
            if (state.ip >= code_.size()) {
                facts_.truncated = true; facts_.cutLine = __LINE__;
                return;
            }
            const Instruction& ins = code_[state.ip];
            std::size_t next = state.ip + 1;
            std::vector<const Sym*>& stack = state.stack;
            const std::int64_t size = static_cast<std::int64_t>(stack.size());
            if (!depthReported_ && SDL_getenv("KOTOR_QUEST_DIAG") != nullptr) {
                std::uint64_t where = state.ip;
                for (const std::size_t call : state.calls) {
                    where = where * 1000003U + call;
                }
                const auto [seen, fresh] = depthAt_.emplace(where, size);
                if (!fresh && seen->second != size) {
                    depthReported_ = true;
                    std::printf("quest graph diag: depth %lld then %lld at @%u of %zu instructions\n", static_cast<long long>(seen->second),
                                static_cast<long long>(size), ins.offset, code_.size());
                }
            }
            switch (ins.op) {
            case Op::NOP:
            case Op::STORESTATEALL:
            case Op::SCRIPTSIZE:
                break;
            case Op::STORESTATE: {
                // The closure body follows the JMP that skips it on the normal path.
                std::size_t entry = state.ip + 1;
                if (entry < code_.size() && code_[entry].op == Op::JMP) {
                    ++entry;
                }
                State body = state;
                body.ip = entry;
                body.calls.clear();
                body.deferred = true;
                if (!saturated(body)) {
                    worklist_.push_back(std::move(body));
                }
                break;
            }
            case Op::CPTOPSP:
            case Op::CPTOPBP: {
                const std::int64_t base = ins.op == Op::CPTOPSP ? size : state.bp;
                const std::int64_t index = base + ins.operand0 / 4;
                const std::int64_t count = ins.u0 / 4;
                if (index < 0 || index + count > size) {
                    facts_.truncated = true; facts_.cutLine = __LINE__;
                    return;
                }
                for (std::int64_t i = 0; i < count; ++i) {
                    stack.push_back(stack[static_cast<std::size_t>(index + i)]);
                }
                break;
            }
            case Op::CPDOWNSP:
            case Op::CPDOWNBP: {
                const std::int64_t base = ins.op == Op::CPDOWNSP ? size : state.bp;
                const std::int64_t index = base + ins.operand0 / 4;
                const std::int64_t count = ins.u0 / 4;
                if (size < count || index < 0 || index + count > size) {
                    facts_.truncated = true; facts_.cutLine = __LINE__;
                    return;
                }
                for (std::int64_t i = 0; i < count; ++i) {
                    stack[static_cast<std::size_t>(index + i)] =
                        stack[static_cast<std::size_t>(size - count + i)];
                }
                break;
            }
            case Op::RSADD:
                switch (ins.qualifier) {
                case 0x03: stack.push_back(zero_); break;
                case 0x05: stack.push_back(constString(state.ip, "")); break;
                case 0x06: stack.push_back(objSym("invalid")); break;
                default: stack.push_back(unknown_); break;
                }
                break;
            case Op::CONST:
                if (ins.qualifier == 0x03) {
                    stack.push_back(intSym(ins.operand0));
                } else if (ins.qualifier == 0x05) {
                    stack.push_back(constString(state.ip, std::string(ins.bytes.begin(), ins.bytes.end())));
                } else if (ins.qualifier == 0x06) {
                    stack.push_back(objSym(ins.operand0 == 0 ? "self" : "invalid"));
                } else {
                    stack.push_back(unknown_);
                }
                break;
            case Op::ACTION:
                if (!doAction(state, ins)) {
                    facts_.truncated = true; facts_.cutLine = __LINE__;
                    return;
                }
                break;
            case Op::SAVEBP:
                stack.push_back(intSym(state.bp));
                state.bp = size;
                break;
            case Op::RESTOREBP:
                if (stack.empty() || stack.back()->kind != SymKind::Int) {
                    facts_.truncated = true; facts_.cutLine = __LINE__;
                    return;
                }
                state.bp = stack.back()->number;
                stack.pop_back();
                break;
            case Op::MOVSP: {
                const std::int64_t count = ins.operand0 / 4;
                if (count < 0) {
                    if (size + count < 0) {
                        facts_.truncated = true; facts_.cutLine = __LINE__;
                        return;
                    }
                    stack.resize(static_cast<std::size_t>(size + count));
                } else {
                    stack.insert(stack.end(), static_cast<std::size_t>(count), zero_);
                }
                break;
            }
            case Op::INCSP:
            case Op::DECSP:
            case Op::INCBP:
            case Op::DECBP: {
                const bool bpBased = ins.op == Op::INCBP || ins.op == Op::DECBP;
                const std::int64_t index = (bpBased ? state.bp : size) + ins.operand0 / 4;
                if (index < 0 || index >= size) {
                    facts_.truncated = true; facts_.cutLine = __LINE__;
                    return;
                }
                const Sym* value = stack[static_cast<std::size_t>(index)];
                const std::int64_t delta = ins.op == Op::INCSP || ins.op == Op::INCBP ? 1 : -1;
                stack[static_cast<std::size_t>(index)] =
                    value->kind == SymKind::Int ? intSym(value->number + delta) : unknown_;
                break;
            }
            case Op::DESTRUCT: {
                const std::int64_t total = ins.u0 / 4;
                const std::int64_t offset = ins.u1 / 4;
                const std::int64_t keep = ins.u2 / 4;
                const std::int64_t begin = size - total;
                if (begin < 0 || offset + keep > total) {
                    facts_.truncated = true; facts_.cutLine = __LINE__;
                    return;
                }
                std::vector<const Sym*> retained(stack.begin() + begin + offset,
                                                 stack.begin() + begin + offset + keep);
                stack.resize(static_cast<std::size_t>(begin));
                stack.insert(stack.end(), retained.begin(), retained.end());
                break;
            }
            case Op::JMP: {
                const auto target = byOffset_.find(static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(ins.offset) + ins.operand0));
                if (target == byOffset_.end()) {
                    facts_.truncated = true; facts_.cutLine = __LINE__;
                    return;
                }
                next = target->second;
                break;
            }
            case Op::JSR: {
                const auto target = byOffset_.find(static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(ins.offset) + ins.operand0));
                if (target == byOffset_.end() || state.calls.size() >= 16) {
                    facts_.truncated = true; facts_.cutLine = __LINE__;
                    return;
                }
                state.calls.push_back(next);
                next = target->second;
                break;
            }
            case Op::RETN:
                if (!state.calls.empty()) {
                    next = state.calls.back();
                    state.calls.pop_back();
                    break;
                }
                finishPath(state);
                return;
            case Op::JZ:
            case Op::JNZ: {
                if (stack.empty()) {
                    facts_.truncated = true; facts_.cutLine = __LINE__;
                    return;
                }
                const Sym* condition = stack.back();
                stack.pop_back();
                const auto target = byOffset_.find(static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(ins.offset) + ins.operand0));
                if (target == byOffset_.end()) {
                    facts_.truncated = true; facts_.cutLine = __LINE__;
                    return;
                }
                const std::size_t ifTrue = ins.op == Op::JNZ ? target->second : next;
                const std::size_t ifFalse = ins.op == Op::JNZ ? next : target->second;
                std::optional<bool> decided;
                // The compiler's "or": a test of the same cell as the jump just fallen through, with
                // nothing between that could change it. Its other side would join at another depth.
                const auto copiesTop = [&](std::size_t at) {
                    return code_[at].op == Op::CPTOPSP && code_[at].operand0 == -4 && code_[at].u0 == 4;
                };
                if (state.ip >= 3 && copiesTop(state.ip - 1) && copiesTop(state.ip - 3)
                    && !jumpTargets_.contains(state.ip - 1) && !jumpTargets_.contains(state.ip - 2)
                    && !jumpTargets_.contains(state.ip)
                    && (code_[state.ip - 2].op == Op::JZ || code_[state.ip - 2].op == Op::JNZ)) {
                    decided = code_[state.ip - 2].op == Op::JZ;
                } else if (condition->kind == SymKind::Int) {
                    decided = condition->number != 0;
                } else if (condition->kind == SymKind::Str) {
                    decided = !condition->text.empty();
                } else if (!state.wrote && stableCondition(condition)) {
                    for (const Guard& guard : state.guards) {
                        if (guard.condition == condition) {
                            decided = guard.truth;
                            break;
                        }
                    }
                }
                if (!decided.has_value()) {
                    decided = impliedByGuards(state, condition);
                }
                if (decided.has_value()) {
                    next = *decided ? ifTrue : ifFalse;
                    break;
                }
                // Fork. The true side continues here and the false side waits.
                State other = state;
                other.ip = ifFalse;
                pushGuard(other, condition, false);
                if (!saturated(other)) {
                    worklist_.push_back(std::move(other));
                }
                pushGuard(state, condition, true);
                state.ip = ifTrue;
                if (saturated(state)) {
                    return;
                }
                continue;
            }
            case Op::LOGAND:
            case Op::LOGOR: {
                if (size < 2) {
                    facts_.truncated = true; facts_.cutLine = __LINE__;
                    return;
                }
                const Sym* b = stack.back();
                stack.pop_back();
                const Sym* a = stack.back();
                stack.pop_back();
                const bool isAnd = ins.op == Op::LOGAND;
                const auto constant = [](const Sym* sym) { return sym->kind == SymKind::Int; };
                if (constant(a) && constant(b)) {
                    const bool value = isAnd ? (a->number != 0 && b->number != 0) : (a->number != 0 || b->number != 0);
                    stack.push_back(value ? one_ : zero_);
                } else if (constant(a)) {
                    stack.push_back(isAnd ? (a->number != 0 ? b : zero_) : (a->number != 0 ? one_ : b));
                } else if (constant(b)) {
                    stack.push_back(isAnd ? (b->number != 0 ? a : zero_) : (b->number != 0 ? one_ : a));
                } else if (a == b) {
                    stack.push_back(a);
                } else {
                    stack.push_back(binary(isAnd ? SymKind::And : SymKind::Or, 0, a, b));
                }
                break;
            }
            case Op::EQUAL:
            case Op::NEQUAL:
            case Op::GEQ:
            case Op::GT:
            case Op::LT:
            case Op::LEQ: {
                std::int64_t width = 1;
                if (ins.qualifier == 0x24) {
                    width = std::max<std::int64_t>(1, ins.u0 / 4);
                }
                if (size < width * 2) {
                    facts_.truncated = true; facts_.cutLine = __LINE__;
                    return;
                }
                if (width != 1) {
                    stack.resize(static_cast<std::size_t>(size - width * 2));
                    stack.push_back(unknown_);
                    break;
                }
                const Sym* b = stack.back();
                stack.pop_back();
                const Sym* a = stack.back();
                stack.pop_back();
                CmpOp op = CmpOp::Eq;
                switch (ins.op) {
                case Op::NEQUAL: op = CmpOp::Ne; break;
                case Op::GEQ: op = CmpOp::Ge; break;
                case Op::GT: op = CmpOp::Gt; break;
                case Op::LT: op = CmpOp::Lt; break;
                case Op::LEQ: op = CmpOp::Le; break;
                default: break;
                }
                stack.push_back(compare(op, a, b));
                break;
            }
            case Op::NOT: {
                if (stack.empty()) {
                    facts_.truncated = true; facts_.cutLine = __LINE__;
                    return;
                }
                stack.back() = notOf(stack.back());
                break;
            }
            case Op::NEG:
            case Op::COMP: {
                if (stack.empty()) {
                    facts_.truncated = true; facts_.cutLine = __LINE__;
                    return;
                }
                const Sym* value = stack.back();
                stack.back() = value->kind == SymKind::Int
                    ? intSym(ins.op == Op::NEG ? -value->number : ~value->number)
                    : unknown_;
                break;
            }
            case Op::ADD:
            case Op::SUB:
            case Op::MUL:
            case Op::DIV:
            case Op::MOD:
            case Op::BOOLAND:
            case Op::INCOR:
            case Op::EXCOR:
            case Op::SHLEFT:
            case Op::SHRIGHT:
            case Op::USHRIGHT: {
                // Vector arithmetic and vector-by-float use other cell counts.
                std::int64_t popA = 1;
                std::int64_t popB = 1;
                std::int64_t push = 1;
                if (ins.qualifier == 0x3A) { // vector op vector
                    popA = popB = push = 3;
                } else if (ins.qualifier == 0x3B) { // vector op float
                    popA = 3;
                    push = 3;
                } else if (ins.qualifier == 0x3C) { // float op vector
                    popB = 3;
                    push = 3;
                }
                if (size < popA + popB) {
                    facts_.truncated = true; facts_.cutLine = __LINE__;
                    return;
                }
                const Sym* b = stack.back();
                const Sym* a = stack[static_cast<std::size_t>(size - popB - 1)];
                stack.resize(static_cast<std::size_t>(size - popA - popB));
                if (popA == 1 && popB == 1 && a->kind == SymKind::Int && b->kind == SymKind::Int) {
                    std::int64_t value = 0;
                    const std::int64_t x = a->number;
                    const std::int64_t y = b->number;
                    switch (ins.op) {
                    case Op::ADD: value = x + y; break;
                    case Op::SUB: value = x - y; break;
                    case Op::MUL: value = x * y; break;
                    case Op::DIV: value = y == 0 ? 0 : x / y; break;
                    case Op::MOD: value = y == 0 ? 0 : x % y; break;
                    case Op::BOOLAND: value = x & y; break;
                    case Op::INCOR: value = x | y; break;
                    case Op::EXCOR: value = x ^ y; break;
                    case Op::SHLEFT: value = x << (y & 31); break;
                    default: value = x >> (y & 31); break;
                    }
                    stack.push_back(intSym(value));
                } else if (ins.op == Op::ADD && a->kind == SymKind::Str && b->kind == SymKind::Str) {
                    stack.push_back(strSym(a->text + b->text));
                } else {
                    stack.insert(stack.end(), static_cast<std::size_t>(push), unknown_);
                }
                break;
            }
            }
            state.ip = next;
        }
        facts_.truncated = true; facts_.cutLine = __LINE__;
    }

    void finishPath(const State& state)
    {
        if (state.deferred || state.stack.empty()) {
            return;
        }
        facts_.isConditional = true;
        const Sym* value = state.stack.front();
        std::vector<ReqSet>& trueList = facts_.truePaths;
        std::vector<ReqSet>& falseList = facts_.falsePaths;
        const auto add = [&](std::vector<ReqSet>& list, ReqSet reqs) {
            if (list.size() >= 4) {
                facts_.truePathsCapped = true;
                return;
            }
            const auto same = [&](const ReqSet& other) {
                if (other.size() != reqs.size()) {
                    return false;
                }
                for (std::size_t i = 0; i < reqs.size(); ++i) {
                    if (other[i].key() != reqs[i].key()) {
                        return false;
                    }
                }
                return true;
            };
            if (std::none_of(list.begin(), list.end(), same)) {
                list.push_back(std::move(reqs));
            }
        };
        ReqSet base = requirements(state);
        if (value->kind == SymKind::Int) {
            add(value->number != 0 ? trueList : falseList, std::move(base));
            return;
        }
        ReqSet yes = base;
        requirementsOf(value, true, yes);
        add(trueList, std::move(yes));
        requirementsOf(value, false, base);
        add(falseList, std::move(base));
    }

    const std::vector<Instruction>& code_;
    std::unordered_set<std::size_t> jumpTargets_;
    std::unordered_map<std::uint64_t, std::int64_t> depthAt_;
    bool depthReported_ = false;
    std::unordered_map<std::uint32_t, std::size_t> byOffset_;
    Interner intern_;
    const Sym* unknown_ = nullptr;
    const Sym* zero_ = nullptr;
    const Sym* one_ = nullptr;
    ScriptFacts facts_;
    std::vector<State> worklist_;
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> visits_;
    std::map<std::string, std::size_t> siteIndex_;
    std::map<std::string, Read> reads_;
    std::uint64_t steps_ = 0;
    std::uint64_t budget_ = 0;
    std::unordered_map<std::int64_t, const Sym*> ints_;
    std::unordered_map<std::size_t, const Sym*> strings_;
    std::unordered_map<std::string, const Sym*> objects_;
    std::array<const Sym*, 772> calls_{};
};

// ---- resources -------------------------------------------------------------------------

std::string fieldText(const kotor::Gff& gff, std::uint32_t structure, const char* label)
{
    const kotor::GffField* field = gff.field(structure, label);
    return field != nullptr ? gff.text(*field) : std::string{};
}

std::int32_t fieldInt(const kotor::Gff& gff, std::uint32_t structure, const char* label, std::int32_t fallback = 0)
{
    const kotor::GffField* field = gff.field(structure, label);
    return field != nullptr ? gff.integer(*field) : fallback;
}

float fieldFloat(const kotor::Gff& gff, std::uint32_t structure, const char* label, float fallback = 0.0f)
{
    const kotor::GffField* field = gff.field(structure, label);
    return field != nullptr ? gff.number(*field) : fallback;
}

// DLG node fields, without any text.
template <typename Node, typename Links>
DlgNode nodeInfo(const Node& node, const Links& targets, const std::vector<std::string>& actives)
{
    DlgNode out;
    out.script = lowered(node.script);
    out.quest = node.quest;
    out.questEntry = node.questEntry;
    out.hasQuestEntry = node.hasQuestEntry;
    out.plotIndex = node.plotIndex;
    out.plotXp = node.plotXpPercentage;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        out.links.emplace_back(targets[i], i < actives.size() ? lowered(actives[i]) : std::string{});
    }
    return out;
}

DlgInfo dlgInfo(std::vector<std::uint8_t> bytes, const std::string& resref, const std::string& module)
{
    const kotor::Gff gff = kotor::Gff::parse(std::move(bytes), resref + ".dlg");
    const kotor::Dlg dlg = kotor::Dlg::parse(gff, nullptr, resref);
    DlgInfo out;
    out.resref = resref;
    out.module = module;
    for (const kotor::DlgEntry& entry : dlg.entries) {
        out.entries.push_back(nodeInfo(entry, entry.replies, entry.repliesActive));
    }
    for (const kotor::DlgReply& reply : dlg.replies) {
        out.replies.push_back(nodeInfo(reply, reply.entries, reply.entriesActive));
    }
    for (std::size_t i = 0; i < dlg.starting.size(); ++i) {
        out.starting.emplace_back(dlg.starting[i],
                                  i < dlg.startingActive.size() ? lowered(dlg.startingActive[i]) : std::string{});
    }
    out.endConversation = lowered(dlg.endConversation);
    out.endAbort = lowered(dlg.endConversationAbort);
    return out;
}

const char* templateKind(std::uint32_t type)
{
    switch (type) {
    case kUtc: return "creature";
    case kUtd: return "door";
    case kUtp: return "placeable";
    case kUtt: return "trigger";
    case kUte: return "encounter";
    case kUtm: return "store";
    case kUtw: return "waypoint";
    default: return "";
    }
}

void readTemplate(const kotor::Gff& gff, std::uint32_t type, ObjectInfo& object)
{
    object.kind = templateKind(type);
    const std::uint32_t root = gff.rootIndex();
    if (const std::string tag = fieldText(gff, root, "Tag"); !tag.empty()) {
        object.tag = tag;
    }
    if (const std::string conversation = fieldText(gff, root, "Conversation"); !conversation.empty()) {
        object.conversation = lowered(conversation);
    }
    kotor::collectScriptHooks(gff, root, object.hooks);
}

struct Walk {
    kotor::Install& install;
    Graph& graph;
    std::unordered_map<std::uint64_t, std::size_t> factsByHash;
    std::set<std::string> rawNames; // Names the game can reach without a script: see markUnreachableHosts.

    void addRawNames(const std::vector<std::uint8_t>& bytes)
    {
        std::string word;
        const auto flush = [&]() {
            if (word.size() >= 3 && word.size() <= 32) {
                rawNames.insert(word);
            }
            word.clear();
        };
        for (const std::uint8_t byte : bytes) {
            if (std::isalnum(byte) != 0 || byte == '_') {
                word.push_back(static_cast<char>(std::tolower(byte)));
            } else {
                flush();
            }
        }
        flush();
    }

    std::size_t factsFor(const std::vector<std::uint8_t>& bytes)
    {
        const std::uint64_t hash = std::hash<std::string_view>{}(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        const auto found = factsByHash.find(hash);
        if (found != factsByHash.end()) {
            return found->second;
        }
        graph.facts.push_back(analyzeScript(bytes));
        ++graph.scriptCount;
        if (graph.facts.back().truncated) {
            ++graph.scriptsTruncated;
        }
        if (graph.facts.back().decodeFailed) {
            ++graph.scriptsFailed;
        }
        const std::size_t index = graph.facts.size() - 1;
        factsByHash.emplace(hash, index);
        return index;
    }

    std::optional<std::vector<std::uint8_t>> globalRead(const std::string& resref, std::uint32_t type)
    {
        return install.find(resref, type);
    }

    void readGit(const kotor::Gff& git, ModuleInfo& module,
                 const std::map<std::string, ObjectInfo>& templates)
    {
        const struct {
            const char* label;
            std::uint32_t type;
        } lists[] = {{"Creature List", kUtc}, {"Door List", kUtd},   {"Placeable List", kUtp},
                     {"TriggerList", kUtt},   {"Encounter List", kUte}, {"StoreList", kUtm},
                     {"WaypointList", kUtw}};
        for (const auto& list : lists) {
            const kotor::GffField* field = git.field(git.rootIndex(), list.label);
            if (field == nullptr) {
                continue;
            }
            for (const std::uint32_t item : git.list(*field)) {
                ObjectInfo object;
                object.kind = templateKind(list.type);
                object.placed = true;
                object.resref = lowered(fieldText(git, item, "TemplateResRef"));
                if (object.resref.empty()) {
                    object.resref = lowered(fieldText(git, item, "ResRef"));
                }
                const std::string key = std::to_string(list.type) + "|" + object.resref;
                if (const auto found = templates.find(key); found != templates.end()) {
                    object = found->second;
                    object.placed = true;
                } else if (auto data = globalRead(object.resref, list.type); data.has_value()) {
                    try {
                        const kotor::Gff gff = kotor::Gff::parse(std::move(*data), object.resref);
                        readTemplate(gff, list.type, object);
                    } catch (const kotor::FormatError&) {
                    }
                }
                if (const std::string tag = fieldText(git, item, "Tag"); !tag.empty()) {
                    object.tag = tag;
                }
                std::map<std::string, std::string> overrides;
                kotor::collectScriptHooks(git, item, overrides);
                for (auto& [hook, script] : overrides) {
                    object.hooks[hook] = script;
                }
                object.linkedModule = lowered(fieldText(git, item, "LinkedToModule"));
                object.linkedTo = fieldText(git, item, "LinkedTo");
                object.position = {fieldFloat(git, item, "XPosition", fieldFloat(git, item, "X")),
                                   fieldFloat(git, item, "YPosition", fieldFloat(git, item, "Y")),
                                   fieldFloat(git, item, "ZPosition", fieldFloat(git, item, "Z"))};
                module.objects.push_back(std::move(object));
            }
        }
    }

    void readModule(const std::string& stem)
    {
        ModuleInfo module;
        module.name = stem;
        module.planet = planetOf(stem);
        const std::filesystem::path root = install.root() / "modules";
        std::map<std::string, ObjectInfo> templates; // "type|resref"
        std::vector<kotor::Gff> gits;
        const auto scan = [&](const std::filesystem::path& path) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error)) {
                return;
            }
            const kotor::Archive archive = kotor::Archive::open(path);
            for (const kotor::ArchiveEntry& entry : archive.entries()) {
                const std::string resref = lowered(entry.resref);
                switch (entry.type) {
                case 2025: // Item template.
                    graph.itemTemplateHomes[resref].push_back(stem);
                    break;
                case kNcs:
                    module.scripts[resref] = factsFor(archive.read(entry));
                    break;
                case kDlg:
                    try {
                        graph.dialogues[stem + "|" + resref] = dlgInfo(archive.read(entry), resref, stem);
                        module.dialogues.push_back(resref);
                    } catch (const kotor::FormatError&) {
                    }
                    break;
                case kAre:
                case kIfo:
                    addRawNames(archive.read(entry));
                    try {
                        const kotor::Gff gff = kotor::Gff::parse(archive.read(entry), resref);
                        kotor::collectScriptHooks(gff, gff.rootIndex(), module.sceneHooks);
                    } catch (const kotor::FormatError&) {
                    }
                    break;
                case kGit:
                    try {
                        gits.push_back(kotor::Gff::parse(archive.read(entry), resref));
                    } catch (const kotor::FormatError&) {
                    }
                    break;
                case kUtc:
                case kUtd:
                case kUtp:
                case kUtt:
                case kUte:
                case kUtm:
                case kUtw:
                    try {
                        ObjectInfo object;
                        object.resref = resref;
                        const kotor::Gff gff = kotor::Gff::parse(archive.read(entry), resref);
                        if (entry.type == kUte) {
                            if (const kotor::GffField* list = gff.field(gff.rootIndex(), "CreatureList"); list != nullptr) {
                                for (const std::uint32_t item : gff.list(*list)) {
                                    rawNames.insert(lowered(fieldText(gff, item, "ResRef")));
                                }
                            }
                        }
                        readTemplate(gff, entry.type, object);
                        templates[std::to_string(entry.type) + "|" + resref] = std::move(object);
                    } catch (const kotor::FormatError&) {
                    }
                    break;
                default:
                    break;
                }
            }
        };
        scan(root / (stem + ".rim"));
        scan(root / (stem + "_s.rim"));
        for (const kotor::Gff& git : gits) {
            readGit(git, module, templates);
        }
        // Templates not placed in the GIT can still be spawned by script.
        std::set<std::string> placed;
        for (const ObjectInfo& object : module.objects) {
            placed.insert(object.kind + "|" + object.resref);
        }
        for (auto& [key, object] : templates) {
            if (!placed.contains(object.kind + "|" + object.resref)) {
                module.objects.push_back(std::move(object));
            }
        }
        graph.modules.push_back(std::move(module));
    }

    void readJournal()
    {
        auto data = install.find("global", kJrl);
        if (!data.has_value()) {
            return;
        }
        const kotor::Gff gff = kotor::Gff::parse(std::move(*data), "global.jrl");
        const kotor::GffField* categories = gff.field(gff.rootIndex(), "Categories");
        if (categories == nullptr) {
            return;
        }
        for (const std::uint32_t structure : gff.list(*categories)) {
            JournalQuest quest;
            quest.tag = fieldText(gff, structure, "Tag");
            quest.plotIndex = fieldInt(gff, structure, "PlotIndex", -1);
            quest.xp = fieldInt(gff, structure, "XP", 0);
            if (const kotor::GffField* list = gff.field(structure, "EntryList"); list != nullptr) {
                for (const std::uint32_t item : gff.list(*list)) {
                    JournalEntry entry;
                    entry.id = fieldInt(gff, item, "ID", 0);
                    entry.end = fieldInt(gff, item, "End", 0) != 0;
                    entry.xpPercentage = fieldFloat(gff, item, "XP_Percentage", 0.0f);
                    quest.entries.push_back(entry);
                }
            }
            graph.quests.push_back(std::move(quest));
        }
    }
};

// ---- edges -----------------------------------------------------------------------------

bool isTracked(const std::string& kind)
{
    return kind == "journal_set" || kind == "journal_remove" || kind == "global_set" || kind == "xp"
        || kind == "plot_xp" || kind == "item_create" || kind == "start_module" || kind == "end_game";
}

bool isDebugScript(const std::string& resref)
{
    return resref.rfind("qa_", 0) == 0 || resref.rfind("k_qa", 0) == 0 || resref.rfind("test", 0) == 0
        || resref.rfind("k_test", 0) == 0;
}

// ---- a condition against its own node ----------------------------------------------------

// A node's script runs straight after a link's condition passed. If every way into the node
// wants a stored value that every path of the script to this call rules out, and no condition
// in that conversation writes it between the two, the call never runs. Only made when each walk
// saw every path: nothing dropped, nothing capped.
// GetGlobalBoolean compared with a value it can never answer (00529030 gives 0 for a wrong kind, and a
// boolean holds 0 or 1): k_pebn_korrib and three like it test K_CAPTURED_LEV == 5 this way.
void markBooleanVsValue(Graph& graph)
{
    for (Edge& edge : graph.edges) {
        if (!edge.dead.empty() || edge.gates.empty()) {
            continue;
        }
        std::string wants;
        const auto refused = [&](const ReqSet& reqs) {
            for (const Req& req : reqs) {
                if (req.boolRead && req.op == "eq" && req.value != 0 && req.value != 1) {
                    wants = req.name;
                    return true;
                }
            }
            return false;
        };
        if (std::all_of(edge.gates.begin(), edge.gates.end(), refused)) {
            edge.dead = "boolean_read_vs_value:" + wants;
        }
    }
}

// An edge behind GetIsObjectValid(GetWaypointByTag(x)) in a module that places no waypoint x, where no
// constant CreateObject makes a waypoint: the lookup finds nothing, so that branch cannot run there
// (the messenger scripts and K_MESSENGER_SPAWN, placed in seven modules).
void markAbsentWaypoints(Graph& graph)
{
    std::set<std::string> created;
    for (const quest::ScriptFacts& facts : graph.facts) {
        for (const Site& site : facts.sites) {
            if (site.effect.kind == "create_object" && site.effect.nameKnown) {
                created.insert(lowered(site.effect.name));
            }
        }
    }
    std::map<std::string, const ModuleInfo*> modules;
    for (const ModuleInfo& module : graph.modules) {
        modules[module.name] = &module;
    }
    for (Edge& edge : graph.edges) {
        const auto home = modules.find(edge.module);
        if (!edge.dead.empty() || edge.gates.empty() || home == modules.end()) {
            continue;
        }
        const ModuleInfo& module = *home->second;
        const quest::ScriptFacts* facts = nullptr;
        if (const auto it = module.scripts.find(edge.siteScript); it != module.scripts.end()) {
            facts = &graph.facts[it->second];
        } else if (const auto global = graph.globalScripts.find(edge.siteScript); global != graph.globalScripts.end()) {
            facts = &graph.facts[global->second];
        }
        if (facts == nullptr || facts->waypointTags.empty()) {
            continue;
        }
        const auto site = std::find_if(facts->sites.begin(), facts->sites.end(),
                                       [&](const Site& candidate) { return candidate.offset == edge.siteOffset; });
        if (site == facts->sites.end()) {
            continue;
        }
        // Paths the walk gave up on could go round the guard, unless the code itself says none can.
        if (facts->truncated || facts->pathsDropped || site->pathsCapped) {
            const bool sealedIn = std::any_of(facts->validGuards.begin(), facts->validGuards.end(), [&](const auto& guard) {
                return edge.siteOffset > guard.first && edge.siteOffset < guard.second;
            });
            if (!sealedIn) {
                continue;
            }
        }
        const auto placed = [&](const std::string& tag) {
            return std::any_of(module.objects.begin(), module.objects.end(), [&](const ObjectInfo& object) {
                return object.placed && lowered(object.tag) == tag;
            });
        };
        std::string wants;
        const auto refused = [&](const ReqSet& reqs) {
            for (const Req& req : reqs) {
                const std::string tag = lowered(req.name);
                if (req.kind == "exists" && (req.op == "true" || (req.op == "eq" && req.value != 0)) && !placed(tag) && !created.contains(tag)
                    && std::find(facts->waypointTags.begin(), facts->waypointTags.end(), tag) != facts->waypointTags.end()) {
                    wants = tag;
                    return true;
                }
            }
            return false;
        };
        if (std::all_of(edge.gates.begin(), edge.gates.end(), refused)) {
            edge.dead = "waypoint_not_placed:" + wants;
        }
    }
}

// A shared script that branches on its own tag (k_act_makeitem, one branch per companion), run from a
// conversation only creatures of one tag ever hold: the branches for other tags cannot run there.
void markOwnerTagBranches(Graph& graph)
{
    std::map<std::string, std::set<std::string>> tagsOf; // conversation -> every holder's tag.
    const auto note = [&](const ObjectInfo& object) {
        if (!object.conversation.empty()) {
            tagsOf[object.conversation].insert(lowered(object.tag));
        }
    };
    for (const ModuleInfo& module : graph.modules) {
        std::for_each(module.objects.begin(), module.objects.end(), note);
    }
    std::for_each(graph.globalObjects.begin(), graph.globalObjects.end(), note);
    std::map<std::string, const ModuleInfo*> modules;
    for (const ModuleInfo& module : graph.modules) {
        modules[module.name] = &module;
    }
    for (Edge& edge : graph.edges) {
        const auto holders = tagsOf.find(edge.host.dlg);
        if (!edge.dead.empty() || edge.host.dlg.empty() || holders == tagsOf.end() || holders->second.size() != 1
            || edge.siteScript != lowered(edge.host.script) || edge.gates.empty()) {
            continue;
        }
        const quest::ScriptFacts* facts = nullptr;
        const ModuleInfo& module = *modules.at(edge.module);
        if (const auto it = module.scripts.find(edge.siteScript); it != module.scripts.end()) {
            facts = &graph.facts[it->second];
        } else if (const auto global = graph.globalScripts.find(edge.siteScript); global != graph.globalScripts.end()) {
            facts = &graph.facts[global->second];
        }
        if (facts == nullptr || facts->tagOfOther || facts->truncated || facts->pathsDropped) {
            continue;
        }
        const auto site = std::find_if(facts->sites.begin(), facts->sites.end(),
                                       [&](const Site& candidate) { return candidate.offset == edge.siteOffset; });
        if (site == facts->sites.end() || site->pathsCapped) {
            continue;
        }
        const std::string& tag = *holders->second.begin();
        std::string wants;
        // A link's condition may ask the tag of something else (k_ptar_sithdis, the armour worn). With
        // such a script in the conversation only what the site's own script asks counts.
        bool linkAsksOther = false;
        const auto noteLink = [&](const std::string& active) {
            if (active.empty()) {
                return;
            }
            const std::string name = lowered(active);
            if (const auto it = module.scripts.find(name); it != module.scripts.end()) {
                linkAsksOther = linkAsksOther || graph.facts[it->second].tagOfOther;
            } else if (const auto global = graph.globalScripts.find(name); global != graph.globalScripts.end()) {
                linkAsksOther = linkAsksOther || graph.facts[global->second].tagOfOther;
            }
        };
        const auto dlgFound = [&]() {
            const auto own = graph.dialogues.find(edge.module + "|" + edge.host.dlg);
            return own != graph.dialogues.end() ? own : graph.dialogues.find("|" + edge.host.dlg);
        }();
        if (dlgFound != graph.dialogues.end()) {
            for (const auto& [target, active] : dlgFound->second.starting) {
                noteLink(active);
            }
            for (const DlgNode& node : dlgFound->second.entries) {
                for (const auto& [target, active] : node.links) {
                    noteLink(active);
                }
            }
            for (const DlgNode& node : dlgFound->second.replies) {
                for (const auto& [target, active] : node.links) {
                    noteLink(active);
                }
            }
        }
        const auto askedHere = [&](const Req& req) {
            return std::any_of(site->paths.begin(), site->paths.end(), [&](const ReqSet& path) {
                return std::any_of(path.begin(), path.end(), [&](const Req& own) { return own.key() == req.key(); });
            });
        };
        const auto refuses = [&](const ReqSet& reqs) {
            for (const Req& req : reqs) {
                if (req.kind == "call" && req.name == "GetTag" && !req.text.empty() && (!linkAsksOther || askedHere(req))) {
                    const bool same = lowered(req.text) == tag;
                    if ((req.op == "eq" && !same) || (req.op == "ne" && same)) {
                        wants = req.text;
                        return true;
                    }
                }
            }
            return false;
        };
        if (std::all_of(edge.gates.begin(), edge.gates.end(), refuses)) {
            edge.dead = "owner_tag_differs:" + wants;
        }
    }
}

void markLinkContradictions(Graph& graph)
{
    std::map<std::string, const ModuleInfo*> modules;
    for (const ModuleInfo& module : graph.modules) {
        modules[module.name] = &module;
    }
    const auto factsIn = [&](const ModuleInfo& module, const std::string& raw) -> const ScriptFacts* {
        const std::string name = lowered(raw);
        if (const auto it = module.scripts.find(name); it != module.scripts.end()) {
            return &graph.facts[it->second];
        }
        if (const auto it = graph.globalScripts.find(name); it != graph.globalScripts.end()) {
            return &graph.facts[it->second];
        }
        return nullptr;
    };
    for (Edge& edge : graph.edges) {
        if (!edge.dead.empty() || edge.host.dlg.empty() || edge.siteScript.empty()
            || edge.siteScript != lowered(edge.host.script)
            || (edge.host.kind != "dlg_entry" && edge.host.kind != "dlg_reply")) {
            continue;
        }
        const ModuleInfo& module = *modules.at(edge.module);
        const auto dlgIt = [&]() {
            const auto own = graph.dialogues.find(edge.module + "|" + edge.host.dlg);
            return own != graph.dialogues.end() ? own : graph.dialogues.find("|" + edge.host.dlg);
        }();
        const ScriptFacts* script = factsIn(module, edge.siteScript);
        if (dlgIt == graph.dialogues.end() || script == nullptr || script->truncated || script->pathsDropped) {
            continue;
        }
        const auto site = std::find_if(script->sites.begin(), script->sites.end(),
                                       [&](const Site& one) { return one.offset == edge.siteOffset; });
        if (site == script->sites.end() || site->pathsCapped || site->paths.empty()) {
            continue;
        }
        const DlgInfo& dlg = dlgIt->second;
        const bool isReply = edge.host.kind == "dlg_reply";
        // Every way in, and every condition of the conversation.
        std::vector<std::string> incoming;
        std::set<std::string> allConditions;
        bool open = false;
        const auto scan = [&](std::uint32_t target, const std::string& active, bool toReply) {
            if (!active.empty()) {
                allConditions.insert(active);
            }
            if (toReply == isReply && target == edge.host.node) {
                if (active.empty()) {
                    open = true;
                } else {
                    incoming.push_back(active);
                }
            }
        };
        for (const auto& [target, active] : dlg.starting) {
            scan(target, active, false);
        }
        for (const DlgNode& node : dlg.entries) {
            for (const auto& [target, active] : node.links) {
                scan(target, active, true);
            }
        }
        for (const DlgNode& node : dlg.replies) {
            for (const auto& [target, active] : node.links) {
                scan(target, active, false);
            }
        }
        if (open || incoming.empty()) {
            continue;
        }
        std::string clash;
        bool proven = true;
        for (const std::string& active : incoming) {
            const ScriptFacts* condition = factsIn(module, active);
            if (condition == nullptr) {
                continue; // Never passes (0059ec90).
            }
            if (condition->truncated || condition->pathsDropped || condition->truePathsCapped || condition->truePaths.empty()) {
                proven = false;
                break;
            }
            for (const ReqSet& way : condition->truePaths) {
                for (const ReqSet& path : site->paths) {
                    ReqSet joined = way;
                    joined.insert(joined.end(), path.begin(), path.end());
                    std::string one;
                    if (jointlyPossible(joined, &one) || one.rfind("global|", 0) != 0) {
                        proven = false;
                        break;
                    }
                    clash = one;
                }
            }
        }
        if (!proven || clash.empty()) {
            continue;
        }
        // Nothing between the two writes it.
        const std::string name = clash.substr(clash.rfind('|') + 1);
        for (const std::string& active : allConditions) {
            const ScriptFacts* condition = factsIn(module, active);
            if (condition != nullptr) {
                for (const Site& one : condition->sites) {
                    if (one.effect.kind == "global_set" && lowered(one.effect.name) == lowered(name)) {
                        proven = false;
                    }
                }
            }
        }
        if (proven) {
            edge.dead = "link_vs_script:" + name;
        }
    }
}

// ---- what can ever exist ----------------------------------------------------------------

// Whole-install reachability of objects, conversations and scripts. Starts from what the game
// itself runs (module and area hooks, tables, the executable's own strings, GIT placements) and
// follows names: a live script makes live whatever it names in a constant string; a live object
// makes its conversation and hooks live; a live conversation makes its scripts live. A name that
// could be built at run time from a fragment in a live script stays live. What is left no shipped
// path can start. The rule errs towards live: a constant used as a tag lookup is the only name
// use not followed, because finding an object creates nothing.
void markUnreachableHosts(Graph& graph, const std::set<std::string>& rawNames)
{
    std::set<std::string> liveNames = rawNames;
    std::vector<std::string> work(rawNames.begin(), rawNames.end());
    const auto mark = [&](const std::string& raw) {
        if (raw.empty()) {
            return;
        }
        std::string name = lowered(raw);
        if (liveNames.insert(name).second) {
            work.push_back(std::move(name));
        }
    };
    // Every copy of a name, per kind. A name is one namespace here on purpose: it errs live.
    std::map<std::string, std::vector<const ScriptFacts*>> scriptsByName;
    std::map<std::string, std::vector<const DlgInfo*>> dialoguesByName;
    std::map<std::string, std::vector<const ObjectInfo*>> objectsByName;
    for (const ModuleInfo& module : graph.modules) {
        for (const auto& [name, index] : module.scripts) {
            scriptsByName[name].push_back(&graph.facts[index]);
        }
        for (const auto& [hook, script] : module.sceneHooks) {
            mark(script);
        }
        for (const ObjectInfo& object : module.objects) {
            objectsByName[lowered(object.resref)].push_back(&object);
            if (object.placed) {
                mark(object.resref);
            }
        }
    }
    for (const auto& [name, index] : graph.globalScripts) {
        scriptsByName[name].push_back(&graph.facts[index]);
    }
    for (const ObjectInfo& object : graph.globalObjects) {
        objectsByName[lowered(object.resref)].push_back(&object);
    }
    for (const auto& [key, dlg] : graph.dialogues) {
        dialoguesByName[lowered(dlg.resref)].push_back(&dlg);
    }
    std::vector<std::string> fragments;
    std::set<const ScriptFacts*> doneFacts;
    while (!work.empty()) {
        const std::string name = std::move(work.back());
        work.pop_back();
        if (const auto found = scriptsByName.find(name); found != scriptsByName.end()) {
            for (const ScriptFacts* facts : found->second) {
                if (!doneFacts.insert(facts).second) {
                    continue;
                }
                for (const std::string& text : facts->names) {
                    mark(text);
                }
                fragments.insert(fragments.end(), facts->fragments.begin(), facts->fragments.end());
            }
        }
        if (const auto found = objectsByName.find(name); found != objectsByName.end()) {
            for (const ObjectInfo* object : found->second) {
                mark(object->conversation);
                for (const auto& [hook, script] : object->hooks) {
                    mark(script);
                }
            }
        }
        if (const auto found = dialoguesByName.find(name); found != dialoguesByName.end()) {
            for (const DlgInfo* dlg : found->second) {
                const auto node = [&](const DlgNode& one) {
                    mark(one.script);
                    for (const auto& [target, active] : one.links) {
                        mark(active);
                    }
                };
                std::for_each(dlg->entries.begin(), dlg->entries.end(), node);
                std::for_each(dlg->replies.begin(), dlg->replies.end(), node);
                for (const auto& [target, active] : dlg->starting) {
                    mark(active);
                }
                mark(dlg->endConversation);
                mark(dlg->endAbort);
            }
        }
    }
    const auto isLive = [&](const std::string& raw) {
        const std::string name = lowered(raw);
        if (liveNames.contains(name)) {
            return true;
        }
        return std::any_of(fragments.begin(), fragments.end(),
                           [&](const std::string& fragment) { return name.rfind(fragment, 0) == 0; });
    };
    graph.unreachableNames = 0;
    for (Edge& edge : graph.edges) {
        const Host& host = edge.host;
        std::string blocked;
        std::string why;
        if (!host.dlg.empty()) {
            if (!isLive(host.dlg)) {
                blocked = host.dlg;
                why = "unreachable_owner_never_placed:";
            }
        } else if (host.kind == "hook") {
            if (!host.ownerResref.empty() && !isLive(host.ownerResref)) {
                blocked = host.ownerResref;
                why = "unreachable_owner_never_placed:";
            }
        } else if (host.kind == "script" && !host.script.empty() && !isLive(host.script)) {
            blocked = host.script;
            why = "unreachable_no_caller:";
        }
        if (!blocked.empty()) {
            edge.dead = why + blocked;
            ++graph.unreachableNames;
        }
    }
}

class EdgeBuilder {
public:
    explicit EdgeBuilder(Graph& graph)
        : graph_(graph)
    {
    }

    std::map<std::pair<const ScriptFacts*, std::int64_t>, RunVarReach> narrowedFacts_;

    const ScriptFacts* factsOf(const ModuleInfo& module, const std::string& resref) const
    {
        const std::string key = lowered(resref);
        if (const auto it = module.scripts.find(key); it != module.scripts.end()) {
            return &graph_.facts[it->second];
        }
        if (const auto it = graph_.globalScripts.find(key); it != graph_.globalScripts.end()) {
            return &graph_.facts[it->second];
        }
        return nullptr;
    }

    static bool reqHoldsFor(std::int64_t value, const Req& req)
    {
        if (req.op == "true") return value != 0;
        if (req.op == "false") return value == 0;
        if (req.op == "eq") return value == req.value;
        if (req.op == "ne") return value != req.value;
        if (req.op == "gt") return value > req.value;
        if (req.op == "ge") return value >= req.value;
        if (req.op == "lt") return value < req.value;
        if (req.op == "le") return value <= req.value;
        return true;
    }

    // Every tracked effect of `script`, following ExecuteScript with a constant name.
    // `selfAs` names the object ExecuteScript ran it on, since that is its OBJECT_SELF;
    // empty means the host's own self.
    void expand(const ModuleInfo& module, const Host& host, const std::string& script,
                const ReqSet& inherited, std::set<std::string>& visited, int depth,
                const std::vector<std::string>& pathActive, bool dlgLinked, bool orphan,
                const std::string& selfAs = {}, std::optional<std::int64_t> runVar = std::nullopt)
    {
        if (script.empty() || depth > 6 || !visited.insert(lowered(script)).second) {
            return;
        }
        // `visited` is the call stack, so only a cycle stops here: a script called from two sites of a
        // host keeps each caller's guards. The budget bounds what that can cost one host.
        struct Leave {
            std::set<std::string>& visited;
            std::string name;
            ~Leave() { visited.erase(name); }
        } leave{visited, lowered(script)};
        expansionsLeft_ = depth == 0 ? 256 : expansionsLeft_ - 1;
        if (expansionsLeft_ <= 0) {
            return;
        }
        referenced_.insert(module.name + "|" + lowered(script));
        const ScriptFacts* facts = factsOf(module, script);
        if (facts == nullptr) {
            return;
        }
        // A dispatch over the run variable: the caller's constant narrows it to one case.
        const RunVarReach* narrowed = nullptr;
        if (runVar.has_value() && facts->bytes != nullptr) {
            const auto key = std::make_pair(facts, *runVar);
            auto cached = narrowedFacts_.find(key);
            if (cached == narrowedFacts_.end()) {
                cached = narrowedFacts_.emplace(key, reachForRunVar(*facts->bytes, *runVar)).first;
                ++graph_.runVarWalks;
                graph_.runVarWalksCut += cached->second.complete ? 0 : 1;
                if (SDL_getenv("KOTOR_QUEST_DIAG") != nullptr) {
                    std::printf("quest graph: run variable walk %s %lld complete=%d reached=%zu %s\n", script.c_str(),
                                static_cast<long long>(*runVar), cached->second.complete ? 1 : 0,
                                cached->second.offsets.size(), cached->second.why.c_str());
                }
            }
            narrowed = cached->second.complete ? &cached->second : nullptr;
        }
        for (const Site& site : facts->sites) {
            // Walked whole with the caller's constant, and the call is never reached.
            const bool noRun = narrowed != nullptr && !narrowed->offsets.contains(site.offset);
            const std::vector<ReqSet>& paths = site.paths;
            // A path whose GetRunScriptVar test cannot hold for the caller's constant is no path.
            std::size_t contradicted = 0;
            const auto gatesFor = [&]() {
                std::vector<ReqSet> gates;
                contradicted = 0;
                for (const ReqSet& path : paths) {
                    ReqSet merged = inherited;
                    bool holds = true;
                    for (const Req& req : path) {
                        if (runVar.has_value() && req.kind == "call" && req.name == "GetRunScriptVar") {
                            holds = holds && reqHoldsFor(*runVar, req);
                            continue;
                        }
                        // OBJECT_SELF in a script ExecuteScript started is the target it named (00535b70).
                        merged.push_back(req);
                        if (!selfAs.empty()) {
                            Req& carried = merged.back();
                            if (carried.owner == "self") {
                                carried.owner = selfAs;
                            }
                            if (const std::size_t tilde = carried.name.rfind("~self");
                                tilde != std::string::npos && tilde + 5 == carried.name.size()) {
                                carried.name = carried.name.substr(0, tilde + 1) + selfAs;
                            }
                        }
                    }
                    if (holds) {
                        gates.push_back(std::move(merged));
                    } else {
                        ++contradicted;
                    }
                }
                if (gates.empty()) {
                    gates.push_back(inherited);
                }
                return gates;
            };
            if (site.effect.kind == "exec_script") {
                if (!site.effect.nameKnown) {
                    addUnresolved(module.name, script, site, "ExecuteScript with a dynamic name");
                    continue;
                }
                const std::vector<ReqSet> gates = gatesFor();
                if (noRun || (contradicted != 0 && contradicted == paths.size())) {
                    continue;
                }
                // The third argument is stored as popped, -1 included (00535c13). Without one the
                // variable stays; a value the walk could not read says nothing.
                const std::optional<std::int64_t> passed = site.effect.flag
                    ? std::optional<std::int64_t>(site.effect.value)
                    : (site.effect.valueKnown ? runVar : std::nullopt);
                for (std::size_t alternative = 0; alternative < gates.size() && alternative < 3; ++alternative) {
                    expand(module, host, site.effect.name, gates[alternative], visited, depth + 1, pathActive,
                           dlgLinked, orphan, site.effect.aux == "self" ? selfAs : site.effect.aux, passed);
                }
                continue;
            }
            if (site.effect.kind == "door_op" && site.effect.nameKnown) {
                const std::string door = lowered(site.effect.name.substr(4));
                std::string operation = site.effect.aux;
                if (operation == "SetLocked") {
                    if (!site.effect.valueKnown || site.effect.value != 0) {
                        continue;
                    }
                    operation += ":0";
                }
                addOpener(door, {module.name, lowered(script), operation});
                continue;
            }
            if (!isTracked(site.effect.kind)) {
                continue;
            }
            if (!site.effect.nameKnown) {
                addUnresolved(module.name, script, site, site.effect.kind + " with a dynamic name");
                continue;
            }
            if (!site.effect.valueKnown && (site.effect.kind == "journal_set")) {
                addUnresolved(module.name, script, site, "journal_set " + site.effect.name + " with a dynamic state");
            }
            Edge edge;
            edge.module = module.name;
            edge.planet = module.planet;
            edge.host = host;
            edge.siteScript = lowered(script);
            edge.siteOffset = site.offset;
            edge.effect = site.effect;
            if (!selfAs.empty() && edge.effect.aux == "self") {
                edge.effect.aux = selfAs;
            }
            edge.gates = gatesFor();
            if (noRun || (contradicted != 0 && contradicted == paths.size())) {
                edge.dead = "run_var:" + std::to_string(*runVar);
            }
            if (!dlgLinked && edge.dead.empty()) {
                edge.dead = "no_route_in_dialogue";
            }
            edge.pathActive = pathActive;
            edge.deferred = site.deferred;
            edge.dlgLinked = dlgLinked;
            edge.orphan = orphan;
            edge.debug = isDebugScript(edge.siteScript) || isDebugScript(host.script);
            add(std::move(edge));
        }
    }

    void addUnresolved(const std::string& module, const std::string& script, const Site& site, std::string detail)
    {
        const std::string key = module + "|" + lowered(script) + "|" + std::to_string(site.offset);
        if (unresolvedSeen_.insert(key).second) {
            graph_.unresolved.push_back({module, lowered(script), site.action, site.offset, std::move(detail)});
        }
    }

    void addOpener(const std::string& door, DoorOpener opener)
    {
        std::vector<DoorOpener>& list = graph_.doorOpeners[door];
        for (const DoorOpener& existing : list) {
            if (existing.module == opener.module && existing.script == opener.script
                && existing.operation == opener.operation) {
                return;
            }
        }
        list.push_back(std::move(opener));
    }

    void add(Edge edge)
    {
        edge.id = edge.module + "|" + edge.host.id() + "|" + edge.siteScript + "@"
            + std::to_string(edge.siteOffset) + "|" + edge.effect.key();
        const auto [known, fresh] = edgeIds_.emplace(edge.id, graph_.edges.size());
        if (fresh) {
            graph_.edges.push_back(std::move(edge));
            return;
        }
        // Another call path to the same effect: one more way to reach it.
        std::vector<ReqSet>& gates = graph_.edges[known->second].gates;
        for (ReqSet& reqs : edge.gates) {
            const auto same = [&](const ReqSet& other) {
                if (other.size() != reqs.size()) {
                    return false;
                }
                for (std::size_t i = 0; i < reqs.size(); ++i) {
                    if (other[i].key() != reqs[i].key()) {
                        return false;
                    }
                }
                return true;
            };
            if (gates.size() < 8 && std::none_of(gates.begin(), gates.end(), same)) {
                gates.push_back(std::move(reqs));
            }
        }
    }

    // The gates of a conversation node: the shortest route from a starting link.
    struct Route {
        bool linked = false;
        std::vector<std::string> actives;
        std::string immediate;
    };

    // A condition script that is not shipped: the run fails to load (005d2260), 005d45d0 returns 0
    // and 0059ec90 hands that back, so the link never passes.
    std::map<std::pair<int, std::uint32_t>, Route> routes(const ModuleInfo& module, const DlgInfo& dlg) const
    {
        const auto passable = [&](const std::string& active) {
            return active.empty() || factsOf(module, active) != nullptr;
        };
        std::map<std::pair<int, std::uint32_t>, Route> out; // (0 entry | 1 reply, index)
        std::queue<std::pair<int, std::uint32_t>> queue;
        for (const auto& [target, active] : dlg.starting) {
            const std::pair<int, std::uint32_t> key{0, target};
            if (target < dlg.entries.size() && !out.contains(key) && passable(active)) {
                Route route;
                route.linked = true;
                route.immediate = active;
                if (!active.empty()) {
                    route.actives.push_back(active);
                }
                out.emplace(key, std::move(route));
                queue.push(key);
            }
        }
        while (!queue.empty()) {
            const auto key = queue.front();
            queue.pop();
            const Route route = out.at(key);
            const DlgNode& node = key.first == 0 ? dlg.entries[key.second] : dlg.replies[key.second];
            const int nextKind = key.first == 0 ? 1 : 0;
            const std::size_t limit = nextKind == 0 ? dlg.entries.size() : dlg.replies.size();
            for (const auto& [target, active] : node.links) {
                const std::pair<int, std::uint32_t> nextKey{nextKind, target};
                if (target >= limit || out.contains(nextKey) || !passable(active)) {
                    continue;
                }
                Route next = route;
                next.immediate = active;
                if (!active.empty()) {
                    next.actives.push_back(active);
                }
                out.emplace(nextKey, std::move(next));
                queue.push(nextKey);
            }
        }
        return out;
    }

    void addDialogue(const ModuleInfo& module, const DlgInfo& dlg)
    {
        graph_.nodeCount += dlg.entries.size() + dlg.replies.size();
        const auto routed = routes(module, dlg);
        std::string ownerTag;
        std::string ownerResref;
        std::string ownerKind;
        for (const ObjectInfo& object : module.objects) {
            if (object.conversation == dlg.resref) {
                const bool better = ownerTag.empty() || object.placed;
                if (better) {
                    ownerTag = object.tag;
                    ownerResref = object.resref;
                    ownerKind = object.kind;
                }
                if (object.placed) {
                    break;
                }
            }
        }
        // A link's condition is a script like any other, and shipped ones write: "say this once"
        // sets its flag from the condition (0059ec90 runs it whenever the link is looked at).
        std::set<std::string> conditionsSeen;
        const auto visitCondition = [&](const std::string& active) {
            if (!conditionsSeen.insert(lowered(active)).second) {
                return;
            }
            Host host;
            host.kind = "dlg_cond";
            host.dlg = dlg.resref;
            host.script = active;
            host.ownerTag = ownerTag;
            host.ownerResref = ownerResref;
            host.ownerKind = ownerKind;
            std::set<std::string> visited;
            expand(module, host, active, {}, visited, 0, {}, true, false);
        };
        const auto visitNode = [&](const DlgNode& node, int kind, std::uint32_t index) {
            const auto route = routed.find({kind, index});
            const bool linked = route != routed.end();
            Host host;
            host.kind = kind == 0 ? "dlg_entry" : "dlg_reply";
            host.dlg = dlg.resref;
            host.node = index;
            host.script = node.script;
            host.ownerTag = ownerTag;
            host.ownerResref = ownerResref;
            host.ownerKind = ownerKind;
            std::vector<std::string> actives;
            if (linked) {
                host.linkActive = route->second.immediate;
                actives = route->second.actives;
            }
            ReqSet inherited;
            if (linked && !host.linkActive.empty()) {
                if (const ScriptFacts* facts = factsOf(module, host.linkActive);
                    facts != nullptr && !facts->truePaths.empty()) {
                    inherited = facts->truePaths.front();
                }
            }
            if (!node.quest.empty() && node.hasQuestEntry) {
                Edge edge;
                edge.module = module.name;
                edge.planet = module.planet;
                edge.host = host;
                edge.effect.kind = "journal_set";
                edge.effect.name = node.quest;
                edge.effect.nameKnown = true;
                edge.effect.value = node.questEntry;
                edge.effect.valueKnown = true;
                edge.gates = {inherited};
                edge.pathActive = actives;
                edge.dlgLinked = linked;
                if (!linked) {
                    edge.dead = "no_route_in_dialogue";
                }
                add(std::move(edge));
            }
            if (node.plotIndex >= 0) {
                Edge edge;
                edge.module = module.name;
                edge.planet = module.planet;
                edge.host = host;
                edge.effect.kind = "plot_xp";
                edge.effect.name = "plotindex:" + std::to_string(node.plotIndex);
                edge.effect.aux = std::to_string(node.plotXp);
                edge.effect.nameKnown = true;
                edge.effect.value = node.plotIndex;
                edge.effect.valueKnown = true;
                edge.gates = {inherited};
                edge.pathActive = actives;
                edge.dlgLinked = linked;
                if (!linked) {
                    edge.dead = "no_route_in_dialogue";
                }
                add(std::move(edge));
            }
            if (!node.script.empty()) {
                std::set<std::string> visited;
                expand(module, host, node.script, inherited, visited, 0, actives, linked, false);
            }
            for (const auto& [target, active] : node.links) {
                if (!active.empty()) {
                    referenced_.insert(module.name + "|" + active);
                    visitCondition(active);
                }
            }
        };
        for (std::uint32_t i = 0; i < dlg.entries.size(); ++i) {
            visitNode(dlg.entries[i], 0, i);
        }
        for (std::uint32_t i = 0; i < dlg.replies.size(); ++i) {
            visitNode(dlg.replies[i], 1, i);
        }
        for (const auto& [target, active] : dlg.starting) {
            if (!active.empty()) {
                referenced_.insert(module.name + "|" + active);
                visitCondition(active);
            }
        }
        for (const bool abort : {false, true}) {
            const std::string& script = abort ? dlg.endAbort : dlg.endConversation;
            if (script.empty()) {
                continue;
            }
            Host host;
            host.kind = abort ? "dlg_abort" : "dlg_end";
            host.dlg = dlg.resref;
            host.script = script;
            host.ownerTag = ownerTag;
            host.ownerResref = ownerResref;
            host.ownerKind = ownerKind;
            std::set<std::string> visited;
            expand(module, host, script, {}, visited, 0, {}, true, false);
        }
    }

    void build()
    {
        for (const ModuleInfo& module : graph_.modules) {
            for (const auto& [hook, script] : module.sceneHooks) {
                Host host;
                host.kind = "scene_hook";
                host.hook = hook;
                host.script = lowered(script);
                std::set<std::string> visited;
                expand(module, host, script, {}, visited, 0, {}, true, false);
            }
            // One edge per script, with the first object that uses it as the owner.
            std::set<std::string> hooked;
            for (const ObjectInfo& object : module.objects) {
                for (const auto& [hook, script] : object.hooks) {
                    const std::string resref = lowered(script);
                    // Placed owners come first in module.objects.
                    if (!hooked.insert(resref).second) {
                        continue;
                    }
                    Host host;
                    host.kind = "hook";
                    host.hook = hook;
                    host.script = resref;
                    host.ownerTag = object.tag;
                    host.ownerResref = object.resref;
                    host.ownerKind = object.kind;
                    std::set<std::string> visited;
                    expand(module, host, resref, {}, visited, 0, {}, true, false);
                }
            }
            std::set<std::string> dlgSeen;
            for (const std::string& resref : module.dialogues) {
                const auto found = graph_.dialogues.find(module.name + "|" + resref);
                if (found != graph_.dialogues.end() && dlgSeen.insert(resref).second) {
                    addDialogue(module, found->second);
                }
            }
        }
        // A global DLG belongs to the modules that name it.
        for (const ModuleInfo& module : graph_.modules) {
            std::set<std::string> named;
            for (const ObjectInfo& object : module.objects) {
                if (!object.conversation.empty()) {
                    named.insert(object.conversation);
                }
            }
            for (const auto& [resref, index] : module.scripts) {
                for (const Site& site : graph_.facts[index].sites) {
                    if (site.effect.kind == "start_conv" && site.effect.nameKnown && !site.effect.name.empty()) {
                        named.insert(lowered(site.effect.name));
                    }
                }
            }
            for (const std::string& resref : named) {
                if (std::find(module.dialogues.begin(), module.dialogues.end(), resref) != module.dialogues.end()) {
                    continue;
                }
                const auto found = graph_.dialogues.find("|" + resref);
                if (found != graph_.dialogues.end()) {
                    addDialogue(module, found->second);
                }
            }
        }
        // Whatever is left in a module's archive has no caller this walk could find.
        for (const ModuleInfo& module : graph_.modules) {
            for (const auto& [resref, index] : module.scripts) {
                if (referenced_.contains(module.name + "|" + resref)) {
                    continue;
                }
                Host host;
                host.kind = "script";
                host.script = resref;
                std::set<std::string> visited;
                expand(module, host, resref, {}, visited, 0, {}, true, true);
            }
        }
        // A shipped global script nothing in any module names (item and spell impact
        // scripts, for instance). Traced in the first module with no owner.
        std::set<std::string> anywhere;
        for (const std::string& key : referenced_) {
            anywhere.insert(key.substr(key.find('|') + 1));
        }
        ModuleInfo global;
        global.name = "global";
        global.planet = "global";
        for (const auto& [resref, index] : graph_.globalScripts) {
            if (anywhere.contains(resref)) {
                continue;
            }
            Host host;
            host.kind = "script";
            host.script = resref;
            std::set<std::string> visited;
            expand(global, host, resref, {}, visited, 0, {}, true, true);
        }
    }

private:
    Graph& graph_;
    std::set<std::string> referenced_;
    std::unordered_map<std::string, std::size_t> edgeIds_;
    int expansionsLeft_ = 0;
    std::set<std::string> unresolvedSeen_;
};

// ---- flags and the main plot -----------------------------------------------------------

void computeFlags(Graph& graph)
{
    std::map<std::string, const JournalQuest*> questByTag;
    for (const JournalQuest& quest : graph.quests) {
        questByTag[lowered(quest.tag)] = &quest;
    }
    std::set<std::string> setEntries;
    std::map<std::string, std::set<std::string>> globalSpellings;
    std::set<std::string> globalsSet;
    for (const Edge& edge : graph.edges) {
        if (edge.effect.kind == "journal_set" && edge.effect.valueKnown) {
            const std::string quest = lowered(edge.effect.name);
            const std::string key = quest + "=" + std::to_string(edge.effect.value);
            setEntries.insert(key);
            const auto found = questByTag.find(quest);
            if (found == questByTag.end()) {
                graph.questsUnknown.push_back(edge.effect.name);
            } else {
                const auto& entries = found->second->entries;
                const std::int64_t wanted = std::llabs(edge.effect.value);
                if (std::none_of(entries.begin(), entries.end(),
                                 [&](const JournalEntry& entry) { return entry.id == wanted; })
                    && wanted != 0) {
                    graph.entriesNotInJournal.push_back(key);
                }
            }
        }
        if (edge.effect.kind == "global_set") {
            globalSpellings[lowered(edge.effect.name)].insert(edge.effect.name);
            globalsSet.insert(lowered(edge.effect.name));
        }
    }
    for (const JournalQuest& quest : graph.quests) {
        for (const JournalEntry& entry : quest.entries) {
            const std::string key = lowered(quest.tag) + "=" + std::to_string(entry.id);
            if (!setEntries.contains(key)) {
                graph.entriesNoSetter.push_back(key);
            }
        }
    }
    // Every read anywhere, also in scripts no edge covers.
    std::map<std::string, std::string> firstReader;
    const auto scan = [&](const ScriptFacts& facts, const std::string& name) {
        for (const Read& read : facts.reads) {
            if (read.kind == "global") {
                globalSpellings[lowered(read.name)].insert(read.name);
                firstReader.emplace(lowered(read.name), name);
            }
        }
        const auto readers = [&](const ReqSet& reqs) {
            for (const Req& req : reqs) {
                if (req.kind == "journal" && !req.op.empty() && req.op != "true" && req.op != "false") {
                    std::vector<std::string>& list =
                        graph.journalReaders[lowered(req.name) + "=" + std::to_string(req.value)];
                    if (list.size() < 16 && std::find(list.begin(), list.end(), name) == list.end()) {
                        list.push_back(name);
                    }
                }
            }
        };
        for (const ReqSet& reqs : facts.truePaths) {
            readers(reqs);
        }
        for (const Site& site : facts.sites) {
            for (const ReqSet& reqs : site.paths) {
                readers(reqs);
            }
        }
    };
    for (const ModuleInfo& module : graph.modules) {
        for (const auto& [resref, index] : module.scripts) {
            scan(graph.facts[index], module.name + ":" + resref);
        }
    }
    for (const auto& [resref, index] : graph.globalScripts) {
        scan(graph.facts[index], resref);
    }
    for (const auto& [lower, spellings] : globalSpellings) {
        if (spellings.size() > 1) {
            graph.caseMismatchedGlobals.push_back(*spellings.begin());
        }
        if (!graph.declaredGlobals.empty() && !graph.declaredGlobals.contains(lower)) {
            graph.undeclaredGlobals.push_back(*spellings.begin());
        }
        if (firstReader.contains(lower) && !globalsSet.contains(lower)) {
            graph.gatesNeverSet.push_back(*spellings.begin());
        }
    }
    const auto unique = [](std::vector<std::string>& list) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    };
    unique(graph.entriesNotInJournal);
    unique(graph.entriesNoSetter);
    unique(graph.questsUnknown);
    unique(graph.gatesNeverSet);
}

void computeModuleEdges(Graph& graph)
{
    std::set<std::string> known;
    for (const ModuleInfo& module : graph.modules) {
        known.insert(module.name);
    }
    std::set<std::string> seen;
    const auto add = [&](ModuleEdge edge) {
        if (!known.contains(edge.to) || edge.to == edge.from) {
            return;
        }
        if (seen.insert(edge.from + ">" + edge.to + ">" + edge.via).second) {
            graph.moduleEdges.push_back(std::move(edge));
        }
    };
    for (const Edge& edge : graph.edges) {
        if (edge.effect.kind == "start_module" && !edge.debug) {
            // k_sup_galaxymap and friends run from the Ebon Hawk's galaxy map.
            const std::string from = edge.module == "global" ? std::string("ebo_m12aa") : edge.module;
            add({from, lowered(edge.effect.name), edge.id, "script", edge.effect.aux});
        }
    }
    for (const ModuleInfo& module : graph.modules) {
        for (const ObjectInfo& object : module.objects) {
            if (!object.placed || object.linkedModule.empty()) {
                continue;
            }
            add({module.name, object.linkedModule, object.kind + ":" + object.tag, object.kind, object.linkedTo});
        }
    }
}

bool satisfiesReq(std::int64_t value, const Req& req)
{
    if (req.op == "true") return value != 0;
    if (req.op == "false") return value == 0;
    if (req.op == "eq") return value == req.value;
    if (req.op == "ne") return value != req.value;
    if (req.op == "gt") return value > req.value;
    if (req.op == "ge") return value >= req.value;
    if (req.op == "lt") return value < req.value;
    if (req.op == "le") return value <= req.value;
    return true;
}

// Breadth-first from the first module over transitions. Returns the chain to `target`.
std::vector<ChainStep> routeTo(const Graph& graph, const std::string& from, const std::string& target)
{
    std::map<std::string, const ModuleEdge*> parent;
    std::queue<std::string> queue;
    queue.push(from);
    std::set<std::string> visited{from};
    while (!queue.empty() && !visited.contains(target)) {
        const std::string at = queue.front();
        queue.pop();
        for (const ModuleEdge& edge : graph.moduleEdges) {
            if (edge.from == at && visited.insert(edge.to).second) {
                parent[edge.to] = &edge;
                queue.push(edge.to);
            }
        }
    }
    std::vector<ChainStep> chain;
    if (!visited.contains(target)) {
        return chain;
    }
    std::string at = target;
    while (at != from) {
        const ModuleEdge* edge = parent.at(at);
        ChainStep step;
        step.from = edge->from;
        step.to = edge->to;
        step.via = edge->via;
        step.kind = edge->kind;
        for (const Edge& candidate : graph.edges) {
            if (candidate.id == edge->via) {
                if (!candidate.gates.empty()) {
                    for (const Req& req : candidate.gates.front()) {
                        step.gates.push_back(req.key());
                    }
                }
                break;
            }
        }
        chain.push_back(std::move(step));
        at = edge->from;
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

// The main plot as ordered checkpoints. The checkpoints are the modules that hold the
// plot's own master writes (K_KOTOR_MASTER, K_STAR_MAP, the star maps, G_FinalChoice in
// the graph); the route between two checkpoints comes from the data. The two endings are
// the cutscene modules whose k_endgame heartbeat ends the game after the credits.
void computeChain(Graph& graph)
{
    static const char* const kCheckpoints[] = {
        "end_m01aa", "tar_m02af", "tar_m08aa", "danm13",   "danm15",    "tat_m18ac", "kas_m25aa",
        "manm28ad",  "korr_m39aa", "lev_m40ac", "unk_m41aa", "unk_m44ac", "sta_m45aa",
    };
    graph.checkpoints.assign(std::begin(kCheckpoints), std::end(kCheckpoints));
    std::vector<ChainStep> common;
    std::string at = kCheckpoints[0];
    graph.mainPath.insert(at);
    const auto leg = [&](const std::string& from, const std::string& to, std::vector<ChainStep>& out) {
        std::vector<ChainStep> steps = routeTo(graph, from, to);
        if (steps.empty() && from != to) {
            graph.chainMissingLegs.push_back(from + ">" + to);
        }
        for (ChainStep& step : steps) {
            graph.mainPath.insert(step.from);
            graph.mainPath.insert(step.to);
            out.push_back(std::move(step));
        }
    };
    for (std::size_t i = 1; i < std::size(kCheckpoints); ++i) {
        leg(at, kCheckpoints[i], common);
        graph.mainPath.insert(kCheckpoints[i]);
        at = kCheckpoints[i];
    }
    graph.chainLight = common;
    graph.chainDark = common;
    leg(at, "stunt_56a", graph.chainLight);
    leg("stunt_56a", "stunt_57", graph.chainLight);
    leg(at, "stunt_54a", graph.chainDark);
    leg("stunt_54a", "stunt_55a", graph.chainDark);
    for (const Edge& edge : graph.edges) {
        if (edge.effect.kind == "end_game" && edge.module.rfind("stunt_", 0) == 0
            && std::find(graph.endingModules.begin(), graph.endingModules.end(), edge.module) == graph.endingModules.end()) {
            graph.endingModules.push_back(edge.module);
        }
    }
    // A gate on the chain that asks for a value no shipped non-debug code writes.
    std::map<std::string, std::set<std::int64_t>> writes;
    std::set<std::string> dynamicWrites;
    for (const Edge& edge : graph.edges) {
        if (edge.debug) {
            continue;
        }
        if (edge.effect.kind == "global_set" || edge.effect.kind == "journal_set") {
            const std::string key = (edge.effect.kind == "global_set" ? "global:" : "journal:") + lowered(edge.effect.name);
            if (edge.effect.valueKnown) {
                writes[key].insert(edge.effect.value);
            } else {
                dynamicWrites.insert(key);
            }
        }
    }
    std::set<std::string> missing;
    std::map<std::string, const Edge*> byId;
    for (const Edge& edge : graph.edges) {
        byId[edge.id] = &edge;
    }
    const auto checkChain = [&](const std::vector<ChainStep>& chain) {
        for (const ChainStep& step : chain) {
            const auto found = byId.find(step.via);
            if (found == byId.end() || found->second->gates.empty()) {
                continue;
            }
            for (const Req& req : found->second->gates.front()) {
                if (req.kind != "global" && req.kind != "journal") {
                    continue;
                }
                const std::string key = req.kind + ":" + lowered(req.name);
                if (dynamicWrites.contains(key)) {
                    continue;
                }
                bool reachable = false;
                for (const std::int64_t value : writes[key]) {
                    reachable = reachable || satisfiesReq(value, req);
                }
                reachable = reachable || satisfiesReq(0, req);
                if (!reachable) {
                    missing.insert(step.from + ">" + step.to + " " + req.key());
                }
            }
        }
    };
    checkChain(graph.chainLight);
    checkChain(graph.chainDark);
    graph.chainGatesNoSetter.assign(missing.begin(), missing.end());
    graph.chainResult = graph.chainMissingLegs.empty() ? "complete" : "missing_legs";
}

// ---- JSON ------------------------------------------------------------------------------

std::string json(const std::string& text)
{
    std::string out = "\"";
    for (const unsigned char ch : text) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[ch >> 4];
                out += hex[ch & 15];
            } else {
                out += static_cast<char>(ch);
            }
            break;
        }
    }
    out += '"';
    return out;
}

void writeStrings(std::ostream& out, const std::vector<std::string>& list)
{
    out << '[';
    for (std::size_t i = 0; i < list.size(); ++i) {
        out << (i == 0 ? "" : ",") << json(list[i]);
    }
    out << ']';
}

void writeReqs(std::ostream& out, const std::vector<ReqSet>& sets)
{
    out << '[';
    for (std::size_t i = 0; i < sets.size(); ++i) {
        std::vector<std::string> keys;
        for (const Req& req : sets[i]) {
            keys.push_back(req.key());
        }
        out << (i == 0 ? "" : ",");
        writeStrings(out, keys);
    }
    out << ']';
}

void writeChain(std::ostream& out, const std::vector<ChainStep>& chain)
{
    out << '[';
    for (std::size_t i = 0; i < chain.size(); ++i) {
        out << (i == 0 ? "" : ",") << "\n    {\"from\":" << json(chain[i].from) << ",\"to\":" << json(chain[i].to)
            << ",\"kind\":" << json(chain[i].kind) << ",\"via\":" << json(chain[i].via) << ",\"gates\":";
        writeStrings(out, chain[i].gates);
        out << '}';
    }
    out << ']';
}

} // namespace

namespace {

// The walk ran out of budget, or stopped following a state after a few visits. A call it never
// reached is still found: its arguments are taken from the constants directly before it, and a
// non-constant one stays unresolved.
void flatScan(const std::vector<Instruction>& code, ScriptFacts& facts)
{
    std::set<std::uint32_t> known;
    for (const Site& site : facts.sites) {
        known.insert(site.offset);
    }
    for (std::size_t i = 0; i < code.size(); ++i) {
        const Instruction& ins = code[i];
        if (ins.op != Op::ACTION || ins.actionId >= 772 || known.contains(ins.offset)) {
            continue;
        }
        const Act act = actionClasses()[ins.actionId];
        Effect effect;
        switch (act) {
        case Act::AddJournal: effect.kind = "journal_set"; break;
        case Act::RemoveJournal: effect.kind = "journal_remove"; effect.valueKnown = true; break;
        case Act::SetGlobalBool:
        case Act::SetGlobalNum: effect.kind = "global_set"; break;
        case Act::StartNewModule: effect.kind = "start_module"; effect.valueKnown = true; break;
        case Act::GivePlotXp: effect.kind = "plot_xp"; break;
        case Act::ExecuteScript: effect.kind = "exec_script"; effect.valueKnown = true; break;
        default: continue;
        }
        // The first argument is pushed last, so it sits directly before the call.
        const auto constAt = [&](std::size_t back) -> const Instruction* {
            return i >= back && code[i - back].op == Op::CONST ? &code[i - back] : nullptr;
        };
        if (const Instruction* first = constAt(1); first != nullptr && first->qualifier == 0x05) {
            effect.name.assign(first->bytes.begin(), first->bytes.end());
            effect.nameKnown = true;
        }
        if (const Instruction* second = constAt(2);
            second != nullptr && second->qualifier == 0x03 && !effect.valueKnown) {
            effect.value = second->operand0;
            effect.valueKnown = true;
        }
        Site site;
        site.offset = ins.offset;
        site.action = script::actionSpec(ins.actionId).name;
        site.effect = std::move(effect);
        Req unreached;
        unreached.kind = "unknown";
        unreached.op = "true";
        site.paths.push_back({unreached});
        facts.sites.push_back(std::move(site));
    }
}

} // namespace

std::string Req::key() const
{
    std::string out = kind + ":";
    if (!owner.empty()) {
        out += owner + "/";
    }
    out += name + " " + op;
    if (op != "true" && op != "false") {
        out += " " + (text.empty() ? std::to_string(value) : "'" + text + "'");
    }
    return out;
}

std::string Effect::key() const
{
    std::string out = kind + ":" + (nameKnown ? name : std::string("?"));
    if (!aux.empty()) {
        out += "/" + aux;
    }
    if (kind != "journal_remove" && kind != "start_module") {
        out += "=" + (valueKnown ? std::to_string(value) : std::string("?"));
    }
    return out;
}

std::string Host::id() const
{
    if (kind == "dlg_entry") {
        return "dlg:" + dlg + ":E" + std::to_string(node);
    }
    if (kind == "dlg_reply") {
        return "dlg:" + dlg + ":R" + std::to_string(node);
    }
    if (kind == "dlg_end" || kind == "dlg_abort") {
        return "dlg:" + dlg + ":" + kind;
    }
    if (kind == "dlg_cond") {
        return "dlg:" + dlg + ":cond:" + script;
    }
    if (kind == "scene_hook") {
        return "scene:" + hook;
    }
    if (kind == "hook") {
        return "hook:" + hook + ":" + script;
    }
    return "script:" + script;
}

std::string planetOf(const std::string& module)
{
    const std::string name = lowered(module);
    const auto starts = [&](const char* prefix) { return name.rfind(prefix, 0) == 0; };
    if (starts("end")) return "endar_spire";
    if (starts("tar")) return "taris";
    if (starts("dan")) return "dantooine";
    if (starts("kas")) return "kashyyyk";
    if (starts("kor")) return "korriban";
    if (starts("man")) return "manaan";
    if (starts("tat")) return "tatooine";
    if (starts("unk")) return "rakata";
    if (starts("sta")) return "star_forge";
    if (starts("lev")) return "leviathan";
    if (starts("ebo")) return "ebon_hawk";
    if (starts("liv")) return "yavin";
    if (starts("stunt")) return "cutscene";
    return "other";
}

ScriptFacts analyzeScript(const std::vector<std::uint8_t>& bytes)
{
    std::vector<Instruction> code;
    try {
        code = script::decodeNcs(bytes);
    } catch (const script::NcsError&) {
        ScriptFacts failed;
        failed.decodeFailed = true;
        return failed;
    }
    std::vector<std::string> names;
    std::vector<std::string> fragments;
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (code[i].op != Op::CONST || code[i].qualifier != 0x05 || code[i].bytes.empty()) {
            continue;
        }
        std::string text = lowered(std::string(code[i].bytes.begin(), code[i].bytes.end()));
        bool tagLookup = false;
        for (std::size_t later = i + 1; later < code.size() && later < i + 8; ++later) {
            if (code[later].op == Op::ADD && code[later].qualifier == 0x23) {
                if (text.size() >= 3) {
                    fragments.push_back(text);
                }
                break;
            }
            if (code[later].op == Op::ACTION && code[later].actionId < 772) {
                const Act act = actionClasses()[code[later].actionId];
                tagLookup = act == Act::ByTag || act == Act::NearestByTag;
                break;
            }
            if (code[later].op == Op::JSR) {
                break;
            }
        }
        if (!tagLookup) {
            names.push_back(std::move(text));
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::sort(fragments.begin(), fragments.end());
    fragments.erase(std::unique(fragments.begin(), fragments.end()), fragments.end());
    const auto withNames = [&](ScriptFacts facts) {
        facts.names = std::move(names);
        facts.fragments = std::move(fragments);
        return facts;
    };
    // Most of the corpus touches nothing this graph follows; skip the walk. A script
    // that only touches locals (the shared AI library, mostly) gets a small budget:
    // such a script gates by locals only when it is a short conditional.
    bool interesting = false;
    bool plot = false;
    for (const Instruction& ins : code) {
        if (ins.op == Op::ACTION && ins.actionId < 772 && actionClasses()[ins.actionId] != Act::None) {
            const Act act = actionClasses()[ins.actionId];
            if (act != Act::IsValid && act != Act::ByTag && act != Act::NearestByTag && act != Act::FirstPc
                && act != Act::Module && act != Act::Area) {
                interesting = true;
                if (act != Act::SetLocalBool && act != Act::SetLocalNum && act != Act::GetLocalBool
                    && act != Act::GetLocalNum) {
                    plot = true;
                    break;
                }
            }
        }
    }
    // A short script may still be a dialogue condition on something else entirely, such
    // as gold or a waypoint, and the harness needs that gate.
    constexpr std::size_t kShortScript = 300;
    if (!interesting && code.size() > kShortScript) {
        return withNames({});
    }
    ScriptFacts facts = Walker(code, plot ? 120000 : 15000).run();
    if (facts.truncated || facts.pathsDropped) {
        flatScan(code, facts);
    }
    if (std::any_of(code.begin(), code.end(), [](const Instruction& ins) {
            return ins.op == Op::ACTION && ins.actionId < 772 && std::string_view(script::actionSpec(ins.actionId).name) == "GetRunScriptVar";
        })) {
        facts.bytes = std::make_shared<const std::vector<std::uint8_t>>(bytes);
    }
    return withNames(std::move(facts));
}

// Which instructions a run can reach when GetRunScriptVar is a known constant. The path walker
// multiplies states on a large script; this keeps one state per instruction and joins, so it is
// linear and it always finishes. A branch is decided only by a constant that came from the pinned
// variable or from CONST; every other branch goes both ways, so the reachable set only ever errs
// large. Anything unmodelled clears `complete`, and then nothing is ruled out.
class PinnedReach {
public:
    PinnedReach(const std::vector<Instruction>& code, std::int64_t runVar)
        : code_(code)
        , runVar_(runVar)
    {
        for (std::size_t i = 0; i < code.size(); ++i) {
            byOffset_[code[i].offset] = i;
        }
        for (const Instruction& ins : code) {
            if (ins.op == Op::JMP || ins.op == Op::JZ || ins.op == Op::JNZ || ins.op == Op::JSR) {
                if (const auto to = target(ins); to.has_value()) {
                    targets_.insert(*to);
                }
            }
        }
    }

    RunVarReach run()
    {
        request(0, true);
        while (!pending_.empty() && ok_) {
            const std::size_t entry = *pending_.begin();
            pending_.erase(pending_.begin());
            walkFunction(entry);
        }
        RunVarReach out;
        out.complete = ok_;
        out.why = why_;
        out.offsets = std::move(reached_);
        return out;
    }

private:
    using Known = std::map<int, std::int64_t>;
    // A cell known only to be non-zero. Not a value: arithmetic and compares treat it as unknown.
    static constexpr std::int64_t kNonZero = std::numeric_limits<std::int64_t>::min();
    struct Flow {
        int depth = 0;        // Cells above the function's entry stack pointer.
        Known known;          // Cell index from that entry -> constant.
        bool pinned = true;   // False after an ExecuteScript could have changed the variable (00535b70).
    };
    struct Summary {
        bool done = false;
        bool active = false;
        std::optional<int> delta; // Net cells at RETN; none when it never returns.
        int lowest = 0;           // Lowest cell written, relative to entry: the caller's cells it reaches.
        bool mayExecute = false;  // An ExecuteScript runs inside.
    };

    static void pop(Flow& flow, int count)
    {
        flow.depth -= count;
        flow.known.erase(flow.known.lower_bound(flow.depth), flow.known.end());
    }
    static void push(Flow& flow, std::optional<std::int64_t> value = std::nullopt)
    {
        if (value.has_value()) {
            flow.known[flow.depth] = *value;
        }
        ++flow.depth;
    }
    static std::optional<std::int64_t> cell(const Flow& flow, int index)
    {
        const auto it = flow.known.find(index);
        return it == flow.known.end() ? std::nullopt : std::optional<std::int64_t>(it->second);
    }

    // A straight instruction's effect on the stack. False when it is not modelled.
    bool apply(const Instruction& ins, Flow& flow, int& lowest, bool& executes, bool readPin) const
    {
        switch (ins.op) {
        case Op::NOP:
        case Op::STORESTATEALL:
        case Op::SCRIPTSIZE:
        case Op::STORESTATE:
        case Op::CPDOWNBP:
            return true;
        case Op::INCBP:
        case Op::DECBP:
            return true;
        case Op::SAVEBP:
            // Cells below this are now globals, which CPDOWNBP can write from anywhere.
            flow.known.clear();
            push(flow);
            return true;
        case Op::RESTOREBP:
            pop(flow, 1);
            return true;
        case Op::RSADD:
            push(flow);
            return true;
        case Op::CONST:
            push(flow, ins.qualifier == 0x03 ? std::optional<std::int64_t>(ins.operand0) : std::nullopt);
            return true;
        case Op::CPTOPBP:
            for (int i = 0; i < ins.u0 / 4; ++i) {
                push(flow);
            }
            return true;
        case Op::CPTOPSP: {
            const int count = ins.u0 / 4;
            const int base = flow.depth + ins.operand0 / 4;
            for (int i = 0; i < count; ++i) {
                push(flow, cell(flow, base + i));
            }
            return true;
        }
        case Op::CPDOWNSP: {
            const int count = ins.u0 / 4;
            const int base = flow.depth + ins.operand0 / 4;
            for (int i = 0; i < count; ++i) {
                const std::optional<std::int64_t> value = cell(flow, flow.depth - count + i);
                lowest = std::min(lowest, base + i);
                if (value.has_value()) {
                    flow.known[base + i] = *value;
                } else {
                    flow.known.erase(base + i);
                }
            }
            return true;
        }
        case Op::MOVSP:
            if (ins.operand0 > 0) {
                return false; // Retail leaves SP (005d21f0); no shipped script should do this.
            }
            pop(flow, -ins.operand0 / 4);
            return true;
        case Op::INCSP:
        case Op::DECSP: {
            const int index = flow.depth + ins.operand0 / 4;
            lowest = std::min(lowest, index);
            if (const auto value = cell(flow, index); value.has_value() && *value != kNonZero) {
                flow.known[index] = *value + (ins.op == Op::INCSP ? 1 : -1);
            } else {
                flow.known.erase(index);
            }
            return true;
        }
        case Op::DESTRUCT: {
            const int total = ins.u0 / 4;
            const int offset = ins.u1 / 4;
            const int keep = ins.u2 / 4;
            const int begin = flow.depth - total;
            std::vector<std::optional<std::int64_t>> kept;
            for (int i = 0; i < keep; ++i) {
                kept.push_back(cell(flow, begin + offset + i));
            }
            pop(flow, total);
            for (const auto& value : kept) {
                push(flow, value);
            }
            return true;
        }
        case Op::ACTION: {
            if (ins.actionId >= 772) {
                return false;
            }
            const script::ActionSpec& spec = script::actionSpec(ins.actionId);
            const int given = std::min<int>(ins.actionArgc, spec.paramCount); // Handlers pop what they declare.
            for (int i = 0; i < given; ++i) {
                if (spec.params[i] != script::ParamKind::Action) {
                    pop(flow, std::max(0, script::parameterCells(spec.params[i])));
                }
            }
            const std::string_view name(spec.name);
            if (name == "ExecuteScript") {
                executes = true;
                flow.pinned = false;
            }
            switch (spec.returnKind) {
            case script::ReturnKind::Void:
                break;
            case script::ReturnKind::Vector:
                pop(flow, 0);
                push(flow);
                push(flow);
                push(flow);
                break;
            default:
                if (name == "GetRunScriptVar" && flow.pinned && readPin) {
                    push(flow, runVar_);
                } else if (name == "ShipBuild") {
                    push(flow, 1); // retail 00543840
                } else {
                    push(flow);
                }
                break;
            }
            return true;
        }
        case Op::LOGAND:
        case Op::LOGOR: {
            const auto b = cell(flow, flow.depth - 1);
            const auto a = cell(flow, flow.depth - 2);
            pop(flow, 2);
            const bool isAnd = ins.op == Op::LOGAND;
            if (a.has_value() && b.has_value()) {
                push(flow, isAnd ? (*a != 0 && *b != 0) : (*a != 0 || *b != 0));
            } else if ((a.has_value() && (*a != 0) != isAnd) || (b.has_value() && (*b != 0) != isAnd)) {
                push(flow, isAnd ? 0 : 1); // Both sides are computed; one alone settles it.
            } else {
                push(flow);
            }
            return true;
        }
        case Op::EQUAL:
        case Op::NEQUAL:
        case Op::GEQ:
        case Op::GT:
        case Op::LT:
        case Op::LEQ: {
            const int width = ins.qualifier == 0x24 ? std::max(1, ins.u0 / 4) : 1;
            const bool bothInt = ins.qualifier == 0x20;
            const auto b = bothInt ? cell(flow, flow.depth - 1) : std::nullopt;
            const auto a = bothInt ? cell(flow, flow.depth - 2) : std::nullopt;
            pop(flow, width * 2);
            if (a.has_value() && b.has_value() && *a != kNonZero && *b != kNonZero) {
                std::int64_t value = 0;
                switch (ins.op) {
                case Op::EQUAL: value = *a == *b; break;
                case Op::NEQUAL: value = *a != *b; break;
                case Op::GEQ: value = *a >= *b; break;
                case Op::GT: value = *a > *b; break;
                case Op::LT: value = *a < *b; break;
                default: value = *a <= *b; break;
                }
                push(flow, value);
            } else {
                push(flow);
            }
            return true;
        }
        case Op::NOT: {
            const auto a = cell(flow, flow.depth - 1);
            pop(flow, 1);
            push(flow, a.has_value() ? std::optional<std::int64_t>(*a == 0) : std::nullopt);
            return true;
        }
        case Op::NEG:
        case Op::COMP:
            pop(flow, 1);
            push(flow);
            return true;
        case Op::ADD:
        case Op::SUB:
        case Op::MUL:
        case Op::DIV:
        case Op::MOD:
        case Op::BOOLAND:
        case Op::INCOR:
        case Op::EXCOR:
        case Op::SHLEFT:
        case Op::SHRIGHT:
        case Op::USHRIGHT: {
            int popped = 2;
            int pushed = 1;
            if (ins.qualifier == 0x3A) {
                popped = 6;
                pushed = 3;
            } else if (ins.qualifier == 0x3B || ins.qualifier == 0x3C) {
                popped = 4;
                pushed = 3;
            }
            pop(flow, popped);
            for (int i = 0; i < pushed; ++i) {
                push(flow);
            }
            return true;
        }
        default:
            return false;
        }
    }

    void fail(const char* reason, std::size_t ip)
    {
        if (ok_) {
            why_ = std::string(reason) + " at " + std::to_string(ip < code_.size() ? code_[ip].offset : 0);
        }
        ok_ = false;
    }

    std::optional<std::size_t> target(const Instruction& ins) const
    {
        const auto it = byOffset_.find(static_cast<std::uint32_t>(static_cast<std::int32_t>(ins.offset) + ins.operand0));
        return it == byOffset_.end() ? std::nullopt : std::optional<std::size_t>(it->second);
    }

    // What a call does to its caller, with the variable unpinned.
    const Summary& summarize(std::size_t entry)
    {
        if (summaries_[entry].done) {
            return summaries_[entry];
        }
        if (summaries_[entry].active) {
            fail("recursion", entry); // The stack effect is not worked out here.
            return summaries_[entry];
        }
        summaries_[entry].active = true;
        Summary local;
        walkFunction(entry, &local);
        local.done = true;
        summaries_[entry] = local;
        return summaries_[entry];
    }

    // `pinned` is true when every caller reaching this entry still holds the constant.
    void request(std::size_t entry, bool pinned)
    {
        const auto it = entries_.find(entry);
        if (it == entries_.end()) {
            entries_.emplace(entry, pinned);
            pending_.insert(entry);
        } else if (it->second && !pinned) {
            it->second = false;
            pending_.insert(entry);
        }
    }

    // One function to its fixed point. With `summary` it only measures the stack; without, it
    // records what is reached and asks for its callees.
    void walkFunction(std::size_t entry, Summary* summary = nullptr)
    {
        std::map<std::size_t, Flow> at;
        std::vector<std::size_t> work;
        const auto send = [&](std::size_t ip, const Flow& flow) {
            const auto it = at.find(ip);
            if (it == at.end()) {
                at.emplace(ip, flow);
                work.push_back(ip);
                return;
            }
            Flow& held = it->second;
            if (held.depth != flow.depth) {
                fail("depth differs at a join", ip);
                return;
            }
            bool changed = false;
            for (auto cellIt = held.known.begin(); cellIt != held.known.end();) {
                const auto other = flow.known.find(cellIt->first);
                if (other == flow.known.end() || other->second != cellIt->second) {
                    cellIt = held.known.erase(cellIt);
                    changed = true;
                } else {
                    ++cellIt;
                }
            }
            if (held.pinned && !flow.pinned) {
                held.pinned = false;
                changed = true;
            }
            if (changed) {
                work.push_back(ip);
            }
        };
        Flow start;
        start.pinned = summary == nullptr && entries_[entry];
        send(entry, start);
        while (!work.empty() && ok_) {
            const std::size_t ip = work.back();
            work.pop_back();
            if (ip >= code_.size()) {
                fail("ran off the end", entry);
                return;
            }
            const Instruction& ins = code_[ip];
            if (summary == nullptr) {
                reached_.insert(ins.offset);
            }
            Flow flow = at[ip];
            switch (ins.op) {
            case Op::JMP: {
                const auto to = target(ins);
                if (!to.has_value()) {
                    fail("no target", ip);
                    break;
                }
                send(*to, flow);
                break;
            }
            case Op::JZ:
            case Op::JNZ: {
                const auto to = target(ins);
                if (!to.has_value()) {
                    fail("no target", ip);
                    break;
                }
                const auto value = cell(flow, flow.depth - 1);
                pop(flow, 1);
                // A test of a copy of the top cell tells what that cell holds on each side. The
                // compiler's || tests its left side twice like this, and the second jump never runs.
                const bool ofCopy = ip > 0 && code_[ip - 1].op == Op::CPTOPSP && code_[ip - 1].operand0 == -4
                    && code_[ip - 1].u0 == 4 && !targets_.contains(ip);
                const bool jumpWhenSet = ins.op == Op::JNZ;
                const auto refined = [&](bool isSet) {
                    Flow side = flow;
                    if (ofCopy && !value.has_value()) {
                        side.known[side.depth - 1] = isSet ? kNonZero : 0;
                    }
                    return side;
                };
                if (!value.has_value() || (*value != 0) == jumpWhenSet) {
                    send(*to, refined(jumpWhenSet));
                }
                if (!value.has_value() || (*value != 0) != jumpWhenSet) {
                    send(ip + 1, refined(!jumpWhenSet));
                }
                break;
            }
            case Op::JSR: {
                const auto to = target(ins);
                if (!to.has_value()) {
                    fail("no target", ip);
                    break;
                }
                const Summary called = summarize(*to);
                if (summary != nullptr) {
                    summary->lowest = std::min(summary->lowest, flow.depth + called.lowest);
                    summary->mayExecute = summary->mayExecute || called.mayExecute;
                } else {
                    request(*to, flow.pinned);
                }
                if (called.delta.has_value()) {
                    flow.known.erase(flow.known.lower_bound(flow.depth + called.lowest), flow.known.end());
                    flow.depth += *called.delta;
                    flow.known.erase(flow.known.lower_bound(flow.depth), flow.known.end());
                    flow.pinned = flow.pinned && !called.mayExecute;
                    send(ip + 1, flow);
                }
                break;
            }
            case Op::RETN:
                if (summary != nullptr) {
                    if (summary->delta.has_value() && *summary->delta != flow.depth) {
                        fail("returns differ", ip);
                    }
                    summary->delta = flow.depth;
                }
                break;
            case Op::STORESTATE: {
                // The closure runs later, on a copy of the stack, when the variable may have moved on.
                if (summary == nullptr) {
                    std::size_t body = ip + 1;
                    if (body < code_.size() && code_[body].op == Op::JMP) {
                        ++body;
                    }
                    request(body, false);
                }
                send(ip + 1, flow);
                break;
            }
            default: {
                int lowest = 0;
                bool executes = false;
                if (!apply(ins, flow, lowest, executes, summary == nullptr)) {
                    fail("not modelled", ip);
                }
                if (summary != nullptr) {
                    summary->lowest = std::min(summary->lowest, lowest);
                    summary->mayExecute = summary->mayExecute || executes;
                }
                send(ip + 1, flow);
                break;
            }
            }
        }
    }

    const std::vector<Instruction>& code_;
    std::int64_t runVar_;
    std::unordered_map<std::uint32_t, std::size_t> byOffset_;
    std::map<std::size_t, Summary> summaries_;
    std::map<std::size_t, bool> entries_;
    std::set<std::size_t> pending_;
    std::set<std::size_t> targets_;
    std::unordered_set<std::uint32_t> reached_;
    std::string why_;
    bool ok_ = true;
};

RunVarReach reachForRunVar(const std::vector<std::uint8_t>& bytes, std::int64_t runVar)
{
    const std::vector<Instruction> code = script::decodeNcs(bytes);
    return PinnedReach(code, runVar).run();
}

Graph buildGraph(kotor::Install& install)
{
    const auto started = std::chrono::steady_clock::now();
    const auto since = [&]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    Graph graph;
    Walk walk{install, graph, {}};
    walk.readJournal();

    if (auto data = install.find("globalcat", 2017); data.has_value()) {
        try {
            const kotor::TwoDa table = kotor::TwoDa::parse(std::move(*data), "globalcat.2da");
            const int name = table.columnIndex("name");
            for (std::size_t row = 0; row < table.rowCount() && name >= 0; ++row) {
                graph.declaredGlobals.insert(lowered(table.cell(row, static_cast<std::size_t>(name))));
            }
        } catch (const kotor::FormatError&) {
        }
    }

    for (const std::string& resref : install.resourceNames(kNcs)) {
        if (auto bytes = install.find(resref, kNcs); bytes.has_value()) {
            graph.globalScripts[lowered(resref)] = walk.factsFor(*bytes);
        }
    }
    for (const std::string& resref : install.resourceNames(kDlg)) {
        if (auto bytes = install.find(resref, kDlg); bytes.has_value()) {
            try {
                const std::string key = lowered(resref);
                graph.dialogues["|" + key] = dlgInfo(std::move(*bytes), key, {});
            } catch (const kotor::FormatError&) {
            }
        }
    }

    // The 117 module stems: every <stem>.rim beside a <stem>_s.rim.
    std::vector<std::string> stems;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(install.root() / "modules", error)) {
        const std::filesystem::path& path = entry.path();
        if (lowered(path.extension().string()) != ".rim") {
            continue;
        }
        const std::string stem = lowered(path.stem().string());
        if (stem.size() > 2 && stem.compare(stem.size() - 2, 2, "_s") == 0) {
            continue;
        }
        stems.push_back(stem);
    }
    std::sort(stems.begin(), stems.end());
    std::printf("quest graph: global scripts=%zu (%.0fs)\n", graph.scriptCount, since());
    for (const std::string& stem : stems) {
        walk.readModule(stem);
    }
    std::printf("quest graph: modules read, unique scripts=%zu truncated=%zu (%.0fs)\n", graph.scriptCount,
                graph.scriptsTruncated, since());
    graph.dlgCount = graph.dialogues.size();

    // Templates shipped outside the modules, the companions among them: a script names one by resref.
    for (const std::uint32_t type : {kUtc, kUtd, kUtp, kUtt, kUte, kUtm, kUtw}) {
        for (const std::string& resref : install.resourceNames(type)) {
            if (auto bytes = install.find(resref, type); bytes.has_value()) {
                try {
                    ObjectInfo object;
                    object.resref = lowered(resref);
                    const kotor::Gff gff = kotor::Gff::parse(std::move(*bytes), resref);
                    readTemplate(gff, type, object);
                    graph.globalObjects.push_back(std::move(object));
                } catch (const kotor::FormatError&) {
                }
            }
        }
    }
    // Tables and the executable name scripts and templates the game itself starts.
    for (const std::string& resref : install.resourceNames(2017)) {
        if (auto bytes = install.find(resref, 2017); bytes.has_value()) {
            walk.addRawNames(*bytes);
        }
    }
    {
        std::ifstream exe(install.root() / "swkotor.exe", std::ios::binary);
        walk.addRawNames(std::vector<std::uint8_t>(std::istreambuf_iterator<char>(exe), {}));
    }
    // Creatures the game brings in by template are plot too: their conversations and hooks had no host
    // while only module archives were looked at. A messenger a module's script spawns by name belongs to
    // that module; the nine companions, each named by AddAvailableNPCByTemplate, stand on the Ebon Hawk.
    {
        std::unordered_map<std::string, const ObjectInfo*> shipped;
        for (const ObjectInfo& object : graph.globalObjects) {
            if (object.kind == "creature") {
                shipped.emplace(object.resref, &object);
            }
        }
        const auto bring = [&](ModuleInfo& module, const std::string& resref) {
            const auto found = shipped.find(resref);
            const auto same = [&](const ObjectInfo& object) { return object.resref == resref; };
            if (found != shipped.end() && std::none_of(module.objects.begin(), module.objects.end(), same)) {
                module.objects.push_back(*found->second);
            }
        };
        for (ModuleInfo& module : graph.modules) {
            std::set<std::string> named;
            for (const auto& [resref, index] : module.scripts) {
                for (const Site& site : graph.facts[index].sites) {
                    if (site.effect.kind == "create_object" && site.effect.nameKnown && site.effect.value == 1) {
                        named.insert(lowered(site.effect.name));
                    }
                }
            }
            for (const std::string& resref : named) {
                bring(module, resref);
            }
            if (module.name == "ebo_m12aa") {
                for (const char* companion : {"p_bastilla", "p_canderous", "p_carth", "p_hk47", "p_jolee", "p_juhani",
                                              "p_mission", "p_t3m4", "p_zaalbar"}) {
                    bring(module, companion);
                }
            }
        }
    }
    EdgeBuilder(graph).build();
    markUnreachableHosts(graph, walk.rawNames);
    markLinkContradictions(graph);
    markOwnerTagBranches(graph);
    markAbsentWaypoints(graph);
    markBooleanVsValue(graph);
    computeFlags(graph);
    computeModuleEdges(graph);

    computeChain(graph);
    return graph;
}

void writeGraphJson(const Graph& graph, const std::filesystem::path& path)
{
    std::ofstream out(path, std::ios::binary);
    std::size_t entries = 0;
    for (const JournalQuest& quest : graph.quests) {
        entries += quest.entries.size();
    }
    out << "{\n\"schema\":\"kotor.quest_graph/1\",\n\"counts\":{\"modules\":" << graph.modules.size()
        << ",\"quests\":" << graph.quests.size() << ",\"entries\":" << entries << ",\"edges\":" << graph.edges.size()
        << ",\"scripts\":" << graph.scriptCount << ",\"scripts_truncated\":" << graph.scriptsTruncated
        << ",\"scripts_decode_failed\":" << graph.scriptsFailed << ",\"dialogues\":" << graph.dlgCount
        << ",\"dialogue_nodes\":" << graph.nodeCount << ",\"unresolved\":" << graph.unresolved.size() << "},\n";

    // Per quest, per entry: setters, gates and readers.
    std::map<std::string, std::vector<const Edge*>> setters;
    for (const Edge& edge : graph.edges) {
        if (edge.effect.kind == "journal_set" && edge.effect.valueKnown) {
            setters[lowered(edge.effect.name) + "=" + std::to_string(edge.effect.value)].push_back(&edge);
        }
    }
    out << "\"quests\":[";
    for (std::size_t q = 0; q < graph.quests.size(); ++q) {
        const JournalQuest& quest = graph.quests[q];
        out << (q == 0 ? "" : ",") << "\n {\"tag\":" << json(quest.tag) << ",\"plot_index\":" << quest.plotIndex
            << ",\"xp\":" << quest.xp << ",\"entries\":[";
        for (std::size_t e = 0; e < quest.entries.size(); ++e) {
            const JournalEntry& entry = quest.entries[e];
            const std::string key = lowered(quest.tag) + "=" + std::to_string(entry.id);
            out << (e == 0 ? "" : ",") << "\n  {\"id\":" << entry.id << ",\"end\":" << (entry.end ? "true" : "false")
                << ",\"xp_percentage\":" << entry.xpPercentage << ",\"setters\":[";
            const auto found = setters.find(key);
            if (found != setters.end()) {
                for (std::size_t s = 0; s < found->second.size(); ++s) {
                    out << (s == 0 ? "" : ",") << json(found->second[s]->id);
                }
            }
            out << "],\"read_by\":";
            const auto readers = graph.journalReaders.find(key);
            writeStrings(out, readers != graph.journalReaders.end() ? readers->second : std::vector<std::string>{});
            out << '}';
        }
        out << "]}";
    }
    out << "\n],\n\"edges\":[";
    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        const Edge& edge = graph.edges[i];
        out << (i == 0 ? "" : ",") << "\n {\"id\":" << json(edge.id) << ",\"module\":" << json(edge.module)
            << ",\"planet\":" << json(edge.planet) << ",\"host\":" << json(edge.host.kind)
            << ",\"owner\":" << json(edge.host.ownerTag) << ",\"owner_template\":" << json(edge.host.ownerResref)
            << ",\"link_active\":" << json(edge.host.linkActive) << ",\"script\":" << json(edge.siteScript)
            << ",\"offset\":" << edge.siteOffset << ",\"effect\":" << json(edge.effect.key())
            << ",\"deferred\":" << (edge.deferred ? "true" : "false")
            << ",\"dlg_linked\":" << (edge.dlgLinked ? "true" : "false")
            << ",\"orphan\":" << (edge.orphan ? "true" : "false")
            << ",\"debug\":" << (edge.debug ? "true" : "false") << ",\"dead\":\"" << edge.dead
            << "\",\"path_active\":";
        writeStrings(out, edge.pathActive);
        out << ",\"gates\":";
        writeReqs(out, edge.gates);
        out << '}';
    }
    out << "\n],\n\"module_edges\":[";
    for (std::size_t i = 0; i < graph.moduleEdges.size(); ++i) {
        const ModuleEdge& edge = graph.moduleEdges[i];
        out << (i == 0 ? "" : ",") << "\n {\"from\":" << json(edge.from) << ",\"to\":" << json(edge.to)
            << ",\"kind\":" << json(edge.kind) << ",\"via\":" << json(edge.via) << ",\"aux\":" << json(edge.aux) << '}';
    }
    out << "\n],\n\"unresolved\":[";
    for (std::size_t i = 0; i < graph.unresolved.size(); ++i) {
        const Unresolved& item = graph.unresolved[i];
        out << (i == 0 ? "" : ",") << "\n {\"module\":" << json(item.module) << ",\"script\":" << json(item.script)
            << ",\"action\":" << json(item.action) << ",\"offset\":" << item.offset
            << ",\"detail\":" << json(item.detail) << '}';
    }
    out << "\n],\n\"flags\":{\"entries_no_setter\":";
    writeStrings(out, graph.entriesNoSetter);
    out << ",\n\"entries_not_in_journal\":";
    writeStrings(out, graph.entriesNotInJournal);
    out << ",\n\"quests_unknown\":";
    writeStrings(out, graph.questsUnknown);
    out << ",\n\"globals_read_never_set\":";
    writeStrings(out, graph.gatesNeverSet);
    out << ",\n\"globals_case_mismatch\":";
    writeStrings(out, graph.caseMismatchedGlobals);
    out << ",\n\"globals_undeclared\":";
    writeStrings(out, graph.undeclaredGlobals);
    out << "},\n\"chain\":{\"ending_modules\":";
    writeStrings(out, graph.endingModules);
    out << ",\n\"main_path\":";
    writeStrings(out, std::vector<std::string>(graph.mainPath.begin(), graph.mainPath.end()));
    out << ",\n\"result\":" << json(graph.chainResult) << ",\n\"missing_legs\":";
    writeStrings(out, graph.chainMissingLegs);
    out << ",\n\"gates_no_setter\":";
    writeStrings(out, graph.chainGatesNoSetter);
    out << ",\n\"route_light\":";
    writeChain(out, graph.chainLight);
    out << ",\n\"route_dark\":";
    writeChain(out, graph.chainDark);
    out << "}\n}\n";
}

} // namespace ke::quest
