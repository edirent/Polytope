#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::chain_confirm {

struct ChainConfirmConfig {
    bool enabled{true};
    std::string mode{"paper_or_research_only"};

    std::string polygon_ws_url_env{"POLYGON_RPC_WS_URL"};
    std::string polygon_http_url_env{"POLYGON_RPC_HTTP_URL"};

    bool include_removed_logs{true};
    bool narrow_filter{true};

    std::uint64_t pending_window_ms{5000};
    std::uint32_t max_candidate_matches{8};
    std::uint64_t expire_unmatched_ms{10000};

    bool backfill_enabled{true};
    std::uint64_t block_lag_tolerance{6};
};

[[nodiscard]] std::string redact_rpc_url(const std::string& url);

}  // namespace trading_engine::chain_confirm
