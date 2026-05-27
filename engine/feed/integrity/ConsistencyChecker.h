#pragma once

#include "feed/state/EntityStateStore.h"

#include <string>

namespace trading_engine::feed {

struct ConsistencyReport {
    bool consistent{true};
    std::string reason;
};

class ConsistencyChecker {
public:
    [[nodiscard]] ConsistencyReport check(const EntityStateStore& store) const;
};

}  // namespace trading_engine::feed
