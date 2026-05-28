#include "decode/core/DecodePipeline.h"

#include <string>
#include <utility>

namespace trading_engine::decode {

namespace {

constexpr std::uint16_t kCodecNone = 0;

DecodePipelineResult make_result(
    DecodeErrorCode code,
    DecodeErrorSeverity severity,
    std::string message,
    JsonDecodeKind kind = JsonDecodeKind::MalformedJson
) {
    DecodePipelineResult result;
    result.error.code = code;
    result.error.severity = severity;
    result.error.message = std::move(message);
    result.payload_kind = kind;
    return result;
}

}  // namespace

DecodePipelineResult DecodePipeline::decode(
    const DecodeInputView& input,
    NormalizedEventBatch* out
) const {
    if (!out) {
        return make_result(
            DecodeErrorCode::InternalError,
            DecodeErrorSeverity::Fatal,
            "DecodePipeline output batch is null"
        );
    }

    out->clear();

    if (input.codec != kCodecNone) {
        return make_result(
            DecodeErrorCode::UnsupportedCodec,
            DecodeErrorSeverity::Error,
            "unsupported payload codec"
        );
    }

    const JsonDecodeResult decoded = decoder_.decode(input);

    DecodePipelineResult result;
    result.payload_kind = decoded.kind;

    if (!decoded.ok()) {
        result.error.code = decoded.error;
        result.error.severity = DecodeErrorSeverity::Error;
        result.error.message = decoded.message;
        return result;
    }

    NormalizationResult normalized;

    if (decoded.has_json_event_payload()) {
        normalized = normalizer_.normalize_json(input, decoded.json);
    } else if (decoded.has_control_payload()) {
        normalized = normalizer_.normalize_control(
            input,
            decoded.control_payload
        );
        result.error.code = DecodeErrorCode::NonJsonControl;
        result.error.severity = DecodeErrorSeverity::Info;
    }

    out->warnings.insert(
        out->warnings.end(),
        normalized.warnings.begin(),
        normalized.warnings.end()
    );

    if (!normalized.ok()) {
        result.error.code = DecodeErrorCode::InternalError;
        result.error.severity = DecodeErrorSeverity::Error;
        result.error.message = normalized.error;
        return result;
    }

    for (const auto& event : normalized.events) {
        if (!out->push_back(event)) {
            result.error.code = DecodeErrorCode::InternalError;
            result.error.severity = DecodeErrorSeverity::Error;
            result.error.message = "too many normalized events in packet";
            return result;
        }
    }

    result.events_emitted = out->size();
    return result;
}

}  // namespace trading_engine::decode
