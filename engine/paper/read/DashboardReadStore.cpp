#include "engine/paper/read/DashboardReadStore.h"

#include <utility>

namespace trading_engine::paper {

DashboardReadStore::DashboardReadStore(std::size_t capacity) : ring_(capacity) {}

RewardDashboardSnapshot reward_dashboard_from_pnl(
    const RewardPnLSnapshot& snapshot
) {
    RewardDashboardSnapshot dashboard;
    dashboard.reward_daily_rate_tick = snapshot.reward_daily_rate_tick;
    dashboard.eligible_quote_seconds = snapshot.eligible_quote_seconds;
    dashboard.reward_accrued_tick_estimate =
        snapshot.reward_accrued_tick_estimate;
    dashboard.reward_reconciled_tick = snapshot.reward_reconciled_tick;
    dashboard.reward_config_age_ms = snapshot.reward_config_age_ms;
    dashboard.reward_source_quality =
        reward::reward_source_quality_name(snapshot.reward_source_quality);
    dashboard.reward_eligible_market_count =
        snapshot.reward_eligible_market_count;
    return dashboard;
}

std::uint64_t DashboardReadStore::publish(DashboardSnapshot snapshot) {
    return ring_.publish(std::move(snapshot));
}

std::optional<DashboardSnapshot> DashboardReadStore::latest() const {
    return ring_.latest();
}

std::vector<DashboardSnapshot> DashboardReadStore::read_since(
    std::uint64_t since_seq,
    std::size_t max_count
) const {
    return ring_.read_since(since_seq, max_count);
}

std::uint64_t DashboardReadStore::latest_seq() const noexcept {
    return ring_.latest_seq();
}

std::uint64_t DashboardReadStore::dropped_frames() const noexcept {
    return ring_.dropped_frames();
}

}  // namespace trading_engine::paper
