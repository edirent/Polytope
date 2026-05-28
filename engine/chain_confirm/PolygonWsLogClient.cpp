#include "chain_confirm/PolygonWsLogClient.h"

#include <algorithm>
#include <utility>

namespace trading_engine::chain_confirm {

PolygonWsLogClient::PolygonWsLogClient(ChainConfirmConfig config)
    : config_(std::move(config)) {}

void PolygonWsLogClient::set_subscription(
    EthLogSubscription subscription
) {
    subscription_ = std::move(subscription);
    subscription_acknowledged_ = false;
}

void PolygonWsLogClient::set_log_handler(LogHandler handler) {
    log_handler_ = std::move(handler);
}

void PolygonWsLogClient::on_subscription_ack(
    std::uint64_t subscription_id
) noexcept {
    subscription_id_ = subscription_id;
    subscription_acknowledged_ = true;
    needs_backfill_ = false;
}

void PolygonWsLogClient::on_log(const EthLog& log) {
    last_seen_block_ = std::max(last_seen_block_, log.block_number);
    if (log_handler_) {
        log_handler_(log);
    }
}

void PolygonWsLogClient::on_disconnect() noexcept {
    subscription_acknowledged_ = false;
    needs_backfill_ = config_.backfill_enabled;
}

bool PolygonWsLogClient::subscription_acknowledged() const noexcept {
    return subscription_acknowledged_;
}

std::uint64_t PolygonWsLogClient::subscription_id() const noexcept {
    return subscription_id_;
}

std::uint64_t PolygonWsLogClient::last_seen_block() const noexcept {
    return last_seen_block_;
}

bool PolygonWsLogClient::needs_backfill() const noexcept {
    return needs_backfill_;
}

std::string PolygonWsLogClient::redacted_ws_url(
    const std::string& ws_url
) const {
    return redact_rpc_url(ws_url);
}

}  // namespace trading_engine::chain_confirm
