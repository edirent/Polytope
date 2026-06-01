#pragma once

#include "engine/paper/public/PaperAccount.h"
#include "engine/paper/public/PerformanceSnapshot.h"
#include "engine/paper/read/RuntimeSnapshotRing.h"
#include "engine/paper/regime/RegimeSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trading_engine::paper {

using PaperAccountSnapshot = PaperAccount;

struct LatencySnapshot {
    std::uint64_t feed_to_state_ns = 0;
    std::uint64_t state_to_signal_ns = 0;
    std::uint64_t signal_to_risk_ns = 0;
    std::uint64_t risk_to_execution_ns = 0;
    std::uint64_t end_to_end_ns = 0;
};

struct SignalDashboardSnapshot {
    std::uint64_t intents_published = 0;
    std::uint64_t paper_opportunities = 0;
    std::uint64_t rejected = 0;
    std::uint64_t output_hash = 0;
};

struct RiskDashboardSnapshot {
    std::uint64_t decisions = 0;
    std::uint64_t approved = 0;
    std::uint64_t rejected = 0;
    std::uint64_t output_hash = 0;
};

struct ExecutionDashboardSnapshot {
    std::uint64_t plans_created = 0;
    std::uint64_t plans_filled = 0;
    std::uint64_t plans_failed = 0;
    std::uint64_t output_hash = 0;
};

struct PaperFilledOrderSnapshot {
    std::uint64_t execution_report_id = 0;
    std::uint64_t plan_id = 0;
    std::uint64_t child_order_id = 0;
    std::uint64_t bundle_id = 0;
    std::uint64_t source_intent_id = 0;
    std::uint64_t approved_intent_id = 0;
    std::uint64_t reservation_id = 0;

    std::string market_id;
    std::string asset_id;
    std::uint32_t market_index = 0;
    std::uint32_t asset_index = 0;
    std::string side;

    std::int64_t filled_lots = 0;
    std::int64_t remaining_lots = 0;
    std::int64_t avg_fill_price_tick = 0;
    std::int64_t limit_price_tick = 0;
    std::int64_t estimated_vwap_tick = 0;
    std::int64_t worst_price_tick = 0;
    std::int64_t notional_tick = 0;

    std::int64_t mark_price_tick = 0;
    std::int64_t unrealized_pnl_tick = 0;
    std::string mark_quality;

    std::uint64_t event_ts_ns = 0;
};

struct DashboardSnapshot {
    std::uint64_t seq_no = 0;
    std::uint64_t ts_ns = 0;

    PaperAccountSnapshot account;
    PerformanceSnapshot performance;
    RegimeSnapshot regime;
    LatencySnapshot latency;

    SignalDashboardSnapshot signal;
    RiskDashboardSnapshot risk;
    ExecutionDashboardSnapshot execution;

    std::vector<PaperFilledOrderSnapshot> filled_orders;
};

class DashboardReadStore {
public:
    explicit DashboardReadStore(std::size_t capacity = 256);

    [[nodiscard]] std::uint64_t publish(DashboardSnapshot snapshot);

    [[nodiscard]] std::optional<DashboardSnapshot> latest() const;

    [[nodiscard]] std::vector<DashboardSnapshot> read_since(
        std::uint64_t since_seq,
        std::size_t max_count = static_cast<std::size_t>(-1)
    ) const;

    [[nodiscard]] std::uint64_t latest_seq() const noexcept;
    [[nodiscard]] std::uint64_t dropped_frames() const noexcept;

private:
    RuntimeSnapshotRing<DashboardSnapshot> ring_;
};

}  // namespace trading_engine::paper
