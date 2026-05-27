#pragma once

#include "feed/decode/JsonDecoder.h"

#include <cstdint>
#include <string>

namespace trading_engine::feed {

struct NormalizedEvent {
    std::string entity_id;
    std::string type;
    std::string payload;
    std::uint64_t sequence{0};
};

class EventNormalizer {
public:
    [[nodiscard]] NormalizedEvent normalize(const DecodedEvent& event) const;
};

}  // namespace trading_engine::feed
