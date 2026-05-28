#include "chain_confirm/ClassifiedFillRecord.h"
#include "decode/core/DecodePipeline.h"
#include "decode/json/JsonDecodeResult.h"
#include "decode/public/NormalizedEventBatch.h"
#include "feed/decode/DecodeInputAdapter.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "feed/raw_ingest/RawPacket.h"
#include "state/MarketStateView.h"
#include "state/core/MarketStateEventAdapter.h"
#include "state/core/MarketStateStore.h"

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace json = boost::json;

using trading_engine::chain_confirm::ClassifiedFillRecord;
using trading_engine::chain_confirm::ConfirmedDirection;
using trading_engine::chain_confirm::FillClassification;
using trading_engine::chain_confirm::FillMappingStatus;
using trading_engine::decode::DecodePipeline;
using trading_engine::decode::JsonDecodeKind;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::decode::NormalizedEventType;
using trading_engine::feed::RawLogReadResult;
using trading_engine::feed::RawLogReader;
using trading_engine::feed::RawPacket;
using trading_engine::feed::to_decode_input_view;
using trading_engine::state::AggressorSide;
using trading_engine::state::MarketStateEventType;
using trading_engine::state::MarketStateSnapshot;
using trading_engine::state::MarketStateStore;
using trading_engine::state::MarketStateView;
using trading_engine::state::StateApplyCode;
using trading_engine::state::from_classified_fill;
using trading_engine::state::from_normalized_batch;

constexpr std::uint64_t kMarket39LegacyBookHash = 12959912045291989833ULL;

struct Config {
    std::string raw_path{"tests/fixtures/polymarket/market_39.raw"};
    std::string chain_fixture{
        "tests/fixtures/chain_confirm/synthetic_order_filled.jsonl"
    };
    std::uint64_t repeat{1};
    bool check_determinism{false};
};

struct LatencyStats {
    std::uint64_t p50{0};
    std::uint64_t p95{0};
    std::uint64_t p99{0};
    std::uint64_t max{0};
};

struct LatencySamples {
    std::vector<std::uint64_t> total_workflow_ns;
    std::vector<std::uint64_t> decode_ns;
    std::vector<std::uint64_t> state_apply_ns;
    std::vector<std::uint64_t> snapshot_read_ns;
};

struct WsPathSummary {
    std::uint64_t packets_read{0};
    std::uint64_t normalized_events{0};
    std::uint64_t snapshot_events{0};
    std::uint64_t delta_events{0};
    std::uint64_t heartbeat_events{0};
    std::uint64_t decode_errors{0};
    std::uint64_t normalization_errors{0};
};

struct ChainPathSummary {
    std::uint64_t chain_logs{0};
    std::uint64_t classified_fills{0};
    std::uint64_t buy_aggressor_fills{0};
    std::uint64_t sell_aggressor_fills{0};
    std::uint64_t unknown_fills{0};
    std::uint64_t ambiguous_fills{0};
    std::uint64_t removed_fills{0};
};

struct StatePathSummary {
    std::uint64_t book_snapshots_applied{0};
    std::uint64_t book_deltas_applied{0};
    std::uint64_t chain_fills_applied{0};
    std::uint64_t state_errors{0};
    std::uint64_t entity_count{0};
};

struct SnapshotOutputSummary {
    bool snapshot_ok{false};
    bool has_book{false};
    bool has_chain_state{false};
    bool has_quality_state{false};
    bool usable_for_depth{false};
    bool usable_for_signal{false};
    std::string market_id;
    std::string asset_id;
};

struct HashSummary {
    std::uint64_t legacy_book_hash{0};
    std::uint64_t chain_hash{0};
    std::uint64_t combined_state_hash{0};
    bool determinism_passed{true};
};

struct WorkflowSummary {
    WsPathSummary ws;
    ChainPathSummary chain;
    StatePathSummary state;
    SnapshotOutputSummary snapshot;
    HashSummary hashes;
};

struct TemplateFill {
    ClassifiedFillRecord fill;
};

struct BookFingerprint {
    std::uint64_t state_hash{0};
    std::uint32_t bid_count{0};
    std::uint32_t ask_count{0};
    bool has_bid{false};
    bool has_ask{false};
    std::int64_t best_bid_tick{0};
    std::int64_t best_ask_tick{0};
};

struct AppliedFill {
    ConfirmedDirection direction{ConfirmedDirection::Unknown};
    std::int64_t size_lots{0};
    bool active{false};
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

bool is_chain_confirmed(const ClassifiedFillRecord& fill) noexcept {
    return fill.classification == FillClassification::ChainConfirmed &&
           !fill.removed;
}

bool is_removed(const ClassifiedFillRecord& fill) noexcept {
    return fill.removed ||
           fill.classification == FillClassification::ChainRemoved;
}

bool is_ambiguous(const ClassifiedFillRecord& fill) noexcept {
    return fill.classification == FillClassification::AmbiguousFill ||
           fill.mapping_status == FillMappingStatus::AmbiguousFill;
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

LatencyStats summarize_latency(std::vector<std::uint64_t> values) {
    LatencyStats out;
    if (values.empty()) {
        return out;
    }

    std::sort(values.begin(), values.end());
    out.p50 = percentile(values, 50, 100);
    out.p95 = percentile(values, 95, 100);
    out.p99 = percentile(values, 99, 100);
    out.max = values.back();
    return out;
}

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= 1099511628211ULL;
}

void hash_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) {
        hash_byte(
            hash,
            static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF)
        );
    }
}

std::uint64_t chain_hash_from_snapshot(
    const MarketStateSnapshot& snapshot
) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    hash_byte(hash, snapshot.has_confirmed_trade ? 1U : 0U);
    hash_u64(hash, static_cast<std::uint64_t>(snapshot.last_trade_price_tick));
    hash_u64(hash, static_cast<std::uint64_t>(snapshot.last_trade_size_lots));
    hash_u64(hash, static_cast<std::uint64_t>(snapshot.last_taker_side));
    hash_u64(hash, static_cast<std::uint64_t>(snapshot.confirmed_buy_lots_2s));
    hash_u64(hash, static_cast<std::uint64_t>(snapshot.confirmed_sell_lots_2s));
    hash_u64(hash, static_cast<std::uint64_t>(snapshot.confirmed_buy_lots_10s));
    hash_u64(hash, static_cast<std::uint64_t>(snapshot.confirmed_sell_lots_10s));
    return hash;
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

const json::value& require_field(
    const json::object& object,
    const char* name
) {
    const auto it = object.find(name);
    if (it == object.end()) {
        fail(std::string{"chain fixture missing field: "} + name);
    }
    return it->value();
}

std::string json_string(const json::object& object, const char* name) {
    return json::value_to<std::string>(require_field(object, name));
}

std::string json_string_default(
    const json::object& object,
    const char* name,
    const std::string& fallback = {}
) {
    const auto it = object.find(name);
    if (it == object.end() || it->value().is_null()) {
        return fallback;
    }
    return json::value_to<std::string>(it->value());
}

std::uint64_t json_u64(const json::object& object, const char* name) {
    return json::value_to<std::uint64_t>(require_field(object, name));
}

std::uint32_t json_u32(const json::object& object, const char* name) {
    return static_cast<std::uint32_t>(json_u64(object, name));
}

std::int64_t json_i64(const json::object& object, const char* name) {
    return json::value_to<std::int64_t>(require_field(object, name));
}

bool json_bool(const json::object& object, const char* name) {
    return json::value_to<bool>(require_field(object, name));
}

ConfirmedDirection parse_direction(const std::string& value) {
    if (value == "BuyAggressor") {
        return ConfirmedDirection::BuyAggressor;
    }
    if (value == "SellAggressor") {
        return ConfirmedDirection::SellAggressor;
    }
    return ConfirmedDirection::Unknown;
}

FillMappingStatus parse_mapping_status(const std::string& value) {
    if (value == "Mapped") {
        return FillMappingStatus::Mapped;
    }
    if (value == "AmbiguousFill") {
        return FillMappingStatus::AmbiguousFill;
    }
    return FillMappingStatus::UnmappedFill;
}

FillClassification parse_classification(const std::string& value) {
    if (value == "ChainConfirmed") {
        return FillClassification::ChainConfirmed;
    }
    if (value == "ChainRemoved") {
        return FillClassification::ChainRemoved;
    }
    if (value == "AmbiguousFill") {
        return FillClassification::AmbiguousFill;
    }
    if (value == "UnmappedFill") {
        return FillClassification::UnmappedFill;
    }
    return FillClassification::Unknown;
}

TemplateFill parse_template_fill(const json::object& object) {
    TemplateFill out;
    out.fill.fill_id = json_string(object, "fill_id");
    out.fill.order_hash = json_string_default(object, "order_hash");
    out.fill.market_id = json_string(object, "market_id");
    out.fill.asset_id = json_string(object, "asset_id");
    out.fill.price_tick = json_i64(object, "price_tick");
    out.fill.size_lots = json_i64(object, "size_lots");
    out.fill.direction = parse_direction(json_string(object, "direction"));
    out.fill.mapping_status = parse_mapping_status(
        json_string(object, "mapping_status")
    );
    out.fill.classification = parse_classification(
        json_string(object, "classification")
    );
    out.fill.block_number = json_u64(object, "block_number");
    out.fill.tx_hash = json_string(object, "tx_hash");
    out.fill.log_index = json_u32(object, "log_index");
    out.fill.chain_seen_monotonic_ns =
        json_u64(object, "chain_seen_monotonic_ns");
    out.fill.source_sequence = json_u64(object, "source_sequence");
    out.fill.removed = json_bool(object, "removed");
    return out;
}

std::vector<TemplateFill> load_chain_fixture(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        fail("failed to open chain fixture: " + path);
    }

    std::vector<TemplateFill> fills;
    std::string line;
    std::uint64_t line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }

        boost::json::error_code error;
        const json::value parsed = json::parse(line, error);
        if (error) {
            fail(
                "failed to parse chain fixture line " +
                std::to_string(line_number) + ": " + error.message()
            );
        }
        if (!parsed.is_object()) {
            fail("chain fixture line is not an object");
        }

        const json::object& object = parsed.as_object();
        const std::string record_type =
            json_string_default(object, "record_type", "classified_fill");
        if (record_type != "classified_fill") {
            fail("unsupported chain fixture record_type: " + record_type);
        }
        fills.push_back(parse_template_fill(object));
    }

    if (fills.empty()) {
        fail("chain fixture is empty");
    }
    return fills;
}

std::string substitute_context(
    std::string value,
    const std::string& asset_id,
    const std::string& market_id
) {
    if (value == "${WS_ASSET_ID}") {
        return asset_id;
    }
    if (value == "${WS_MARKET_ID}") {
        return market_id;
    }
    return value;
}

ClassifiedFillRecord materialize_fill(
    const TemplateFill& input,
    const std::string& asset_id,
    const std::string& market_id,
    std::uint64_t chain_seen_base_ns
) {
    ClassifiedFillRecord out = input.fill;
    out.asset_id = substitute_context(out.asset_id, asset_id, market_id);
    out.market_id = substitute_context(out.market_id, asset_id, market_id);
    if (out.chain_seen_monotonic_ns < chain_seen_base_ns) {
        out.chain_seen_monotonic_ns += chain_seen_base_ns;
    }
    return out;
}

BookFingerprint fingerprint(const MarketStateSnapshot& snapshot) {
    return BookFingerprint{
        snapshot.state_hash,
        snapshot.bid_count,
        snapshot.ask_count,
        snapshot.has_bid,
        snapshot.has_ask,
        snapshot.best_bid_tick,
        snapshot.best_ask_tick
    };
}

bool same_book(
    const BookFingerprint& before,
    const MarketStateSnapshot& after
) noexcept {
    return before.state_hash == after.state_hash &&
           before.bid_count == after.bid_count &&
           before.ask_count == after.ask_count &&
           before.has_bid == after.has_bid &&
           before.has_ask == after.has_ask &&
           before.best_bid_tick == after.best_bid_tick &&
           before.best_ask_tick == after.best_ask_tick;
}

void count_normalized_events(
    const NormalizedEventBatch& batch,
    WorkflowSummary* summary,
    std::string* asset_id,
    std::string* market_id,
    std::uint64_t* last_recv_ns
) {
    summary->ws.normalized_events += static_cast<std::uint64_t>(batch.size());
    if (batch.overflowed) {
        ++summary->ws.normalization_errors;
    }

    for (const auto& event : batch.events) {
        if (event.recv_monotonic_ns > *last_recv_ns) {
            *last_recv_ns = event.recv_monotonic_ns;
        }

        if (event.event_type != NormalizedEventType::Heartbeat) {
            if (asset_id->empty() && !event.entity_id.empty()) {
                *asset_id = event.entity_id;
            }
            if (market_id->empty() && !event.market_id.empty()) {
                *market_id = event.market_id;
            }
        }

        switch (event.event_type) {
            case NormalizedEventType::Snapshot:
                ++summary->ws.snapshot_events;
                break;
            case NormalizedEventType::Delta:
                ++summary->ws.delta_events;
                break;
            case NormalizedEventType::Heartbeat:
                ++summary->ws.heartbeat_events;
                break;
            default:
                break;
        }
    }
}

void count_state_event(
    MarketStateEventType type,
    const trading_engine::state::StateApplyResult& result,
    WorkflowSummary* summary
) {
    if (!result.ok()) {
        ++summary->state.state_errors;
        return;
    }

    if (type == MarketStateEventType::WsBookSnapshot &&
        result.code == StateApplyCode::Applied) {
        ++summary->state.book_snapshots_applied;
    } else if (type == MarketStateEventType::WsBookDelta &&
               result.code == StateApplyCode::Applied) {
        ++summary->state.book_deltas_applied;
    } else if (
        (type == MarketStateEventType::ChainConfirmedFill ||
         type == MarketStateEventType::ChainRemovedFill) &&
        result.state_changed) {
        ++summary->state.chain_fills_applied;
    }
}

void count_chain_fill(
    const ClassifiedFillRecord& fill,
    WorkflowSummary* summary
) {
    ++summary->chain.chain_logs;
    ++summary->chain.classified_fills;

    if (is_removed(fill)) {
        ++summary->chain.removed_fills;
        return;
    }

    if (is_ambiguous(fill)) {
        ++summary->chain.ambiguous_fills;
        return;
    }

    if (fill.direction == ConfirmedDirection::BuyAggressor &&
        is_chain_confirmed(fill)) {
        ++summary->chain.buy_aggressor_fills;
    } else if (
        fill.direction == ConfirmedDirection::SellAggressor &&
        is_chain_confirmed(fill)) {
        ++summary->chain.sell_aggressor_fills;
    } else {
        ++summary->chain.unknown_fills;
    }
}

void update_expected_flow(
    const ClassifiedFillRecord& fill,
    std::map<std::string, AppliedFill>* active_fills,
    std::int64_t* expected_buy_lots,
    std::int64_t* expected_sell_lots
) {
    if (is_removed(fill)) {
        const auto it = active_fills->find(fill.fill_id);
        if (it != active_fills->end() && it->second.active) {
            if (it->second.direction == ConfirmedDirection::BuyAggressor) {
                *expected_buy_lots -= it->second.size_lots;
            } else if (
                it->second.direction ==
                ConfirmedDirection::SellAggressor) {
                *expected_sell_lots -= it->second.size_lots;
            }
            it->second.active = false;
        }
        return;
    }

    if (!is_chain_confirmed(fill) || is_ambiguous(fill)) {
        return;
    }

    if (fill.direction == ConfirmedDirection::BuyAggressor) {
        *expected_buy_lots += fill.size_lots;
        (*active_fills)[fill.fill_id] = AppliedFill{
            fill.direction,
            fill.size_lots,
            true
        };
    } else if (fill.direction == ConfirmedDirection::SellAggressor) {
        *expected_sell_lots += fill.size_lots;
        (*active_fills)[fill.fill_id] = AppliedFill{
            fill.direction,
            fill.size_lots,
            true
        };
    }
}

bool same_summary_except_determinism(
    const WorkflowSummary& lhs,
    const WorkflowSummary& rhs
) noexcept {
    return lhs.ws.packets_read == rhs.ws.packets_read &&
           lhs.ws.normalized_events == rhs.ws.normalized_events &&
           lhs.ws.snapshot_events == rhs.ws.snapshot_events &&
           lhs.ws.delta_events == rhs.ws.delta_events &&
           lhs.ws.heartbeat_events == rhs.ws.heartbeat_events &&
           lhs.ws.decode_errors == rhs.ws.decode_errors &&
           lhs.ws.normalization_errors == rhs.ws.normalization_errors &&
           lhs.chain.chain_logs == rhs.chain.chain_logs &&
           lhs.chain.classified_fills == rhs.chain.classified_fills &&
           lhs.chain.buy_aggressor_fills == rhs.chain.buy_aggressor_fills &&
           lhs.chain.sell_aggressor_fills == rhs.chain.sell_aggressor_fills &&
           lhs.chain.unknown_fills == rhs.chain.unknown_fills &&
           lhs.chain.ambiguous_fills == rhs.chain.ambiguous_fills &&
           lhs.chain.removed_fills == rhs.chain.removed_fills &&
           lhs.state.book_snapshots_applied ==
               rhs.state.book_snapshots_applied &&
           lhs.state.book_deltas_applied == rhs.state.book_deltas_applied &&
           lhs.state.chain_fills_applied == rhs.state.chain_fills_applied &&
           lhs.state.state_errors == rhs.state.state_errors &&
           lhs.state.entity_count == rhs.state.entity_count &&
           lhs.snapshot.snapshot_ok == rhs.snapshot.snapshot_ok &&
           lhs.snapshot.has_book == rhs.snapshot.has_book &&
           lhs.snapshot.has_chain_state == rhs.snapshot.has_chain_state &&
           lhs.snapshot.has_quality_state == rhs.snapshot.has_quality_state &&
           lhs.hashes.legacy_book_hash == rhs.hashes.legacy_book_hash &&
           lhs.hashes.chain_hash == rhs.hashes.chain_hash &&
           lhs.hashes.combined_state_hash == rhs.hashes.combined_state_hash;
}

void record_latency(
    LatencySamples* samples,
    std::uint64_t total_ns,
    std::uint64_t decode_ns,
    std::uint64_t state_apply_ns,
    std::uint64_t snapshot_read_ns
) {
    samples->total_workflow_ns.push_back(total_ns);
    if (decode_ns > 0) {
        samples->decode_ns.push_back(decode_ns);
    }
    samples->state_apply_ns.push_back(state_apply_ns);
    samples->snapshot_read_ns.push_back(snapshot_read_ns);
}

WorkflowSummary run_once(
    const std::vector<RawPacket>& packets,
    const std::vector<TemplateFill>& chain_templates,
    LatencySamples* latency
) {
    DecodePipeline pipeline;
    MarketStateStore store;
    MarketStateView view(store);
    WorkflowSummary summary;

    std::string asset_id;
    std::string market_id;
    std::uint64_t last_recv_ns = 0;

    for (const auto& packet : packets) {
        const auto total_start_ns = now_ns();
        const auto input = to_decode_input_view(packet);

        NormalizedEventBatch batch;
        const auto decode_start_ns = now_ns();
        const auto decoded = pipeline.decode(input, &batch);
        const auto decode_end_ns = now_ns();

        ++summary.ws.packets_read;
        if (!decoded.ok() &&
            decoded.payload_kind != JsonDecodeKind::NonJsonControl) {
            ++summary.ws.decode_errors;
        }

        count_normalized_events(
            batch,
            &summary,
            &asset_id,
            &market_id,
            &last_recv_ns
        );

        const auto state_events = from_normalized_batch(batch);
        std::uint64_t state_apply_ns = 0;
        for (const auto& event : state_events) {
            const auto apply_start_ns = now_ns();
            const auto result = store.apply(event);
            const auto apply_end_ns = now_ns();
            state_apply_ns += checked_sub(apply_end_ns, apply_start_ns);
            count_state_event(event.type, result, &summary);
        }

        std::uint64_t snapshot_read_ns = 0;
        if (!asset_id.empty()) {
            const auto read_start_ns = now_ns();
            const auto snapshot = view.get_snapshot(asset_id);
            const auto read_end_ns = now_ns();
            (void)snapshot;
            snapshot_read_ns = checked_sub(read_end_ns, read_start_ns);
        }

        record_latency(
            latency,
            checked_sub(now_ns(), total_start_ns),
            checked_sub(decode_end_ns, decode_start_ns),
            state_apply_ns,
            snapshot_read_ns
        );
    }

    if (asset_id.empty()) {
        fail("WS replay did not produce an asset id");
    }

    auto before_chain_snapshot = view.get_snapshot(asset_id);
    if (!before_chain_snapshot.ok) {
        fail("snapshot missing after WS replay");
    }

    const BookFingerprint book_before_chain =
        fingerprint(before_chain_snapshot.value);
    const std::uint64_t legacy_book_hash_before_chain = store.global_hash();

    std::map<std::string, AppliedFill> active_fills;
    std::int64_t expected_buy_lots = 0;
    std::int64_t expected_sell_lots = 0;

    for (const auto& chain_template : chain_templates) {
        ClassifiedFillRecord fill = materialize_fill(
            chain_template,
            asset_id,
            market_id,
            last_recv_ns
        );

        count_chain_fill(fill, &summary);

        const auto total_start_ns = now_ns();
        const auto event = from_classified_fill(fill);

        const auto apply_start_ns = now_ns();
        const auto result = store.apply(event);
        const auto apply_end_ns = now_ns();
        count_state_event(event.type, result, &summary);

        const auto read_start_ns = now_ns();
        const auto after_fill_snapshot = view.get_snapshot(asset_id);
        const auto read_end_ns = now_ns();
        if (!after_fill_snapshot.ok) {
            fail("snapshot missing after chain fill");
        }

        if (store.global_hash() != legacy_book_hash_before_chain) {
            fail("chain fill changed legacy book hash");
        }
        if (!same_book(book_before_chain, after_fill_snapshot.value)) {
            fail("chain fill changed book depth or BBO");
        }

        update_expected_flow(
            fill,
            &active_fills,
            &expected_buy_lots,
            &expected_sell_lots
        );

        record_latency(
            latency,
            checked_sub(now_ns(), total_start_ns),
            0,
            checked_sub(apply_end_ns, apply_start_ns),
            checked_sub(read_end_ns, read_start_ns)
        );
    }

    const auto final_snapshot = view.get_snapshot(asset_id);
    if (!final_snapshot.ok) {
        fail("final snapshot missing");
    }

    if (final_snapshot.value.confirmed_buy_lots_10s != expected_buy_lots ||
        final_snapshot.value.confirmed_sell_lots_10s != expected_sell_lots) {
        fail("unknown, ambiguous, or removed fill polluted confirmed flow");
    }

    summary.state.entity_count = store.exists(asset_id) ? 1ULL : 0ULL;
    summary.snapshot.snapshot_ok = final_snapshot.ok;
    summary.snapshot.asset_id = final_snapshot.value.entity_id;
    summary.snapshot.market_id = final_snapshot.value.market_id;
    summary.snapshot.has_book =
        final_snapshot.value.bid_count + final_snapshot.value.ask_count > 0U;
    summary.snapshot.has_chain_state =
        final_snapshot.value.has_confirmed_trade ||
        final_snapshot.value.confirmed_buy_lots_10s != 0 ||
        final_snapshot.value.confirmed_sell_lots_10s != 0;
    summary.snapshot.has_quality_state = final_snapshot.ok;
    summary.snapshot.usable_for_depth = final_snapshot.value.usable_for_depth;
    summary.snapshot.usable_for_signal = final_snapshot.value.usable_for_signal;

    summary.hashes.legacy_book_hash = store.global_hash();
    summary.hashes.chain_hash = chain_hash_from_snapshot(final_snapshot.value);
    summary.hashes.combined_state_hash =
        summary.hashes.legacy_book_hash ^ summary.hashes.chain_hash;

    return summary;
}

void validate_summary(const WorkflowSummary& summary) {
    if (summary.ws.decode_errors > 0 ||
        summary.ws.normalization_errors > 0 ||
        summary.state.state_errors > 0) {
        fail("workflow produced errors");
    }
    if (summary.ws.packets_read != 39 ||
        summary.ws.normalized_events != 39 ||
        summary.ws.snapshot_events != 1 ||
        summary.ws.delta_events != 35 ||
        summary.ws.heartbeat_events != 3) {
        fail("WS path counters do not match market_39 baseline");
    }
    if (summary.state.book_snapshots_applied != 1 ||
        summary.state.book_deltas_applied != 35 ||
        summary.state.entity_count != 1) {
        fail("state path counters do not match baseline");
    }
    if (summary.hashes.legacy_book_hash != kMarket39LegacyBookHash) {
        fail("legacy book hash does not match baseline");
    }
    if (!summary.snapshot.snapshot_ok ||
        !summary.snapshot.has_book ||
        !summary.snapshot.has_quality_state ||
        !summary.snapshot.usable_for_depth ||
        summary.snapshot.asset_id.empty() ||
        summary.snapshot.market_id.empty()) {
        fail("snapshot output is incomplete");
    }
    if (summary.chain.classified_fills == 0 ||
        summary.chain.buy_aggressor_fills != 1 ||
        summary.chain.sell_aggressor_fills != 1 ||
        (summary.chain.unknown_fills + summary.chain.ambiguous_fills) < 1 ||
        summary.chain.removed_fills < 1 ||
        summary.state.chain_fills_applied == 0 ||
        !summary.snapshot.has_chain_state) {
        fail("chain path counters do not meet fixture expectations");
    }
    if (summary.hashes.chain_hash == 0 ||
        summary.hashes.combined_state_hash == 0) {
        fail("combined hashes were not produced");
    }
}

void print_latency_block(
    const std::string& name,
    const LatencyStats& stats,
    bool include_max
) {
    std::cout << "  " << name << ":\n";
    std::cout << "    p50: " << stats.p50 << '\n';
    std::cout << "    p95: " << stats.p95 << '\n';
    std::cout << "    p99: " << stats.p99 << '\n';
    if (include_max) {
        std::cout << "    max: " << stats.max << '\n';
    }
}

void print_bool(const std::string& name, bool value) {
    std::cout << "  " << name << ": " << (value ? "true" : "false") << '\n';
}

void print_report(
    const WorkflowSummary& summary,
    const LatencySamples& latency
) {
    std::cout << "ws_path:\n";
    std::cout << "  packets_read: " << summary.ws.packets_read << '\n';
    std::cout << "  normalized_events: " << summary.ws.normalized_events << '\n';
    std::cout << "  snapshot_events: " << summary.ws.snapshot_events << '\n';
    std::cout << "  delta_events: " << summary.ws.delta_events << '\n';
    std::cout << "  heartbeat_events: " << summary.ws.heartbeat_events << '\n';
    std::cout << "  decode_errors: " << summary.ws.decode_errors << '\n';
    std::cout << "  normalization_errors: "
              << summary.ws.normalization_errors << '\n';
    std::cout << '\n';

    std::cout << "chain_path:\n";
    std::cout << "  chain_logs: " << summary.chain.chain_logs << '\n';
    std::cout << "  classified_fills: "
              << summary.chain.classified_fills << '\n';
    std::cout << "  buy_aggressor_fills: "
              << summary.chain.buy_aggressor_fills << '\n';
    std::cout << "  sell_aggressor_fills: "
              << summary.chain.sell_aggressor_fills << '\n';
    std::cout << "  unknown_fills: " << summary.chain.unknown_fills << '\n';
    std::cout << "  ambiguous_fills: "
              << summary.chain.ambiguous_fills << '\n';
    std::cout << "  removed_fills: " << summary.chain.removed_fills << '\n';
    std::cout << '\n';

    std::cout << "state_path:\n";
    std::cout << "  book_snapshots_applied: "
              << summary.state.book_snapshots_applied << '\n';
    std::cout << "  book_deltas_applied: "
              << summary.state.book_deltas_applied << '\n';
    std::cout << "  chain_fills_applied: "
              << summary.state.chain_fills_applied << '\n';
    std::cout << "  state_errors: " << summary.state.state_errors << '\n';
    std::cout << "  entity_count: " << summary.state.entity_count << '\n';
    std::cout << '\n';

    std::cout << "snapshot_output:\n";
    print_bool("snapshot_ok", summary.snapshot.snapshot_ok);
    print_bool("has_book", summary.snapshot.has_book);
    print_bool("has_chain_state", summary.snapshot.has_chain_state);
    print_bool("has_quality_state", summary.snapshot.has_quality_state);
    print_bool("usable_for_depth", summary.snapshot.usable_for_depth);
    print_bool("usable_for_signal", summary.snapshot.usable_for_signal);
    std::cout << '\n';

    std::cout << "hashes:\n";
    std::cout << "  legacy_book_hash: "
              << summary.hashes.legacy_book_hash << '\n';
    std::cout << "  chain_hash: " << summary.hashes.chain_hash << '\n';
    std::cout << "  combined_state_hash: "
              << summary.hashes.combined_state_hash << '\n';
    print_bool("determinism_passed", summary.hashes.determinism_passed);
    std::cout << '\n';

    std::cout << "latency:\n";
    print_latency_block(
        "total_workflow_ns",
        summarize_latency(latency.total_workflow_ns),
        true
    );
    print_latency_block("decode_ns", summarize_latency(latency.decode_ns), false);
    print_latency_block(
        "state_apply_ns",
        summarize_latency(latency.state_apply_ns),
        false
    );
    print_latency_block(
        "snapshot_read_ns",
        summarize_latency(latency.snapshot_read_ns),
        false
    );
}

Config parse_args(int argc, char** argv) {
    Config config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--raw") {
            if (++i >= argc) {
                fail("--raw requires a value");
            }
            config.raw_path = argv[i];
        } else if (arg == "--chain-fixture") {
            if (++i >= argc) {
                fail("--chain-fixture requires a value");
            }
            config.chain_fixture = argv[i];
        } else if (arg == "--repeat") {
            if (++i >= argc) {
                fail("--repeat requires a value");
            }
            config.repeat = std::stoull(argv[i]);
        } else if (arg == "--check-determinism") {
            config.check_determinism = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "usage: verify_market_state_workflow "
                << "--raw path --chain-fixture path "
                << "[--repeat N] [--check-determinism]\n";
            std::exit(0);
        } else {
            fail("unknown argument: " + arg);
        }
    }

    if (config.repeat == 0) {
        fail("--repeat must be greater than zero");
    }
    return config;
}

int run(int argc, char** argv) {
    const Config config = parse_args(argc, argv);
    const std::vector<RawPacket> packets = load_packets(config.raw_path);
    const std::vector<TemplateFill> chain_templates =
        load_chain_fixture(config.chain_fixture);

    if (packets.empty()) {
        fail("raw fixture is empty");
    }

    LatencySamples latency;
    WorkflowSummary baseline;

    for (std::uint64_t i = 0; i < config.repeat; ++i) {
        WorkflowSummary current = run_once(packets, chain_templates, &latency);
        validate_summary(current);

        if (i == 0) {
            baseline = current;
        } else if (
            config.check_determinism &&
            !same_summary_except_determinism(baseline, current)) {
            baseline.hashes.determinism_passed = false;
        }
    }

    if (config.check_determinism && !baseline.hashes.determinism_passed) {
        fail("workflow determinism check failed");
    }

    print_report(baseline, latency);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "verify_market_state_workflow failed: "
                  << error.what() << '\n';
        return 1;
    }
}
