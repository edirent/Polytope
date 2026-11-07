#pragma once
#include "engine/state/cross_venue/Types.h"
namespace trading_engine::cross_venue {
// Value view over the existing store: never retains or invents market state.
struct JointBookAsOf {
    std::uint64_t decision_time=0,update_skew_ns=0;
    bool transport_healthy=true,reconstruction_valid=true,source_authority_valid=true;
    bool book_freshness_acceptable=true,both_markets_tradable=true,available=true;
    bool executable_asks_available=true;
    bool diagnostic_eligible=false,strict_strategy_eligible=false;
    std::string reason;
};
JointBookAsOf joint_book_asof(const std::array<VersionedBook,2>& books,std::uint64_t t,
    const JointBookPolicy& policy,std::array<bool,2> websocket,std::array<bool,2> tradable,
    bool approved=false);
}
