#include "chain_confirm/PolygonHttpBackfillClient.h"

#include <algorithm>
#include <utility>

namespace trading_engine::chain_confirm {

PolygonHttpBackfillClient::PolygonHttpBackfillClient(
    ChainConfirmConfig config
)
    : config_(std::move(config)) {}

BackfillRequest PolygonHttpBackfillClient::make_request(
    std::uint64_t last_seen_block,
    std::uint64_t current_block,
    const EthLogSubscription& subscription
) const {
    BackfillRequest request;
    request.subscription = subscription;

    const std::uint64_t margin = config_.block_lag_tolerance;
    request.from_block = last_seen_block > margin
        ? last_seen_block - margin
        : 0;
    request.to_block = std::max(request.from_block, current_block);

    return request;
}

std::string PolygonHttpBackfillClient::redacted_http_url(
    const std::string& http_url
) const {
    return redact_rpc_url(http_url);
}

}  // namespace trading_engine::chain_confirm
