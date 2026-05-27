cmake_minimum_required(VERSION 3.20)

project(trading_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(trading_engine
    engine/main.cpp
    engine/runtime/Runtime.cpp
    engine/runtime/ThreadManager.cpp
    engine/runtime/CpuAffinity.cpp
    engine/runtime/Shutdown.cpp
    engine/feed/RealtimeFeedEngine.cpp
    engine/feed/source_runtime/WebSocketClient.cpp
    engine/feed/source_runtime/HeartbeatController.cpp
    engine/feed/source_runtime/ReconnectController.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogWriter.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
    engine/feed/decode/JsonDecoder.cpp
    engine/feed/decode/EventNormalizer.cpp
    engine/feed/state/EntityStateStore.cpp
    engine/feed/state/StateHasher.cpp
    engine/feed/integrity/ConsistencyChecker.cpp
    engine/feed/integrity/StaleDetector.cpp
    engine/feed/integrity/RecoveryController.cpp
    engine/feed/output/EventBus.cpp
    engine/feed/output/HealthPublisher.cpp
    engine/feed/output/ReplayRunner.cpp
    engine/decode/Decompressor.cpp
    engine/decode/JsonParser.cpp
    engine/decode/BinaryParser.cpp
    engine/decode/Normalizer.cpp
    engine/state/LOBEngine.cpp
    engine/state/SnapshotPublisher.cpp
    engine/state/SettlementState.cpp
    engine/state/VersionedState.cpp
    engine/signal/SignalEngine.cpp
    engine/signal/BundleScanner.cpp
    engine/signal/EdgeCalculator.cpp
    engine/signal/OpportunityRanker.cpp
    engine/risk/RiskEngine.cpp
    engine/risk/VWAPSimulator.cpp
    engine/risk/ExposureManager.cpp
    engine/risk/FeeModel.cpp
    engine/risk/KillSwitch.cpp
    engine/execution/PaperExecution.cpp
    engine/execution/SandboxExecution.cpp
    engine/execution/LiveExecution.cpp
    engine/execution/OrderStateMachine.cpp
    engine/execution/FillTracker.cpp
    engine/memory/SlabAllocator.cpp
    engine/metrics/Metrics.cpp
    engine/metrics/LatencyHistogram.cpp
    engine/metrics/EventCounter.cpp
)

target_include_directories(trading_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)
