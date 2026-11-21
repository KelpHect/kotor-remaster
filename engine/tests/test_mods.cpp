// Modding API v1 (docs/MODDING_API.md). Everything here runs against directories the test writes
// itself: no game install, no game data, no window.

#include "kotor/Formats.h"
#include "kotor/Mods.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace ke::kotor;

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    } else {
        std::cout << "ok: " << what << '\n';
    }
}

void write(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

fs::path scratch(const std::string& name)
{
    const fs::path root = fs::temp_directory_path() / "kotor_mods_test" / name;
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

std::string manifest(const std::string& id, const std::string& extra = {}, const std::string& version = "1.0")
{
    return "id = " + id + "\nversion = " + version + "\napi = 1\n" + extra;
}

std::string table(const std::string& rows)
{
    return "2DA V2.0\n\n   name value\n" + rows;
}

std::string order(const ModSet& set)
{
    std::string text;
    for (const Mod& mod : set.mods()) {
        text += (text.empty() ? "" : ",") + mod.manifest.id;
    }
    return text;
}

bool hasError(const ModSet& set, const std::string& mod, const std::string& fragment)
{
    return std::any_of(set.errors().begin(), set.errors().end(), [&](const ModProblem& problem) {
        return problem.mod == mod && problem.message.find(fragment) != std::string::npos;
    });
}

bool parseFails(const std::string& text, const std::string& fragment)
{
    try {
        (void)ModManifest::parse(text, "mod.ini");
    } catch (const FormatError& failure) {
        return std::string(failure.what()).find(fragment) != std::string::npos;
    }
    return false;
}

std::string text(const std::optional<std::vector<std::uint8_t>>& bytes)
{
    return bytes ? std::string(bytes->begin(), bytes->end()) : std::string("<missing>");
}

std::set<std::string> listing(const fs::path& root)
{
    std::set<std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        files.insert(entry.path().lexically_relative(root).generic_string() + ":"
                     + (entry.is_regular_file() ? std::to_string(entry.file_size()) : "dir"));
    }
    return files;
}

void testVersion()
{
    expect(ModVersion::parse("1.2.3") == ModVersion{1, 2, 3}, "version 1.2.3 parses");
    expect(ModVersion::parse("2") == ModVersion{2, 0, 0}, "version 2 parses as 2.0.0");
    expect(ModVersion{1, 10, 0} > ModVersion{1, 9, 9}, "versions compare by number, not text");
    for (const char* bad : {"", "1.", ".1", "1.2.3.4", "1.x", "v1", "-1"}) {
        expect(!ModVersion::parse(bad).has_value(), std::string("version rejected: '") + bad + "'");
    }
}

void testManifest()
{
    const ModManifest parsed = ModManifest::parse(
        "\xEF\xBB\xBF# comment\r\nid = hd-ui\r\nName = HD UI\r\nversion = 1.4\r\napi = 1\r\n"
        "game = KotOR1\r\nrequires = base-fixes >= 2.1, other\r\nafter = a, b\r\nconflicts = c\r\n"
        "future_key = kept for a later API\r\n",
        "mod.ini");
    expect(parsed.id == "hd-ui" && parsed.name == "HD UI" && parsed.api == 1, "manifest fields parse");
    expect(parsed.version == ModVersion{1, 4, 0} && parsed.game == "kotor1", "manifest version and game");
    expect(parsed.requires_.size() == 2 && parsed.requires_[0].id == "base-fixes"
               && parsed.requires_[0].minimum == ModVersion{2, 1, 0} && !parsed.requires_[1].minimum,
           "requires with and without a minimum version");
    expect(parsed.after.size() == 2 && parsed.conflicts.size() == 1, "after and conflicts lists");
    expect(ModManifest::parse(manifest("plain"), "mod.ini").name == "plain", "name defaults to the id");

    expect(parseFails("version = 1\napi = 1\n", "`id` is missing"), "missing id is rejected");
    expect(parseFails("id = a\napi = 1\n", "`version` is missing"), "missing version is rejected");
    expect(parseFails("id = a\nversion = 1\n", "`api` is missing"), "missing api is rejected");
    expect(parseFails("id = Bad Id\nversion = 1\napi = 1\n", "mod.ini:1:"), "bad id names its line");
    expect(parseFails("id = ../up\nversion = 1\napi = 1\n", "`id` must be"), "id cannot be a path");
    expect(parseFails("id = a\nid = b\n", "set twice"), "duplicate key is rejected");
    expect(parseFails("id = a\nnonsense\n", "mod.ini:2:"), "line without = is rejected");
    expect(parseFails(manifest("a", "game = kotor3\n"), "`game` must be"), "unknown game is rejected");
    expect(parseFails(manifest("a", "requires = b >= x\n"), "bad version"), "bad requires version is rejected");
    expect(parseFails("id = a\nversion = 1\napi = 1.5\n", "whole number"), "fractional api is rejected");
}

void testDiscovery()
{
    expect(ModSet::discover(scratch("none") / "absent", GameKind::KotOR1).ok(), "no mods directory is not an error");

    {
        const fs::path mods = scratch("order");
        write(mods / "zeta" / "mod.ini", manifest("zeta"));
        write(mods / "alpha" / "mod.ini", manifest("alpha", "requires = zeta\n", "2.5.1"));
        write(mods / "mid" / "mod.ini", manifest("mid", "after = zeta, not-installed\n"));
        write(mods / "_disabled" / "mod.ini", "not even a manifest");
        write(mods / "notes" / "readme.txt", "no manifest here");
        const ModSet set = ModSet::discover(mods, GameKind::KotOR1);
        expect(set.ok() && order(set) == "zeta,alpha,mid", "id order, moved only by requires/after: " + order(set));
        expect(set.fingerprint() == "zeta@1.0.0;alpha@2.5.1;mid@1.0.0", "fingerprint lists id@version in load order");
        expect(set.warnings().size() == 1 && set.warnings()[0].mod == "notes", "directory without mod.ini is a warning");
        expect(set.find("alpha") != nullptr && set.find("_disabled") == nullptr, "underscore directory is skipped");

        write(mods / "load_order.txt", "# newest last\nmid\n\nzeta\nzeta\n");
        const ModSet listed = ModSet::discover(mods, GameKind::KotOR1);
        expect(listed.ok() && order(listed) == "zeta,mid", "load_order.txt enables and orders: " + order(listed));
        expect(listed.warnings().size() == 2, "duplicate load_order.txt line is a warning");

        write(mods / "load_order.txt", "alpha\nghost\n");
        const ModSet broken = ModSet::discover(mods, GameKind::KotOR1);
        expect(order(broken).empty() && hasError(broken, "alpha", "requires `zeta`")
                   && hasError(broken, "ghost", "load_order.txt"),
               "requirement that is not enabled and unknown id are errors");
    }
    {
        const fs::path mods = scratch("rejects");
        write(mods / "newer" / "mod.ini", "id = newer\nversion = 1\napi = 2\n");
        write(mods / "sequel" / "mod.ini", manifest("sequel", "game = kotor2\n"));
        write(mods / "renamed" / "mod.ini", manifest("original"));
        write(mods / "garbled" / "mod.ini", "id = garbled\nversion = one\napi = 1\n");
        write(mods / "badtable" / "mod.ini", manifest("badtable"));
        write(mods / "badtable" / "2da" / "feat.2da", "this is not a table");
        write(mods / "base" / "mod.ini", manifest("base", "", "1.2"));
        write(mods / "needs-new" / "mod.ini", manifest("needs-new", "requires = base >= 1.3\n"));
        write(mods / "needs-chain" / "mod.ini", manifest("needs-chain", "requires = needs-new\n"));
        write(mods / "rival" / "mod.ini", manifest("rival", "conflicts = base\n"));
        write(mods / "loop-a" / "mod.ini", manifest("loop-a", "after = loop-b\n"));
        write(mods / "loop-b" / "mod.ini", manifest("loop-b", "requires = loop-a\n"));
        const ModSet set = ModSet::discover(mods, GameKind::KotOR1);
        expect(order(set) == "base", "only the sound mod loads: " + order(set));
        expect(hasError(set, "newer", "needs modding API 2"), "newer api is rejected");
        expect(hasError(set, "sequel", "is for kotor2"), "mod for the other game is rejected");
        expect(hasError(set, "renamed", "named after the mod id"), "directory/id mismatch is rejected");
        expect(hasError(set, "garbled", "garbled/mod.ini:2:"), "manifest error names file and line");
        expect(hasError(set, "badtable", "2da/feat.2da"), "unparseable table patch rejects the mod");
        expect(hasError(set, "needs-new", "1.3.0 or later; found 1.2.0"), "minimum version is enforced");
        expect(hasError(set, "needs-chain", "requires `needs-new`"), "a rejected mod rejects what requires it");
        expect(hasError(set, "rival", "conflicts with `base`"), "declared conflict is rejected");
        expect(hasError(set, "loop-a", "cycle") && hasError(set, "loop-b", "cycle"), "cycle is rejected");
        expect(ModSet::discover(mods, GameKind::KotOR2).find("sequel") != nullptr, "kotor2 mod loads for kotor2");
    }
}

// A stand-in install: an empty KEY and a placeholder executable, written by the test.
fs::path syntheticInstall(const std::string& name)
{
    const fs::path root = scratch(name);
    std::string key(64, '\0');
    key.replace(0, 8, "KEY V1  ");
    key[16] = 64; // file table offset
    key[20] = 64; // key table offset
    write(root / "chitin.key", key);
    write(root / "swkotor.exe", "placeholder, not a game executable");
    return root;
}

void testMountedMods()
{
    const fs::path game = syntheticInstall("install");
    const fs::path mods = scratch("mounted");
    const std::uint32_t twoDa = resType(ResType::TwoDa);
    const std::uint32_t txi = 2022;

    write(game / "Override" / "shared.txi", "from install override");
    write(game / "Override" / "untouched.txi", "install only");
    write(game / "Override" / "stats.2da", table("0 base 10\n1 base 20\n"));
    write(mods / "first" / "mod.ini", manifest("first"));
    write(mods / "first" / "override" / "Shared.TXI", "from first");
    write(mods / "first" / "override" / "only_first.txi", "first only");
    write(mods / "first" / "override" / "nested" / "deep.txi", "ignored");
    write(mods / "first" / "2da" / "stats.2da", table("1 first 21\n2 first 30\n"));
    write(mods / "second" / "mod.ini", manifest("second", "after = first\n"));
    write(mods / "second" / "Override" / "shared.txi", "from second");
    write(mods / "second" / "2da" / "Stats.2da", table("2 **** 31\n"));
    write(mods / "second" / "2da" / "brandnew.2da", table("0 made 1\n"));

    const ModSet set = ModSet::discover(mods, GameKind::KotOR1);
    expect(set.ok() && order(set) == "first,second", "both mods load: " + order(set));
    expect(set.overlaps().size() == 2, "two overlaps are reported");
    for (const ModOverlap& overlap : set.overlaps()) {
        expect((overlap.resource == "shared.txi" || overlap.resource == "2da:stats")
                   && overlap.winner == "second" && overlap.loser == "first",
               "overlap " + overlap.resource + ": second wins over first");
    }
    expect(set.warnings().size() == 1 && set.warnings()[0].message.find("nested") != std::string::npos,
           "sub-directory of override is a warning");

    const std::set<std::string> before = listing(game);
    Install install(game, false);
    std::string source;
    expect(text(install.find("shared", txi)) == "from install override", "without mods the install Override answers");
    expect(install.table("stats")->cell("1", "value") == "20", "without mods the table is the install's");

    install.mountMods(set);
    expect(text(install.find("shared", txi, &source)) == "from second" && source == "mod:second/override/shared.txi",
           "later mod wins over earlier mod and Override: " + source);
    expect(text(install.find("only_first", txi, &source)) == "first only"
               && source == "mod:first/override/only_first.txi",
           "earlier mod still provides what nothing overrides: " + source);
    expect(text(install.find("untouched", txi)) == "install only", "unrelated install resources are unchanged");
    expect(!install.find("deep", txi).has_value(), "nested override files are not mounted");

    const auto stats = install.table("stats");
    expect(stats && stats->rowCount() == 3, "table patches append rows");
    expect(stats && stats->cell("0", "name") == "base" && stats->cell("0", "value") == "10", "unpatched row is kept");
    expect(stats && stats->cell("1", "name") == "first" && stats->cell("1", "value") == "21", "first mod patches row 1");
    expect(stats && stats->cell("2", "name") == "first" && stats->cell("2", "value") == "31",
           "second mod patches on top of first; **** leaves the cell alone");
    const auto brandNew = install.table("brandnew");
    expect(brandNew && brandNew->cell("0", "name") == "made", "a patch for a table nothing provides is the table");
    expect(text(install.find("stats", twoDa)).find("base 20") != std::string::npos,
           "find() returns the unpatched table bytes");

    install.mountMods(ModSet{});
    expect(text(install.find("shared", txi)) == "from install override", "unmounting restores the install Override");
    expect(install.table("stats")->rowCount() == 2 && !install.table("brandnew"), "unmounting drops table patches");
    expect(listing(game) == before, "the install directory is not written to");
}

} // namespace

int main()
{
    testVersion();
    testManifest();
    testDiscovery();
    testMountedMods();
    std::error_code error;
    fs::remove_all(fs::temp_directory_path() / "kotor_mods_test", error);
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "all mod tests passed\n";
    return 0;
}
