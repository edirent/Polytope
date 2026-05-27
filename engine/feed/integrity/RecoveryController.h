#pragma once

#include "feed/integrity/ConsistencyChecker.h"
#include "feed/integrity/StaleDetector.h"

#include <string>

namespace trading_engine::feed {

enum class RecoveryAction {
    None,
    MarkUnsafe,
    RequestSnapshot,
    ReconnectSource,
    ResetEntity,
    DisableEntity
};

class RecoveryController {
public:
    [[nodiscard]] RecoveryAction decide(
        const ConsistencyResult& consistency
    ) const noexcept;

    [[nodiscard]] RecoveryAction decide(const StaleResult& stale) const noexcept;

    [[nodiscard]] RecoveryAction decide(
        const ConsistencyResult& consistency,
        const StaleResult& stale
    ) const noexcept;
};

[[nodiscard]] std::string to_string(RecoveryAction action);

}  // namespace trading_engine::feed
