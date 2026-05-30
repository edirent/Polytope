#include "engine/signal/publish/IntentDeduper.h"

namespace trading_engine::signal {

IntentDeduper::IntentDeduper(std::uint64_t ttl_ns)
    : ttl_ns_(ttl_ns) {}

bool IntentDeduper::seen_recently(
    const std::string& idempotency_key,
    std::uint64_t now_ns
) {
    if (idempotency_key.empty()) {
        return false;
    }

    const auto it = seen_.find(idempotency_key);
    if (it == seen_.end()) {
        return false;
    }

    const auto seen_at_ns = it->second;
    if (now_ns >= seen_at_ns && now_ns - seen_at_ns < ttl_ns_) {
        return true;
    }

    seen_.erase(it);
    return false;
}

void IntentDeduper::mark_seen(
    const std::string& idempotency_key,
    std::uint64_t now_ns
) {
    if (idempotency_key.empty()) {
        return;
    }
    seen_[idempotency_key] = now_ns;
}

}  // namespace trading_engine::signal
