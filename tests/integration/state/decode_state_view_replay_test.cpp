#include "decode/core/DecodePipeline.h"
#include "decode/json/JsonDecodeResult.h"
#include "decode/public/NormalizedEventBatch.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "state/MarketStateView.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateStore.h"

#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::decode::DecodePipeline;
using trading_engine::decode::JsonDecodeKind;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::decode::NormalizedEventType;
using trading_engine::feed::RawLogReadResult;
using trading_engine::feed::RawLogReader;
using trading_engine::feed::to_decode_input_view;
using trading_engine::state::MarketStateView;
using trading_engine::state::MarketStateStore;
using trading_engine::state::from_normalized_batch;

struct ReplaySummary {
    std::uint64_t packets_read{0};
    std::uint64_t normalized_events{0};
    std::uint64_t state_errors{0};
    std::uint64_t entity_count{0};
    std::uint64_t global_hash{0};
    std::string entity_id;
};

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

ReplaySummary replay_market39(MarketStateStore& store) {
    DecodePipeline pipeline;
    RawLogReader reader(market39_fixture_path());
    ReplaySummary summary;

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

        if (!decoded.ok() &&
            decoded.payload_kind != JsonDecodeKind::NonJsonControl) {
            fail("decode failed: " + decoded.error.message);
        }

        for (const auto& event : batch.events) {
            ++summary.normalized_events;

            if (summary.entity_id.empty() &&
                event.event_type != NormalizedEventType::Heartbeat &&
                !event.entity_id.empty()) {
                summary.entity_id = event.entity_id;
            }
        }

        const auto state_events = from_normalized_batch(batch);
        for (const auto& event : state_events) {
            const auto applied = store.apply(event);
            if (!applied.ok()) {
                fail("market state apply failed: " + applied.message);
            }
        }
    }

    summary.global_hash = store.global_hash();
    summary.entity_count = !summary.entity_id.empty() &&
        store.exists(summary.entity_id) ? 1ULL : 0ULL;
    return summary;
}

void DecodeStateViewReplay_Market39SnapshotQueryable() {
    MarketStateStore store;
    const ReplaySummary summary = replay_market39(store);
    MarketStateView view(store);

    expect_equal(summary.packets_read, 39ULL, "packets_read");
    expect_equal(summary.normalized_events, 39ULL, "normalized_events");
    expect_equal(summary.state_errors, 0ULL, "state_errors");
    expect_equal(summary.entity_count, 1ULL, "entity_count");
    expect_equal(
        summary.global_hash,
        12959912045291989833ULL,
        "global_hash"
    );

    expect_true(!summary.entity_id.empty(), "entity_id captured");
    expect_true(view.exists(summary.entity_id), "view exists");
    expect_equal(
        view.global_hash(),
        12959912045291989833ULL,
        "view global hash"
    );

    const auto snapshot = view.get_snapshot(summary.entity_id);
    expect_true(snapshot.ok, "snapshot ok");
    expect_equal(snapshot.value.state_hash, view.state_hash(summary.entity_id), "state hash");

    if (snapshot.value.has_bid && snapshot.value.has_ask &&
        !snapshot.value.crossed &&
        !snapshot.value.closed &&
        !snapshot.value.resolved &&
        !snapshot.value.recovering) {
        const auto bbo = view.get_bbo(summary.entity_id);
        const auto mid = view.get_mid_tick(summary.entity_id);
        const auto spread = view.get_spread_tick(summary.entity_id);

        expect_true(bbo.ok, "bbo ok");
        expect_true(mid.ok, "mid ok");
        expect_true(spread.ok, "spread ok");
    }
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "DecodeStateViewReplay_Market39SnapshotQueryable",
            &DecodeStateViewReplay_Market39SnapshotQueryable
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
