#include "engine/risk/core/RiskEngine.h"
#include <algorithm>

namespace trading_engine::risk {
PortfolioAdmission RiskEngine::check_portfolio(const std::string& owner,const std::vector<PortfolioLeg>& proposed,
    const PortfolioAccountView& account,const PortfolioLimits& limits,bool frozen) const {
    auto reject=[](const char* why){return PortfolioAdmission{false,why,0};};
    if(frozen)return reject("PORTFOLIO_FROZEN");
    if(proposed.empty() || proposed.size()>16 || owner.empty())return reject("INVALID_RESOURCE_REQUEST");
    const auto cost=portfolio_cash(proposed);
    if(cost<0 || cost>limits.order_cost_tick)return reject("ORDER_COST_LIMIT");
    const auto snapshot=ledger_snapshot();
    std::map<std::string,std::int64_t> buy,sell,events,venues,accounts,strategies;
    std::int64_t filled=0;
    for(const auto& [asset,h]:account.holdings) {
        if(h.lots<0 || h.cost_tick<0)return reject("ACCOUNT_MISMATCH");
        filled+=h.cost_tick;events[h.event]+=h.cost_tick;venues[h.venue]+=h.cost_tick;accounts[h.account]+=h.cost_tick;
    }
    strategies=account.strategy_inventory_cost;
    std::size_t open=0;
    auto add=[&](const std::string& strategy,const PortfolioLeg& leg) {
        if(leg.buy) {
            buy[leg.asset]+=leg.quantity;auto exposure=leg.quantity*leg.price_tick;
            events[leg.event]+=exposure;venues[leg.venue]+=exposure;accounts[leg.account]+=exposure;strategies[strategy]+=exposure+leg.quantity*leg.fee_per_lot_tick;
        }else {sell[leg.asset]+=leg.quantity;strategies[strategy]+=leg.quantity*leg.fee_per_lot_tick;}
    };
    for(const auto& [id,r]:portfolio_reservations())if(r.active) {
        ++open;for(const auto& leg:r.remaining)add(r.owner,leg);
    }
    if(open+proposed.size()>limits.max_open_orders)return reject("OPEN_ORDER_LIMIT");
    for(const auto& leg:proposed) {
        if(leg.asset.empty() || leg.market.empty() || leg.event.empty() || leg.venue.empty() || leg.account.empty() || leg.settlement_asset.empty() ||
           leg.quantity<=0 || leg.quantity>1'000'000 || leg.price_tick<=0 || leg.price_tick>=1'000'000 || leg.fee_per_lot_tick<0 || leg.fee_per_lot_tick>1'000'000)
            return reject("INVALID_RESOURCE_REQUEST");
        add(owner,leg);
    }
    if(account.cash_tick-snapshot.total_reserved_cost_tick-limits.safety_cash_tick<cost)return reject("INSUFFICIENT_SHARED_CASH");
    if(filled+snapshot.total_reserved_exposure_tick+cost>limits.total_cost_tick)return reject("GLOBAL_EXPOSURE_LIMIT");
    for(const auto& [asset,qty]:sell) {
        const auto it=account.holdings.find(asset);const auto held=it==account.holdings.end()?0:it->second.lots;
        if(qty>held)return reject("INSUFFICIENT_SHARED_SELL_INVENTORY");
    }
    for(const auto& [asset,qty]:buy) {
        const auto it=account.holdings.find(asset);const auto held=it==account.holdings.end()?0:it->second.lots;
        if(qty+held>limits.asset_lots)return reject("ASSET_INVENTORY_LIMIT");
    }
    for(const auto& [event,value]:events)if(value>limits.event_cost_tick)return reject("EVENT_CONCENTRATION_LIMIT");
    for(const auto& [strategy,value]:strategies) {
        const auto it=limits.strategy_budget.find(strategy);
        if(it==limits.strategy_budget.end() || value>it->second)return reject("STRATEGY_BUDGET_LIMIT");
    }
    for(const auto& [venue,value]:venues) {
        auto it=limits.venue_budget.find(venue);if(it!=limits.venue_budget.end() && value>it->second)return reject("VENUE_BUDGET_LIMIT");
    }
    for(const auto& [id,value]:accounts) {
        auto it=limits.account_budget.find(id);if(it!=limits.account_budget.end() && value>it->second)return reject("ACCOUNT_BUDGET_LIMIT");
    }
    return {true,"APPROVED",0};
}
PortfolioAdmission RiskEngine::reserve_portfolio(const std::string& owner,const std::string& key,const std::vector<PortfolioLeg>& legs,
    const PortfolioAccountView& account,const PortfolioLimits& limits,bool frozen) {
    auto result=check_portfolio(owner,legs,account,limits,frozen);
    if(result.ok)result.reservation_id=reservation_book()->reserve_portfolio(owner,key,legs);
    return result;
}
}
