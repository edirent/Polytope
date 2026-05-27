#include "feed/decode/EventNormalizer.h"
#include "feed/decode/JsonDecoder.h"
#include "feed/raw_ingest/RawLogReader.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

namespace {

using trading_engine::feed::EventNormalizer;
using trading_engine::feed::JsonDecodeStatus;
using trading_engine::feed::JsonDecoder;
using trading_engine::feed::NormalizationResult;
using trading_engine::feed::NormalizedEvent;
using trading_engine::feed::NormalizedEventType;
using trading_engine::feed::RawLogReadResult;
using trading_engine::feed::RawLogReader;

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
    JsonDecodeStatus status,
    NormalizedInspection& inspection
) {
    switch (status) {
        case JsonDecodeStatus::JsonObject:
            ++inspection.decode_json_object;
            break;

        case JsonDecodeStatus::JsonArray:
            ++inspection.decode_json_array;
            break;

        case JsonDecodeStatus::NonJsonControl:
            ++inspection.decode_control;
            break;

        case JsonDecodeStatus::UnsupportedJson:
        case JsonDecodeStatus::MalformedJson:
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
    }
}

void count_normalization_result(
    const NormalizationResult& result,
    NormalizedInspection& inspection
) {
    inspection.normalization_warnings +=
        static_cast<std::uint64_t>(result.warnings.size());

    if (!result.ok()) {
        ++inspection.normalization_errors;
        return;
    }

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
    JsonDecoder decoder;
    EventNormalizer normalizer;
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

        const auto decoded = decoder.decode(*raw_result.packet);
        count_decode_status(decoded.status, inspection);

        if (decoded.has_json_event_payload()) {
            count_normalization_result(
                normalizer.normalize_json(*raw_result.packet, decoded.json),
                inspection
            );
        } else if (decoded.has_control_payload()) {
            count_normalization_result(
                normalizer.normalize_control(
                    *raw_result.packet,
                    decoded.control_payload
                ),
                inspection
            );
        }
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
