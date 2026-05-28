#include "decode/core/DecodePipeline.h"
#include "decode/json/JsonDecodeResult.h"
#include "decode/public/DecodeError.h"
#include "decode/public/DecodeInputView.h"
#include "decode/public/NormalizedEvent.h"
#include "decode/public/NormalizedEventBatch.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawLogReader.h"

#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

using trading_engine::decode::DecodeErrorCode;
using trading_engine::decode::DecodeInputView;
using trading_engine::decode::DecodePipeline;
using trading_engine::decode::JsonDecodeKind;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::decode::NormalizedEventType;
using trading_engine::decode::SourceId;
using trading_engine::feed::RawLogReadResult;
using trading_engine::feed::RawLogReader;
using trading_engine::feed::to_decode_input_view;

struct PipelineCounts {
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
};

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

DecodeInputView input_for(
    std::string_view payload,
    std::uint16_t codec = 0
) {
    return DecodeInputView{
        .packet_id = 1,
        .connection_id = 1,
        .recv_wall_ns = 100,
        .recv_monotonic_ns = 200,
        .source_id = SourceId::PolymarketMarket,
        .codec = codec,
        .flags = 0,
        .payload = payload
    };
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

void count_payload_kind(JsonDecodeKind kind, PipelineCounts& counts) {
    switch (kind) {
        case JsonDecodeKind::JsonObject:
            ++counts.decoded_json_object;
            break;
        case JsonDecodeKind::JsonArray:
            ++counts.decoded_json_array;
            break;
        case JsonDecodeKind::NonJsonControl:
            ++counts.decoded_control;
            break;
        case JsonDecodeKind::UnsupportedJson:
        case JsonDecodeKind::MalformedJson:
            ++counts.decode_errors;
            break;
    }
}

void count_event_type(
    const NormalizedEventBatch& batch,
    PipelineCounts& counts
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
                ++counts.unknown_events;
                break;
            case NormalizedEventType::StatusChange:
            case NormalizedEventType::LifecycleEvent:
            case NormalizedEventType::TradeEvent:
            case NormalizedEventType::DecodeError:
                break;
        }
    }
}

void DecodePipeline_BookProducesSnapshot() {
    DecodePipeline pipeline;
    NormalizedEventBatch batch;

    const auto result = pipeline.decode(
        input_for(
            R"({"event_type":"book","asset_id":"asset_x","bids":[],"asks":[]})"
        ),
        &batch
    );

    expect_true(result.ok(), "decode ok");
    expect_equal(result.payload_kind, JsonDecodeKind::JsonObject, "kind");
    expect_equal(batch.size(), 1U, "batch size");
    expect_equal(
        batch.events.front().event_type,
        NormalizedEventType::Snapshot,
        "event type"
    );
    expect_equal(batch.events.front().entity_id, std::string{"asset_x"}, "id");
}

void DecodePipeline_PriceChangeProducesDelta() {
    DecodePipeline pipeline;
    NormalizedEventBatch batch;

    const auto result = pipeline.decode(
        input_for(
            R"({"event_type":"price_change","market":"market_x","price_changes":[{"asset_id":"asset_x","price":"0.42","size":"10","side":"BUY"}]})"
        ),
        &batch
    );

    expect_true(result.ok(), "decode ok");
    expect_equal(result.payload_kind, JsonDecodeKind::JsonObject, "kind");
    expect_equal(batch.size(), 1U, "batch size");
    expect_equal(
        batch.events.front().event_type,
        NormalizedEventType::Delta,
        "event type"
    );
    expect_equal(batch.events.front().entity_id, std::string{"asset_x"}, "id");
    expect_equal(batch.events.front().changes.size(), 1U, "changes");
}

void DecodePipeline_ControlProducesHeartbeat() {
    DecodePipeline pipeline;
    NormalizedEventBatch batch;

    const auto result = pipeline.decode(input_for("PONG"), &batch);

    expect_true(result.ok(), "decode ok");
    expect_equal(result.payload_kind, JsonDecodeKind::NonJsonControl, "kind");
    expect_equal(result.error.code, DecodeErrorCode::NonJsonControl, "code");
    expect_equal(batch.size(), 1U, "batch size");
    expect_equal(
        batch.events.front().event_type,
        NormalizedEventType::Heartbeat,
        "event type"
    );
}

void DecodePipeline_ArrayWrappedPayloadWorks() {
    DecodePipeline pipeline;
    NormalizedEventBatch batch;

    const auto result = pipeline.decode(
        input_for(
            R"([{"event_type":"book","asset_id":"asset_x","bids":[],"asks":[]}])"
        ),
        &batch
    );

    expect_true(result.ok(), "decode ok");
    expect_equal(result.payload_kind, JsonDecodeKind::JsonArray, "kind");
    expect_equal(batch.size(), 1U, "batch size");
    expect_equal(
        batch.events.front().event_type,
        NormalizedEventType::Snapshot,
        "event type"
    );
}

void DecodePipeline_MalformedJsonProducesErrorOrNoCrash() {
    DecodePipeline pipeline;
    NormalizedEventBatch batch;

    const auto result = pipeline.decode(input_for(R"({"event_type":)"), &batch);

    expect_false(result.ok(), "decode ok");
    expect_equal(result.payload_kind, JsonDecodeKind::MalformedJson, "kind");
    expect_equal(result.error.code, DecodeErrorCode::MalformedJson, "code");
    expect_true(batch.empty(), "batch empty");
}

void DecodePipeline_UnsupportedCodecRejected() {
    DecodePipeline pipeline;
    NormalizedEventBatch batch;

    const auto result = pipeline.decode(
        input_for(R"({"event_type":"book"})", 1),
        &batch
    );

    expect_false(result.ok(), "decode ok");
    expect_equal(result.error.code, DecodeErrorCode::UnsupportedCodec, "code");
    expect_true(batch.empty(), "batch empty");
}

void DecodePipeline_Market39ProducesExpectedCounts() {
    DecodePipeline pipeline;
    PipelineCounts counts;
    RawLogReader reader(market39_fixture_path());

    while (true) {
        RawLogReadResult raw = reader.next();
        if (raw.eof()) {
            break;
        }

        if (!raw.ok()) {
            fail("raw log read failed: " + raw.message);
        }

        ++counts.packets_read;

        NormalizedEventBatch batch;
        const auto result = pipeline.decode(
            to_decode_input_view(*raw.packet),
            &batch
        );

        count_payload_kind(result.payload_kind, counts);

        if (!result.ok()) {
            ++counts.decode_errors;
            continue;
        }

        count_event_type(batch, counts);
    }

    expect_equal(counts.packets_read, 39ULL, "packets_read");
    expect_equal(counts.decoded_json_object, 35ULL, "decoded_json_object");
    expect_equal(counts.decoded_json_array, 1ULL, "decoded_json_array");
    expect_equal(counts.decoded_control, 3ULL, "decoded_control");
    expect_equal(counts.decode_errors, 0ULL, "decode_errors");

    expect_equal(counts.normalized_events, 39ULL, "normalized_events");
    expect_equal(counts.snapshot_events, 1ULL, "snapshot_events");
    expect_equal(counts.delta_events, 35ULL, "delta_events");
    expect_equal(counts.heartbeat_events, 3ULL, "heartbeat_events");
    expect_equal(counts.unknown_events, 0ULL, "unknown_events");
    expect_equal(
        counts.normalization_errors,
        0ULL,
        "normalization_errors"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"DecodePipeline_BookProducesSnapshot",
         &DecodePipeline_BookProducesSnapshot},
        {"DecodePipeline_PriceChangeProducesDelta",
         &DecodePipeline_PriceChangeProducesDelta},
        {"DecodePipeline_ControlProducesHeartbeat",
         &DecodePipeline_ControlProducesHeartbeat},
        {"DecodePipeline_ArrayWrappedPayloadWorks",
         &DecodePipeline_ArrayWrappedPayloadWorks},
        {"DecodePipeline_MalformedJsonProducesErrorOrNoCrash",
         &DecodePipeline_MalformedJsonProducesErrorOrNoCrash},
        {"DecodePipeline_UnsupportedCodecRejected",
         &DecodePipeline_UnsupportedCodecRejected},
        {"DecodePipeline_Market39ProducesExpectedCounts",
         &DecodePipeline_Market39ProducesExpectedCounts}
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
