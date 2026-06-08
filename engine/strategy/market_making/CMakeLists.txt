add_library(engine_strategy_market_making STATIC
    core/MarketMakingEngine.cpp
    fair/DigitalOptionFairModel.cpp
    fair/FairValueModel.cpp
    metrics/MarketMakingMetrics.cpp
    public/CancelQuoteIntent.cpp
    public/MarketMakingResult.cpp
    public/QuoteIntent.cpp
    quote/QuoteEngine.cpp
    quote/RewardAwareQuoteConstraint.cpp
    refresh/QuoteRefreshPolicy.cpp
    state/QuoteBook.cpp
)

target_compile_features(engine_strategy_market_making PUBLIC cxx_std_20)

target_include_directories(engine_strategy_market_making PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(engine_strategy_market_making PUBLIC
    engine_state
)

add_library(market_making_tools STATIC
    tools/MarketMakingTools.cpp
)

target_compile_features(market_making_tools PUBLIC cxx_std_20)

target_include_directories(market_making_tools PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(market_making_tools PUBLIC
    engine_strategy_market_making
)

add_executable(verify_market_making_workflow
    tools/verify_market_making_workflow.cpp
)

target_link_libraries(verify_market_making_workflow PRIVATE
    market_making_tools
)

set_target_properties(verify_market_making_workflow PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

foreach(test_case
    VerifyMarketMakingWorkflow_PostsQuote
    VerifyMarketMakingWorkflow_Deterministic
    VerifyMarketMakingWorkflow_EmitsSummary
)
    add_test(NAME ${test_case}
        COMMAND verify_market_making_workflow
            --check-determinism
            --require-quote
    )
endforeach()

add_executable(market_making_contract_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/market_making/market_making_contract_test.cpp
)
target_link_libraries(market_making_contract_tests PRIVATE
    engine_strategy_market_making
)

foreach(test_case
    QuoteIntent_DefaultNone
    QuoteIntent_HasHashes
    CancelQuoteIntent_HasReason
    MarketMakingConfig_DefaultsSafe
)
    add_test(NAME ${test_case} COMMAND market_making_contract_tests ${test_case})
endforeach()

add_executable(market_making_fair_value_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/market_making/fair_value_model_test.cpp
)
target_link_libraries(market_making_fair_value_tests PRIVATE
    market_making_tools
)

foreach(test_case
    DigitalOptionFairModel_PricesAtmDigitalBelowHalf
    DigitalOptionFairModel_ExpiresToIntrinsic
    AvellanedaStoikovModel_WidensOnVolatilityMultiplier
    FairValueModel_MidFairValue
    FairValueModel_MicropriceMovesTowardThinAsk
    FairValueModel_BlendsComplementImpliedFair
    FairValueModel_ExternalFairOverridesBookFair
    FairValueModel_ExternalFairBypassesBookLowConfidence
    FairValueModel_RejectsMissingRequiredExternalFair
    FairValueModel_RejectsWideSpread
    FairValueModel_RejectsMissingBidAsk
    FairValueModel_RejectsCrossedBook
    FairValueModel_RejectsStaleBook
)
    add_test(NAME ${test_case} COMMAND market_making_fair_value_tests ${test_case})
endforeach()

add_executable(market_making_quote_model_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/market_making/quote_model_test.cpp
)
target_link_libraries(market_making_quote_model_tests PRIVATE
    market_making_tools
)

foreach(test_case
    SpreadModel_UsesMaxBuffers
    InventorySkew_LongInventoryShiftsQuotesDown
    InventorySkew_UsesTargetInventoryAsNeutral
    InventorySkew_TteAmplifiesNearExpiry
    InventorySkew_NonlinearPressureAcceleratesPastThreshold
    QuoteSizeModel_ReducesBidNearInventoryLimit
    QuoteEngine_BuildsBidAsk
    QuoteEngine_DisablesBidSide
    QuoteEngine_AllowsReducingAskBelowFair
    QuoteEngine_PassiveReduceAskJoinsBestAsk
    QuoteEngine_UrgentReduceAskMovesToMidpoint
    QuoteEngine_ReduceOnlyQtyCappedToTarget
    QuoteEngine_ClampsOpeningAskToCorrectSideOfFair
    QuoteEngine_RequiresExternalFairForOpeningQuotes
    QuoteEngine_DropsOpeningBidWhenReducingAskWouldSelfCross
    QuoteEngine_RejectsCrossedQuote
)
    add_test(NAME ${test_case} COMMAND market_making_quote_model_tests ${test_case})
endforeach()

add_executable(market_making_refresh_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/market_making/quote_refresh_policy_test.cpp
)
target_link_libraries(market_making_refresh_tests PRIVATE
    market_making_tools
)

foreach(test_case
    QuoteRefreshPolicy_ReplacesOnFairMove
    QuoteRefreshPolicy_DoesNotReplaceSameShapeWithNewHash
    QuoteRefreshPolicy_HoldsChangedQuoteInsideTwoMsCooldown
    QuoteRefreshPolicy_CancelsOnStaleBook
    QuoteRefreshPolicy_CancelsOnTtl
)
    add_test(NAME ${test_case} COMMAND market_making_refresh_tests ${test_case})
endforeach()

add_executable(market_making_quote_book_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/market_making/quote_book_test.cpp
)
target_link_libraries(market_making_quote_book_tests PRIVATE
    engine_strategy_market_making
)

foreach(test_case
    QuoteBook_UpsertsAndFinds
    QuoteBook_Removes
    QuoteBook_HashDeterministic
    QuoteStateMachine_RejectsTerminalTransition
)
    add_test(NAME ${test_case} COMMAND market_making_quote_book_tests ${test_case})
endforeach()

add_executable(market_making_engine_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/market_making/market_making_engine_test.cpp
)
target_link_libraries(market_making_engine_tests PRIVATE
    market_making_tools
)

foreach(test_case
    MarketMakingWorkflow_PostsQuote
    MarketMakingWorkflow_CancelsOnFairMove
    MarketMakingWorkflow_InventorySkewChangesQuote
    MarketMakingWorkflow_CancelsOnBadBook
    MarketMakingWorkflow_ReportsNoQuoteReason
    MarketMakingWorkflow_ReportsExternalStaleNoQuoteReason
    MarketMakingWorkflow_ExternalRemovalAllowsRequote
    MarketMakingWorkflow_DynamicHalfSpreadOverridesStaticBase
)
    add_test(NAME ${test_case} COMMAND market_making_engine_tests ${test_case})
endforeach()
