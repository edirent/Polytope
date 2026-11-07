#pragma once
#include "engine/state/cross_venue/Types.h"
#include <map>

namespace trading_engine::cross_venue {

// Single-writer replay store. A read returns copies of BOTH books; production
// multi-threaded ingestion must supply serialization around apply/read.
class SynchronizedBookStore {
public:
    explicit SynchronizedBookStore(bool retain_observed_depth = false, bool allow_empty_sides = false) : retain_observed_depth_(retain_observed_depth), allow_empty_sides_(allow_empty_sides) {}
    bool apply(const BookUpdate& update);
    VersionedBook read(const VenueInstrumentKey& key) const;
    std::array<VersionedBook, 2> read_pair(const ApprovedPair& pair) const;
    void note_liveness(const VenueInstrumentKey& key, std::uint64_t now_ns);
    void invalidate(const VenueInstrumentKey& key, std::string reason, std::uint64_t now_ns);
    bool reconcile(const VenueInstrumentKey& key, const VersionedBook& authoritative, std::uint64_t now_ns);
private:
    struct Book {
        VersionedBook view;
        std::map<Money, std::int64_t> bids, asks;
        bool initialized = false;
        std::uint64_t last_packet_recv = 0;
    };
    std::map<std::string, Book> books_;
    bool retain_observed_depth_ = false, allow_empty_sides_ = false;
};
void validate_books(const ApprovedPair& pair, const std::array<VersionedBook, 2>& books,
                    const PairPolicy& policy, std::uint64_t now_ns);
}  // namespace trading_engine::cross_venue
