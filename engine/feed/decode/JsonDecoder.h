#pragma once

#include "feed/raw_ingest/RawPacket.h"

#include <string>

#include <boost/json.hpp>

namespace trading_engine::feed {

/**
 * @brief Result category after trying to decode a raw payload as JSON.
 *
 * JsonDecoder does not understand Polymarket event semantics.
 * It only answers:
 *
 * - is this valid JSON?
 * - is it a JSON object?
 * - is it a JSON array?
 * - is it a known non-JSON control message like PONG?
 * - is it malformed?
 */
enum class JsonDecodeStatus {
    /**
     * @brief Decoded payload is a JSON object.
     *
     * Example:
     *
     *     {"type":"book", ...}
     */
    JsonObject = 0,

    /**
     * @brief Decoded payload is a JSON array.
     *
     * Example:
     *
     *     [{"type":"book", ...}]
     *
     * Polymarket real data has already shown array-wrapped packets, so this
     * must be supported.
     */
    JsonArray,

    /**
     * @brief Payload is a recognized non-JSON control message.
     *
     * Example:
     *
     *     PONG
     *     pong
     *
     * This is not a decode error. It is normal WebSocket control traffic.
     */
    NonJsonControl,

    /**
     * @brief Payload is valid JSON, but not an object/array/control string.
     *
     * Example:
     *
     *     123
     *     true
     *     "hello"
     *
     * This is unusual for market data.
     */
    UnsupportedJson,

    /**
     * @brief Payload is not valid JSON and not a recognized control message.
     */
    MalformedJson
};

/**
 * @brief Output of JsonDecoder.
 *
 * This deliberately contains no market-specific normalized event fields.
 * EventNormalizer consumes this result and performs semantic mapping.
 */
struct JsonDecodeResult {
    JsonDecodeStatus status{JsonDecodeStatus::MalformedJson};

    /**
     * @brief Parsed JSON value.
     *
     * Valid when status is JsonObject, JsonArray, or UnsupportedJson.
     */
    boost::json::value json;

    /**
     * @brief Original control payload.
     *
     * Valid when status is NonJsonControl.
     */
    std::string control_payload;

    /**
     * @brief Error or diagnostic message.
     *
     * For MalformedJson, this explains why parsing failed.
     * For UnsupportedJson, this explains what kind of JSON was seen.
     */
    std::string message;

    /**
     * @brief True if payload was handled successfully.
     *
     * NonJsonControl is considered ok because heartbeat/control payloads are
     * valid runtime messages.
     */
    [[nodiscard]] bool ok() const noexcept;

    /**
     * @brief True if this result contains object/array JSON suitable for
     * EventNormalizer::normalize_json().
     */
    [[nodiscard]] bool has_json_event_payload() const noexcept;

    /**
     * @brief True if this result contains a control payload suitable for
     * EventNormalizer::normalize_control().
     */
    [[nodiscard]] bool has_control_payload() const noexcept;
};

/**
 * @brief Thin JSON parser/classifier for raw packet payloads.
 *
 * JsonDecoder responsibilities:
 *
 * - parse payload text as JSON;
 * - identify object payloads;
 * - identify array-wrapped payloads;
 * - classify PONG/ping-like control payloads;
 * - report malformed JSON without throwing.
 *
 * JsonDecoder must NOT:
 *
 * - map "book" to Snapshot;
 * - map "price_change" to Delta;
 * - update state;
 * - write logs;
 * - reconnect sources.
 */
class JsonDecoder {
public:
    using Json = boost::json::value;

    /**
     * @brief Decode payload from a RawPacket.
     *
     * The RawPacket metadata is not modified.
     */
    [[nodiscard]] JsonDecodeResult decode(const RawPacket& packet) const;

    /**
     * @brief Decode a raw payload string directly.
     *
     * Useful for unit tests.
     */
    [[nodiscard]] JsonDecodeResult decode_payload(
        const std::string& payload
    ) const;

    /**
     * @brief Return true if payload is recognized as control message.
     */
    [[nodiscard]] static bool is_control_payload(
        const std::string& payload
    );

private:
    /**
     * @brief Trim leading/trailing whitespace.
     */
    [[nodiscard]] static std::string trim_copy(const std::string& input);

    /**
     * @brief Convert parsed JSON string to control payload if recognized.
     *
     * Example:
     *
     *     "PONG"
     *
     * is valid JSON string, but semantically it is heartbeat/control.
     */
    [[nodiscard]] static bool is_control_string_json(const Json& json);
};

/**
 * @brief Convert decode status to string for tests/tools.
 */
[[nodiscard]] std::string to_string(JsonDecodeStatus status);

}  // namespace trading_engine::feed
