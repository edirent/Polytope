#include "oracle/compiler/ConstraintGraph.h"

#include <algorithm>

namespace trading_engine::oracle {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hash_byte(std::uint64_t* hash, std::uint8_t value) noexcept {
    *hash ^= value;
    *hash *= kFnvPrime;
}

void hash_u32(std::uint64_t* hash, std::uint32_t value) noexcept {
    for (int shift = 0; shift < 32; shift += 8) {
        hash_byte(hash, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void hash_string(std::uint64_t* hash, const std::string& value) noexcept {
    for (const unsigned char byte : value) {
        hash_byte(hash, byte);
    }
    hash_byte(hash, 0xffU);
}

}  // namespace

ConstraintGraph ConstraintGraphBuilder::build(
    const CompiledConstraintSet& compiled
) const {
    ConstraintGraph graph;

    graph.nodes.reserve(compiled.variables.size());
    for (const auto& variable : compiled.variables) {
        graph.nodes.push_back(ConstraintGraphNode{
            .var_id = variable.var_id,
            .variable_id = variable.variable_key,
            .market_id = variable.market_id,
            .event_id = {}
        });
    }

    graph.edges.reserve(compiled.constraints.size());
    for (std::size_t i = 0; i < compiled.constraints.size(); ++i) {
        auto var_ids = compiled.constraints[i].var_ids;
        std::sort(var_ids.begin(), var_ids.end());
        graph.edges.push_back(ConstraintGraphEdge{
            .constraint_id = static_cast<ConstraintId>(i),
            .var_ids = std::move(var_ids)
        });
    }

    graph.graph_hash = hash_constraint_graph(graph);
    return graph;
}

std::uint64_t hash_constraint_graph(const ConstraintGraph& graph) noexcept {
    std::uint64_t hash = kFnvOffset;

    for (const auto& node : graph.nodes) {
        hash_u32(&hash, node.var_id);
        hash_string(&hash, node.variable_id);
        hash_string(&hash, node.market_id);
        hash_string(&hash, node.event_id);
    }

    for (const auto& edge : graph.edges) {
        hash_u32(&hash, edge.constraint_id);
        hash_u32(&hash, static_cast<std::uint32_t>(edge.var_ids.size()));
        for (const auto var_id : edge.var_ids) {
            hash_u32(&hash, var_id);
        }
    }

    return hash;
}

}  // namespace trading_engine::oracle
