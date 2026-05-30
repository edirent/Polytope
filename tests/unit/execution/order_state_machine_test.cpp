#include "engine/execution/state/OrderStateMachine.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::execution::ChildOrderStatus;
using trading_engine::execution::OrderStateMachine;

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

void OrderStateMachine_RejectsInvalidTransition() {
    const OrderStateMachine machine;

    expect_true(
        !machine.can_transition(
            ChildOrderStatus::Created,
            ChildOrderStatus::Filled
        ),
        "Created -> Filled rejected"
    );
    expect_equal(
        machine.transition(
            ChildOrderStatus::Created,
            ChildOrderStatus::Filled
        ),
        ChildOrderStatus::Created,
        "invalid transition keeps current"
    );
}

void OrderStateMachine_AllowsCreatedToPlanned() {
    const OrderStateMachine machine;

    expect_true(
        machine.can_transition(
            ChildOrderStatus::Created,
            ChildOrderStatus::Planned
        ),
        "Created -> Planned allowed"
    );
    expect_equal(
        machine.transition(
            ChildOrderStatus::Created,
            ChildOrderStatus::Planned
        ),
        ChildOrderStatus::Planned,
        "Created -> Planned"
    );
}

void OrderStateMachine_AllowsAckedToFilled() {
    const OrderStateMachine machine;

    expect_true(
        machine.can_transition(
            ChildOrderStatus::Acked,
            ChildOrderStatus::Filled
        ),
        "Acked -> Filled allowed"
    );
    expect_equal(
        machine.transition(
            ChildOrderStatus::Acked,
            ChildOrderStatus::Filled
        ),
        ChildOrderStatus::Filled,
        "Acked -> Filled"
    );
}

void OrderStateMachine_TerminalStateCannotTransition() {
    const OrderStateMachine machine;

    expect_true(
        !machine.can_transition(
            ChildOrderStatus::Filled,
            ChildOrderStatus::CancelRequested
        ),
        "terminal cannot transition"
    );
    expect_equal(
        machine.transition(
            ChildOrderStatus::Filled,
            ChildOrderStatus::CancelRequested
        ),
        ChildOrderStatus::Filled,
        "terminal remains filled"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "OrderStateMachine_RejectsInvalidTransition",
            &OrderStateMachine_RejectsInvalidTransition
        },
        {
            "OrderStateMachine_AllowsCreatedToPlanned",
            &OrderStateMachine_AllowsCreatedToPlanned
        },
        {
            "OrderStateMachine_AllowsAckedToFilled",
            &OrderStateMachine_AllowsAckedToFilled
        },
        {
            "OrderStateMachine_TerminalStateCannotTransition",
            &OrderStateMachine_TerminalStateCannotTransition
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
