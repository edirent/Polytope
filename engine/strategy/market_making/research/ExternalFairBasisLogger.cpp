#include "engine/strategy/market_making/research/ExternalFairBasisLogger.h"

#include <filesystem>
#include <iomanip>
#include <utility>

namespace trading_engine::strategy::market_making::research {

ExternalFairBasisLogger::ExternalFairBasisLogger(std::filesystem::path path)
    : path_(std::move(path)) {
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path());
    }
    out_.open(path_, std::ios::out | std::ios::trunc);
    if (!out_.is_open()) {
        return;
    }

    out_ << "ts_ms,market_id,token_id,symbol,event_type,barrier_price,"
            "outcome_side,spot,annualized_vol,tte_years,external_fair_tick,"
            "yes_probability,best_bid_tick,best_ask_tick,bid_size,ask_size,"
            "book_mid_tick,book_micro_tick,spread_tick,mid_basis_tick,"
            "micro_basis_tick,buy_edge_tick,sell_edge_tick,book_age_ms,"
            "spot_age_ms,vol_age_ms,spot_source\n";
}

bool ExternalFairBasisLogger::ok() const noexcept {
    return out_.is_open() && out_.good();
}

const std::filesystem::path& ExternalFairBasisLogger::path() const noexcept {
    return path_;
}

void ExternalFairBasisLogger::log(
    const ExternalFairBasisSnapshot& snapshot
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out_.is_open()) {
        return;
    }

    out_ << snapshot.ts_ms << ','
         << csv_escape(snapshot.market_id) << ','
         << csv_escape(snapshot.token_id) << ','
         << symbol_name(snapshot.symbol) << ','
         << event_type_name(snapshot.event_type) << ','
         << std::setprecision(12) << snapshot.barrier_price << ','
         << outcome_side_name(snapshot.outcome_side) << ','
         << std::setprecision(12) << snapshot.spot << ','
         << std::setprecision(12) << snapshot.annualized_vol << ','
         << std::setprecision(12) << snapshot.tte_years << ','
         << snapshot.external_fair_tick << ','
         << std::setprecision(12) << snapshot.yes_probability << ','
         << snapshot.best_bid_tick << ','
         << snapshot.best_ask_tick << ','
         << snapshot.bid_size << ','
         << snapshot.ask_size << ','
         << snapshot.book_mid_tick << ','
         << snapshot.book_micro_tick << ','
         << snapshot.spread_tick << ','
         << snapshot.mid_basis_tick << ','
         << snapshot.micro_basis_tick << ','
         << snapshot.buy_edge_tick << ','
         << snapshot.sell_edge_tick << ','
         << snapshot.book_age_ms << ','
         << snapshot.spot_age_ms << ','
         << snapshot.vol_age_ms << ','
         << csv_escape(snapshot.spot_source) << '\n';
}

const char* ExternalFairBasisLogger::symbol_name(
    ExternalFairSymbol symbol
) noexcept {
    switch (symbol) {
        case ExternalFairSymbol::BTC:
            return "BTC";
        case ExternalFairSymbol::ETH:
            return "ETH";
        case ExternalFairSymbol::SOL:
            return "SOL";
        case ExternalFairSymbol::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

const char* ExternalFairBasisLogger::event_type_name(
    ExternalFairEventType event_type
) noexcept {
    switch (event_type) {
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

const char* ExternalFairBasisLogger::outcome_side_name(
    OutcomeSide side
) noexcept {
    switch (side) {
        case OutcomeSide::Yes:
            return "Yes";
        case OutcomeSide::No:
            return "No";
    }
    return "Yes";
}

std::string ExternalFairBasisLogger::csv_escape(const std::string& value) {
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
