#pragma once

#include "oracle/compiler/CompiledComponent.h"
#include "oracle/compiler/ConstraintGraph.h"

#include <cstdint>
#include <vector>

namespace trading_engine::oracle {

struct ComponentPartitionResult {
    std::vector<CompiledComponent> components;
    std::uint64_t partition_hash = 0;
};

class ComponentPartitioner {
public:
    [[nodiscard]] ComponentPartitionResult partition(
        const ConstraintGraph& graph,
        const CompiledConstraintSet& compiled
    ) const;
};

[[nodiscard]] std::uint64_t hash_components(
    const std::vector<CompiledComponent>& components
) noexcept;

}  // namespace trading_engine::oracle
