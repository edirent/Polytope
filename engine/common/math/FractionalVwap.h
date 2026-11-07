#pragma once
#include "engine/state/cross_venue/Types.h"
#include "engine/common/math/VwapMath.h"

namespace trading_engine::common::math {
struct MicroVwap { bool ok=false; cross_venue::Money cost=0,worst=0; std::int64_t available=0; };
inline MicroVwap buy_vwap_micro(const cross_venue::VersionedBook& b,std::int64_t q) {
    using namespace cross_venue;MicroVwap r;require(q>0 && q<=multiply(max_quantity,unit),"invalid micro quantity");
    __int128 cost=0;auto remaining=q;
    for(unsigned i=0;i<b.depth.ask_count;++i){auto n=b.ask_qty_micro[i],p=b.depth.asks[i].price_tick;require(n>=0 && p>=0 && p<=unit,"invalid micro depth");r.available=add(r.available,n);auto take=std::min(remaining,n);cost+=static_cast<__int128>(take)*p;remaining-=take;if(take)r.worst=p;}
    if(remaining)return r;require((cost+unit-1)/unit<=std::numeric_limits<Money>::max(),"micro cost overflow");r.ok=true;r.cost=static_cast<Money>((cost+unit-1)/unit);return r;
}
// Exact micro-share depth for an integer candidate size. Monetary products use
// int128 and round total acquisition cost upward once to a money tick.
inline VwapMathResult buy_vwap_exact(const cross_venue::VersionedBook& book,std::int64_t quantity) {
    if(!book.fractional_quantities) return buy_vwap_linear(book.depth.asks.data(),book.depth.ask_count,quantity);
    using namespace cross_venue;
    VwapMathResult r;if(quantity<=0 || quantity>max_quantity)return r;
    auto remaining=multiply(quantity,unit);__int128 cost=0;std::int64_t available=0;
    for(std::uint16_t i=0;i<book.depth.ask_count;++i){
        const auto q=book.ask_qty_micro[i],p=book.depth.asks[i].price_tick;
        require(q>=0 && p>=0 && p<=unit,"invalid fractional depth");available=add(available,q);
        const auto take=std::min(remaining,q);cost+=static_cast<__int128>(p)*take;
        if(take>0)r.worst_price_tick=p;remaining-=take;
    }
    r.executable_qty_lots=available/unit;if(remaining>0)return r;
    const auto total=(cost+unit-1)/unit;require(total<=std::numeric_limits<Money>::max(),"fractional cost overflow");
    r.ok=true;r.total_cost_tick=static_cast<Money>(total);r.vwap_tick=r.total_cost_tick/quantity;return r;
}
}  // namespace trading_engine::common::math
