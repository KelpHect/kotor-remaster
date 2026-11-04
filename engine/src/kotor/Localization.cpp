#include "kotor/Localization.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

namespace ke::kotor {

namespace {

std::string lower(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return {};
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return {};
    }
    return bytes;
}

bool isDigits(const std::string& text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

} // namespace

const std::vector<LanguageInfo>& supportedLanguages()
{
    static const std::vector<LanguageInfo> languages = {
        {0, "English"}, {1, "French"}, {2, "German"}, {3, "Italian"},
        {4, "Spanish"}, {5, "Polish"}, {6, "Russian"}, {7, "Czech"},
        {8, "Hungarian"}, {9, "Portuguese"}, {10, "Chinese"},
        {11, "Japanese"}, {12, "Korean"},
    };
    return languages;
}

const char* languageName(std::uint32_t id)
{
    for (const LanguageInfo& language : supportedLanguages()) {
        if (language.id == id) {
            return language.name;
        }
    }
    return "Unknown";
}

int languageId(const std::string& nameOrId)
{
    if (isDigits(nameOrId)) {
        const long value = std::strtol(nameOrId.c_str(), nullptr, 10);
        for (const LanguageInfo& language : supportedLanguages()) {
            if (language.id == static_cast<std::uint32_t>(value)) {
                return static_cast<int>(language.id);
            }
        }
        return -1;
    }
    const std::string wanted = lower(nameOrId);
    for (const LanguageInfo& language : supportedLanguages()) {
        if (lower(language.name) == wanted) {
            return static_cast<int>(language.id);
        }
    }
    return -1;
}

Localization::Localization(std::filesystem::path root)
    : root_(std::move(root))
{
}

bool Localization::load(int requestedLanguage, std::string& error)
{
    error.clear();
    std::vector<std::filesystem::path> candidates;
    if (requestedLanguage >= 0) {
        const std::string id = std::to_string(requestedLanguage);
        candidates.push_back(root_ / ("dialog_" + id + ".tlk"));
        candidates.push_back(root_ / ("dialog_" + lower(ke::kotor::languageName(
            static_cast<std::uint32_t>(requestedLanguage))) + ".tlk"));
    }
    candidates.push_back(root_ / "dialog.tlk");

    std::optional<Tlk> loaded;
    std::string chosen;
    for (const std::filesystem::path& path : candidates) {
        std::vector<std::uint8_t> bytes = readFile(path);
        if (bytes.empty()) {
            continue;
        }
        try {
            loaded = Tlk::parse(std::move(bytes), path.filename().string());
            chosen = path.filename().string();
            break;
        } catch (const FormatError& exception) {
            error = exception.what();
            return false;
        }
    }
    if (!loaded.has_value()) {
        error = "localization: no dialog.tlk in " + root_.string();
        return false;
    }
    if (requestedLanguage >= 0
        && loaded->language() != static_cast<std::uint32_t>(requestedLanguage)) {
        error = "localization: requested " + std::string(ke::kotor::languageName(
                    static_cast<std::uint32_t>(requestedLanguage)))
            + " (" + std::to_string(requestedLanguage) + ") but "
            + chosen + " is " + ke::kotor::languageName(loaded->language())
            + " (" + std::to_string(loaded->language()) + ")";
        return false;
    }
    tlk_ = std::move(loaded);
    return true;
}

bool Localization::load(Install& install, int requestedLanguage,
                        std::string& error)
{
    error.clear();
    const auto readCandidate = [&](const std::string& stem,
                                   std::string& chosen) -> std::vector<std::uint8_t> {
        const std::string file = stem + ".tlk";
        const std::filesystem::path paths[] = {
            install.overlayRoot() / file,
            install.root() / file,
        };
        for (const std::filesystem::path& path : paths) {
            std::vector<std::uint8_t> bytes = readFile(path);
            if (!bytes.empty()) {
                chosen = path.filename().string();
                return bytes;
            }
        }
        if (auto data = install.find(stem, 2018); data.has_value()) {
            chosen = stem + ".tlk";
            return std::move(*data);
        }
        return {};
    };
    std::vector<std::string> stems;
    if (requestedLanguage >= 0) {
        stems.push_back("dialog_" + std::to_string(requestedLanguage));
        stems.push_back("dialog_" + lower(ke::kotor::languageName(
            static_cast<std::uint32_t>(requestedLanguage))));
    }
    stems.push_back("dialog");
    std::optional<Tlk> loaded;
    std::string chosen;
    for (const std::string& stem : stems) {
        std::vector<std::uint8_t> bytes = readCandidate(stem, chosen);
        if (bytes.empty()) {
            continue;
        }
        try {
            loaded = Tlk::parse(std::move(bytes), chosen);
            break;
        } catch (const FormatError& exception) {
            error = exception.what();
            return false;
        }
    }
    if (!loaded.has_value()) {
        error = "localization: no dialog TLK in " + install.root().string();
        return false;
    }
    if (requestedLanguage >= 0
        && loaded->language() != static_cast<std::uint32_t>(requestedLanguage)) {
        error = "localization: requested " + std::string(ke::kotor::languageName(
                    static_cast<std::uint32_t>(requestedLanguage)))
            + " (" + std::to_string(requestedLanguage) + ") but "
            + chosen + " is "
            + ke::kotor::languageName(loaded->language())
            + " (" + std::to_string(loaded->language()) + ")";
        return false;
    }
    const auto applyPatch = [&](const char* resref, std::size_t replaceCount) {
        const auto data = install.find(resref, 2018);
        if (!data.has_value()) {
            return true;
        }
        try {
            const Tlk patch = Tlk::parse(*data, std::string(resref) + ".tlk");
            *loaded = Tlk::merge(*loaded, patch, std::string(resref) + "_merged.tlk",
                                 replaceCount);
            return true;
        } catch (const FormatError& exception) {
            error = exception.what();
            return false;
        }
    };
    if (!applyPatch("replace", static_cast<std::size_t>(-1))
        || !applyPatch("append", 0)) {
        return false;
    }
    tlk_ = std::move(loaded);
    return true;
}

std::uint32_t Localization::languageId() const
{
    return tlk_.has_value() ? tlk_->language() : 0;
}

std::string Localization::languageName() const
{
    return tlk_.has_value()
        ? std::string(ke::kotor::languageName(tlk_->language())) : std::string{};
}

std::size_t Localization::entryCount() const
{
    return tlk_.has_value() ? tlk_->size() : 0;
}

std::filesystem::path Localization::voiceFile(const std::string& resref) const
{
    const std::string key = lower(resref);
    if (key.empty() || key.find('/') != std::string::npos
        || key.find('\\') != std::string::npos) {
        return {};
    }
    std::vector<std::filesystem::path> candidates;
    if (key.size() >= 12 && key[0] == 'n') {
        const std::string module = key.substr(1, 5);
        const std::string speaker = key.substr(6, 6);
        candidates.push_back(root_ / "streamwaves" / module / speaker / (key + ".wav"));
    }
    candidates.push_back(root_ / "streamwaves" / (key + ".wav"));
    candidates.push_back(root_ / "streamsounds" / (key + ".wav"));
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    return {};
}

std::size_t Localization::localizedLipArchives() const
{
    std::error_code error;
    const std::filesystem::path lips = root_ / "lips";
    if (!std::filesystem::is_directory(lips, error)) {
        return 0;
    }
    std::size_t count = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(lips, error)) {
        if (error || !entry.is_regular_file()) {
            continue;
        }
        const std::string name = lower(entry.path().filename().string());
        if (name == "localization.mod"
            || (name.size() > 8 && name.substr(name.size() - 8) == "_loc.mod")) {
            ++count;
        }
    }
    return count;
}

} // namespace ke::kotor
