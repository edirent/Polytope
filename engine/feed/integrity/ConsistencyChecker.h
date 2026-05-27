#pragma once

#include "feed/state/EntityStateStore.h"

#include <cstdint>
#include <string>

namespace trading_engine::feed {

enum class ConsistencyCode {
    Ok = 0,
    MissingEntityId,
    DeltaBeforeSnapshot,
    InvalidValue,
    CrossedBook,
    ExternalBboDiverged,
    ClosedEntityMutation,
    DecodeError,
    UnknownEvent
};

struct ConsistencyResult {
    ConsistencyCode code{ConsistencyCode::Ok};
    std::string entity_id;
    std::string reason;
    std::uint64_t checked_ns{0};

    [[nodiscard]] bool ok() const noexcept {
        return code == ConsistencyCode::Ok;
    }
};

using ConsistencyReport = ConsistencyResult;

class ConsistencyChecker {
public:
    [[nodiscard]] ConsistencyResult check_event(
        const NormalizedEvent& event
    ) const noexcept;

    [[nodiscard]] ConsistencyResult check_apply_result(
        const StateApplyResult& result
    ) const noexcept;

    [[nodiscard]] ConsistencyResult check_entity_state(
        const EntityState& entity
    ) const noexcept;

    [[nodiscard]] ConsistencyResult check(
        const NormalizedEvent& event,
        const StateApplyResult& result,
        const EntityState* entity
    ) const noexcept;
};

[[nodiscard]] std::string to_string(ConsistencyCode code);

}  // namespace trading_engine::feed
