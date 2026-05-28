#pragma once

#include "chain_confirm/ChainConfirmConfig.h"
#include "chain_confirm/EthLog.h"
#include "chain_confirm/EthLogSubscription.h"

#include <cstdint>
#include <functional>
#include <string>

namespace trading_engine::chain_confirm {

class PolygonWsLogClient {
public:
    using LogHandler = std::function<void(const EthLog&)>;

    explicit PolygonWsLogClient(ChainConfirmConfig config = {});

    void set_subscription(EthLogSubscription subscription);
    void set_log_handler(LogHandler handler);

    void on_subscription_ack(std::uint64_t subscription_id) noexcept;
    void on_log(const EthLog& log);
    void on_disconnect() noexcept;

    [[nodiscard]] bool subscription_acknowledged() const noexcept;
    [[nodiscard]] std::uint64_t subscription_id() const noexcept;
    [[nodiscard]] std::uint64_t last_seen_block() const noexcept;
    [[nodiscard]] bool needs_backfill() const noexcept;

    [[nodiscard]] std::string redacted_ws_url(
        const std::string& ws_url
    ) const;

private:
    ChainConfirmConfig config_;
    EthLogSubscription subscription_;
    LogHandler log_handler_;
    bool subscription_acknowledged_{false};
    bool needs_backfill_{false};
    std::uint64_t subscription_id_{0};
    std::uint64_t last_seen_block_{0};
};

}  // namespace trading_engine::chain_confirm
