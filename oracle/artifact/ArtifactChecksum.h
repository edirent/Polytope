#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace trading_engine::oracle {

struct ArtifactChecksumResult {
    std::uint64_t value = 0;
    std::string hex;
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty();
    }
};

[[nodiscard]] std::uint64_t fnv1a64(
    std::span<const std::byte> data
) noexcept;

[[nodiscard]] std::uint64_t fnv1a64(std::string_view data) noexcept;

[[nodiscard]] std::string checksum_hex(std::uint64_t value);

[[nodiscard]] ArtifactChecksumResult checksum_file(
    const std::filesystem::path& path
);

}  // namespace trading_engine::oracle
