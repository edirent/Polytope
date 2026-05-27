#include "feed/output/ReplayRunner.h"

#include <utility>

namespace trading_engine::feed {

void ReplayRunner::load(std::vector<RawPacket> packets) {
    reader_.load(std::move(packets));
}

void ReplayRunner::reset() noexcept {
    reader_.reset();
}

bool ReplayRunner::replay_next(
    JsonDecoder& decoder,
    EventNormalizer& normalizer,
    EventBus& event_bus) {
    auto packet = reader_.next();
    if (!packet.has_value()) {
        return false;
    }

    event_bus.publish(normalizer.normalize(decoder.decode(*packet)));
    return true;
}

}  // namespace trading_engine::feed
