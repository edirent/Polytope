#pragma once

#include "engine/signal/core/SignalScratch.h"
#include "engine/signal/core/SignalWorkflow.h"
#include "engine/signal/edge/TheoreticalEdgeCalculator.h"
#include "engine/signal/pricing/VWAPPrecheck.h"
#include "engine/signal/public/SignalConfig.h"
#include "engine/signal/public/SignalEvidenceView.h"
#include "engine/signal/publish/IntentDeduper.h"
#include "engine/signal/publish/IntentRateLimiter.h"
#include "engine/signal/reader/SnapshotBatchReader.h"
#include "engine/signal/publish/IIntentPublisher.h"
#include "engine/signal/rank/OpportunityRanker.h"
#include "engine/signal/reader/MarketSnapshotReader.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace trading_engine::signal {

struct SignalPublishedEvidence {
    std::uint16_t snapshot_count = 0;
    std::array<trading_engine::state::MarketStateSnapshot, kMaxIntentLegs>
        snapshots{};

    std::uint16_t depth_view_count = 0;
    std::array<trading_engine::state::MarketDepthView, kMaxIntentLegs>
        depth_views{};

    std::uint64_t snapshot_version_hash = 0;
    std::uint64_t read_ts_ns = 0;

    [[nodiscard]] SignalEvidenceView view() const noexcept {
        SignalEvidenceView out;
        out.snapshots = snapshot_count == 0 ? nullptr : snapshots.data();
        out.snapshot_count = snapshot_count;
        out.depth_views = depth_view_count == 0 ? nullptr : depth_views.data();
        out.depth_view_count = depth_view_count;
        out.snapshot_version_hash = snapshot_version_hash;
        out.read_ts_ns = read_ts_ns;
        return out;
    }
};

class SignalEngine {
public:
    SignalEngine(
        SignalConfig config,
        const IMarketSnapshotReader* snapshot_reader,
        const OracleArtifactReader* artifact_reader,
        const SettlementMaskChecker* settlement_checker,
        const VWAPPrecheck* vwap,
        const TheoreticalEdgeCalculator* edge_calculator,
        const OpportunityRanker* ranker,
        IIntentPublisher* publisher,
        IntentDeduper* deduper = nullptr,
        IntentRateLimiter* rate_limiter = nullptr
    );

    [[nodiscard]] SignalRunResult scan_once(
        const SignalScanContext& context
    );

    [[nodiscard]] std::span<const OpportunityIntent> last_published_intents()
        const noexcept;

    [[nodiscard]] SignalEvidenceView last_published_evidence_at(
        std::size_t index
    ) const noexcept;

private:
    SignalConfig config_;
    const IMarketSnapshotReader* snapshot_reader_ = nullptr;
    const ISnapshotBatchReader* snapshot_batch_reader_ = nullptr;
    const IDepthBatchReader* depth_batch_reader_ = nullptr;
    const OracleArtifactReader* artifact_reader_ = nullptr;
    const SettlementMaskChecker* settlement_checker_ = nullptr;
    const VWAPPrecheck* vwap_ = nullptr;
    const TheoreticalEdgeCalculator* edge_calculator_ = nullptr;
    const OpportunityRanker* ranker_ = nullptr;
    IIntentPublisher* publisher_ = nullptr;
    IntentDeduper* deduper_ = nullptr;
    IntentRateLimiter default_rate_limiter_;
    IntentRateLimiter* external_rate_limiter_ = nullptr;
    std::unique_ptr<SignalScratch> scratch_;

    std::vector<OpportunityIntent> last_published_intents_;
    std::vector<SignalPublishedEvidence> last_published_evidence_;
    std::vector<std::size_t> last_published_evidence_indices_;
};

}  // namespace trading_engine::signal
