#pragma once

#include "engine/execution/publish/ExecutionReportPublisher.h"

#include <ostream>
#include <string>

namespace trading_engine::execution {

class JsonlExecutionReportWriter final : public ExecutionReportPublisher {
public:
    explicit JsonlExecutionReportWriter(std::ostream* output)
        : output_(output) {}

    void publish(const ExecutionReport& report) override {
        if (output_ == nullptr) {
            return;
        }
        *output_ << "{\"plan_id\":" << report.plan_id
                 << ",\"child_order_id\":" << report.child_order_id
                 << ",\"status\":\"" << to_string(report.status)
                 << "\",\"venue_order_id\":\""
                 << escape_json(report.venue_order_id)
                 << "\",\"reject_reason\":\""
                 << escape_json(report.reject_reason)
                 << "\",\"filled_lots\":" << report.filled_lots
                 << ",\"remaining_lots\":" << report.remaining_lots
                 << ",\"avg_fill_price_tick\":"
                 << report.avg_fill_price_tick
                 << ",\"event_ts_ns\":" << report.event_ts_ns
                 << "}\n";
    }

private:
    [[nodiscard]] static std::string escape_json(const std::string& value) {
        std::string escaped;
        escaped.reserve(value.size());
        for (const char ch : value) {
            switch (ch) {
                case '\\':
                    escaped += "\\\\";
                    break;
                case '"':
                    escaped += "\\\"";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                default:
                    escaped += ch;
                    break;
            }
        }
        return escaped;
    }

    std::ostream* output_ = nullptr;
};

}  // namespace trading_engine::execution
