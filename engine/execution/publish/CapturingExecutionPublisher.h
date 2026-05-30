#pragma once

#include "engine/execution/publish/ExecutionReportPublisher.h"

#include <vector>

namespace trading_engine::execution {

class CapturingExecutionPublisher final : public ExecutionReportPublisher {
public:
    void publish(const ExecutionReport& report) override {
        reports_.push_back(report);
    }

    [[nodiscard]] const std::vector<ExecutionReport>& reports() const noexcept {
        return reports_;
    }

private:
    std::vector<ExecutionReport> reports_;
};

}  // namespace trading_engine::execution
