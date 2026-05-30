#include "oracle/bundles/CombinatorialBundleGenerator.h"
#include "oracle/payoff/PayoutRule.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using trading_engine::oracle::CombinatorialBundleGenerator;
using trading_engine::oracle::PAYOUT_ONE_TICK;
using trading_engine::oracle::RawMarketRecord;
using trading_engine::oracle::RuleType;
using trading_engine::oracle::Rulebook;
using trading_engine::oracle::Side;
using trading_engine::oracle::ValidatedRule;

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

RawMarketRecord binary_market(
    const std::string& market_id,
    const std::string& yes_asset,
    const std::string& no_asset
) {
    RawMarketRecord record;
    record.market_id = market_id;
    record.event_id = "harvey";
    record.title = "Sentence bracket";
    record.description = "Exactly one sentence bracket resolves Yes.";
    record.outcomes = {"Yes", "No"};
    record.asset_ids = {yes_asset, no_asset};
    return record;
}

std::vector<RawMarketRecord> bracket_markets(std::size_t count) {
    std::vector<RawMarketRecord> markets;
    for (std::size_t i = 0; i < count; ++i) {
        markets.push_back(binary_market(
            "m" + std::to_string(i + 1),
            "asset_m" + std::to_string(i + 1) + "_yes",
            "asset_m" + std::to_string(i + 1) + "_no"
        ));
    }
    return markets;
}

ValidatedRule rule(
    std::string rule_id,
    RuleType type,
    const std::vector<std::string>& variable_ids
) {
    ValidatedRule out;
    out.rule_id = std::move(rule_id);
    out.type = type;
    out.variable_ids = variable_ids;
    out.approved = true;
    out.approved_by = "fixture";
    out.approved_at_ns = 1;
    return out;
}

Rulebook rulebook_with(const ValidatedRule& validated_rule) {
    Rulebook rulebook;
    rulebook.add_rule(validated_rule);
    return rulebook;
}

std::vector<std::string> yes_variables(std::size_t count) {
    std::vector<std::string> variables;
    for (std::size_t i = 0; i < count; ++i) {
        variables.push_back("m" + std::to_string(i + 1) + ":Yes");
    }
    return variables;
}

std::uint32_t distinct_market_count(
    const trading_engine::oracle::CandidateBundle& bundle
) {
    std::unordered_set<std::string> markets;
    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        markets.insert(bundle.legs[i].market_id);
    }
    return static_cast<std::uint32_t>(markets.size());
}

void CombinatorialBundleGenerator_ExactlyOneGeneratesYesAndNoBaskets() {
    CombinatorialBundleGenerator generator;
    const auto result = generator.generate_from_rulebook(
        bracket_markets(6),
        rulebook_with(rule("r_harvey", RuleType::ExactlyOne, yes_variables(6)))
    );

    expect_true(result.ok(), "generate ok");
    expect_equal(result.bundles.size(), 2U, "bundle count");

    const auto& yes_bundle = result.bundles[0];
    expect_equal(yes_bundle.leg_count, 6U, "yes leg count");
    expect_equal(distinct_market_count(yes_bundle), 6U, "yes markets");
    expect_equal(
        yes_bundle.guaranteed_payout_tick,
        PAYOUT_ONE_TICK,
        "yes guaranteed payout"
    );
    for (std::uint16_t i = 0; i < yes_bundle.leg_count; ++i) {
        expect_true(yes_bundle.legs[i].side == Side::Buy, "yes side");
        expect_true(
            yes_bundle.legs[i].asset_id.find("_yes") != std::string::npos,
            "yes asset"
        );
    }

    const auto& no_bundle = result.bundles[1];
    expect_equal(no_bundle.leg_count, 6U, "no leg count");
    expect_equal(distinct_market_count(no_bundle), 6U, "no markets");
    expect_equal(
        no_bundle.guaranteed_payout_tick,
        5 * PAYOUT_ONE_TICK,
        "no guaranteed payout"
    );
    for (std::uint16_t i = 0; i < no_bundle.leg_count; ++i) {
        expect_true(no_bundle.legs[i].side == Side::Buy, "no side");
        expect_true(
            no_bundle.legs[i].asset_id.find("_no") != std::string::npos,
            "no asset"
        );
    }
    expect_true(result.bundle_hash != 0, "bundle hash");
}

void CombinatorialBundleGenerator_AtMostOneGeneratesOnlyNoBasket() {
    CombinatorialBundleGenerator generator;
    const auto result = generator.generate_from_rulebook(
        bracket_markets(4),
        rulebook_with(rule("r_world_cup", RuleType::AtMostOne, yes_variables(4)))
    );

    expect_true(result.ok(), "generate ok");
    expect_equal(result.bundles.size(), 1U, "bundle count");

    const auto& bundle = result.bundles.front();
    expect_equal(bundle.leg_count, 4U, "leg count");
    expect_equal(distinct_market_count(bundle), 4U, "markets");
    expect_equal(
        bundle.guaranteed_payout_tick,
        3 * PAYOUT_ONE_TICK,
        "guaranteed payout"
    );
    for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
        expect_true(
            bundle.legs[i].asset_id.find("_no") != std::string::npos,
            "only no assets"
        );
    }
}

void CombinatorialBundleGenerator_DoesNotGenerateAtMostOneYesBasket() {
    CombinatorialBundleGenerator generator;
    const auto result = generator.generate_from_rulebook(
        bracket_markets(3),
        rulebook_with(rule("r_top_teams", RuleType::AtMostOne, yes_variables(3)))
    );

    expect_true(result.ok(), "generate ok");
    expect_equal(result.bundles.size(), 1U, "bundle count");
    for (std::uint16_t i = 0; i < result.bundles.front().leg_count; ++i) {
        expect_true(
            result.bundles.front().legs[i].asset_id.find("_yes") ==
                std::string::npos,
            "no yes basket"
        );
    }
}

void CombinatorialBundleGenerator_SkipsSingleMarketTrivialRule() {
    CombinatorialBundleGenerator generator;
    const auto result = generator.generate_from_rulebook(
        bracket_markets(1),
        rulebook_with(rule(
            "r_single_market",
            RuleType::ExactlyOne,
            {"m1:Yes", "m1:No"}
        ))
    );

    expect_true(result.ok(), "generate ok");
    expect_true(result.bundles.empty(), "no bundles");
    expect_true(!result.warnings.empty(), "warning");
}

void CombinatorialBundleGenerator_SkipsTooManyLegs() {
    CombinatorialBundleGenerator generator;
    const auto result = generator.generate_from_rulebook(
        bracket_markets(17),
        rulebook_with(rule("r_too_many", RuleType::AtMostOne, yes_variables(17)))
    );

    expect_true(result.ok(), "generate ok");
    expect_true(result.bundles.empty(), "no bundles");
    expect_true(!result.warnings.empty(), "warning");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "CombinatorialBundleGenerator_ExactlyOneGeneratesYesAndNoBaskets",
            &CombinatorialBundleGenerator_ExactlyOneGeneratesYesAndNoBaskets
        },
        {
            "CombinatorialBundleGenerator_AtMostOneGeneratesOnlyNoBasket",
            &CombinatorialBundleGenerator_AtMostOneGeneratesOnlyNoBasket
        },
        {
            "CombinatorialBundleGenerator_DoesNotGenerateAtMostOneYesBasket",
            &CombinatorialBundleGenerator_DoesNotGenerateAtMostOneYesBasket
        },
        {
            "CombinatorialBundleGenerator_SkipsSingleMarketTrivialRule",
            &CombinatorialBundleGenerator_SkipsSingleMarketTrivialRule
        },
        {
            "CombinatorialBundleGenerator_SkipsTooManyLegs",
            &CombinatorialBundleGenerator_SkipsTooManyLegs
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
