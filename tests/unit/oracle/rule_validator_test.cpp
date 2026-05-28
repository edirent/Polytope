#include "oracle/rules/RuleValidator.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::oracle::Rulebook;
using trading_engine::oracle::RuleType;
using trading_engine::oracle::RuleValidator;
using trading_engine::oracle::ValidatedRule;

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

ValidatedRule base_rule() {
    ValidatedRule rule;
    rule.rule_id = "r1";
    rule.type = RuleType::ExactlyOne;
    rule.variable_ids = {"m1:YES", "m1:NO"};
    rule.approved = true;
    rule.approved_by = "fixture";
    rule.approved_at_ns = 1;
    return rule;
}

void RuleValidator_RejectsUnknownVariable() {
    Rulebook rulebook;
    rulebook.add_rule(base_rule());

    RuleValidator validator;
    const auto result = validator.validate_rulebook(rulebook, {"m1:YES"});

    expect_false(result.ok(), "validation ok");
    expect_false(result.errors.empty(), "errors");
}

void RuleValidator_RejectsDuplicateRuleId() {
    Rulebook rulebook;
    rulebook.add_rule(base_rule());
    rulebook.add_rule(base_rule());

    RuleValidator validator;
    const auto result = validator.validate_rulebook(
        rulebook,
        {"m1:YES", "m1:NO"}
    );

    expect_false(result.ok(), "validation ok");
    expect_false(result.errors.empty(), "errors");
}

void RuleValidator_RejectsEmptyVariableSet() {
    auto rule = base_rule();
    rule.variable_ids.clear();

    Rulebook rulebook;
    rulebook.add_rule(rule);

    RuleValidator validator;
    const auto result = validator.validate_rulebook(rulebook);

    expect_false(result.ok(), "validation ok");
    expect_false(result.errors.empty(), "errors");
}

void RuleValidator_ApprovedFixtureIsCompilerReady() {
    Rulebook rulebook;
    rulebook.add_rule(base_rule());

    RuleValidator validator;
    const auto result = validator.validate_rulebook(
        rulebook,
        {"m1:YES", "m1:NO"}
    );

    expect_true(result.ok(), "validation ok");
    expect_true(result.compiler_ready(), "compiler ready");
    expect_equal(result.unapproved_rules.size(), 0U, "unapproved count");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "RuleValidator_RejectsUnknownVariable",
            &RuleValidator_RejectsUnknownVariable
        },
        {
            "RuleValidator_RejectsDuplicateRuleId",
            &RuleValidator_RejectsDuplicateRuleId
        },
        {
            "RuleValidator_RejectsEmptyVariableSet",
            &RuleValidator_RejectsEmptyVariableSet
        },
        {
            "RuleValidator_ApprovedFixtureIsCompilerReady",
            &RuleValidator_ApprovedFixtureIsCompilerReady
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
