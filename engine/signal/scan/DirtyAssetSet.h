#pragma once

#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace trading_engine::signal {

class DirtyAssetSet {
public:
    void mark_dirty(const std::string& asset_id) {
        if (asset_id.empty()) {
            return;
        }
        const auto [_, inserted] = seen_.insert(asset_id);
        if (inserted) {
            assets_.push_back(asset_id);
        }
    }

    void clear() {
        assets_.clear();
        seen_.clear();
    }

    [[nodiscard]] bool empty() const noexcept {
        return assets_.empty();
    }

    [[nodiscard]] std::span<const std::string> assets() const noexcept {
        return assets_;
    }

private:
    std::vector<std::string> assets_;
    std::unordered_set<std::string> seen_;
};

}  // namespace trading_engine::signal
