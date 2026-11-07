#pragma once
#include "engine/state/cross_venue/Types.h"

namespace trading_engine::cross_venue {
// Exact-decimal boundary for the international CLOB schema already used in
// this repository. Supports book snapshots and absolute-size price_change.
// local_sequence counts captured packets; it is NOT an exchange sequence.
class CrossVenueClobNormalizer {
public:
    explicit CrossVenueClobNormalizer(VenueInstrumentKey key, bool preserve_fractional = false);
    BookUpdate normalize(std::string_view raw, BookProvenance provenance, std::uint64_t local_sequence);
    BookUpdate disconnect(BookProvenance provenance);
private:
    VenueInstrumentKey key_;
    bool ready_ = false;
    bool fractional_ = false;
    std::uint64_t seq_ = 0;
};
}  // namespace trading_engine::cross_venue
