#include "decode/json/JsonDecoder.h"

#include <boost/json/src.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace trading_engine::decode {

namespace {

std::string lowercase_ascii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    return value;
}

std::string json_kind(const JsonDecoder::Json& json) {
    if (json.is_null()) {
        return "null";
    }

    if (json.is_bool()) {
        return "boolean";
    }

    if (json.is_int64() || json.is_uint64() || json.is_double()) {
        return "number";
    }

    if (json.is_string()) {
        return "string";
    }

    if (json.is_object()) {
        return "object";
    }

    if (json.is_array()) {
        return "array";
    }

    return "unknown";
}

void set_kind(
    JsonDecodeResult& result,
    JsonDecodeKind kind,
    DecodeErrorCode error
) {
    result.kind = kind;
    result.status = kind;
    result.error = error;
}

}  // namespace

bool JsonDecodeResult::ok() const noexcept {
    return kind == JsonDecodeKind::JsonObject ||
           kind == JsonDecodeKind::JsonArray ||
           kind == JsonDecodeKind::NonJsonControl;
}

bool JsonDecodeResult::has_json_event_payload() const noexcept {
    return kind == JsonDecodeKind::JsonObject ||
           kind == JsonDecodeKind::JsonArray;
}

bool JsonDecodeResult::has_control_payload() const noexcept {
    return kind == JsonDecodeKind::NonJsonControl;
}

JsonView JsonDecodeResult::view() const noexcept {
    if (!has_json_event_payload() && kind != JsonDecodeKind::UnsupportedJson) {
        return JsonView{};
    }

    return JsonView{&json};
}

JsonDecodeResult JsonDecoder::decode(
    const DecodeInputView& input
) const {
    return decode_payload(std::string{input.payload});
}

JsonDecodeResult JsonDecoder::decode_payload(
    const std::string& payload
) const {
    JsonDecodeResult result;

    const std::string trimmed = trim_copy(payload);

    if (trimmed.empty()) {
        set_kind(
            result,
            JsonDecodeKind::MalformedJson,
            DecodeErrorCode::EmptyPayload
        );
        result.message = "payload is empty";
        result.diagnostics.push_back(result.message);
        return result;
    }

    if (is_control_payload(trimmed)) {
        set_kind(
            result,
            JsonDecodeKind::NonJsonControl,
            DecodeErrorCode::NonJsonControl
        );
        result.control_payload = trimmed;
        return result;
    }

    boost::json::error_code error;
    auto json = boost::json::parse(trimmed, error);

    if (error) {
        set_kind(
            result,
            JsonDecodeKind::MalformedJson,
            DecodeErrorCode::MalformedJson
        );
        result.message = "payload is neither valid JSON nor known control message";
        result.diagnostics.push_back(result.message);
        return result;
    }

    if (json.is_object()) {
        set_kind(
            result,
            JsonDecodeKind::JsonObject,
            DecodeErrorCode::None
        );
        result.json = std::move(json);
        return result;
    }

    if (json.is_array()) {
        set_kind(
            result,
            JsonDecodeKind::JsonArray,
            DecodeErrorCode::None
        );
        result.json = std::move(json);
        return result;
    }

    if (is_control_string_json(json)) {
        set_kind(
            result,
            JsonDecodeKind::NonJsonControl,
            DecodeErrorCode::NonJsonControl
        );
        const auto& string = json.as_string();
        result.control_payload.assign(string.data(), string.size());
        return result;
    }

    set_kind(
        result,
        JsonDecodeKind::UnsupportedJson,
        DecodeErrorCode::UnsupportedJson
    );
    result.json = std::move(json);
    result.message = "unsupported JSON kind: " + json_kind(result.json);
    result.diagnostics.push_back(result.message);
    return result;
}

bool JsonDecoder::is_control_payload(const std::string& payload) {
    const std::string trimmed = trim_copy(payload);
    const std::string lower = lowercase_ascii(trimmed);

    return lower == "pong" ||
           lower == "ping";
}

std::string JsonDecoder::trim_copy(const std::string& input) {
    const auto is_not_space = [](unsigned char c) {
        return !std::isspace(c);
    };

    const auto begin = std::find_if(
        input.begin(),
        input.end(),
        is_not_space
    );

    if (begin == input.end()) {
        return {};
    }

    const auto rbegin = std::find_if(
        input.rbegin(),
        input.rend(),
        is_not_space
    );

    const auto end = rbegin.base();

    return std::string(begin, end);
}

bool JsonDecoder::is_control_string_json(const Json& json) {
    if (!json.is_string()) {
        return false;
    }

    const auto& string = json.as_string();
    return is_control_payload(std::string(string.data(), string.size()));
}

std::string to_string(JsonDecodeKind kind) {
    switch (kind) {
        case JsonDecodeKind::JsonObject:
            return "JsonObject";
        case JsonDecodeKind::JsonArray:
            return "JsonArray";
        case JsonDecodeKind::NonJsonControl:
            return "NonJsonControl";
        case JsonDecodeKind::UnsupportedJson:
            return "UnsupportedJson";
        case JsonDecodeKind::MalformedJson:
        default:
            return "MalformedJson";
    }
}

std::string to_string(DecodeErrorCode code) {
    switch (code) {
        case DecodeErrorCode::None:
            return "None";
        case DecodeErrorCode::NonJsonControl:
            return "NonJsonControl";
        case DecodeErrorCode::UnsupportedCodec:
            return "UnsupportedCodec";
        case DecodeErrorCode::MalformedJson:
            return "MalformedJson";
        case DecodeErrorCode::UnsupportedJson:
            return "UnsupportedJson";
        case DecodeErrorCode::EmptyPayload:
            return "EmptyPayload";
        case DecodeErrorCode::InternalError:
        default:
            return "InternalError";
    }
}

std::string to_string(DecodeErrorSeverity severity) {
    switch (severity) {
        case DecodeErrorSeverity::Info:
            return "Info";
        case DecodeErrorSeverity::Warning:
            return "Warning";
        case DecodeErrorSeverity::Error:
            return "Error";
        case DecodeErrorSeverity::Fatal:
        default:
            return "Fatal";
    }
}

}  // namespace trading_engine::decode
