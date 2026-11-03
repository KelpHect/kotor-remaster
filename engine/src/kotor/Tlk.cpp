#include "kotor/Formats.h"

#include <cstring>

namespace ke::kotor {

std::string Tlk::text(std::uint32_t strref) const
{
    if (strref >= entries_.size()) {
        return {};
    }
    const Entry& entry = entries_[strref];
    if (entry.offset > text_.size() || entry.length > text_.size() - entry.offset) {
        return {};
    }
    std::string out(reinterpret_cast<const char*>(text_.data() + entry.offset), entry.length);
    while (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return out;
}

Tlk Tlk::parse(std::vector<std::uint8_t> bytes, std::string name)
{
    Tlk tlk;
    tlk.name_ = std::move(name);
    if (bytes.size() < 20 || std::memcmp(bytes.data(), "TLK ", 4) != 0
        || std::memcmp(bytes.data() + 4, "V3.0", 4) != 0) {
        throw FormatError(tlk.name_ + ": not a TLK V3.0 file");
    }
    const std::uint32_t count = readU32(bytes.data(), bytes.size(), 12, "TLK entry count");
    const std::uint32_t textOffset = readU32(bytes.data(), bytes.size(), 16, "TLK text offset");
    requireRange(bytes.size(), 20, static_cast<std::size_t>(count) * 40u, "TLK entry table");
    if (count > 1'000'000u) {
        throw FormatError(tlk.name_ + ": implausible TLK entry count");
    }
    tlk.entries_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t o = 20u + static_cast<std::size_t>(i) * 40u;
        tlk.entries_[i].offset = readU32(bytes.data(), bytes.size(), o + 28u, "TLK string offset");
        tlk.entries_[i].length = readU32(bytes.data(), bytes.size(), o + 32u, "TLK string length");
    }
    if (textOffset > bytes.size()) {
        throw FormatError(tlk.name_ + ": TLK text offset outside file");
    }
    requireRange(bytes.size(), textOffset, bytes.size() - textOffset, "TLK text blob");
    tlk.text_.assign(bytes.begin() + textOffset, bytes.end());
    return tlk;
}

Dlg Dlg::parse(const Gff& gff, const Tlk* tlk, std::string name)
{
    Dlg dlg;
    const std::uint32_t root = gff.rootIndex();
    dlg.endConversation = [&]() {
        const GffField* f = gff.field(root, "EndConversation");
        return f != nullptr ? gff.text(*f) : std::string{};
    }();
    dlg.endConversationAbort = [&]() {
        const GffField* f = gff.field(root, "EndConverAbort");
        return f != nullptr ? gff.text(*f) : std::string{};
    }();
    if (const GffField* f = gff.field(root, "Skippable"); f != nullptr) {
        dlg.skippable = gff.number(*f) != 0.0f;
    }
    if (const GffField* f = gff.field(root, "CameraModel"); f != nullptr) {
        dlg.cameraModel = gff.text(*f);
    }
    const auto resolve = [&](const GffField& field) -> std::string {
        const std::string value = gff.text(field);
        if (tlk == nullptr || value.empty()) {
            return value;
        }
        try {
            const std::uint32_t strref = static_cast<std::uint32_t>(std::stoul(value));
            const std::string resolved = tlk->text(strref);
            return resolved.empty() ? value : resolved;
        } catch (const std::exception&) {
            return value;
        }
    };
    const auto replaceStrref = [&](const std::string& raw) -> std::string {
        if (tlk == nullptr || raw.empty()) {
            return raw;
        }
        try {
            const std::uint32_t strref = static_cast<std::uint32_t>(std::stoul(raw));
            return tlk->text(strref);
        } catch (const std::exception&) {
            return raw;
        }
    };
    const auto resolveLocString = [&](const GffField& field) -> std::string {
        if (field.type != GffType::LocString) {
            return resolve(field);
        }
        const std::uint32_t strref = gff.locStringStrref(field);
        if (tlk != nullptr && strref != UINT32_MAX) {
            const std::string resolved = tlk->text(strref);
            if (!resolved.empty()) {
                return resolved;
            }
        }
        return replaceStrref(gff.locString(field));
    };
    const auto action = [&](const GffField& field, std::int32_t* out) {
        if (field.type == GffType::Int32 || field.type == GffType::UInt32 || field.type == GffType::Int16
            || field.type == GffType::UInt16 || field.type == GffType::Int8 || field.type == GffType::UInt8) {
            *out = gff.integer(field);
        }
    };
    const auto camera = [&](std::uint32_t structure, std::int32_t& cameraId,
                            std::int32_t& cameraAnimation, std::int32_t& cameraAngle,
                            std::int32_t& camVidEffect, float& camHeightOffset,
                            float& tarHeightOffset) {
        if (const GffField* f = gff.field(structure, "CameraID")) {
            cameraId = gff.integer(*f);
        }
        if (const GffField* f = gff.field(structure, "CameraAnimation")) {
            cameraAnimation = gff.integer(*f);
        }
        if (const GffField* f = gff.field(structure, "CameraAngle")) {
            cameraAngle = gff.integer(*f);
        }
        if (const GffField* f = gff.field(structure, "CamVidEffect")) {
            camVidEffect = gff.integer(*f);
        }
        if (const GffField* f = gff.field(structure, "CamHeightOffset")) {
            camHeightOffset = gff.number(*f);
        }
        if (const GffField* f = gff.field(structure, "TarHeightOffset")) {
            tarHeightOffset = gff.number(*f);
        }
    };
    if (const GffField* f = gff.field(root, "EntryList"); f != nullptr) {
        for (const std::uint32_t s : gff.list(*f)) {
            DlgEntry entry;
            if (const GffField* v = gff.field(s, "Speaker"); v != nullptr) {
                entry.speaker = gff.text(*v);
            }
            if (const GffField* v = gff.field(s, "Text"); v != nullptr) {
                entry.text = resolveLocString(*v);
            }
            if (const GffField* v = gff.field(s, "Script"); v != nullptr) {
                entry.script = gff.text(*v);
            }
            if (const GffField* v = gff.field(s, "Active"); v != nullptr) {
                entry.active = gff.text(*v);
            }
            if (const GffField* v = gff.field(s, "Quest"); v != nullptr) {
                entry.quest = gff.text(*v);
            }
            for (int i = 0; i < 4; ++i) {
                const std::string label = "ActionParam" + std::to_string(i + 1);
                if (const GffField* v = gff.field(s, label); v != nullptr) {
                    action(*v, &entry.action[i]);
                }
            }
            camera(s, entry.cameraId, entry.cameraAnimation, entry.cameraAngle,
                   entry.camVidEffect, entry.camHeightOffset, entry.tarHeightOffset);
            if (const GffField* v = gff.field(s, "Comment"); v != nullptr) {
                entry.comment = gff.text(*v);
            }
            if (const GffField* v = gff.field(s, "Sound"); v != nullptr) {
                entry.sound = gff.text(*v);
            }
            if (const GffField* v = gff.field(s, "RepliesList"); v != nullptr) {
                for (const std::uint32_t link : gff.list(*v)) {
                    if (const GffField* idx = gff.field(link, "Index"); idx != nullptr) {
                        entry.replies.push_back(static_cast<std::uint32_t>(gff.integer(*idx)));
                    }
                }
            }
            dlg.entries.push_back(std::move(entry));
        }
    }
    if (const GffField* f = gff.field(root, "ReplyList"); f != nullptr) {
        for (const std::uint32_t s : gff.list(*f)) {
            DlgReply reply;
            if (const GffField* v = gff.field(s, "Text"); v != nullptr) {
                reply.text = resolveLocString(*v);
            }
            if (const GffField* v = gff.field(s, "Script"); v != nullptr) {
                reply.script = gff.text(*v);
            }
            if (const GffField* v = gff.field(s, "Active"); v != nullptr) {
                reply.active = gff.text(*v);
            }
            if (const GffField* v = gff.field(s, "Quest"); v != nullptr) {
                reply.quest = gff.text(*v);
            }
            for (int i = 0; i < 4; ++i) {
                const std::string label = "ActionParam" + std::to_string(i + 1);
                if (const GffField* v = gff.field(s, label); v != nullptr) {
                    action(*v, &reply.action[i]);
                }
            }
            camera(s, reply.cameraId, reply.cameraAnimation, reply.cameraAngle,
                   reply.camVidEffect, reply.camHeightOffset, reply.tarHeightOffset);
            if (const GffField* v = gff.field(s, "Comment"); v != nullptr) {
                reply.comment = gff.text(*v);
            }
            if (const GffField* v = gff.field(s, "Sound"); v != nullptr) {
                reply.sound = gff.text(*v);
            }
            if (const GffField* v = gff.field(s, "EntriesList"); v != nullptr) {
                for (const std::uint32_t link : gff.list(*v)) {
                    if (const GffField* idx = gff.field(link, "Index"); idx != nullptr) {
                        reply.entries.push_back(static_cast<std::uint32_t>(gff.integer(*idx)));
                    }
                }
            }
            dlg.replies.push_back(std::move(reply));
        }
    }
    if (const GffField* f = gff.field(root, "StartingList"); f != nullptr) {
        for (const std::uint32_t s : gff.list(*f)) {
            if (const GffField* idx = gff.field(s, "Index"); idx != nullptr) {
                dlg.starting.push_back(static_cast<std::uint32_t>(gff.integer(*idx)));
                std::string active;
                if (const GffField* a = gff.field(s, "Active"); a != nullptr) {
                    active = gff.text(*a);
                }
                dlg.startingActive.push_back(std::move(active));
            }
        }
    }
    (void)name;
    return dlg;
}

} // namespace ke::kotor
