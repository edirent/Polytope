#pragma once

#include "DecodeError.h"

namespace trading_engine::decode {

struct DecodeResult {
    DecodeError error;

    [[nodiscard]] bool ok() const noexcept {
        return error.ok();
    }
};

}  // namespace trading_engine::decode
