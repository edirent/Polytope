#pragma once

#include "engine/signal/publish/IIntentPublisher.h"
#include "engine/signal/publish/JsonlIntentWriter.h"

namespace trading_engine::signal {

class PaperIntentPublisher final : public IIntentPublisher {
public:
    explicit PaperIntentPublisher(JsonlIntentWriter* writer);

    void publish(const OpportunityIntent& intent) override;

private:
    JsonlIntentWriter* writer_ = nullptr;
};

}  // namespace trading_engine::signal
