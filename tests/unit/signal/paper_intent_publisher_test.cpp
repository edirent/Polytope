#include "engine/signal/publish/CapturingIntentPublisher.h"
#include "engine/signal/publish/JsonlIntentWriter.h"
#include "engine/signal/publish/PaperIntentPublisher.h"

#include <boost/json.hpp>

#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::oracle::Side;
using trading_engine::signal::CapturingIntentPublisher;
using trading_engine::signal::IntentStatus;
using trading_engine::signal::JsonlIntentWriter;
using trading_engine::signal::OpportunityIntent;
using trading_engine::signal::PaperIntentPublisher;

namespace json = boost::json;

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

std::uint64_t json_u64(const json::value& value) {
    if (value.is_uint64()) {
        return value.as_uint64();
    }
    if (value.is_int64() && value.as_int64() >= 0) {
        return static_cast<std::uint64_t>(value.as_int64());
    }
    fail("json value is not non-negative integer");
}

OpportunityIntent intent(std::uint64_t intent_id) {
    OpportunityIntent out;
    out.intent_id = intent_id;
    out.bundle_id = 42;
    out.status = IntentStatus::PaperOpportunity;
    out.valid_under_settlement = true;
    out.passed_quality_gate = true;
    out.enough_depth = true;
    out.guaranteed_payout_tick = 1'000'000;
    out.estimated_cost_tick = 900'000;
    out.estimated_fee_tick = 1'000;
    out.latency_buffer_tick = 2'000;
    out.estimated_edge_tick = 97'000;
    out.min_edge_tick = 50'000;
    out.snapshot_version_hash = 123;
    out.oracle_artifact_version = 1;
    out.leg_count = 1;
    out.legs[0].market_id = "m1";
    out.legs[0].asset_id = "asset_yes";
    out.legs[0].side = Side::Buy;
    out.legs[0].quantity_lots = 10;
    out.legs[0].estimated_vwap_tick = 500'000;
    out.legs[0].worst_price_tick = 510'000;
    out.legs[0].estimated_cost_tick = 5'000'000;
    out.legs[0].enough_depth = true;
    return out;
}

void CapturingIntentPublisher_CapturesIntent() {
    CapturingIntentPublisher publisher;
    publisher.publish(intent(1));

    expect_equal(publisher.intents().size(), 1U, "intent count");
    expect_equal(publisher.intents()[0].intent_id, 1ULL, "intent id");
}

void CapturingIntentPublisher_PreservesOrder() {
    CapturingIntentPublisher publisher;
    publisher.publish(intent(2));
    publisher.publish(intent(1));
    publisher.publish(intent(3));

    expect_equal(publisher.intents().size(), 3U, "intent count");
    expect_equal(publisher.intents()[0].intent_id, 2ULL, "intent 0");
    expect_equal(publisher.intents()[1].intent_id, 1ULL, "intent 1");
    expect_equal(publisher.intents()[2].intent_id, 3ULL, "intent 2");
}

void JsonlIntentWriter_WritesValidJsonLine() {
    std::ostringstream output;
    JsonlIntentWriter writer(&output);

    expect_true(writer.write(intent(7)), "write");

    const auto parsed = json::parse(output.str());
    expect_true(parsed.is_object(), "json object");
    const auto& object = parsed.as_object();
    expect_equal(json_u64(object.at("intent_id")), 7ULL, "intent_id");
    expect_equal(json_u64(object.at("bundle_id")), 42ULL, "bundle_id");
    expect_equal(
        json::value_to<std::string>(object.at("status")),
        std::string{"PaperOpportunity"},
        "status"
    );
    expect_equal(
        object.at("estimated_edge_tick").as_int64(),
        97'000LL,
        "edge"
    );
    expect_true(object.at("legs").is_array(), "legs array");
    expect_equal(object.at("legs").as_array().size(), 1U, "leg count");
}

void JsonlIntentWriter_DoesNotRequireExecution() {
    std::ostringstream output;
    JsonlIntentWriter writer(&output);
    PaperIntentPublisher publisher(&writer);

    publisher.publish(intent(9));

    const auto parsed = json::parse(output.str());
    expect_equal(
        json_u64(parsed.as_object().at("intent_id")),
        9ULL,
        "intent_id"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "CapturingIntentPublisher_CapturesIntent",
            &CapturingIntentPublisher_CapturesIntent
        },
        {
            "CapturingIntentPublisher_PreservesOrder",
            &CapturingIntentPublisher_PreservesOrder
        },
        {
            "JsonlIntentWriter_WritesValidJsonLine",
            &JsonlIntentWriter_WritesValidJsonLine
        },
        {
            "JsonlIntentWriter_DoesNotRequireExecution",
            &JsonlIntentWriter_DoesNotRequireExecution
        }
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

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }
    return run_test(argv[1]);
}
