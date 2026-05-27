#include "feed/raw_ingest/RawLogReader.h"
#include "feed/raw_ingest/RawPacket.h"

#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

namespace {

using trading_engine::feed::RAW_PACKET_MAGIC;
using trading_engine::feed::RAW_PACKET_VERSION;
using trading_engine::feed::RawLogReadResult;
using trading_engine::feed::RawLogReader;
using trading_engine::feed::RawPacket;
using trading_engine::feed::RawPacketHeader;
using trading_engine::feed::crc32;

int fail(const std::string& message) {
    std::cerr << "validate_raw_log failed: " << message << '\n';
    return 1;
}

bool check_packet_header(
    const RawPacket& packet,
    std::uint64_t packet_number,
    std::string& error
) {
    if (packet.header.magic != RAW_PACKET_MAGIC) {
        error = "bad magic at packet " + std::to_string(packet_number);
        return false;
    }

    if (packet.header.version != RAW_PACKET_VERSION) {
        error = "bad version at packet " + std::to_string(packet_number);
        return false;
    }

    if (packet.header.header_size != sizeof(RawPacketHeader)) {
        error = "bad header_size at packet " + std::to_string(packet_number);
        return false;
    }

    if (packet.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "payload too large at packet " + std::to_string(packet_number);
        return false;
    }

    const auto expected_payload_len =
        static_cast<std::uint32_t>(packet.payload.size());

    if (packet.header.payload_len != expected_payload_len) {
        error = "bad payload_len at packet " + std::to_string(packet_number);
        return false;
    }

    if (packet.header.payload_crc32 != crc32(packet.payload)) {
        error = "bad crc32 at packet " + std::to_string(packet_number);
        return false;
    }

    return true;
}

int validate(const std::string& raw_path, const std::string& jsonl_path) {
    RawLogReader reader(raw_path);

    std::ifstream jsonl(jsonl_path, std::ios::binary);
    if (!jsonl.is_open()) {
        return fail("failed to open jsonl file: " + jsonl_path);
    }

    std::uint64_t packet_number = 0;
    std::uint64_t previous_packet_id = 0;
    std::uint64_t first_packet_id = 0;
    std::uint64_t last_packet_id = 0;

    while (true) {
        RawLogReadResult result = reader.next();
        if (result.eof()) {
            break;
        }

        if (!result.ok()) {
            return fail("raw read failed: " + result.message);
        }

        ++packet_number;
        const RawPacket& packet = *result.packet;

        std::string header_error;
        if (!check_packet_header(packet, packet_number, header_error)) {
            return fail(header_error);
        }

        std::string jsonl_payload(packet.payload.size(), '\0');
        if (!jsonl_payload.empty()) {
            jsonl.read(
                jsonl_payload.data(),
                static_cast<std::streamsize>(jsonl_payload.size())
            );

            if (jsonl.gcount() !=
                static_cast<std::streamsize>(jsonl_payload.size())) {
                return fail(
                    "jsonl ended inside payload for packet " +
                    std::to_string(packet_number)
                );
            }
        }

        if (packet.payload != jsonl_payload) {
            return fail(
                "payload mismatch at packet " +
                std::to_string(packet_number)
            );
        }

        char delimiter = '\0';
        if (!jsonl.get(delimiter)) {
            return fail(
                "jsonl missing delimiter after packet " +
                std::to_string(packet_number)
            );
        }

        if (delimiter != '\n') {
            return fail(
                "jsonl delimiter is not newline after packet " +
                std::to_string(packet_number)
            );
        }

        if (packet.header.packet_id <= previous_packet_id) {
            return fail(
                "packet_id not strictly increasing at packet " +
                std::to_string(packet_number)
            );
        }

        if (packet_number == 1) {
            first_packet_id = packet.header.packet_id;
        }

        previous_packet_id = packet.header.packet_id;
        last_packet_id = packet.header.packet_id;
    }

    char trailing = '\0';
    if (jsonl.get(trailing)) {
        return fail("jsonl has bytes after raw EOF");
    }

    std::cout
        << "validated " << packet_number
        << " packets raw=" << raw_path
        << " jsonl=" << jsonl_path
        << " packets_read=" << reader.packets_read()
        << " bytes_read=" << reader.bytes_read()
        << " first_packet_id=" << first_packet_id
        << " last_packet_id=" << last_packet_id
        << '\n';

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string raw_path = argc > 1 ? argv[1] : "logs/market.raw";
    const std::string jsonl_path = argc > 2 ? argv[2] : "logs/market.jsonl";

    if (argc > 3) {
        return fail("usage: validate_raw_log [raw_path] [jsonl_path]");
    }

    try {
        return validate(raw_path, jsonl_path);
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}
