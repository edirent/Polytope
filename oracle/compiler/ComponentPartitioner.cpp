#include "oracle/compiler/ComponentPartitioner.h"

#include <algorithm>
#include <numeric>
#include <unordered_map>

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

class DisjointSet {
public:
    explicit DisjointSet(std::size_t size) : parent_(size), rank_(size, 0) {
        std::iota(parent_.begin(), parent_.end(), 0);
    }

    std::size_t find(std::size_t value) {
        if (parent_[value] != value) {
            parent_[value] = find(parent_[value]);
        }
        return parent_[value];
    }

    void unite(std::size_t lhs, std::size_t rhs) {
        auto left = find(lhs);
        auto right = find(rhs);
        if (left == right) {
            return;
        }
        if (rank_[left] < rank_[right]) {
            std::swap(left, right);
        }
        parent_[right] = left;
        if (rank_[left] == rank_[right]) {
            ++rank_[left];
        }
    }

private:
    std::vector<std::size_t> parent_;
    std::vector<std::uint8_t> rank_;
};

[[nodiscard]] bool all_coeffs_one(const LinearBooleanConstraint& constraint) {
    return std::all_of(
        constraint.coeffs.begin(),
        constraint.coeffs.end(),
        [](std::int32_t coeff) { return coeff == 1; }
    );
}

[[nodiscard]] bool is_exactly_one_constraint(
    const LinearBooleanConstraint& constraint,
    const CompiledComponent& component
) {
    return constraint.op == ConstraintOp::Equal && constraint.rhs == 1 &&
           constraint.var_ids.size() == component.variable_ids.size() &&
           all_coeffs_one(constraint);
}

[[nodiscard]] bool is_at_most_one_constraint(
    const LinearBooleanConstraint& constraint,
    const CompiledComponent& component
) {
    return constraint.op == ConstraintOp::LessEqual && constraint.rhs == 1 &&
           constraint.var_ids.size() == component.variable_ids.size() &&
           all_coeffs_one(constraint);
}

[[nodiscard]] bool is_large_exactly_one_hyperedge(
    const LinearBooleanConstraint& constraint
) {
    return constraint.op == ConstraintOp::Equal && constraint.rhs == 1 &&
           constraint.var_ids.size() > 2 && all_coeffs_one(constraint);
}

[[nodiscard]] bool is_large_at_most_one_hyperedge(
    const LinearBooleanConstraint& constraint
) {
    return constraint.op == ConstraintOp::LessEqual && constraint.rhs == 1 &&
           constraint.var_ids.size() > 2 && all_coeffs_one(constraint);
}

[[nodiscard]] bool is_binary_complement_constraint(
    const LinearBooleanConstraint& constraint
) {
    return constraint.op == ConstraintOp::Equal && constraint.rhs == 1 &&
           constraint.var_ids.size() == 2 && all_coeffs_one(constraint);
}

[[nodiscard]] ComponentKind classify_component(
    const CompiledComponent& component,
    const CompiledConstraintSet& compiled
) {
    if (component.constraint_ids.size() == 1) {
        const auto& constraint =
            compiled.constraints[component.constraint_ids.front()];
        if (is_exactly_one_constraint(constraint, component)) {
            return ComponentKind::ExactlyOne;
        }
        if (is_at_most_one_constraint(constraint, component)) {
            return ComponentKind::AtMostOne;
        }
    }

    bool has_large_exactly_one = false;
    bool has_large_at_most_one = false;
    bool all_constraints_supported = !component.constraint_ids.empty();
    for (const auto constraint_id : component.constraint_ids) {
        const auto& constraint = compiled.constraints[constraint_id];
        if (is_large_exactly_one_hyperedge(constraint)) {
            has_large_exactly_one = true;
            continue;
        }
        if (is_large_at_most_one_hyperedge(constraint)) {
            has_large_at_most_one = true;
            continue;
        }
        if (!is_binary_complement_constraint(constraint)) {
            all_constraints_supported = false;
            break;
        }
    }

    if (all_constraints_supported && has_large_exactly_one) {
        return ComponentKind::ExactlyOne;
    }
    if (all_constraints_supported && has_large_at_most_one) {
        return ComponentKind::AtMostOne;
    }

    if (component.variable_ids.size() <= 32) {
        return ComponentKind::SmallEnumerable;
    }
    return ComponentKind::GenericLinearBoolean;
}

[[nodiscard]] std::string component_hash_string(
    const CompiledComponent& component
) {
    return std::to_string(hash_components({component}));
}

}  // namespace

const char* component_kind_to_string(ComponentKind kind) noexcept {
    switch (kind) {
        case ComponentKind::SmallEnumerable:
            return "SmallEnumerable";
        case ComponentKind::ExactlyOne:
            return "ExactlyOne";
        case ComponentKind::AtMostOne:
            return "AtMostOne";
        case ComponentKind::ImplicationDag:
            return "ImplicationDag";
        case ComponentKind::WeightedThreshold:
            return "WeightedThreshold";
        case ComponentKind::GenericLinearBoolean:
            return "GenericLinearBoolean";
    }
    return "GenericLinearBoolean";
}

ComponentPartitionResult ComponentPartitioner::partition(
    const ConstraintGraph& graph,
    const CompiledConstraintSet& compiled
) const {
    ComponentPartitionResult result;
    DisjointSet set(graph.nodes.size());

    for (const auto& edge : graph.edges) {
        if (edge.var_ids.empty()) {
            continue;
        }
        const auto root = edge.var_ids.front();
        for (const auto var_id : edge.var_ids) {
            if (root < graph.nodes.size() && var_id < graph.nodes.size()) {
                set.unite(root, var_id);
            }
        }
    }

    std::unordered_map<std::size_t, ComponentId> root_to_component_id;
    for (const auto& node : graph.nodes) {
        const auto root = set.find(node.var_id);
        auto [it, inserted] = root_to_component_id.emplace(
            root,
            static_cast<ComponentId>(root_to_component_id.size())
        );
        if (inserted) {
            result.components.push_back(CompiledComponent{
                .component_id = it->second
            });
        }
        result.components[it->second].variable_ids.push_back(node.var_id);
    }

    for (const auto& edge : graph.edges) {
        if (edge.var_ids.empty()) {
            continue;
        }
        const auto root = set.find(edge.var_ids.front());
        const auto component_it = root_to_component_id.find(root);
        if (component_it == root_to_component_id.end()) {
            continue;
        }
        result.components[component_it->second].constraint_ids.push_back(
            edge.constraint_id
        );
    }

    for (auto& component : result.components) {
        std::sort(component.variable_ids.begin(), component.variable_ids.end());
        std::sort(
            component.constraint_ids.begin(),
            component.constraint_ids.end()
        );
        component.kind = classify_component(component, compiled);
        component.component_hash = component_hash_string(component);
    }

    std::sort(
        result.components.begin(),
        result.components.end(),
        [](const CompiledComponent& lhs, const CompiledComponent& rhs) {
            return lhs.variable_ids < rhs.variable_ids;
        }
    );
    for (std::size_t i = 0; i < result.components.size(); ++i) {
        result.components[i].component_id = static_cast<ComponentId>(i);
        result.components[i].component_hash =
            component_hash_string(result.components[i]);
    }

    result.partition_hash = hash_components(result.components);
    return result;
}

std::uint64_t hash_components(
    const std::vector<CompiledComponent>& components
) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (const auto& component : components) {
        hash_u32(&hash, component.component_id);
        hash_byte(&hash, static_cast<std::uint8_t>(component.kind));
        hash_u32(
            &hash,
            static_cast<std::uint32_t>(component.variable_ids.size())
        );
        for (const auto var_id : component.variable_ids) {
            hash_u32(&hash, var_id);
        }
        hash_u32(
            &hash,
            static_cast<std::uint32_t>(component.constraint_ids.size())
        );
        for (const auto constraint_id : component.constraint_ids) {
            hash_u32(&hash, constraint_id);
        }
    }
    return hash;
}

}  // namespace trading_engine::oracle
