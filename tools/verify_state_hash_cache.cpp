#include "decode/core/DecodePipeline.h"
#include "decode/json/JsonDecodeResult.h"
#include "decode/public/NormalizedEventBatch.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "state/EntityStateStore.h"
#include "state/core/StateHashPolicy.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {

namespace decode = trading_engine::decode;
namespace feed = trading_engine::feed;
namespace state = trading_engine::state;

constexpr std::uint64_t kMarket39LegacyBookHash = 12959912045291989833ULL;

struct Config {
    std::string raw_path{"tests/fixtures/polymarket/market_39.raw"};
    state::StateHashMode hash_mode{state::StateHashMode::DebugVerify};
    std::uint64_t expected_legacy_book_hash{kMarket39LegacyBookHash};
    bool check_legacy_book_hash{true};
};

struct Summary {
    std::uint64_t packets_read{0};
    std::uint64_t normalized_events{0};
    std::uint64_t state_events_applied{0};
    std::uint64_t state_errors{0};
    std::uint64_t entity_hash_checks{0};
    std::uint64_t global_hash_checks{0};
    std::uint64_t hash_mismatches{0};
    std::uint64_t cached_global_hash{0};
    std::uint64_t recomputed_global_hash{0};
    std::uint64_t legacy_book_hash{0};
    bool legacy_book_hash_ok{true};
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

state::StateHashMode parse_hash_mode(const std::string& value) {
    if (value == "hot-path-light") {
        return state::StateHashMode::HotPathLight;
    }
    if (value == "replay-full") {
        return state::StateHashMode::ReplayFull;
    }
    if (value == "debug-verify") {
        return state::StateHashMode::DebugVerify;
    }
    fail("unknown hash mode: " + value);
}

const char* to_string(state::StateHashMode mode) noexcept {
    switch (mode) {
        case state::StateHashMode::HotPathLight:
            return "hot-path-light";
        case state::StateHashMode::ReplayFull:
            return "replay-full";
        case state::StateHashMode::DebugVerify:
            return "debug-verify";
    }
    return "unknown";
}

Config parse_args(int argc, char** argv) {
    Config config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto require_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                fail(std::string{"missing value for "} + name);
            }
            return argv[++i];
        };

        if (arg == "--raw") {
            config.raw_path = require_value("--raw");
        } else if (arg == "--hash-mode") {
            config.hash_mode = parse_hash_mode(require_value("--hash-mode"));
        } else if (arg == "--expect-legacy-book-hash") {
            config.expected_legacy_book_hash =
                std::stoull(require_value("--expect-legacy-book-hash"));
            config.check_legacy_book_hash = true;
        } else if (arg == "--no-legacy-book-hash-check") {
            config.check_legacy_book_hash = false;
        } else {
            fail("unknown argument: " + arg);
        }
    }

    return config;
}

void verify_entity_hash(
    const std::string& entity_id,
    state::EntityStateStore* store,
    Summary* summary
) {
    const auto cached = store->state_hash(entity_id);
    const auto recomputed = store->debug_recomputed_state_hash(entity_id);
    ++summary->entity_hash_checks;
    if (cached != recomputed) {
        ++summary->hash_mismatches;
    }
}

void verify_global_hash(
    state::EntityStateStore* store,
    Summary* summary
) {
    summary->cached_global_hash = store->global_hash();
    summary->recomputed_global_hash = store->debug_recomputed_global_hash();
    ++summary->global_hash_checks;
    if (summary->cached_global_hash != summary->recomputed_global_hash) {
        ++summary->hash_mismatches;
    }
}

Summary run_verify(const Config& config) {
    state::StateRuntimeConfig runtime_config;
    runtime_config.hash_mode = config.hash_mode;

    feed::RawLogReader reader(config.raw_path);
    decode::DecodePipeline pipeline;
    state::EntityStateStore store(runtime_config);
    Summary summary;
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
            fail("decode error in hash cache verifier");
        }

        summary.normalized_events += static_cast<std::uint64_t>(batch.size());
        for (const auto& event : batch.events) {
            const auto result = store.apply(event);
            if (result.code == state::StateApplyCode::Applied) {
                ++summary.state_events_applied;
            }
            if (!result.ok()) {
                ++summary.state_errors;
            }

            if (!result.entity_id.empty()) {
                entity_ids.insert(result.entity_id);
                if (config.hash_mode == state::StateHashMode::DebugVerify &&
                    result.mutation.state_changed) {
                    verify_entity_hash(result.entity_id, &store, &summary);
                    verify_global_hash(&store, &summary);
                }
            }
        }
    }

    for (const auto& entity_id : entity_ids) {
        verify_entity_hash(entity_id, &store, &summary);
    }
    verify_global_hash(&store, &summary);

    summary.legacy_book_hash = summary.cached_global_hash;
    if (config.check_legacy_book_hash) {
        summary.legacy_book_hash_ok =
            summary.legacy_book_hash == config.expected_legacy_book_hash;
    }
    return summary;
}

void print_report(const Config& config, const Summary& summary) {
    const bool passed = summary.state_errors == 0 &&
        summary.hash_mismatches == 0 &&
        summary.legacy_book_hash_ok;

    std::cout << "state_hash_cache_verification:\n";
    std::cout << "  raw: " << config.raw_path << '\n';
    std::cout << "  hash_mode: " << to_string(config.hash_mode) << '\n';
    std::cout << "  packets_read: " << summary.packets_read << '\n';
    std::cout << "  normalized_events: " << summary.normalized_events << '\n';
    std::cout << "  state_events_applied: "
              << summary.state_events_applied << '\n';
    std::cout << "  state_errors: " << summary.state_errors << '\n';
    std::cout << "  entity_hash_checks: "
              << summary.entity_hash_checks << '\n';
    std::cout << "  global_hash_checks: "
              << summary.global_hash_checks << '\n';
    std::cout << "  hash_mismatches: " << summary.hash_mismatches << '\n';
    std::cout << "  cached_global_hash: "
              << summary.cached_global_hash << '\n';
    std::cout << "  recomputed_global_hash: "
              << summary.recomputed_global_hash << '\n';
    std::cout << "  legacy_book_hash: " << summary.legacy_book_hash << '\n';
    if (config.check_legacy_book_hash) {
        std::cout << "  expected_legacy_book_hash: "
                  << config.expected_legacy_book_hash << '\n';
        std::cout << "  legacy_book_hash_ok: "
                  << (summary.legacy_book_hash_ok ? "true" : "false")
                  << '\n';
    }
    std::cout << "  passed: " << (passed ? "true" : "false") << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto config = parse_args(argc, argv);
        const auto summary = run_verify(config);
        print_report(config, summary);

        return summary.state_errors == 0 &&
               summary.hash_mismatches == 0 &&
               summary.legacy_book_hash_ok
            ? 0
            : 1;
    } catch (const std::exception& error) {
        std::cerr << "verify_state_hash_cache failed: "
                  << error.what() << '\n';
        return 1;
    }
}
