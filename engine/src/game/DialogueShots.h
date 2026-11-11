#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>

namespace ke {

// How retail picks the shot for a dialogue node whose CameraAngle is 0. Nothing here is random:
// a table of 19 angles (written at 005a0f84) is walked from a start, step and direction that a
// hash of the conversation's resref gives (0059f220), so one conversation always cuts the same
// way. A line takes one step (005a194b), and so does each list of replies, however many it holds
// (005a1ecc). The first line and the first list get angle 2 (005a19b1, 005a2342), but still take
// their step. The value is written into the node, so it holds while the conversation runs.
// Authored angles are kept and still take their step. Checked on retail at 005a13d0: end_trask01
// starts at index 9, step 1, forward, and shows angles 2, 2, 3, then 2 on its spoken reply.
class DialogueShots {
public:
    DialogueShots() = default;

    explicit DialogueShots(std::string_view resref)
    {
        // 0059f29c: each character times 10^position + 1, truncated to 32 bits.
        std::uint32_t hash = 0;
        std::uint64_t power = 1;
        for (const char c : resref) {
            const std::uint32_t scale = static_cast<std::uint32_t>(power) + 1u;
            hash += scale * static_cast<std::uint32_t>(static_cast<std::int32_t>(c));
            power *= 10u;
        }
        index_ = static_cast<std::int32_t>(hash % kAngles.size());
        step_ = static_cast<std::int32_t>(hash % 5u) + 1;
        forward_ = (hash & 1u) != 0u;
    }

    // A line starts: the angle to shoot it at.
    std::int32_t enterEntry(std::uint32_t entry, std::int32_t authored)
    {
        const std::int32_t next = take();
        const bool first = firstEntry_;
        firstEntry_ = false;
        if (authored > 0) {
            return authored;
        }
        const auto [slot, added] = entries_.try_emplace(entry, first ? 2 : next);
        return slot->second;
    }

    // A line's replies are offered. Call once per list, then offerReply for each reply on it.
    void beginReplies()
    {
        listAngle_ = take();
        listFirst_ = firstReplies_;
        firstReplies_ = false;
    }

    std::int32_t offerReply(std::uint32_t reply, std::int32_t authored)
    {
        if (authored > 0) {
            return authored;
        }
        const auto [slot, added] = replies_.try_emplace(reply, listFirst_ ? 2 : listAngle_);
        return slot->second;
    }

private:
    std::int32_t take()
    {
        const std::int32_t value = kAngles[static_cast<std::size_t>(index_)];
        index_ += forward_ ? step_ : -step_;
        // 005a1980: past the end goes to the start, below the start to the end, not modulo.
        if (index_ >= static_cast<std::int32_t>(kAngles.size())) {
            index_ = 0;
        } else if (index_ < 0) {
            index_ = static_cast<std::int32_t>(kAngles.size()) - 1;
        }
        return value;
    }

    static constexpr std::array<std::int32_t, 19> kAngles{1, 3, 1, 3, 2, 2, 1, 3, 1, 2,
                                                          1, 2, 1, 3, 2, 3, 1, 1, 1};
    std::int32_t index_ = 0;
    std::int32_t step_ = 1;
    bool forward_ = false;
    bool firstEntry_ = true;
    bool firstReplies_ = true;
    std::int32_t listAngle_ = 2;
    bool listFirst_ = true;
    std::map<std::uint32_t, std::int32_t> entries_;
    std::map<std::uint32_t, std::int32_t> replies_;
};

} // namespace ke
