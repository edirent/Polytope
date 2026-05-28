#include "feed_decode_test_utils.h"

#include <exception>
#include <iostream>

namespace {

using trading_engine::decode::DecodeErrorCode;
using trading_engine::decode::DecodePipeline;
using trading_engine::decode::JsonDecodeKind;
using trading_engine::decode::NormalizedEventBatch;
using trading_engine::decode::NormalizedEventType;
using trading_engine::feed::to_decode_input_view;

void FeedDecodeError_EmptyPayloadDoesNotCrash() {
    DecodePipeline pipeline;
    NormalizedEventBatch batch;
    const auto packet = feed_decode_test::make_test_packet("");

    const auto result = pipeline.decode(to_decode_input_view(packet), &batch);

    feed_decode_test::expect_false(result.ok(), "decode ok");
    feed_decode_test::expect_equal(
        result.payload_kind,
        JsonDecodeKind::MalformedJson,
        "payload kind"
    );
    feed_decode_test::expect_equal(
        result.error.code,
        DecodeErrorCode::EmptyPayload,
        "error code"
    );
    feed_decode_test::expect_true(batch.empty(), "batch empty");
}

void FeedDecodeError_MalformedJsonDoesNotCrash() {
    DecodePipeline pipeline;
    NormalizedEventBatch batch;
    const auto packet = feed_decode_test::make_test_packet(R"({"event_type":)");

    const auto result = pipeline.decode(to_decode_input_view(packet), &batch);

    feed_decode_test::expect_false(result.ok(), "decode ok");
    feed_decode_test::expect_equal(
        result.payload_kind,
        JsonDecodeKind::MalformedJson,
        "payload kind"
    );
    feed_decode_test::expect_equal(
        result.error.code,
        DecodeErrorCode::MalformedJson,
        "error code"
    );
    feed_decode_test::expect_true(batch.empty(), "batch empty");
}

void FeedDecodeError_UnsupportedCodecRejected() {
    DecodePipeline pipeline;
    NormalizedEventBatch batch;
    auto packet = feed_decode_test::make_test_packet(R"({"event_type":"book"})");
    packet.header.codec = trading_engine::feed::Codec::Gzip;

    const auto result = pipeline.decode(to_decode_input_view(packet), &batch);

    feed_decode_test::expect_false(result.ok(), "decode ok");
    feed_decode_test::expect_equal(
        result.error.code,
        DecodeErrorCode::UnsupportedCodec,
        "error code"
    );
    feed_decode_test::expect_true(batch.empty(), "batch empty");
}

void FeedDecodeError_ControlPayloadIsNotMalformed() {
    DecodePipeline pipeline;
    NormalizedEventBatch batch;
    const auto packet = feed_decode_test::make_test_packet("PONG");

    const auto result = pipeline.decode(to_decode_input_view(packet), &batch);

    feed_decode_test::expect_true(result.ok(), "decode ok");
    feed_decode_test::expect_equal(
        result.payload_kind,
        JsonDecodeKind::NonJsonControl,
        "payload kind"
    );
    feed_decode_test::expect_equal(
        result.error.code,
        DecodeErrorCode::NonJsonControl,
        "error code"
    );
    feed_decode_test::expect_equal(batch.size(), 1U, "event count");
    feed_decode_test::expect_equal(
        batch.events.front().event_type,
        NormalizedEventType::Heartbeat,
        "event type"
    );
}

}  // namespace

int main() {
    try {
        FeedDecodeError_EmptyPayloadDoesNotCrash();
        FeedDecodeError_MalformedJsonDoesNotCrash();
        FeedDecodeError_UnsupportedCodecRejected();
        FeedDecodeError_ControlPayloadIsNotMalformed();
    } catch (const std::exception& error) {
        std::cerr << "feed_decode_error_test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "feed_decode_error_test passed\n";
    return 0;
}
