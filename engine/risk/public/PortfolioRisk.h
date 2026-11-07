#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace trading_engine::risk {
// Inputs are value projections of the canonical PaperLedger, never another ledger.
struct PortfolioHolding { std::int64_t lots=0,cost_tick=0; std::string event,venue,account; };
struct PortfolioAccountView {
    std::int64_t cash_tick=0;
    std::map<std::string,PortfolioHolding> holdings;
    std::map<std::string,std::int64_t> strategy_inventory_cost;
};
struct PortfolioLeg {
    std::string asset,market,event,venue,account,settlement_asset;
    bool buy=true;
    std::int64_t quantity=0,price_tick=0,fee_per_lot_tick=0;
    bool operator==(const PortfolioLeg&) const = default;
};
struct PortfolioLimits {
    std::int64_t total_cost_tick=0,event_cost_tick=0,asset_lots=0,safety_cash_tick=0,order_cost_tick=0;
    std::map<std::string,std::int64_t> strategy_budget,venue_budget,account_budget;
    std::size_t max_open_orders=128;
};
struct PortfolioReservation {
    std::uint64_t id=0;
    std::string owner,key;
    std::vector<PortfolioLeg> remaining;
    bool active=true;
};
struct PortfolioAdmission { bool ok=false; std::string reason; std::uint64_t reservation_id=0; };
inline std::int64_t portfolio_cash(const std::vector<PortfolioLeg>& legs) {
    __int128 total=0;
    for(const auto& leg:legs) total+=static_cast<__int128>(leg.quantity)*(static_cast<__int128>(leg.buy?leg.price_tick:0)+leg.fee_per_lot_tick);
    if(total<0 || total>1'000'000'000'000LL) return -1;
    return static_cast<std::int64_t>(total);
}
}
