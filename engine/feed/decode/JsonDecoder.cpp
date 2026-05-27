#include "feed/decode/JsonDecoder.h"

namespace trading_engine::feed {

DecodedEvent JsonDecoder::decode(const RawPacket& packet) const {
    return DecodedEvent{
        .type = "raw_json",
        .payload = packet.payload,
        .sequence = packet.sequence,
    };
}

}  // namespace trading_engine::feed
