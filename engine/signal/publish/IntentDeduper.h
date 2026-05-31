#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace trading_engine::signal {

class IntentDeduper {
public:
    explicit IntentDeduper(std::uint64_t ttl_ns);

    [[nodiscard]] bool seen_recently(
        const std::string& idempotency_key,
        std::uint64_t now_ns
    );

    [[nodiscard]] bool seen_recently(
        std::uint64_t idempotency_hash,
        std::uint64_t now_ns
    );

    void mark_seen(
        const std::string& idempotency_key,
        std::uint64_t now_ns
    );

    void mark_seen(
        std::uint64_t idempotency_hash,
        std::uint64_t now_ns
    );

private:
    std::uint64_t ttl_ns_ = 0;
    std::unordered_map<std::string, std::uint64_t> seen_;
    std::unordered_map<std::uint64_t, std::uint64_t> seen_hashes_;
};

}  // namespace trading_engine::signal
