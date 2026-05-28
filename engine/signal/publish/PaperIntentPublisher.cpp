#include "engine/signal/publish/PaperIntentPublisher.h"

namespace trading_engine::signal {

PaperIntentPublisher::PaperIntentPublisher(JsonlIntentWriter* writer)
    : writer_(writer) {}

void PaperIntentPublisher::publish(const OpportunityIntent& intent) {
    if (writer_) {
        (void)writer_->write(intent);
    }
}

}  // namespace trading_engine::signal
