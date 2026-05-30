#pragma once

#include "engine/signal/core/SignalEngine.h"
#include "engine/signal/edge/LatencyBufferModel.h"
#include "engine/signal/edge/TheoreticalEdgeCalculator.h"
#include "engine/signal/pricing/FeeModel.h"
#include "engine/signal/pricing/VWAPPrecheck.h"
#include "engine/signal/publish/CapturingIntentPublisher.h"
#include "engine/signal/publish/IntentDeduper.h"
#include "engine/signal/publish/IntentRateLimiter.h"
#include "engine/signal/rank/OpportunityRanker.h"
#include "engine/signal/reader/MarketSnapshotReader.h"
#include "engine/signal/reader/OracleArtifactReader.h"
#include "oracle/artifact/ArtifactExporter.h"

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace signal_workflow_test {

using trading_engine::oracle::ArtifactExporter;
using trading_engine::oracle::BundleLeg;
using trading_engine::oracle::OracleArtifactContents;
using trading_engine::oracle::Side;
using trading_engine::signal::CandidateBundle;
using trading_engine::signal::CapturingIntentPublisher;
using trading_engine::signal::FeeModel;
using trading_engine::signal::IMarketSnapshotReader;
using trading_engine::signal::IntentStatus;
using trading_engine::signal::IntentDeduper;
using trading_engine::signal::IntentRateLimiter;
using trading_engine::signal::LatencyBufferModel;
using trading_engine::signal::MarketStateSnapshot;
using trading_engine::signal::OracleArtifactReader;
using trading_engine::signal::SettlementMaskChecker;
using trading_engine::signal::SignalConfig;
using trading_engine::signal::SignalEngine;
using trading_engine::signal::SignalScanContext;
using trading_engine::signal::SnapshotReadResult;
using trading_engine::signal::TheoreticalEdgeCalculator;
using trading_engine::signal::VWAPPrecheck;
using trading_engine::signal::validate_bundle_snapshots;
using trading_engine::state::BookQuality;
using trading_engine::state::PriceLevel;

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

inline void append_u8(std::vector<std::byte>* out, std::uint8_t value) {
    out->push_back(static_cast<std::byte>(value));
}

inline void append_u32(std::vector<std::byte>* out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        append_u8(out, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

inline void append_u64(std::vector<std::byte>* out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        append_u8(out, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

inline void append_i64(std::vector<std::byte>* out, std::int64_t value) {
    append_u64(out, static_cast<std::uint64_t>(value));
}

inline void append_string(
    std::vector<std::byte>* out,
    const std::string& value
) {
    append_u32(out, static_cast<std::uint32_t>(value.size()));
    for (const unsigned char byte : value) {
        append_u8(out, byte);
    }
}

inline std::vector<std::byte> serialize_bundles(
    const std::vector<CandidateBundle>& bundles
) {
    std::vector<std::byte> out;
    append_u32(&out, static_cast<std::uint32_t>(bundles.size()));
    for (const auto& candidate : bundles) {
        append_u64(&out, candidate.bundle_id);
        append_u64(&out, candidate.required_true_mask);
        append_u64(&out, candidate.required_false_mask);
        append_u64(&out, candidate.invalid_mask);
        append_i64(&out, candidate.guaranteed_payout_tick);
        append_u32(&out, candidate.leg_count);
        append_i64(&out, candidate.min_edge_tick);
        for (std::uint16_t i = 0; i < candidate.leg_count; ++i) {
            const auto& leg = candidate.legs[i];
            append_string(&out, leg.market_id);
            append_string(&out, leg.asset_id);
            append_u8(&out, static_cast<std::uint8_t>(leg.side));
            append_i64(&out, leg.quantity_lots);
            append_i64(&out, leg.max_price_tick);
        }
    }
    return out;
}

inline CandidateBundle two_leg_bundle(
    std::int64_t quantity_lots = 1,
    std::int64_t guaranteed_payout_tick = 1'000'000,
    std::int64_t min_edge_tick = 0
) {
    CandidateBundle bundle;
    bundle.bundle_id = 100;
    bundle.required_true_mask = 0;
    bundle.guaranteed_payout_tick = guaranteed_payout_tick;
    bundle.min_edge_tick = min_edge_tick;
    bundle.leg_count = 2;
    bundle.legs[0] = BundleLeg{
        .market_id = "m1",
        .asset_id = "asset_yes",
        .side = Side::Buy,
        .quantity_lots = quantity_lots,
        .max_price_tick = 1'000'000
    };
    bundle.legs[1] = BundleLeg{
        .market_id = "m1",
        .asset_id = "asset_no",
        .side = Side::Buy,
        .quantity_lots = quantity_lots,
        .max_price_tick = 1'000'000
    };
    return bundle;
}

inline std::filesystem::path export_artifact(
    const std::string& name,
    const std::vector<CandidateBundle>& bundles
) {
    OracleArtifactContents contents;
    contents.manifest.artifact_version = 1;
    contents.manifest.created_at_ns = 1;
    contents.manifest.market_count = 1;
    contents.manifest.asset_count = 2;
    contents.manifest.variable_count = 2;
    contents.manifest.rule_count = 1;
    contents.manifest.constraint_count = 1;
    contents.manifest.feasible_state_count = 2;
    contents.manifest.bundle_count =
        static_cast<std::uint64_t>(bundles.size());
    contents.manifest.llm_provider = "none";
    contents.market_universe_json = "{\"markets\":[\"m1\"]}\n";
    contents.rulebook_json = "{\"rules\":[\"r1\"]}\n";
    contents.variables_bin = {std::byte{1}};
    contents.constraints_bin = {std::byte{2}};
    contents.feasible_states_bin = {std::byte{3}};
    contents.payoff_matrix_bin = {std::byte{4}};
    contents.candidate_bundles_bin = serialize_bundles(bundles);

    const auto suffix = std::chrono::steady_clock::now()
        .time_since_epoch()
        .count();
    const auto root =
        std::filesystem::temp_directory_path() /
        ("signal_workflow_" + name + "_" + std::to_string(suffix));
    std::filesystem::remove_all(root);

    ArtifactExporter exporter;
    const auto result = exporter.export_artifact(root, "artifact", contents);
    expect_true(result.ok(), "export artifact");
    return result.artifact_dir;
}

inline MarketStateSnapshot snapshot(
    const std::string& asset_id,
    std::int64_t ask_tick,
    double ask_size,
    bool recovering = false,
    bool crossed = false
) {
    MarketStateSnapshot out;
    out.entity_id = asset_id;
    out.market_id = "m1";
    out.version = 1;
    out.live = true;
    out.recovering = recovering;
    out.crossed = crossed;
    out.has_ask = ask_size > 0.0;
    out.ask_count = ask_size > 0.0 ? 1U : 0U;
    out.best_ask_tick = ask_tick;
    out.asks[0] = PriceLevel{
        .price_tick = ask_tick,
        .price = static_cast<double>(ask_tick) / 1'000'000.0,
        .size = ask_size
    };
    out.state_hash = asset_id == "asset_yes" ? 101 : 102;
    out.quality = recovering
        ? BookQuality::Recovering
        : (crossed ? BookQuality::Crossed : BookQuality::Good);
    out.usable_for_depth = true;
    out.usable_for_signal = true;
    return out;
}

class StaticSnapshotReader final : public IMarketSnapshotReader {
public:
    explicit StaticSnapshotReader(std::vector<MarketStateSnapshot> snapshots)
        : snapshots_(std::move(snapshots)) {}

    SnapshotReadResult read_for_bundle(
        const CandidateBundle& bundle,
        const SignalConfig& config,
        std::uint64_t now_ns = 0
    ) const override {
        std::vector<MarketStateSnapshot> selected;
        selected.reserve(bundle.leg_count);
        for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
            const auto& asset_id = bundle.legs[i].asset_id;
            for (const auto& snapshot : snapshots_) {
                if (snapshot.entity_id == asset_id) {
                    selected.push_back(snapshot);
                    break;
                }
            }
        }
        return validate_bundle_snapshots(bundle, config, selected, now_ns);
    }

private:
    std::vector<MarketStateSnapshot> snapshots_;
};

struct EngineHarness {
    SignalConfig config;
    OracleArtifactReader artifact_reader;
    StaticSnapshotReader snapshot_reader;
    SettlementMaskChecker settlement_checker;
    VWAPPrecheck vwap;
    FeeModel fee_model;
    LatencyBufferModel latency_model;
    TheoreticalEdgeCalculator edge_calculator;
    trading_engine::signal::OpportunityRanker ranker;
    CapturingIntentPublisher publisher;

    EngineHarness(
        std::vector<CandidateBundle> bundles,
        std::vector<MarketStateSnapshot> snapshots,
        std::int64_t fee_tick = 0,
        std::int64_t latency_tick = 0
    ) : snapshot_reader(std::move(snapshots)),
        fee_model(fee_tick),
        latency_model(latency_tick),
        edge_calculator(fee_model, latency_model) {
        const auto artifact_dir = export_artifact("fixture", bundles);
        const auto load = artifact_reader.load(artifact_dir);
        expect_true(load.ok, "load artifact: " + load.error);
    }

    SignalEngine make_engine(
        IntentDeduper* deduper = nullptr,
        IntentRateLimiter* rate_limiter = nullptr
    ) {
        return SignalEngine(
            config,
            &snapshot_reader,
            &artifact_reader,
            &settlement_checker,
            &vwap,
            &edge_calculator,
            &ranker,
            &publisher,
            deduper,
            rate_limiter
        );
    }
};

inline SignalScanContext context() {
    SignalScanContext out;
    out.scan_id = 7;
    out.now_monotonic_ns = 1000;
    return out;
}

}  // namespace signal_workflow_test
