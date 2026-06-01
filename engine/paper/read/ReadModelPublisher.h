#pragma once

#include "engine/paper/read/DashboardReadStore.h"

#include <cstdint>
#include <utility>

namespace trading_engine::paper {

class ReadModelPublisher {
public:
    explicit ReadModelPublisher(DashboardReadStore* store) : store_(store) {}

    [[nodiscard]] std::uint64_t publish(DashboardSnapshot snapshot) const {
        if (store_ == nullptr) {
            return 0;
        }
        return store_->publish(std::move(snapshot));
    }

private:
    DashboardReadStore* store_ = nullptr;
};

}  // namespace trading_engine::paper
