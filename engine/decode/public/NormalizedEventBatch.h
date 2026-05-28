#pragma once

#include "NormalizedEvent.h"

#include <cstddef>
#include <string>
#include <vector>

namespace trading_engine::decode {

/**
 * @brief Bounded batch of normalized events produced from one raw packet.
 */
struct NormalizedEventBatch {
    static constexpr std::size_t kMaxEvents = 64;

    std::vector<NormalizedEvent> events;
    std::vector<std::string> warnings;
    bool overflowed{false};

    [[nodiscard]] bool push_back(const NormalizedEvent& event) {
        if (events.size() >= kMaxEvents) {
            overflowed = true;
            return false;
        }

        events.push_back(event);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return events.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return events.size();
    }

    void clear() noexcept {
        events.clear();
        warnings.clear();
        overflowed = false;
    }
};

}  // namespace trading_engine::decode
