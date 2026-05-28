#pragma once

#include "decode/json/JsonDecodeResult.h"
#include "decode/public/DecodeTypes.h"

#include <boost/json.hpp>

#include <string>

namespace trading_engine::decode {

class JsonDecoder {
public:
    using Json = boost::json::value;

    [[nodiscard]] JsonDecodeResult decode(
        const DecodeInputView& input
    ) const;

    [[nodiscard]] JsonDecodeResult decode_payload(
        const std::string& payload
    ) const;

    [[nodiscard]] static bool is_control_payload(
        const std::string& payload
    );

private:
    [[nodiscard]] static std::string trim_copy(const std::string& input);
    [[nodiscard]] static bool is_control_string_json(const Json& json);
};

}  // namespace trading_engine::decode
