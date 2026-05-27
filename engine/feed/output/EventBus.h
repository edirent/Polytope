#pragma once

#include "feed/decode/EventNormalizer.h"

#include <vector>

namespace trading_engine::feed {

class EventBus {
public:
    void publish(NormalizedEvent event);
    void clear() noexcept;

    [[nodiscard]] const std::vector<NormalizedEvent>& events() const noexcept;

private:
    std::vector<NormalizedEvent> events_;
};

}  // namespace trading_engine::feed
