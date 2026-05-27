#pragma once

#include "feed/decode/EventNormalizer.h"
#include "feed/decode/JsonDecoder.h"
#include "feed/output/EventBus.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "feed/raw_ingest/RawPacket.h"

#include <memory>
#include <string>

namespace trading_engine::feed {

class ReplayRunner {
public:
    explicit ReplayRunner(std::string raw_log_path = {});

    void load(std::string raw_log_path);
    void reset();

    [[nodiscard]] bool replay_next(
        JsonDecoder& decoder,
        EventNormalizer& normalizer,
        EventBus& event_bus);

private:
    std::unique_ptr<RawLogReader> reader_;
};

}  // namespace trading_engine::feed
