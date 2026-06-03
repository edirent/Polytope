#pragma once

#include "engine/execution/adapter/IExecutionAdapter.h"
#include "engine/execution/adapter/LiveOrderBridge.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace trading_engine::execution {

class LiveExecutionAdapter final : public IExecutionAdapter {
public:
    LiveExecutionAdapter() = default;
    LiveExecutionAdapter(
        LiveExecutionConfig config,
        ILiveOrderSigner* signer,
        ILiveOrderTransport* transport
    );

    [[nodiscard]] AdapterSubmitResult submit_plan(
        const OrderPlan& plan,
        const ExecutionContext& context
    ) override;

    [[nodiscard]] std::vector<ExecutionReport> poll_reports() override;

    [[nodiscard]] AdapterCancelResult cancel_plan(
        std::uint64_t plan_id
    ) override;

private:
    struct AcceptedLiveOrder {
        ChildOrderId child_order_id = 0;
        std::string venue_order_id;
    };

    [[nodiscard]] AdapterSubmitResult disabled_submit(
        const OrderPlan& plan,
        const ExecutionContext& context
    ) const;

    [[nodiscard]] bool live_context_enabled(
        const ExecutionContext& context
    ) const noexcept;

    [[nodiscard]] bool side_allowed(OrderSide side) const noexcept;

    [[nodiscard]] bool child_notional_allowed(
        const ChildOrder& order
    ) const noexcept;

    [[nodiscard]] LiveOrderRequest make_request(
        const OrderPlan& plan,
        const ChildOrder& order
    ) const;

    void push_rejection(
        const OrderPlan& plan,
        const ChildOrder& order,
        std::uint64_t now_ns,
        std::string reason
    );

    LiveExecutionConfig config_;
    ILiveOrderSigner* signer_ = nullptr;
    ILiveOrderTransport* transport_ = nullptr;

    std::vector<ExecutionReport> pending_reports_;
    std::unordered_map<PlanId, std::vector<AcceptedLiveOrder>>
        accepted_orders_by_plan_;
};

}  // namespace trading_engine::execution
