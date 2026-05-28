#include "oracle/bundles/BundleValidator.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

using trading_engine::oracle::BundleValidator;
using trading_engine::oracle::CandidateBundle;
using trading_engine::oracle::Side;
using trading_engine::oracle::kMaxBundleLegs;

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

std::unordered_set<std::string> known_markets() {
    return {"m1"};
}

std::unordered_set<std::string> known_assets() {
    return {"asset_yes", "asset_no"};
}

CandidateBundle valid_bundle(std::uint64_t bundle_id = 1) {
    CandidateBundle bundle;
    bundle.bundle_id = bundle_id;
    bundle.required_true_mask = 1;
    bundle.guaranteed_payout_tick = 1'000'000;
    bundle.leg_count = 2;
    bundle.legs[0].market_id = "m1";
    bundle.legs[0].asset_id = "asset_yes";
    bundle.legs[0].side = Side::Buy;
    bundle.legs[0].quantity_lots = 1;
    bundle.legs[0].max_price_tick = 500'000;
    bundle.legs[1].market_id = "m1";
    bundle.legs[1].asset_id = "asset_no";
    bundle.legs[1].side = Side::Buy;
    bundle.legs[1].quantity_lots = 1;
    bundle.legs[1].max_price_tick = 500'000;
    return bundle;
}

void BundleValidator_AcceptsValidBundle() {
    BundleValidator validator;
    const auto result = validator.validate(
        {valid_bundle()},
        known_markets(),
        known_assets()
    );

    expect_true(result.ok(), "validation ok");
}

void BundleValidator_RejectsUnknownAsset() {
    auto bundle = valid_bundle();
    bundle.legs[0].asset_id = "unknown_asset";

    BundleValidator validator;
    const auto result = validator.validate(
        {bundle},
        known_markets(),
        known_assets()
    );

    expect_false(result.ok(), "validation ok");
    expect_false(result.errors.empty(), "errors");
}

void BundleValidator_RejectsZeroLegs() {
    auto bundle = valid_bundle();
    bundle.leg_count = 0;

    BundleValidator validator;
    const auto result = validator.validate(
        {bundle},
        known_markets(),
        known_assets()
    );

    expect_false(result.ok(), "validation ok");
    expect_false(result.errors.empty(), "errors");
}

void BundleValidator_RejectsTooManyLegs() {
    auto bundle = valid_bundle();
    bundle.leg_count = kMaxBundleLegs + 1;

    BundleValidator validator;
    const auto result = validator.validate(
        {bundle},
        known_markets(),
        known_assets()
    );

    expect_false(result.ok(), "validation ok");
    expect_false(result.errors.empty(), "errors");
}

void BundleValidator_RejectsConflictingMasks() {
    auto bundle = valid_bundle();
    bundle.required_true_mask = 1;
    bundle.required_false_mask = 1;

    BundleValidator validator;
    const auto result = validator.validate(
        {bundle},
        known_markets(),
        known_assets()
    );

    expect_false(result.ok(), "validation ok");
    expect_false(result.errors.empty(), "errors");
}

void BundleValidator_RejectsDuplicateBundleId() {
    BundleValidator validator;
    const auto result = validator.validate(
        {valid_bundle(1), valid_bundle(1)},
        known_markets(),
        known_assets()
    );

    expect_false(result.ok(), "validation ok");
    expect_false(result.duplicate_bundle_ids.empty(), "duplicates");
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {"BundleValidator_AcceptsValidBundle", &BundleValidator_AcceptsValidBundle},
        {"BundleValidator_RejectsUnknownAsset", &BundleValidator_RejectsUnknownAsset},
        {"BundleValidator_RejectsZeroLegs", &BundleValidator_RejectsZeroLegs},
        {"BundleValidator_RejectsTooManyLegs", &BundleValidator_RejectsTooManyLegs},
        {
            "BundleValidator_RejectsConflictingMasks",
            &BundleValidator_RejectsConflictingMasks
        },
        {
            "BundleValidator_RejectsDuplicateBundleId",
            &BundleValidator_RejectsDuplicateBundleId
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
