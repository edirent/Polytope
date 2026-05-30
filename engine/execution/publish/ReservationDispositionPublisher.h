#pragma once

#include "engine/execution/public/ReservationDisposition.h"

namespace trading_engine::execution {

class ReservationDispositionPublisher {
public:
    virtual ~ReservationDispositionPublisher() = default;

    virtual void publish(const ReservationDisposition& disposition) = 0;
};

}  // namespace trading_engine::execution
