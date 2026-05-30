#pragma once

#include "oracle/compiler/CompiledComponent.h"
#include "oracle/compiler/ConstraintCompiler.h"

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::oracle {

struct ConstraintGraphNode {
    std::uint32_t var_id = 0;
    std::string variable_id;
    std::string market_id;
    std::string event_id;
};

struct ConstraintGraphEdge {
    ConstraintId constraint_id = 0;
    std::vector<std::uint32_t> var_ids;
};

struct ConstraintGraph {
    std::vector<ConstraintGraphNode> nodes;
    std::vector<ConstraintGraphEdge> edges;
    std::uint64_t graph_hash = 0;
};

class ConstraintGraphBuilder {
public:
    [[nodiscard]] ConstraintGraph build(
        const CompiledConstraintSet& compiled
    ) const;
};

[[nodiscard]] std::uint64_t hash_constraint_graph(
    const ConstraintGraph& graph
) noexcept;

}  // namespace trading_engine::oracle
