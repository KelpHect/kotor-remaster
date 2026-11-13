#include "game/UserSettings.h"

#include <cstdlib>
#include <fstream>

namespace ke {

const std::map<std::string, float>& UserSettings::defaults()
{
    static const std::map<std::string, float> table = {
        {"gameplay.invertcamera", 0.0f}, {"gameplay.autolevelup", 0.0f}, {"gameplay.autosave", 1.0f},
        {"gameplay.reversecamera", 0.0f}, {"gameplay.disablemouse", 1.0f}, {"gameplay.difficulty", 1.0f},
        {"sound.music", 80.0f}, {"sound.voice", 100.0f}, {"sound.effects", 100.0f}, {"sound.movie", 100.0f},
        {"sound.forcesoftware", 0.0f},
        {"graphics.gamma", 50.0f}, {"graphics.shadows", 1.0f}, {"graphics.grass", 1.0f},
        {"graphics.framebuffer", 1.0f}, {"graphics.vsync", 1.0f}, {"graphics.softshadows", 0.0f},
        {"autopause.enemysighted", 1.0f}, {"autopause.partykilled", 1.0f}, {"autopause.actionmenu", 0.0f},
        {"autopause.endround", 0.0f}, {"autopause.triggers", 1.0f}, {"autopause.minesighted", 1.0f},
        {"mouse.sensitivity", 50.0f}, {"mouse.reversebuttons", 0.0f},
        {"feedback.hideunequippable", 0.0f}, {"feedback.tutorialpopups", 1.0f}, {"feedback.subtitles", 1.0f},
        {"feedback.minimap", 1.0f}, {"feedback.floatingnumbers", 1.0f}, {"feedback.statussummary", 1.0f},
        {"feedback.hidequickmenu", 0.0f}, {"feedback.smallfont", 0.0f}, {"feedback.tooltips", 1.0f},
    };
    return table;
}

UserSettings::UserSettings()
    : values_(defaults())
{
}

float UserSettings::get(const std::string& key) const
{
    const auto found = values_.find(key);
    return found != values_.end() ? found->second : 0.0f;
}

void UserSettings::set(const std::string& key, float value)
{
    float& slot = values_[key];
    if (slot != value) {
        slot = value;
        dirty = true;
    }
}

void UserSettings::resetGroup(const std::string& group)
{
    for (const auto& [key, value] : defaults()) {
        if (key.rfind(group, 0) == 0) {
            set(key, value);
        }
    }
}

bool UserSettings::load(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "movie") {
            seenMovies.insert(value);
        } else {
            values_[key] = std::strtof(value.c_str(), nullptr);
        }
    }
    dirty = false;
    return true;
}

bool UserSettings::save(const std::filesystem::path& path) const
{
    std::error_code ignored;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ignored);
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file) {
            return false;
        }
        for (const auto& [key, value] : values_) {
            file << key << '=' << value << '\n';
        }
        for (const std::string& movie : seenMovies) {
            file << "movie=" << movie << '\n';
        }
        file.flush();
        if (!file) {
            file.close();
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ignored);
    if (ignored) {
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

} // namespace ke
