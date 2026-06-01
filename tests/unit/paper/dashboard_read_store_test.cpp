#include "engine/paper/read/DashboardReadStore.h"

#include <iostream>
#include <string>

namespace {

using trading_engine::paper::DashboardReadStore;
using trading_engine::paper::DashboardSnapshot;

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

template <typename T, typename U>
int expect_equal(const T& actual, const U& expected, const char* message) {
    if (!(actual == expected)) {
        std::cerr << message << ": expected " << expected << ", got " << actual
                  << '\n';
        return 1;
    }
    return 0;
}

DashboardSnapshot snapshot(std::uint64_t ts_ns, std::int64_t cash_tick) {
    DashboardSnapshot out;
    out.ts_ns = ts_ns;
    out.account.cash_balance_tick = cash_tick;
    out.performance.intents_observed = ts_ns;
    out.signal.output_hash = ts_ns * 17;
    return out;
}

int test_publishes_latest() {
    DashboardReadStore store{4};
    const auto seq = store.publish(snapshot(10, 1000));
    if (const auto check = expect_equal(seq, 1ULL, "published seq");
        check != 0) {
        return check;
    }

    const auto latest = store.latest();
    if (!latest) {
        return fail("latest snapshot missing");
    }
    if (const auto check = expect_equal(latest->seq_no, 1ULL, "latest seq");
        check != 0) {
        return check;
    }
    return expect_equal(
        latest->account.cash_balance_tick,
        1000LL,
        "latest account cash"
    );
}

int test_reads_since_seq() {
    DashboardReadStore store{4};
    (void)store.publish(snapshot(10, 1000));
    (void)store.publish(snapshot(20, 2000));
    (void)store.publish(snapshot(30, 3000));

    const auto frames = store.read_since(1);
    if (const auto check = expect_equal(frames.size(), 2ULL, "frame count");
        check != 0) {
        return check;
    }
    if (const auto check = expect_equal(frames[0].seq_no, 2ULL, "first seq");
        check != 0) {
        return check;
    }
    return expect_equal(frames[1].seq_no, 3ULL, "second seq");
}

int test_drops_old_frames_when_ring_full() {
    DashboardReadStore store{2};
    (void)store.publish(snapshot(10, 1000));
    (void)store.publish(snapshot(20, 2000));
    (void)store.publish(snapshot(30, 3000));

    const auto frames = store.read_since(0);
    if (const auto check = expect_equal(frames.size(), 2ULL, "frame count");
        check != 0) {
        return check;
    }
    if (const auto check = expect_equal(frames[0].seq_no, 2ULL, "oldest seq");
        check != 0) {
        return check;
    }
    return expect_equal(frames[1].seq_no, 3ULL, "latest seq");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return fail("expected one test case name");
    }

    const std::string test_case{argv[1]};
    if (test_case == "DashboardReadStore_PublishesLatest") {
        return test_publishes_latest();
    }
    if (test_case == "DashboardReadStore_ReadsSinceSeq") {
        return test_reads_since_seq();
    }
    if (test_case == "DashboardReadStore_DropsOldFramesWhenRingFull") {
        return test_drops_old_frames_when_ring_full();
    }

    return fail("unknown test case");
}
