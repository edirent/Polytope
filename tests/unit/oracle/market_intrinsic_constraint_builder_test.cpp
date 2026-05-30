#include "oracle/compiler/MarketIntrinsicConstraintBuilder.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::oracle::BooleanVariable;
using trading_engine::oracle::ConstraintOp;
using trading_engine::oracle::MarketIntrinsicConstraintBuilder;
using trading_engine::oracle::MarketUniverse;
using trading_engine::oracle::RawMarketRecord;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
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

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

RawMarketRecord market(std::string market_id) {
    RawMarketRecord out;
    out.market_id = std::move(market_id);
    out.event_id = "event";
    out.outcomes = {"Yes", "No"};
    out.asset_ids = {out.market_id + "_yes", out.market_id + "_no"};
    return out;
}

std::vector<BooleanVariable> variables_for(
    const std::vector<RawMarketRecord>& markets
) {
    std::vector<BooleanVariable> out;
    for (const auto& item : markets) {
        for (std::size_t i = 0; i < item.outcomes.size(); ++i) {
            out.push_back(BooleanVariable{
                .var_id = static_cast<std::uint32_t>(out.size()),
                .variable_key = item.market_id + ":" + item.outcomes[i],
                .market_id = item.market_id,
                .outcome_id = item.outcomes[i],
                .asset_id = item.asset_ids[i]
            });
        }
    }
    return out;
}

std::string signature_for(
    const std::vector<trading_engine::oracle::LinearBooleanConstraint>& constraints
) {
    std::string out;
    for (const auto& constraint : constraints) {
        out += std::to_string(static_cast<int>(constraint.op)) + ":";
        out += std::to_string(constraint.rhs) + ":";
        for (const auto var_id : constraint.var_ids) {
            out += std::to_string(var_id) + ",";
        }
        out += ";";
    }
    return out;
}

void BinaryMarket_GeneratesYesNoExactlyOne() {
    MarketUniverse universe;
    universe.markets = {market("m1")};

    MarketIntrinsicConstraintBuilder builder;
    const auto constraints = builder.build(
        universe,
        variables_for(universe.markets)
    );

    expect_equal(constraints.size(), 1U, "constraint count");
    const auto& constraint = constraints.front();
    expect_equal(constraint.op, ConstraintOp::Equal, "op");
    expect_equal(constraint.rhs, 1, "rhs");
    expect_equal(constraint.var_ids.size(), 2U, "var ids");
    expect_equal(constraint.coeffs, std::vector<std::int32_t>{1, 1}, "coeffs");
}

void WorldCup_96AssetsAdds48ComplementConstraints() {
    MarketUniverse universe;
    for (std::uint32_t i = 0; i < 48; ++i) {
        universe.markets.push_back(market("world_cup_" + std::to_string(i)));
    }

    MarketIntrinsicConstraintBuilder builder;
    const auto constraints = builder.build(
        universe,
        variables_for(universe.markets)
    );

    expect_equal(constraints.size(), 48U, "constraint count");
    for (const auto& constraint : constraints) {
        expect_equal(constraint.op, ConstraintOp::Equal, "op");
        expect_equal(constraint.rhs, 1, "rhs");
        expect_equal(constraint.var_ids.size(), 2U, "var ids");
    }
}

void ComplementConstraintsAreDeterministic() {
    MarketUniverse left;
    left.markets = {market("m2"), market("m1")};
    auto right = left;
    std::reverse(right.markets.begin(), right.markets.end());

    const auto variables = variables_for(left.markets);
    MarketIntrinsicConstraintBuilder builder;
    const auto a = builder.build(left, variables);
    const auto b = builder.build(right, variables);

    expect_equal(signature_for(a), signature_for(b), "constraint signature");
    expect_true(!signature_for(a).empty(), "signature nonempty");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "BinaryMarket_GeneratesYesNoExactlyOne",
            &BinaryMarket_GeneratesYesNoExactlyOne
        },
        {
            "WorldCup_96AssetsAdds48ComplementConstraints",
            &WorldCup_96AssetsAdds48ComplementConstraints
        },
        {
            "ComplementConstraintsAreDeterministic",
            &ComplementConstraintsAreDeterministic
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

    for (const auto& [name, _] : tests()) {
        const auto status = run_test(name);
        if (status != 0) {
            return status;
        }
    }
    return 0;
}
