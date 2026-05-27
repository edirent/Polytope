#pragma once

#include "feed/decode/EventNormalizer.h"
#include "feed/decode/JsonDecoder.h"
#include "feed/output/EventBus.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "feed/raw_ingest/RawPacket.h"

#include <vector>

namespace trading_engine::feed {

class ReplayRunner {
public:
    void load(std::vector<RawPacket> packets);
    void reset() noexcept;

    [[nodiscard]] bool replay_next(
        JsonDecoder& decoder,
        EventNormalizer& normalizer,
        EventBus& event_bus);

private:
    RawLogReader reader_;
};

}  // namespace trading_engine::feed
