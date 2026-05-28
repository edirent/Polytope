#pragma once

#include "decode/normalize/EventNormalizer.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/decode/NormalizedEvent.h"

namespace trading_engine::feed {

using trading_engine::decode::NormalizationResult;

class EventNormalizer {
public:
    using Json = boost::json::value;

    [[nodiscard]] NormalizationResult normalize_json(
        const RawPacket& packet,
        const Json& json
    ) const {
        return normalizer_.normalize_json(to_decode_input_view(packet), json);
    }

    [[nodiscard]] NormalizationResult normalize_control(
        const RawPacket& packet,
        const std::string& payload
    ) const {
        return normalizer_.normalize_control(
            to_decode_input_view(packet),
            payload
        );
    }

private:
    trading_engine::decode::EventNormalizer normalizer_;
};

}  // namespace trading_engine::feed
