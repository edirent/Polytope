#include "feed/output/ReplayRunner.h"

#include <memory>
#include <stdexcept>
#include <utility>

namespace trading_engine::feed {

ReplayRunner::ReplayRunner(std::string raw_log_path) {
    if (!raw_log_path.empty()) {
        load(std::move(raw_log_path));
    }
}

void ReplayRunner::load(std::string raw_log_path) {
    reader_ = std::make_unique<RawLogReader>(std::move(raw_log_path));
}

void ReplayRunner::reset() {
    if (reader_) {
        reader_->reset();
    }
}

bool ReplayRunner::replay_next(
    JsonDecoder& decoder,
    EventNormalizer& normalizer,
    EventBus& event_bus) {
    if (!reader_) {
        return false;
    }

    auto packet = reader_->next();
    if (packet.eof()) {
        return false;
    }

    if (!packet.ok()) {
        throw std::runtime_error("ReplayRunner raw read failed: " + packet.message);
    }

    event_bus.publish(normalizer.normalize(decoder.decode(*packet.packet)));
    return true;
}

}  // namespace trading_engine::feed
