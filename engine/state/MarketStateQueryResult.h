#pragma once

#include <cstdint>
#include <string>

namespace trading_engine::state {

enum class StateQueryError : std::uint8_t {
    None = 0,
    MissingEntity,
    Recovering,
    Closed,
    Resolved,
    EmptyBook,
    MissingBid,
    MissingAsk,
    CrossedBook,
    InvalidDepth
};

template <typename T>
struct StateQueryResult {
    bool ok{false};
    StateQueryError error{StateQueryError::None};
    std::string entity_id;
    std::uint64_t version{0};
    T value{};
};

[[nodiscard]] std::string to_string(StateQueryError error);

}  // namespace trading_engine::state
