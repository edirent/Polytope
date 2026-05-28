#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::oracle {

struct ArtifactManifest {
    std::uint32_t artifact_version = 1;

    std::uint64_t created_at_ns = 0;

    std::uint32_t market_count = 0;
    std::uint32_t asset_count = 0;
    std::uint32_t variable_count = 0;
    std::uint32_t rule_count = 0;
    std::uint32_t constraint_count = 0;
    std::uint64_t feasible_state_count = 0;
    std::uint64_t bundle_count = 0;

    bool llm_enabled = false;
    bool llm_outputs_used = false;
    bool llm_outputs_require_manual_review = false;

    std::string llm_provider = "none";

    std::string input_snapshot_hash;
    std::string rulebook_hash;
    std::string constraint_hash;
    std::string feasible_states_hash;
    std::string payoff_hash;
    std::string bundle_hash;
};

}  // namespace trading_engine::oracle
