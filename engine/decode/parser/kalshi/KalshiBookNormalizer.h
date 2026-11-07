#pragma once
#include "engine/state/cross_venue/Types.h"

namespace trading_engine::cross_venue {
// Offline decoder for documented orderbook_fp REST and fixed-point WS shapes.
// One market per subscription; a REST snapshot cannot seed a WS delta stream.
class KalshiBookNormalizer {
public:
    KalshiBookNormalizer(VenueInstrumentKey key, bool buy_yes, bool preserve_fractional = false);
    BookUpdate rest_snapshot(std::string_view raw, BookProvenance provenance);
    BookUpdate websocket(std::string_view raw, BookProvenance provenance, bool subscription_sequence_validated = false);
    BookUpdate disconnect(BookProvenance provenance);
private:
    VenueInstrumentKey key_;
    bool buy_yes_, ready_ = false;
    bool fractional_ = false;
    std::uint64_t sid_ = 0, seq_ = 0, last_clock_ = 0;
    std::map<Money, Money> yes_bids_, no_bids_;  // quantities in millionths
};
}  // namespace trading_engine::cross_venue
