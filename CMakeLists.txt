cmake_minimum_required(VERSION 3.20)

project(trading_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

enable_testing()

add_subdirectory(engine/decode)

# Until a smaller engine_decode_public target exists, downstream targets may
# link engine_decode. State code may include only decode/public headers.

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
    engine/feed/state/EntityStateStore.cpp
    engine/feed/state/StateHasher.cpp
    engine/feed/integrity/ConsistencyChecker.cpp
    engine/feed/integrity/StaleDetector.cpp
    engine/feed/integrity/RecoveryController.cpp
    engine/feed/output/EventBus.cpp
    engine/feed/output/HealthPublisher.cpp
    engine/feed/output/ReplayRunner.cpp
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
    engine_decode
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

add_executable(state_hasher_tests
    tests/unit/state_hasher_tests.cpp
    engine/feed/state/StateHasher.cpp
)

target_include_directories(state_hasher_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

foreach(test_case
    SameEntitySameHash
    DifferentBookDifferentHash
    DifferentStatusDifferentHash
    TimestampDoesNotChangeHash
    MapInsertionOrderDoesNotChangeHash
    GlobalHashStableForSameEntities
)
    add_test(NAME ${test_case} COMMAND state_hasher_tests ${test_case})
endforeach()

add_executable(consistency_checker_tests
    tests/unit/consistency_checker_tests.cpp
    engine/feed/integrity/ConsistencyChecker.cpp
    engine/feed/raw_ingest/RawPacket.cpp
)

target_include_directories(consistency_checker_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

foreach(test_case
    GoodSnapshotOk
    DeltaBeforeSnapshot
    PriceGreaterThanOneInvalidValue
    NegativeSizeInvalidValue
    CrossedBook
    ExternalBboDiverged
)
    add_test(NAME ${test_case} COMMAND consistency_checker_tests ${test_case})
endforeach()

add_executable(stale_detector_tests
    tests/unit/stale_detector_tests.cpp
    engine/feed/integrity/StaleDetector.cpp
)

target_include_directories(stale_detector_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

foreach(test_case
    LastMessageWithinTimeoutOk
    SourceNoMessageBeyondThreshold
    EntityNoUpdateBeyondThreshold
    ClosedEntityIgnored
    UninitializedEntityIgnored
)
    add_test(NAME ${test_case} COMMAND stale_detector_tests ${test_case})
endforeach()

add_executable(recovery_controller_tests
    tests/unit/recovery_controller_tests.cpp
    engine/feed/integrity/RecoveryController.cpp
)

target_include_directories(recovery_controller_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

foreach(test_case
    DeltaBeforeSnapshotRequestsSnapshot
    CrossedBookRequestsSnapshot
    SourceStaleReconnectsSource
    DecodeErrorMarksUnsafe
    OkDoesNothing
)
    add_test(NAME ${test_case} COMMAND recovery_controller_tests ${test_case})
endforeach()

add_executable(health_publisher_tests
    tests/unit/health_publisher_tests.cpp
    engine/feed/output/HealthPublisher.cpp
    engine/feed/state/EntityStateStore.cpp
    engine/feed/state/StateHasher.cpp
)

target_include_directories(health_publisher_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(health_publisher_tests PRIVATE
    engine_decode
)

foreach(test_case
    SnapshotSerializesProcessAndReplay
    SnapshotSerializesSourceAndEntity
    PublishWritesJsonLine
)
    add_test(NAME ${test_case} COMMAND health_publisher_tests ${test_case})
endforeach()

add_executable(event_bus_tests
    tests/unit/event_bus_tests.cpp
    engine/feed/output/EventBus.cpp
)

target_include_directories(event_bus_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

foreach(test_case
    PublishEventCallsSubscribersSynchronously
    PublishStateCallsStateSubscribersOnly
    PublishHealthCallsHealthSubscribers
    PublishOrderIsSubscriptionOrder
)
    add_test(NAME ${test_case} COMMAND event_bus_tests ${test_case})
endforeach()

add_executable(decode_json_decoder_tests
    tests/unit/decode/json_decoder_test.cpp
)

target_include_directories(decode_json_decoder_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(decode_json_decoder_tests PRIVATE
    engine_decode
)

foreach(test_case
    JsonDecoder_ObjectPayload
    JsonDecoder_ArrayPayload
    JsonDecoder_PongIsControl
    JsonDecoder_ControlIsNotMalformed
    JsonDecoder_MalformedJson
    JsonDecoder_UnsupportedScalarJson
    JsonDecoder_EmptyPayload
)
    add_test(NAME ${test_case} COMMAND decode_json_decoder_tests ${test_case})
endforeach()

add_executable(decode_normalized_event_contract_tests
    tests/unit/decode/normalized_event_contract_test.cpp
)

target_include_directories(decode_normalized_event_contract_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(decode_normalized_event_contract_tests PRIVATE
    engine_decode
)

foreach(test_case
    NormalizedEvent_DefaultConstructsSafely
    NormalizedEvent_HeartbeatHasStableFields
    NormalizedEvent_SnapshotHasStableEntityId
    NormalizedEvent_DeltaHasStableEntityId
    NormalizedEventBatch_CanHoldMultipleEvents
    NormalizedEventBatch_RejectsOrCapsOverflow
)
    add_test(
        NAME ${test_case}
        COMMAND decode_normalized_event_contract_tests ${test_case}
    )
endforeach()

add_executable(decode_event_normalizer_tests
    tests/unit/decode/event_normalizer_test.cpp
)

target_include_directories(decode_event_normalizer_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(decode_event_normalizer_tests PRIVATE
    engine_decode
)

foreach(test_case
    EventNormalizer_BookToSnapshot
    EventNormalizer_PriceChangeToDelta
    EventNormalizer_ControlPongToHeartbeat
    EventNormalizer_UnknownToUnknown
    EventNormalizer_ArrayWrappedPayloadWorks
    EventNormalizer_OnePacketCanEmitMultipleEvents
    EventNormalizer_MalformedJsonDoesNotCrash
    PriceChange_UsesNestedAssetId
    PriceChange_DoesNotUseTopLevelMarketAsEntityId
    PriceChange_GroupsOnlySameAssetChanges
    PriceChange_MultipleAssetsDoNotPolluteOneDelta
)
    add_test(
        NAME ${test_case}
        COMMAND decode_event_normalizer_tests ${test_case}
    )
endforeach()

add_executable(decode_normalized_event_compatibility_tests
    tests/integration/decode/normalized_event_compatibility_test.cpp
    engine/feed/raw_ingest/RawPacket.cpp
)

target_include_directories(decode_normalized_event_compatibility_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(decode_normalized_event_compatibility_tests PRIVATE
    engine_decode
)

foreach(test_case
    FeedEventNormalizer_EmitsDecodePublicNormalizedEvent
    FeedNormalizedEventAlias_IsDecodePublicType
)
    add_test(
        NAME ${test_case}
        COMMAND decode_normalized_event_compatibility_tests ${test_case}
    )
endforeach()

add_executable(decode_pipeline_tests
    tests/integration/decode/decode_pipeline_test.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
)

target_include_directories(decode_pipeline_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(decode_pipeline_tests PRIVATE
    engine_decode
)

foreach(test_case
    DecodePipeline_BookProducesSnapshot
    DecodePipeline_PriceChangeProducesDelta
    DecodePipeline_ControlProducesHeartbeat
    DecodePipeline_ArrayWrappedPayloadWorks
    DecodePipeline_MalformedJsonProducesErrorOrNoCrash
    DecodePipeline_UnsupportedCodecRejected
    DecodePipeline_Market39ProducesExpectedCounts
)
    add_test(
        NAME ${test_case}
        COMMAND decode_pipeline_tests ${test_case}
    )
endforeach()

add_executable(feed_e2e_decode_pipeline_tests
    tests/integration/feed/feed_e2e_uses_decode_pipeline_test.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
    engine/feed/state/EntityStateStore.cpp
    engine/feed/state/StateHasher.cpp
)

target_include_directories(feed_e2e_decode_pipeline_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(feed_e2e_decode_pipeline_tests PRIVATE
    engine_decode
)

add_test(
    NAME FeedE2E_UsesDecodePipelineMarket39StateStable
    COMMAND feed_e2e_decode_pipeline_tests
        FeedE2E_UsesDecodePipelineMarket39StateStable
)

add_executable(feed_decode_pipeline_tests
    tests/integration/feed_decode/feed_decode_pipeline_test.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
)

target_include_directories(feed_decode_pipeline_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/feed_decode
)

target_link_libraries(feed_decode_pipeline_tests PRIVATE
    engine_decode
)

add_test(
    NAME FeedDecodePipeline_Market39CountsStable
    COMMAND feed_decode_pipeline_tests
)

add_executable(feed_decode_determinism_tests
    tests/integration/feed_decode/feed_decode_determinism_test.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
)

target_include_directories(feed_decode_determinism_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/feed_decode
)

target_link_libraries(feed_decode_determinism_tests PRIVATE
    engine_decode
)

add_test(
    NAME FeedDecodeDeterminism_Market39TraceHashStable
    COMMAND feed_decode_determinism_tests
)

add_executable(feed_decode_adapter_tests
    tests/integration/feed_decode/feed_decode_adapter_test.cpp
    engine/feed/raw_ingest/RawPacket.cpp
)

target_include_directories(feed_decode_adapter_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/feed_decode
)

target_link_libraries(feed_decode_adapter_tests PRIVATE
    engine_decode
)

add_test(
    NAME FeedDecodeAdapter_PreservesRawPacketMetadata
    COMMAND feed_decode_adapter_tests
)

add_executable(feed_decode_error_tests
    tests/integration/feed_decode/feed_decode_error_test.cpp
    engine/feed/raw_ingest/RawPacket.cpp
)

target_include_directories(feed_decode_error_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/feed_decode
)

target_link_libraries(feed_decode_error_tests PRIVATE
    engine_decode
)

add_test(
    NAME FeedDecodeError_HandlesDecodeFailures
    COMMAND feed_decode_error_tests
)

add_executable(validate_raw_log
    tools/validate_raw_log.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
)

target_include_directories(validate_raw_log PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

add_test(
    NAME ValidatePolymarketFixture
    COMMAND validate_raw_log
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/polymarket/market_39.raw
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/polymarket/market_39.jsonl
)

add_executable(inspect_polymarket_payloads
    tools/inspect_polymarket_payloads.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
)

target_include_directories(inspect_polymarket_payloads PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

add_test(
    NAME InspectPolymarketFixture
    COMMAND inspect_polymarket_payloads
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/polymarket/market_39.raw
)

add_executable(verify_polymarket_pipeline
    tools/verify_polymarket_pipeline.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
)

target_include_directories(verify_polymarket_pipeline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(verify_polymarket_pipeline PRIVATE
    engine_decode
)

add_test(
    NAME VerifyPolymarketPipelineFixture
    COMMAND verify_polymarket_pipeline
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/polymarket/market_39.raw
        --expect-market-39
)

add_executable(replay_raw_log
    tools/replay_raw_log.cpp
    engine/feed/output/ReplayRunner.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
    engine/feed/state/EntityStateStore.cpp
    engine/feed/state/StateHasher.cpp
)

target_include_directories(replay_raw_log PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(replay_raw_log PRIVATE
    engine_decode
)

add_test(
    NAME ReplayPolymarketTraceA
    COMMAND replay_raw_log
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/polymarket/market_39.raw
        ${CMAKE_CURRENT_BINARY_DIR}/market_39_trace_a.csv
)

add_test(
    NAME ReplayPolymarketTraceB
    COMMAND replay_raw_log
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/polymarket/market_39.raw
        ${CMAKE_CURRENT_BINARY_DIR}/market_39_trace_b.csv
)

add_test(
    NAME ReplayPolymarketTraceDeterministic
    COMMAND ${CMAKE_COMMAND} -E compare_files
        ${CMAKE_CURRENT_BINARY_DIR}/market_39_trace_a.csv
        ${CMAKE_CURRENT_BINARY_DIR}/market_39_trace_b.csv
)

set_tests_properties(
    ReplayPolymarketTraceB
    PROPERTIES DEPENDS ReplayPolymarketTraceA
)

set_tests_properties(
    ReplayPolymarketTraceDeterministic
    PROPERTIES DEPENDS "ReplayPolymarketTraceA;ReplayPolymarketTraceB"
)

add_executable(inspect_normalized_events
    tools/inspect_normalized_events.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
)

target_include_directories(inspect_normalized_events PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(inspect_normalized_events PRIVATE
    engine_decode
)

add_test(
    NAME InspectNormalizedEventsFixture
    COMMAND inspect_normalized_events
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/polymarket/market_39.raw
)

add_executable(run_feed_e2e
    tools/run_feed_e2e.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
    engine/feed/state/EntityStateStore.cpp
    engine/feed/state/StateHasher.cpp
    engine/feed/integrity/ConsistencyChecker.cpp
)

target_include_directories(run_feed_e2e PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(run_feed_e2e PRIVATE
    engine_decode
)

add_test(
    NAME FeedE2EFixture
    COMMAND run_feed_e2e
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/polymarket/market_39.raw
        --repeat 1000
        --check-determinism
)

add_executable(bench_feed_decode_latency
    tools/bench_feed_decode_latency.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
)

target_include_directories(bench_feed_decode_latency PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(bench_feed_decode_latency PRIVATE
    engine_decode
)

add_executable(run_live_feed_smoke
    tools/run_live_feed_smoke.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogWriter.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
    engine/feed/source_runtime/WebSocketClient.cpp
)

target_include_directories(run_live_feed_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(run_live_feed_smoke PRIVATE
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

add_test(
    NAME LiveFeedSmokeManual
    COMMAND run_live_feed_smoke
        --seconds 300
        --asset-id YOUR_POLYMARKET_ASSET_ID
)

set_tests_properties(
    LiveFeedSmokeManual
    PROPERTIES
        LABELS manual
        DISABLED TRUE
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
