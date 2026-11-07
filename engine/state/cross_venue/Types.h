#pragma once

#include "engine/signal/public/OpportunityIntent.h"
#include "engine/state/view/MarketDepthView.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace trading_engine::cross_venue {

using Money = std::int64_t;  // millionths of a currency unit; never a double
constexpr Money unit = 1'000'000;
constexpr std::int64_t max_quantity = 1'000'000'000;

Money money(std::string_view decimal);
std::string decimal(Money value);
Money add(Money a, Money b);
Money multiply(Money a, std::int64_t b);
std::string sha256(std::string_view bytes);
bool valid_sha256(const std::string& value);
std::uint64_t compact_hash(std::string_view bytes);
std::string field(std::string_view value);  // unambiguous length-prefixed encoding
void require(bool condition, std::string_view reason);

enum class VenueId { Kalshi, PolymarketInternational, PolymarketUS };
std::string venue_name(VenueId venue);
VenueId venue_id(std::string_view name);

struct VenueInstrumentKey {
    VenueId venue = VenueId::Kalshi;
    std::string market_id;
    std::string instrument_id;
    std::string currency;
    std::string identity() const;
    bool operator==(const VenueInstrumentKey&) const = default;
};

struct PayoffState {
    std::string name;
    Money yes = 0;
    Money no = 0;
};

struct ApprovedPair {
    std::string pair_id;
    VenueInstrumentKey leg_yes, leg_no;
    std::string rules_hash_yes, rules_hash_no;
    std::string review_id;
    std::uint64_t approval_revision = 0;
    bool enabled = false;
    bool synthetic = true;
    std::vector<PayoffState> payoffs;
    std::string artifact_hash;
};
std::string pair_hash(const ApprovedPair& pair);
void validate_pair(const ApprovedPair& pair);

struct BookProvenance {
    std::uint64_t recv_monotonic_ns = 0;
    std::uint64_t source_ts_ns = 0;  // source clock; never compared to local monotonic
    std::uint64_t snapshot_version = 0;
    std::uint64_t sequence = 0;
    std::uint64_t raw_log_offset = 0;
    std::string raw_hash;
    bool seq_gap = false, stale = true, halted = false;
    std::uint64_t recv_wall_ns = 0, last_authoritative_snapshot_ns = 0, last_liveness_ns = 0;
    std::uint64_t stream_sid = 0, exchange_clock = 0, connection_epoch = 0;
    bool authoritative_snapshot = false;
    std::uint64_t last_book_change_ns = 0, content_hash = 0;
    std::string health_reason;
};

// Internal replay protocol, NOT a claimed wire protocol for either exchange.
struct BookUpdate {
    VenueInstrumentKey key;
    enum class Kind { Snapshot, Delta, Disconnect, Halt } kind = Kind::Snapshot;
    BookProvenance evidence;
    std::vector<std::pair<Money, std::int64_t>> bids, asks;
    bool complete = true;
    bool subscription_sequence_validated = false;
    bool fractional_quantities = false;  // level quantities in millionths of one share
    bool bounded_prefix = false;  // exact best known prefix; raw capture retains full depth
};

struct VersionedBook {
    VenueInstrumentKey key;
    state::MarketDepthView depth;
    BookProvenance evidence;
    bool fractional_quantities = false;
    bool bounded_prefix = false;
    std::array<std::int64_t, state::kMaxSnapshotDepth> bid_qty_micro{}, ask_qty_micro{};
};

// Explicit TOTAL fee/slippage budgets for a quantity, scoped to the pair and
// artifact. No universal exchange fee formula is embedded in V0.
struct FeeQuote {
    std::int64_t qty = 0;
    std::array<Money, 2> fees{}, slippage{};
    std::string version;
    std::string pair_artifact_hash;
    std::uint64_t valid_from_ns = 0, valid_until_ns = 0;
};

struct JointBookPolicy {
    std::uint64_t liveness_ns = 30000000000ULL, authority_ns = 45000000000ULL, change_ns = 15000000000ULL;
    bool legacy_skew = false, research_only = false;
};

struct PairPolicy {
    std::uint64_t max_age_ns = 250'000'000, max_skew_ns = 100'000'000;
    Money max_total_payout = 500 * unit;
    Money max_single_leg_loss = 100 * unit;
    Money min_profit = unit;
    std::int64_t min_profit_bps = 50;  // denominator: budgeted total spend
    std::int64_t min_quantity = 1, quantity_step = 1;
    std::vector<std::int64_t> quantities{25, 50, 100, 200, 500};
    std::uint64_t max_liveness_age_ns = 0, max_authoritative_age_ns = 0;
    bool asof_enabled = false;
    JointBookPolicy asof;
};

struct CrossVenueOpportunity {
    signal::OpportunityIntent intent;  // existing contract remains unchanged
    std::string candidate_id, pair_id, artifact_hash;
    std::uint64_t approval_revision = 0;
    std::array<BookProvenance, 2> evidence;
    std::array<Money, 2> costs{}, fees{}, slippage{};
    std::string fee_version;
};

struct FillReceipt {
    VenueInstrumentKey key;
    std::string fill_id, order_id, pair_id;
    bool canonical_yes = true;
    std::int64_t qty = 0;
    Money price = 0, fee_paid = 0;
    std::uint64_t exchange_time_ns = 0, received_time_ns = 0;
    std::uint64_t raw_log_offset = 0;
    std::string raw_hash;
    std::string fee_version;
    bool simulated = true;
    std::string identity() const;
    bool operator==(const FillReceipt&) const = default;
};
void validate_receipt(const ApprovedPair& pair, const FillReceipt& fill);

}  // namespace trading_engine::cross_venue
