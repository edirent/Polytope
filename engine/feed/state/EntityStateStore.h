#pragma once

#include "feed/decode/EventNormalizer.h"

#include <cstddef>
#include <string>
#include <unordered_map>

namespace trading_engine::feed {

class EntityStateStore {
public:
    void apply(const NormalizedEvent& event);
    void clear() noexcept;

    [[nodiscard]] const std::string* find(const std::string& entity_id) const;
    [[nodiscard]] const std::unordered_map<std::string, std::string>& states() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::unordered_map<std::string, std::string> states_;
};

}  // namespace trading_engine::feed
