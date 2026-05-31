#include "engine/risk/guards/DuplicateIntentGuard.h"
#include "engine/risk/guards/IntentExpiryGuard.h"
#include "engine/risk/guards/KillSwitchGuard.h"
#include "engine/risk/guards/RateLimitGuard.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::risk::GuardResult;
using trading_engine::risk::IRiskGuard;
using trading_engine::risk::IntentExpiryGuard;
using trading_engine::risk::DuplicateIntentGuard;
using trading_engine::risk::KillSwitchGuard;
using trading_engine::risk::RateLimitGuard;
using trading_engine::risk::RiskDecisionType;
using trading_engine::risk::RiskResult;
using trading_engine::risk::kRiskRejectFlagDuplicateIntent;
using trading_engine::risk::kRiskRejectFlagExpiredIntent;
using trading_engine::risk::kRiskRejectFlagKillSwitch;
using trading_engine::risk::kRiskRejectFlagRateLimited;
using trading_engine::risk::record_guard_result;
using trading_engine::risk::run_risk_guards;
using trading_engine::signal::OpportunityIntent;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

void expect_false(bool value, const std::string& field) {
    if (value) {
        fail("expected false: " + field);
    }
}

template <typename Actual, typename Expected>
void expect_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& field
) {
    if (!(actual == expected)) {
        fail("mismatch: " + field);
    }
}

OpportunityIntent make_intent() {
    OpportunityIntent intent;
    intent.intent_id = 1;
    intent.bundle_id = 2;
    intent.created_ts_ns = 1'000;
    intent.expires_at_ns = 2'000;
    intent.idempotency_key = "risk-intent-1";
    return intent;
}

class CountingPassGuard final : public IRiskGuard {
public:
    GuardResult check(const OpportunityIntent&, std::uint64_t) override {
        ++calls;
        GuardResult result;
        result.pass = true;
        result.rejection = RiskDecisionType::Pass;
        result.reject_flag = 0;
        return result;
    }

    int calls = 0;
};

void KillSwitchGuard_RejectsWhenEnabled() {
    KillSwitchGuard guard(true);
    const auto result = guard.check(make_intent(), 1'500);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectKillSwitch,
        "rejection"
    );
    expect_equal(result.reject_flag, kRiskRejectFlagKillSwitch, "flag");
}

void IntentExpiryGuard_RejectsExpired() {
    IntentExpiryGuard guard;
    auto intent = make_intent();
    intent.expires_at_ns = 1'500;

    const auto result = guard.check(intent, 1'500);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectExpiredIntent,
        "rejection"
    );
    expect_equal(result.reject_flag, kRiskRejectFlagExpiredIntent, "flag");
}

void DuplicateIntentGuard_RejectsRepeatedIdempotencyKey() {
    DuplicateIntentGuard guard;
    const auto intent = make_intent();

    const auto first = guard.check(intent, 1'500);
    const auto second = guard.check(intent, 1'501);

    expect_true(first.pass, "first pass");
    expect_false(second.pass, "second pass");
    expect_equal(
        second.rejection,
        RiskDecisionType::RejectDuplicateIntent,
        "rejection"
    );
    expect_equal(
        second.reject_flag,
        kRiskRejectFlagDuplicateIntent,
        "flag"
    );
}

void DuplicateIntentGuard_UsesIdempotencyHashWithoutStringKey() {
    DuplicateIntentGuard guard;
    auto intent = make_intent();
    intent.idempotency_hash = 12345;
    intent.idempotency_key.clear();

    const auto first = guard.check(intent, 1'500);
    const auto second = guard.check(intent, 1'501);

    expect_true(first.pass, "first pass");
    expect_false(second.pass, "second pass");
    expect_equal(
        second.rejection,
        RiskDecisionType::RejectDuplicateIntent,
        "rejection"
    );
    expect_equal(
        second.reject_flag,
        kRiskRejectFlagDuplicateIntent,
        "flag"
    );
}

void RateLimitGuard_RejectsOverLimit() {
    RateLimitGuard guard(1);
    const auto intent = make_intent();

    const auto first = guard.check(intent, 1'000'000'000);
    const auto second = guard.check(intent, 1'000'000'001);

    expect_true(first.pass, "first pass");
    expect_false(second.pass, "second pass");
    expect_equal(
        second.rejection,
        RiskDecisionType::RejectRateLimited,
        "rejection"
    );
    expect_equal(second.reject_flag, kRiskRejectFlagRateLimited, "flag");
}

void CheapGuardFailure_ShortCircuitsBeforeVwap() {
    KillSwitchGuard kill_switch(true);
    CountingPassGuard expensive_guard;
    IRiskGuard* guards[] = {&kill_switch, &expensive_guard};

    const auto result = run_risk_guards(guards, make_intent(), 1'500);

    expect_false(result.pass, "pass");
    expect_equal(
        result.rejection,
        RiskDecisionType::RejectKillSwitch,
        "rejection"
    );
    expect_equal(expensive_guard.calls, 0, "expensive guard not called");
}

void CheapGuardRejectCountersAreCorrect() {
    RiskResult result;

    GuardResult kill_switch;
    kill_switch.pass = false;
    kill_switch.rejection = RiskDecisionType::RejectKillSwitch;
    record_guard_result(kill_switch, &result);

    GuardResult expired;
    expired.pass = false;
    expired.rejection = RiskDecisionType::RejectExpiredIntent;
    record_guard_result(expired, &result);

    GuardResult duplicate;
    duplicate.pass = false;
    duplicate.rejection = RiskDecisionType::RejectDuplicateIntent;
    record_guard_result(duplicate, &result);

    GuardResult limited;
    limited.pass = false;
    limited.rejection = RiskDecisionType::RejectRateLimited;
    record_guard_result(limited, &result);

    expect_equal(result.intents_rejected, 4ULL, "rejected total");
    expect_equal(result.rejected_kill_switch, 1ULL, "kill switch");
    expect_equal(result.rejected_stale_or_expired, 1ULL, "expired");
    expect_equal(
        result.rejected_pending_or_rate_limit,
        2ULL,
        "pending/rate"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "KillSwitchGuard_RejectsWhenEnabled",
            &KillSwitchGuard_RejectsWhenEnabled
        },
        {
            "IntentExpiryGuard_RejectsExpired",
            &IntentExpiryGuard_RejectsExpired
        },
        {
            "DuplicateIntentGuard_RejectsRepeatedIdempotencyKey",
            &DuplicateIntentGuard_RejectsRepeatedIdempotencyKey
        },
        {
            "DuplicateIntentGuard_UsesIdempotencyHashWithoutStringKey",
            &DuplicateIntentGuard_UsesIdempotencyHashWithoutStringKey
        },
        {
            "RateLimitGuard_RejectsOverLimit",
            &RateLimitGuard_RejectsOverLimit
        },
        {
            "CheapGuardFailure_ShortCircuitsBeforeVwap",
            &CheapGuardFailure_ShortCircuitsBeforeVwap
        },
        {
            "CheapGuardRejectCountersAreCorrect",
            &CheapGuardRejectCountersAreCorrect
        }
    };
    return test_map;
}

int run_test(const std::string& name) {
    const auto it = tests().find(name);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << name << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << name << " failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << name << " passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        return run_test(argv[1]);
    }

    int failures = 0;
    for (const auto& [name, _] : tests()) {
        failures += run_test(name) == 0 ? 0 : 1;
    }
    return failures == 0 ? 0 : 1;
}
