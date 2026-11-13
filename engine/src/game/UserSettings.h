#pragma once

// User options (swkotor.ini in retail). Keys are the engine's own names. The same file is read by the
// engine and edited by the launcher, so this class lives in the data library, apart from the front end.

#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace ke {

class UserSettings {
public:
    UserSettings();
    float get(const std::string& key) const;
    void set(const std::string& key, float value);
    void resetGroup(const std::string& group);
    bool load(const std::filesystem::path& path);
    // Writes a temporary file beside `path` and renames it over, so a crash never leaves a cut-off file.
    bool save(const std::filesystem::path& path) const;
    // The shipped default of every known key.
    static const std::map<std::string, float>& defaults();
    std::set<std::string> seenMovies;
    bool dirty = false;

private:
    std::map<std::string, float> values_;
};

} // namespace ke
