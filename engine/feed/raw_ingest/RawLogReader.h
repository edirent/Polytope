#pragma once

#include "feed/raw_ingest/RawPacket.h"

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>

namespace trading_engine::feed {

/**
 * @brief Error code returned by RawLogReader.
 *
 * RawLogReader should not silently ignore corrupted logs.
 * Every failure mode should be explicit so replay can decide whether to stop,
 * skip, or mark the log as unsafe.
 */
enum class RawLogReadError {
    None = 0,

    /**
     * @brief Clean end of file.
     *
     * This is not a corruption error. It simply means there are no more packets.
     */
    EndOfFile,

    /**
     * @brief Could not read a full RawPacketHeader.
     *
     * This usually means the file ended in the middle of a header.
     */
    TruncatedHeader,

    /**
     * @brief Header magic does not match RAW_PACKET_MAGIC.
     *
     * This means the reader is misaligned, the file is corrupted, or the file
     * is not a Helix raw log.
     */
    BadMagic,

    /**
     * @brief Header version is not supported by this reader.
     */
    UnsupportedVersion,

    /**
     * @brief Header size does not match sizeof(RawPacketHeader).
     *
     * For MVP, we require exact match.
     * Future versions may support variable header sizes.
     */
    BadHeaderSize,

    /**
     * @brief Payload ended before payload_len bytes were read.
     */
    TruncatedPayload,

    /**
     * @brief Recomputed CRC32 does not match header.payload_crc32.
     */
    CrcMismatch,

    /**
     * @brief Generic file I/O failure.
     */
    IoError
};

/**
 * @brief Result returned by RawLogReader::next().
 *
 * If packet has value and error == None, read succeeded.
 *
 * If error == EndOfFile, replay is complete.
 *
 * Any other error means the raw log is not safe to continue consuming without
 * an explicit policy decision.
 */
struct RawLogReadResult {
    std::optional<RawPacket> packet;
    RawLogReadError error{RawLogReadError::None};
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return packet.has_value() && error == RawLogReadError::None;
    }

    [[nodiscard]] bool eof() const noexcept {
        return !packet.has_value() && error == RawLogReadError::EndOfFile;
    }
};

/**
 * @brief File-backed binary raw log reader.
 *
 * This is the counterpart of RawLogWriter.
 *
 * It reads packets written in this format:
 *
 *     [RawPacketHeader][payload bytes]
 *     [RawPacketHeader][payload bytes]
 *     [RawPacketHeader][payload bytes]
 *
 * This reader is used by:
 *
 * - ReplayRunner
 * - raw log validation tests
 * - offline debugging tools
 *
 * It should not decode JSON. It only reconstructs RawPacket objects.
 */
class RawLogReader {
public:
    /**
     * @brief Open a binary raw log file for reading.
     *
     * @throws std::runtime_error if the file cannot be opened.
     */
    explicit RawLogReader(const std::string& path);

    RawLogReader(const RawLogReader&) = delete;
    RawLogReader& operator=(const RawLogReader&) = delete;

    RawLogReader(RawLogReader&&) = delete;
    RawLogReader& operator=(RawLogReader&&) = delete;

    /**
     * @brief Return whether the underlying input file is open.
     */
    [[nodiscard]] bool is_open() const noexcept;

    /**
     * @brief Read the next packet from the raw log.
     *
     * Returns:
     *
     * - packet + None on success
     * - nullopt + EndOfFile on clean EOF
     * - nullopt + specific error on corruption / invalid data
     */
    [[nodiscard]] RawLogReadResult next();

    /**
     * @brief Rewind reader to beginning of file.
     *
     * Useful for deterministic replay tests.
     */
    void reset();

    /**
     * @brief Number of packets successfully read by this reader instance.
     */
    [[nodiscard]] std::uint64_t packets_read() const noexcept;

    /**
     * @brief Number of bytes successfully read by this reader instance.
     *
     * Counts only complete packets:
     *
     *     sizeof(RawPacketHeader) + payload_len
     */
    [[nodiscard]] std::uint64_t bytes_read() const noexcept;

private:
    std::ifstream in_;

    std::uint64_t packets_read_{0};
    std::uint64_t bytes_read_{0};
};

}  // namespace trading_engine::feed