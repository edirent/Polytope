#include "engine/state/cross_venue/Types.h"

#include <openssl/evp.h>
#include <algorithm>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace trading_engine::cross_venue {
void require(bool condition, std::string_view reason) {
    if (!condition) throw std::invalid_argument(std::string(reason));
}
Money add(Money a, Money b) {
    const auto n = static_cast<__int128>(a) + b;
    require(n >= std::numeric_limits<Money>::min() && n <= std::numeric_limits<Money>::max(), "money overflow");
    return static_cast<Money>(n);
}
Money multiply(Money a, std::int64_t b) {
    const auto n = static_cast<__int128>(a) * b;
    require(n >= std::numeric_limits<Money>::min() && n <= std::numeric_limits<Money>::max(), "money overflow");
    return static_cast<Money>(n);
}
Money money(std::string_view text) {
    require(!text.empty() && text.size() <= 24, "invalid decimal");
    bool negative = text.front() == '-';
    if (negative) text.remove_prefix(1);
    require(!text.empty(), "invalid decimal");
    __int128 n = 0;
    int places = -1;
    for (char c : text) {
        if (c == '.') { require(places == -1, "invalid decimal"); places = 0; continue; }
        require(c >= '0' && c <= '9', "decimal must use digits, no exponent");
        if (places >= 0) { ++places; require(places <= 6, "precision exceeds six places"); }
        n = n * 10 + c - '0';
    }
    require(text.front() != '.' && text.back() != '.', "invalid decimal");
    for (int i = std::max(places, 0); i < 6; ++i) n *= 10;
    if (negative) n = -n;
    require(n >= std::numeric_limits<Money>::min() && n <= std::numeric_limits<Money>::max(), "money overflow");
    return static_cast<Money>(n);
}
std::string decimal(Money value) {
    const auto magnitude = value < 0 ? -static_cast<__int128>(value) : static_cast<__int128>(value);
    std::ostringstream out;
    if (value < 0) out << '-';
    out << static_cast<std::uint64_t>(magnitude / unit) << '.' << std::setfill('0') << std::setw(6)
        << static_cast<std::uint64_t>(magnitude % unit);
    return out.str();
}
std::string sha256(std::string_view bytes) {
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int length = 0;
    require(EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(), nullptr) == 1, "SHA256 failed");
    std::ostringstream out;
    for (unsigned int i = 0; i < length; ++i) out << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(digest[i]);
    return out.str();
}
std::uint64_t compact_hash(std::string_view bytes) { return std::stoull(sha256(bytes).substr(0, 16), nullptr, 16); }
std::string field(std::string_view value) { return std::to_string(value.size()) + ":" + std::string(value); }
std::string venue_name(VenueId v) {
    switch (v) {
        case VenueId::Kalshi: return "kalshi";
        case VenueId::PolymarketInternational: return "polymarket_international";
        case VenueId::PolymarketUS: return "polymarket_us";
    }
    throw std::invalid_argument("invalid venue");
}
VenueId venue_id(std::string_view s) {
    if (s == "kalshi") return VenueId::Kalshi;
    if (s == "polymarket_international") return VenueId::PolymarketInternational;
    if (s == "polymarket_us") return VenueId::PolymarketUS;
    throw std::invalid_argument("unknown venue");
}
std::string VenueInstrumentKey::identity() const {
    return field(venue_name(venue)) + field(market_id) + field(instrument_id) + field(currency);
}
bool valid_sha256(const std::string& s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::string FillReceipt::identity() const { return field(venue_name(key.venue)) + field(fill_id); }
void validate_receipt(const ApprovedPair& p, const FillReceipt& f) {
    validate_pair(p);
    require(f.simulated, "cross-venue accepts SIMULATED receipts only");
    require(f.pair_id == p.pair_id && f.key == (f.canonical_yes ? p.leg_yes : p.leg_no), "receipt instrument/pair mismatch");
    require(!f.fill_id.empty() && !f.order_id.empty() && !f.fee_version.empty() && valid_sha256(f.raw_hash), "missing receipt evidence");
    require(f.qty > 0 && f.qty <= max_quantity && f.price >= 0 && f.price <= unit && f.fee_paid >= 0, "invalid receipt money/quantity");
    require(f.exchange_time_ns > 0 && f.received_time_ns >= f.exchange_time_ns, "reversed receipt timestamp");
    (void)add(multiply(f.price, f.qty), f.fee_paid);
}
}  // namespace trading_engine::cross_venue
