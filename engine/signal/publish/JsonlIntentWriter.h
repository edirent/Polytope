#pragma once

#include "engine/signal/public/OpportunityIntent.h"

#include <iosfwd>

namespace trading_engine::signal {

class JsonlIntentWriter {
public:
    explicit JsonlIntentWriter(std::ostream* output);

    [[nodiscard]] bool write(const OpportunityIntent& intent);

private:
    std::ostream* output_ = nullptr;
};

}  // namespace trading_engine::signal
