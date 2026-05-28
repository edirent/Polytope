#pragma once

#include "decode/json/JsonDecodeResult.h"
#include "decode/public/DecodeError.h"

#include <cstddef>

namespace trading_engine::decode {

struct DecodePipelineResult {
    DecodeError error;
    JsonDecodeKind payload_kind{JsonDecodeKind::MalformedJson};
    std::size_t events_emitted{0};

    [[nodiscard]] bool ok() const noexcept {
        return error.ok();
    }
};

}  // namespace trading_engine::decode
