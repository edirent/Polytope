#include "chain_confirm/FillReconciler.h"

#include <algorithm>
#include <utility>

namespace trading_engine::chain_confirm {

namespace {

ReconciliationResult base_result_from_hint(
    const PendingTradeHint& hint,
    ReconciliationStatus status
) {
    ReconciliationResult result;
    result.status = status;
    result.hint_ids.push_back(hint.hint_id);
    result.market_id = hint.market_id;
    result.asset_id = hint.asset_id;
    result.price_tick = hint.price_tick;
    result.size_lots = hint.size_lots;
    result.hint_direction = hint.hint_direction;
    return result;
}

ReconciliationResult base_result_from_fill(
    const ConfirmedFill& fill,
    ReconciliationStatus status
) {
    ReconciliationResult result;
    result.status = status;
    result.fill_id = fill.fill_id;
    result.market_id = fill.market_id;
    result.asset_id = fill.asset_id;
    result.price_tick = fill.price_tick;
    result.size_lots = fill.size_lots;
    result.confirmed_direction = fill.direction;
    return result;
}

}  // namespace

FillReconciler::FillReconciler(
    ConfirmedFillStore* confirmed_store,
    ChainConfirmConfig config
)
    : pending_(4096),
      confirmed_store_(confirmed_store),
      config_(std::move(config)) {}

ReconciliationResult FillReconciler::on_ws_hint(
    const PendingTradeHint& hint
) {
    pending_.push(hint);
    auto result = base_result_from_hint(
        hint,
        ReconciliationStatus::UnmatchedHint
    );
    result.reason = "WS hint stored as unconfirmed";
    return result;
}

ReconciliationResult FillReconciler::on_chain_fill(
    const ConfirmedFill& fill
) {
    if (confirmed_store_) {
        const auto store_result = confirmed_store_->upsert(fill);
        (void)store_result;
    }

    if (fill.removed) {
        auto result = base_result_from_fill(
            fill,
            ReconciliationStatus::RemovedByReorg
        );
        result.reason = "chain log removed by reorg";
        return result;
    }

    if (fill.mapping_status != FillMappingStatus::Mapped ||
        fill.market_id.empty() ||
        fill.asset_id.empty()) {
        auto result = base_result_from_fill(
            fill,
            ReconciliationStatus::UnmatchedFill
        );
        result.reason = to_string(fill.mapping_status);
        return result;
    }

    auto candidates = pending_.candidates(
        fill.market_id,
        fill.asset_id,
        fill.price_tick,
        fill.size_lots,
        fill.chain_seen_monotonic_ns,
        pending_window_ns()
    );

    if (config_.max_candidate_matches > 0 &&
        candidates.size() > config_.max_candidate_matches) {
        candidates.resize(config_.max_candidate_matches);
    }

    if (candidates.empty()) {
        auto result = base_result_from_fill(
            fill,
            ReconciliationStatus::UnmatchedFill
        );
        result.reason = "no matching WS hint";
        return result;
    }

    if (candidates.size() > 1) {
        auto result = base_result_from_fill(
            fill,
            ReconciliationStatus::Ambiguous
        );
        result.candidate_count =
            static_cast<std::uint32_t>(candidates.size());
        for (const auto* hint : candidates) {
            result.hint_ids.push_back(hint->hint_id);
        }
        result.reason = "multiple WS hints match chain fill";
        return result;
    }

    PendingTradeHint* hint = candidates.front();
    hint->finalized = true;

    auto result = base_result_from_fill(
        fill,
        ReconciliationStatus::ConfirmedOneToOne
    );
    result.hint_ids.push_back(hint->hint_id);
    result.hint_direction = hint->hint_direction;
    result.candidate_count = 1;
    result.finalized = true;
    result.reason = "chain fill confirmed WS hint";
    return result;
}

std::vector<ReconciliationResult> FillReconciler::expire_unmatched(
    std::uint64_t now_monotonic_ns
) {
    const auto expired = pending_.expire(
        now_monotonic_ns,
        expire_unmatched_ns()
    );

    std::vector<ReconciliationResult> results;
    results.reserve(expired.size());
    for (const auto& hint : expired) {
        auto result = base_result_from_hint(
            hint,
            ReconciliationStatus::ExpiredHint
        );
        result.reason = "WS hint expired without chain confirmation";
        results.push_back(std::move(result));
    }

    return results;
}

std::size_t FillReconciler::pending_count() const noexcept {
    return pending_.size();
}

std::uint64_t FillReconciler::pending_window_ns() const noexcept {
    return config_.pending_window_ms * 1'000'000ULL;
}

std::uint64_t FillReconciler::expire_unmatched_ns() const noexcept {
    return config_.expire_unmatched_ms * 1'000'000ULL;
}

}  // namespace trading_engine::chain_confirm
