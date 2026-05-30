#include "oracle/ingestion/MarketApiClient.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::oracle::parse_polymarket_gamma_markets;

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

std::string gamma_fixture() {
    return R"json([
      {
        "id": "540817",
        "question": "Example market?",
        "conditionId": "0xcondition",
        "description": "This market resolves Yes if the event happens.",
        "outcomes": "[\"Yes\", \"No\"]",
        "clobTokenIds": "[\"12345678901234567890\", \"98765432109876543210\"]",
        "resolutionSource": "official source",
        "endDate": "2030-01-01T00:00:00Z",
        "enableOrderBook": true,
        "events": [
          {
            "id": "event_1",
            "title": "Example event",
            "description": "Event dependency text"
          }
        ]
      }
    ])json";
}

void MarketApiClient_ParsesGammaMarket() {
    const auto result = parse_polymarket_gamma_markets(gamma_fixture(), 7);

    expect_true(result.ok(), "parse ok");
    expect_equal(result.response_count, 1U, "response count");
    expect_equal(result.records.size(), 1U, "record count");

    const auto& record = result.records.front();
    expect_equal(record.market_id, std::string{"0xcondition"}, "market id");
    expect_equal(record.event_id, std::string{"event_1"}, "event id");
    expect_equal(record.title, std::string{"Example market?"}, "title");
    expect_equal(record.outcomes.size(), 2U, "outcome count");
    expect_equal(record.outcomes[0], std::string{"Yes"}, "yes outcome");
    expect_equal(record.asset_ids.size(), 2U, "asset count");
    expect_equal(
        record.asset_ids[0],
        std::string{"12345678901234567890"},
        "asset id preserved"
    );
    expect_equal(record.fetched_at_ns, 7ULL, "fetched_at_ns");
    expect_equal(
        record.source,
        std::string{"polymarket_gamma"},
        "source"
    );
    expect_true(
        record.description.find("Event dependency text") != std::string::npos,
        "event dependency text"
    );
}

void MarketApiClient_RejectsMalformedGammaResponse() {
    const auto result = parse_polymarket_gamma_markets("{", 7);

    expect_true(!result.ok(), "parse failed");
    expect_true(!result.errors.empty(), "errors");
}

void MarketApiClient_SkipsOrderbookDisabledMarket() {
    const auto result = parse_polymarket_gamma_markets(R"json([
      {
        "id": "1",
        "question": "No order book",
        "conditionId": "0xdisabled",
        "outcomes": "[\"Yes\", \"No\"]",
        "clobTokenIds": "[\"1\", \"2\"]",
        "enableOrderBook": false
      }
    ])json", 7);

    expect_true(result.ok(), "parse ok");
    expect_true(result.records.empty(), "disabled skipped");
    expect_true(!result.warnings.empty(), "warning");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"MarketApiClient_ParsesGammaMarket", &MarketApiClient_ParsesGammaMarket},
        {
            "MarketApiClient_RejectsMalformedGammaResponse",
            &MarketApiClient_RejectsMalformedGammaResponse
        },
        {
            "MarketApiClient_SkipsOrderbookDisabledMarket",
            &MarketApiClient_SkipsOrderbookDisabledMarket
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
