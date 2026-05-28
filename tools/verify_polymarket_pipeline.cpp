#include "decode/core/DecodePipeline.h"
#include "decode/json/JsonDecodeResult.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawLogReader.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

using trading_engine::decode::DecodePipeline;
using trading_engine::decode::JsonDecodeKind;
using trading_engine::decode::NormalizedEvent;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::decode::NormalizedEventType;
using trading_engine::feed::RawLogReadResult;
using trading_engine::feed::RawLogReader;
using trading_engine::feed::to_decode_input_view;

struct PipelineCounts {
    std::uint64_t packets{0};
    std::uint64_t json_packets{0};
    std::uint64_t control_packets{0};
    std::uint64_t normalized_events{0};
    std::uint64_t snapshot{0};
    std::uint64_t delta{0};
    std::uint64_t heartbeat{0};
    std::uint64_t unknown{0};
    std::uint64_t fatal_errors{0};
};

void count_event_type(const NormalizedEvent& event, PipelineCounts& counts) {
    ++counts.normalized_events;

    switch (event.event_type) {
        case NormalizedEventType::Snapshot:
            ++counts.snapshot;
            break;

        case NormalizedEventType::Delta:
            ++counts.delta;
            break;

        case NormalizedEventType::Heartbeat:
            ++counts.heartbeat;
            break;

        case NormalizedEventType::Unknown:
            ++counts.unknown;
            break;

        case NormalizedEventType::StatusChange:
        case NormalizedEventType::LifecycleEvent:
        case NormalizedEventType::TradeEvent:
        case NormalizedEventType::DecodeError:
            break;
    }
}

void print_counts(const PipelineCounts& counts) {
    std::cout << "packets: " << counts.packets << '\n';
    std::cout << "json_packets: " << counts.json_packets << '\n';
    std::cout << "control_packets: " << counts.control_packets << '\n';
    std::cout << "normalized_events: " << counts.normalized_events << '\n';
    std::cout << "snapshot: " << counts.snapshot << '\n';
    std::cout << "delta: " << counts.delta << '\n';
    std::cout << "heartbeat: " << counts.heartbeat << '\n';
    std::cout << "unknown: " << counts.unknown << '\n';
    std::cout << "fatal_errors: " << counts.fatal_errors << '\n';
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
        << "verify_polymarket_pipeline failed: " << field
        << " expected " << expected
        << " got " << actual
        << '\n';
    return false;
}

bool expect_market_39_counts(const PipelineCounts& counts) {
    bool ok = true;

    ok &= expect_equal(counts.packets, 39, "packets");
    ok &= expect_equal(counts.json_packets, 36, "json_packets");
    ok &= expect_equal(counts.control_packets, 3, "control_packets");
    ok &= expect_equal(counts.normalized_events, 39, "normalized_events");
    ok &= expect_equal(counts.snapshot, 1, "snapshot");
    ok &= expect_equal(counts.delta, 35, "delta");
    ok &= expect_equal(counts.heartbeat, 3, "heartbeat");
    ok &= expect_equal(counts.unknown, 0, "unknown");
    ok &= expect_equal(counts.fatal_errors, 0, "fatal_errors");

    return ok;
}

int verify(const std::string& raw_path, bool expect_market_39) {
    RawLogReader reader(raw_path);
    DecodePipeline pipeline;
    PipelineCounts counts;
    std::vector<NormalizedEvent> normalized_events;

    while (true) {
        RawLogReadResult read_result = reader.next();
        if (read_result.eof()) {
            break;
        }

        if (!read_result.ok()) {
            ++counts.fatal_errors;
            std::cerr
                << "raw read failed after packet " << counts.packets
                << ": " << read_result.message
                << '\n';
            break;
        }

        ++counts.packets;

        NormalizedEventBatch batch;
        const auto decoded = pipeline.decode(
            to_decode_input_view(*read_result.packet),
            &batch
        );

        if (decoded.payload_kind == JsonDecodeKind::JsonObject ||
            decoded.payload_kind == JsonDecodeKind::JsonArray) {
            ++counts.json_packets;
        } else if (decoded.payload_kind == JsonDecodeKind::NonJsonControl) {
            ++counts.control_packets;
        }

        if (!decoded.ok()) {
            ++counts.fatal_errors;
            std::cerr
                << "decode failed at packet_id "
                << read_result.packet->header.packet_id
                << ": " << decoded.error.message
                << '\n';
            continue;
        }

        if (batch.overflowed) {
            ++counts.fatal_errors;
            std::cerr
                << "normalize failed at packet_id "
                << read_result.packet->header.packet_id
                << ": too many normalized events in packet"
                << '\n';
            continue;
        }

        for (const auto& event : batch.events) {
            count_event_type(event, counts);
            normalized_events.push_back(event);
        }
    }

    print_counts(counts);

    if (expect_market_39 && !expect_market_39_counts(counts)) {
        return 1;
    }

    return counts.fatal_errors == 0 ? 0 : 1;
}

int fail(const std::string& message) {
    std::cerr << "verify_polymarket_pipeline failed: " << message << '\n';
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string raw_path = "tests/fixtures/polymarket/market_39.raw";
    bool expect_market_39 = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--expect-market-39") {
            expect_market_39 = true;
            continue;
        }

        if (arg == "--help" || arg == "-h") {
            std::cout
                << "usage: verify_polymarket_pipeline [raw_path] "
                << "[--expect-market-39]\n";
            return 0;
        }

        if (raw_path != "tests/fixtures/polymarket/market_39.raw") {
            return fail("multiple raw paths provided");
        }

        raw_path = arg;
    }

    try {
        return verify(raw_path, expect_market_39);
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}
