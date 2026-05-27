#include "feed/state/EntityStateStore.h"

namespace trading_engine::feed {

void EntityStateStore::apply(const NormalizedEvent& event) {
    states_[event.entity_id] = event.payload;
}

void EntityStateStore::clear() noexcept {
    states_.clear();
}

const std::string* EntityStateStore::find(const std::string& entity_id) const {
    const auto iter = states_.find(entity_id);
    if (iter == states_.end()) {
        return nullptr;
    }

    return &iter->second;
}

const std::unordered_map<std::string, std::string>& EntityStateStore::states() const noexcept {
    return states_;
}

std::size_t EntityStateStore::size() const noexcept {
    return states_.size();
}

}  // namespace trading_engine::feed
