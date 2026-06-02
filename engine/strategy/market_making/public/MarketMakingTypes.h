#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace trading_engine::strategy::market_making {

constexpr std::uint16_t kMaxQuoteLegs = 2;
constexpr std::int64_t kPriceOneTick = 1'000'000;

enum class QuoteIntentType : std::uint8_t {
    None,
    PlaceQuote,
    ReplaceQuote,
    CancelQuote
};

enum class QuoteSide : std::uint8_t {
    Bid,
    Ask
};

enum class CancelReason : std::uint8_t {
    None,
    QuoteExpired,
    FairValueMoved,
    InventoryChanged,
    BookStale,
    MarketHalted,
    RiskDegraded,
    ReplacedByNewQuote
};

enum class QuoteStatus : std::uint8_t {
    Created,
    RiskApproved,
    PostedPaper,
    ActivePaper,
    PartiallyFilled,
    Filled,
    CancelRequested,
    Cancelled,
    Expired,
    Replaced,
    Rejected
};

enum class FairValueQuality : std::uint8_t {
    Good,
    MissingBidAsk,
    StaleBook,
    CrossedBook,
    LowConfidence,
    Disabled
};

enum class FairValueSourceKind : std::uint8_t {
    Mid,
    External,
    OracleBand,
    ConservativeFallback
};

[[nodiscard]] inline std::uint64_t fnv1a_mix(
    std::uint64_t hash,
    std::uint64_t value
) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
        hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffU);
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] inline std::uint64_t stable_hash_string(
    std::uint64_t hash,
    const std::string& value
) noexcept {
    for (const auto c : value) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] inline std::int64_t clamp_tick(
    std::int64_t value,
    std::int64_t min_value,
    std::int64_t max_value
) noexcept {
    return std::max(min_value, std::min(max_value, value));
}

}  // namespace trading_engine::strategy::market_making
