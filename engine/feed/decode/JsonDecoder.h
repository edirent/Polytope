#pragma once

#include "feed/raw_ingest/RawPacket.h"

#include <cstdint>
#include <string>

namespace trading_engine::feed {

struct DecodedEvent {
    std::string type;
    std::string payload;
    std::uint64_t sequence{0};
};

class JsonDecoder {
public:
    [[nodiscard]] DecodedEvent decode(const RawPacket& packet) const;
};

}  // namespace trading_engine::feed
