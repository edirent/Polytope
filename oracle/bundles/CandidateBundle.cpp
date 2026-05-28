#include "oracle/bundles/CandidateBundle.h"

namespace trading_engine::oracle {

const char* side_to_string(Side side) noexcept {
    switch (side) {
        case Side::Buy:
            return "Buy";
        case Side::Sell:
            return "Sell";
    }

    return "Buy";
}

bool side_from_string(
    const std::string& value,
    Side* out
) noexcept {
    if (value == "Buy") {
        if (out) {
            *out = Side::Buy;
        }
        return true;
    }

    if (value == "Sell") {
        if (out) {
            *out = Side::Sell;
        }
        return true;
    }

    return false;
}

}  // namespace trading_engine::oracle
