#pragma once

#include "engine/execution/public/OrderPlan.h"

#include <cstddef>
#include <unordered_map>

namespace trading_engine::execution {

class ExecutionPlanStore {
public:
    void put(const OrderPlan& plan) {
        plans_[plan.plan_id] = plan;
    }

    [[nodiscard]] const OrderPlan* find(PlanId plan_id) const {
        const auto it = plans_.find(plan_id);
        return it == plans_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return plans_.size();
    }

private:
    std::unordered_map<PlanId, OrderPlan> plans_;
};

}  // namespace trading_engine::execution
