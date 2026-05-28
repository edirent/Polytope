#include "oracle/ingestion/MarketDescriptionLoader.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::oracle::MarketDescriptionLoader;

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

std::filesystem::path temp_file(const std::string& name) {
    return std::filesystem::temp_directory_path() / name;
}

void write_file(const std::filesystem::path& path, const std::string& data) {
    std::ofstream out(path);
    if (!out) {
        fail("failed to open temp file");
    }
    out << data;
}

void RawMarketRecord_LoadsFixture() {
    MarketDescriptionLoader loader;
    const std::filesystem::path fixture_path =
        std::filesystem::path{POLYTOPE_SOURCE_DIR} /
        "tests/fixtures/oracle/raw_markets_small.jsonl";
    const auto result = loader.load_jsonl(
        fixture_path.string()
    );

    expect_true(result.ok(), "load ok");
    expect_equal(result.records.size(), 1U, "record count");

    const auto& record = result.records.front();
    expect_equal(record.market_id, std::string{"m1"}, "market_id");
    expect_equal(record.event_id, std::string{"e1"}, "event_id");
    expect_equal(record.outcomes.size(), 2U, "outcome count");
    expect_equal(record.asset_ids.size(), 2U, "asset count");
    expect_equal(record.asset_ids[0], std::string{"asset_yes"}, "asset yes");
    expect_equal(record.fetched_at_ns, 1ULL, "fetched_at_ns");
}

void RawMarketRecord_RejectsMissingMarketId() {
    const auto path = temp_file("oracle_missing_market_id.jsonl");
    write_file(
        path,
        R"({"event_id":"e1","title":"Winner?","outcomes":["YES"],"asset_ids":["asset_yes"]})"
        "\n"
    );

    MarketDescriptionLoader loader;
    const auto result = loader.load_jsonl(path.string());

    expect_false(result.ok(), "load ok");
    expect_false(result.errors.empty(), "errors");
    std::filesystem::remove(path);
}

void RawMarketRecord_RejectsMismatchedOutcomeAssetCount() {
    const auto path = temp_file("oracle_mismatched_assets.jsonl");
    write_file(
        path,
        R"({"market_id":"m1","event_id":"e1","title":"Winner?","outcomes":["YES","NO"],"asset_ids":["asset_yes"]})"
        "\n"
    );

    MarketDescriptionLoader loader;
    const auto result = loader.load_jsonl(path.string());

    expect_false(result.ok(), "load ok");
    expect_false(result.errors.empty(), "errors");
    std::filesystem::remove(path);
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"RawMarketRecord_LoadsFixture", &RawMarketRecord_LoadsFixture},
        {
            "RawMarketRecord_RejectsMissingMarketId",
            &RawMarketRecord_RejectsMissingMarketId
        },
        {
            "RawMarketRecord_RejectsMismatchedOutcomeAssetCount",
            &RawMarketRecord_RejectsMismatchedOutcomeAssetCount
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
