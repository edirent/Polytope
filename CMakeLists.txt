cmake_minimum_required(VERSION 3.20)

project(trading_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

enable_testing()

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

target_link_libraries(trading_engine PRIVATE
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

add_executable(raw_ingest_tests
    tests/unit/raw_ingest_tests.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogWriter.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
)

target_include_directories(raw_ingest_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

foreach(test_case
    WriteThenReadOnePacket
    WriteThenReadMultiplePackets
    WriteThenReadRealPolymarketPackets
    DetectBadMagic
    DetectCrcMismatch
    DetectTruncatedHeader
    DetectTruncatedPayload
    ResetReader
)
    add_test(NAME ${test_case} COMMAND raw_ingest_tests ${test_case})
endforeach()

add_executable(validate_raw_log
    tools/validate_raw_log.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
)

target_include_directories(validate_raw_log PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

add_executable(source_runtime_tests
    tests/unit/source_runtime_tests.cpp
    engine/feed/source_runtime/WebSocketClient.cpp
    engine/feed/source_runtime/HeartbeatController.cpp
    engine/feed/source_runtime/ReconnectController.cpp
)

target_include_directories(source_runtime_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(source_runtime_tests PRIVATE
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

foreach(test_case
    WebSocketEndpointSetting
    WebSocketConnectCallsOnOpen
    WebSocketReceivesRawMessage
    WebSocketSendPreservesPayload
    WebSocketCloseCallback
    WebSocketErrorCallbackOnConnectFailure
    WebSocketErrorCallbackOnReadError
    HeartbeatOpenAllowsImmediatePing
    HeartbeatPingSentWaitingPong
    HeartbeatPongRestoresOk
    HeartbeatPongTimeout
    HeartbeatStaleDetection
    HeartbeatMarketMessageUpdatesLastMessage
    ReconnectRequestReconnect
    ReconnectBackoffNotElapsed
    ReconnectBackoffElapsed
    ReconnectAttemptState
    ReconnectSuccessResetsBackoff
    ReconnectFailureBackoff
    ReconnectMaxAttemptsDisables
    ReconnectDuplicateRequestDoesNotRefreshTimer
    SourceRuntimeOpenSendsSubscription
    SourceRuntimeRawMessageCapture
    SourceRuntimePingPongOk
    SourceRuntimePongTimeoutRequestsReconnect
    SourceRuntimeStaleFeedRequestsReconnect
    SourceRuntimeServerCloseRequestsReconnect
    SourceRuntimeReconnectResubscribes
)
    add_test(NAME ${test_case} COMMAND source_runtime_tests ${test_case})
endforeach()
