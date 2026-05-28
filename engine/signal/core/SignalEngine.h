#pragma once

#include "engine/signal/core/SignalWorkflow.h"
#include "engine/signal/edge/TheoreticalEdgeCalculator.h"
#include "engine/signal/pricing/VWAPPrecheck.h"
#include "engine/signal/public/SignalConfig.h"
#include "engine/signal/publish/IIntentPublisher.h"
#include "engine/signal/rank/OpportunityRanker.h"
#include "engine/signal/reader/MarketSnapshotReader.h"

namespace trading_engine::signal {

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
        IIntentPublisher* publisher
    );

    [[nodiscard]] SignalRunResult scan_once(
        const SignalScanContext& context
    );

private:
    SignalConfig config_;
    const IMarketSnapshotReader* snapshot_reader_ = nullptr;
    const OracleArtifactReader* artifact_reader_ = nullptr;
    const SettlementMaskChecker* settlement_checker_ = nullptr;
    const VWAPPrecheck* vwap_ = nullptr;
    const TheoreticalEdgeCalculator* edge_calculator_ = nullptr;
    const OpportunityRanker* ranker_ = nullptr;
    IIntentPublisher* publisher_ = nullptr;
};

}  // namespace trading_engine::signal
