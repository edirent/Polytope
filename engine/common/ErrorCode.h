#pragma once

namespace trading_engine::common {

enum class ErrorCode {
    Ok = 0,
    InvalidConfig,
    FeedDisconnected,
    DecodeError,
    RiskRejected,
    ExecutionRejected
};

}  // namespace trading_engine::common
