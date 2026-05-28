#pragma once

#include "engine/signal/public/OpportunityIntent.h"
#include "engine/signal/public/SignalResult.h"
#include "engine/signal/reader/OracleArtifactReader.h"

#include <cstdint>
#include <span>

namespace trading_engine::signal {

struct SignalScanContext {
    std::uint64_t scan_id = 0;
    std::uint64_t now_monotonic_ns = 0;

    std::uint64_t current_true_mask = 0;
    std::uint64_t current_false_mask = 0;

    bool settlement_masks_available = false;
};

class SettlementMaskChecker {
public:
    [[nodiscard]] bool is_valid(
        const CandidateBundle& bundle,
        const SignalScanContext& context
    ) const noexcept;
};

void increment_counter_for_status(
    IntentStatus status,
    SignalRunResult* result
) noexcept;

[[nodiscard]] std::uint64_t hash_published_intents(
    std::span<const OpportunityIntent> intents
) noexcept;

}  // namespace trading_engine::signal
