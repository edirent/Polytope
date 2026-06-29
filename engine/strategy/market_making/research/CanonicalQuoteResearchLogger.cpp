#include "engine/strategy/market_making/research/CanonicalQuoteResearchLogger.h"

#include <utility>

namespace trading_engine::strategy::market_making::research {

CanonicalQuoteResearchLogger::CanonicalQuoteResearchLogger(
    std::filesystem::path path
) : path_(std::move(path)) {
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path());
    }
    out_.open(path_, std::ios::out | std::ios::trunc);
    if (!out_.is_open()) {
        return;
    }
    out_ << "ts_ms,market_id,token_id,complement_token_id,event_type,"
            "outcome_side,spot,vol,tte_ns,book_bid,book_ask,book_mid,"
            "spread,canonical_yes_bid,canonical_yes_ask,"
            "canonical_yes_market_mid,canonical_yes_external_raw,"
            "canonical_yes_tradable_fair,asset_external_raw,"
            "asset_tradable_fair,basis_raw,basis_tradable,buy_edge,"
            "sell_edge,current_inventory_asset,"
            "current_inventory_canonical_yes,target_inventory_canonical_yes,"
            "portfolio_touch_exposure,quote_side,quote_price,quote_size,"
            "quote_reason,risk_decision,risk_reject_reason,latency_ns,"
            "book_age_ms,spot_age_ms,vol_age_ms\n";
}

bool CanonicalQuoteResearchLogger::ok() const noexcept {
    return out_.is_open() && out_.good();
}

const std::filesystem::path& CanonicalQuoteResearchLogger::path() const noexcept {
    return path_;
}

void CanonicalQuoteResearchLogger::log(
    const CanonicalQuoteResearchRow& row
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out_.is_open()) {
        return;
    }
    const auto& s = row.state;
    out_ << row.ts_ms << ','
         << csv_escape(s.market_id) << ','
         << csv_escape(s.token_id) << ','
         << csv_escape(s.complement_token_id) << ','
         << event_type_name(s.event_type) << ','
         << outcome_side_name(s.asset_side) << ','
         << s.spot << ','
         << s.annualized_vol << ','
         << s.tte_ns << ','
         << s.book_bid_tick << ','
         << s.book_ask_tick << ','
         << s.book_mid_tick << ','
         << s.spread_tick << ','
         << s.canonical_yes_bid_tick << ','
         << s.canonical_yes_ask_tick << ','
         << row.canonical_yes_market_mid_tick << ','
         << row.canonical_yes_external_raw_tick << ','
         << row.canonical_yes_tradable_fair_tick << ','
         << row.asset_external_raw_tick << ','
         << row.asset_tradable_fair_tick << ','
         << row.basis_raw_tick << ','
         << row.basis_tradable_tick << ','
         << row.buy_edge_tick << ','
         << row.sell_edge_tick << ','
         << row.current_inventory_asset << ','
         << row.current_inventory_canonical_yes << ','
         << row.target_inventory_canonical_yes << ','
         << row.portfolio_touch_exposure << ','
         << csv_escape(row.quote_side) << ','
         << row.quote_price_tick << ','
         << row.quote_size_lots << ','
         << csv_escape(row.quote_reason) << ','
         << csv_escape(row.risk_decision) << ','
         << csv_escape(row.risk_reject_reason) << ','
         << row.latency_ns << ','
         << s.book_age_ms << ','
         << s.spot_age_ms << ','
         << s.vol_age_ms << '\n';
}

const char* CanonicalQuoteResearchLogger::outcome_side_name(
    OutcomeSide side
) noexcept {
    return side == OutcomeSide::Yes ? "Yes" : "No";
}

const char* CanonicalQuoteResearchLogger::event_type_name(
    ExternalFairEventType type
) noexcept {
    switch (type) {
        case ExternalFairEventType::TerminalAbove:
            return "TerminalAbove";
        case ExternalFairEventType::TerminalBelow:
            return "TerminalBelow";
        case ExternalFairEventType::UpTouch:
            return "UpTouch";
        case ExternalFairEventType::DownTouch:
            return "DownTouch";
        case ExternalFairEventType::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

std::string CanonicalQuoteResearchLogger::csv_escape(
    const std::string& value
) {
    if (value.find_first_of(",\"\n\r") == std::string::npos) {
        return value;
    }
    std::string out{"\""};
    for (const auto c : value) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

}  // namespace trading_engine::strategy::market_making::research
