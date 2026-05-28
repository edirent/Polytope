#include "decode/core/DecodePipelineResult.h"
#include "decode/json/JsonDecoder.h"
#include "decode/json/JsonDecodeResult.h"
#include "decode/normalize/EventNormalizer.h"
#include "decode/public/DecodeError.h"
#include "decode/public/NormalizedEvent.h"
#include "decode/public/NormalizedEventBatch.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "feed/raw_ingest/RawPacket.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using trading_engine::decode::DecodeErrorCode;
using trading_engine::decode::DecodeErrorSeverity;
using trading_engine::decode::DecodeInputView;
using trading_engine::decode::DecodePipelineResult;
using trading_engine::decode::EventNormalizer;
using trading_engine::decode::JsonDecodeKind;
using trading_engine::decode::JsonDecodeResult;
using trading_engine::decode::JsonDecoder;
using trading_engine::decode::NormalizationResult;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::decode::NormalizedEventType;
using trading_engine::feed::RawLogReadResult;
using trading_engine::feed::RawLogReader;
using trading_engine::feed::RawPacket;
using trading_engine::feed::to_decode_input_view;

constexpr std::uint16_t kCodecNone = 0;
constexpr std::size_t kJsonKindCount = 5;
constexpr std::size_t kEventTypeCount = 8;
constexpr std::size_t kSlowestPacketLimit = 10;

struct Config {
    std::string raw_path{"tests/fixtures/polymarket/market_39.raw"};
    std::string mode{"memory"};
    std::string csv_path;
    std::uint64_t repeat{1};
    std::uint64_t warmup{0};
};

struct LatencyStats {
    std::uint64_t count{0};
    std::uint64_t p50{0};
    std::uint64_t p90{0};
    std::uint64_t p95{0};
    std::uint64_t p99{0};
    std::uint64_t p999{0};
    std::uint64_t max{0};
    double mean{0.0};
};

struct Samples {
    std::vector<std::uint64_t> raw_read_ns;
    std::vector<std::uint64_t> adapter_ns;
    std::vector<std::uint64_t> decode_pipeline_ns;
    std::vector<std::uint64_t> json_decode_ns;
    std::vector<std::uint64_t> normalize_ns;
    std::vector<std::uint64_t> pipeline_overhead_ns;
    std::vector<std::uint64_t> total_feed_decode_ns;
    std::array<std::vector<std::uint64_t>, kJsonKindCount> by_decode_kind;
    std::array<std::vector<std::uint64_t>, kEventTypeCount> by_event_type;
};

struct EventCounts {
    std::uint64_t packets_decoded{0};
    std::uint64_t normalized_events{0};
    std::uint64_t snapshot_events{0};
    std::uint64_t delta_events{0};
    std::uint64_t heartbeat_events{0};
    std::uint64_t unknown_events{0};
    std::uint64_t decode_errors{0};
    std::uint64_t normalization_errors{0};
};

struct SlowPacketObservation {
    std::uint64_t packet_id{0};
    std::uint32_t payload_len{0};
    JsonDecodeKind decode_kind{JsonDecodeKind::MalformedJson};
    NormalizedEventType event_type{NormalizedEventType::Unknown};
    std::size_t event_count{0};
    std::uint64_t adapter_ns{0};
    std::uint64_t json_decode_ns{0};
    std::uint64_t normalize_ns{0};
    std::uint64_t pipeline_overhead_ns{0};
    std::uint64_t total_ns{0};
};

struct BenchState {
    Samples samples;
    EventCounts counts;
    std::vector<SlowPacketObservation> slowest_packets;
    std::ofstream csv;
};

struct MeasuredDecodeResult {
    DecodePipelineResult result;
    std::uint64_t json_decode_ns{0};
    std::uint64_t normalize_ns{0};
    std::uint64_t pipeline_overhead_ns{0};
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::uint64_t now_ns() {
    const auto now = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count()
    );
}

std::uint64_t checked_sub(
    std::uint64_t end_ns,
    std::uint64_t start_ns
) noexcept {
    return end_ns >= start_ns ? end_ns - start_ns : 0;
}

std::uint64_t checked_pipeline_overhead(
    std::uint64_t pipeline_ns,
    std::uint64_t json_decode_ns,
    std::uint64_t normalize_ns
) noexcept {
    const auto measured_ns = json_decode_ns + normalize_ns;
    return pipeline_ns >= measured_ns ? pipeline_ns - measured_ns : 0;
}

std::size_t decode_kind_index(JsonDecodeKind kind) noexcept {
    switch (kind) {
        case JsonDecodeKind::JsonObject:
            return 0;
        case JsonDecodeKind::JsonArray:
            return 1;
        case JsonDecodeKind::NonJsonControl:
            return 2;
        case JsonDecodeKind::UnsupportedJson:
            return 3;
        case JsonDecodeKind::MalformedJson:
        default:
            return 4;
    }
}

std::size_t event_type_index(NormalizedEventType type) noexcept {
    switch (type) {
        case NormalizedEventType::Snapshot:
            return 0;
        case NormalizedEventType::Delta:
            return 1;
        case NormalizedEventType::StatusChange:
            return 2;
        case NormalizedEventType::LifecycleEvent:
            return 3;
        case NormalizedEventType::TradeEvent:
            return 4;
        case NormalizedEventType::Heartbeat:
            return 5;
        case NormalizedEventType::DecodeError:
            return 6;
        case NormalizedEventType::Unknown:
        default:
            return 7;
    }
}

std::uint64_t percentile(
    const std::vector<std::uint64_t>& sorted,
    std::uint64_t numerator,
    std::uint64_t denominator
) {
    if (sorted.empty()) {
        return 0;
    }

    std::uint64_t index =
        (static_cast<std::uint64_t>(sorted.size()) * numerator +
         denominator - 1) /
        denominator;

    if (index == 0) {
        index = 1;
    }

    if (index > sorted.size()) {
        index = static_cast<std::uint64_t>(sorted.size());
    }

    return sorted[static_cast<std::size_t>(index - 1)];
}

LatencyStats summarize(std::vector<std::uint64_t> values) {
    LatencyStats stats;
    stats.count = static_cast<std::uint64_t>(values.size());

    if (values.empty()) {
        return stats;
    }

    std::sort(values.begin(), values.end());

    const long double sum = std::accumulate(
        values.begin(),
        values.end(),
        static_cast<long double>(0.0)
    );

    stats.p50 = percentile(values, 50, 100);
    stats.p90 = percentile(values, 90, 100);
    stats.p95 = percentile(values, 95, 100);
    stats.p99 = percentile(values, 99, 100);
    stats.p999 = percentile(values, 999, 1000);
    stats.max = values.back();
    stats.mean = static_cast<double>(sum / values.size());

    return stats;
}

std::string to_string_kind(JsonDecodeKind kind) {
    return trading_engine::decode::to_string(kind);
}

std::string to_string_error(
    trading_engine::decode::DecodeErrorCode code
) {
    return trading_engine::decode::to_string(code);
}

std::string to_string_event_type(NormalizedEventType type) {
    return trading_engine::decode::to_string(type);
}

DecodePipelineResult make_pipeline_result(
    DecodeErrorCode code,
    DecodeErrorSeverity severity,
    std::string message,
    JsonDecodeKind kind = JsonDecodeKind::MalformedJson
) {
    DecodePipelineResult result;
    result.error.code = code;
    result.error.severity = severity;
    result.error.message = std::move(message);
    result.payload_kind = kind;
    return result;
}

MeasuredDecodeResult decode_measured(
    const JsonDecoder& decoder,
    const EventNormalizer& normalizer,
    const DecodeInputView& input,
    NormalizedEventBatch* out
) {
    // Benchmark-only instrumentation that mirrors DecodePipeline::decode so
    // production DecodePipeline does not grow timing concerns.
    MeasuredDecodeResult measured;

    if (!out) {
        measured.result = make_pipeline_result(
            DecodeErrorCode::InternalError,
            DecodeErrorSeverity::Fatal,
            "DecodePipeline output batch is null"
        );
        return measured;
    }

    out->clear();

    if (input.codec != kCodecNone) {
        measured.result = make_pipeline_result(
            DecodeErrorCode::UnsupportedCodec,
            DecodeErrorSeverity::Error,
            "unsupported payload codec"
        );
        return measured;
    }

    const auto json_start = now_ns();
    const JsonDecodeResult decoded = decoder.decode(input);
    const auto json_end = now_ns();
    measured.json_decode_ns = checked_sub(json_end, json_start);
    measured.result.payload_kind = decoded.kind;

    if (!decoded.ok()) {
        measured.result.error.code = decoded.error;
        measured.result.error.severity = DecodeErrorSeverity::Error;
        measured.result.error.message = decoded.message;
        return measured;
    }

    NormalizationResult normalized;

    const auto normalize_start = now_ns();
    if (decoded.has_json_event_payload()) {
        normalized = normalizer.normalize_json(input, decoded.json);
    } else if (decoded.has_control_payload()) {
        normalized = normalizer.normalize_control(
            input,
            decoded.control_payload
        );
        measured.result.error.code = DecodeErrorCode::NonJsonControl;
        measured.result.error.severity = DecodeErrorSeverity::Info;
    }
    const auto normalize_end = now_ns();
    measured.normalize_ns = checked_sub(normalize_end, normalize_start);

    out->warnings.insert(
        out->warnings.end(),
        normalized.warnings.begin(),
        normalized.warnings.end()
    );

    if (!normalized.ok()) {
        measured.result.error.code = DecodeErrorCode::InternalError;
        measured.result.error.severity = DecodeErrorSeverity::Error;
        measured.result.error.message = normalized.error;
        return measured;
    }

    for (const auto& event : normalized.events) {
        if (!out->push_back(event)) {
            measured.result.error.code = DecodeErrorCode::InternalError;
            measured.result.error.severity = DecodeErrorSeverity::Error;
            measured.result.error.message =
                "too many normalized events in packet";
            return measured;
        }
    }

    measured.result.events_emitted = out->size();
    return measured;
}

void count_events(
    const NormalizedEventBatch& batch,
    EventCounts& counts
) {
    counts.normalized_events += static_cast<std::uint64_t>(batch.size());

    for (const auto& event : batch.events) {
        switch (event.event_type) {
            case NormalizedEventType::Snapshot:
                ++counts.snapshot_events;
                break;
            case NormalizedEventType::Delta:
                ++counts.delta_events;
                break;
            case NormalizedEventType::Heartbeat:
                ++counts.heartbeat_events;
                break;
            case NormalizedEventType::Unknown:
            case NormalizedEventType::DecodeError:
                ++counts.unknown_events;
                break;
            case NormalizedEventType::StatusChange:
            case NormalizedEventType::LifecycleEvent:
            case NormalizedEventType::TradeEvent:
                break;
        }
    }
}

NormalizedEventType primary_event_type(
    const NormalizedEventBatch& batch,
    const DecodePipelineResult& result
) noexcept {
    if (!batch.events.empty()) {
        return batch.events.front().event_type;
    }

    if (!result.ok()) {
        return NormalizedEventType::DecodeError;
    }

    return NormalizedEventType::Unknown;
}

void record_kind_latency(
    Samples* samples,
    JsonDecodeKind kind,
    std::uint64_t total_ns
) {
    samples->by_decode_kind[decode_kind_index(kind)].push_back(total_ns);
}

void record_event_type_latency(
    Samples* samples,
    const NormalizedEventBatch& batch,
    const DecodePipelineResult& result,
    std::uint64_t total_ns
) {
    if (batch.events.empty()) {
        samples->by_event_type[
            event_type_index(primary_event_type(batch, result))
        ].push_back(total_ns);
        return;
    }

    for (const auto& event : batch.events) {
        samples->by_event_type[event_type_index(event.event_type)].push_back(
            total_ns
        );
    }
}

void record_slowest_packet(
    BenchState* state,
    SlowPacketObservation observation
) {
    auto& slowest = state->slowest_packets;

    if (slowest.size() < kSlowestPacketLimit) {
        slowest.push_back(std::move(observation));
    } else {
        auto min_it = std::min_element(
            slowest.begin(),
            slowest.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.total_ns < rhs.total_ns;
            }
        );

        if (min_it != slowest.end() &&
            observation.total_ns > min_it->total_ns) {
            *min_it = std::move(observation);
        }
    }

    std::sort(
        slowest.begin(),
        slowest.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.total_ns > rhs.total_ns;
        }
    );
}

std::vector<RawPacket> load_packets(const std::string& raw_path) {
    RawLogReader reader(raw_path);
    std::vector<RawPacket> packets;

    while (true) {
        RawLogReadResult raw = reader.next();
        if (raw.eof()) {
            break;
        }

        if (!raw.ok()) {
            fail("raw read failed: " + raw.message);
        }

        packets.push_back(std::move(*raw.packet));
    }

    return packets;
}

void run_packet_sample(
    const JsonDecoder& decoder,
    const EventNormalizer& normalizer,
    const RawPacket& packet,
    BenchState* state,
    bool record,
    std::uint64_t total_start_ns,
    std::uint64_t raw_read_ns = 0
) {
    const auto adapter_start = now_ns();
    const auto input = to_decode_input_view(packet);
    const auto adapter_end = now_ns();

    NormalizedEventBatch batch;
    const auto pipeline_start = adapter_end;
    MeasuredDecodeResult measured =
        decode_measured(decoder, normalizer, input, &batch);
    const auto pipeline_end = now_ns();

    if (!record) {
        return;
    }

    const auto adapter_ns = checked_sub(adapter_end, adapter_start);
    const auto pipeline_ns = checked_sub(pipeline_end, pipeline_start);
    measured.pipeline_overhead_ns = checked_pipeline_overhead(
        pipeline_ns,
        measured.json_decode_ns,
        measured.normalize_ns
    );
    const auto total_ns = checked_sub(pipeline_end, total_start_ns);

    if (raw_read_ns > 0) {
        state->samples.raw_read_ns.push_back(raw_read_ns);
    }
    state->samples.adapter_ns.push_back(adapter_ns);
    state->samples.decode_pipeline_ns.push_back(pipeline_ns);
    state->samples.json_decode_ns.push_back(measured.json_decode_ns);
    state->samples.normalize_ns.push_back(measured.normalize_ns);
    state->samples.pipeline_overhead_ns.push_back(
        measured.pipeline_overhead_ns
    );
    state->samples.total_feed_decode_ns.push_back(total_ns);
    record_kind_latency(
        &state->samples,
        measured.result.payload_kind,
        total_ns
    );
    record_event_type_latency(
        &state->samples,
        batch,
        measured.result,
        total_ns
    );

    ++state->counts.packets_decoded;

    if (!measured.result.ok() &&
        measured.result.error.code != DecodeErrorCode::NonJsonControl) {
        ++state->counts.decode_errors;
    }

    if (batch.overflowed) {
        ++state->counts.normalization_errors;
    }

    count_events(batch, state->counts);

    record_slowest_packet(
        state,
        SlowPacketObservation{
            packet.header.packet_id,
            packet.header.payload_len,
            measured.result.payload_kind,
            primary_event_type(batch, measured.result),
            batch.size(),
            adapter_ns,
            measured.json_decode_ns,
            measured.normalize_ns,
            measured.pipeline_overhead_ns,
            total_ns
        }
    );

    if (state->csv.is_open()) {
        state->csv
            << packet.header.packet_id << ','
            << packet.header.payload_len << ','
            << adapter_ns << ','
            << measured.json_decode_ns << ','
            << measured.normalize_ns << ','
            << measured.pipeline_overhead_ns << ','
            << pipeline_ns << ','
            << total_ns << ','
            << batch.size() << ','
            << to_string_kind(measured.result.payload_kind) << ','
            << to_string_event_type(
                   primary_event_type(batch, measured.result)
               ) << ','
            << to_string_error(measured.result.error.code) << '\n';
    }
}

void run_memory_mode(const Config& config, BenchState* state) {
    const std::vector<RawPacket> packets = load_packets(config.raw_path);
    if (packets.empty()) {
        fail("raw log contains no packets");
    }

    JsonDecoder decoder;
    EventNormalizer normalizer;

    for (std::uint64_t i = 0; i < config.warmup; ++i) {
        for (const auto& packet : packets) {
            const auto total_start = now_ns();
            run_packet_sample(
                decoder,
                normalizer,
                packet,
                state,
                false,
                total_start
            );
        }
    }

    state->samples.adapter_ns.reserve(
        static_cast<std::size_t>(packets.size() * config.repeat)
    );
    state->samples.decode_pipeline_ns.reserve(
        static_cast<std::size_t>(packets.size() * config.repeat)
    );
    state->samples.json_decode_ns.reserve(
        static_cast<std::size_t>(packets.size() * config.repeat)
    );
    state->samples.normalize_ns.reserve(
        static_cast<std::size_t>(packets.size() * config.repeat)
    );
    state->samples.pipeline_overhead_ns.reserve(
        static_cast<std::size_t>(packets.size() * config.repeat)
    );
    state->samples.total_feed_decode_ns.reserve(
        static_cast<std::size_t>(packets.size() * config.repeat)
    );

    for (std::uint64_t i = 0; i < config.repeat; ++i) {
        for (const auto& packet : packets) {
            const auto total_start = now_ns();
            run_packet_sample(
                decoder,
                normalizer,
                packet,
                state,
                true,
                total_start
            );
        }
    }
}

void run_replay_io_mode(const Config& config, BenchState* state) {
    JsonDecoder decoder;
    EventNormalizer normalizer;

    for (std::uint64_t i = 0; i < config.warmup + config.repeat; ++i) {
        const bool record = i >= config.warmup;
        RawLogReader reader(config.raw_path);

        while (true) {
            const auto total_start = now_ns();
            RawLogReadResult raw = reader.next();
            const auto read_end = now_ns();

            if (raw.eof()) {
                break;
            }

            if (!raw.ok()) {
                fail("raw read failed: " + raw.message);
            }

            run_packet_sample(
                decoder,
                normalizer,
                *raw.packet,
                state,
                record,
                total_start,
                checked_sub(read_end, total_start)
            );
        }
    }
}

void print_latency(const std::string& name, const LatencyStats& stats) {
    std::cout << name << ":\n";
    std::cout << "  count: " << stats.count << '\n';
    std::cout << "  p50: " << stats.p50 << '\n';
    std::cout << "  p90: " << stats.p90 << '\n';
    std::cout << "  p95: " << stats.p95 << '\n';
    std::cout << "  p99: " << stats.p99 << '\n';
    std::cout << "  p99.9: " << stats.p999 << '\n';
    std::cout << "  max: " << stats.max << '\n';
    std::cout << "  mean: " << stats.mean << '\n';
}

void print_latency_nested(
    const std::string& name,
    const LatencyStats& stats
) {
    std::cout << "  " << name << ":\n";
    std::cout << "    count: " << stats.count << '\n';
    std::cout << "    p50: " << stats.p50 << '\n';
    std::cout << "    p90: " << stats.p90 << '\n';
    std::cout << "    p95: " << stats.p95 << '\n';
    std::cout << "    p99: " << stats.p99 << '\n';
    std::cout << "    p99.9: " << stats.p999 << '\n';
    std::cout << "    max: " << stats.max << '\n';
    std::cout << "    mean: " << stats.mean << '\n';
}

void print_decode_kind_latency(const Samples& samples) {
    std::cout << "by_decode_kind_total_ns:\n";
    print_latency_nested(
        "json_object",
        summarize(samples.by_decode_kind[0])
    );
    print_latency_nested(
        "json_array",
        summarize(samples.by_decode_kind[1])
    );
    print_latency_nested(
        "control",
        summarize(samples.by_decode_kind[2])
    );
    print_latency_nested(
        "unsupported",
        summarize(samples.by_decode_kind[3])
    );
    print_latency_nested(
        "malformed",
        summarize(samples.by_decode_kind[4])
    );
}

void print_event_type_latency(const Samples& samples) {
    std::cout << "by_event_type_total_ns:\n";
    print_latency_nested("snapshot", summarize(samples.by_event_type[0]));
    print_latency_nested("delta", summarize(samples.by_event_type[1]));
    print_latency_nested(
        "status_change",
        summarize(samples.by_event_type[2])
    );
    print_latency_nested("lifecycle", summarize(samples.by_event_type[3]));
    print_latency_nested("trade", summarize(samples.by_event_type[4]));
    print_latency_nested("heartbeat", summarize(samples.by_event_type[5]));
    print_latency_nested("decode_error", summarize(samples.by_event_type[6]));
    print_latency_nested("unknown", summarize(samples.by_event_type[7]));
}

void print_slowest_packets(
    const std::vector<SlowPacketObservation>& slowest_packets
) {
    std::cout << "slowest_packets:\n";
    for (const auto& packet : slowest_packets) {
        std::cout
            << "  - packet_id: " << packet.packet_id << '\n'
            << "    payload_len: " << packet.payload_len << '\n'
            << "    decode_kind: " << to_string_kind(packet.decode_kind)
            << '\n'
            << "    event_type: "
            << to_string_event_type(packet.event_type) << '\n'
            << "    event_count: " << packet.event_count << '\n'
            << "    adapter_ns: " << packet.adapter_ns << '\n'
            << "    json_decode_ns: " << packet.json_decode_ns << '\n'
            << "    normalize_ns: " << packet.normalize_ns << '\n'
            << "    pipeline_overhead_ns: "
            << packet.pipeline_overhead_ns << '\n'
            << "    total_ns: " << packet.total_ns << '\n';
    }
}

void print_report(
    const Config& config,
    const BenchState& state,
    std::uint64_t packets_loaded
) {
    std::cout << "feed_decode_latency\n";
    std::cout << "mode: " << config.mode << '\n';
    std::cout << "packets_loaded: " << packets_loaded << '\n';
    std::cout << "iterations: " << config.repeat << '\n';
    std::cout << "warmup: " << config.warmup << '\n';
    std::cout << "samples: "
              << state.samples.total_feed_decode_ns.size() << '\n';
    std::cout << '\n';

    if (!state.samples.raw_read_ns.empty()) {
        print_latency("raw_read_ns", summarize(state.samples.raw_read_ns));
        std::cout << '\n';
    }

    print_latency("adapter_ns", summarize(state.samples.adapter_ns));
    std::cout << '\n';
    print_latency(
        "json_decode_ns",
        summarize(state.samples.json_decode_ns)
    );
    std::cout << '\n';
    print_latency("normalize_ns", summarize(state.samples.normalize_ns));
    std::cout << '\n';
    print_latency(
        "pipeline_overhead_ns",
        summarize(state.samples.pipeline_overhead_ns)
    );
    std::cout << '\n';
    print_latency(
        "decode_pipeline_ns",
        summarize(state.samples.decode_pipeline_ns)
    );
    std::cout << '\n';
    print_latency(
        "total_feed_decode_ns",
        summarize(state.samples.total_feed_decode_ns)
    );
    std::cout << '\n';

    std::cout << "pipeline_breakdown:\n";
    print_latency_nested("adapter_ns", summarize(state.samples.adapter_ns));
    print_latency_nested(
        "json_decode_ns",
        summarize(state.samples.json_decode_ns)
    );
    print_latency_nested(
        "normalize_ns",
        summarize(state.samples.normalize_ns)
    );
    print_latency_nested(
        "pipeline_overhead_ns",
        summarize(state.samples.pipeline_overhead_ns)
    );
    print_latency_nested(
        "decode_pipeline_ns",
        summarize(state.samples.decode_pipeline_ns)
    );
    print_latency_nested(
        "total_feed_decode_ns",
        summarize(state.samples.total_feed_decode_ns)
    );
    std::cout << '\n';

    print_decode_kind_latency(state.samples);
    std::cout << '\n';
    print_event_type_latency(state.samples);
    std::cout << '\n';
    print_slowest_packets(state.slowest_packets);
    std::cout << '\n';

    std::cout << "event_counts:\n";
    std::cout << "  packets_decoded: "
              << state.counts.packets_decoded << '\n';
    std::cout << "  normalized_events: "
              << state.counts.normalized_events << '\n';
    std::cout << "  snapshot_events: "
              << state.counts.snapshot_events << '\n';
    std::cout << "  delta_events: " << state.counts.delta_events << '\n';
    std::cout << "  heartbeat_events: "
              << state.counts.heartbeat_events << '\n';
    std::cout << "  unknown_events: " << state.counts.unknown_events << '\n';
    std::cout << "  decode_errors: " << state.counts.decode_errors << '\n';
    std::cout << "  normalization_errors: "
              << state.counts.normalization_errors << '\n';
}

Config parse_args(int argc, char** argv) {
    Config config;

    if (argc > 1 && argv[1][0] != '-') {
        config.raw_path = argv[1];
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--repeat") {
            if (++i >= argc) {
                fail("--repeat requires a value");
            }
            config.repeat = std::stoull(argv[i]);
            continue;
        }

        if (arg == "--warmup") {
            if (++i >= argc) {
                fail("--warmup requires a value");
            }
            config.warmup = std::stoull(argv[i]);
            continue;
        }

        if (arg == "--mode") {
            if (++i >= argc) {
                fail("--mode requires a value");
            }
            config.mode = argv[i];
            continue;
        }

        if (arg == "--csv") {
            if (++i >= argc) {
                fail("--csv requires a value");
            }
            config.csv_path = argv[i];
            continue;
        }

        if (arg == "--help" || arg == "-h") {
            std::cout
                << "usage: bench_feed_decode_latency [raw_path] "
                << "[--repeat N] [--warmup N] "
                << "[--mode memory|replay-io] [--csv path]\n";
            std::exit(0);
        }
    }

    if (config.mode != "memory" && config.mode != "replay-io") {
        fail("unsupported mode: " + config.mode);
    }

    if (config.repeat == 0) {
        fail("--repeat must be greater than zero");
    }

    return config;
}

int run(int argc, char** argv) {
    const Config config = parse_args(argc, argv);
    BenchState state;

    if (!config.csv_path.empty()) {
        state.csv.open(config.csv_path, std::ios::out | std::ios::trunc);
        if (!state.csv.is_open()) {
            fail("failed to open csv: " + config.csv_path);
        }
        state.csv
            << "packet_id,payload_len,adapter_ns,json_decode_ns,"
            << "normalize_ns,pipeline_overhead_ns,pipeline_ns,total_ns,"
            << "event_count,decode_kind,primary_event_type,error_code\n";
    }

    const std::vector<RawPacket> packets_for_count =
        load_packets(config.raw_path);
    const auto packets_loaded =
        static_cast<std::uint64_t>(packets_for_count.size());

    if (config.mode == "memory") {
        BenchState local_state;
        if (!config.csv_path.empty()) {
            local_state.csv = std::move(state.csv);
        }
        run_memory_mode(config, &local_state);
        print_report(config, local_state, packets_loaded);
        return 0;
    }

    run_replay_io_mode(config, &state);
    print_report(config, state, packets_loaded);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "bench_feed_decode_latency failed: "
                  << error.what() << '\n';
        return 1;
    }
}
