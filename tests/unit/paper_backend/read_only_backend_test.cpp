#include "apps/paper_backend/ReadOnlyHttpServer.h"
#include "apps/paper_backend/SsePublisher.h"
#include "engine/paper/read/DashboardReadStore.h"

#include <iostream>
#include <string>

namespace {

using trading_engine::paper::DashboardReadStore;
using trading_engine::paper::DashboardSnapshot;
using trading_engine::paper_backend::DashboardApiRoutes;
using trading_engine::paper_backend::HttpMethod;
using trading_engine::paper_backend::HttpRequest;
using trading_engine::paper_backend::ReadOnlyHttpServer;
using trading_engine::paper_backend::SsePublisher;

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

DashboardSnapshot snapshot(std::uint64_t seq_no = 1) {
    DashboardSnapshot out;
    out.seq_no = seq_no;
    out.ts_ns = 1000 + seq_no;
    out.account.starting_cash_tick = 1'000'000;
    out.account.cash_balance_tick = 999'000;
    out.account.unrealized_pnl_tick = 500;
    out.performance.execution_reports_observed = 1;
    out.signal.intents_published = 2;
    out.risk.decisions = 3;
    out.execution.plans_filled = 1;
    out.filled_orders.resize(1);
    out.filled_orders[0].asset_id = "asset_yes";
    out.filled_orders[0].filled_lots = 10;
    out.filled_orders[0].unrealized_pnl_tick = 2000;
    return out;
}

ReadOnlyHttpServer server_with_snapshot() {
    static DashboardReadStore store{8};
    (void)store.publish(snapshot());
    return ReadOnlyHttpServer{DashboardApiRoutes{&store}};
}

int test_allows_get() {
    auto server = server_with_snapshot();
    const auto health = server.handle_request({
        HttpMethod::Get,
        "/api/v1/health"
    });
    if (const auto check = expect_equal(health.status, 200, "health status");
        check != 0) {
        return check;
    }

    const auto latest = server.handle_request({
        HttpMethod::Get,
        "/api/v1/snapshot/latest"
    });
    if (const auto check = expect_equal(latest.status, 200, "latest status");
        check != 0) {
        return check;
    }
    if (latest.body.find("\"seq_no\"") == std::string::npos) {
        return fail("latest snapshot JSON missing seq_no");
    }
    if (latest.body.find("\"filled_orders\"") == std::string::npos) {
        return fail("latest snapshot JSON missing filled_orders");
    }

    const auto reports = server.handle_request({
        HttpMethod::Get,
        "/api/v1/execution-reports"
    });
    if (const auto check = expect_equal(reports.status, 200, "reports status");
        check != 0) {
        return check;
    }
    if (reports.body.find("asset_yes") == std::string::npos) {
        return fail("execution reports missing filled order asset");
    }

    const auto head = server.handle_request({
        HttpMethod::Head,
        "/api/v1/performance"
    });
    if (const auto check = expect_equal(head.status, 200, "head status");
        check != 0) {
        return check;
    }
    return expect_equal(head.body.size(), 0ULL, "HEAD empty body");
}

int test_rejects_post() {
    const ReadOnlyHttpServer server;
    const auto response = server.handle_request({
        HttpMethod::Post,
        "/api/v1/snapshot/latest"
    });
    return expect_equal(response.status, 405, "POST status");
}

int test_rejects_put() {
    const ReadOnlyHttpServer server;
    const auto response = server.handle_request({
        HttpMethod::Put,
        "/api/v1/snapshot/latest"
    });
    return expect_equal(response.status, 405, "PUT status");
}

int test_rejects_delete() {
    const ReadOnlyHttpServer server;
    const auto response = server.handle_request({
        HttpMethod::Delete,
        "/api/v1/snapshot/latest"
    });
    return expect_equal(response.status, 405, "DELETE status");
}

int test_sse_does_not_block_runtime() {
    SsePublisher publisher{2};
    for (std::uint64_t seq = 1; seq <= 100; ++seq) {
        publisher.publish(snapshot(seq));
    }

    if (const auto check =
            expect_equal(publisher.latest_seq(), 100ULL, "latest seq");
        check != 0) {
        return check;
    }
    if (publisher.dropped_events() == 0) {
        return fail("SSE publisher should drop old frames instead of blocking");
    }

    const auto events = publisher.read_since(98);
    if (const auto check = expect_equal(events.size(), 2ULL, "event count");
        check != 0) {
        return check;
    }
    return expect_equal(events.back().seq_no, 100ULL, "last event seq");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return fail("expected one test case name");
    }

    const std::string test_case{argv[1]};
    if (test_case == "ReadOnlyApi_AllowsGet") {
        return test_allows_get();
    }
    if (test_case == "ReadOnlyApi_RejectsPost") {
        return test_rejects_post();
    }
    if (test_case == "ReadOnlyApi_RejectsPut") {
        return test_rejects_put();
    }
    if (test_case == "ReadOnlyApi_RejectsDelete") {
        return test_rejects_delete();
    }
    if (test_case == "Sse_DoesNotBlockRuntime") {
        return test_sse_does_not_block_runtime();
    }

    return fail("unknown test case");
}
