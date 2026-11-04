#pragma once

#include "kotor/Formats.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ke::kotor {

struct LanguageInfo {
    std::uint32_t id = 0;
    const char* name = "English";
};

const std::vector<LanguageInfo>& supportedLanguages();
const char* languageName(std::uint32_t id);
int languageId(const std::string& nameOrId); // -1 when unknown

// KotOR 1 localization: TLK language selection and the localized voice/LIP
// path conventions. The install's dialog.tlk is the language of record; a
// requested language must match it (or a dialog_<id>.tlk override).
class Localization {
public:
    explicit Localization(std::filesystem::path root);

    // requested < 0 means "use the TLK's own language". On failure the
    // previous language (if any) is left unchanged.
    bool load(int requestedLanguage, std::string& error);
    bool loaded() const { return tlk_.has_value(); }

    const Tlk& tlk() const { return *tlk_; }
    std::uint32_t languageId() const;
    std::string languageName() const;
    std::size_t entryCount() const;

    // Resolve a voice resref through the localized streamwaves layout:
    //   streamwaves/<module>/<speaker>/<resref>.wav (n<module><speaker>...),
    // then streamwaves/<resref>.wav and streamsounds/<resref>.wav.
    std::filesystem::path voiceFile(const std::string& resref) const;

    // lips/<module>_loc.mod plus lips/localization.mod.
    std::size_t localizedLipArchives() const;

private:
    std::filesystem::path root_;
    std::optional<Tlk> tlk_;
};

} // namespace ke::kotor
