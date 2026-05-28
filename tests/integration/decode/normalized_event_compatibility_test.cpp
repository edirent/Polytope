#include "decode/public/NormalizedEvent.h"
#include "feed/decode/EventNormalizer.h"
#include "feed/raw_ingest/RawPacket.h"

#include <boost/json.hpp>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace {

using trading_engine::decode::NormalizedEvent;
using trading_engine::decode::NormalizedEventType;
using trading_engine::feed::EventNormalizer;
using trading_engine::feed::RawPacket;
using trading_engine::feed::SourceId;
using trading_engine::feed::make_raw_packet;

static_assert(
    std::is_same_v<trading_engine::feed::NormalizedEvent, NormalizedEvent>
);
static_assert(
    std::is_same_v<
        trading_engine::feed::NormalizedEventType,
        NormalizedEventType
    >
);

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
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

RawPacket packet_with_payload(std::string payload) {
    return make_raw_packet(
        SourceId::PolymarketMarket,
        1,
        1,
        std::move(payload)
    );
}

void FeedEventNormalizer_EmitsDecodePublicNormalizedEvent() {
    EventNormalizer normalizer;
    const auto packet = packet_with_payload(
        R"({"event_type":"book","asset_id":"asset_x","market":"market_x","bids":[],"asks":[]})"
    );
    const auto parsed = boost::json::parse(packet.payload);

    const auto result = normalizer.normalize_json(packet, parsed);

    expect_true(result.ok(), "normalization ok");
    expect_equal(result.events.size(), 1U, "event count");

    const NormalizedEvent& event = result.events.front();
    expect_equal(event.event_type, NormalizedEventType::Snapshot, "event type");
    expect_equal(event.entity_id, std::string{"asset_x"}, "entity id");
    expect_equal(event.asset_id, std::string{"asset_x"}, "asset id");
    expect_equal(event.market_id, std::string{"market_x"}, "market id");
}

void FeedNormalizedEventAlias_IsDecodePublicType() {
    trading_engine::feed::NormalizedEvent event;
    event.event_type = NormalizedEventType::Heartbeat;
    event.raw_type = "PONG";

    NormalizedEvent& decode_event = event;

    expect_equal(
        decode_event.event_type,
        NormalizedEventType::Heartbeat,
        "event type"
    );
    expect_equal(decode_event.raw_type, std::string{"PONG"}, "raw type");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"FeedEventNormalizer_EmitsDecodePublicNormalizedEvent",
         &FeedEventNormalizer_EmitsDecodePublicNormalizedEvent},
        {"FeedNormalizedEventAlias_IsDecodePublicType",
         &FeedNormalizedEventAlias_IsDecodePublicType}
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
