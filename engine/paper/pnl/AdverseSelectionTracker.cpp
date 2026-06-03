#include "engine/paper/pnl/AdverseSelectionTracker.h"

namespace trading_engine::paper {

namespace {

constexpr std::uint64_t kFiveSecondsNs = 5'000'000'000ULL;
constexpr std::uint64_t kThirtySecondsNs = 30'000'000'000ULL;

[[nodiscard]] std::int64_t adverse_selection(
    Side side,
    std::int64_t mid_at_fill_tick,
    std::int64_t mid_after_horizon_tick
) noexcept {
    return side == Side::Sell
        ? mid_at_fill_tick - mid_after_horizon_tick
        : mid_after_horizon_tick - mid_at_fill_tick;
}

void update_horizon(
    AdverseSelectionRecord* record,
    std::uint64_t mark_ts_ns,
    std::optional<std::int64_t> mid_tick,
    std::uint64_t horizon_ns,
    AdverseSelectionStatus* status,
    std::int64_t* value
) {
    if (*status != AdverseSelectionStatus::PendingHorizon ||
        mark_ts_ns < record->fill_ts_ns + horizon_ns) {
        return;
    }
    if (!mid_tick) {
        *status = AdverseSelectionStatus::MissingMark;
        return;
    }
    *value = adverse_selection(
        record->side,
        record->mid_at_fill_tick,
        *mid_tick
    );
    *status = AdverseSelectionStatus::Ready;
}

}  // namespace

void AdverseSelectionTracker::observe_fill(
    const PaperFill& fill,
    std::optional<std::int64_t> mid_at_fill_tick
) {
    AdverseSelectionRecord record;
    record.fill_id = fill.fill_id;
    record.asset_index = fill.asset_index;
    record.side = fill.side;
    record.fill_price_tick = fill.fill_price_tick;
    record.fill_ts_ns = fill.ts_ns;

    if (!mid_at_fill_tick) {
        record.status_5s = AdverseSelectionStatus::MissingMark;
        record.status_30s = AdverseSelectionStatus::MissingMark;
    } else {
        record.mid_at_fill_tick = *mid_at_fill_tick;
    }

    records_.push_back(record);
}

void AdverseSelectionTracker::observe_fill(
    const PaperFill& fill,
    std::int64_t mid_at_fill_tick
) {
    observe_fill(fill, std::optional<std::int64_t>{mid_at_fill_tick});
}

void AdverseSelectionTracker::observe_mark(
    std::uint32_t asset_index,
    std::uint64_t ts_ns,
    std::optional<std::int64_t> mid_tick
) {
    for (auto& record : records_) {
        if (record.asset_index != asset_index) {
            continue;
        }
        update_horizon(
            &record,
            ts_ns,
            mid_tick,
            kFiveSecondsNs,
            &record.status_5s,
            &record.adverse_selection_5s_tick
        );
        update_horizon(
            &record,
            ts_ns,
            mid_tick,
            kThirtySecondsNs,
            &record.status_30s,
            &record.adverse_selection_30s_tick
        );
    }
}

void AdverseSelectionTracker::observe_mark(
    std::uint32_t asset_index,
    std::uint64_t ts_ns,
    std::int64_t mid_tick
) {
    observe_mark(asset_index, ts_ns, std::optional<std::int64_t>{mid_tick});
}

const AdverseSelectionRecord* AdverseSelectionTracker::find(
    std::uint64_t fill_id
) const noexcept {
    for (const auto& record : records_) {
        if (record.fill_id == fill_id) {
            return &record;
        }
    }
    return nullptr;
}

std::span<const AdverseSelectionRecord> AdverseSelectionTracker::records()
    const noexcept {
    return std::span<const AdverseSelectionRecord>{
        records_.data(),
        records_.size()
    };
}

}  // namespace trading_engine::paper
