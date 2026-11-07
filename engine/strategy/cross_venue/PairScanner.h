#pragma once
#include "engine/state/cross_venue/SynchronizedBookStore.h"

namespace trading_engine::cross_venue {
class PairScanner {
public:
    static CrossVenueOpportunity quote(const ApprovedPair& pair,
        const std::array<VersionedBook, 2>& books, const FeeQuote& fee,
        const PairPolicy& policy, std::uint64_t now_ns);
};
}  // namespace trading_engine::cross_venue
