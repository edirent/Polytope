#include "feed/integrity/ConsistencyChecker.h"

namespace trading_engine::feed {

ConsistencyReport ConsistencyChecker::check(const EntityStateStore& store) const {
    if (store.size() == 0) {
        return ConsistencyReport{
            .consistent = false,
            .reason = "entity state store is empty",
        };
    }

    return ConsistencyReport{};
}

}  // namespace trading_engine::feed
