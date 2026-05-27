#pragma once

#include "feed/raw_ingest/RawPacket.h"

#include <cstdint>
#include <fstream>
#include <string>

namespace trading_engine::feed {

/**
 * @brief Append-only binary raw log writer.
 *
 * This is the real production/MVP writer.
 *
 * It writes packets in the format:
 *
 *   [RawPacketHeader][payload bytes]
 *   [RawPacketHeader][payload bytes]
 *   ...
 *
 * This file can later be read by RawLogReader and replayed deterministically.
 */
class RawLogWriter {
public:
    explicit RawLogWriter(const std::string& path);

    RawLogWriter(const RawLogWriter&) = delete;
    RawLogWriter& operator=(const RawLogWriter&) = delete;

    ~RawLogWriter();

    [[nodiscard]] bool is_open() const noexcept;

    void write_packet(const RawPacket& packet);
    void flush();

    [[nodiscard]] std::uint64_t packets_written() const noexcept;
    [[nodiscard]] std::uint64_t bytes_written() const noexcept;

private:
    std::ofstream out_;

    std::uint64_t packets_written_{0};
    std::uint64_t bytes_written_{0};
};

}  // namespace trading_engine::feed