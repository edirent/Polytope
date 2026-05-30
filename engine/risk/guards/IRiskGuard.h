#pragma once

#include "engine/risk/public/RiskResult.h"
#include "engine/risk/public/RiskTypes.h"
#include "engine/signal/public/OpportunityIntent.h"

#include <cstdint>
#include <span>
#include <string>

namespace trading_engine::risk {

inline constexpr std::uint64_t kRiskRejectFlagKillSwitch = 1ULL << 0;
inline constexpr std::uint64_t kRiskRejectFlagExpiredIntent = 1ULL << 1;
inline constexpr std::uint64_t kRiskRejectFlagDuplicateIntent = 1ULL << 2;
inline constexpr std::uint64_t kRiskRejectFlagRateLimited = 1ULL << 3;
inline constexpr std::uint64_t kRiskRejectFlagBadMarketState = 1ULL << 4;
inline constexpr std::uint64_t kRiskRejectFlagStaleSnapshot = 1ULL << 5;
inline constexpr std::uint64_t kRiskRejectFlagInternalError = 1ULL << 63;

struct GuardResult {
    bool pass = false;
    bool requires_reprice = false;
    RiskDecisionType rejection = RiskDecisionType::RejectInternalError;
    std::uint64_t reject_flag = kRiskRejectFlagInternalError;
    std::string reason;
};

class IRiskGuard {
public:
    virtual ~IRiskGuard() = default;

    virtual GuardResult check(
        const signal::OpportunityIntent& intent,
        std::uint64_t now_ns
    ) = 0;
};

[[nodiscard]] GuardResult pass_guard();

void record_guard_result(const GuardResult& result, RiskResult* risk_result);

[[nodiscard]] GuardResult run_risk_guards(
    std::span<IRiskGuard* const> guards,
    const signal::OpportunityIntent& intent,
    std::uint64_t now_ns
);

}  // namespace trading_engine::risk
