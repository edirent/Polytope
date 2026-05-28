#include "decode/core/DecodePipeline.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/integrity/ConsistencyChecker.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "feed/state/EntityStateStore.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using trading_engine::feed::ConsistencyChecker;
using trading_engine::feed::EntityStateStore;
using trading_engine::feed::RawLogReader;
using trading_engine::feed::StateApplyCode;
using trading_engine::feed::StateApplyResult;
using trading_engine::feed::to_decode_input_view;
using trading_engine::decode::DecodePipeline;
using trading_engine::decode::JsonDecodeKind;
using trading_engine::decode::NormalizedEvent;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::decode::NormalizedEventType;

struct Percentiles {
    std::uint64_t p50{0};
    std::uint64_t p95{0};
    std::uint64_t p99{0};
    std::uint64_t max{0};
};

struct LatencySamples {
    std::vector<std::uint64_t> raw_read;
    std::vector<std::uint64_t> decode;
    std::vector<std::uint64_t> normalize;
    std::vector<std::uint64_t> state_apply;
    std::vector<std::uint64_t> consistency;
    std::vector<std::uint64_t> total_packet_pipeline;
};

struct FeedE2ESummary {
    std::uint64_t packets_read{0};
    std::uint64_t decoded_json_object{0};
    std::uint64_t decoded_json_array{0};
    std::uint64_t decoded_control{0};
    std::uint64_t decode_errors{0};

    std::uint64_t normalized_events{0};
    std::uint64_t snapshot_events{0};
    std::uint64_t delta_events{0};
    std::uint64_t heartbeat_events{0};
    std::uint64_t unknown_events{0};
    std::uint64_t normalization_errors{0};

    std::uint64_t state_events_applied{0};
    std::uint64_t snapshots_applied{0};
    std::uint64_t deltas_applied{0};
    std::uint64_t heartbeats_ignored{0};
    std::uint64_t state_errors{0};
    std::uint64_t consistency_errors{0};
    std::uint64_t entity_count{0};
    std::uint64_t global_hash{0};
};

struct RunResult {
    FeedE2ESummary summary;
    std::vector<std::string> trace;
};

struct Config {
    std::string raw_path;
    std::uint64_t repeat{1};
    bool check_determinism{false};
    bool expect_market_39{false};
};

std::uint64_t now_ns() {
    const auto now = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()
    );

    return static_cast<std::uint64_t>(ns.count());
}

std::uint64_t checked_sub(
    std::uint64_t end_ns,
    std::uint64_t start_ns
) noexcept {
    if (end_ns < start_ns) {
        return 0;
    }

    return end_ns - start_ns;
}

std::uint64_t percentile_value(
    std::vector<std::uint64_t> values,
    std::uint64_t numerator,
    std::uint64_t denominator
) {
    if (values.empty()) {
        return 0;
    }

    std::sort(values.begin(), values.end());

    const auto count = static_cast<std::uint64_t>(values.size());
    std::uint64_t index = (count * numerator + denominator - 1) / denominator;

    if (index == 0) {
        index = 1;
    }

    return values[static_cast<std::size_t>(index - 1)];
}

Percentiles summarize_latency(const std::vector<std::uint64_t>& values) {
    if (values.empty()) {
        return {};
    }

    Percentiles summary;
    summary.p50 = percentile_value(values, 50, 100);
    summary.p95 = percentile_value(values, 95, 100);
    summary.p99 = percentile_value(values, 99, 100);
    summary.max = *std::max_element(values.begin(), values.end());

    return summary;
}

void count_decode_status(JsonDecodeKind status, FeedE2ESummary& summary) {
    switch (status) {
        case JsonDecodeKind::JsonObject:
            ++summary.decoded_json_object;
            break;
        case JsonDecodeKind::JsonArray:
            ++summary.decoded_json_array;
            break;
        case JsonDecodeKind::NonJsonControl:
            ++summary.decoded_control;
            break;
        case JsonDecodeKind::UnsupportedJson:
        case JsonDecodeKind::MalformedJson:
            ++summary.decode_errors;
            break;
    }
}

void count_event_type(const NormalizedEvent& event, FeedE2ESummary& summary) {
    ++summary.normalized_events;

    switch (event.event_type) {
        case NormalizedEventType::Snapshot:
            ++summary.snapshot_events;
            break;
        case NormalizedEventType::Delta:
            ++summary.delta_events;
            break;
        case NormalizedEventType::Heartbeat:
            ++summary.heartbeat_events;
            break;
        case NormalizedEventType::Unknown:
            ++summary.unknown_events;
            break;
        case NormalizedEventType::StatusChange:
        case NormalizedEventType::LifecycleEvent:
        case NormalizedEventType::TradeEvent:
        case NormalizedEventType::DecodeError:
            break;
    }
}

std::string trace_row(
    const NormalizedEvent& event,
    std::uint64_t event_index,
    const StateApplyResult& result,
    const EntityStateStore& store
) {
    std::ostringstream out;

    std::string entity_status;
    if (!result.entity_id.empty()) {
        entity_status = trading_engine::feed::to_string(
            store.status(result.entity_id)
        );
    }

    out
        << event.packet_id << ','
        << event_index << ','
        << trading_engine::decode::to_string(event.event_type) << ','
        << event.raw_type << ','
        << result.entity_id << ','
        << trading_engine::feed::to_string(result.code) << ','
        << entity_status << ','
        << result.entity_hash << ','
        << result.global_hash;

    return out.str();
}

using AppliedEvents =
    std::vector<std::pair<const NormalizedEvent*, StateApplyResult>>;

AppliedEvents apply_state(
    const std::vector<NormalizedEvent>& events,
    EntityStateStore& store,
    FeedE2ESummary& summary,
    std::vector<std::string>* trace
) {
    AppliedEvents applied;
    applied.reserve(events.size());

    std::uint64_t event_index = 0;

    for (const auto& event : events) {
        StateApplyResult apply_result = store.apply(event);

        if (apply_result.code == StateApplyCode::Applied) {
            ++summary.state_events_applied;
        }

        if (!apply_result.ok()) {
            ++summary.state_errors;
        }

        if (trace) {
            trace->push_back(trace_row(event, event_index, apply_result, store));
        }

        applied.emplace_back(&event, std::move(apply_result));
        ++event_index;
    }

    return applied;
}

void check_consistency(
    const AppliedEvents& applied,
    const EntityStateStore& store,
    ConsistencyChecker& consistency_checker,
    FeedE2ESummary& summary
) {
    for (const auto& [event, apply_result] : applied) {
        const auto* entity = apply_result.entity_id.empty()
            ? nullptr
            : store.get(apply_result.entity_id);

        const auto check =
            consistency_checker.check(*event, apply_result, entity);

        if (!check.ok()) {
            ++summary.consistency_errors;
        }
    }
}

RunResult run_once(
    const std::string& raw_path,
    LatencySamples* latencies,
    bool collect_trace
) {
    RawLogReader reader(raw_path);
    DecodePipeline pipeline;
    EntityStateStore store;
    ConsistencyChecker consistency_checker;

    RunResult result;
    std::vector<std::string>* trace =
        collect_trace ? &result.trace : nullptr;

    while (true) {
        const auto t0 = now_ns();
        auto raw_result = reader.next();
        const auto t1 = now_ns();

        if (raw_result.eof()) {
            break;
        }

        if (!raw_result.ok()) {
            ++result.summary.decode_errors;
            throw std::runtime_error(
                "raw read failed: " + raw_result.message
            );
        }

        ++result.summary.packets_read;

        NormalizedEventBatch batch;
        const auto decoded = pipeline.decode(
            to_decode_input_view(*raw_result.packet),
            &batch
        );
        const auto t2 = now_ns();

        count_decode_status(decoded.payload_kind, result.summary);

        const auto t3 = now_ns();

        if (!decoded.ok()) {
            if (decoded.payload_kind == JsonDecodeKind::JsonObject ||
                decoded.payload_kind == JsonDecodeKind::JsonArray ||
                decoded.payload_kind == JsonDecodeKind::NonJsonControl) {
                ++result.summary.normalization_errors;
            }
        }

        if (batch.overflowed) {
            ++result.summary.normalization_errors;
        }

        const auto applied = apply_state(
            batch.events,
            store,
            result.summary,
            trace
        );
        const auto t4 = now_ns();

        check_consistency(
            applied,
            store,
            consistency_checker,
            result.summary
        );
        const auto t5 = now_ns();

        for (const auto& event : batch.events) {
            count_event_type(event, result.summary);
        }

        if (latencies) {
            latencies->raw_read.push_back(checked_sub(t1, t0));
            latencies->decode.push_back(checked_sub(t2, t1));
            latencies->normalize.push_back(checked_sub(t3, t2));
            latencies->state_apply.push_back(checked_sub(t4, t3));
            latencies->consistency.push_back(checked_sub(t5, t4));
            latencies->total_packet_pipeline.push_back(checked_sub(t5, t0));
        }
    }

    result.summary.snapshots_applied = store.snapshots_applied();
    result.summary.deltas_applied = store.deltas_applied();
    result.summary.heartbeats_ignored = store.heartbeats_ignored();
    result.summary.state_errors = store.errors();
    result.summary.entity_count =
        static_cast<std::uint64_t>(store.entity_count());
    result.summary.global_hash = store.global_hash();

    return result;
}

bool basename_is_market_39(const std::string& path) {
    return std::filesystem::path(path).filename() == "market_39.raw";
}

bool expect_equal(
    std::uint64_t actual,
    std::uint64_t expected,
    const char* field
) {
    if (actual == expected) {
        return true;
    }

    std::cerr
        << "run_feed_e2e assertion failed: " << field
        << " expected " << expected
        << " got " << actual
        << '\n';

    return false;
}

bool expect_at_least(
    std::uint64_t actual,
    std::uint64_t expected,
    const char* field
) {
    if (actual >= expected) {
        return true;
    }

    std::cerr
        << "run_feed_e2e assertion failed: " << field
        << " expected at least " << expected
        << " got " << actual
        << '\n';

    return false;
}

bool assert_market_39(const FeedE2ESummary& summary) {
    bool ok = true;

    ok &= expect_equal(summary.packets_read, 39, "packets_read");
    ok &= expect_equal(summary.decode_errors, 0, "decode_errors");
    ok &= expect_equal(summary.normalized_events, 39, "normalized_events");
    ok &= expect_equal(summary.snapshot_events, 1, "snapshot_events");
    ok &= expect_equal(summary.delta_events, 35, "delta_events");
    ok &= expect_equal(summary.heartbeat_events, 3, "heartbeat_events");
    ok &= expect_equal(summary.unknown_events, 0, "unknown_events");
    ok &= expect_equal(summary.state_errors, 0, "state_errors");
    ok &= expect_equal(
        summary.consistency_errors,
        0,
        "consistency_errors"
    );
    ok &= expect_at_least(summary.entity_count, 1, "entity_count");

    return ok;
}

void print_latency_block(const char* name, const std::vector<std::uint64_t>& values) {
    const auto summary = summarize_latency(values);

    std::cout << "  " << name << ":\n";
    std::cout << "    p50: " << summary.p50 << '\n';
    std::cout << "    p95: " << summary.p95 << '\n';
    std::cout << "    p99: " << summary.p99 << '\n';
    std::cout << "    max: " << summary.max << '\n';
}

void print_report(
    const FeedE2ESummary& summary,
    const LatencySamples& latencies,
    std::uint64_t repeat,
    bool determinism_checked,
    std::uint64_t run_1_hash,
    std::uint64_t run_2_hash,
    bool trace_equal,
    bool determinism_passed
) {
    std::cout << "feed_e2e_summary:\n";
    std::cout << "  packets_read: " << summary.packets_read << '\n';
    std::cout << "  decoded_json_object: " << summary.decoded_json_object << '\n';
    std::cout << "  decoded_json_array: " << summary.decoded_json_array << '\n';
    std::cout << "  decoded_control: " << summary.decoded_control << '\n';
    std::cout << "  decode_errors: " << summary.decode_errors << '\n';
    std::cout << '\n';
    std::cout << "  normalized_events: " << summary.normalized_events << '\n';
    std::cout << "  snapshot_events: " << summary.snapshot_events << '\n';
    std::cout << "  delta_events: " << summary.delta_events << '\n';
    std::cout << "  heartbeat_events: " << summary.heartbeat_events << '\n';
    std::cout << "  unknown_events: " << summary.unknown_events << '\n';
    std::cout << "  normalization_errors: " << summary.normalization_errors << '\n';
    std::cout << '\n';
    std::cout << "  state_events_applied: " << summary.state_events_applied << '\n';
    std::cout << "  snapshots_applied: " << summary.snapshots_applied << '\n';
    std::cout << "  deltas_applied: " << summary.deltas_applied << '\n';
    std::cout << "  heartbeats_ignored: " << summary.heartbeats_ignored << '\n';
    std::cout << "  state_errors: " << summary.state_errors << '\n';
    std::cout << "  entity_count: " << summary.entity_count << '\n';
    std::cout << "  global_hash: " << summary.global_hash << '\n';
    std::cout << "  repeat: " << repeat << '\n';
    std::cout << "  latency_samples: "
              << latencies.total_packet_pipeline.size() << '\n';

    std::cout << '\n';
    std::cout << "latency_ns:\n";
    print_latency_block("raw_read", latencies.raw_read);
    std::cout << '\n';
    print_latency_block("decode", latencies.decode);
    std::cout << '\n';
    print_latency_block("normalize", latencies.normalize);
    std::cout << '\n';
    print_latency_block("state_apply", latencies.state_apply);
    std::cout << '\n';
    print_latency_block("consistency", latencies.consistency);
    std::cout << '\n';
    print_latency_block(
        "total_packet_pipeline",
        latencies.total_packet_pipeline
    );

    if (determinism_checked) {
        std::cout << '\n';
        std::cout << "determinism:\n";
        std::cout << "  run_1_global_hash: " << run_1_hash << '\n';
        std::cout << "  run_2_global_hash: " << run_2_hash << '\n';
        std::cout << "  trace_equal: " << (trace_equal ? "true" : "false") << '\n';
        std::cout << "  passed: "
                  << (determinism_passed ? "true" : "false") << '\n';
    }
}

Config parse_args(int argc, char** argv) {
    if (argc < 2) {
        throw std::runtime_error(
            "usage: run_feed_e2e raw_log [--repeat N] "
            "[--check-determinism] [--expect-market-39]"
        );
    }

    Config config;
    config.raw_path = argv[1];
    config.expect_market_39 = basename_is_market_39(config.raw_path);

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--repeat") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--repeat requires a value");
            }

            config.repeat = std::stoull(argv[++i]);
            if (config.repeat == 0) {
                throw std::runtime_error("--repeat must be greater than zero");
            }

            continue;
        }

        if (arg == "--check-determinism") {
            config.check_determinism = true;
            continue;
        }

        if (arg == "--expect-market-39") {
            config.expect_market_39 = true;
            continue;
        }

        if (arg == "--no-fixture-assertions") {
            config.expect_market_39 = false;
            continue;
        }

        throw std::runtime_error("unknown argument: " + arg);
    }

    if (config.check_determinism && config.repeat < 2) {
        throw std::runtime_error(
            "--check-determinism requires --repeat 2 or greater"
        );
    }

    return config;
}

int run(const Config& config) {
    LatencySamples latencies;
    RunResult first;
    RunResult second;

    for (std::uint64_t i = 0; i < config.repeat; ++i) {
        const bool collect_trace = config.check_determinism && i < 2;
        RunResult result = run_once(
            config.raw_path,
            &latencies,
            collect_trace
        );

        if (i == 0) {
            first = std::move(result);
        } else if (i == 1) {
            second = std::move(result);
        }
    }

    bool passed = true;

    if (config.expect_market_39) {
        passed &= assert_market_39(first.summary);
    }

    const bool trace_equal =
        config.check_determinism && first.trace == second.trace;

    const bool determinism_passed =
        !config.check_determinism ||
        (first.summary.global_hash == second.summary.global_hash &&
         trace_equal);

    if (config.check_determinism && !determinism_passed) {
        std::cerr << "run_feed_e2e determinism check failed\n";
        passed = false;
    }

    print_report(
        first.summary,
        latencies,
        config.repeat,
        config.check_determinism,
        first.summary.global_hash,
        second.summary.global_hash,
        trace_equal,
        determinism_passed
    );

    return passed ? 0 : 1;
}

int fail(const std::string& message) {
    std::cerr << "run_feed_e2e failed: " << message << '\n';
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_args(argc, argv));
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}
