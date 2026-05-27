#include "feed/output/EventBus.h"

#include <utility>

namespace trading_engine::feed {

void EventBus::publish(NormalizedEvent event) {
    events_.push_back(std::move(event));
}

void EventBus::clear() noexcept {
    events_.clear();
}

const std::vector<NormalizedEvent>& EventBus::events() const noexcept {
    return events_;
}

}  // namespace trading_engine::feed
