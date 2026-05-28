#include "oracle/bundles/BundleHash.h"

namespace trading_engine::oracle {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hash_byte(std::uint64_t* hash, std::uint8_t value) noexcept {
    *hash ^= value;
    *hash *= kFnvPrime;
}

void hash_u16(std::uint64_t* hash, std::uint16_t value) noexcept {
    for (int shift = 0; shift < 16; shift += 8) {
        hash_byte(hash, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void hash_u64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        hash_byte(hash, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void hash_i64(std::uint64_t* hash, std::int64_t value) noexcept {
    hash_u64(hash, static_cast<std::uint64_t>(value));
}

void hash_string(std::uint64_t* hash, const std::string& value) noexcept {
    for (unsigned char c : value) {
        hash_byte(hash, c);
    }
    hash_byte(hash, 0xffU);
}

}  // namespace

std::uint64_t hash_candidate_bundle(
    const CandidateBundle& bundle
) noexcept {
    std::uint64_t hash = kFnvOffset;

    hash_u64(&hash, bundle.bundle_id);
    hash_u64(&hash, bundle.required_true_mask);
    hash_u64(&hash, bundle.required_false_mask);
    hash_u64(&hash, bundle.invalid_mask);
    hash_i64(&hash, bundle.guaranteed_payout_tick);
    hash_u16(&hash, bundle.leg_count);

    for (std::uint16_t i = 0; i < bundle.leg_count && i < kMaxBundleLegs; ++i) {
        const auto& leg = bundle.legs[i];
        hash_string(&hash, leg.market_id);
        hash_string(&hash, leg.asset_id);
        hash_byte(&hash, static_cast<std::uint8_t>(leg.side));
        hash_i64(&hash, leg.quantity_lots);
        hash_i64(&hash, leg.max_price_tick);
    }

    hash_i64(&hash, bundle.min_edge_tick);
    return hash;
}

std::uint64_t hash_candidate_bundles(
    const std::vector<CandidateBundle>& bundles
) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (const auto& bundle : bundles) {
        hash_u64(&hash, hash_candidate_bundle(bundle));
    }
    return hash;
}

}  // namespace trading_engine::oracle
