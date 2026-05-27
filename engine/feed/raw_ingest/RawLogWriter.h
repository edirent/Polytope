#pragma once

#include "feed/raw_ingest/RawPacket.h"

#include <vector>

namespace trading_engine::feed {

class RawLogWriter {
public:
    void append(RawPacket packet);
    void clear() noexcept;

    [[nodiscard]] const std::vector<RawPacket>& packets() const noexcept;

private:
    std::vector<RawPacket> packets_;
};

}  // namespace trading_engine::feed
