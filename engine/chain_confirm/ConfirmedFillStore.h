#pragma once

#include "chain_confirm/ConfirmedFill.h"

#include <cstdint>
#include <map>
#include <string>

namespace trading_engine::chain_confirm {

enum class ConfirmedFillStoreCode : std::uint8_t {
    Inserted = 0,
    Duplicate,
    Updated,
    MarkedRemoved,
    RemovedUnknown
};

struct ConfirmedFillStoreResult {
    ConfirmedFillStoreCode code{ConfirmedFillStoreCode::Inserted};
    std::string fill_id;
};

class ConfirmedFillStore {
public:
    [[nodiscard]] ConfirmedFillStoreResult upsert(
        const ConfirmedFill& fill
    );

    [[nodiscard]] bool contains(const std::string& fill_id) const noexcept;
    [[nodiscard]] const ConfirmedFill* get(
        const std::string& fill_id
    ) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t active_count() const noexcept;
    void clear();

private:
    std::map<std::string, ConfirmedFill> fills_;
};

[[nodiscard]] std::string to_string(ConfirmedFillStoreCode code);

}  // namespace trading_engine::chain_confirm
