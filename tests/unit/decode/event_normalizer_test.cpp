#include "decode/json/JsonDecoder.h"
#include "decode/normalize/EventNormalizer.h"
#include "decode/public/DecodeTypes.h"
#include "decode/public/NormalizedEvent.h"

#include <boost/json.hpp>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

using trading_engine::decode::EventNormalizer;
using trading_engine::decode::DecodeInputView;
using trading_engine::decode::JsonDecodeKind;
using trading_engine::decode::JsonDecoder;
using trading_engine::decode::NormalizedEventType;
using trading_engine::decode::NormalizedSide;
using trading_engine::decode::SourceId;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
    }
}

template <typename Actual, typename Expected>
void expect_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& field
) {
    if (!(actual == expected)) {
        fail("mismatch: " + field);
    }
}

DecodeInputView input_view() {
    return DecodeInputView{
        .packet_id = 1,
        .connection_id = 1,
        .recv_wall_ns = 100,
        .recv_monotonic_ns = 200,
        .source_id = SourceId::PolymarketMarket,
        .payload = {}
    };
}

boost::json::value parse_json(const std::string& payload) {
    return boost::json::parse(payload);
}

void EventNormalizer_BookToSnapshot() {
    EventNormalizer normalizer;
    const std::string payload =
        R"({"event_type":"book","asset_id":"asset_x","market":"market_x","bids":[{"price":"0.42","size":"10"}],"asks":[{"price":"0.58","size":"11"}]})"
    ;

    const auto result = normalizer.normalize_json(
        input_view(),
        parse_json(payload)
    );

    expect_true(result.ok(), "normalization ok");
    expect_equal(result.events.size(), 1U, "event count");

    const auto& event = result.events.front();
    expect_equal(event.event_type, NormalizedEventType::Snapshot, "type");
    expect_equal(event.raw_type, std::string{"book"}, "raw_type");
    expect_equal(event.entity_id, std::string{"asset_x"}, "entity_id");
    expect_equal(event.bids.size(), 1U, "bid count");
    expect_equal(event.asks.size(), 1U, "ask count");
}

void EventNormalizer_PriceChangeToDelta() {
    EventNormalizer normalizer;
    const std::string payload =
        R"({"event_type":"price_change","price_changes":[{"asset_id":"asset_x","price":"0.42","size":"10","side":"BUY"}],"market":"market_x"})"
    ;

    const auto result = normalizer.normalize_json(
        input_view(),
        parse_json(payload)
    );

    expect_true(result.ok(), "normalization ok");
    expect_equal(result.events.size(), 1U, "event count");

    const auto& event = result.events.front();
    expect_equal(event.event_type, NormalizedEventType::Delta, "type");
    expect_equal(event.raw_type, std::string{"price_change"}, "raw_type");
    expect_equal(event.entity_id, std::string{"asset_x"}, "entity_id");
    expect_equal(event.changes.size(), 1U, "change count");
    expect_equal(event.changes.front().side, NormalizedSide::Bid, "side");
}

void EventNormalizer_ControlPongToHeartbeat() {
    EventNormalizer normalizer;

    const auto result = normalizer.normalize_control(input_view(), "PONG");

    expect_true(result.ok(), "normalization ok");
    expect_equal(result.events.size(), 1U, "event count");
    expect_equal(
        result.events.front().event_type,
        NormalizedEventType::Heartbeat,
        "type"
    );
    expect_equal(result.events.front().raw_type, std::string{"PONG"}, "raw_type");
}

void EventNormalizer_UnknownToUnknown() {
    EventNormalizer normalizer;
    const std::string payload =
        R"({"event_type":"new_polymarket_shape","asset_id":"asset_x"})"
    ;

    const auto result = normalizer.normalize_json(
        input_view(),
        parse_json(payload)
    );

    expect_true(result.ok(), "normalization ok");
    expect_equal(result.events.size(), 1U, "event count");
    expect_equal(
        result.events.front().event_type,
        NormalizedEventType::Unknown,
        "type"
    );
    expect_false(result.events.front().warnings.empty(), "warnings");
}

void EventNormalizer_ArrayWrappedPayloadWorks() {
    EventNormalizer normalizer;
    const std::string payload =
        R"([{"event_type":"book","asset_id":"asset_x","bids":[],"asks":[]}])"
    ;

    const auto result = normalizer.normalize_json(
        input_view(),
        parse_json(payload)
    );

    expect_true(result.ok(), "normalization ok");
    expect_equal(result.events.size(), 1U, "event count");
    expect_equal(
        result.events.front().event_type,
        NormalizedEventType::Snapshot,
        "type"
    );
    expect_equal(result.events.front().entity_id, std::string{"asset_x"}, "id");
}

void EventNormalizer_OnePacketCanEmitMultipleEvents() {
    EventNormalizer normalizer;
    const std::string payload =
        R"([{"event_type":"book","asset_id":"asset_x","bids":[],"asks":[]},{"event_type":"price_change","price_changes":[{"asset_id":"asset_x","price":"0.42","size":"10","side":"BUY"}]}])"
    ;

    const auto result = normalizer.normalize_json(
        input_view(),
        parse_json(payload)
    );

    expect_true(result.ok(), "normalization ok");
    expect_equal(result.events.size(), 2U, "event count");
    expect_equal(
        result.events[0].event_type,
        NormalizedEventType::Snapshot,
        "first type"
    );
    expect_equal(
        result.events[1].event_type,
        NormalizedEventType::Delta,
        "second type"
    );
}

void EventNormalizer_MalformedJsonDoesNotCrash() {
    JsonDecoder decoder;

    const auto decoded = decoder.decode_payload(R"({"event_type":)");

    expect_equal(decoded.kind, JsonDecodeKind::MalformedJson, "kind");
    expect_false(decoded.ok(), "decode ok");
    expect_false(decoded.has_json_event_payload(), "json event payload");
}

void PriceChange_UsesNestedAssetId() {
    EventNormalizer normalizer;
    const std::string payload =
        R"({"market":"market_x","event_type":"price_change","price_changes":[{"asset_id":"asset_x","price":"0.42","size":"10","side":"BUY"}]})"
    ;

    const auto result = normalizer.normalize_json(
        input_view(),
        parse_json(payload)
    );

    expect_true(result.ok(), "normalization ok");
    expect_equal(result.events.size(), 1U, "event count");
    expect_equal(result.events.front().asset_id, std::string{"asset_x"}, "asset");
    expect_equal(result.events.front().entity_id, std::string{"asset_x"}, "id");
}

void PriceChange_DoesNotUseTopLevelMarketAsEntityId() {
    EventNormalizer normalizer;
    const std::string payload =
        R"({"market":"market_x","event_type":"price_change","price_changes":[{"asset_id":"asset_x","price":"0.42","size":"10","side":"BUY"}]})"
    ;

    const auto result = normalizer.normalize_json(
        input_view(),
        parse_json(payload)
    );

    expect_true(result.ok(), "normalization ok");
    expect_equal(result.events.size(), 1U, "event count");
    expect_false(result.events.front().entity_id == "market_x", "market entity");
    expect_equal(result.events.front().market_id, std::string{"market_x"}, "market");
}

void PriceChange_GroupsOnlySameAssetChanges() {
    EventNormalizer normalizer;
    const std::string payload =
        R"({"market":"market_x","event_type":"price_change","price_changes":[{"asset_id":"asset_x","price":"0.42","size":"10","side":"BUY"},{"asset_id":"asset_x","price":"0.41","size":"12","side":"BUY"},{"asset_id":"asset_y","price":"0.58","size":"11","side":"SELL"}]})"
    ;

    const auto result = normalizer.normalize_json(
        input_view(),
        parse_json(payload)
    );

    expect_true(result.ok(), "normalization ok");
    expect_equal(result.events.size(), 1U, "event count");
    expect_equal(result.events.front().entity_id, std::string{"asset_x"}, "id");
    expect_equal(result.events.front().changes.size(), 2U, "change count");
}

void PriceChange_MultipleAssetsDoNotPolluteOneDelta() {
    EventNormalizer normalizer;
    const std::string payload =
        R"({"market":"market_x","event_type":"price_change","price_changes":[{"asset_id":"asset_x","price":"0.42","size":"10","side":"BUY"},{"asset_id":"asset_y","price":"0.58","size":"11","side":"SELL"}]})"
    ;

    const auto result = normalizer.normalize_json(
        input_view(),
        parse_json(payload)
    );

    expect_true(result.ok(), "normalization ok");
    expect_equal(result.events.size(), 1U, "event count");
    expect_equal(result.events.front().entity_id, std::string{"asset_x"}, "id");
    expect_equal(result.events.front().changes.size(), 1U, "change count");
    expect_equal(result.events.front().changes.front().price, 0.42, "price");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"EventNormalizer_BookToSnapshot", &EventNormalizer_BookToSnapshot},
        {"EventNormalizer_PriceChangeToDelta", &EventNormalizer_PriceChangeToDelta},
        {"EventNormalizer_ControlPongToHeartbeat", &EventNormalizer_ControlPongToHeartbeat},
        {"EventNormalizer_UnknownToUnknown", &EventNormalizer_UnknownToUnknown},
        {"EventNormalizer_ArrayWrappedPayloadWorks", &EventNormalizer_ArrayWrappedPayloadWorks},
        {"EventNormalizer_OnePacketCanEmitMultipleEvents", &EventNormalizer_OnePacketCanEmitMultipleEvents},
        {"EventNormalizer_MalformedJsonDoesNotCrash", &EventNormalizer_MalformedJsonDoesNotCrash},
        {"PriceChange_UsesNestedAssetId", &PriceChange_UsesNestedAssetId},
        {"PriceChange_DoesNotUseTopLevelMarketAsEntityId", &PriceChange_DoesNotUseTopLevelMarketAsEntityId},
        {"PriceChange_GroupsOnlySameAssetChanges", &PriceChange_GroupsOnlySameAssetChanges},
        {"PriceChange_MultipleAssetsDoNotPolluteOneDelta", &PriceChange_MultipleAssetsDoNotPolluteOneDelta}
    };

    return test_map;
}

int run_test(const std::string& name) {
    const auto it = tests().find(name);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << name << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << name << " failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << name << " passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        return run_test(argv[1]);
    }

    int failures = 0;
    for (const auto& [name, _] : tests()) {
        failures += run_test(name) == 0 ? 0 : 1;
    }

    return failures == 0 ? 0 : 1;
}
