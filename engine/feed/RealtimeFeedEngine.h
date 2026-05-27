#pragma once

#include "feed/decode/EventNormalizer.h"
#include "feed/decode/JsonDecoder.h"
#include "feed/integrity/ConsistencyChecker.h"
#include "feed/integrity/RecoveryController.h"
#include "feed/integrity/StaleDetector.h"
#include "feed/output/EventBus.h"
#include "feed/output/HealthPublisher.h"
#include "feed/output/ReplayRunner.h"
#include "feed/raw_ingest/RawLogReader.h"
#include "feed/raw_ingest/RawLogWriter.h"
#include "feed/source_runtime/HeartbeatController.h"
#include "feed/source_runtime/ReconnectController.h"
#include "feed/source_runtime/WebSocketClient.h"
#include "feed/state/EntityStateStore.h"
#include "feed/state/StateHasher.h"

namespace trading_engine::feed {

class RealtimeFeedEngine {
public:
    RealtimeFeedEngine();

    void start();
    void stop();

    [[nodiscard]] bool running() const noexcept;

private:
    WebSocketClient websocket_client_;
    HeartbeatController heartbeat_controller_;
    ReconnectController reconnect_controller_;

    RawLogWriter raw_log_writer_;
    RawLogReader raw_log_reader_;

    JsonDecoder json_decoder_;
    EventNormalizer event_normalizer_;

    EntityStateStore entity_state_store_;
    StateHasher state_hasher_;

    ConsistencyChecker consistency_checker_;
    StaleDetector stale_detector_;
    RecoveryController recovery_controller_;

    EventBus event_bus_;
    HealthPublisher health_publisher_;
    ReplayRunner replay_runner_;

    bool running_{false};
};

}  // namespace trading_engine::feed
