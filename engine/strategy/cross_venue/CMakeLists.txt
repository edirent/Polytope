# Offline composition reuses existing State view/math, RiskLedger and PaperLedger.
add_library(engine_cross_venue STATIC
    ${CMAKE_SOURCE_DIR}/oracle/artifact/ApprovedPairArtifact.cpp
    ${CMAKE_SOURCE_DIR}/engine/state/cross_venue/Types.cpp
    ${CMAKE_SOURCE_DIR}/engine/state/cross_venue/SynchronizedBookStore.cpp
    ${CMAKE_SOURCE_DIR}/engine/decode/parser/kalshi/KalshiBookNormalizer.cpp
    ${CMAKE_SOURCE_DIR}/engine/decode/parser/polymarket/CrossVenueClobNormalizer.cpp
    ${CMAKE_SOURCE_DIR}/engine/risk/ledger/MultiVenueReservationBook.cpp
    ${CMAKE_SOURCE_DIR}/engine/execution/state/PairExecutionStateMachine.cpp
    ${CMAKE_SOURCE_DIR}/engine/paper/ledger/TwoAccountPaperLedger.cpp
    ${CMAKE_SOURCE_DIR}/engine/state/cross_venue/JointBookAsOf.cpp
    PairScanner.cpp
    FixtureIO.cpp
)
target_compile_features(engine_cross_venue PUBLIC cxx_std_20)
target_include_directories(engine_cross_venue PUBLIC ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/engine)
target_link_libraries(engine_cross_venue PUBLIC engine_paper OpenSSL::Crypto)

add_executable(run_cross_venue_shadow ${CMAKE_SOURCE_DIR}/tools/run_cross_venue_shadow.cpp)
target_link_libraries(run_cross_venue_shadow PRIVATE engine_cross_venue)
set_target_properties(run_cross_venue_shadow PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(cross_venue_v0_tests ${CMAKE_SOURCE_DIR}/tests/integration/cross_venue/cross_venue_v0_test.cpp)
target_link_libraries(cross_venue_v0_tests PRIVATE engine_cross_venue)
target_compile_definitions(cross_venue_v0_tests PRIVATE POLYTOPE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(cross_venue_v0_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME CrossVenueV0_ContractsStateRiskExecutionAccounting COMMAND cross_venue_v0_tests)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME CrossVenueV0_PythonReference COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests -v)
    set_tests_properties(CrossVenueV0_PythonReference PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/research/cross_venue)
    add_test(NAME CrossVenueV0_GoldenParityAndDeterminism
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/cross_venue/golden_parity.py $<TARGET_FILE:run_cross_venue_shadow>)
endif()

execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE CV_GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE)
add_library(engine_cross_venue_research STATIC
    ${CMAKE_SOURCE_DIR}/engine/feed/source_runtime/CrossVenuePublicData.cpp
    ${CMAKE_SOURCE_DIR}/engine/feed/source_runtime/CrossVenueLiveCapture.cpp
    ${CMAKE_SOURCE_DIR}/engine/feed/source_runtime/WebSocketClient.cpp
    ${CMAKE_SOURCE_DIR}/replay/cross_venue/LiveL2Replay.cpp
    ${CMAKE_SOURCE_DIR}/replay/cross_venue/FeedAudit.cpp
    GeneralizedDiscovery.cpp
    GeneralizedQuantityVerification.cpp
    GeneralizedOptimizer.cpp
    GeneralizedReplay.cpp
    EconomicDiscovery.cpp
    MarketDiscovery.cpp
    ResearchState.cpp
)
target_compile_features(engine_cross_venue_research PUBLIC cxx_std_20)
target_link_libraries(engine_cross_venue_research PUBLIC engine_cross_venue Boost::system OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
target_compile_definitions(engine_cross_venue_research PRIVATE
    POLYTOPE_SOURCE_DIR="${CMAKE_SOURCE_DIR}" POLYTOPE_GIT_REVISION="${CV_GIT_REV}")
foreach(command cross_venue_discover run_cross_venue_live_shadow run_cross_venue_l2_backtest cross_venue_feed_audit cross_venue_key_check cross_venue_economic_discovery)
    add_executable(${command} ${CMAKE_SOURCE_DIR}/tools/cross_venue_research_main.cpp)
    target_link_libraries(${command} PRIVATE engine_cross_venue_research)
    target_compile_definitions(${command} PRIVATE CV_RESEARCH_COMMAND="${command}")
    set_target_properties(${command} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endforeach()
add_executable(cross_venue_v0_1_tests ${CMAKE_SOURCE_DIR}/tests/integration/cross_venue/cross_venue_v0_1_test.cpp)
target_link_libraries(cross_venue_v0_1_tests PRIVATE engine_cross_venue_research)
target_compile_definitions(cross_venue_v0_1_tests PRIVATE POLYTOPE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(cross_venue_v0_1_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME CrossVenueV01_NativeDiscoveryHealthLatencyCapitalAudit COMMAND cross_venue_v0_1_tests)
if(Python3_Interpreter_FOUND)
    add_test(NAME CrossVenueV01_IndependentPythonAudit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/cross_venue/independent_v0_1_audit.py $<TARGET_FILE:cross_venue_v0_1_tests>)
    add_test(NAME CrossVenueV011_RealCandidateReviewAndCampaignGates
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/cross_venue/test_v0_1_1_review.py)
    add_test(NAME CrossVenueV011_SecureLocalEnvironmentTransfer
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/cross_venue/test_secure_kalshi_environment.py)
    add_test(NAME CrossVenueV011_AuthenticatedWorkflowEvidenceGates
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/cross_venue/test_authenticated_ws_workflow.py)
endif()

add_executable(cross_venue_v0_1_2_tests ${CMAKE_SOURCE_DIR}/tests/integration/cross_venue/cross_venue_v0_1_2_test.cpp)
target_link_libraries(cross_venue_v0_1_2_tests PRIVATE engine_cross_venue_research)
set_target_properties(cross_venue_v0_1_2_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME CrossVenueV012_AsOfDepthFeesClockAndApproval COMMAND cross_venue_v0_1_2_tests)
if(Python3_Interpreter_FOUND)
    add_test(NAME CrossVenueV012_RealReplayIndependentArithmeticDepthAndDeterminism
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/cross_venue/v012_replay_audit.py $<TARGET_FILE:cross_venue_economic_discovery>)
    set_tests_properties(CrossVenueV012_RealReplayIndependentArithmeticDepthAndDeterminism PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_executable(run_generalized_payoff_research ${CMAKE_SOURCE_DIR}/tools/generalized_payoff_main.cpp)
target_link_libraries(run_generalized_payoff_research PRIVATE engine_cross_venue_research)
set_target_properties(run_generalized_payoff_research PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(generalized_payoff_tests ${CMAKE_SOURCE_DIR}/tests/integration/cross_venue/generalized_payoff_test.cpp)
target_link_libraries(generalized_payoff_tests PRIVATE engine_cross_venue_research)
set_target_properties(generalized_payoff_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME CrossVenueV013_GeneralizedPayoffOptimizerCapitalApproval COMMAND generalized_payoff_tests)
if(Python3_Interpreter_FOUND)
    add_test(NAME CrossVenueV013_StructuredSourcesCounterexamplesAndNoLive
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/cross_venue/generalized_discovery_test.py $<TARGET_FILE:run_generalized_payoff_research>)
endif()

if(Python3_Interpreter_FOUND)
    add_test(NAME CrossVenueV013_IndependentRealL2OptimizerAndDeterminism
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/cross_venue/generalized_replay_audit_test.py $<TARGET_FILE:run_generalized_payoff_research>)
    set_tests_properties(CrossVenueV013_IndependentRealL2OptimizerAndDeterminism PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(Python3_Interpreter_FOUND)
    add_test(NAME CrossVenueV0131_IndependentRealQuantityBreakpointsAndBounds
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/cross_venue/v0131_quantity_audit_test.py $<TARGET_FILE:run_generalized_payoff_research>)
    set_tests_properties(CrossVenueV0131_IndependentRealQuantityBreakpointsAndBounds PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_executable(cross_venue_overlap_inventory ${CMAKE_SOURCE_DIR}/tools/overlap_inventory_main.cpp)
target_link_libraries(cross_venue_overlap_inventory PRIVATE engine_cross_venue_research)
set_target_properties(cross_venue_overlap_inventory PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
