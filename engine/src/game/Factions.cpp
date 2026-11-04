#include "game/Factions.h"

#include "kotor/GffWrite.h"

#include <algorithm>
#include <cstdlib>

namespace ke {

namespace {

int reputationValue(const std::string& text, int fallback = 50)
{
    if (text.empty() || text == "****") {
        return fallback;
    }
    return std::clamp(std::atoi(text.c_str()), 0, 100);
}

} // namespace

void FactionSystem::resize(std::size_t count)
{
    names_.resize(count);
    parents_.assign(count, -1);
    matrix_.assign(count * count, 100);
    relations_ = 0;
}

bool FactionSystem::load(kotor::Install& install, std::string& error)
{
    error.clear();
    auto data = install.find("repute", kotor::resType(kotor::ResType::TwoDa));
    if (!data.has_value()) {
        error = "repute.2da is missing";
        return false;
    }
    try {
        const kotor::TwoDa table = kotor::TwoDa::parse(std::move(*data), "repute");
        const std::size_t count = table.rowCount();
        if (count < 2 || table.columnCount() != count) {
            error = "repute.2da is not a square faction matrix";
            return false;
        }
        resize(count);
        for (std::size_t row = 0; row < count; ++row) {
            names_[row] = table.cell(row, 0);
            if (names_[row].empty()) {
                names_[row] = "Faction_" + std::to_string(row);
            }
            // Column 0 holds the faction name, so player relations live in
            // row 0 and are mirrored into column 0 for the other rows.
            matrix_[row * count] = row == 0
                ? 50 : reputationValue(table.cell(0, row));
            for (std::size_t column = 1; column < count; ++column) {
                matrix_[row * count + column] =
                    reputationValue(table.cell(row, column));
            }
        }
        matrix_[0] = 50;
        relations_ = 0;
        for (const int value : matrix_) {
            if (value != 100) {
                ++relations_;
            }
        }
        return true;
    } catch (const kotor::FormatError& exception) {
        error = exception.what();
        return false;
    }
}

const std::string& FactionSystem::name(int faction) const
{
    static const std::string empty;
    return faction >= 0 && static_cast<std::size_t>(faction) < names_.size()
        ? names_[static_cast<std::size_t>(faction)] : empty;
}

int FactionSystem::parent(int faction) const
{
    return faction >= 0 && static_cast<std::size_t>(faction) < parents_.size()
        ? parents_[static_cast<std::size_t>(faction)] : -1;
}

int FactionSystem::reputation(int factionA, int factionB) const
{
    if (!loaded() || factionA < 0 || factionB < 0
        || static_cast<std::size_t>(factionA) >= names_.size()
        || static_cast<std::size_t>(factionB) >= names_.size()) {
        return 50;
    }
    return matrix_[static_cast<std::size_t>(factionA) * names_.size()
                   + static_cast<std::size_t>(factionB)];
}

bool FactionSystem::setReputation(int factionA, int factionB, int value)
{
    if (!loaded() || factionA < 0 || factionB < 0
        || static_cast<std::size_t>(factionA) >= names_.size()
        || static_cast<std::size_t>(factionB) >= names_.size()) {
        return false;
    }
    matrix_[static_cast<std::size_t>(factionA) * names_.size()
            + static_cast<std::size_t>(factionB)] = std::clamp(value, 0, 100);
    relations_ = 0;
    for (const int cell : matrix_) {
        if (cell != 100) {
            ++relations_;
        }
    }
    return true;
}

bool FactionSystem::adjustReputation(int factionA, int factionB, int delta)
{
    return setReputation(factionA, factionB, reputation(factionA, factionB) + delta);
}

bool FactionSystem::loadFac(const std::vector<std::uint8_t>& bytes, std::string& error)
{
    error.clear();
    if (bytes.empty()) {
        return false;
    }
    try {
        const kotor::Gff fac = kotor::Gff::parse(bytes, "REPUTE");
        const kotor::GffField* factions = fac.field(fac.rootIndex(), "FactionList");
        if (factions == nullptr || factions->type != kotor::GffType::List) {
            error = "REPUTE has no FactionList";
            return false;
        }
        const std::vector<std::uint32_t> factionRows = fac.list(*factions);
        if (factionRows.empty()) {
            // An empty FAC is the synthetic fresh-save scaffold; keep the
            // repute.2da defaults.
            return true;
        }
        std::vector<std::string> names;
        std::vector<int> parents;
        names.reserve(factionRows.size());
        parents.reserve(factionRows.size());
        for (const std::uint32_t row : factionRows) {
            const kotor::GffField* name = fac.field(row, "FactionName");
            const kotor::GffField* parent = fac.field(row, "FactionParentID");
            names.push_back(name != nullptr ? fac.text(*name)
                                            : "Faction_" + std::to_string(names.size()));
            parents.push_back(parent != nullptr ? fac.integer(*parent) : -1);
        }
        names_.swap(names);
        parents_.swap(parents);
        matrix_.assign(names_.size() * names_.size(), 100);
        const kotor::GffField* reps = fac.field(fac.rootIndex(), "RepList");
        if (reps != nullptr && reps->type == kotor::GffType::List) {
            for (const std::uint32_t row : fac.list(*reps)) {
                const kotor::GffField* id1 = fac.field(row, "FactionID1");
                const kotor::GffField* id2 = fac.field(row, "FactionID2");
                const kotor::GffField* value = fac.field(row, "FactionRep");
                if (id1 == nullptr || id2 == nullptr || value == nullptr) {
                    continue;
                }
                const std::int32_t a = fac.integer(*id1);
                const std::int32_t b = fac.integer(*id2);
                if (a < 0 || b < 0 || static_cast<std::size_t>(a) >= names_.size()
                    || static_cast<std::size_t>(b) >= names_.size()) {
                    continue;
                }
                matrix_[static_cast<std::size_t>(a) * names_.size()
                        + static_cast<std::size_t>(b)] =
                    std::clamp(fac.integer(*value), 0, 100);
            }
        }
        relations_ = 0;
        for (const int cell : matrix_) {
            if (cell != 100) {
                ++relations_;
            }
        }
        return true;
    } catch (const kotor::FormatError& exception) {
        error = exception.what();
        return false;
    }
}

std::vector<std::uint8_t> FactionSystem::saveFac() const
{
    if (!loaded()) {
        return {};
    }
    kotor::GffNode root(0);
    std::vector<kotor::GffNodePtr> factions;
    factions.reserve(names_.size());
    for (std::size_t i = 0; i < names_.size(); ++i) {
        auto row = std::make_shared<kotor::GffNode>(0);
        row->addString("FactionName", names_[i]);
        row->addInt32("FactionParentID", parents_[i]);
        row->addInt32("FactionGlobal", 1);
        factions.push_back(std::move(row));
    }
    root.addList("FactionList", std::move(factions));
    std::vector<kotor::GffNodePtr> reps;
    for (std::size_t a = 0; a < names_.size(); ++a) {
        for (std::size_t b = 0; b < names_.size(); ++b) {
            const int value = matrix_[a * names_.size() + b];
            if (value == 100) {
                continue;
            }
            auto row = std::make_shared<kotor::GffNode>(0);
            row->addInt32("FactionID1", static_cast<std::int32_t>(a));
            row->addInt32("FactionID2", static_cast<std::int32_t>(b));
            row->addInt32("FactionRep", value);
            reps.push_back(std::move(row));
        }
    }
    root.addList("RepList", std::move(reps));
    return kotor::writeGff("FAC ", root);
}

} // namespace ke
