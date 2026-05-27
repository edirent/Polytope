#include "feed/output/ReplayRunner.h"

#include <exception>
#include <iostream>
#include <string>

namespace {

int fail(const std::string& message) {
    std::cerr << "replay_raw_log failed: " << message << '\n';
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: replay_raw_log raw_log trace_csv\n";
        return 2;
    }

    try {
        trading_engine::feed::ReplayRunner runner(argv[1]);
        const auto summary = runner.replay_to_csv(argv[2]);

        std::cout
            << "packets_read: " << summary.packets_read << '\n'
            << "normalized_events: " << summary.normalized_events << '\n'
            << "trace_rows: " << summary.trace_rows << '\n';

        return 0;
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}
