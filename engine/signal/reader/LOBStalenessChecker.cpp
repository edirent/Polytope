#include "engine/signal/reader/LOBStalenessChecker.h"

#include <limits>

namespace trading_engine::signal {

std::uint64_t LOBStalenessChecker::age_ns(
    const trading_engine::state::MarketStateSnapshot& snapshot,
    std::uint64_t now_ns
) const noexcept {
    if (snapshot.last_book_update_ns == 0 || now_ns == 0) {
        return 0;
    }
    if (now_ns < snapshot.last_book_update_ns) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return now_ns - snapshot.last_book_update_ns;
}

bool LOBStalenessChecker::is_stale(
    const trading_engine::state::MarketStateSnapshot& snapshot,
    std::uint64_t now_ns,
    std::int64_t max_age_ns
) const noexcept {
    if (snapshot.last_book_update_ns == 0 || now_ns == 0 || max_age_ns < 0) {
        return false;
    }
    return age_ns(snapshot, now_ns) > static_cast<std::uint64_t>(max_age_ns);
}

}  // namespace trading_engine::signal
