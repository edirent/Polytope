#include "feed_decode_test_utils.h"

#include <exception>
#include <iostream>

namespace {

void FeedDecodeDeterminism_Market39TraceHashStable() {
    const auto baseline = feed_decode_test::run_market39_decode();
    feed_decode_test::expect_market39_counts(baseline.counts);

    for (int i = 0; i < 10; ++i) {
        const auto actual = feed_decode_test::run_market39_decode();

        feed_decode_test::expect_equal(
            actual.normalized_trace_hash,
            baseline.normalized_trace_hash,
            "normalized_trace_hash"
        );
        feed_decode_test::expect_equal(
            actual.counts.normalized_events,
            baseline.counts.normalized_events,
            "normalized_events"
        );
        feed_decode_test::expect_equal(
            actual.counts.snapshot_events,
            baseline.counts.snapshot_events,
            "snapshot_events"
        );
        feed_decode_test::expect_equal(
            actual.counts.delta_events,
            baseline.counts.delta_events,
            "delta_events"
        );
        feed_decode_test::expect_equal(
            actual.counts.heartbeat_events,
            baseline.counts.heartbeat_events,
            "heartbeat_events"
        );
        feed_decode_test::expect_equal(
            actual.counts.decode_errors,
            baseline.counts.decode_errors,
            "decode_errors"
        );
    }
}

}  // namespace

int main() {
    try {
        FeedDecodeDeterminism_Market39TraceHashStable();
    } catch (const std::exception& error) {
        std::cerr
            << "FeedDecodeDeterminism_Market39TraceHashStable failed: "
            << error.what() << '\n';
        return 1;
    }

    std::cout << "FeedDecodeDeterminism_Market39TraceHashStable passed\n";
    return 0;
}
