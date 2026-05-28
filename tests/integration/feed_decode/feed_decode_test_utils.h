#pragma once

#include "decode/core/DecodePipeline.h"
#include "decode/json/JsonDecodeResult.h"
#include "decode/public/DecodeError.h"
#include "decode/public/NormalizedEvent.h"
#include "decode/public/NormalizedEventBatch.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "feed/raw_ingest/RawPacket.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace feed_decode_test {

using trading_engine::decode::DecodeErrorCode;
using trading_engine::decode::DecodePipeline;
using trading_engine::decode::DecodePipelineResult;
using trading_engine::decode::JsonDecodeKind;
using trading_engine::decode::NormalizedEvent;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::decode::NormalizedEventType;
using trading_engine::feed::RawLogReadResult;
using trading_engine::feed::RawLogReader;
using trading_engine::feed::RawPacket;
using trading_engine::feed::to_decode_input_view;

struct FeedDecodeCounts {
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

struct FeedDecodeRun {
    FeedDecodeCounts counts;
    std::uint64_t normalized_trace_hash{1469598103934665603ULL};
};

[[noreturn]] inline void fail(const std::string& message) {
    throw std::runtime_error(message);
}

inline void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

inline void expect_false(bool value, const std::string& field) {
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

inline std::string market39_fixture_path() {
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

inline void count_decode_kind(
    JsonDecodeKind kind,
    FeedDecodeCounts& counts
) {
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

inline void count_event(
    const NormalizedEvent& event,
    FeedDecodeCounts& counts
) {
    ++counts.normalized_events;

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

inline void hash_bytes(
    std::uint64_t& hash,
    const void* data,
    std::size_t size
) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= 1099511628211ULL;
    }
}

template <typename T>
void hash_value(std::uint64_t& hash, const T& value) {
    hash_bytes(hash, &value, sizeof(T));
}

inline void hash_string(std::uint64_t& hash, const std::string& value) {
    hash_value(hash, static_cast<std::uint64_t>(value.size()));
    hash_bytes(hash, value.data(), value.size());
}

inline void hash_event(
    std::uint64_t& hash,
    const NormalizedEvent& event
) {
    hash_value(hash, static_cast<std::uint64_t>(event.event_type));
    hash_value(hash, event.packet_id);
    hash_value(hash, event.recv_wall_ns);
    hash_value(hash, event.recv_monotonic_ns);
    hash_value(hash, static_cast<std::uint16_t>(event.source_id));
    hash_string(hash, event.raw_type);
    hash_string(hash, event.entity_id);
    hash_string(hash, event.asset_id);
    hash_string(hash, event.market_id);
    hash_string(hash, event.condition_id);
    hash_value(hash, event.event_ts);

    hash_value(hash, static_cast<std::uint64_t>(event.bids.size()));
    for (const auto& level : event.bids) {
        hash_value(hash, level.price);
        hash_value(hash, level.size);
    }

    hash_value(hash, static_cast<std::uint64_t>(event.asks.size()));
    for (const auto& level : event.asks) {
        hash_value(hash, level.price);
        hash_value(hash, level.size);
    }

    hash_value(hash, static_cast<std::uint64_t>(event.changes.size()));
    for (const auto& change : event.changes) {
        hash_value(hash, static_cast<std::uint64_t>(change.side));
        hash_value(hash, change.price);
        hash_value(hash, change.size);
    }

    hash_value(hash, event.best_bid.has_value());
    if (event.best_bid) {
        hash_value(hash, *event.best_bid);
    }

    hash_value(hash, event.best_ask.has_value());
    if (event.best_ask) {
        hash_value(hash, *event.best_ask);
    }

    hash_value(hash, event.tick_size.has_value());
    if (event.tick_size) {
        hash_value(hash, *event.tick_size);
    }

    hash_string(hash, event.winning_asset_id);
}

inline FeedDecodeRun run_market39_decode() {
    DecodePipeline pipeline;
    RawLogReader reader(market39_fixture_path());
    FeedDecodeRun run;

    while (true) {
        RawLogReadResult raw = reader.next();
        if (raw.eof()) {
            break;
        }

        if (!raw.ok()) {
            fail("raw read failed: " + raw.message);
        }

        ++run.counts.packets_read;

        NormalizedEventBatch batch;
        const DecodePipelineResult decoded = pipeline.decode(
            to_decode_input_view(*raw.packet),
            &batch
        );

        count_decode_kind(decoded.payload_kind, run.counts);

        if (!decoded.ok()) {
            if (decoded.payload_kind == JsonDecodeKind::JsonObject ||
                decoded.payload_kind == JsonDecodeKind::JsonArray ||
                decoded.payload_kind == JsonDecodeKind::NonJsonControl) {
                ++run.counts.normalization_errors;
            }
            continue;
        }

        if (batch.overflowed) {
            ++run.counts.normalization_errors;
        }

        for (const auto& event : batch.events) {
            count_event(event, run.counts);
            hash_event(run.normalized_trace_hash, event);
        }
    }

    return run;
}

inline void expect_market39_counts(const FeedDecodeCounts& counts) {
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
    expect_equal(counts.normalization_errors, 0ULL, "normalization_errors");
}

inline RawPacket make_test_packet(std::string payload) {
    RawPacket packet;
    packet.header.packet_id = 42;
    packet.header.connection_id = 7;
    packet.header.recv_wall_ns = 111;
    packet.header.recv_monotonic_ns = 222;
    packet.header.source_id = trading_engine::feed::SourceId::PolymarketMarket;
    packet.header.codec = trading_engine::feed::Codec::None;
    packet.header.flags =
        trading_engine::feed::PacketHeartbeat |
        trading_engine::feed::PacketReplayed;
    packet.payload = std::move(payload);
    packet.header.payload_len =
        static_cast<std::uint32_t>(packet.payload.size());
    return packet;
}

}  // namespace feed_decode_test
