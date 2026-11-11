#include "game/StateTrace.h"

#include "game/GameSession.h"
#include "game/GuiSystem.h"
#include "game/ScriptHost.h"
#include "kotor/Gui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace ke {

namespace {

constexpr std::uint64_t kSettleFrames = 90;
constexpr std::uint64_t kPanelFrames = 12;
constexpr std::uint64_t kDialogueFrames = 20;
constexpr std::uint64_t kRoundFrames = 20;
constexpr std::uint64_t kControlFrames = 2;
constexpr std::size_t kVoiceCap = 1024;

std::string lowered(std::string text)
{
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// Floats are written with exactly three decimals; -0.000 is written 0.000.
std::string f3(float value)
{
    if (!std::isfinite(value)) {
        return "0.000";
    }
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%.3f", static_cast<double>(value));
    std::string text = buffer;
    if (text == "-0.000") {
        text = "0.000";
    }
    return text;
}

std::string hex(std::uint32_t value)
{
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "0x%x", value);
    return buffer;
}

// An empty value is `-`; TAB, CR and LF inside a value become a space.
std::string clean(std::string text)
{
    for (char& c : text) {
        if (c == '\t' || c == '\r' || c == '\n') {
            c = ' ';
        }
    }
    return text.empty() ? std::string("-") : text;
}

// A panel is named by its layout resref: the shipped GUI file without ".gui", lower case.
std::string panelName(const std::string& screenName)
{
    std::string name = lowered(screenName);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".gui") == 0) {
        name.resize(name.size() - 4);
    }
    return name;
}

void flatten(const kotor::GuiControl& control, std::vector<const kotor::GuiControl*>& out)
{
    out.push_back(&control);
    for (const kotor::GuiControl& child : control.children) {
        flatten(child, out);
    }
}

} // namespace

StateTrace::StateTrace() = default;

StateTrace::~StateTrace()
{
    detach();
    if (open_) {
        out_.flush();
        out_.close();
        open_ = false;
    }
}

bool StateTrace::open(const std::filesystem::path& file, int samp, std::string& error)
{
    std::error_code ignored;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ignored);
    }
    out_.open(file, std::ios::binary | std::ios::trunc);
    if (!out_) {
        error = "cannot open " + file.generic_string();
        return false;
    }
    samp_ = std::max(1, samp);
    shotDir_ = file;
    shotDir_.replace_extension();
    shotDir_ += "_shots";
    startTicks_ = SDL_GetTicks();
    out_ << "#kotor-state-trace\t1\n";
    out_ << "#meta\tsource\tengine\n";
    out_ << "#meta\tbuild\tengine-dev\n";
    out_ << "#meta\tcfg\t{\"samp\":" << samp_ << ",\"types\":[5,9,10],\"settle\":" << kSettleFrames
         << ",\"panelDelay\":" << kPanelFrames << ",\"dlgDelay\":" << kDialogueFrames << ",\"rndDelay\":"
         << kRoundFrames << "}\n";
    out_ << "#meta\tspec\tTRACE_SPEC v1 (phase 2), ENGINE_EMITTER phase 1\n";
    out_ << "#meta\tshot_format\tbmp\n";
    open_ = true;
    return true;
}

void StateTrace::attach(GameSession& game, bool menuBackdrop)
{
    game_ = &game;
    attached_ = true;
    backdrop_ = menuBackdrop;
    game.setStateTrace(this);
    game.audio().mixer().setPlayObserver([this](const audio::Mixer::PlayInfo& info) {
        if (!open_ || info.name == nullptr) {
            return;
        }
        // Behind the main menu the session runs a backdrop world retail has no counterpart for: only the
        // menu's own sounds (the theme and the interface sounds) are reported until a real module is ready.
        if (backdrop_ && info.name->rfind("gui_", 0) != 0 && info.name->rfind("mus_theme", 0) != 0) {
            return;
        }
        // Music, the ambient loops and the spoken lines are streams in retail; everything else is a sample.
        const bool stream = info.bus == audio::Bus::Music || info.bus == audio::Bus::Ambient
            || (info.bus == audio::Bus::Voice && !info.positional);
        std::vector<std::string> tail;
        tail.push_back(stream ? "stream" : (info.positional ? "sample3d" : "sample2d"));
        tail.push_back(clean(*info.name));
        tail.push_back(info.positional ? f3(info.position.x) : "-");
        tail.push_back(info.positional ? f3(info.position.y) : "-");
        tail.push_back(info.positional ? f3(info.position.z) : "-");
        tail.push_back("-"); // vol: the engine has the mixer level, not the source volume byte
        tail.push_back("-"); // group: the engine keeps the priority group only inside playGroupSound
        tail.push_back(info.loop ? "1" : "0");
        tail.push_back(f3(info.minDistance));
        tail.push_back(f3(info.maxDistance));
        tail.push_back(stream ? "stream" : "sample");
        std::vector<std::string> fields{"start"};
        fields.insert(fields.end(), tail.begin(), tail.end());
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        writeSound(fields);
        voices_.push_back({info.voice, std::move(tail)});
        if (voices_.size() > kVoiceCap) {
            voices_.erase(voices_.begin());
        }
    });
    if (!menuBackdrop) {
        onModuleReady(game);
    }
}

void StateTrace::detach()
{
    if (attached_ && game_ != nullptr) {
        game_->audio().mixer().setPlayObserver(nullptr);
        game_->setStateTrace(nullptr);
    }
    attached_ = false;
    game_ = nullptr;
}

// ---- writing ---------------------------------------------------------------------------------------------------------

void StateTrace::write(const char* kind, const std::vector<std::string>& fields)
{
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!open_) {
        return;
    }
    if (std::string_view(kind) == "chk") {
        // Ground truth next to the mirror: a comment line, never compared.
        out_ << "#chk\tframe " << frame_;
        for (const std::string& field : fields) {
            out_ << ' ' << field;
        }
        out_ << '\n';
        return;
    }
    out_ << frame_ << '\t' << (SDL_GetTicks() - startTicks_) << "\t-\t" << kind;
    for (const std::string& field : fields) {
        out_ << '\t' << field;
    }
    out_ << '\n';
}

// A state record: written only when its content differs from the last one written for the same class and key.
void StateTrace::state(const char* kind, const std::string& key, const std::vector<std::string>& fields)
{
    std::string payload;
    for (const std::string& field : fields) {
        payload += field;
        payload += '\t';
    }
    std::string& previous = last_[std::string(kind) + '\n' + key];
    if (previous == payload) {
        return;
    }
    previous = std::move(payload);
    write(kind, fields);
}

void StateTrace::schedule(std::uint64_t afterFrames, const std::string& shotName)
{
    Pending pending;
    pending.due = frame_ + afterFrames;
    pending.name = shotName;
    pending_.push_back(std::move(pending));
}

std::vector<std::filesystem::path> StateTrace::takeShots()
{
    std::vector<std::filesystem::path> out;
    out.swap(shotsOut_);
    return out;
}

void StateTrace::resetWorld()
{
    for (auto it = last_.begin(); it != last_.end();) {
        const std::string& id = it->first;
        const bool world = id.rfind("obj", 0) == 0 || id.rfind("clip", 0) == 0 || id.rfind("chk", 0) == 0
            || id.rfind("party", 0) == 0 || id.rfind("sel", 0) == 0 || id.rfind("cam", 0) == 0
            || id.rfind("area", 0) == 0 || id.rfind("pause", 0) == 0;
        it = world ? last_.erase(it) : std::next(it);
    }
    keyById_.clear();
    seen_.clear();
    objects_.clear();
    entityKey_.clear();
    dlgcamLast_.clear();
    dlgcamDirty_ = false;
    camMode_.clear();
}

// ---- hooks -----------------------------------------------------------------------------------------------------------

void StateTrace::onModuleReady(GameSession& game)
{
    if (!open_) {
        return;
    }
    if (!loadScreen_.empty()) {
        write("gui", {"close", loadScreen_, "-", "-", "-", "-", "-"});
        loadScreen_.clear();
    }
    if (!reported_.empty()) {
        write("mod", {"unload", clean(reported_)});
    }
    reported_ = game.module_;
    backdrop_ = false;
    resetWorld();
    write("mod", {"ready", clean(reported_)});
    schedule(kSettleFrames, "settle:" + reported_);
}

void StateTrace::onLoadScreen(const std::string& name, int x, int y, int w, int h)
{
    if (!open_) {
        return;
    }
    const std::string shown = lowered(name);
    write("gui", {"open", clean(shown), "1", std::to_string(x), std::to_string(y), std::to_string(w), std::to_string(h)});
    loadScreen_ = shown;
}

void StateTrace::onAction(std::uint16_t actionId)
{
    if (open_ && !backdrop_) {
        write("act", {std::to_string(actionId)});
    }
}

// A combat round started (TRACE_SPEC 4.11). The engine runs rounds only for creatures that attack; retail starts one for
// every creature about every 3 s, so this class differs by design. Only the first 8 rounds get an anchor (section 5).
void StateTrace::onRound(GameSession& game, std::int32_t number, std::uint32_t owner)
{
    if (!open_ || backdrop_) {
        return;
    }
    write("rnd", {"start", std::to_string(number), keyOfObject(game, owner)});
    if (number <= 8) {
        schedule(kRoundFrames, "round:" + std::to_string(number));
    }
}

void StateTrace::onDialogueLine(GameSession& game, std::uint32_t entry)
{
    if (!open_ || backdrop_ || !game.activeDialogue_.has_value() || entry >= game.activeDialogue_->dlg.entries.size()) {
        return;
    }
    const kotor::DlgEntry& line = game.activeDialogue_->dlg.entries[entry];
    ++dlgCount_;
    // TRACE_SPEC 4.9: dlgResref reads empty in retail, so it is `-` here too and the anchor is dlg:-:n. The text is
    // only hashed (FNV-1a 32 over the line as the UI holds it, after token substitution), never written.
    std::uint32_t hash = 2166136261u;
    for (const char c : game.dialogueView_.text) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
    }
    char hashText[16];
    std::snprintf(hashText, sizeof hashText, "%08x", hash);
    const DialogueView& view = game.dialogueView_;
    const std::string owner = keyOfObject(game, game.activeDialogue_->npcObject);
    const std::string speaker = keyOfObject(game, view.speakerObject);
    const std::string listener = keyOfObject(game, view.listenerObject);
    write("dlg", {"line", "-", std::to_string(dlgCount_), owner, speaker, listener, std::to_string(view.cameraAngle),
                  std::to_string(view.cameraId), std::to_string(line.animList.size()), view.text.empty() ? "-" : hashText,
                  "-"});
    schedule(kDialogueFrames, "dlg:-:" + std::to_string(dlgCount_));
    dlgcamDirty_ = true;
}

// ---- per frame -------------------------------------------------------------------------------------------------------

void StateTrace::step(const StateTraceFrame& frame)
{
    if (!open_) {
        return;
    }
    if (frame.game != nullptr && frame.game != game_) {
        game_ = frame.game;
    }
    hudSeen_ = hudSeen_ || !frame.hudLayout.empty();
    const bool menuUp = frame.menuGui != nullptr && frame.menuGui->active();
    const bool atMainMenu = menuUp && frame.menuGui->screenName().rfind("mainmenu", 0) == 0;
    inPlay_ = frame.game != nullptr && !atMainMenu && (hudSeen_ || !menuUp);

    trackGui(frame);
    if (frame.game != nullptr) {
        GameSession& game = *frame.game;
        if (inPlay_) {
            if (frame_ % static_cast<std::uint64_t>(samp_) == 0) {
                sampleState(frame);
            }
            trackDialogueCamera(game);
        }
        trackSounds();
    }

    // Controls and anchors that fell due.
    for (std::size_t i = 0; i < pending_.size();) {
        if (pending_[i].due > frame_) {
            ++i;
            continue;
        }
        Pending due = std::move(pending_[i]);
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
        if (due.control) {
            for (const std::string& line : due.lines) {
                write("ctl", {line});
            }
            continue;
        }
        char seq[16];
        std::snprintf(seq, sizeof seq, "%04u", shotSeq_++);
        std::string safe;
        for (const char c : due.name) {
            safe += (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' || c == '-') ? c : '_';
        }
        const std::string file = std::string(seq) + "_" + safe + ".bmp";
        write("shot", {clean(due.name), file});
        std::error_code ignored;
        std::filesystem::create_directories(shotDir_, ignored);
        shotsOut_.push_back(shotDir_ / file);
    }
    ++frame_;
    if (frame_ % 120 == 0) {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        out_.flush();
    }
}

void StateTrace::trackGui(const StateTraceFrame& frame)
{
    std::vector<Panel> now;
    if (frame.game != nullptr && !frame.hudLayout.empty()) {
        // Retail lists the in-game HUD as the one manager entry mipc210x7 (its sub-panels are controls of it).
        Panel hud;
        hud.name = "mipc210x7";
        hud.screen = frame.hudGui != nullptr ? frame.hudGui->screen() : nullptr;
        hud.x = hud.screen != nullptr ? hud.screen->extent.left : 0;
        hud.y = hud.screen != nullptr ? hud.screen->extent.top : 0;
        hud.w = hud.screen != nullptr ? hud.screen->extent.width : frame.windowWidth;
        hud.h = hud.screen != nullptr ? hud.screen->extent.height : frame.windowHeight;
        now.push_back(hud);
    }
    if (frame.game != nullptr && frame.game->sim_->fade.active() && inPlay_) {
        Panel fade;
        fade.name = "fade";
        fade.w = frame.windowWidth;
        fade.h = frame.windowHeight;
        now.push_back(fade);
    }
    if (frame.menuGui != nullptr && frame.menuGui->active()) {
        const auto add = [&](const kotor::GuiScreen* screen, int mode) {
            if (screen == nullptr) {
                return;
            }
            Panel panel;
            panel.name = panelName(screen->name);
            panel.mode = mode;
            panel.x = screen->extent.left;
            panel.y = screen->extent.top;
            panel.w = screen->extent.width;
            panel.h = screen->extent.height;
            panel.screen = screen;
            now.push_back(panel);
        };
        for (std::size_t i = 0; i < frame.menuGui->underlayCount(); ++i) {
            add(frame.menuGui->underlayScreen(i), 0);
        }
        add(frame.menuGui->screen(), frame.menuGui->modalActive() ? 1 : 0);
    }
    if (frame.game != nullptr && !backdrop_ && frame.game->dialogue().active && frame.dialogueGui != nullptr
        && frame.dialogueGui->screen() != nullptr) {
        const kotor::GuiScreen* screen = frame.dialogueGui->screen();
        Panel panel;
        panel.name = panelName(screen->name);
        panel.x = screen->extent.left;
        panel.y = screen->extent.top;
        panel.w = screen->extent.width;
        panel.h = screen->extent.height;
        panel.screen = screen;
        now.push_back(panel);
    }

    // Opens and closes by name (a name may appear once).
    const auto has = [](const std::vector<Panel>& list, const std::string& name) {
        return std::any_of(list.begin(), list.end(), [&](const Panel& p) { return p.name == name; });
    };
    for (const Panel& old : panels_) {
        if (!has(now, old.name)) {
            write("gui", {"close", clean(old.name), "-", "-", "-", "-", "-"});
        }
    }
    for (const Panel& panel : now) {
        if (has(panels_, panel.name)) {
            continue;
        }
        write("gui", {"open", clean(panel.name), std::to_string(panel.mode), std::to_string(panel.x),
                      std::to_string(panel.y), std::to_string(panel.w), std::to_string(panel.h)});
        if (panel.screen != nullptr) {
            Pending controls;
            controls.due = frame_ + kControlFrames;
            controls.control = true;
            std::vector<const kotor::GuiControl*> flat;
            for (const kotor::GuiControl& control : panel.screen->controls) {
                flatten(control, flat);
            }
            for (const kotor::GuiControl* control : flat) {
                // The control's tag and strref are not recoverable from the retail runtime control (TRACE_SPEC 4.8),
                // so both columns are `-` here as well.
                controls.lines.push_back(clean(panel.name) + "	" + std::to_string(control->id) + "	"
                                         + std::to_string(control->extent.left) + "	" + std::to_string(control->extent.top)
                                         + "	" + std::to_string(control->extent.width) + "	"
                                         + std::to_string(control->extent.height) + "	-	-");
            }
            pending_.push_back(std::move(controls));
        }
        schedule(kPanelFrames, "panel:" + panel.name);
    }
    panels_ = now;
    std::string stack;
    for (const Panel& panel : now) {
        stack += (stack.empty() ? "" : ",") + panel.name;
    }
    state("gui_stack", "", {clean(stack)});
}

// TRACE_SPEC 4.10: a record identical in (op, kind, resref, position) to one written in the last 1000 ms of wall time
// is dropped; ambient 3D emitters restart many times a second and the trace is a record of distinct starts and stops.
void StateTrace::writeSound(const std::vector<std::string>& fields)
{
    const std::uint64_t now = SDL_GetTicks();
    std::string key;
    for (std::size_t i = 0; i < 6 && i < fields.size(); ++i) {
        key += fields[i];
        key += '|';
    }
    if (const auto found = soundWritten_.find(key); found != soundWritten_.end() && now - found->second < 1000u) {
        return;
    }
    if (soundWritten_.size() > 4096) {
        for (auto it = soundWritten_.begin(); it != soundWritten_.end();) {
            it = now - it->second >= 1000u ? soundWritten_.erase(it) : std::next(it);
        }
    }
    soundWritten_[key] = now;
    write("snd", fields);
}

void StateTrace::trackSounds()
{
    if (game_ == nullptr) {
        return;
    }
    const std::lock_guard<std::recursive_mutex> lock(mutex_);
    const audio::Mixer& mixer = game_->audio().mixer();
    for (std::size_t i = 0; i < voices_.size();) {
        if (mixer.voiceActive(voices_[i].id)) {
            ++i;
            continue;
        }
        std::vector<std::string> fields{"stop"};
        fields.insert(fields.end(), voices_[i].tail.begin(), voices_[i].tail.end());
        writeSound(fields);
        voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// tag#n: n counts the objects met with the same tag and template since the module became ready (TRACE_SPEC 3).
std::string StateTrace::keyFor(GameSession& g, std::size_t index, std::uint32_t& idOut)
{
    if (index >= g.scene_.entities.size()) {
        idOut = 0;
        return "none";
    }
    const kotor::Entity& entity = g.scene_.entities[index];
    std::uint32_t id = g.objectOfBody(index);
    if (id == script::kObjectInvalid || id == 0) {
        id = 0x80000000u | static_cast<std::uint32_t>(index);
    }
    idOut = id;
    if (const auto found = keyById_.find(id); found != keyById_.end()) {
        return found->second;
    }
    std::string tag = entity.tag == "PC" ? std::string("PC") : lowered(entity.tag);
    if (tag.empty()) {
        tag = "-";
    }
    const std::string templateName = entity.tag == "PC" || entity.templateResref.empty()
        ? std::string("-") : lowered(entity.templateResref);
    std::string key = tag + "#" + std::to_string(++seen_[tag + "|" + templateName]);
    keyById_[id] = key;
    return key;
}

std::string StateTrace::keyOfObject(GameSession& g, std::uint32_t object)
{
    if (object == script::kObjectInvalid || object == 0) {
        return "none";
    }
    const std::size_t index = g.bodyOfObject(object);
    if (index >= g.scene_.entities.size()) {
        return "none";
    }
    std::uint32_t ignored = 0;
    return keyFor(g, index, ignored);
}

std::string StateTrace::keyOf(std::size_t entity) const
{
    return entity < entityKey_.size() && !entityKey_[entity].empty() ? entityKey_[entity] : std::string("none");
}

void StateTrace::sampleState(const StateTraceFrame& frame)
{
    GameSession& g = *frame.game;
    const kotor::Scene& scene = g.scene_;

    state("area", "", {clean(lowered(scene.areaTag))});

    const bool scriptPause = g.scriptPauseActive() || g.dialoguePaused();
    const std::uint32_t bits = (frame.userPause ? 1u : 0u) | ((scriptPause || frame.uiPause) ? 2u : 0u);
    state("pause", "", {bits != 0 ? "1" : "0", hex(bits)});

    // ---- objects ----
    std::vector<const kotor::EntityMesh*> bodyMesh(scene.entities.size(), nullptr);
    for (const kotor::EntityMesh& mesh : scene.entityMeshes) {
        if (mesh.entity < bodyMesh.size() && bodyMesh[mesh.entity] == nullptr) {
            bodyMesh[mesh.entity] = &mesh;
        }
    }
    entityKey_.assign(scene.entities.size(), std::string());
    std::unordered_map<std::string, std::string> present;
    for (std::size_t i = 0; i < scene.entities.size(); ++i) {
        const kotor::Entity& entity = scene.entities[i];
        const bool creature = entity.kind == kotor::EntityKind::Creature;
        const bool placeable = entity.kind == kotor::EntityKind::Placeable;
        const bool door = entity.kind == kotor::EntityKind::Door;
        if (!creature && !placeable && !door) {
            continue;
        }
        std::uint32_t id = 0;
        const std::string key = keyFor(g, i, id);
        entityKey_[i] = key;
        const std::string idText = hex(id); // the state identity: two objects can share tag#n (other templates)
        present[idText] = key;

        // Orientation: the vector the retail object holds at +0x9c. A creature's bearing is the angle of its
        // XOrientation/YOrientation (Scene.cpp, SaveGame.cpp objectBearing), a door's or placeable's is the GIT
        // Bearing, which retail holds as (-sin B, cos B). TRACE_SPEC 4.5.
        const float b = entity.bearing;
        const float ox = creature ? std::cos(b) : -std::sin(b);
        const float oy = creature ? std::sin(b) : std::cos(b);
        const std::int32_t hp = i == g.playerEntity_ ? g.playerHp_ : entity.hitPoints;
        const kotor::EntityMesh* mesh = bodyMesh[i];
        std::string clip = "-";
        if (mesh != nullptr && !mesh->animation.empty()) {
            clip = mesh->animation + "@" + f3(mesh->animationTime);
        }
        const std::string templateName = entity.tag == "PC" || entity.templateResref.empty()
            ? std::string("-") : lowered(entity.templateResref);
        state("obj", idText,
              {key, idText, creature ? "creature" : (placeable ? "placeable" : "door"), templateName,
               f3(entity.position.x), f3(entity.position.y), f3(entity.position.z), f3(ox), f3(oy),
               std::to_string(hp), mesh == nullptr ? "-" : (mesh->visible ? "1" : "0"),
               creature ? std::to_string(entity.appearance) : "-", "-", clip});
        // Ground truth next to the mirror: the raw bearing in radians, once per object.
        state("chk", idText, {"bearing", key, f3(b)});
        if (mesh != nullptr && !mesh->animation.empty()) {
            state("clip", idText, {key, mesh->animation, "1.000"});
        }
    }
    for (auto it = objects_.begin(); it != objects_.end();) {
        if (present.find(it->first) == present.end()) {
            write("obj_del", {it->second, it->first});
            last_.erase("obj\n" + it->first);
            last_.erase("clip\n" + it->first);
            last_.erase("chk\n" + it->first);
            it = objects_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [id, key] : present) {
        objects_[id] = key;
    }

    // ---- party: the PC first, then the companions in join order ----
    const auto keyOfTag = [&](const std::string& wanted) -> std::string {
        const std::string tag = lowered(wanted);
        if (tag == "pc") {
            return "PC#1";
        }
        for (std::size_t i = 0; i < scene.entities.size(); ++i) {
            if (!entityKey_[i].empty() && lowered(scene.entities[i].tag) == tag) {
                return entityKey_[i];
            }
        }
        return tag + "#1";
    };
    std::string members = keyOfTag("PC");
    for (const std::string& tag : g.partyTags_) {
        members += "," + keyOfTag(tag);
    }
    state("party", "", {keyOfTag(g.leaderTag_), clean(members)});

    // ---- camera ----
    const SceneCamera camera = g.camera();
    state("cam", "", {f3(camera.eye.x), f3(camera.eye.y), f3(camera.eye.z), f3(camera.forward.x),
                      f3(camera.forward.y), f3(camera.forward.z), f3(camera.up.x), f3(camera.up.y),
                      f3(camera.up.z), f3(camera.fovYDegrees), f3(camera.nearClip), f3(camera.farClip)});
    // The module camera follows; a conversation takes it over (the stunt camera's model is `animated`) and
    // gives it back with `restore`.
    const bool talking = g.dialogueView_.active && !g.dialogueView_.computer;
    const bool stunt = talking && g.dialogueCameraModel_ && !g.dialogueCameraClip_.empty();
    std::string mode = "follow";
    if (stunt) {
        mode = "animated";
    } else if (talking) {
        mode = "dialog";
    } else if (camMode_ == "dialog" || camMode_ == "animated" || camMode_ == "restore") {
        mode = "restore";
    }
    camMode_ = mode;
    state("camm", "", {mode});

    // ---- selection ----
    const std::uint32_t selected = g.selectedEntity_;
    state("sel", "", {selected < entityKey_.size() && !entityKey_[selected].empty() ? entityKey_[selected] : "none",
                      "none", "0.000", "0.000", "0.000"});
}

// The dialogue camera as retail logs it (006bd260 shot entry, 006bb7b0 eye): one `shot` per line and an `eye`
// whenever the (angle, side) pair changes. Same choice of framed and other creature as GameSession::camera().
void StateTrace::trackDialogueCamera(GameSession& g)
{
    const DialogueView& view = g.dialogueView_;
    if (!view.active || view.computer || !g.activeDialogue_.has_value()) {
        dlgcamLast_.clear();
        return;
    }
    const bool listShot = view.shotOnListener && !g.dialogueLineSpeaking();
    const std::int32_t angle = listShot && view.replyCameraAngle > 0 ? view.replyCameraAngle : view.cameraAngle;
    if (angle < 1 || angle > 3 || (g.dialogueCameraModel_ && !g.dialogueCameraClip_.empty())) {
        return;
    }
    const auto entityOr = [&](std::uint32_t object, std::size_t fallback) {
        const std::size_t named = g.entityOfObject(object);
        return named != static_cast<std::size_t>(-1) ? named : fallback;
    };
    const std::size_t led = g.controlledEntity();
    const std::size_t listenerEntity = entityOr(view.listenerObject, led);
    const std::size_t speakerEntity = entityOr(view.speakerObject, listenerEntity);
    if (speakerEntity >= g.scene_.entities.size() || listenerEntity >= g.scene_.entities.size()) {
        return;
    }
    const std::size_t framed = listShot ? listenerEntity : speakerEntity;
    const std::size_t other = listShot ? speakerEntity : listenerEntity;
    const int side = framed == led ? 1 : (other == led ? 2 : (framed < other ? 2 : 1));
    if (dlgcamDirty_) {
        dlgcamDirty_ = false;
        write("dlgcam", {"shot", keyOf(speakerEntity), speakerEntity == listenerEntity ? std::string("none") : keyOf(listenerEntity),
                         std::to_string(angle), f3(view.camHeightOffset), f3(view.tarHeightOffset), "0"});
    }
    const SceneCamera camera = g.camera();
    // Retail writes an `eye` whenever the eye position changes (TRACE_SPEC 4.9), so the pose includes it.
    const std::string pose = std::to_string(angle) + ":" + std::to_string(side) + ":" + f3(camera.eye.x) + ":"
        + f3(camera.eye.y) + ":" + f3(camera.eye.z);
    if (pose != dlgcamLast_) {
        dlgcamLast_ = pose;
        write("dlgcam", {"eye", std::to_string(angle), std::to_string(side), f3(camera.eye.x), f3(camera.eye.y),
                         f3(camera.eye.z)});
    }
}

} // namespace ke
