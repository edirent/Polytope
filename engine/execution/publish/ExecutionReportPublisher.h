#pragma once

#include "engine/execution/public/ExecutionReport.h"

namespace trading_engine::execution {

class ExecutionReportPublisher {
public:
    virtual ~ExecutionReportPublisher() = default;

    virtual void publish(const ExecutionReport& report) = 0;
};

}  // namespace trading_engine::execution
