#include "oracle/rules/ManualRuleEditor.h"
#include "oracle/rules/RuleValidator.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

using trading_engine::oracle::ManualRuleEditor;
using trading_engine::oracle::RuleDraft;
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

ValidatedRule rule(
    std::string id,
    bool approved
) {
    ValidatedRule out;
    out.rule_id = std::move(id);
    out.type = RuleType::ExactlyOne;
    out.variable_ids = {"m1:YES", "m1:NO"};
    out.approved = approved;
    out.approved_by = approved ? "fixture" : "";
    out.approved_at_ns = approved ? 1 : 0;
    out.source_rule_draft_id = "draft_1";
    return out;
}

void ManualRuleEditor_RoundTripsRulebook() {
    Rulebook rulebook;
    rulebook.add_rule(rule("approved", true));
    rulebook.add_rule(rule("draft_only", false));

    ManualRuleEditor editor;
    const auto unapproved = editor.list_unapproved_rules(rulebook);
    expect_equal(unapproved.size(), 1U, "unapproved count");
    expect_equal(
        unapproved.front().rule_id,
        std::string{"draft_only"},
        "unapproved id"
    );

    const auto out_path =
        std::filesystem::temp_directory_path() /
        "oracle_approved_rulebook_roundtrip.json";
    std::vector<std::string> errors;
    expect_true(
        editor.write_approved_rulebook(rulebook, out_path.string(), &errors),
        "write approved rulebook"
    );
    expect_true(errors.empty(), "write errors");

    const auto loaded = editor.load_rulebook(out_path.string());
    expect_true(loaded.ok(), "load ok");
    expect_equal(loaded.rules.size(), 1U, "loaded rule count");
    expect_equal(loaded.rules.front().rule_id, std::string{"approved"}, "id");
    expect_true(loaded.rules.front().approved, "approved");

    Rulebook approved_only;
    for (const auto& loaded_rule : loaded.rules) {
        approved_only.add_rule(loaded_rule);
    }

    RuleValidator validator;
    const auto validation = validator.validate_rulebook(
        approved_only,
        {"m1:YES", "m1:NO"}
    );
    expect_true(validation.compiler_ready(), "compiler ready");

    std::filesystem::remove(out_path);
}

void ManualRuleEditor_RoundTripsRuleDrafts() {
    RuleDraft draft;
    draft.rule_id = "draft_exactly_one_m1";
    draft.type = RuleType::ExactlyOne;
    draft.variable_ids = {"m1:YES", "m1:NO"};
    draft.source_text = "fixture text";
    draft.rationale = "binary market";
    draft.requires_manual_review = true;

    ManualRuleEditor editor;
    const auto out_path =
        std::filesystem::temp_directory_path() /
        "oracle_rule_drafts_roundtrip.json";
    std::vector<std::string> errors;
    expect_true(
        editor.write_rule_drafts({draft}, out_path.string(), &errors),
        "write drafts"
    );
    expect_true(errors.empty(), "write errors");

    const auto loaded = editor.load_rule_drafts(out_path.string());
    expect_true(loaded.ok(), "load drafts");
    expect_equal(loaded.drafts.size(), 1U, "draft count");
    expect_equal(loaded.drafts.front().rule_id, draft.rule_id, "draft id");
    expect_true(
        loaded.drafts.front().requires_manual_review,
        "manual review"
    );

    std::filesystem::remove(out_path);
}

void ManualRuleEditor_ApprovesDraftsIntoRulebook() {
    RuleDraft draft;
    draft.rule_id = "draft_exactly_one_m1";
    draft.type = RuleType::ExactlyOne;
    draft.variable_ids = {"m1:YES", "m1:NO"};
    draft.source_text = "fixture text";
    draft.rationale = "binary market";

    ManualRuleEditor editor;
    auto rulebook = editor.approve_drafts({draft}, "human", 123);
    expect_equal(rulebook.rules().size(), 1U, "rule count");
    const auto& rule = rulebook.rules().front();
    expect_true(rule.approved, "approved");
    expect_equal(rule.approved_by, std::string{"human"}, "approved by");
    expect_equal(rule.approved_at_ns, 123ULL, "approved at");
    expect_equal(
        rule.source_rule_draft_id,
        std::string{"draft_exactly_one_m1"},
        "source draft"
    );

    RuleValidator validator;
    const auto validation = validator.validate_rulebook(
        rulebook,
        {"m1:YES", "m1:NO"}
    );
    expect_true(validation.compiler_ready(), "compiler ready");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "ManualRuleEditor_RoundTripsRulebook",
            &ManualRuleEditor_RoundTripsRulebook
        },
        {
            "ManualRuleEditor_RoundTripsRuleDrafts",
            &ManualRuleEditor_RoundTripsRuleDrafts
        },
        {
            "ManualRuleEditor_ApprovesDraftsIntoRulebook",
            &ManualRuleEditor_ApprovesDraftsIntoRulebook
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
