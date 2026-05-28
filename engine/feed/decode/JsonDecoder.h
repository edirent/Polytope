#pragma once

#include "feed/decode/DecodeInputAdapter.h"

#include "decode/json/JsonDecoder.h"

#include <boost/json.hpp>

#include <string>

namespace trading_engine::feed {

using JsonDecodeStatus = trading_engine::decode::JsonDecodeKind;
using JsonDecodeKind = trading_engine::decode::JsonDecodeKind;
using JsonDecodeResult = trading_engine::decode::JsonDecodeResult;

using trading_engine::decode::to_string;

class JsonDecoder {
public:
    using Json = boost::json::value;

    [[nodiscard]] JsonDecodeResult decode(const RawPacket& packet) const {
        return decoder_.decode(to_decode_input_view(packet));
    }

    [[nodiscard]] JsonDecodeResult decode_payload(
        const std::string& payload
    ) const {
        return decoder_.decode_payload(payload);
    }

    [[nodiscard]] static bool is_control_payload(
        const std::string& payload
    ) {
        return trading_engine::decode::JsonDecoder::is_control_payload(payload);
    }

private:
    trading_engine::decode::JsonDecoder decoder_;
};

}  // namespace trading_engine::feed
