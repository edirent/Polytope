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
add_subdirectory(engine/chain_confirm)
add_subdirectory(engine/state)
add_subdirectory(oracle)
add_subdirectory(engine/signal)

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
    engine_state
    engine_decode
    engine_signal
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
    tests/unit/state/state_hasher_test.cpp
)

target_include_directories(state_hasher_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(state_hasher_tests PRIVATE
    engine_state
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

add_executable(market_state_view_tests
    tests/unit/state/market_state_view_test.cpp
)

target_include_directories(market_state_view_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(market_state_view_tests PRIVATE
    engine_state
)

foreach(test_case
    MarketStateView_EmptyStoreReturnsMissing
    MarketStateView_SnapshotReturnsBestBidAsk
    MarketStateView_DeltaUpdatesBestBidAsk
    MarketStateView_MidAndSpreadComputedCorrectly
    MarketStateView_MissingBidOrAskReturnsError
    MarketStateView_RecoveringEntityReturnsNoExecutablePrice
    MarketStateView_ClosedEntityReturnsNoExecutablePrice
    MarketStateView_ResolvedEntityReturnsNoExecutablePrice
    MarketStateView_CrossedBookReturnsError
    MarketStateView_SnapshotCopyHasStableVersion
    MarketStateView_StateHashStable
    MarketStateView_ReadsPublishedSnapshot
    MarketStateView_DoesNotReadMutableEntityStore
    MarketStateView_ReturnsBookAndChainFields
)
    add_test(NAME ${test_case} COMMAND market_state_view_tests ${test_case})
endforeach()

add_executable(signal_types_tests
    tests/unit/signal/signal_types_test.cpp
)

target_link_libraries(signal_types_tests PRIVATE
    engine_signal
)

foreach(test_case
    SignalConfig_DefaultsAreSafe
    OpportunityIntent_DefaultIsCandidateOnly
    SignalRunResult_DefaultCountersZero
)
    add_test(NAME ${test_case} COMMAND signal_types_tests ${test_case})
endforeach()

add_executable(signal_oracle_artifact_reader_tests
    tests/unit/signal/oracle_artifact_reader_test.cpp
)

target_link_libraries(signal_oracle_artifact_reader_tests PRIVATE
    engine_signal
)

target_compile_definitions(signal_oracle_artifact_reader_tests PRIVATE
    POLYTOPE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

foreach(test_case
    OracleArtifactReader_LoadsFixtureBundles
    OracleArtifactReader_RejectsMissingManifest
    OracleArtifactReader_RejectsChecksumMismatch
    OracleArtifactReader_RejectsUnsupportedVersion
    OracleArtifactReader_RejectsDuplicateBundleId
)
    add_test(NAME ${test_case} COMMAND signal_oracle_artifact_reader_tests ${test_case})
endforeach()

add_executable(signal_market_snapshot_reader_tests
    tests/unit/signal/market_snapshot_reader_test.cpp
)

target_link_libraries(signal_market_snapshot_reader_tests PRIVATE
    engine_signal
)

target_compile_definitions(signal_market_snapshot_reader_tests PRIVATE
    POLYTOPE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

foreach(test_case
    MarketSnapshotReader_ReadsRequiredSnapshots
    MarketSnapshotReader_RejectsMissingSnapshot
    MarketSnapshotReader_RejectsRecoveringSnapshot
    MarketSnapshotReader_RejectsCrossedSnapshot
    MarketSnapshotReader_RejectsNotUsableForDepth
    MarketSnapshotReader_RequiresUsableForSignalWhenConfigured
    MarketSnapshotReader_MarketStateViewAdapterReadsSnapshot
)
    add_test(NAME ${test_case} COMMAND signal_market_snapshot_reader_tests ${test_case})
endforeach()

add_executable(signal_vwap_precheck_tests
    tests/unit/signal/vwap_precheck_test.cpp
)

target_link_libraries(signal_vwap_precheck_tests PRIVATE
    engine_signal
)

foreach(test_case
    VWAPPrecheck_BuyConsumesAsks
    VWAPPrecheck_MultiLevelBuyComputesVWAP
    VWAPPrecheck_RejectsInsufficientDepth
    VWAPPrecheck_RejectsMissingAskSide
    VWAPPrecheck_RejectsInvalidQuantity
    VWAPPrecheck_RejectsUnsupportedSellLegInV0
)
    add_test(NAME ${test_case} COMMAND signal_vwap_precheck_tests ${test_case})
endforeach()

add_executable(signal_fee_model_tests
    tests/unit/signal/fee_model_test.cpp
)

target_link_libraries(signal_fee_model_tests PRIVATE
    engine_signal
)

add_test(NAME FeeModel_ReturnsDefaultFee
    COMMAND signal_fee_model_tests FeeModel_ReturnsDefaultFee
)

add_executable(signal_latency_buffer_model_tests
    tests/unit/signal/latency_buffer_model_test.cpp
)

target_link_libraries(signal_latency_buffer_model_tests PRIVATE
    engine_signal
)

add_test(NAME LatencyBufferModel_ReturnsDefaultBuffer
    COMMAND signal_latency_buffer_model_tests LatencyBufferModel_ReturnsDefaultBuffer
)

add_executable(signal_edge_calculator_tests
    tests/unit/signal/edge_calculator_test.cpp
)

target_link_libraries(signal_edge_calculator_tests PRIVATE
    engine_signal
)

foreach(test_case
    EdgeCalculator_ComputesPositiveEdge
    EdgeCalculator_SubtractsFee
    EdgeCalculator_SubtractsLatencyBuffer
    EdgeCalculator_RejectsBelowMinEdge
    EdgeCalculator_HandlesZeroCost
)
    add_test(NAME ${test_case} COMMAND signal_edge_calculator_tests ${test_case})
endforeach()

add_executable(signal_opportunity_ranker_tests
    tests/unit/signal/opportunity_ranker_test.cpp
)

target_link_libraries(signal_opportunity_ranker_tests PRIVATE
    engine_signal
)

foreach(test_case
    OpportunityRanker_SortsPaperOpportunitiesFirst
    OpportunityRanker_SortsByEdgeDescending
    OpportunityRanker_StableTieBreakByBundleId
    OpportunityRanker_DeterministicOrdering
)
    add_test(NAME ${test_case} COMMAND signal_opportunity_ranker_tests ${test_case})
endforeach()

add_executable(signal_paper_intent_publisher_tests
    tests/unit/signal/paper_intent_publisher_test.cpp
)

target_link_libraries(signal_paper_intent_publisher_tests PRIVATE
    engine_signal
)

foreach(test_case
    CapturingIntentPublisher_CapturesIntent
    CapturingIntentPublisher_PreservesOrder
    JsonlIntentWriter_WritesValidJsonLine
    JsonlIntentWriter_DoesNotRequireExecution
)
    add_test(NAME ${test_case} COMMAND signal_paper_intent_publisher_tests ${test_case})
endforeach()

add_executable(signal_workflow_small_fixture_tests
    tests/integration/signal/signal_workflow_small_fixture_test.cpp
)

target_link_libraries(signal_workflow_small_fixture_tests PRIVATE
    engine_signal
)

foreach(test_case
    SignalEngine_ScansBundles
    SignalEngine_PublishesPaperOpportunityWhenEdgePositive
)
    add_test(NAME ${test_case} COMMAND signal_workflow_small_fixture_tests ${test_case})
endforeach()

add_executable(signal_rejects_bad_state_tests
    tests/integration/signal/signal_rejects_bad_state_test.cpp
)

target_link_libraries(signal_rejects_bad_state_tests PRIVATE
    engine_signal
)

foreach(test_case
    SignalEngine_PublishesRejections
    SignalEngine_RejectsBadMarketState
)
    add_test(NAME ${test_case} COMMAND signal_rejects_bad_state_tests ${test_case})
endforeach()

add_executable(signal_rejects_insufficient_depth_tests
    tests/integration/signal/signal_rejects_insufficient_depth_test.cpp
)

target_link_libraries(signal_rejects_insufficient_depth_tests PRIVATE
    engine_signal
)

foreach(test_case
    SignalEngine_RejectsInsufficientDepth
    SignalEngine_RejectsLowEdge
)
    add_test(NAME ${test_case} COMMAND signal_rejects_insufficient_depth_tests ${test_case})
endforeach()

add_executable(signal_determinism_tests
    tests/integration/signal/signal_determinism_test.cpp
)

target_link_libraries(signal_determinism_tests PRIVATE
    engine_signal
)

add_test(NAME SignalEngine_OutputHashDeterministic
    COMMAND signal_determinism_tests SignalEngine_OutputHashDeterministic
)

add_executable(verify_signal_workflow_tests
    tests/integration/signal/verify_signal_workflow_test.cpp
)

target_compile_definitions(verify_signal_workflow_tests PRIVATE
    POLYTOPE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    VERIFY_SIGNAL_WORKFLOW_BIN="$<TARGET_FILE:verify_signal_workflow>"
)

add_dependencies(verify_signal_workflow_tests verify_signal_workflow)

foreach(test_case
    VerifySignalWorkflow_SmallFixtureRuns
    VerifySignalWorkflow_Deterministic
    VerifySignalWorkflow_EmitsSummary
    VerifySignalWorkflow_DoesNotRequireNetwork
    VerifySignalWorkflow_PositiveFixtureEmitsPaperOpportunity
)
    add_test(
        NAME ${test_case}
        COMMAND verify_signal_workflow_tests ${test_case}
    )
endforeach()

add_executable(market_state_event_tests
    tests/unit/state/market_state_event_test.cpp
)

target_include_directories(market_state_event_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(market_state_event_tests PRIVATE
    engine_state
)

foreach(test_case
    MarketStateEvent_NormalizedSnapshotToWsBookSnapshot
    MarketStateEvent_NormalizedDeltaToWsBookDelta
    MarketStateEvent_HeartbeatToWsHeartbeat
    MarketStateEvent_LifecycleToWsLifecycle
    MarketStateEvent_ClassifiedConfirmedFillToChainConfirmedFill
    MarketStateEvent_RemovedFillToChainRemovedFill
    MarketStateEvent_MetadataPreserved
)
    add_test(NAME ${test_case} COMMAND market_state_event_tests ${test_case})
endforeach()

add_executable(lob_writer_tests
    tests/unit/state/lob_writer_test.cpp
)

target_include_directories(lob_writer_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(lob_writer_tests PRIVATE
    engine_state
)

foreach(test_case
    LOBWriter_AppliesSnapshot
    LOBWriter_AppliesDeltaAfterSnapshot
    LOBWriter_RejectsDeltaBeforeSnapshot
    LOBWriter_IgnoresHeartbeat
    LOBWriter_IgnoresChainConfirmedFill
    LOBWriter_BatchCountsAreCorrect
)
    add_test(NAME ${test_case} COMMAND lob_writer_tests ${test_case})
endforeach()

add_executable(confirmed_trade_state_tests
    tests/unit/state/confirmed_trade_state_test.cpp
)

target_include_directories(confirmed_trade_state_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(confirmed_trade_state_tests PRIVATE
    engine_state
)

foreach(test_case
    ChainConfirmedFill_BuyAggressorUpdatesBuyWindow
    ChainConfirmedFill_SellAggressorUpdatesSellWindow
    UnknownDirection_DoesNotBecomeBuyOrSell
    AmbiguousDirection_DoesNotBecomeBuyOrSell
    RemovedFill_MarksOrRevertsTrade
    ChainConfirmedFill_DoesNotMutateOrderBook
)
    add_test(NAME ${test_case} COMMAND confirmed_trade_state_tests ${test_case})
endforeach()

add_executable(settlement_state_tests
    tests/unit/state/settlement_state_test.cpp
)

target_include_directories(settlement_state_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(settlement_state_tests PRIVATE
    engine_state
)

foreach(test_case
    Settlement_StartsOpen
    Settlement_MarksClosed
    Settlement_MarksResolved
    ResolvedState_StoresWinningAsset
    GlobalBitmask_DefaultsZero
    Settlement_DoesNotMutateOrderBook
)
    add_test(NAME ${test_case} COMMAND settlement_state_tests ${test_case})
endforeach()

add_executable(book_quality_state_tests
    tests/unit/state/book_quality_state_test.cpp
)

target_include_directories(book_quality_state_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(book_quality_state_tests PRIVATE
    engine_state
)

foreach(test_case
    Quality_GoodWhenBookAndChainHealthy
    Quality_RecoveringWhenDeltaBeforeSnapshot
    Quality_CrossedWhenBestBidGreaterThanAsk
    Quality_StaleWhenWsAgeTooHigh
    Quality_ChainLaggingWhenChainBehind
    Quality_ChainMismatchWhenReconciliationFails
    ClosedOrResolved_DisablesSignal
)
    add_test(NAME ${test_case} COMMAND book_quality_state_tests ${test_case})
endforeach()

add_executable(snapshot_publisher_tests
    tests/unit/state/snapshot_publisher_test.cpp
)

target_include_directories(snapshot_publisher_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(snapshot_publisher_tests PRIVATE
    engine_state
)

foreach(test_case
    SnapshotPublisher_ReadMissingReturnsMissingEntity
    SnapshotPublisher_PublishThenReadReturnsSnapshot
    SnapshotPublisher_SecondPublishIncrementsVersion
    SnapshotPublisher_DoesNotExposeMutableReference
    SnapshotPublisher_DoubleBufferKeepsPreviousSnapshotValidDuringPublish
)
    add_test(NAME ${test_case} COMMAND snapshot_publisher_tests ${test_case})
endforeach()

add_executable(lob_shard_tests
    tests/unit/state/lob_shard_test.cpp
)

target_include_directories(lob_shard_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(lob_shard_tests PRIVATE
    engine_state
)

foreach(test_case
    LOBShard_AppliesWsSnapshotAndPublishesSnapshot
    LOBShard_AppliesWsDeltaAndUpdatesSnapshot
    LOBShard_AppliesChainFillWithoutChangingBook
    LOBShard_RemovedFillUpdatesChainState
    LOBShard_PublishesQualityState
    ShardRouter_ReturnsZeroForNow
)
    add_test(NAME ${test_case} COMMAND lob_shard_tests ${test_case})
endforeach()

add_executable(oracle_manifest_tests
    tests/unit/oracle/oracle_manifest_test.cpp
)

target_link_libraries(oracle_manifest_tests PRIVATE
    oracle_core
)

foreach(test_case
    ArtifactManifest_DefaultsAreSafe
    OracleErrorCode_DefaultNone
)
    add_test(NAME ${test_case} COMMAND oracle_manifest_tests ${test_case})
endforeach()

add_executable(oracle_cmake_smoke_tests
    tests/unit/oracle/oracle_cmake_smoke_test.cpp
)

target_link_libraries(oracle_cmake_smoke_tests PRIVATE
    oracle_tools
)

add_test(
    NAME OracleBuild_NoLlmRequired
    COMMAND oracle_cmake_smoke_tests OracleBuild_NoLlmRequired
)

add_executable(oracle_market_description_loader_tests
    tests/unit/oracle/market_description_loader_test.cpp
)

target_link_libraries(oracle_market_description_loader_tests PRIVATE
    oracle_ingestion
)

target_compile_definitions(oracle_market_description_loader_tests PRIVATE
    POLYTOPE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

foreach(test_case
    RawMarketRecord_LoadsFixture
    RawMarketRecord_RejectsMissingMarketId
    RawMarketRecord_RejectsMismatchedOutcomeAssetCount
)
    add_test(
        NAME ${test_case}
        COMMAND oracle_market_description_loader_tests ${test_case}
    )
endforeach()

add_executable(oracle_market_universe_builder_tests
    tests/unit/oracle/market_universe_builder_test.cpp
)

target_link_libraries(oracle_market_universe_builder_tests PRIVATE
    oracle_ingestion
)

foreach(test_case
    MarketUniverseBuilder_DeduplicatesMarkets
    MarketUniverseBuilder_PreservesAssetIdsAsString
)
    add_test(
        NAME ${test_case}
        COMMAND oracle_market_universe_builder_tests ${test_case}
    )
endforeach()

add_test(
    NAME FetchMarketUniverseFixture
    COMMAND fetch_market_universe
        --fixture ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/oracle/raw_markets_small.jsonl
        --out ${CMAKE_CURRENT_BINARY_DIR}/raw_market_snapshot.jsonl
)

add_executable(oracle_rulebook_tests
    tests/unit/oracle/rulebook_test.cpp
)

target_link_libraries(oracle_rulebook_tests PRIVATE
    oracle_rules
)

target_compile_definitions(oracle_rulebook_tests PRIVATE
    POLYTOPE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

foreach(test_case
    RuleDraft_DefaultRequiresManualReview
    Rulebook_LoadsApprovedRules
    Rulebook_RejectsUnapprovedRulesForCompiler
)
    add_test(NAME ${test_case} COMMAND oracle_rulebook_tests ${test_case})
endforeach()

add_executable(oracle_rule_validator_tests
    tests/unit/oracle/rule_validator_test.cpp
)

target_link_libraries(oracle_rule_validator_tests PRIVATE
    oracle_rules
)

foreach(test_case
    RuleValidator_RejectsUnknownVariable
    RuleValidator_RejectsDuplicateRuleId
    RuleValidator_RejectsEmptyVariableSet
    RuleValidator_ApprovedFixtureIsCompilerReady
)
    add_test(
        NAME ${test_case}
        COMMAND oracle_rule_validator_tests ${test_case}
    )
endforeach()

add_executable(oracle_manual_rule_editor_tests
    tests/unit/oracle/manual_rule_editor_test.cpp
)

target_link_libraries(oracle_manual_rule_editor_tests PRIVATE
    oracle_rules
)

add_test(
    NAME ManualRuleEditor_RoundTripsRulebook
    COMMAND oracle_manual_rule_editor_tests ManualRuleEditor_RoundTripsRulebook
)

add_executable(oracle_llm_rule_extractor_tests
    tests/unit/oracle/llm_rule_extractor_test.cpp
)

target_link_libraries(oracle_llm_rule_extractor_tests PRIVATE
    oracle_llm
)

foreach(test_case
    StubRuleExtractor_NoNetwork
    StubRuleExtractor_ReturnsDisabled
    StubRuleExtractor_DoesNotProduceValidatedRules
    ClaudeRuleExtractor_DisabledByDefault
    ClaudeRuleExtractor_MissingApiKeyWhenEnabled
)
    add_test(
        NAME ${test_case}
        COMMAND oracle_llm_rule_extractor_tests ${test_case}
    )
endforeach()

add_test(
    NAME ClaudeRuleExtractorManual
    COMMAND oracle_llm_rule_extractor_tests ClaudeRuleExtractorManual
)
set_tests_properties(ClaudeRuleExtractorManual PROPERTIES
    DISABLED TRUE
    LABELS manual
)

add_executable(oracle_boolean_constraint_builder_tests
    tests/unit/oracle/boolean_constraint_builder_test.cpp
)

target_link_libraries(oracle_boolean_constraint_builder_tests PRIVATE
    oracle_compiler
)

foreach(test_case
    BooleanConstraintBuilder_ExactlyOne
    BooleanConstraintBuilder_AtMostOne
    BooleanConstraintBuilder_AtLeastOne
    BooleanConstraintBuilder_Implies
)
    add_test(
        NAME ${test_case}
        COMMAND oracle_boolean_constraint_builder_tests ${test_case}
    )
endforeach()

add_executable(oracle_constraint_compiler_tests
    tests/unit/oracle/constraint_compiler_test.cpp
)

target_link_libraries(oracle_constraint_compiler_tests PRIVATE
    oracle_compiler
)

foreach(test_case
    ConstraintCompiler_ExactlyOne
    ConstraintCompiler_AtMostOne
    ConstraintCompiler_AtLeastOne
    ConstraintCompiler_Implies
    ConstraintCompiler_RejectsUnapprovedRules
    ConstraintCompiler_RejectsUnknownVariable
    ConstraintCompiler_DeterministicVariableOrdering
    ConstraintCompiler_DeterministicConstraintHash
    ConstraintCompiler_WritesMatrixArtifacts
)
    add_test(
        NAME ${test_case}
        COMMAND oracle_constraint_compiler_tests ${test_case}
    )
endforeach()

add_executable(oracle_feasibility_checker_tests
    tests/unit/oracle/feasibility_checker_test.cpp
)

target_link_libraries(oracle_feasibility_checker_tests PRIVATE
    oracle_enumerate
)

foreach(test_case
    FeasibilityChecker_SatisfiesExactlyOne
    FeasibilityChecker_DetectsContradiction
)
    add_test(
        NAME ${test_case}
        COMMAND oracle_feasibility_checker_tests ${test_case}
    )
endforeach()

add_executable(oracle_state_enumerator_tests
    tests/unit/oracle/state_enumerator_test.cpp
)

target_link_libraries(oracle_state_enumerator_tests PRIVATE
    oracle_enumerate
)

target_compile_definitions(oracle_state_enumerator_tests PRIVATE
    POLYTOPE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

foreach(test_case
    StateEnumerator_ExactlyOneTwoVarsProducesTwoStates
    StateEnumerator_ExactlyOneThreeVarsProducesThreeStates
    StateEnumerator_AtMostOneThreeVarsProducesFourStates
    StateEnumerator_RejectsMoreThan32Vars
    StateEnumerator_DeterministicStateOrdering
    StateEnumerator_ContradictoryRulebookProducesZeroStates
    StateEnumerator_ValidFixtureProducesFeasibleStates
    StateEnumerator_ContradictoryCompiledRulebookProducesZeroStates
)
    add_test(
        NAME ${test_case}
        COMMAND oracle_state_enumerator_tests ${test_case}
    )
endforeach()

add_executable(oracle_payoff_matrix_builder_tests
    tests/unit/oracle/payoff_matrix_builder_test.cpp
)

target_link_libraries(oracle_payoff_matrix_builder_tests PRIVATE
    oracle_payoff
)

foreach(test_case
    PayoffMatrixBuilder_BinaryOutcomeTruePaysOne
    PayoffMatrixBuilder_BinaryOutcomeFalsePaysZero
    PayoffMatrixBuilder_HasRowsForAllStates
    PayoffMatrixBuilder_HasColumnsForAllAssets
    PayoffMatrixBuilder_RejectsUnknownAsset
    PayoffMatrixBuilder_DeterministicHash
    PayoffMatrixBuilder_WritesArtifacts
)
    add_test(
        NAME ${test_case}
        COMMAND oracle_payoff_matrix_builder_tests ${test_case}
    )
endforeach()

add_executable(oracle_bundle_validator_tests
    tests/unit/oracle/bundle_validator_test.cpp
)

target_link_libraries(oracle_bundle_validator_tests PRIVATE
    oracle_bundles
)

foreach(test_case
    BundleValidator_AcceptsValidBundle
    BundleValidator_RejectsUnknownAsset
    BundleValidator_RejectsZeroLegs
    BundleValidator_RejectsTooManyLegs
    BundleValidator_RejectsConflictingMasks
    BundleValidator_RejectsDuplicateBundleId
)
    add_test(
        NAME ${test_case}
        COMMAND oracle_bundle_validator_tests ${test_case}
    )
endforeach()

add_executable(oracle_candidate_bundle_generator_tests
    tests/unit/oracle/candidate_bundle_generator_test.cpp
)

target_link_libraries(oracle_candidate_bundle_generator_tests PRIVATE
    oracle_bundles
)

target_compile_definitions(oracle_candidate_bundle_generator_tests PRIVATE
    POLYTOPE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

foreach(test_case
    CandidateBundleGenerator_LoadsFixtureBundles
    CandidateBundleGenerator_DeterministicHash
    CandidateBundleGenerator_ExportsArtifact
)
    add_test(
        NAME ${test_case}
        COMMAND oracle_candidate_bundle_generator_tests ${test_case}
    )
endforeach()

add_executable(oracle_artifact_exporter_tests
    tests/unit/oracle/artifact_exporter_test.cpp
)

target_link_libraries(oracle_artifact_exporter_tests PRIVATE
    oracle_artifact
)

foreach(test_case
    ArtifactExporter_WritesAllFiles
    ArtifactExporter_WritesManifest
    ArtifactExporter_WritesChecksums
    ArtifactExporter_DeterministicOutput
)
    add_test(
        NAME ${test_case}
        COMMAND oracle_artifact_exporter_tests ${test_case}
    )
endforeach()

add_executable(oracle_artifact_loader_tests
    tests/unit/oracle/artifact_loader_test.cpp
)

target_link_libraries(oracle_artifact_loader_tests PRIVATE
    oracle_artifact
)

foreach(test_case
    ArtifactLoader_RoundTripsArtifact
    ArtifactLoader_RejectsChecksumMismatch
)
    add_test(
        NAME ${test_case}
        COMMAND oracle_artifact_loader_tests ${test_case}
    )
endforeach()

add_executable(oracle_artifact_roundtrip_tests
    tests/integration/oracle/oracle_artifact_roundtrip_test.cpp
)

target_link_libraries(oracle_artifact_roundtrip_tests PRIVATE
    oracle_artifact
)

add_test(
    NAME OracleArtifactRoundTrip_WritesAndLoads
    COMMAND oracle_artifact_roundtrip_tests OracleArtifactRoundTrip_WritesAndLoads
)

add_executable(oracle_workflow_small_fixture_tests
    tests/integration/oracle/oracle_workflow_small_fixture_test.cpp
)

target_compile_definitions(oracle_workflow_small_fixture_tests PRIVATE
    POLYTOPE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    VERIFY_ORACLE_WORKFLOW_BIN="$<TARGET_FILE:verify_oracle_workflow>"
)

add_dependencies(oracle_workflow_small_fixture_tests verify_oracle_workflow)

foreach(test_case
    OracleWorkflow_SmallFixtureCompiles
    OracleWorkflow_LlmDisabledStillPasses
)
    add_test(
        NAME ${test_case}
        COMMAND oracle_workflow_small_fixture_tests ${test_case}
    )
endforeach()

add_executable(oracle_determinism_tests
    tests/integration/oracle/oracle_determinism_test.cpp
)

target_compile_definitions(oracle_determinism_tests PRIVATE
    POLYTOPE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    VERIFY_ORACLE_WORKFLOW_BIN="$<TARGET_FILE:verify_oracle_workflow>"
)

add_dependencies(oracle_determinism_tests verify_oracle_workflow)

add_test(
    NAME OracleWorkflow_DeterministicHashes
    COMMAND oracle_determinism_tests OracleWorkflow_DeterministicHashes
)

add_executable(oracle_contradiction_fails_tests
    tests/integration/oracle/oracle_contradiction_fails_test.cpp
)

target_compile_definitions(oracle_contradiction_fails_tests PRIVATE
    POLYTOPE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    VERIFY_ORACLE_WORKFLOW_BIN="$<TARGET_FILE:verify_oracle_workflow>"
)

add_dependencies(oracle_contradiction_fails_tests verify_oracle_workflow)

add_test(
    NAME OracleWorkflow_ContradictionFails
    COMMAND oracle_contradiction_fails_tests OracleWorkflow_ContradictionFails
)

add_executable(order_filled_decoder_tests
    tests/unit/chain_confirm/order_filled_decoder_test.cpp
)

target_include_directories(order_filled_decoder_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(order_filled_decoder_tests PRIVATE
    engine_chain_confirm
)

foreach(test_case
    OrderFilledDecoder_DecodesFields
    OrderFilledDecoder_RejectsWrongTopic
    OrderFilledDecoder_HandlesRemovedLog
    OrderFilledDecoder_DirectionFromAssetFields
)
    add_test(NAME ${test_case} COMMAND order_filled_decoder_tests ${test_case})
endforeach()

add_executable(confirmed_fill_store_tests
    tests/unit/chain_confirm/confirmed_fill_store_test.cpp
)

target_include_directories(confirmed_fill_store_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(confirmed_fill_store_tests PRIVATE
    engine_chain_confirm
)

foreach(test_case
    ConfirmedFillStore_IdempotentByTxHashLogIndex
    ConfirmedFillStore_MarksRemovedOnReorg
)
    add_test(NAME ${test_case} COMMAND confirmed_fill_store_tests ${test_case})
endforeach()

add_executable(fill_reconciler_tests
    tests/unit/chain_confirm/fill_reconciler_test.cpp
)

target_include_directories(fill_reconciler_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(fill_reconciler_tests PRIVATE
    engine_chain_confirm
)

foreach(test_case
    PendingTradeHintRing_ExpiresOldHints
    FillReconciler_OneToOneMatch
    FillReconciler_AmbiguousSamePriceSize
    FillReconciler_UnmatchedHintExpires
    FillReconciler_ChainFillOverwritesWsHint
    FillReconciler_RemovedLogMarksReorg
)
    add_test(NAME ${test_case} COMMAND fill_reconciler_tests ${test_case})
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
)

target_include_directories(health_publisher_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(health_publisher_tests PRIVATE
    engine_state
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
)

target_include_directories(feed_e2e_decode_pipeline_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(feed_e2e_decode_pipeline_tests PRIVATE
    engine_state
    engine_decode
)

add_test(
    NAME FeedE2E_UsesDecodePipelineMarket39StateStable
    COMMAND feed_e2e_decode_pipeline_tests
        FeedE2E_UsesDecodePipelineMarket39StateStable
)

add_executable(feed_state_compatibility_tests
    tests/integration/feed/feed_state_compatibility_test.cpp
)

target_include_directories(feed_state_compatibility_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(feed_state_compatibility_tests PRIVATE
    engine_state
)

add_test(
    NAME FeedStateCompatibility_OldPathAliasesStateTypes
    COMMAND feed_state_compatibility_tests
        FeedStateCompatibility_OldPathAliasesStateTypes
)

add_executable(decode_state_view_replay_tests
    tests/integration/state/decode_state_view_replay_test.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
)

target_include_directories(decode_state_view_replay_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(decode_state_view_replay_tests PRIVATE
    engine_state
    engine_decode
)

add_test(
    NAME DecodeStateViewReplay_Market39SnapshotQueryable
    COMMAND decode_state_view_replay_tests
        DecodeStateViewReplay_Market39SnapshotQueryable
)

add_executable(ws_book_to_snapshot_workflow_tests
    tests/integration/workflow/ws_book_to_snapshot_test.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
)

target_include_directories(ws_book_to_snapshot_workflow_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(ws_book_to_snapshot_workflow_tests PRIVATE
    engine_state
    engine_decode
)

add_test(
    NAME WsBookToSnapshot_Market39ReconstructsBook
    COMMAND ws_book_to_snapshot_workflow_tests
)

add_executable(chain_fill_to_snapshot_workflow_tests
    tests/integration/workflow/chain_fill_to_snapshot_test.cpp
)

target_include_directories(chain_fill_to_snapshot_workflow_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(chain_fill_to_snapshot_workflow_tests PRIVATE
    engine_state
)

add_test(
    NAME ChainFillToSnapshot_ConfirmedAndUnsafeFillsAreSeparated
    COMMAND chain_fill_to_snapshot_workflow_tests
)

add_executable(combined_market_state_workflow_tests
    tests/integration/workflow/combined_market_state_workflow_test.cpp
)

target_include_directories(combined_market_state_workflow_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

add_test(
    NAME CombinedMarketStateWorkflow_VerifierPasses
    COMMAND combined_market_state_workflow_tests
)

add_executable(chain_confirm_synthetic_reconciliation_tests
    tests/integration/chain_confirm/synthetic_reconciliation_test.cpp
)

target_include_directories(chain_confirm_synthetic_reconciliation_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(chain_confirm_synthetic_reconciliation_tests PRIVATE
    engine_chain_confirm
)

foreach(test_case
    SyntheticReconciliation_WsHintRequiresChainConfirmation
    SyntheticReconciliation_ChainFillConfirmsAndOverridesHint
    SyntheticReconciliation_AmbiguousIsNotForced
    SyntheticReconciliation_RemovedLogMarksFillRemoved
)
    add_test(
        NAME ${test_case}
        COMMAND chain_confirm_synthetic_reconciliation_tests ${test_case}
    )
endforeach()

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
)

target_include_directories(replay_raw_log PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(replay_raw_log PRIVATE
    engine_state
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
    engine/feed/integrity/ConsistencyChecker.cpp
)

target_include_directories(run_feed_e2e PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(run_feed_e2e PRIVATE
    engine_state
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

add_executable(bench_market_state_latency
    tools/bench_market_state_latency.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
)

target_include_directories(bench_market_state_latency PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(bench_market_state_latency PRIVATE
    engine_state
    engine_decode
)

add_executable(verify_market_state_workflow
    tools/verify_market_state_workflow.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/raw_ingest/RawLogReader.cpp
)

target_include_directories(verify_market_state_workflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(verify_market_state_workflow PRIVATE
    engine_state
    engine_decode
)

add_test(
    NAME VerifyMarketStateWorkflow
    COMMAND verify_market_state_workflow
        --raw ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/polymarket/market_39.raw
        --chain-fixture ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/chain_confirm/synthetic_order_filled.jsonl
        --repeat 1000
        --check-determinism
)

add_executable(multi_asset_synthetic_workflow
    tools/multi_asset_synthetic_workflow.cpp
)

target_include_directories(multi_asset_synthetic_workflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(multi_asset_synthetic_workflow PRIVATE
    engine_state
)

add_test(
    NAME MultiAssetSyntheticWorkflow
    COMMAND multi_asset_synthetic_workflow
)

add_executable(run_market_state_live_smoke
    tools/run_market_state_live_smoke.cpp
    engine/feed/raw_ingest/RawPacket.cpp
    engine/feed/source_runtime/WebSocketClient.cpp
)

target_include_directories(run_market_state_live_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(run_market_state_live_smoke PRIVATE
    engine_state
    engine_decode
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

add_executable(chain_ws_http_parity_smoke
    tools/chain_ws_http_parity_smoke.cpp
    engine/feed/source_runtime/WebSocketClient.cpp
)

target_include_directories(chain_ws_http_parity_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
)

target_link_libraries(chain_ws_http_parity_smoke PRIVATE
    engine_chain_confirm
    engine_decode
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

add_test(
    NAME MarketStateLiveSmokeManual
    COMMAND run_market_state_live_smoke
        --seconds 300
        --asset-id YOUR_POLYMARKET_ASSET_ID
)

set_tests_properties(
    MarketStateLiveSmokeManual
    PROPERTIES
        LABELS manual
        DISABLED TRUE
)

add_test(
    NAME ChainWsHttpParitySmokeManual
    COMMAND chain_ws_http_parity_smoke
        --seconds 300
)

set_tests_properties(
    ChainWsHttpParitySmokeManual
    PROPERTIES
        LABELS manual
        DISABLED TRUE
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
