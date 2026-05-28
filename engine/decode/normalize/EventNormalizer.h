#pragma once

#include "decode/public/NormalizedEvent.h"

#include <string>
#include <vector>

#include <boost/json.hpp>

namespace trading_engine::decode {

/**
 * @brief Result of normalizing one decode input packet view.
 *
 * A single packet view can produce multiple NormalizedEvent objects because
 * real Polymarket payloads may be array-wrapped:
 *
 *     [{"type":"book", ...}]
 *
 * Future payloads may contain arrays with more than one object.
 */
struct NormalizationResult {
    std::vector<NormalizedEvent> events;
    std::vector<std::string> warnings;

    /**
     * @brief Fatal error, if normalization could not proceed.
     *
     * Unknown event type is not fatal.
     */
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty();
    }
};

/**
 * @brief Convert decoded JSON/control payload into internal normalized events.
 *
 * EventNormalizer does venue-specific semantic mapping.
 *
 * It does NOT:
 *
 * - parse raw JSON text,
 * - write raw logs,
 * - update order books,
 * - perform consistency checks,
 * - reconnect sources.
 *
 * It only maps decoded payloads into internal event envelopes.
 */
class EventNormalizer {
public:
    using Json = boost::json::value;

    /**
     * @brief Normalize a JSON payload from one decode input packet view.
     *
     * Supports both:
     *
     * - object payload:
     *       {"type":"book", ...}
     *
     * - array wrapper:
     *       [{"type":"book", ...}]
     *
     * @param input Packet metadata source.
     * @param json Decoded JSON object or array.
     *
     * @return NormalizationResult containing zero or more events.
     */
    [[nodiscard]] NormalizationResult normalize_json(
        const DecodeInputView& input,
        const Json& json
    ) const;

    /**
     * @brief Normalize a non-JSON control payload.
     *
     * Examples:
     *
     *     PONG
     *     pong
     *     "PONG"
     *
     * @param input Packet metadata source.
     * @param payload Original non-JSON/control payload.
     *
     * @return Heartbeat event if recognized; Unknown otherwise.
     */
    [[nodiscard]] NormalizationResult normalize_control(
        const DecodeInputView& input,
        const std::string& payload
    ) const;

private:
    [[nodiscard]] NormalizedEvent make_base_event(
        const DecodeInputView& input
    ) const;

    [[nodiscard]] NormalizedEvent normalize_one_object(
        const DecodeInputView& input,
        const Json& object
    ) const;

    [[nodiscard]] NormalizedEvent normalize_book(
        const DecodeInputView& input,
        const Json& object,
        std::string raw_type
    ) const;

    [[nodiscard]] NormalizedEvent normalize_price_change(
        const DecodeInputView& input,
        const Json& object,
        std::string raw_type
    ) const;

    [[nodiscard]] NormalizedEvent normalize_best_bid_ask(
        const DecodeInputView& input,
        const Json& object,
        std::string raw_type
    ) const;

    [[nodiscard]] NormalizedEvent normalize_tick_size_change(
        const DecodeInputView& input,
        const Json& object,
        std::string raw_type
    ) const;

    [[nodiscard]] NormalizedEvent normalize_lifecycle(
        const DecodeInputView& input,
        const Json& object,
        std::string raw_type
    ) const;

    [[nodiscard]] NormalizedEvent normalize_trade(
        const DecodeInputView& input,
        const Json& object,
        std::string raw_type
    ) const;

    [[nodiscard]] NormalizedEvent normalize_unknown(
        const DecodeInputView& input,
        const Json& object,
        std::string raw_type
    ) const;
};

}  // namespace trading_engine::decode
