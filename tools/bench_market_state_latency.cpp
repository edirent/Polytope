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
#include <vector>

namespace {

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
constexpr std::size_t kGroupCount = 6;

enum class SampleGroup : std::uint8_t {
    Snapshot = 0,
    Delta,
    Heartbeat,
    ChainConfirmedFill,
    ChainRemovedFill,
    Unknown
};

struct Config {
    std::string raw_path{"tests/fixtures/polymarket/market_39.raw"};
    std::string mode{"memory"};
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
    std::vector<std::uint64_t> adapter_ns;
    std::vector<std::uint64_t> decode_pipeline_ns;
    std::vector<std::uint64_t> state_event_adapter_ns;
    std::vector<std::uint64_t> market_state_apply_ns;
    std::vector<std::uint64_t> snapshot_publish_ns;
    std::vector<std::uint64_t> view_snapshot_read_ns;
    std::vector<std::uint64_t> total_ns;

    std::array<std::vector<std::uint64_t>, kGroupCount> by_group_total_ns;
};

struct Counters {
    std::uint64_t packets_decoded{0};
    std::uint64_t normalized_events{0};
    std::uint64_t book_snapshots_applied{0};
    std::uint64_t book_deltas_applied{0};
    std::uint64_t heartbeats_ignored{0};
    std::uint64_t chain_fills_applied{0};
    std::uint64_t chain_removed_fills_applied{0};
    std::uint64_t state_errors{0};
    std::uint64_t decode_errors{0};
};

struct Hashes {
    std::uint64_t legacy_book_hash{0};
    std::uint64_t chain_hash{0};
    std::uint64_t combined_state_hash{0};
};

struct BenchState {
    Samples samples;
    Counters counters;
    Hashes hashes;
};

struct IterationContext {
    std::string asset_id;
    std::string market_id;
    std::uint64_t last_recv_monotonic_ns{0};
};

struct StageDurations {
    std::uint64_t adapter_ns{0};
    std::uint64_t decode_pipeline_ns{0};
    std::uint64_t state_event_adapter_ns{0};
    std::uint64_t market_state_apply_ns{0};
    std::uint64_t snapshot_publish_ns{0};
    std::uint64_t view_snapshot_read_ns{0};
    std::uint64_t total_ns{0};
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

std::size_t group_index(SampleGroup group) noexcept {
    return static_cast<std::size_t>(group);
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
    hash_u64(
        hash,
        static_cast<std::uint64_t>(snapshot.last_trade_price_tick)
    );
    hash_u64(
        hash,
        static_cast<std::uint64_t>(snapshot.last_trade_size_lots)
    );
    hash_u64(
        hash,
        static_cast<std::uint64_t>(snapshot.last_taker_side)
    );
    hash_u64(
        hash,
        static_cast<std::uint64_t>(snapshot.confirmed_buy_lots_2s)
    );
    hash_u64(
        hash,
        static_cast<std::uint64_t>(snapshot.confirmed_sell_lots_2s)
    );
    hash_u64(
        hash,
        static_cast<std::uint64_t>(snapshot.confirmed_buy_lots_10s)
    );
    hash_u64(
        hash,
        static_cast<std::uint64_t>(snapshot.confirmed_sell_lots_10s)
    );
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

SampleGroup group_for_batch(
    const NormalizedEventBatch& batch,
    const trading_engine::decode::DecodePipelineResult& result
) noexcept {
    for (const auto& event : batch.events) {
        if (event.event_type == NormalizedEventType::Snapshot) {
            return SampleGroup::Snapshot;
        }
    }

    for (const auto& event : batch.events) {
        if (event.event_type == NormalizedEventType::Delta) {
            return SampleGroup::Delta;
        }
    }

    for (const auto& event : batch.events) {
        if (event.event_type == NormalizedEventType::Heartbeat) {
            return SampleGroup::Heartbeat;
        }
    }

    if (!result.ok()) {
        return SampleGroup::Unknown;
    }

    return SampleGroup::Unknown;
}

void update_context(
    const NormalizedEventBatch& batch,
    IterationContext* context
) {
    for (const auto& event : batch.events) {
        if (event.event_type != NormalizedEventType::Heartbeat) {
            if (context->asset_id.empty() && !event.entity_id.empty()) {
                context->asset_id = event.entity_id;
            }
            if (context->market_id.empty() && !event.market_id.empty()) {
                context->market_id = event.market_id;
            }
        }

        if (event.recv_monotonic_ns > context->last_recv_monotonic_ns) {
            context->last_recv_monotonic_ns = event.recv_monotonic_ns;
        }
    }
}

void record_sample(
    BenchState* state,
    SampleGroup group,
    const StageDurations& durations
) {
    state->samples.adapter_ns.push_back(durations.adapter_ns);
    state->samples.decode_pipeline_ns.push_back(
        durations.decode_pipeline_ns
    );
    state->samples.state_event_adapter_ns.push_back(
        durations.state_event_adapter_ns
    );
    state->samples.market_state_apply_ns.push_back(
        durations.market_state_apply_ns
    );
    state->samples.snapshot_publish_ns.push_back(
        durations.snapshot_publish_ns
    );
    state->samples.view_snapshot_read_ns.push_back(
        durations.view_snapshot_read_ns
    );
    state->samples.total_ns.push_back(durations.total_ns);
    state->samples.by_group_total_ns[group_index(group)].push_back(
        durations.total_ns
    );
}

void count_state_result(
    MarketStateEventType event_type,
    const trading_engine::state::StateApplyResult& result,
    Counters* counters
) {
    if (!result.ok()) {
        ++counters->state_errors;
        return;
    }

    switch (event_type) {
        case MarketStateEventType::WsBookSnapshot:
            if (result.code == StateApplyCode::Applied) {
                ++counters->book_snapshots_applied;
            }
            break;

        case MarketStateEventType::WsBookDelta:
            if (result.code == StateApplyCode::Applied) {
                ++counters->book_deltas_applied;
            }
            break;

        case MarketStateEventType::WsHeartbeat:
            if (result.code == StateApplyCode::IgnoredHeartbeat) {
                ++counters->heartbeats_ignored;
            }
            break;

        case MarketStateEventType::ChainConfirmedFill:
            if (result.state_changed) {
                ++counters->chain_fills_applied;
            }
            break;

        case MarketStateEventType::ChainRemovedFill:
            if (result.state_changed) {
                ++counters->chain_removed_fills_applied;
            }
            break;

        case MarketStateEventType::WsLifecycle:
        case MarketStateEventType::ChainSettlement:
        case MarketStateEventType::DataQualityUpdate:
        default:
            break;
    }
}

void run_raw_packet_sample(
    const DecodePipeline& pipeline,
    const RawPacket& packet,
    MarketStateStore* store,
    MarketStateView* view,
    IterationContext* context,
    BenchState* state,
    bool record
) {
    const auto total_start_ns = now_ns();

    const auto adapter_start_ns = total_start_ns;
    const auto input = to_decode_input_view(packet);
    const auto adapter_end_ns = now_ns();

    NormalizedEventBatch batch;
    const auto decode_start_ns = adapter_end_ns;
    const auto decoded = pipeline.decode(input, &batch);
    const auto decode_end_ns = now_ns();

    const auto state_adapter_start_ns = decode_end_ns;
    const auto state_events = from_normalized_batch(batch);
    const auto state_adapter_end_ns = now_ns();

    update_context(batch, context);

    std::uint64_t apply_ex_publish_ns = 0;
    std::uint64_t publish_ns = 0;

    for (const auto& event : state_events) {
        const auto apply_start_ns = now_ns();
        const auto result = store->apply(event);
        const auto apply_end_ns = now_ns();
        const auto apply_total_ns = checked_sub(apply_end_ns, apply_start_ns);
        publish_ns += result.snapshot_publish_ns;
        apply_ex_publish_ns += apply_total_ns >= result.snapshot_publish_ns
            ? apply_total_ns - result.snapshot_publish_ns
            : 0;

        if (record) {
            count_state_result(event.type, result, &state->counters);
        }
    }

    std::uint64_t view_read_ns = 0;
    if (!context->asset_id.empty()) {
        const auto view_start_ns = now_ns();
        const auto snapshot = view->get_snapshot(context->asset_id);
        const auto view_end_ns = now_ns();
        view_read_ns = checked_sub(view_end_ns, view_start_ns);
        (void)snapshot;
    }

    const auto total_end_ns = now_ns();

    if (!record) {
        return;
    }

    ++state->counters.packets_decoded;
    state->counters.normalized_events +=
        static_cast<std::uint64_t>(batch.size());

    if (!decoded.ok() &&
        decoded.payload_kind != JsonDecodeKind::NonJsonControl) {
        ++state->counters.decode_errors;
    }

    record_sample(
        state,
        group_for_batch(batch, decoded),
        StageDurations{
            checked_sub(adapter_end_ns, adapter_start_ns),
            checked_sub(decode_end_ns, decode_start_ns),
            checked_sub(state_adapter_end_ns, state_adapter_start_ns),
            apply_ex_publish_ns,
            publish_ns,
            view_read_ns,
            checked_sub(total_end_ns, total_start_ns)
        }
    );
}

ClassifiedFillRecord synthetic_fill(
    const IterationContext& context,
    const MarketStateSnapshot& snapshot,
    bool removed
) {
    ClassifiedFillRecord fill;
    fill.fill_id = "0xbench-market-state:1";
    fill.order_hash = "0xbench-order";
    fill.market_id = context.market_id.empty()
        ? "bench-market"
        : context.market_id;
    fill.asset_id = context.asset_id;
    fill.price_tick = snapshot.has_bid ? snapshot.best_bid_tick : 500000;
    fill.size_lots = 1234;
    fill.direction = ConfirmedDirection::BuyAggressor;
    fill.mapping_status = FillMappingStatus::Mapped;
    fill.classification = removed
        ? FillClassification::ChainRemoved
        : FillClassification::ChainConfirmed;
    fill.block_number = 123456789;
    fill.tx_hash = "0xbench-market-state";
    fill.log_index = 1;
    fill.chain_seen_monotonic_ns =
        context.last_recv_monotonic_ns + (removed ? 2'000'000ULL : 1'000'000ULL);
    fill.source_sequence = removed ? 123456790ULL : 123456789ULL;
    fill.removed = removed;
    return fill;
}

void run_chain_sample(
    const ClassifiedFillRecord& fill,
    SampleGroup group,
    MarketStateStore* store,
    MarketStateView* view,
    BenchState* state,
    bool record
) {
    const auto total_start_ns = now_ns();

    const auto state_adapter_start_ns = total_start_ns;
    const auto event = from_classified_fill(fill);
    const auto state_adapter_end_ns = now_ns();

    const auto apply_start_ns = state_adapter_end_ns;
    const auto result = store->apply(event);
    const auto apply_end_ns = now_ns();

    const auto apply_total_ns = checked_sub(apply_end_ns, apply_start_ns);
    const auto apply_ex_publish_ns =
        apply_total_ns >= result.snapshot_publish_ns
            ? apply_total_ns - result.snapshot_publish_ns
            : 0;

    const auto view_start_ns = now_ns();
    const auto snapshot = view->get_snapshot(fill.asset_id);
    const auto view_end_ns = now_ns();
    (void)snapshot;

    const auto total_end_ns = now_ns();

    if (!record) {
        return;
    }

    count_state_result(event.type, result, &state->counters);
    record_sample(
        state,
        group,
        StageDurations{
            0,
            0,
            checked_sub(state_adapter_end_ns, state_adapter_start_ns),
            apply_ex_publish_ns,
            result.snapshot_publish_ns,
            checked_sub(view_end_ns, view_start_ns),
            checked_sub(total_end_ns, total_start_ns)
        }
    );
}

void update_hashes(
    const MarketStateStore& store,
    const MarketStateView& view,
    const std::string& asset_id,
    BenchState* state
) {
    if (asset_id.empty()) {
        fail("no asset id captured from replay");
    }

    const auto snapshot = view.get_snapshot(asset_id);
    if (!snapshot.ok) {
        fail("final snapshot missing for asset: " + asset_id);
    }

    state->hashes.legacy_book_hash = store.global_hash();
    state->hashes.chain_hash = chain_hash_from_snapshot(snapshot.value);
    state->hashes.combined_state_hash =
        state->hashes.legacy_book_hash ^ state->hashes.chain_hash;
}

void reserve_samples(
    BenchState* state,
    std::uint64_t packet_count,
    std::uint64_t repeat
) {
    const auto sample_count = static_cast<std::size_t>(
        (packet_count + 2) * repeat
    );

    state->samples.adapter_ns.reserve(sample_count);
    state->samples.decode_pipeline_ns.reserve(sample_count);
    state->samples.state_event_adapter_ns.reserve(sample_count);
    state->samples.market_state_apply_ns.reserve(sample_count);
    state->samples.snapshot_publish_ns.reserve(sample_count);
    state->samples.view_snapshot_read_ns.reserve(sample_count);
    state->samples.total_ns.reserve(sample_count);
}

void run_one_iteration(
    const DecodePipeline& pipeline,
    const std::vector<RawPacket>& packets,
    BenchState* state,
    bool record
) {
    MarketStateStore store;
    MarketStateView view(store);
    IterationContext context;

    for (const auto& packet : packets) {
        run_raw_packet_sample(
            pipeline,
            packet,
            &store,
            &view,
            &context,
            state,
            record
        );
    }

    if (context.asset_id.empty()) {
        fail("market replay did not produce an asset id");
    }

    if (store.global_hash() != kMarket39LegacyBookHash) {
        fail("legacy_book_hash changed before chain fill");
    }

    const auto before_chain = view.get_snapshot(context.asset_id);
    if (!before_chain.ok) {
        fail("snapshot missing before synthetic chain fill");
    }

    const auto confirmed = synthetic_fill(
        context,
        before_chain.value,
        false
    );
    run_chain_sample(
        confirmed,
        SampleGroup::ChainConfirmedFill,
        &store,
        &view,
        state,
        record
    );

    const auto removed = synthetic_fill(context, before_chain.value, true);
    run_chain_sample(
        removed,
        SampleGroup::ChainRemovedFill,
        &store,
        &view,
        state,
        record
    );

    if (store.global_hash() != kMarket39LegacyBookHash) {
        fail("legacy_book_hash changed after chain fill");
    }

    if (record) {
        update_hashes(store, view, context.asset_id, state);
    }
}

void run_memory_mode(
    const Config& config,
    const std::vector<RawPacket>& packets,
    BenchState* state
) {
    if (packets.empty()) {
        fail("raw log contains no packets");
    }

    DecodePipeline pipeline;

    for (std::uint64_t i = 0; i < config.warmup; ++i) {
        run_one_iteration(pipeline, packets, state, false);
    }

    reserve_samples(
        state,
        static_cast<std::uint64_t>(packets.size()),
        config.repeat
    );

    for (std::uint64_t i = 0; i < config.repeat; ++i) {
        run_one_iteration(pipeline, packets, state, true);
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

void print_group_latencies(const Samples& samples) {
    std::cout << "by_market_state_event_total_ns:\n";
    print_latency_nested(
        "snapshot",
        summarize(samples.by_group_total_ns[group_index(SampleGroup::Snapshot)])
    );
    print_latency_nested(
        "delta",
        summarize(samples.by_group_total_ns[group_index(SampleGroup::Delta)])
    );
    print_latency_nested(
        "heartbeat",
        summarize(samples.by_group_total_ns[group_index(SampleGroup::Heartbeat)])
    );
    print_latency_nested(
        "chain_confirmed_fill",
        summarize(
            samples.by_group_total_ns[
                group_index(SampleGroup::ChainConfirmedFill)
            ]
        )
    );
    print_latency_nested(
        "chain_removed_fill",
        summarize(
            samples.by_group_total_ns[
                group_index(SampleGroup::ChainRemovedFill)
            ]
        )
    );
    print_latency_nested(
        "unknown",
        summarize(samples.by_group_total_ns[group_index(SampleGroup::Unknown)])
    );
}

void print_report(
    const Config& config,
    const BenchState& state,
    std::uint64_t packets_loaded
) {
    std::cout << "market_state_latency\n";
    std::cout << "mode: " << config.mode << '\n';
    std::cout << "packets_loaded: " << packets_loaded << '\n';
    std::cout << "iterations: " << config.repeat << '\n';
    std::cout << "warmup: " << config.warmup << '\n';
    std::cout << "samples: " << state.samples.total_ns.size() << '\n';
    std::cout << '\n';

    print_latency("adapter_ns", summarize(state.samples.adapter_ns));
    std::cout << '\n';
    print_latency(
        "decode_pipeline_ns",
        summarize(state.samples.decode_pipeline_ns)
    );
    std::cout << '\n';
    print_latency(
        "state_event_adapter_ns",
        summarize(state.samples.state_event_adapter_ns)
    );
    std::cout << '\n';
    print_latency(
        "market_state_apply_ns",
        summarize(state.samples.market_state_apply_ns)
    );
    std::cout << '\n';
    print_latency(
        "snapshot_publish_ns",
        summarize(state.samples.snapshot_publish_ns)
    );
    std::cout << '\n';
    print_latency(
        "view_snapshot_read_ns",
        summarize(state.samples.view_snapshot_read_ns)
    );
    std::cout << '\n';
    print_latency("total_ns", summarize(state.samples.total_ns));
    std::cout << '\n';

    print_group_latencies(state.samples);
    std::cout << '\n';

    std::cout << "counters:\n";
    std::cout << "  packets_decoded: "
              << state.counters.packets_decoded << '\n';
    std::cout << "  normalized_events: "
              << state.counters.normalized_events << '\n';
    std::cout << "  book_snapshots_applied: "
              << state.counters.book_snapshots_applied << '\n';
    std::cout << "  book_deltas_applied: "
              << state.counters.book_deltas_applied << '\n';
    std::cout << "  heartbeats_ignored: "
              << state.counters.heartbeats_ignored << '\n';
    std::cout << "  chain_fills_applied: "
              << state.counters.chain_fills_applied << '\n';
    std::cout << "  chain_removed_fills_applied: "
              << state.counters.chain_removed_fills_applied << '\n';
    std::cout << "  decode_errors: "
              << state.counters.decode_errors << '\n';
    std::cout << "  state_errors: "
              << state.counters.state_errors << '\n';
    std::cout << "  legacy_book_hash: "
              << state.hashes.legacy_book_hash << '\n';
    std::cout << "  chain_hash: " << state.hashes.chain_hash << '\n';
    std::cout << "  combined_state_hash: "
              << state.hashes.combined_state_hash << '\n';
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

        if (arg == "--help" || arg == "-h") {
            std::cout
                << "usage: bench_market_state_latency [raw_path] "
                << "[--repeat N] [--warmup N] [--mode memory]\n";
            std::exit(0);
        }
    }

    if (config.mode != "memory") {
        fail("unsupported mode: " + config.mode);
    }

    if (config.repeat == 0) {
        fail("--repeat must be greater than zero");
    }

    return config;
}

int run(int argc, char** argv) {
    const Config config = parse_args(argc, argv);
    const std::vector<RawPacket> packets = load_packets(config.raw_path);

    BenchState state;
    run_memory_mode(config, packets, &state);

    if (state.hashes.legacy_book_hash != kMarket39LegacyBookHash) {
        fail("legacy_book_hash did not match market_39 baseline");
    }

    print_report(
        config,
        state,
        static_cast<std::uint64_t>(packets.size())
    );
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "bench_market_state_latency failed: "
                  << error.what() << '\n';
        return 1;
    }
}
