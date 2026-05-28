#pragma once

#include "state/EntityStateStore.h"
#include "state/book/BookApplyResult.h"
#include "state/core/MarketStateEvent.h"

#include <span>

namespace trading_engine::state {

class LOBWriter {
public:
    explicit LOBWriter(EntityStateStore* store);

    BookApplyResult apply(const MarketStateEvent& event);
    BookBatchApplyResult apply_batch(std::span<const MarketStateEvent> events);

private:
    EntityStateStore* store_{nullptr};
};

}  // namespace trading_engine::state
