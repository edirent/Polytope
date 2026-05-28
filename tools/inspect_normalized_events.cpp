#include "decode/core/DecodePipeline.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawLogReader.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

namespace {

using trading_engine::decode::DecodePipeline;
using trading_engine::decode::JsonDecodeKind;
using trading_engine::decode::NormalizedEvent;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::decode::NormalizedEventType;
using trading_engine::feed::RawLogReadResult;
using trading_engine::feed::RawLogReader;
using trading_engine::feed::to_decode_input_view;

struct NormalizedInspection {
    std::uint64_t total_packets{0};

    std::uint64_t decode_json_object{0};
    std::uint64_t decode_json_array{0};
    std::uint64_t decode_control{0};
    std::uint64_t decode_errors{0};

    std::uint64_t normalized_events{0};
    std::uint64_t snapshot_events{0};
    std::uint64_t delta_events{0};
    std::uint64_t status_change_events{0};
    std::uint64_t lifecycle_events{0};
    std::uint64_t trade_events{0};
    std::uint64_t heartbeat_events{0};
    std::uint64_t unknown_events{0};

    std::uint64_t normalization_warnings{0};
    std::uint64_t normalization_errors{0};
};

void count_decode_status(
    JsonDecodeKind status,
    NormalizedInspection& inspection
) {
    switch (status) {
        case JsonDecodeKind::JsonObject:
            ++inspection.decode_json_object;
            break;

        case JsonDecodeKind::JsonArray:
            ++inspection.decode_json_array;
            break;

        case JsonDecodeKind::NonJsonControl:
            ++inspection.decode_control;
            break;

        case JsonDecodeKind::UnsupportedJson:
        case JsonDecodeKind::MalformedJson:
            ++inspection.decode_errors;
            break;
    }
}

void count_event_type(
    const NormalizedEvent& event,
    NormalizedInspection& inspection
) {
    ++inspection.normalized_events;
    inspection.normalization_warnings +=
        static_cast<std::uint64_t>(event.warnings.size());

    switch (event.event_type) {
        case NormalizedEventType::Snapshot:
            ++inspection.snapshot_events;
            break;

        case NormalizedEventType::Delta:
            ++inspection.delta_events;
            break;

        case NormalizedEventType::StatusChange:
            ++inspection.status_change_events;
            break;

        case NormalizedEventType::LifecycleEvent:
            ++inspection.lifecycle_events;
            break;

        case NormalizedEventType::TradeEvent:
            ++inspection.trade_events;
            break;

        case NormalizedEventType::Heartbeat:
            ++inspection.heartbeat_events;
            break;

        case NormalizedEventType::Unknown:
            ++inspection.unknown_events;
            break;

        case NormalizedEventType::DecodeError:
            ++inspection.unknown_events;
            break;
    }
}

void count_batch(
    const NormalizedEventBatch& result,
    NormalizedInspection& inspection
) {
    inspection.normalization_warnings +=
        static_cast<std::uint64_t>(result.warnings.size());

    for (const auto& event : result.events) {
        count_event_type(event, inspection);
    }
}

void print_inspection(const NormalizedInspection& inspection) {
    std::cout << "total_packets: " << inspection.total_packets << '\n';
    std::cout << "decode_json_object: " << inspection.decode_json_object << '\n';
    std::cout << "decode_json_array: " << inspection.decode_json_array << '\n';
    std::cout << "decode_control: " << inspection.decode_control << '\n';
    std::cout << "decode_errors: " << inspection.decode_errors << '\n';

    std::cout << '\n';
    std::cout << "normalized_events: " << inspection.normalized_events << '\n';
    std::cout << "snapshot_events: " << inspection.snapshot_events << '\n';
    std::cout << "delta_events: " << inspection.delta_events << '\n';
    std::cout << "status_change_events: " << inspection.status_change_events << '\n';
    std::cout << "lifecycle_events: " << inspection.lifecycle_events << '\n';
    std::cout << "trade_events: " << inspection.trade_events << '\n';
    std::cout << "heartbeat_events: " << inspection.heartbeat_events << '\n';
    std::cout << "unknown_events: " << inspection.unknown_events << '\n';
    std::cout << "normalization_warnings: "
              << inspection.normalization_warnings << '\n';
    std::cout << "normalization_errors: "
              << inspection.normalization_errors << '\n';
}

int inspect(const std::string& raw_path) {
    RawLogReader reader(raw_path);
    DecodePipeline pipeline;
    NormalizedInspection inspection;

    while (true) {
        RawLogReadResult raw_result = reader.next();
        if (raw_result.eof()) {
            break;
        }

        if (!raw_result.ok()) {
            ++inspection.decode_errors;
            break;
        }

        ++inspection.total_packets;

        NormalizedEventBatch batch;
        const auto decoded = pipeline.decode(
            to_decode_input_view(*raw_result.packet),
            &batch
        );

        count_decode_status(decoded.payload_kind, inspection);

        if (!decoded.ok()) {
            if (decoded.payload_kind == JsonDecodeKind::JsonObject ||
                decoded.payload_kind == JsonDecodeKind::JsonArray ||
                decoded.payload_kind == JsonDecodeKind::NonJsonControl) {
                ++inspection.normalization_errors;
            }
            continue;
        }

        count_batch(batch, inspection);
    }

    print_inspection(inspection);

    return inspection.normalization_errors == 0 ? 0 : 1;
}

int fail(const std::string& message) {
    std::cerr << "inspect_normalized_events failed: " << message << '\n';
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string raw_path =
        argc > 1 ? argv[1] : "tests/fixtures/polymarket/market_39.raw";

    if (argc > 2) {
        return fail("usage: inspect_normalized_events [raw_path]");
    }

    try {
        return inspect(raw_path);
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}
