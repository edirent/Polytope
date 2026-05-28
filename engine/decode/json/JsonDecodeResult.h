#pragma once

#include "decode/json/JsonView.h"
#include "decode/public/DecodeError.h"

#include <boost/json.hpp>

#include <string>
#include <vector>

namespace trading_engine::decode {

enum class JsonDecodeKind {
    JsonObject = 0,
    JsonArray,
    NonJsonControl,
    UnsupportedJson,
    MalformedJson
};

struct JsonDecodeResult {
    JsonDecodeKind kind{JsonDecodeKind::MalformedJson};

    // Compatibility field for the previous Feed JsonDecoder API.
    JsonDecodeKind status{JsonDecodeKind::MalformedJson};

    boost::json::value json;
    std::string control_payload;
    DecodeErrorCode error{DecodeErrorCode::MalformedJson};
    std::string message;
    std::vector<std::string> diagnostics;

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] bool has_json_event_payload() const noexcept;
    [[nodiscard]] bool has_control_payload() const noexcept;
    [[nodiscard]] JsonView view() const noexcept;
};

[[nodiscard]] std::string to_string(JsonDecodeKind kind);

}  // namespace trading_engine::decode
