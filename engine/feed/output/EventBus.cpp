#include "feed/output/EventBus.h"

#include <utility>

namespace trading_engine::feed {

void EventBus::subscribe_event(EventHandler handler) {
    event_handlers_.push_back(std::move(handler));
}

void EventBus::subscribe_state(StateHandler handler) {
    state_handlers_.push_back(std::move(handler));
}

void EventBus::subscribe_health(HealthHandler handler) {
    health_handlers_.push_back(std::move(handler));
}

void EventBus::publish_event(const NormalizedEvent& event) {
    for (const auto& handler : event_handlers_) {
        handler(event);
    }
}

void EventBus::publish_state(const StateApplyResult& result) {
    for (const auto& handler : state_handlers_) {
        handler(result);
    }
}

void EventBus::publish_health(const HealthSnapshot& snapshot) {
    for (const auto& handler : health_handlers_) {
        handler(snapshot);
    }
}

}  // namespace trading_engine::feed
