#include "decode/core/DecodePipeline.h"
#include "decode/public/NormalizedEventBatch.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "feed/state/EntityStateStore.h"

#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::decode::DecodePipeline;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::feed::EntityStateStore;
using trading_engine::feed::RawLogReadResult;
using trading_engine::feed::RawLogReader;
using trading_engine::feed::to_decode_input_view;

struct FeedPipelineSummary {
    std::uint64_t packets_read{0};
    std::uint64_t events_applied{0};
    std::uint64_t state_errors{0};
    std::uint64_t entity_count{0};
    std::uint64_t global_hash{0};
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
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

std::string market39_fixture_path() {
    constexpr const char* candidates[] = {
        "tests/fixtures/polymarket/market_39.raw",
        "../tests/fixtures/polymarket/market_39.raw"
    };

    for (const char* candidate : candidates) {
        std::ifstream in(candidate, std::ios::binary);
        if (in.good()) {
            return candidate;
        }
    }

    return candidates[0];
}

FeedPipelineSummary replay_market39_through_pipeline() {
    RawLogReader reader(market39_fixture_path());
    DecodePipeline pipeline;
    EntityStateStore store;
    FeedPipelineSummary summary;

    while (true) {
        RawLogReadResult raw = reader.next();
        if (raw.eof()) {
            break;
        }

        if (!raw.ok()) {
            fail("raw read failed: " + raw.message);
        }

        ++summary.packets_read;

        NormalizedEventBatch batch;
        const auto decoded = pipeline.decode(
            to_decode_input_view(*raw.packet),
            &batch
        );

        if (!decoded.ok()) {
            fail("decode pipeline failed: " + decoded.error.message);
        }

        for (const auto& event : batch.events) {
            const auto applied = store.apply(event);
            if (applied.ok()) {
                ++summary.events_applied;
            }
        }
    }

    summary.state_errors = store.errors();
    summary.entity_count = static_cast<std::uint64_t>(store.entity_count());
    summary.global_hash = store.global_hash();

    return summary;
}

void FeedE2E_UsesDecodePipelineMarket39StateStable() {
    const auto summary = replay_market39_through_pipeline();

    expect_equal(summary.packets_read, 39ULL, "packets_read");
    expect_equal(summary.events_applied, 39ULL, "events_applied");
    expect_equal(summary.state_errors, 0ULL, "state_errors");
    expect_equal(summary.entity_count, 1ULL, "entity_count");
    expect_equal(
        summary.global_hash,
        12959912045291989833ULL,
        "global_hash"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"FeedE2E_UsesDecodePipelineMarket39StateStable",
         &FeedE2E_UsesDecodePipelineMarket39StateStable}
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
