#include "oracle/artifact/ArtifactChecksum.h"

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace trading_engine::oracle {

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

}  // namespace

std::uint64_t fnv1a64(std::span<const std::byte> data) noexcept {
    auto hash = kFnvOffsetBasis;
    for (const auto byte : data) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= kFnvPrime;
    }
    return hash;
}

std::uint64_t fnv1a64(std::string_view data) noexcept {
    auto hash = kFnvOffsetBasis;
    for (const char value : data) {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= kFnvPrime;
    }
    return hash;
}

std::string checksum_hex(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::nouppercase << std::setfill('0') << std::setw(16)
        << value;
    return out.str();
}

ArtifactChecksumResult checksum_file(const std::filesystem::path& path) {
    ArtifactChecksumResult result;

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.error = "failed to open checksum file: " + path.string();
        return result;
    }

    std::vector<std::byte> bytes;
    std::array<char, 4096> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            bytes.push_back(static_cast<std::byte>(buffer[static_cast<std::size_t>(i)]));
        }
    }

    if (!input.eof()) {
        result.error = "failed to read checksum file: " + path.string();
        return result;
    }

    result.value = fnv1a64(bytes);
    result.hex = checksum_hex(result.value);
    return result;
}

}  // namespace trading_engine::oracle
