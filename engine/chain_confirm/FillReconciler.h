#pragma once

#include "chain_confirm/ChainConfirmConfig.h"
#include "chain_confirm/ConfirmedFill.h"
#include "chain_confirm/ConfirmedFillStore.h"
#include "chain_confirm/PendingTradeHintRing.h"
#include "chain_confirm/ReconciliationResult.h"

#include <cstdint>
#include <vector>

namespace trading_engine::chain_confirm {

class FillReconciler {
public:
    explicit FillReconciler(
        ConfirmedFillStore* confirmed_store,
        ChainConfirmConfig config = {}
    );

    [[nodiscard]] ReconciliationResult on_ws_hint(
        const PendingTradeHint& hint
    );

    [[nodiscard]] ReconciliationResult on_chain_fill(
        const ConfirmedFill& fill
    );

    [[nodiscard]] std::vector<ReconciliationResult> expire_unmatched(
        std::uint64_t now_monotonic_ns
    );

    [[nodiscard]] std::size_t pending_count() const noexcept;

private:
    [[nodiscard]] std::uint64_t pending_window_ns() const noexcept;
    [[nodiscard]] std::uint64_t expire_unmatched_ns() const noexcept;

private:
    PendingTradeHintRing pending_;
    ConfirmedFillStore* confirmed_store_{nullptr};
    ChainConfirmConfig config_;
};

}  // namespace trading_engine::chain_confirm
