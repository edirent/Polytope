#pragma once

#include "engine/risk/guards/IRiskGuard.h"

#include <cstdint>
#include <string>
#include <unordered_set>

namespace trading_engine::risk {

class DuplicateIntentGuard final : public IRiskGuard {
public:
    GuardResult check(
        const signal::OpportunityIntent& intent,
        std::uint64_t now_ns
    ) override;

    void clear();

private:
    std::unordered_set<std::uint64_t> seen_idempotency_hashes_;
    std::unordered_set<std::string> seen_idempotency_keys_;
};

}  // namespace trading_engine::risk
