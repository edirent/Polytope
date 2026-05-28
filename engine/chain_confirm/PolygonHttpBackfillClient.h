#pragma once

#include "chain_confirm/ChainConfirmConfig.h"
#include "chain_confirm/EthLog.h"
#include "chain_confirm/EthLogSubscription.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::chain_confirm {

struct BackfillRequest {
    std::uint64_t from_block{0};
    std::uint64_t to_block{0};
    EthLogSubscription subscription;
};

struct BackfillResult {
    bool ok{false};
    std::string error;
    std::vector<EthLog> logs;
};

class PolygonHttpBackfillClient {
public:
    explicit PolygonHttpBackfillClient(ChainConfirmConfig config = {});

    [[nodiscard]] BackfillRequest make_request(
        std::uint64_t last_seen_block,
        std::uint64_t current_block,
        const EthLogSubscription& subscription
    ) const;

    [[nodiscard]] std::string redacted_http_url(
        const std::string& http_url
    ) const;

private:
    ChainConfirmConfig config_;
};

}  // namespace trading_engine::chain_confirm
