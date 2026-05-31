#include "decode/core/DecodePipeline.h"
#include "decode/json/JsonDecodeResult.h"
#include "decode/public/NormalizedEventBatch.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "state/EntityStateStore.h"
#include "state/core/StateHashPolicy.h"

#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

namespace decode = trading_engine::decode;
namespace feed = trading_engine::feed;
namespace state = trading_engine::state;

constexpr std::uint64_t kMarket39LegacyBookHash = 12959912045291989833ULL;

struct ReplaySummary {
    std::uint64_t packets_read{0};
    std::uint64_t normalized_events{0};
    std::uint64_t state_errors{0};
    std::uint64_t entity_hash_checks{0};
    std::uint64_t global_hash_checks{0};
    std::uint64_t hash_mismatches{0};
    std::uint64_t final_global_hash{0};
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

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
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

ReplaySummary replay_market39(state::StateHashMode mode) {
    state::StateRuntimeConfig runtime_config;
    runtime_config.hash_mode = mode;

    decode::DecodePipeline pipeline;
    feed::RawLogReader reader(market39_fixture_path());
    state::EntityStateStore store(runtime_config);
    ReplaySummary summary;
    std::set<std::string> entity_ids;

    while (true) {
        auto raw = reader.next();
        if (raw.eof()) {
            break;
        }
        if (!raw.ok()) {
            fail("raw read failed: " + raw.message);
        }

        ++summary.packets_read;
        decode::NormalizedEventBatch batch;
        const auto decoded = pipeline.decode(
            feed::to_decode_input_view(*raw.packet),
            &batch
        );
        if (!decoded.ok() &&
            decoded.payload_kind != decode::JsonDecodeKind::NonJsonControl) {
            fail("decode failed");
        }

        summary.normalized_events += static_cast<std::uint64_t>(batch.size());
        for (const auto& event : batch.events) {
            const auto result = store.apply(event);
            if (!result.ok()) {
                ++summary.state_errors;
            }

            if (result.entity_id.empty()) {
                continue;
            }
            entity_ids.insert(result.entity_id);

            if (mode == state::StateHashMode::DebugVerify &&
                result.mutation.state_changed) {
                const auto cached_entity = store.state_hash(result.entity_id);
                const auto recomputed_entity =
                    store.debug_recomputed_state_hash(result.entity_id);
                ++summary.entity_hash_checks;
                if (cached_entity != recomputed_entity) {
                    ++summary.hash_mismatches;
                }

                const auto cached_global = store.global_hash();
                const auto recomputed_global =
                    store.debug_recomputed_global_hash();
                ++summary.global_hash_checks;
                if (cached_global != recomputed_global) {
                    ++summary.hash_mismatches;
                }
            }
        }
    }

    for (const auto& entity_id : entity_ids) {
        const auto cached = store.state_hash(entity_id);
        const auto recomputed = store.debug_recomputed_state_hash(entity_id);
        ++summary.entity_hash_checks;
        if (cached != recomputed) {
            ++summary.hash_mismatches;
        }
    }

    summary.final_global_hash = store.global_hash();
    ++summary.global_hash_checks;
    if (summary.final_global_hash != store.debug_recomputed_global_hash()) {
        ++summary.hash_mismatches;
    }
    return summary;
}

void StateHashCacheDeterminism_DebugVerifyMatchesFullRecompute() {
    const auto summary = replay_market39(state::StateHashMode::DebugVerify);

    expect_equal(summary.packets_read, 39ULL, "packets_read");
    expect_equal(summary.normalized_events, 39ULL, "normalized_events");
    expect_equal(summary.state_errors, 0ULL, "state_errors");
    expect_true(summary.entity_hash_checks > 0, "entity hash checks");
    expect_true(summary.global_hash_checks > 0, "global hash checks");
    expect_equal(summary.hash_mismatches, 0ULL, "hash mismatches");
    expect_equal(
        summary.final_global_hash,
        kMarket39LegacyBookHash,
        "legacy book hash"
    );
}

void StateHashCacheDeterminism_ReplayFullPreservesLegacyBookHash() {
    const auto summary = replay_market39(state::StateHashMode::ReplayFull);

    expect_equal(summary.packets_read, 39ULL, "packets_read");
    expect_equal(summary.normalized_events, 39ULL, "normalized_events");
    expect_equal(summary.state_errors, 0ULL, "state_errors");
    expect_equal(summary.hash_mismatches, 0ULL, "hash mismatches");
    expect_equal(
        summary.final_global_hash,
        kMarket39LegacyBookHash,
        "legacy book hash"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"StateHashCacheDeterminism_DebugVerifyMatchesFullRecompute",
         &StateHashCacheDeterminism_DebugVerifyMatchesFullRecompute},
        {"StateHashCacheDeterminism_ReplayFullPreservesLegacyBookHash",
         &StateHashCacheDeterminism_ReplayFullPreservesLegacyBookHash}
    };
    return test_map;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            fail("expected exactly one test name");
        }

        const auto it = tests().find(argv[1]);
        if (it == tests().end()) {
            fail(std::string("unknown test: ") + argv[1]);
        }

        it->second();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
