#include "feed_decode_test_utils.h"

#include <exception>
#include <iostream>

namespace {

void FeedDecodePipeline_Market39CountsStable() {
    const auto run = feed_decode_test::run_market39_decode();
    feed_decode_test::expect_market39_counts(run.counts);
}

}  // namespace

int main() {
    try {
        FeedDecodePipeline_Market39CountsStable();
    } catch (const std::exception& error) {
        std::cerr
            << "FeedDecodePipeline_Market39CountsStable failed: "
            << error.what() << '\n';
        return 1;
    }

    std::cout << "FeedDecodePipeline_Market39CountsStable passed\n";
    return 0;
}
