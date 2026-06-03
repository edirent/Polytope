#pragma once

#include "engine/paper/public/PaperFill.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace trading_engine::paper {

enum class AdverseSelectionStatus : std::uint8_t {
    PendingHorizon,
    Ready,
    MissingMark
};

struct AdverseSelectionRecord {
    std::uint64_t fill_id = 0;
    std::uint32_t asset_index = 0;

    Side side = Side::Buy;
    std::int64_t fill_price_tick = 0;
    std::int64_t mid_at_fill_tick = 0;

    std::uint64_t fill_ts_ns = 0;

    std::int64_t adverse_selection_5s_tick = 0;
    std::int64_t adverse_selection_30s_tick = 0;

    AdverseSelectionStatus status_5s =
        AdverseSelectionStatus::PendingHorizon;
    AdverseSelectionStatus status_30s =
        AdverseSelectionStatus::PendingHorizon;
};

class AdverseSelectionTracker {
public:
    void observe_fill(
        const PaperFill& fill,
        std::optional<std::int64_t> mid_at_fill_tick
    );

    void observe_fill(
        const PaperFill& fill,
        std::int64_t mid_at_fill_tick
    );

    void observe_mark(
        std::uint32_t asset_index,
        std::uint64_t ts_ns,
        std::optional<std::int64_t> mid_tick
    );

    void observe_mark(
        std::uint32_t asset_index,
        std::uint64_t ts_ns,
        std::int64_t mid_tick
    );

    [[nodiscard]] const AdverseSelectionRecord* find(
        std::uint64_t fill_id
    ) const noexcept;

    [[nodiscard]] std::span<const AdverseSelectionRecord> records()
        const noexcept;

private:
    std::vector<AdverseSelectionRecord> records_;
};

}  // namespace trading_engine::paper
