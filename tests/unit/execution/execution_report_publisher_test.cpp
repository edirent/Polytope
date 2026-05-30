#include "engine/execution/publish/CapturingExecutionPublisher.h"
#include "engine/execution/publish/JsonlExecutionReportWriter.h"
#include "engine/execution/publish/ReservationDispositionPublisher.h"

#include <boost/json.hpp>

#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trading_engine::execution::CapturingExecutionPublisher;
using trading_engine::execution::ChildOrderStatus;
using trading_engine::execution::ExecutionReport;
using trading_engine::execution::JsonlExecutionReportWriter;
using trading_engine::execution::ReservationDisposition;
using trading_engine::execution::ReservationDispositionPublisher;
using trading_engine::execution::ReservationDispositionType;

namespace json = boost::json;

class CapturingReservationDispositionPublisher final
    : public ReservationDispositionPublisher {
public:
    void publish(const ReservationDisposition& disposition) override {
        dispositions_.push_back(disposition);
    }

    [[nodiscard]] const std::vector<ReservationDisposition>& dispositions()
        const noexcept {
        return dispositions_;
    }

private:
    std::vector<ReservationDisposition> dispositions_;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect_true(bool value, const std::string& field) {
    if (!value) {
        fail("expected true: " + field);
    }
}

template <typename Actual, typename Expected>
void expect_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string& field
) {
    if (!(actual == expected)) {
        fail("mismatch: " + field);
    }
}

ExecutionReport sample_report() {
    ExecutionReport report;
    report.plan_id = 11;
    report.child_order_id = 22;
    report.status = ChildOrderStatus::Filled;
    report.venue_order_id = "venue-22";
    report.reject_reason = "none";
    report.filled_lots = 10;
    report.remaining_lots = 0;
    report.avg_fill_price_tick = 800;
    report.event_ts_ns = 1234;
    return report;
}

void CapturingExecutionPublisher_CapturesReport() {
    CapturingExecutionPublisher publisher;
    const auto report = sample_report();

    publisher.publish(report);

    expect_equal(
        publisher.reports().size(),
        static_cast<std::size_t>(1),
        "report count"
    );
    expect_equal(publisher.reports()[0].plan_id, 11ULL, "plan id");
    expect_equal(
        publisher.reports()[0].child_order_id,
        22ULL,
        "child order id"
    );
}

void CapturingExecutionPublisher_PreservesOrder() {
    CapturingExecutionPublisher publisher;
    auto first = sample_report();
    auto second = sample_report();
    second.child_order_id = 23;

    publisher.publish(first);
    publisher.publish(second);

    expect_equal(publisher.reports()[0].child_order_id, 22ULL, "first");
    expect_equal(publisher.reports()[1].child_order_id, 23ULL, "second");
}

void JsonlExecutionReportWriter_WritesValidJsonLine() {
    std::ostringstream output;
    JsonlExecutionReportWriter writer(&output);

    writer.publish(sample_report());

    const auto parsed = json::parse(output.str());
    expect_true(parsed.is_object(), "json object");
    const auto& object = parsed.as_object();
    expect_equal(object.at("plan_id").to_number<std::uint64_t>(), 11ULL, "plan id");
    expect_equal(
        object.at("child_order_id").to_number<std::uint64_t>(),
        22ULL,
        "child order id"
    );
    expect_equal(
        std::string{object.at("status").as_string().c_str()},
        std::string{"Filled"},
        "status"
    );
    expect_equal(object.at("filled_lots").as_int64(), 10LL, "filled lots");
    expect_equal(
        object.at("avg_fill_price_tick").as_int64(),
        800LL,
        "avg price"
    );
}

void JsonlExecutionReportWriter_EscapesStrings() {
    std::ostringstream output;
    JsonlExecutionReportWriter writer(&output);
    auto report = sample_report();
    report.reject_reason = "bad \"quote\"";

    writer.publish(report);

    const auto parsed = json::parse(output.str());
    expect_equal(
        std::string{parsed.as_object().at("reject_reason").as_string().c_str()},
        std::string{"bad \"quote\""},
        "escaped reject reason"
    );
}

void ReservationDispositionPublisher_CapturesDisposition() {
    CapturingReservationDispositionPublisher publisher;

    publisher.publish({
        .reservation_id = "r-1",
        .plan_id = 42,
        .type = ReservationDispositionType::Consume,
        .reason = "filled"
    });

    expect_equal(
        publisher.dispositions().size(),
        static_cast<std::size_t>(1),
        "disposition count"
    );
    expect_equal(
        publisher.dispositions()[0].type,
        ReservationDispositionType::Consume,
        "consume"
    );
}

using TestFn = void (*)();

const std::unordered_map<std::string, TestFn>& tests() {
    static const std::unordered_map<std::string, TestFn> test_map{
        {
            "CapturingExecutionPublisher_CapturesReport",
            &CapturingExecutionPublisher_CapturesReport
        },
        {
            "CapturingExecutionPublisher_PreservesOrder",
            &CapturingExecutionPublisher_PreservesOrder
        },
        {
            "JsonlExecutionReportWriter_WritesValidJsonLine",
            &JsonlExecutionReportWriter_WritesValidJsonLine
        },
        {
            "JsonlExecutionReportWriter_EscapesStrings",
            &JsonlExecutionReportWriter_EscapesStrings
        },
        {
            "ReservationDispositionPublisher_CapturesDisposition",
            &ReservationDispositionPublisher_CapturesDisposition
        }
    };
    return test_map;
}

int run_test(const std::string& name) {
    const auto it = tests().find(name);
    if (it == tests().end()) {
        std::cerr << "unknown test: " << name << '\n';
        return 2;
    }

    try {
        it->second();
    } catch (const std::exception& error) {
        std::cerr << name << " failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << name << " passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: execution_report_publisher_tests <test-name>\n";
        return 2;
    }
    return run_test(argv[1]);
}
