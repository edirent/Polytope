#include "engine/paper/read/DashboardReadStore.h"

#include <utility>

namespace trading_engine::paper {

DashboardReadStore::DashboardReadStore(std::size_t capacity) : ring_(capacity) {}

std::uint64_t DashboardReadStore::publish(DashboardSnapshot snapshot) {
    return ring_.publish(std::move(snapshot));
}

std::optional<DashboardSnapshot> DashboardReadStore::latest() const {
    return ring_.latest();
}

std::vector<DashboardSnapshot> DashboardReadStore::read_since(
    std::uint64_t since_seq,
    std::size_t max_count
) const {
    return ring_.read_since(since_seq, max_count);
}

std::uint64_t DashboardReadStore::latest_seq() const noexcept {
    return ring_.latest_seq();
}

std::uint64_t DashboardReadStore::dropped_frames() const noexcept {
    return ring_.dropped_frames();
}

}  // namespace trading_engine::paper
