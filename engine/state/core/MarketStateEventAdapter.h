#pragma once

#include "chain_confirm/ClassifiedFillRecord.h"
#include "decode/public/NormalizedEventBatch.h"
#include "state/core/MarketStateEvent.h"

#include <vector>

namespace trading_engine::state {

[[nodiscard]] std::vector<MarketStateEvent> from_normalized_batch(
    const decode::NormalizedEventBatch& batch
);

[[nodiscard]] MarketStateEvent from_classified_fill(
    const chain_confirm::ClassifiedFillRecord& fill
);

}  // namespace trading_engine::state
