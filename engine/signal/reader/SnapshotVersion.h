#pragma once

#include <cstdint>

namespace trading_engine::signal {

struct SnapshotVersion {
    std::uint64_t min_book_version = 0;
    std::uint64_t max_book_version = 0;
    std::uint64_t combined_hash = 0;
    std::uint64_t read_ts_ns = 0;
};

enum class SnapshotConsistencyMode : std::uint8_t {
    StrictSameVersion,
    BoundedSkew
};

}  // namespace trading_engine::signal
