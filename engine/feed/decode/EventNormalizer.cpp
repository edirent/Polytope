#include "feed/decode/EventNormalizer.h"

namespace trading_engine::feed {

NormalizedEvent EventNormalizer::normalize(const DecodedEvent& event) const {
    return NormalizedEvent{
        .entity_id = event.type,
        .type = event.type,
        .payload = event.payload,
        .sequence = event.sequence,
    };
}

}  // namespace trading_engine::feed
