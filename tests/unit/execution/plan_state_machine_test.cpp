#include "engine/execution/state/PlanStateMachine.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::PlanStateMachine;
using trading_engine::execution::PlanStatus;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
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

void PlanStateMachine_RejectsInvalidTransition() {
    const PlanStateMachine machine;

    expect_true(
        !machine.can_transition(PlanStatus::Created, PlanStatus::Filled),
        "Created -> Filled rejected"
    );
    expect_equal(
        machine.transition(PlanStatus::Created, PlanStatus::Filled),
        PlanStatus::Created,
        "invalid transition keeps current"
    );
}

void PlanStateMachine_AllowsCreatedToPlanned() {
    const PlanStateMachine machine;

    expect_true(
        machine.can_transition(PlanStatus::Created, PlanStatus::Planned),
        "Created -> Planned allowed"
    );
    expect_equal(
        machine.transition(PlanStatus::Created, PlanStatus::Planned),
        PlanStatus::Planned,
        "Created -> Planned"
    );
}

void PlanStateMachine_AllowsAckedToFilled() {
    const PlanStateMachine machine;

    expect_true(
        machine.can_transition(PlanStatus::Acked, PlanStatus::Filled),
        "Acked -> Filled allowed"
    );
    expect_equal(
        machine.transition(PlanStatus::Acked, PlanStatus::Filled),
        PlanStatus::Filled,
        "Acked -> Filled"
    );
}

void PlanStateMachine_PartialFillCanBecomeHedgeRequired() {
    const PlanStateMachine machine;

    expect_true(
        machine.can_transition(
            PlanStatus::PartiallyFilled,
            PlanStatus::HedgeRequired
        ),
        "partial -> hedge required allowed"
    );
    expect_equal(
        machine.transition(
            PlanStatus::PartiallyFilled,
            PlanStatus::HedgeRequired
        ),
        PlanStatus::HedgeRequired,
        "partial -> hedge required"
    );
}

void PlanStateMachine_TerminalStateCannotTransition() {
    const PlanStateMachine machine;

    expect_true(
        !machine.can_transition(PlanStatus::Filled, PlanStatus::CancelRequested),
        "terminal cannot transition"
    );
    expect_equal(
        machine.transition(PlanStatus::Filled, PlanStatus::CancelRequested),
        PlanStatus::Filled,
        "terminal remains filled"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "PlanStateMachine_RejectsInvalidTransition",
            &PlanStateMachine_RejectsInvalidTransition
        },
        {
            "PlanStateMachine_AllowsCreatedToPlanned",
            &PlanStateMachine_AllowsCreatedToPlanned
        },
        {
            "PlanStateMachine_AllowsAckedToFilled",
            &PlanStateMachine_AllowsAckedToFilled
        },
        {
            "PlanStateMachine_PartialFillCanBecomeHedgeRequired",
            &PlanStateMachine_PartialFillCanBecomeHedgeRequired
        },
        {
            "PlanStateMachine_TerminalStateCannotTransition",
            &PlanStateMachine_TerminalStateCannotTransition
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
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <test-name>\n";
        return 2;
    }
    return run_test(argv[1]);
}
