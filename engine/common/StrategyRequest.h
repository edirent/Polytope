#pragma once
#include "engine/signal/public/OpportunityIntent.h"
#include "engine/strategy/market_making/public/QuoteIntent.h"
#include "engine/strategy/market_making/public/CancelQuoteIntent.h"
#include "engine/order_decision/public/OrderDecision.h"
#include <string>
#include <variant>

namespace trading_engine {
struct StrategyRequestMetadata {
    std::string strategy_id, strategy_instance_id,model_version,request_id,idempotency_key;
    std::uint64_t configuration_epoch=0,market_state_version=0,source_timestamp=0,availability_ns=0;
};
struct FuturePairRequest {std::string approved_rule_revision;}; // Unsupported until pair integration.
using StrategyRequestPayload=std::variant<signal::OpportunityIntent,strategy::market_making::QuoteIntent,
    strategy::market_making::CancelQuoteIntent,order_decision::OrderDecision,FuturePairRequest>;
struct StrategyRequest {StrategyRequestMetadata metadata;StrategyRequestPayload payload;};
inline std::uint64_t strategy_identity_hash(const std::string& value) {
    std::uint64_t h=14695981039346656037ULL;for(unsigned char c:value){h^=c;h*=1099511628211ULL;}return h?h:1;
}
}
