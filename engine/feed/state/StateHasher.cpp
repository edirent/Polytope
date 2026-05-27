#include "feed/state/StateHasher.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace trading_engine::feed {

std::size_t StateHasher::hash(const EntityStateStore& store) const noexcept {
    std::size_t result = 0;
    std::hash<std::string> hasher;
    std::vector<const std::pair<const std::string, std::string>*> ordered_states;

    ordered_states.reserve(store.states().size());
    for (const auto& state : store.states()) {
        ordered_states.push_back(&state);
    }

    std::sort(ordered_states.begin(), ordered_states.end(), [](const auto* left, const auto* right) {
        return left->first < right->first;
    });

    for (const auto* state : ordered_states) {
        const auto& [entity_id, payload] = *state;
        result ^= hasher(entity_id) + 0x9e3779b9 + (result << 6) + (result >> 2);
        result ^= hasher(payload) + 0x9e3779b9 + (result << 6) + (result >> 2);
    }

    return result;
}

}  // namespace trading_engine::feed
