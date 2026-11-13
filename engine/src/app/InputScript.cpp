#include "app/InputScript.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ke {

namespace {

std::string pad(std::size_t value, int width)
{
    std::string text = std::to_string(value);
    while (static_cast<int>(text.size()) < width) {
        text.insert(text.begin(), '0');
    }
    return text;
}

bool isWalkVerb(const std::string& op)
{
    static const char* const verbs[] = {"goto", "use", "talk", "dialogue", "autodialogue", "fight", "module",
                                        "expect", "idle", "partyselect", "note", "list", "loot", "equip", "leader", "unlock"};
    for (const char* verb : verbs) {
        if (op == verb) {
            return true;
        }
    }
    return false;
}

} // namespace

bool InputScript::hasWalkSteps() const
{
    return std::any_of(steps_.begin(), steps_.end(), [](const Step& step) { return step.kind == Step::Kind::Walk; });
}

bool InputScript::load(const std::string& path, const std::string& outDir)
{
    steps_.clear();
    next_ = 0;
    finished_ = false;
    outDir_ = outDir.empty() ? std::string(".") : outDir;
    std::ifstream in(path);
    if (!in) {
        error_ = "cannot read " + path;
        return false;
    }
    std::uint64_t clock = 0;
    std::uint64_t every = 0;
    std::uint64_t lastAuto = 0;
    std::string section = "walk";
    std::size_t shotIndex = 0;
    const auto shotPath = [&](const std::string& name) {
        return outDir_ + "/" + pad(shotIndex++, 4) + "_" + section + "_" + name + ".bmp";
    };
    // Advancing the clock drops the periodic frames that fall inside the stretch.
    const auto advanceClock = [&](std::uint64_t ms) {
        const std::uint64_t until = clock + ms;
        if (every > 0) {
            for (std::uint64_t t = lastAuto + every; t <= until; t += every) {
                Step shot;
                shot.atMs = t;
                shot.kind = Step::Kind::Shot;
                shot.name = shotPath("t" + pad(static_cast<std::size_t>(t), 7));
                steps_.push_back(shot);
                lastAuto = t;
            }
        }
        clock = until;
    };
    std::string line;
    int lineNumber = 0;
    int groups = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (const std::size_t hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        std::istringstream words(line);
        std::string op;
        if (!(words >> op)) {
            continue;
        }
        const auto bad = [&]() {
            error_ = path + ":" + std::to_string(lineNumber) + ": cannot read '" + line + "'";
            return false;
        };
        Step step;
        step.atMs = clock;
        if (op == "if") {
            if (!(words >> step.onlyIf)) {
                return bad();
            }
            if (step.onlyIf == "gui" || step.onlyIf == "panel" || step.onlyIf == "entry") {
                std::string argument;
                if (!(words >> argument)) {
                    return bad();
                }
                step.onlyIf += " " + argument;
            }
            if (!(words >> op)) {
                return bad();
            }
            step.group = ++groups;
        }
        if (op == "section") {
            if (!(words >> section)) {
                return bad();
            }
        } else if (op == "every") {
            if (!(words >> every)) {
                return bad();
            }
            lastAuto = clock;
        } else if (op == "wait") {
            std::uint64_t ms = 0;
            if (!(words >> ms)) {
                return bad();
            }
            advanceClock(ms);
        } else if (op == "move" || op == "click") {
            if (!(words >> step.x >> step.y)) {
                return bad();
            }
            std::string which;
            words >> which;
            step.button = which == "right" ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
            step.kind = Step::Kind::Move;
            steps_.push_back(step);
            if (op == "click") {
                // Down and up on separate frames, as a hand would: 50 ms apart.
                step.kind = Step::Kind::Down;
                steps_.push_back(step);
                step.kind = Step::Kind::Up;
                step.atMs = clock + 50;
                steps_.push_back(step);
            }
        } else if (op == "key" || op == "hold") {
            std::string name;
            std::uint64_t ms = 50;
            if (!(words >> name) || (op == "hold" && !(words >> ms))) {
                return bad();
            }
            step.scancode = SDL_GetScancodeFromName(name.c_str());
            if (step.scancode == SDL_SCANCODE_UNKNOWN) {
                return bad();
            }
            step.kind = Step::Kind::KeyDown;
            steps_.push_back(step);
            if (op == "hold") {
                advanceClock(ms);
            }
            step.kind = Step::Kind::KeyUp;
            step.atMs = op == "hold" ? clock : clock + 50;
            steps_.push_back(step);
        } else if (op == "shot") {
            std::string name;
            if (!(words >> name)) {
                return bad();
            }
            step.kind = Step::Kind::Shot;
            step.name = shotPath(name);
            steps_.push_back(step);
        } else if (op == "shotas") {
            std::string name;
            if (!(words >> name)) {
                return bad();
            }
            step.kind = Step::Kind::Shot;
            step.name = outDir_ + "/" + name + ".bmp";
            steps_.push_back(step);
        } else if (op == "script") {
            if (!(words >> step.name)) {
                return bad();
            }
            step.kind = Step::Kind::Script;
            steps_.push_back(step);
        } else if (op == "camera") {
            std::string first;
            if (!(words >> first)) {
                return bad();
            }
            step.kind = Step::Kind::Camera;
            if (first == "off") {
                step.camera[9] = 0.0f;
            } else {
                try {
                    step.camera[0] = std::stof(first);
                } catch (const std::exception&) {
                    return bad();
                }
                for (std::size_t i = 1; i < step.camera.size(); ++i) {
                    if (!(words >> step.camera[i])) {
                        return bad();
                    }
                }
            }
            steps_.push_back(step);
        } else if (op == "waitfor") {
            // The rest of the line is the condition; a trailing number is the timeout.
            std::vector<std::string> parts;
            for (std::string word; words >> word;) {
                parts.push_back(word);
            }
            if (parts.empty()) {
                return bad();
            }
            if (parts.size() >= 2 && parts.back().find_first_not_of("0123456789") == std::string::npos
                && (parts[0] != "entry" || parts.size() >= 3)) {
                step.timeoutMs = std::stoull(parts.back());
                parts.pop_back();
            }
            for (const std::string& part : parts) {
                step.name += (step.name.empty() ? "" : " ") + part;
            }
            // After every step already due (a key's release 50 ms on), so nothing is held through it.
            for (const Step& earlier : steps_) {
                step.atMs = std::max(step.atMs, earlier.atMs);
            }
            clock = step.atMs;
            step.kind = Step::Kind::WaitFor;
            steps_.push_back(step);
        } else if (isWalkVerb(op)) {
            // The whole line (comment removed) goes to the walk driver; like waitfor, the step comes after
            // everything already due, and the script holds until it reports done.
            const std::size_t first = line.find_first_not_of(" \t\r");
            const std::size_t last = line.find_last_not_of(" \t\r");
            step.name = line.substr(first, last - first + 1);
            for (const Step& earlier : steps_) {
                step.atMs = std::max(step.atMs, earlier.atMs);
            }
            clock = step.atMs;
            step.kind = Step::Kind::Walk;
            steps_.push_back(step);
        } else if (op == "end") {
            step.kind = Step::Kind::End;
            steps_.push_back(step);
            break;
        } else {
            return bad();
        }
    }
    if (steps_.empty() || steps_.back().kind != Step::Kind::End) {
        Step end;
        end.atMs = clock;
        end.kind = Step::Kind::End;
        steps_.push_back(end);
    }
    std::stable_sort(steps_.begin(), steps_.end(),
                     [](const Step& a, const Step& b) { return a.atMs < b.atMs; });
    return true;
}

std::vector<std::string> InputScript::advance(std::uint64_t nowMs, SDL_Window* window, const Condition& condition,
                                              const WalkHook& walk)
{
    std::vector<std::string> shots;
    const SDL_WindowID windowId = window != nullptr ? SDL_GetWindowID(window) : 0;
    while (next_ < steps_.size() && steps_[next_].atMs + offsetMs_ <= nowMs && !finished_) {
        const Step& step = steps_[next_];
        if (step.kind == Step::Kind::WaitFor) {
            if (!waiting_) {
                waiting_ = true;
                waitStartMs_ = nowMs;
            }
            const bool met = condition && condition(step.name);
            const bool timedOut = !met && nowMs - waitStartMs_ >= step.timeoutMs;
            if (!met && !timedOut) {
                return shots; // asked again next frame
            }
            waiting_ = false;
            waitResult_ = {step.name, nowMs - waitStartMs_, timedOut, true};
            // Later steps keep their spacing from this point.
            offsetMs_ = nowMs - step.atMs;
            ++next_;
            continue;
        }
        if (step.kind == Step::Kind::Walk) {
            const bool first = !waiting_;
            if (first) {
                waiting_ = true;
                waitStartMs_ = nowMs;
            }
            const int result = walk ? walk(step.name, first) : -1;
            if (result == 0) {
                return shots; // asked again next frame
            }
            waiting_ = false;
            offsetMs_ = nowMs - step.atMs;
            ++next_;
            if (result < 0 && abortOnFail_) {
                finished_ = true;
            }
            continue;
        }
        if (step.group != 0) {
            if (step.group != group_) {
                group_ = step.group;
                groupRun_ = condition && condition(step.onlyIf);
            }
            if (!groupRun_) {
                ++next_;
                continue;
            }
        }
        ++next_;
        SDL_Event event{};
        switch (step.kind) {
        case Step::Kind::Move:
            event.type = SDL_EVENT_MOUSE_MOTION;
            event.motion.windowID = windowId;
            event.motion.which = 1;
            event.motion.x = step.x;
            event.motion.y = step.y;
            event.motion.xrel = step.x - pointerX_;
            event.motion.yrel = step.y - pointerY_;
            pointerX_ = step.x;
            pointerY_ = step.y;
            SDL_PushEvent(&event);
            break;
        case Step::Kind::Down:
        case Step::Kind::Up:
            event.type = step.kind == Step::Kind::Down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            event.button.windowID = windowId;
            event.button.which = 1;
            event.button.button = static_cast<Uint8>(step.button);
            event.button.down = step.kind == Step::Kind::Down;
            event.button.clicks = 1;
            event.button.x = step.x;
            event.button.y = step.y;
            SDL_PushEvent(&event);
            break;
        case Step::Kind::KeyDown:
        case Step::Kind::KeyUp:
            held_[step.scancode] = step.kind == Step::Kind::KeyDown;
            event.type = step.kind == Step::Kind::KeyDown ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID = windowId;
            event.key.which = 1;
            event.key.scancode = step.scancode;
            event.key.key = SDL_GetKeyFromScancode(step.scancode, SDL_KMOD_NONE, false);
            event.key.down = step.kind == Step::Kind::KeyDown;
            SDL_PushEvent(&event);
            break;
        case Step::Kind::Shot:
            shots.push_back(step.name);
            // The frame is taken after this one is drawn: steps due at the same time wait for the
            // next frame, so the shot shows the state before them (as the retail driver grabs first).
            return shots;
        case Step::Kind::Script:
            scripts_.push_back(step.name);
            break;
        case Step::Kind::Camera:
            camera_.on = step.camera[9] > 0.0f;
            for (int i = 0; i < 3; ++i) {
                camera_.eye[i] = step.camera[static_cast<std::size_t>(i)];
                camera_.forward[i] = step.camera[static_cast<std::size_t>(3 + i)];
                camera_.up[i] = step.camera[static_cast<std::size_t>(6 + i)];
            }
            camera_.fovy = step.camera[9];
            break;
        case Step::Kind::WaitFor:
        case Step::Kind::Walk:
            break;
        case Step::Kind::End:
            finished_ = true;
            break;
        }
    }
    return shots;
}

const bool* InputScript::keyboardState()
{
    int count = 0;
    const bool* real = SDL_GetKeyboardState(&count);
    for (int i = 0; i < SDL_SCANCODE_COUNT; ++i) {
        merged_[static_cast<std::size_t>(i)] = (i < count && real[i]) || held_[static_cast<std::size_t>(i)];
    }
    return merged_.data();
}

} // namespace ke
