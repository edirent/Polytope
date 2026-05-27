#pragma once

#include "feed/state/EntityStateStore.h"

#include <cstddef>

namespace trading_engine::feed {

class StateHasher {
public:
    [[nodiscard]] std::size_t hash(const EntityStateStore& store) const noexcept;
};

}  // namespace trading_engine::feed
