#include "engine/risk/core/RiskScratch.h"

#include <atomic>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

std::atomic<std::uint64_t> g_allocations{0};

}  // namespace

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* ptr = std::malloc(size)) {
        return ptr;
    }
    throw std::bad_alloc();
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void* operator new[](std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* ptr = std::malloc(size)) {
        return ptr;
    }
    throw std::bad_alloc();
}

void operator delete[](void* ptr) noexcept {
    std::free(ptr);
}

namespace {

using trading_engine::risk::RevalidatedLegCost;
using trading_engine::risk::RiskScratch;
using trading_engine::risk::RiskAuditStepCode;
using trading_engine::risk::RiskDecisionType;

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

void Scratch_ResetClearsCounts() {
    RiskScratch scratch;
    (void)scratch.push_revalidated_leg(RevalidatedLegCost{});
    scratch.audit_trace.step_count = 1;
    scratch.audit_trace.steps[0].step = RiskAuditStepCode::CostRevalidator;
    scratch.audit_trace.steps[0].rejection = RiskDecisionType::Pass;

    scratch.reset();

    expect_equal(scratch.leg_count, static_cast<std::uint16_t>(0), "legs");
    expect_equal(
        scratch.audit_trace.step_count,
        static_cast<std::uint8_t>(0),
        "audit"
    );
}

void Scratch_DoesNotAllocate() {
    RiskScratch scratch;
    g_allocations.store(0, std::memory_order_relaxed);

    scratch.reset();
    (void)scratch.push_revalidated_leg(RevalidatedLegCost{});

    expect_equal(g_allocations.load(std::memory_order_relaxed), 0ULL, "allocations");
}

void Scratch_MaxLegCountEnforced() {
    RiskScratch scratch;
    for (std::uint16_t i = 0; i < trading_engine::risk::kMaxRevalidatedLegCosts; ++i) {
        expect_true(scratch.push_revalidated_leg(RevalidatedLegCost{}), "push leg");
    }
    expect_false(
        scratch.push_revalidated_leg(RevalidatedLegCost{}),
        "leg overflow"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"Scratch_ResetClearsCounts", &Scratch_ResetClearsCounts},
        {"Scratch_DoesNotAllocate", &Scratch_DoesNotAllocate},
        {"Scratch_MaxLegCountEnforced", &Scratch_MaxLegCountEnforced}
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

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }
    return run_test(argv[1]);
}
