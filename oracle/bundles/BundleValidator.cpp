#include "oracle/bundles/BundleValidator.h"

#include <algorithm>
#include <unordered_set>

namespace trading_engine::oracle {

namespace {

bool masks_conflict(const CandidateBundle& bundle) noexcept {
    return ((bundle.required_true_mask & bundle.required_false_mask) != 0) ||
           ((bundle.required_true_mask & bundle.invalid_mask) != 0) ||
           ((bundle.required_false_mask & bundle.invalid_mask) != 0);
}

void add_error(
    std::vector<std::string>* errors,
    std::uint64_t bundle_id,
    const std::string& message
) {
    errors->push_back(
        "bundle " + std::to_string(bundle_id) + ": " + message
    );
}

}  // namespace

BundleValidationResult BundleValidator::validate(
    const std::vector<CandidateBundle>& bundles,
    const std::unordered_set<std::string>& known_market_ids,
    const std::unordered_set<std::string>& known_asset_ids
) const {
    BundleValidationResult result;
    std::unordered_set<std::uint64_t> seen_bundle_ids;

    for (const auto& bundle : bundles) {
        const auto [_, inserted] = seen_bundle_ids.insert(bundle.bundle_id);
        if (!inserted) {
            result.duplicate_bundle_ids.push_back(bundle.bundle_id);
            add_error(&result.errors, bundle.bundle_id, "duplicate bundle_id");
        }

        if (bundle.leg_count == 0) {
            add_error(&result.errors, bundle.bundle_id, "leg_count is zero");
        }
        if (bundle.leg_count > kMaxBundleLegs) {
            add_error(&result.errors, bundle.bundle_id, "leg_count exceeds 16");
        }
        if (masks_conflict(bundle)) {
            add_error(
                &result.errors,
                bundle.bundle_id,
                "required masks conflict"
            );
        }

        const auto limit = std::min<std::uint16_t>(
            bundle.leg_count,
            kMaxBundleLegs
        );
        for (std::uint16_t i = 0; i < limit; ++i) {
            const auto& leg = bundle.legs[i];
            if (leg.market_id.empty() ||
                known_market_ids.find(leg.market_id) == known_market_ids.end()) {
                add_error(
                    &result.errors,
                    bundle.bundle_id,
                    "unknown market_id in leg " + std::to_string(i)
                );
            }
            if (leg.asset_id.empty() ||
                known_asset_ids.find(leg.asset_id) == known_asset_ids.end()) {
                add_error(
                    &result.errors,
                    bundle.bundle_id,
                    "unknown asset_id in leg " + std::to_string(i)
                );
            }
            if (leg.quantity_lots <= 0) {
                add_error(
                    &result.errors,
                    bundle.bundle_id,
                    "non-positive quantity_lots in leg " + std::to_string(i)
                );
            }
            if (leg.max_price_tick < 0) {
                add_error(
                    &result.errors,
                    bundle.bundle_id,
                    "negative max_price_tick in leg " + std::to_string(i)
                );
            }
        }
    }

    return result;
}

}  // namespace trading_engine::oracle
