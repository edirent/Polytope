#include "oracle/payoff/PayoffMatrixBuilder.h"

#include "oracle/payoff/PayoutRule.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

using trading_engine::oracle::BooleanVariable;
using trading_engine::oracle::FeasibleState;
using trading_engine::oracle::PAYOUT_ONE_TICK;
using trading_engine::oracle::PayoffMatrix;
using trading_engine::oracle::PayoffMatrixBuilder;

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

std::vector<BooleanVariable> variables() {
    return {
        BooleanVariable{
            .var_id = 0,
            .variable_key = "m1:YES",
            .market_id = "m1",
            .outcome_id = "YES",
            .asset_id = "asset_yes"
        },
        BooleanVariable{
            .var_id = 1,
            .variable_key = "m1:NO",
            .market_id = "m1",
            .outcome_id = "NO",
            .asset_id = "asset_no"
        }
    };
}

std::vector<FeasibleState> feasible_states() {
    return {
        FeasibleState{1, {1}},
        FeasibleState{2, {2}}
    };
}

std::uint32_t asset_index(
    const PayoffMatrix& matrix,
    const std::string& asset_id
) {
    for (std::uint32_t i = 0; i < matrix.asset_ids.size(); ++i) {
        if (matrix.asset_ids[i] == asset_id) {
            return i;
        }
    }
    fail("missing asset: " + asset_id);
}

std::int64_t payout_for(
    const PayoffMatrix& matrix,
    std::uint64_t state_id,
    const std::string& asset_id
) {
    const auto index = asset_index(matrix, asset_id);
    for (const auto& entry : matrix.entries) {
        if (entry.state_id == state_id && entry.asset_index == index) {
            return entry.payout_tick;
        }
    }
    fail("missing payoff entry");
}

PayoffMatrix build_fixture_matrix() {
    PayoffMatrixBuilder builder;
    const auto result = builder.build(variables(), feasible_states());
    expect_true(result.ok(), "build ok");
    return result.matrix;
}

void PayoffMatrixBuilder_BinaryOutcomeTruePaysOne() {
    const auto matrix = build_fixture_matrix();

    expect_equal(
        payout_for(matrix, 1, "asset_yes"),
        PAYOUT_ONE_TICK,
        "yes pays in state 1"
    );
    expect_equal(
        payout_for(matrix, 2, "asset_no"),
        PAYOUT_ONE_TICK,
        "no pays in state 2"
    );
}

void PayoffMatrixBuilder_BinaryOutcomeFalsePaysZero() {
    const auto matrix = build_fixture_matrix();

    expect_equal(
        payout_for(matrix, 1, "asset_no"),
        0LL,
        "no pays zero in state 1"
    );
    expect_equal(
        payout_for(matrix, 2, "asset_yes"),
        0LL,
        "yes pays zero in state 2"
    );
}

void PayoffMatrixBuilder_HasRowsForAllStates() {
    const auto matrix = build_fixture_matrix();

    expect_equal(matrix.row_count, 2U, "row count");
    expect_equal(matrix.state_ids.size(), 2U, "state id count");
    expect_equal(matrix.state_ids[0], 1ULL, "state 0");
    expect_equal(matrix.state_ids[1], 2ULL, "state 1");
}

void PayoffMatrixBuilder_HasColumnsForAllAssets() {
    const auto matrix = build_fixture_matrix();

    expect_equal(matrix.column_count, 2U, "column count");
    expect_equal(matrix.asset_ids.size(), 2U, "asset id count");
    expect_equal(matrix.entries.size(), 4U, "entry count");
    asset_index(matrix, "asset_yes");
    asset_index(matrix, "asset_no");
}

void PayoffMatrixBuilder_RejectsUnknownAsset() {
    auto bad_variables = variables();
    bad_variables[0].asset_id.clear();

    PayoffMatrixBuilder builder;
    const auto result = builder.build(bad_variables, feasible_states());

    expect_false(result.ok(), "build ok");
    expect_false(result.errors.empty(), "errors");
}

void PayoffMatrixBuilder_DeterministicHash() {
    PayoffMatrixBuilder builder;
    auto reversed_variables = variables();
    std::swap(reversed_variables[0], reversed_variables[1]);

    const auto a = builder.build(variables(), feasible_states());
    const auto b = builder.build(reversed_variables, feasible_states());

    expect_true(a.ok(), "build a ok");
    expect_true(b.ok(), "build b ok");
    expect_equal(a.matrix.payoff_hash, b.matrix.payoff_hash, "hash");
    expect_true(a.matrix.payoff_hash != 0, "hash nonzero");
}

void PayoffMatrixBuilder_WritesArtifacts() {
    PayoffMatrixBuilder builder;
    const auto result = builder.build(variables(), feasible_states());
    expect_true(result.ok(), "build ok");

    const auto out_dir =
        std::filesystem::temp_directory_path() /
        "oracle_payoff_matrix_artifacts";
    std::filesystem::remove_all(out_dir);

    std::vector<std::string> errors;
    expect_true(
        builder.write(result.matrix, out_dir.string(), &errors),
        "write artifacts"
    );
    expect_true(errors.empty(), "write errors");
    expect_true(
        std::filesystem::exists(out_dir / "payoff_matrix.bin"),
        "payoff_matrix.bin"
    );
    expect_true(
        std::filesystem::exists(out_dir / "payoff_matrix.debug.json"),
        "payoff_matrix.debug.json"
    );

    std::filesystem::remove_all(out_dir);
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "PayoffMatrixBuilder_BinaryOutcomeTruePaysOne",
            &PayoffMatrixBuilder_BinaryOutcomeTruePaysOne
        },
        {
            "PayoffMatrixBuilder_BinaryOutcomeFalsePaysZero",
            &PayoffMatrixBuilder_BinaryOutcomeFalsePaysZero
        },
        {
            "PayoffMatrixBuilder_HasRowsForAllStates",
            &PayoffMatrixBuilder_HasRowsForAllStates
        },
        {
            "PayoffMatrixBuilder_HasColumnsForAllAssets",
            &PayoffMatrixBuilder_HasColumnsForAllAssets
        },
        {
            "PayoffMatrixBuilder_RejectsUnknownAsset",
            &PayoffMatrixBuilder_RejectsUnknownAsset
        },
        {
            "PayoffMatrixBuilder_DeterministicHash",
            &PayoffMatrixBuilder_DeterministicHash
        },
        {
            "PayoffMatrixBuilder_WritesArtifacts",
            &PayoffMatrixBuilder_WritesArtifacts
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
