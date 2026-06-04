#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace trading_engine::strategy::market_making {

constexpr std::uint16_t kMaxQuoteLegs = 2;
constexpr std::int64_t kPriceOneTick = 1'000'000;

enum class QuoteIntentType : std::uint8_t {
    None,
    PlaceQuote,
    ReplaceQuote,
    PassiveUnwind,
    ForcedUnwind,
    CancelQuote,
    CancelOnly
};

enum class QuoteIntentRiskMode : std::uint8_t {
    Opening,
    Mixed,
    ReduceOnly,
    ForcedReduce
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
    Valid,
    MissingBidAsk,
    StaleBook,
    CrossedBook,
    LowConfidence,
    ExternalStale,
    Disabled
};

enum class FairValueSourceKind : std::uint8_t {
    Mid,
    MicropriceVwap,
    ComplementMarket,
    External,
    OracleBand,
    ConservativeFallback
};

enum class NoQuoteReason : std::uint8_t {
    None,
    MissingDepth,
    FairValueUnavailable,
    FairStaleBook,
    FairCrossedBook,
    FairMissingLiquidity,
    FairLowConfidence,
    FairExternalStale,
    FairUnavailableUnknown,
    QuoteSizeUnavailable,
    InvalidQuotePrices,
    NoQuoteSides,
    QuoteBuildFailed
};

inline constexpr std::size_t kNoQuoteReasonCount = 13;

[[nodiscard]] inline const char* fair_value_quality_name(
    FairValueQuality quality
) noexcept {
    switch (quality) {
        case FairValueQuality::Valid:
            return "valid";
        case FairValueQuality::MissingBidAsk:
            return "missing_bid_ask";
        case FairValueQuality::StaleBook:
            return "stale_book";
        case FairValueQuality::CrossedBook:
            return "crossed_book";
        case FairValueQuality::LowConfidence:
            return "low_confidence";
        case FairValueQuality::ExternalStale:
            return "external_stale";
        case FairValueQuality::Disabled:
            return "disabled";
    }
    return "unknown";
}

[[nodiscard]] inline const char* no_quote_reason_name(
    NoQuoteReason reason
) noexcept {
    switch (reason) {
        case NoQuoteReason::None:
            return "none";
        case NoQuoteReason::MissingDepth:
            return "missing_depth";
        case NoQuoteReason::FairValueUnavailable:
            return "fair_value_unavailable";
        case NoQuoteReason::FairStaleBook:
            return "fair_stale_book";
        case NoQuoteReason::FairCrossedBook:
            return "fair_crossed_book";
        case NoQuoteReason::FairMissingLiquidity:
            return "fair_missing_liquidity";
        case NoQuoteReason::FairLowConfidence:
            return "fair_low_confidence";
        case NoQuoteReason::FairExternalStale:
            return "fair_external_stale";
        case NoQuoteReason::FairUnavailableUnknown:
            return "fair_unavailable_unknown";
        case NoQuoteReason::QuoteSizeUnavailable:
            return "quote_size_unavailable";
        case NoQuoteReason::InvalidQuotePrices:
            return "invalid_quote_prices";
        case NoQuoteReason::NoQuoteSides:
            return "no_quote_sides";
        case NoQuoteReason::QuoteBuildFailed:
            return "quote_build_failed";
    }
    return "unknown";
}

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
