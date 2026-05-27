#pragma once

namespace trading_engine::execution {

class IExecutionAdapter {
public:
    virtual ~IExecutionAdapter() = default;
};

}  // namespace trading_engine::execution
