#pragma once

#include "engine/execution/public/ExecutionTypes.h"
#include "engine/execution/public/OrderPlan.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace trading_engine::execution {

struct LiveExecutionConfig {
    bool enabled = false;
    bool require_execution_enabled = true;
    bool require_context_live_enabled = true;

    std::uint32_t max_child_orders_per_plan = 1;
    std::uint32_t max_quote_orders_per_group = 2;

    std::int64_t max_child_notional_tick = 0;

    bool allow_buy_orders = true;
    bool allow_sell_orders = true;
    bool reject_expired_plans = true;

    bool maker_post_only = true;
    bool maker_require_resting_ack = true;

    std::string default_order_type = "GTC";
};

struct LiveOrderRequest {
    std::uint64_t parent_id = 0;
    std::uint64_t child_id = 0;

    std::string client_order_id;
    std::string market_id;
    std::string asset_id;

    OrderSide side = OrderSide::Buy;
    std::int64_t quantity_lots = 0;
    std::int64_t price_tick = 0;

    std::uint64_t created_ts_ns = 0;
    std::uint64_t expire_after_ns = 0;

    std::string order_type = "GTC";
    bool post_only = false;
};

struct SignedLiveOrder {
    std::string request_body_json;
    std::string venue_order_id_hint;
};

struct LiveOrderSignResult {
    bool ok = false;
    SignedLiveOrder order;
    std::string error;
};

class ILiveOrderSigner {
public:
    virtual ~ILiveOrderSigner() = default;

    [[nodiscard]] virtual LiveOrderSignResult sign_order(
        const LiveOrderRequest& request
    ) = 0;
};

struct LiveTransportSubmitResult {
    bool ok = false;
    std::string venue_order_id;
    std::string venue_status;
    std::string raw_response;
    std::string error;
};

struct LiveTransportCancelResult {
    bool ok = false;
    std::string raw_response;
    std::string error;
};

class ILiveOrderTransport {
public:
    virtual ~ILiveOrderTransport() = default;

    [[nodiscard]] virtual LiveTransportSubmitResult submit_order(
        std::string_view request_body_json
    ) = 0;

    [[nodiscard]] virtual LiveTransportCancelResult cancel_order(
        std::string_view venue_order_id
    ) = 0;
};

struct PolymarketL2Credentials {
    std::string address;
    std::string api_key;
    std::string secret;
    std::string passphrase;

    [[nodiscard]] bool complete() const noexcept {
        return !address.empty() &&
            !api_key.empty() &&
            !secret.empty() &&
            !passphrase.empty();
    }
};

struct PolymarketL2Headers {
    std::string address;
    std::string api_key;
    std::string passphrase;
    std::string timestamp;
    std::string signature;
};

class PolymarketL2Authenticator {
public:
    explicit PolymarketL2Authenticator(PolymarketL2Credentials credentials);

    [[nodiscard]] PolymarketL2Headers build_headers(
        std::string_view method,
        std::string_view request_path,
        std::string_view body,
        std::int64_t unix_timestamp_seconds
    ) const;

private:
    PolymarketL2Credentials credentials_;
};

class PolymarketHttpsOrderTransport final : public ILiveOrderTransport {
public:
    explicit PolymarketHttpsOrderTransport(
        PolymarketL2Credentials credentials,
        std::string host = "clob.polymarket.com",
        std::string port = "443"
    );

    [[nodiscard]] LiveTransportSubmitResult submit_order(
        std::string_view request_body_json
    ) override;

    [[nodiscard]] LiveTransportCancelResult cancel_order(
        std::string_view venue_order_id
    ) override;

private:
    [[nodiscard]] std::string authenticated_request(
        std::string_view method,
        std::string_view request_path,
        std::string_view body
    ) const;

    PolymarketL2Authenticator authenticator_;
    std::string host_;
    std::string port_;
};

}  // namespace trading_engine::execution
