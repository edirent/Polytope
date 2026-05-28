#include "decode/normalize/EventNormalizer.h"

#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace trading_engine::decode {

namespace {

using Json = boost::json::value;

std::string json_string_to_std(const boost::json::string& value) {
    return std::string(value.data(), value.size());
}

/**
 * @brief Return true if JSON object contains key.
 */
bool has_key(const Json& object, const char* key) {
    return object.is_object() && object.as_object().contains(key);
}

const Json& at_key(const Json& object, const char* key) {
    return object.as_object().at(key);
}

/**
 * @brief Try to read a string field.
 *
 * Polymarket fields are often strings, but this helper also accepts numbers
 * and booleans and converts them to strings for robustness.
 */
std::optional<std::string> get_string_field(
    const Json& object,
    const char* key
) {
    if (!has_key(object, key)) {
        return std::nullopt;
    }

    const auto& value = at_key(object, key);

    if (value.is_string()) {
        return json_string_to_std(value.as_string());
    }

    if (value.is_int64()) {
        return std::to_string(value.as_int64());
    }

    if (value.is_uint64()) {
        return std::to_string(value.as_uint64());
    }

    if (value.is_double()) {
        return std::to_string(value.as_double());
    }

    if (value.is_bool()) {
        return value.as_bool() ? "true" : "false";
    }

    return std::nullopt;
}

/**
 * @brief Try to read an unsigned integer timestamp-like field.
 *
 * Accepts both JSON numbers and numeric strings.
 */
std::uint64_t get_u64_field_or_zero(const Json& object, const char* key) {
    if (!has_key(object, key)) {
        return 0;
    }

    const auto& value = at_key(object, key);

    if (value.is_uint64()) {
        return value.as_uint64();
    }

    if (value.is_int64()) {
        const auto signed_value = value.as_int64();
        return signed_value < 0 ? 0 : static_cast<std::uint64_t>(signed_value);
    }

    if (value.is_string()) {
        try {
            return static_cast<std::uint64_t>(
                std::stoull(json_string_to_std(value.as_string()))
            );
        } catch (...) {
            return 0;
        }
    }

    return 0;
}

/**
 * @brief Try to parse a JSON value as double.
 *
 * Polymarket numeric fields are frequently strings:
 *
 *     "0.51"
 *
 * This helper accepts both string and numeric JSON values.
 */
std::optional<double> value_to_double(const Json& value) {
    if (value.is_double()) {
        return value.as_double();
    }

    if (value.is_int64()) {
        return static_cast<double>(value.as_int64());
    }

    if (value.is_uint64()) {
        return static_cast<double>(value.as_uint64());
    }

    if (value.is_string()) {
        try {
            return std::stod(json_string_to_std(value.as_string()));
        } catch (...) {
            return std::nullopt;
        }
    }

    return std::nullopt;
}

/**
 * @brief Try to read a double field from an object.
 */
std::optional<double> get_double_field(
    const Json& object,
    const char* key
) {
    if (!has_key(object, key)) {
        return std::nullopt;
    }

    return value_to_double(at_key(object, key));
}

/**
 * @brief Extract raw event type from Polymarket event object.
 *
 * Real APIs are not always perfectly consistent. Support both:
 *
 * - "type"
 * - "event_type"
 */
std::string extract_raw_type(const Json& object) {
    if (auto type = get_string_field(object, "type")) {
        return *type;
    }

    if (auto event_type = get_string_field(object, "event_type")) {
        return *event_type;
    }

    return {};
}

/**
 * @brief Extract best available entity id.
 *
 * Priority:
 *
 * 1. asset_id
 * 2. token_id
 * 3. condition_id
 * 4. market_id
 * 5. market
 * 6. event_id
 *
 * For Polymarket Market channel, asset_id should usually be the main entity.
 */
std::string extract_entity_id(const Json& object) {
    constexpr const char* keys[] = {
        "asset_id",
        "token_id",
        "condition_id",
        "market_id",
        "market",
        "event_id"
    };

    for (const char* key : keys) {
        if (auto value = get_string_field(object, key)) {
            if (!value->empty()) {
                return *value;
            }
        }
    }

    return {};
}

/**
 * @brief Parse side string into NormalizedSide.
 *
 * Polymarket may use BUY/SELL or bid/ask depending on event.
 */
NormalizedSide parse_side(const std::string& side) {
    if (side == "BUY" || side == "buy" || side == "BID" ||
        side == "bid" || side == "Bid") {
        return NormalizedSide::Bid;
    }

    if (side == "SELL" || side == "sell" || side == "ASK" ||
        side == "ask" || side == "Ask") {
        return NormalizedSide::Ask;
    }

    return NormalizedSide::Unknown;
}

/**
 * @brief Parse one book level.
 *
 * Supports both formats:
 *
 *     ["0.49", "100"]
 *
 * and:
 *
 *     {"price":"0.49","size":"100"}
 */
std::optional<BookLevel> parse_book_level(const Json& level) {
    if (level.is_array() && level.as_array().size() >= 2) {
        const auto& array = level.as_array();
        const auto price = value_to_double(array.at(0));
        const auto size = value_to_double(array.at(1));

        if (price && size) {
            return BookLevel{*price, *size};
        }

        return std::nullopt;
    }

    if (level.is_object()) {
        const auto price = get_double_field(level, "price");
        const auto size = get_double_field(level, "size");

        if (price && size) {
            return BookLevel{*price, *size};
        }

        return std::nullopt;
    }

    return std::nullopt;
}

/**
 * @brief Parse book side levels.
 */
std::vector<BookLevel> parse_book_levels(
    const Json& object,
    const char* key
) {
    std::vector<BookLevel> levels;

    if (!has_key(object, key)) {
        return levels;
    }

    const auto& raw_levels = at_key(object, key);

    if (!raw_levels.is_array()) {
        return levels;
    }

    for (const auto& raw_level : raw_levels.as_array()) {
        if (auto level = parse_book_level(raw_level)) {
            levels.push_back(*level);
        }
    }

    return levels;
}

/**
 * @brief Parse one price_change update.
 *
 * Supports:
 *
 *     {"side":"BUY","price":"0.50","size":"80"}
 */
std::optional<PriceLevelChange> parse_price_level_change(const Json& object) {
    if (!object.is_object()) {
        return std::nullopt;
    }

    auto side_string = get_string_field(object, "side");
    auto price = get_double_field(object, "price");
    auto size = get_double_field(object, "size");

    if (!side_string || !price || !size) {
        return std::nullopt;
    }

    return PriceLevelChange{
        parse_side(*side_string),
        *price,
        *size
    };
}

std::optional<std::string> first_price_change_asset_id(const Json& object) {
    if (!has_key(object, "price_changes") ||
        !at_key(object, "price_changes").is_array()) {
        return std::nullopt;
    }

    for (const auto& raw_change : at_key(object, "price_changes").as_array()) {
        if (!raw_change.is_object()) {
            continue;
        }

        if (auto asset_id = get_string_field(raw_change, "asset_id")) {
            if (!asset_id->empty()) {
                return asset_id;
            }
        }

        if (auto token_id = get_string_field(raw_change, "token_id")) {
            if (!token_id->empty()) {
                return token_id;
            }
        }
    }

    return std::nullopt;
}

bool price_change_belongs_to_entity(
    const Json& raw_change,
    const std::string& entity_id
) {
    if (entity_id.empty() || !raw_change.is_object()) {
        return true;
    }

    const std::string change_entity_id = extract_entity_id(raw_change);

    if (change_entity_id.empty()) {
        return true;
    }

    return change_entity_id == entity_id;
}

/**
 * @brief Return true if payload is recognized heartbeat text.
 */
bool is_pong_text(const std::string& payload) {
    return payload == "PONG" ||
           payload == "pong" ||
           payload == "\"PONG\"" ||
           payload == "\"pong\"";
}

/**
 * @brief Copy common Polymarket ids into event fields.
 */
void fill_common_ids(NormalizedEvent& event, const Json& object) {
    if (auto asset_id = get_string_field(object, "asset_id")) {
        event.asset_id = *asset_id;
    }

    if (auto token_id = get_string_field(object, "token_id");
        event.asset_id.empty() && token_id) {
        event.asset_id = *token_id;
    }

    if (auto market_id = get_string_field(object, "market_id")) {
        event.market_id = *market_id;
    }

    if (auto market = get_string_field(object, "market");
        event.market_id.empty() && market) {
        event.market_id = *market;
    }

    if (auto condition_id = get_string_field(object, "condition_id")) {
        event.condition_id = *condition_id;
    }

    event.entity_id = extract_entity_id(object);

    event.event_ts = get_u64_field_or_zero(object, "timestamp");

    if (event.event_ts == 0) {
        event.event_ts = get_u64_field_or_zero(object, "ts");
    }
}

}  // namespace

NormalizationResult EventNormalizer::normalize_json(
    const DecodeInputView& input,
    const Json& json
) const {
    NormalizationResult result;

    if (json.is_object()) {
        result.events.push_back(normalize_one_object(input, json));
        return result;
    }

    if (json.is_array()) {
        for (const auto& element : json.as_array()) {
            if (!element.is_object()) {
                result.warnings.push_back(
                    "array wrapper contains non-object element"
                );
                continue;
            }

            result.events.push_back(normalize_one_object(input, element));
        }

        return result;
    }

    result.error = "decoded JSON is neither object nor array";
    return result;
}

NormalizationResult EventNormalizer::normalize_control(
    const DecodeInputView& input,
    const std::string& payload
) const {
    NormalizationResult result;

    NormalizedEvent event = make_base_event(input);
    event.raw_type = payload;

    if (is_pong_text(payload)) {
        event.event_type = NormalizedEventType::Heartbeat;
    } else {
        event.event_type = NormalizedEventType::Unknown;
        event.warnings.push_back("unknown non-json control payload");
        result.warnings.push_back("unknown non-json control payload");
    }

    result.events.push_back(std::move(event));
    return result;
}

NormalizedEvent EventNormalizer::make_base_event(
    const DecodeInputView& input
) const {
    NormalizedEvent event;

    event.packet_id = input.packet_id;
    event.recv_wall_ns = input.recv_wall_ns;
    event.recv_monotonic_ns = input.recv_monotonic_ns;
    event.source_id = input.source_id;

    return event;
}

NormalizedEvent EventNormalizer::normalize_one_object(
    const DecodeInputView& input,
    const Json& object
) const {
    const std::string raw_type = extract_raw_type(object);

    if (raw_type == "book") {
        return normalize_book(input, object, raw_type);
    }

    if (raw_type == "price_change") {
        return normalize_price_change(input, object, raw_type);
    }

    if (raw_type == "best_bid_ask") {
        return normalize_best_bid_ask(input, object, raw_type);
    }

    if (raw_type == "tick_size_change") {
        return normalize_tick_size_change(input, object, raw_type);
    }

    if (raw_type == "new_market" || raw_type == "market_resolved") {
        return normalize_lifecycle(input, object, raw_type);
    }

    if (raw_type == "last_trade_price") {
        return normalize_trade(input, object, raw_type);
    }

    return normalize_unknown(input, object, raw_type);
}

NormalizedEvent EventNormalizer::normalize_book(
    const DecodeInputView& input,
    const Json& object,
    std::string raw_type
) const {
    NormalizedEvent event = make_base_event(input);

    event.event_type = NormalizedEventType::Snapshot;
    event.raw_type = std::move(raw_type);

    fill_common_ids(event, object);

    event.bids = parse_book_levels(object, "bids");
    event.asks = parse_book_levels(object, "asks");

    if (auto tick_size = get_double_field(object, "tick_size")) {
        event.tick_size = *tick_size;
    }

    if (event.entity_id.empty()) {
        event.warnings.push_back("book event missing entity id");
    }

    if (event.bids.empty() && event.asks.empty()) {
        event.warnings.push_back("book event has no bids and no asks");
    }

    return event;
}

NormalizedEvent EventNormalizer::normalize_price_change(
    const DecodeInputView& input,
    const Json& object,
    std::string raw_type
) const {
    NormalizedEvent event = make_base_event(input);

    event.event_type = NormalizedEventType::Delta;
    event.raw_type = std::move(raw_type);

    fill_common_ids(event, object);

    if (auto asset_id = first_price_change_asset_id(object)) {
        event.asset_id = *asset_id;
        event.entity_id = *asset_id;
    }

    /**
     * Polymarket price_change can appear either as:
     *
     *     {"type":"price_change", "side":"BUY", "price":"0.5", "size":"10"}
     *
     * or as:
     *
     *     {"type":"price_change", "changes":[{...}, {...}]}
     *
     * Real Polymarket market-channel payloads use:
     *
     *     {"event_type":"price_change", "price_changes":[{...}, {...}]}
     *
     * Support both.
     */
    if (has_key(object, "changes") && at_key(object, "changes").is_array()) {
        for (const auto& raw_change : at_key(object, "changes").as_array()) {
            if (auto change = parse_price_level_change(raw_change)) {
                event.changes.push_back(*change);
            }
        }
    } else if (has_key(object, "price_changes") &&
               at_key(object, "price_changes").is_array()) {
        for (const auto& raw_change : at_key(object, "price_changes").as_array()) {
            if (!price_change_belongs_to_entity(raw_change, event.entity_id)) {
                continue;
            }

            if (auto change = parse_price_level_change(raw_change)) {
                event.changes.push_back(*change);
            }
        }
    } else {
        if (auto change = parse_price_level_change(object)) {
            event.changes.push_back(*change);
        }
    }

    if (event.entity_id.empty()) {
        event.warnings.push_back("price_change event missing entity id");
    }

    if (event.changes.empty()) {
        event.warnings.push_back("price_change event has no valid changes");
    }

    return event;
}

NormalizedEvent EventNormalizer::normalize_best_bid_ask(
    const DecodeInputView& input,
    const Json& object,
    std::string raw_type
) const {
    NormalizedEvent event = make_base_event(input);

    event.event_type = NormalizedEventType::StatusChange;
    event.raw_type = std::move(raw_type);

    fill_common_ids(event, object);

    event.best_bid = get_double_field(object, "best_bid");
    event.best_ask = get_double_field(object, "best_ask");

    if (!event.best_bid) {
        event.best_bid = get_double_field(object, "bid");
    }

    if (!event.best_ask) {
        event.best_ask = get_double_field(object, "ask");
    }

    if (event.entity_id.empty()) {
        event.warnings.push_back("best_bid_ask event missing entity id");
    }

    if (!event.best_bid && !event.best_ask) {
        event.warnings.push_back("best_bid_ask event missing best_bid/best_ask");
    }

    return event;
}

NormalizedEvent EventNormalizer::normalize_tick_size_change(
    const DecodeInputView& input,
    const Json& object,
    std::string raw_type
) const {
    NormalizedEvent event = make_base_event(input);

    event.event_type = NormalizedEventType::StatusChange;
    event.raw_type = std::move(raw_type);

    fill_common_ids(event, object);

    event.tick_size = get_double_field(object, "tick_size");

    if (!event.tick_size) {
        event.tick_size = get_double_field(object, "new_tick_size");
    }

    if (event.entity_id.empty()) {
        event.warnings.push_back("tick_size_change event missing entity id");
    }

    return event;
}

NormalizedEvent EventNormalizer::normalize_lifecycle(
    const DecodeInputView& input,
    const Json& object,
    std::string raw_type
) const {
    NormalizedEvent event = make_base_event(input);

    event.event_type = NormalizedEventType::LifecycleEvent;
    event.raw_type = std::move(raw_type);

    fill_common_ids(event, object);

    if (auto winning_asset_id = get_string_field(object, "winning_asset_id")) {
        event.winning_asset_id = *winning_asset_id;
    }

    if (event.entity_id.empty()) {
        event.warnings.push_back("lifecycle event missing entity id");
    }

    return event;
}

NormalizedEvent EventNormalizer::normalize_trade(
    const DecodeInputView& input,
    const Json& object,
    std::string raw_type
) const {
    NormalizedEvent event = make_base_event(input);

    event.event_type = NormalizedEventType::TradeEvent;
    event.raw_type = std::move(raw_type);

    fill_common_ids(event, object);

    if (auto price = get_double_field(object, "price")) {
        event.best_bid = *price;
    }

    if (event.entity_id.empty()) {
        event.warnings.push_back("trade event missing entity id");
    }

    return event;
}

NormalizedEvent EventNormalizer::normalize_unknown(
    const DecodeInputView& input,
    const Json& object,
    std::string raw_type
) const {
    NormalizedEvent event = make_base_event(input);

    event.event_type = NormalizedEventType::Unknown;
    event.raw_type = std::move(raw_type);

    fill_common_ids(event, object);

    if (event.raw_type.empty()) {
        event.warnings.push_back("unknown event missing type/event_type");
    } else {
        event.warnings.push_back("unrecognized event type: " + event.raw_type);
    }

    return event;
}

}  // namespace trading_engine::decode
