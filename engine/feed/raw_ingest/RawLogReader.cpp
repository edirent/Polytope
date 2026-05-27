#include "feed/raw_ingest/RawLogReader.h"

#include <ios>
#include <stdexcept>
#include <streambuf>
#include <type_traits>
#include <utility>

namespace trading_engine::feed {

namespace {

/**
 * @brief Ensure RawPacketHeader can be read as raw binary bytes.
 *
 * RawLogReader reads RawPacketHeader directly from disk using:
 *
 *     in.read(reinterpret_cast<char*>(&header), sizeof(RawPacketHeader));
 *
 * That only makes sense if RawPacketHeader is trivially copyable.
 *
 * If someone later adds std::string, std::vector, virtual methods, or other
 * non-trivial fields to RawPacketHeader, this static_assert will fail.
 */
static_assert(
    std::is_trivially_copyable_v<RawPacketHeader>,
    "RawPacketHeader must be trivially copyable for binary raw-log reads"
);

/**
 * @brief Helper to construct a failed read result.
 */
RawLogReadResult make_error(RawLogReadError error, std::string message) {
    RawLogReadResult result;
    result.packet = std::nullopt;
    result.error = error;
    result.message = std::move(message);
    return result;
}

/**
 * @brief Helper to construct a successful read result.
 */
RawLogReadResult make_success(RawPacket packet) {
    RawLogReadResult result;
    result.packet = std::move(packet);
    result.error = RawLogReadError::None;
    result.message.clear();
    return result;
}

}  // namespace

/**
 * @brief Open a raw log file in binary read mode.
 *
 * The file must have been written by RawLogWriter using:
 *
 *     [RawPacketHeader][payload bytes]
 *
 * @param path Path to .raw file.
 *
 * @throws std::runtime_error if file cannot be opened.
 */
RawLogReader::RawLogReader(const std::string& path)
    : in_(path, std::ios::binary | std::ios::in) {
    if (!in_.is_open()) {
        throw std::runtime_error("RawLogReader failed to open file: " + path);
    }
}

/**
 * @brief Return whether the input stream is open.
 */
bool RawLogReader::is_open() const noexcept {
    return in_.is_open();
}

/**
 * @brief Read and validate the next RawPacket from the file.
 *
 * Read sequence:
 *
 * 1. Read RawPacketHeader.
 * 2. Detect clean EOF or truncated header.
 * 3. Validate magic/version/header_size.
 * 4. Read payload_len bytes.
 * 5. Detect truncated payload.
 * 6. Verify CRC32.
 * 7. Return RawPacket.
 *
 * This function does not decode payload JSON.
 * It only reconstructs raw packets safely.
 */
RawLogReadResult RawLogReader::next() {
    if (!in_.is_open()) {
        return make_error(
            RawLogReadError::IoError,
            "RawLogReader cannot read: file is not open"
        );
    }

    RawPacketHeader header{};

    in_.read(
        reinterpret_cast<char*>(&header),
        static_cast<std::streamsize>(sizeof(RawPacketHeader))
    );

    const auto header_bytes_read = in_.gcount();

    // Clean EOF: no bytes were read because file ended exactly after the last
    // complete packet.
    if (header_bytes_read == 0 && in_.eof()) {
        return make_error(
            RawLogReadError::EndOfFile,
            "end of raw log"
        );
    }

    // Partial header: some bytes were read, but not enough to form a full
    // RawPacketHeader. This means the file is truncated or corrupted.
    if (header_bytes_read != static_cast<std::streamsize>(sizeof(RawPacketHeader))) {
        return make_error(
            RawLogReadError::TruncatedHeader,
            "truncated raw packet header"
        );
    }

    // Validate format marker.
    if (header.magic != RAW_PACKET_MAGIC) {
        return make_error(
            RawLogReadError::BadMagic,
            "bad raw packet magic"
        );
    }

    // Validate raw packet version.
    if (header.version != RAW_PACKET_VERSION) {
        return make_error(
            RawLogReadError::UnsupportedVersion,
            "unsupported raw packet version"
        );
    }

    // For MVP, require exact header size.
    //
    // Later, if you introduce versioned headers, you can relax this and support
    // skipping extra header bytes.
    if (header.header_size != sizeof(RawPacketHeader)) {
        return make_error(
            RawLogReadError::BadHeaderSize,
            "bad raw packet header size"
        );
    }

    std::string payload;
    payload.resize(header.payload_len);

    if (header.payload_len > 0) {
        in_.read(
            payload.data(),
            static_cast<std::streamsize>(header.payload_len)
        );

        const auto payload_bytes_read = in_.gcount();

        if (payload_bytes_read != static_cast<std::streamsize>(header.payload_len)) {
            return make_error(
                RawLogReadError::TruncatedPayload,
                "truncated raw packet payload"
            );
        }
    }

    // Verify payload integrity.
    //
    // If this fails, replay must not treat the payload as trustworthy.
    const auto actual_crc = crc32(payload);

    if (actual_crc != header.payload_crc32) {
        return make_error(
            RawLogReadError::CrcMismatch,
            "raw packet payload CRC32 mismatch"
        );
    }

    RawPacket packet;
    packet.header = header;
    packet.payload = std::move(payload);

    ++packets_read_;

    bytes_read_ +=
        static_cast<std::uint64_t>(sizeof(RawPacketHeader)) +
        static_cast<std::uint64_t>(packet.header.payload_len);

    return make_success(std::move(packet));
}

/**
 * @brief Rewind reader to beginning of file.
 *
 * This is useful for replay determinism tests:
 *
 *     replay once
 *     reset
 *     replay again
 *
 * The resulting traces should be identical if the downstream pipeline is
 * deterministic.
 */
void RawLogReader::reset() {
    if (!in_.is_open()) {
        throw std::runtime_error("RawLogReader cannot reset: file is not open");
    }

    in_.clear();
    in_.seekg(0, std::ios::beg);

    if (!in_) {
        throw std::runtime_error("RawLogReader failed to seek to beginning");
    }

    packets_read_ = 0;
    bytes_read_ = 0;
}

/**
 * @brief Return number of complete packets successfully read.
 */
std::uint64_t RawLogReader::packets_read() const noexcept {
    return packets_read_;
}

/**
 * @brief Return number of complete packet bytes successfully read.
 */
std::uint64_t RawLogReader::bytes_read() const noexcept {
    return bytes_read_;
}

}  // namespace trading_engine::feed