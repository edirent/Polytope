#pragma once

#include "state/EntityStateStore.h"
#include "state/core/MarketStateEvent.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::state {

enum class BookApplyCode : std::uint8_t {
    Applied = 0,
    IgnoredHeartbeat,
    IgnoredChainEvent,
    IgnoredDataQualityUpdate,
    MissingStore,
    StateRejected
};

struct BookApplyResult {
    BookApplyCode code{BookApplyCode::StateRejected};
    MarketStateEventType event_type{MarketStateEventType::DataQualityUpdate};
    StateApplyCode state_code{StateApplyCode::Noop};

    std::string entity_id;
    std::string message;

    bool state_changed{false};
    std::uint64_t entity_hash{0};
    std::uint64_t global_hash{0};

    [[nodiscard]] bool ok() const noexcept {
        return code == BookApplyCode::Applied ||
               code == BookApplyCode::IgnoredHeartbeat ||
               code == BookApplyCode::IgnoredChainEvent ||
               code == BookApplyCode::IgnoredDataQualityUpdate;
    }

    [[nodiscard]] bool ignored() const noexcept {
        return code == BookApplyCode::IgnoredHeartbeat ||
               code == BookApplyCode::IgnoredChainEvent ||
               code == BookApplyCode::IgnoredDataQualityUpdate;
    }
};

struct BookBatchApplyResult {
    std::size_t events_seen{0};
    std::size_t applied{0};
    std::size_t ignored{0};
    std::size_t errors{0};
    std::size_t state_changed{0};

    std::uint64_t global_hash{0};

    std::vector<BookApplyResult> results;

    [[nodiscard]] bool ok() const noexcept {
        return errors == 0;
    }
};

[[nodiscard]] std::string to_string(BookApplyCode code);

}  // namespace trading_engine::state
