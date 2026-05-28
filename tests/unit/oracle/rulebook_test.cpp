#include "oracle/rules/RuleValidator.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

using trading_engine::oracle::Rulebook;
using trading_engine::oracle::RuleDraft;
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

std::filesystem::path fixture_path() {
    return std::filesystem::path{POLYTOPE_SOURCE_DIR} /
           "tests/fixtures/oracle/rulebook_small.json";
}

ValidatedRule approved_rule() {
    ValidatedRule rule;
    rule.rule_id = "r_exactly_one_m1";
    rule.type = RuleType::ExactlyOne;
    rule.variable_ids = {"m1:YES", "m1:NO"};
    rule.approved = true;
    rule.approved_by = "fixture";
    rule.approved_at_ns = 1;
    rule.source_rule_draft_id = "draft_1";
    return rule;
}

void RuleDraft_DefaultRequiresManualReview() {
    RuleDraft draft;
    expect_true(
        draft.requires_manual_review,
        "requires_manual_review default"
    );
}

void Rulebook_LoadsApprovedRules() {
    const auto loaded = Rulebook::load_json(fixture_path().string());
    expect_true(loaded.ok(), "load ok");
    expect_equal(loaded.rules.size(), 1U, "rule count");

    Rulebook rulebook;
    for (const auto& rule : loaded.rules) {
        rulebook.add_rule(rule);
    }

    expect_equal(rulebook.approved_rules().size(), 1U, "approved rules");
    expect_equal(rulebook.unapproved_rules().size(), 0U, "unapproved rules");
    expect_equal(
        rulebook.rules().front().variable_ids.front(),
        std::string{"m1:YES"},
        "variable id"
    );
}

void Rulebook_RejectsUnapprovedRulesForCompiler() {
    auto rule = approved_rule();
    rule.approved = false;

    Rulebook rulebook;
    rulebook.add_rule(rule);

    RuleValidator validator;
    const auto result = validator.validate_rulebook(
        rulebook,
        {"m1:YES", "m1:NO"}
    );

    expect_true(result.ok(), "structural validation ok");
    expect_false(result.compiler_ready(), "compiler ready");
    expect_equal(result.unapproved_rules.size(), 1U, "unapproved count");
    expect_equal(
        result.unapproved_rules.front(),
        std::string{"r_exactly_one_m1"},
        "unapproved rule id"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "RuleDraft_DefaultRequiresManualReview",
            &RuleDraft_DefaultRequiresManualReview
        },
        {"Rulebook_LoadsApprovedRules", &Rulebook_LoadsApprovedRules},
        {
            "Rulebook_RejectsUnapprovedRulesForCompiler",
            &Rulebook_RejectsUnapprovedRulesForCompiler
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
