#pragma once

#include <string>

namespace trading_engine::decode {

enum class DecodeErrorCode {
    None = 0,
    NonJsonControl,
    UnsupportedCodec,
    MalformedJson,
    UnsupportedJson,
    EmptyPayload,
    InternalError
};

enum class DecodeErrorSeverity {
    Info = 0,
    Warning,
    Error,
    Fatal
};

struct DecodeError {
    DecodeErrorCode code{DecodeErrorCode::None};
    DecodeErrorSeverity severity{DecodeErrorSeverity::Info};
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return code == DecodeErrorCode::None ||
               code == DecodeErrorCode::NonJsonControl;
    }
};

[[nodiscard]] std::string to_string(DecodeErrorCode code);
[[nodiscard]] std::string to_string(DecodeErrorSeverity severity);

}  // namespace trading_engine::decode
