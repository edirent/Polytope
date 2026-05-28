#pragma once

#include "decode/core/DecodePipelineResult.h"
#include "decode/json/JsonDecoder.h"
#include "decode/normalize/EventNormalizer.h"
#include "decode/public/DecodeInputView.h"
#include "decode/public/NormalizedEventBatch.h"

namespace trading_engine::decode {

class DecodePipeline {
public:
    [[nodiscard]] DecodePipelineResult decode(
        const DecodeInputView& input,
        NormalizedEventBatch* out
    ) const;

private:
    JsonDecoder decoder_;
    EventNormalizer normalizer_;
};

}  // namespace trading_engine::decode
