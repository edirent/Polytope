#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace trading_engine::oracle {

using ComponentId = std::uint32_t;
using ConstraintId = std::uint32_t;

enum class ComponentKind : std::uint8_t {
    SmallEnumerable,
    ExactlyOne,
    AtMostOne,
    ImplicationDag,
    WeightedThreshold,
    GenericLinearBoolean
};

struct CompiledComponent {
    ComponentId component_id = 0;
    std::vector<std::uint32_t> variable_ids;
    std::vector<ConstraintId> constraint_ids;
    std::string component_hash;
    ComponentKind kind = ComponentKind::GenericLinearBoolean;
};

[[nodiscard]] const char* component_kind_to_string(
    ComponentKind kind
) noexcept;

}  // namespace trading_engine::oracle
