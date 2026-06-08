add_library(engine_paper STATIC
    core/PaperTradingEngine.cpp
    core/PaperTradingWorkflow.cpp
    ledger/FillApplication.cpp
    ledger/MakerFillApplication.cpp
    ledger/PaperEventAdapter.cpp
    ledger/PaperLedger.cpp
    ledger/PositionLedger.cpp
    metrics/MakerPerformanceMetrics.cpp
    metrics/PerformanceMetricsEngine.cpp
    pnl/AdverseSelectionTracker.cpp
    pnl/MarkPriceProvider.cpp
    pnl/MakerPnLEngine.cpp
    pnl/PaperPnLEngine.cpp
    pnl/RewardPnLEngine.cpp
    portfolio/PaperPortfolio.cpp
    public/PaperModule.cpp
    read/DashboardReadStore.cpp
    regime/RegimeEngine.cpp
)

target_compile_features(engine_paper PUBLIC cxx_std_20)

target_include_directories(engine_paper PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(engine_paper PUBLIC
    engine_state
    engine_signal
    engine_risk
    engine_execution
    engine_reward
)

add_library(paper_tools STATIC
    tools/PaperTools.cpp
)

target_compile_features(paper_tools PUBLIC cxx_std_20)

target_include_directories(paper_tools PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(paper_tools PUBLIC
    engine_paper
)

add_executable(verify_paper_trading_workflow
    tools/verify_paper_trading_workflow.cpp
)

target_compile_features(verify_paper_trading_workflow PRIVATE cxx_std_20)

target_include_directories(verify_paper_trading_workflow PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(verify_paper_trading_workflow PRIVATE
    engine_paper
)

set_target_properties(verify_paper_trading_workflow PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_executable(verify_market_making_pnl_workflow
    tools/verify_market_making_pnl_workflow.cpp
)

target_compile_features(verify_market_making_pnl_workflow PRIVATE cxx_std_20)

target_include_directories(verify_market_making_pnl_workflow PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(verify_market_making_pnl_workflow PRIVATE
    engine_paper
)

set_target_properties(verify_market_making_pnl_workflow PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_executable(paper_skeleton_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/paper/paper_skeleton_test.cpp
)

target_compile_features(paper_skeleton_tests PRIVATE cxx_std_20)

target_link_libraries(paper_skeleton_tests PRIVATE
    engine_paper
    paper_tools
)

foreach(test_case
    PaperModule_DefaultsToNoNetwork
    PaperModule_PublicContractsCompile
    PaperTools_TargetAvailable
    PaperEvent_IsObservationOnly
    PaperSnapshot_DefaultsSafe
)
    add_test(NAME ${test_case} COMMAND paper_skeleton_tests ${test_case})
endforeach()

add_executable(paper_trading_engine_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/paper/paper_trading_engine_test.cpp
)

target_compile_features(paper_trading_engine_tests PRIVATE cxx_std_20)

target_link_libraries(paper_trading_engine_tests PRIVATE
    engine_paper
)

foreach(test_case
    PaperTradingEngine_ExecutionReportUpdatesDashboard
    PaperTradingEngine_OpportunityDoesNotCreatePnL
    PaperTradingEngine_RiskDecisionDoesNotChangeLedger
    PaperTradingWorkflow_ExposesLatestDashboard
)
    add_test(NAME ${test_case} COMMAND paper_trading_engine_tests ${test_case})
endforeach()

add_executable(verify_paper_trading_workflow_tests
    ${CMAKE_SOURCE_DIR}/tests/integration/paper/verify_paper_trading_workflow_test.cpp
)

target_compile_features(verify_paper_trading_workflow_tests PRIVATE cxx_std_20)

target_compile_definitions(verify_paper_trading_workflow_tests PRIVATE
    POLYTOPE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    VERIFY_PAPER_TRADING_WORKFLOW_BIN="$<TARGET_FILE:verify_paper_trading_workflow>"
)

add_dependencies(
    verify_paper_trading_workflow_tests
    verify_paper_trading_workflow
)

foreach(test_case
    VerifyPaperTradingWorkflow_PositiveFixtureRuns
    VerifyPaperTradingWorkflow_DeterministicHashes
    VerifyPaperTradingWorkflow_EmitsSummary
)
    add_test(
        NAME ${test_case}
        COMMAND verify_paper_trading_workflow_tests ${test_case}
    )
endforeach()

add_executable(market_making_pnl_workflow_tests
    ${CMAKE_SOURCE_DIR}/tests/integration/paper/market_making_pnl_workflow_test.cpp
)

target_compile_features(market_making_pnl_workflow_tests PRIVATE cxx_std_20)

target_compile_definitions(market_making_pnl_workflow_tests PRIVATE
    POLYTOPE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    VERIFY_MARKET_MAKING_PNL_WORKFLOW_BIN="$<TARGET_FILE:verify_market_making_pnl_workflow>"
)

add_dependencies(
    market_making_pnl_workflow_tests
    verify_market_making_pnl_workflow
)

foreach(test_case
    MakerRoundTrip_ComputesExpectedRealizedPnl
    MarketMakingPnlWorkflow_Deterministic
    MarketMakingPnlWorkflow_NoFillProducesZeroPnl
)
    add_test(
        NAME ${test_case}
        COMMAND market_making_pnl_workflow_tests ${test_case}
    )
endforeach()

add_executable(paper_ledger_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/paper/paper_ledger_test.cpp
)

target_compile_features(paper_ledger_tests PRIVATE cxx_std_20)

target_link_libraries(paper_ledger_tests PRIVATE
    engine_paper
)

foreach(test_case
    PaperLedger_InitialCash
    PaperLedger_ApplyBuyFill
    PaperLedger_RejectsDuplicateExecutionReport
    PaperLedger_TracksFees
    PaperLedger_UpdatesCash
    PaperLedger_OpportunityAndRiskEventsDoNotProducePnL
    PaperLedger_MakerBidFillIncreasesPosition
    PaperLedger_MakerBidFillDecreasesCash
    PaperLedger_MakerAskFillRealizesPnl
    PaperLedger_DuplicateMakerReportIgnored
    PaperLedger_TakerBuyFillAppliesWithoutMakerCount
)
    add_test(NAME ${test_case} COMMAND paper_ledger_tests ${test_case})
endforeach()

add_executable(maker_fill_application_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/paper/maker_fill_application_test.cpp
)

target_compile_features(maker_fill_application_tests PRIVATE cxx_std_20)

target_link_libraries(maker_fill_application_tests PRIVATE
    engine_paper
)

foreach(test_case
    MakerFillApplication_CreatesFillFromFilledMakerReport
    MakerFillApplication_DoesNotCreateFillFromUnfilledReport
    MakerFillApplication_PreservesQuoteIds
)
    add_test(NAME ${test_case} COMMAND maker_fill_application_tests ${test_case})
endforeach()

add_executable(paper_event_adapter_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/paper/paper_event_adapter_test.cpp
)

target_compile_features(paper_event_adapter_tests PRIVATE cxx_std_20)

target_link_libraries(paper_event_adapter_tests PRIVATE
    engine_paper
)

foreach(test_case
    Adapter_ExecutionReportCreatesFillEvent
    Adapter_OpportunityIntentDoesNotCreateFill
    Adapter_RiskDecisionDoesNotChangeLedger
    Adapter_QuoteIntentDoesNotCreateFill
    Adapter_ApprovedQuoteDoesNotCreateFill
    Adapter_QuoteRiskDecisionDoesNotCreateFill
    Adapter_MakerExecutionReportWithFillCreatesPaperFill
    Adapter_MakerExecutionReportWithoutFillDoesNotCreatePaperFill
)
    add_test(NAME ${test_case} COMMAND paper_event_adapter_tests ${test_case})
endforeach()

add_executable(position_ledger_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/paper/position_ledger_test.cpp
)

target_compile_features(position_ledger_tests PRIVATE cxx_std_20)

target_link_libraries(position_ledger_tests PRIVATE
    engine_paper
)

foreach(test_case
    PositionLedger_BuyIncreasesQty
    PositionLedger_BuyUpdatesAvgCost
    PositionLedger_RejectsUnsupportedSellInV0
    PaperPortfolio_BuyFillUpdatesExposure
)
    add_test(NAME ${test_case} COMMAND position_ledger_tests ${test_case})
endforeach()

add_executable(maker_pnl_engine_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/paper/maker_pnl_engine_test.cpp
)

target_compile_features(maker_pnl_engine_tests PRIVATE cxx_std_20)

target_link_libraries(maker_pnl_engine_tests PRIVATE
    engine_paper
)

foreach(test_case
    MakerPnL_MarksLongAtMid
    MakerPnL_MarksLongAtLiquidationBid
    MakerPnL_MissingBidLiquidationMarkZero
    MakerPnL_DegradedBookDoesNotProduceGoodMark
)
    add_test(NAME ${test_case} COMMAND maker_pnl_engine_tests ${test_case})
endforeach()

add_executable(paper_pnl_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/paper/paper_pnl_test.cpp
)

target_compile_features(paper_pnl_tests PRIVATE cxx_std_20)

target_link_libraries(paper_pnl_tests PRIVATE
    engine_paper
)

foreach(test_case
    PnL_MarksLongAtMid
    PnL_MarksLongAtLiquidationBid
    PnL_MissingBidIsDegraded
    PnL_ComputesEquityCurve
)
    add_test(NAME ${test_case} COMMAND paper_pnl_tests ${test_case})
endforeach()

add_executable(adverse_selection_tracker_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/paper/adverse_selection_tracker_test.cpp
)

target_compile_features(adverse_selection_tracker_tests PRIVATE cxx_std_20)

target_link_libraries(adverse_selection_tracker_tests PRIVATE
    engine_paper
)

foreach(test_case
    AdverseSelection_BidFillDownMoveNegative
    AdverseSelection_AskFillUpMoveNegative
    AdverseSelection_PendingWhenHorizonMissing
)
    add_test(NAME ${test_case} COMMAND adverse_selection_tracker_tests ${test_case})
endforeach()

add_executable(maker_performance_metrics_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/paper/maker_performance_metrics_test.cpp
)

target_compile_features(maker_performance_metrics_tests PRIVATE cxx_std_20)

target_link_libraries(maker_performance_metrics_tests PRIVATE
    engine_paper
)

foreach(test_case
    MakerMetrics_ComputesSpreadCapture
)
    add_test(NAME ${test_case} COMMAND maker_performance_metrics_tests ${test_case})
endforeach()

add_executable(performance_metrics_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/paper/performance_metrics_test.cpp
)

target_compile_features(performance_metrics_tests PRIVATE cxx_std_20)

target_link_libraries(performance_metrics_tests PRIVATE
    engine_paper
)

foreach(test_case
    Sharpe_InsufficientSamples
    Sharpe_ZeroVolReturnsInsufficient
    Drawdown_ComputesMaxDrawdown
    Performance_ComputesFillRate
)
    add_test(NAME ${test_case} COMMAND performance_metrics_tests ${test_case})
endforeach()

add_executable(regime_engine_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/paper/regime_engine_test.cpp
)

target_compile_features(regime_engine_tests PRIVATE cxx_std_20)

target_link_libraries(regime_engine_tests PRIVATE
    engine_paper
)

foreach(test_case
    Regime_HealthyWhenAllOk
    Regime_StaleWhenBookStale
    Regime_RiskRejecting
    Regime_HedgeRequired
)
    add_test(NAME ${test_case} COMMAND regime_engine_tests ${test_case})
endforeach()

add_executable(dashboard_read_store_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/paper/dashboard_read_store_test.cpp
)

target_compile_features(dashboard_read_store_tests PRIVATE cxx_std_20)

target_link_libraries(dashboard_read_store_tests PRIVATE
    engine_paper
)

foreach(test_case
    DashboardReadStore_PublishesLatest
    DashboardReadStore_ReadsSinceSeq
    DashboardReadStore_DropsOldFramesWhenRingFull
)
    add_test(NAME ${test_case} COMMAND dashboard_read_store_tests ${test_case})
endforeach()
