#pragma once

#include "feed/raw_ingest/RawPacket.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace trading_engine::feed {

class RawLogReader {
public:
    void load(std::vector<RawPacket> packets);
    void reset() noexcept;

    [[nodiscard]] std::optional<RawPacket> next();
    [[nodiscard]] bool exhausted() const noexcept;

private:
    std::vector<RawPacket> packets_;
    std::size_t cursor_{0};
};

}  // namespace trading_engine::feed
