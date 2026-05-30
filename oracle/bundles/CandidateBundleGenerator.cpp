#include "oracle/bundles/CandidateBundleGenerator.h"

#include "oracle/bundles/BundleHash.h"
#include "oracle/bundles/BundleValidator.h"
#include "oracle/payoff/PayoutRule.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>

namespace trading_engine::oracle {

namespace {

namespace json = boost::json;

std::uint64_t u64_field(
    const json::object& object,
    const char* name
) {
    const auto it = object.find(name);
    if (it == object.end() || it->value().is_null()) {
        return 0;
    }
    if (it->value().is_uint64()) {
        return it->value().as_uint64();
    }
    if (it->value().is_int64() && it->value().as_int64() >= 0) {
        return static_cast<std::uint64_t>(it->value().as_int64());
    }
    return 0;
}

std::int64_t i64_field(
    const json::object& object,
    const char* name
) {
    const auto it = object.find(name);
    if (it == object.end() || it->value().is_null()) {
        return 0;
    }
    if (it->value().is_int64()) {
        return it->value().as_int64();
    }
    if (it->value().is_uint64() &&
        it->value().as_uint64() <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(it->value().as_uint64());
    }
    return 0;
}

std::string string_field(
    const json::object& object,
    const char* name
) {
    const auto it = object.find(name);
    if (it == object.end() || !it->value().is_string()) {
        return {};
    }
    return json::value_to<std::string>(it->value());
}

BundleLeg parse_leg(
    const json::object& object,
    std::vector<std::string>* errors
) {
    BundleLeg leg;
    leg.market_id = string_field(object, "market_id");
    leg.asset_id = string_field(object, "asset_id");
    leg.quantity_lots = i64_field(object, "quantity_lots");
    leg.max_price_tick = i64_field(object, "max_price_tick");

    const std::string side_text = string_field(object, "side");
    if (!side_from_string(side_text, &leg.side)) {
        errors->push_back("unknown side: " + side_text);
    }

    return leg;
}

CandidateBundle parse_bundle(
    const json::object& object,
    std::size_t index,
    CandidateBundleLoadResult* result
) {
    CandidateBundle bundle;
    bundle.bundle_id = u64_field(object, "bundle_id");
    bundle.required_true_mask = u64_field(object, "required_true_mask");
    bundle.required_false_mask = u64_field(object, "required_false_mask");
    bundle.invalid_mask = u64_field(object, "invalid_mask");
    bundle.guaranteed_payout_tick =
        i64_field(object, "guaranteed_payout_tick");
    bundle.min_edge_tick = i64_field(object, "min_edge_tick");

    const auto legs_it = object.find("legs");
    if (legs_it == object.end() || !legs_it->value().is_array()) {
        result->errors.push_back(
            "bundles[" + std::to_string(index) + "]: missing legs array"
        );
        return bundle;
    }

    const auto& legs = legs_it->value().as_array();
    bundle.leg_count = static_cast<std::uint16_t>(
        std::min<std::size_t>(legs.size(), kMaxBundleLegs)
    );
    if (legs.size() > kMaxBundleLegs) {
        result->errors.push_back(
            "bundles[" + std::to_string(index) + "]: too many legs"
        );
    }

    for (std::size_t i = 0; i < bundle.leg_count; ++i) {
        if (!legs[i].is_object()) {
            result->errors.push_back(
                "bundles[" + std::to_string(index) + "].legs[" +
                std::to_string(i) + "]: expected object"
            );
            continue;
        }
        bundle.legs[i] = parse_leg(legs[i].as_object(), &result->errors);
    }

    return bundle;
}

json::object leg_to_json(const BundleLeg& leg) {
    json::object object;
    object["market_id"] = leg.market_id;
    object["asset_id"] = leg.asset_id;
    object["side"] = side_to_string(leg.side);
    object["quantity_lots"] = leg.quantity_lots;
    object["max_price_tick"] = leg.max_price_tick;
    return object;
}

json::object bundle_to_json(const CandidateBundle& bundle) {
    json::array legs;
    for (std::uint16_t i = 0; i < bundle.leg_count && i < kMaxBundleLegs; ++i) {
        legs.push_back(leg_to_json(bundle.legs[i]));
    }

    json::object object;
    object["bundle_id"] = bundle.bundle_id;
    object["required_true_mask"] = bundle.required_true_mask;
    object["required_false_mask"] = bundle.required_false_mask;
    object["invalid_mask"] = bundle.invalid_mask;
    object["guaranteed_payout_tick"] = bundle.guaranteed_payout_tick;
    object["min_edge_tick"] = bundle.min_edge_tick;
    object["legs"] = std::move(legs);
    return object;
}

std::string lower_copy(std::string value) {
    for (auto& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool has_ambiguous_split_resolution(const RawMarketRecord& market) {
    const auto text = lower_copy(
        market.title + "\n" + market.description + "\n" +
        market.resolution_source
    );
    return text.find("50-50") != std::string::npos ||
           text.find("50/50") != std::string::npos ||
           text.find("split") != std::string::npos ||
           text.find("proportion") != std::string::npos;
}

}  // namespace

CandidateBundleLoadResult CandidateBundleGenerator::load_fixture(
    const std::string& path,
    const std::unordered_set<std::string>& known_market_ids,
    const std::unordered_set<std::string>& known_asset_ids
) const {
    CandidateBundleLoadResult result;

    std::ifstream input(path);
    if (!input) {
        result.errors.push_back("failed to open candidate bundle fixture: " + path);
        return result;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();

    boost::json::error_code error;
    const auto parsed = json::parse(buffer.str(), error);
    if (error || !parsed.is_object()) {
        result.errors.push_back("malformed candidate bundle JSON");
        return result;
    }

    const auto& root = parsed.as_object();
    const auto bundles_it = root.find("bundles");
    if (bundles_it == root.end() || !bundles_it->value().is_array()) {
        result.errors.push_back("missing bundles array");
        return result;
    }

    const auto& bundles = bundles_it->value().as_array();
    for (std::size_t i = 0; i < bundles.size(); ++i) {
        if (!bundles[i].is_object()) {
            result.errors.push_back(
                "bundles[" + std::to_string(i) + "]: expected object"
            );
            continue;
        }
        result.bundles.push_back(
            parse_bundle(bundles[i].as_object(), i, &result)
        );
    }

    if (!result.errors.empty()) {
        return result;
    }

    BundleValidator validator;
    const auto validation = validator.validate(
        result.bundles,
        known_market_ids,
        known_asset_ids
    );
    result.errors.insert(
        result.errors.end(),
        validation.errors.begin(),
        validation.errors.end()
    );
    if (!result.errors.empty()) {
        return result;
    }

    result.bundle_hash = hash_candidate_bundles(result.bundles);
    return result;
}

CandidateBundleLoadResult CandidateBundleGenerator::generate_buy_all_outcomes(
    const std::vector<RawMarketRecord>& markets,
    const std::unordered_set<std::string>& known_market_ids,
    const std::unordered_set<std::string>& known_asset_ids
) const {
    CandidateBundleLoadResult result;
    std::uint64_t next_bundle_id = 1;

    for (const auto& market : markets) {
        if (market.market_id.empty()) {
            result.warnings.push_back("skipping market with empty market_id");
            continue;
        }
        if (market.outcomes.size() < 2 ||
            market.outcomes.size() != market.asset_ids.size()) {
            result.warnings.push_back(
                "skipping market without complete outcome/token mapping: " +
                market.market_id
            );
            continue;
        }
        if (market.asset_ids.size() > kMaxBundleLegs) {
            result.warnings.push_back(
                "skipping market with too many outcomes: " + market.market_id
            );
            continue;
        }
        if (has_ambiguous_split_resolution(market)) {
            result.warnings.push_back(
                "skipping market with split/proportional resolution text: " +
                market.market_id
            );
            continue;
        }

        CandidateBundle bundle;
        bundle.bundle_id = next_bundle_id++;
        bundle.guaranteed_payout_tick = PAYOUT_ONE_TICK;
        bundle.min_edge_tick = 0;
        bundle.required_true_mask = 0;
        bundle.required_false_mask = 0;
        bundle.invalid_mask = 0;
        bundle.leg_count = static_cast<std::uint16_t>(market.asset_ids.size());

        for (std::uint16_t i = 0; i < bundle.leg_count; ++i) {
            bundle.legs[i].market_id = market.market_id;
            bundle.legs[i].asset_id = market.asset_ids[i];
            bundle.legs[i].side = Side::Buy;
            bundle.legs[i].quantity_lots = 1;
            bundle.legs[i].max_price_tick = PAYOUT_ONE_TICK;
        }

        result.bundles.push_back(std::move(bundle));
    }

    BundleValidator validator;
    const auto validation = validator.validate(
        result.bundles,
        known_market_ids,
        known_asset_ids
    );
    result.errors.insert(
        result.errors.end(),
        validation.errors.begin(),
        validation.errors.end()
    );
    if (!result.errors.empty()) {
        return result;
    }

    result.bundle_hash = hash_candidate_bundles(result.bundles);
    return result;
}

bool CandidateBundleGenerator::export_fixture_artifact(
    const std::vector<CandidateBundle>& bundles,
    const std::string& path,
    std::vector<std::string>* errors
) const {
    std::vector<std::string> local_errors;
    auto* out_errors = errors ? errors : &local_errors;

    json::array bundle_values;
    for (const auto& bundle : bundles) {
        bundle_values.push_back(bundle_to_json(bundle));
    }

    json::object root;
    root["bundle_hash"] = hash_candidate_bundles(bundles);
    root["bundles"] = std::move(bundle_values);

    std::ofstream output(path);
    if (!output) {
        out_errors->push_back("failed to open bundle artifact: " + path);
        return false;
    }

    output << json::serialize(root) << '\n';
    if (!output) {
        out_errors->push_back("failed to write bundle artifact: " + path);
        return false;
    }

    return true;
}

}  // namespace trading_engine::oracle
