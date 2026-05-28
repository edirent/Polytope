#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trading_engine::chain_confirm::eth_log_decoder {

[[nodiscard]] std::string normalize_hex(std::string value);
[[nodiscard]] bool is_hex_32_word(const std::string& value);
[[nodiscard]] std::string topic_to_address(const std::string& topic);
[[nodiscard]] std::vector<std::string> split_data_words(
    const std::string& data
);
[[nodiscard]] std::string uint256_to_decimal_string(
    const std::string& word
);
[[nodiscard]] std::optional<std::uint64_t> uint256_to_u64(
    const std::string& word
);

}  // namespace trading_engine::chain_confirm::eth_log_decoder
