add_library(engine_order_decision STATIC
    core/FastFixedShapeOrderDecisionImpl.cpp
    core/GenericOrderDecisionImpl.cpp
    core/OrderDecisionCache.cpp
    core/OrderDecisionEngine.cpp
    limits/LimitPriceBuilder.cpp
    math/CostCurve.cpp
    math/PrefixVwap.cpp
    public/OrderDecision.cpp
    sizing/BundleSizeOptimizer.cpp
    sizing/CostCurveBuilder.cpp
    sizing/SizeCandidateEnumerator.cpp
)

target_compile_features(engine_order_decision PUBLIC cxx_std_20)

target_include_directories(engine_order_decision PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(engine_order_decision PUBLIC
    engine_state
    oracle_bundles
)

add_library(order_decision_tools STATIC
    tools/OrderDecisionTools.cpp
)

target_compile_features(order_decision_tools PUBLIC cxx_std_20)

target_include_directories(order_decision_tools PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(order_decision_tools PUBLIC
    engine_order_decision
)

add_executable(verify_order_decision_workflow
    tools/verify_order_decision_workflow.cpp
)

target_link_libraries(verify_order_decision_workflow PRIVATE
    order_decision_tools
)

set_target_properties(verify_order_decision_workflow PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

foreach(test_case
    VerifyOrderDecisionWorkflow_PositiveRuns
    VerifyOrderDecisionWorkflow_Deterministic
    VerifyOrderDecisionWorkflow_EmitsSummary
)
    add_test(NAME ${test_case}
        COMMAND verify_order_decision_workflow
            --intent-fixture ${CMAKE_SOURCE_DIR}/tests/fixtures/order_decision/intents_positive.jsonl
            --depth-fixture ${CMAKE_SOURCE_DIR}/tests/fixtures/order_decision/depth_positive.json
            --bundle-fixture ${CMAKE_SOURCE_DIR}/tests/fixtures/order_decision/bundles_positive.json
            --risk-policy ${CMAKE_SOURCE_DIR}/tests/fixtures/order_decision/risk_policy_positive.json
            --check-determinism
            --require-decision
    )
endforeach()

add_executable(order_decision_contract_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/order_decision/order_decision_contract_test.cpp
)
target_link_libraries(order_decision_contract_tests PRIVATE
    engine_order_decision
)

foreach(test_case
    OrderDecision_DefaultNoTrade
    OrderDecision_HasSnapshotBundlePolicyHashes
    OrderDecisionLeg_HasQtyAndLimitPrice
    ApprovedOrderDecisionEnvelope_HasDecisionAndApprovalHashes
)
    add_test(NAME ${test_case} COMMAND order_decision_contract_tests ${test_case})
endforeach()

add_executable(order_decision_cost_curve_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/order_decision/cost_curve_builder_test.cpp
)
target_link_libraries(order_decision_cost_curve_tests PRIVATE
    engine_order_decision
)

foreach(test_case
    CostCurveBuilder_BuildsAskCurve
    CostCurveBuilder_RejectsNoAskDepth
    CostCurveBuilder_PreservesCumulativeCost
)
    add_test(NAME ${test_case} COMMAND order_decision_cost_curve_tests ${test_case})
endforeach()

add_executable(order_decision_size_candidate_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/order_decision/size_candidate_enumerator_test.cpp
)
target_link_libraries(order_decision_size_candidate_tests PRIVATE
    engine_order_decision
)

foreach(test_case
    SizeEnumerator_GeneratesBreakpoints
    SizeEnumerator_DeduplicatesCandidates
    SizeEnumerator_ClampsToRiskBudget
    KWayCandidateEnumerator_ProducesSameCandidatesAsGeneric
)
    add_test(NAME ${test_case} COMMAND order_decision_size_candidate_tests ${test_case})
endforeach()

add_executable(order_decision_prefix_vwap_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/order_decision/prefix_vwap_test.cpp
)
target_link_libraries(order_decision_prefix_vwap_tests PRIVATE
    engine_order_decision
)

foreach(test_case
    PrefixVWAP_OneLevelExact
    PrefixVWAP_MultiLevelExact
    PrefixVWAP_ExactBoundary
    PrefixVWAP_BoundaryPlusOne
    PrefixVWAP_InsufficientDepth
    PrefixVWAP_MatchesLinearVWAP
)
    add_test(NAME ${test_case} COMMAND order_decision_prefix_vwap_tests ${test_case})
endforeach()

add_executable(order_decision_bundle_size_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/order_decision/bundle_size_optimizer_test.cpp
)
target_link_libraries(order_decision_bundle_size_tests PRIVATE
    engine_order_decision
)

foreach(test_case
    Optimizer_PicksMaxTotalEdge
    Optimizer_RejectsLowUnitEdge
    Optimizer_RejectsLowTotalEdge
    Optimizer_RejectsInsufficientDepth
    Optimizer_TieBreaksByEdgeBpsThenCost
)
    add_test(NAME ${test_case} COMMAND order_decision_bundle_size_tests ${test_case})
endforeach()

add_executable(order_decision_limit_price_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/order_decision/limit_price_builder_test.cpp
)
target_link_libraries(order_decision_limit_price_tests PRIVATE
    engine_order_decision
)

foreach(test_case
    LimitPriceBuilder_UsesWorstConsumedPrice
    LimitPriceBuilder_AddsProtectionBuffer
    LimitPriceBuilder_RejectsAboveMaxAllowed
)
    add_test(NAME ${test_case} COMMAND order_decision_limit_price_tests ${test_case})
endforeach()

add_executable(order_decision_engine_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/order_decision/order_decision_engine_test.cpp
)
target_link_libraries(order_decision_engine_tests PRIVATE
    engine_order_decision
)

foreach(test_case
    OrderDecisionEngine_ProducesDecisionForBuyOnlyBundle
    OrderDecisionEngine_RejectsSellInV1
    OrderDecisionEngine_RejectsExpiredIntent
    OrderDecisionEngine_DeterministicDecisionHash
    OrderDecisionEngine_FastFixedShapeReturnsOrderDecisionResult
    OrderDecisionEngine_FastFixedShapeRejectsSellLeg
    OrderDecisionEngine_FastFixedShapeMatchesGenericPositive
    OrderDecisionEngine_GenericMemoCacheExactHit
    OrderDecisionEngine_GenericCandidateCacheAndIncrementalEval
    OrderDecisionEngine_FastFixedShapeSpecCacheHit
)
    add_test(NAME ${test_case} COMMAND order_decision_engine_tests ${test_case})
endforeach()

add_executable(order_decision_prefix_vs_linear_tests
    ${CMAKE_SOURCE_DIR}/tests/integration/order_decision/order_decision_prefix_vs_linear_test.cpp
)
target_link_libraries(order_decision_prefix_vs_linear_tests PRIVATE
    engine_order_decision
)

add_test(NAME OrderDecision_PrefixMatchesLinearOnSmallFixture
    COMMAND order_decision_prefix_vs_linear_tests
        OrderDecision_PrefixMatchesLinearOnSmallFixture
)
