#pragma once

#include "feed/decode/NormalizedEvent.h"
#include "feed/output/HealthPublisher.h"
#include "feed/state/EntityStateStore.h"

#include <functional>
#include <vector>

namespace trading_engine::feed {

using EventHandler = std::function<void(const NormalizedEvent&)>;
using StateHandler = std::function<void(const StateApplyResult&)>;
using HealthHandler = std::function<void(const HealthSnapshot&)>;

class EventBus {
public:
    void subscribe_event(EventHandler handler);
    void subscribe_state(StateHandler handler);
    void subscribe_health(HealthHandler handler);

    void publish_event(const NormalizedEvent& event);
    void publish_state(const StateApplyResult& result);
    void publish_health(const HealthSnapshot& snapshot);

private:
    std::vector<EventHandler> event_handlers_;
    std::vector<StateHandler> state_handlers_;
    std::vector<HealthHandler> health_handlers_;
};

}  // namespace trading_engine::feed
