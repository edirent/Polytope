#include "feed/decode/JsonDecoder.h"

#include <boost/json/src.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace trading_engine::feed {

namespace {

/**
 * @brief Lowercase ASCII helper.
 *
 * Heartbeat/control payloads are tiny strings, so this simple helper is enough.
 */
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

/**
 * @brief Classify JSON primitive type for diagnostics.
 */
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

}  // namespace

bool JsonDecodeResult::ok() const noexcept {
    return status == JsonDecodeStatus::JsonObject ||
           status == JsonDecodeStatus::JsonArray ||
           status == JsonDecodeStatus::NonJsonControl;
}

bool JsonDecodeResult::has_json_event_payload() const noexcept {
    return status == JsonDecodeStatus::JsonObject ||
           status == JsonDecodeStatus::JsonArray;
}

bool JsonDecodeResult::has_control_payload() const noexcept {
    return status == JsonDecodeStatus::NonJsonControl;
}

JsonDecodeResult JsonDecoder::decode(const RawPacket& packet) const {
    return decode_payload(packet.payload);
}

JsonDecodeResult JsonDecoder::decode_payload(
    const std::string& payload
) const {
    JsonDecodeResult result;

    const std::string trimmed = trim_copy(payload);

    /**
     * First handle non-JSON control payloads.
     *
     * Example:
     *
     *     PONG
     *
     * If we tried to parse this as JSON first, it would fail. But this is not
     * a malformed market-data packet; it is a normal heartbeat response.
     */
    if (is_control_payload(trimmed)) {
        result.status = JsonDecodeStatus::NonJsonControl;
        result.control_payload = trimmed;
        return result;
    }

    /**
     * Parse JSON without throwing.
     *
     * boost::json::parse(..., error_code) reports parse failures through the
     * provided error_code instead of throwing an exception.
     *
     * This is important because malformed payloads should become explicit
     * DecodeResult values, not process crashes.
     */
    boost::json::error_code error;
    auto json = boost::json::parse(trimmed, error);

    if (error) {
        result.status = JsonDecodeStatus::MalformedJson;
        result.message = "payload is neither valid JSON nor known control message";
        return result;
    }

    /**
     * JSON object:
     *
     *     {"type":"book", ...}
     *
     * This is the standard single-event payload shape.
     */
    if (json.is_object()) {
        result.status = JsonDecodeStatus::JsonObject;
        result.json = std::move(json);
        return result;
    }

    /**
     * JSON array:
     *
     *     [{"type":"book", ...}]
     *
     * Real Polymarket payloads have already shown this wrapper, so the decoder
     * must preserve the array and let EventNormalizer expand it.
     */
    if (json.is_array()) {
        result.status = JsonDecodeStatus::JsonArray;
        result.json = std::move(json);
        return result;
    }

    /**
     * JSON string control payload:
     *
     *     "PONG"
     *
     * This is valid JSON, but semantically it is still a heartbeat/control
     * message, not a market event.
     */
    if (is_control_string_json(json)) {
        result.status = JsonDecodeStatus::NonJsonControl;
        const auto& string = json.as_string();
        result.control_payload.assign(string.data(), string.size());
        return result;
    }

    /**
     * Valid JSON but not event-shaped.
     *
     * We do not treat this as MalformedJson because parsing succeeded.
     * But it is not suitable for EventNormalizer::normalize_json().
     */
    result.status = JsonDecodeStatus::UnsupportedJson;
    result.json = std::move(json);
    result.message = "unsupported JSON kind: " + json_kind(result.json);
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

std::string to_string(JsonDecodeStatus status) {
    switch (status) {
        case JsonDecodeStatus::JsonObject:
            return "JsonObject";

        case JsonDecodeStatus::JsonArray:
            return "JsonArray";

        case JsonDecodeStatus::NonJsonControl:
            return "NonJsonControl";

        case JsonDecodeStatus::UnsupportedJson:
            return "UnsupportedJson";

        case JsonDecodeStatus::MalformedJson:
        default:
            return "MalformedJson";
    }
}

}  // namespace trading_engine::feed
