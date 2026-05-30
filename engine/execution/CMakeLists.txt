add_library(engine_execution STATIC
    public/ExecutionTypes.cpp
    core/ExecutionGateway.cpp
    core/ExecutionWorkflow.cpp
    plan/ExecutionPlanner.cpp
    plan/OrderBuilder.cpp
    plan/PlanValidator.cpp
    adapter/PaperExecutionAdapter.cpp
    adapter/SandboxExecutionAdapter.cpp
    adapter/LiveExecutionAdapter.cpp
    state/OrderStateMachine.cpp
    state/PlanStateMachine.cpp
    state/FillTracker.cpp
    state/PartialFillPolicy.cpp
    cancel/CancelManager.cpp
    metrics/ExecutionMetrics.cpp
)

target_compile_features(engine_execution PUBLIC cxx_std_20)

target_include_directories(engine_execution PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(engine_execution PUBLIC
    engine_risk
)

add_executable(verify_execution_workflow
    tools/verify_execution_workflow.cpp
)

target_compile_features(verify_execution_workflow PRIVATE cxx_std_20)

target_include_directories(verify_execution_workflow PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(verify_execution_workflow PRIVATE
    engine_execution
)

set_target_properties(verify_execution_workflow PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_executable(execution_child_order_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/execution/child_order_test.cpp
)

target_compile_features(execution_child_order_tests PRIVATE cxx_std_20)

target_link_libraries(execution_child_order_tests PRIVATE
    engine_execution
)

foreach(test_case
    ChildOrder_DefaultsCreated
    ChildOrder_RejectsZeroQuantity
)
    add_test(NAME ${test_case} COMMAND execution_child_order_tests ${test_case})
endforeach()

add_executable(execution_order_plan_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/execution/order_plan_test.cpp
)

target_compile_features(execution_order_plan_tests PRIVATE cxx_std_20)

target_link_libraries(execution_order_plan_tests PRIVATE
    engine_execution
)

foreach(test_case
    OrderPlan_DefaultsCreated
    OrderPlan_RequiresReservation
    OrderPlan_RejectsTooManyChildOrders
)
    add_test(NAME ${test_case} COMMAND execution_order_plan_tests ${test_case})
endforeach()

add_executable(execution_planner_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/execution/execution_planner_test.cpp
)

target_compile_features(execution_planner_tests PRIVATE cxx_std_20)

target_link_libraries(execution_planner_tests PRIVATE
    engine_execution
)

foreach(test_case
    ExecutionPlanner_BuildsPlanFromApprovedIntent
    ExecutionPlanner_RejectsMissingReservation
    ExecutionPlanner_RejectsExpiredIntent
    ExecutionPlanner_RejectsMismatchedIdempotencyKey
    ExecutionPlanner_RejectsSellLegInV0
    ExecutionPlanner_GeneratesDeterministicClientOrderIds
)
    add_test(NAME ${test_case} COMMAND execution_planner_tests ${test_case})
endforeach()

add_executable(execution_plan_validator_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/execution/plan_validator_test.cpp
)

target_compile_features(execution_plan_validator_tests PRIVATE cxx_std_20)

target_link_libraries(execution_plan_validator_tests PRIVATE
    engine_execution
)

foreach(test_case
    PlanValidator_AcceptsValidPlan
    PlanValidator_RejectsMissingCostLimits
    PlanValidator_RejectsTtlBeyondConfig
    PlanValidator_RejectsMissingReservationWithConfig
    PlanValidator_RejectsDuplicateClientOrderId
    PlanValidator_RejectsInvalidChildOrder
)
    add_test(NAME ${test_case} COMMAND execution_plan_validator_tests ${test_case})
endforeach()

add_executable(execution_order_builder_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/execution/order_builder_test.cpp
)

target_compile_features(execution_order_builder_tests PRIVATE cxx_std_20)

target_link_libraries(execution_order_builder_tests PRIVATE
    engine_execution
)

foreach(test_case
    OrderBuilder_BuildsPlanFromApprovedIntent
    OrderBuilder_HashesNonNumericReservationIdDeterministically
    OrderBuilder_PreservesLegPricingFields
)
    add_test(NAME ${test_case} COMMAND execution_order_builder_tests ${test_case})
endforeach()

add_executable(execution_live_adapter_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/execution/live_execution_adapter_test.cpp
)

target_compile_features(execution_live_adapter_tests PRIVATE cxx_std_20)

target_link_libraries(execution_live_adapter_tests PRIVATE
    engine_execution
)

foreach(test_case
    LiveExecutionAdapter_DisabledByDefault
    LiveExecutionAdapter_SubmitReturnsDisabled
    LiveExecutionAdapter_DoesNotAccessNetwork
)
    add_test(NAME ${test_case} COMMAND execution_live_adapter_tests ${test_case})
endforeach()

add_executable(execution_paper_adapter_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/execution/paper_execution_adapter_test.cpp
)

target_compile_features(execution_paper_adapter_tests PRIVATE cxx_std_20)

target_link_libraries(execution_paper_adapter_tests PRIVATE
    engine_execution
)

foreach(test_case
    PaperExecutionAdapter_AtomicFillsAllLegsWhenDepthEnough
    PaperExecutionAdapter_AtomicFailsAllWhenOneLegInsufficient
    PaperExecutionAdapter_DoesNotPartialFillInAtomicMode
    PaperExecutionAdapter_SequentialAllowsPartialFillWhenConfigured
    PaperExecutionAdapter_RespectsLimitPrice
    PaperExecutionAdapter_RejectsMissingSnapshot
)
    add_test(NAME ${test_case} COMMAND execution_paper_adapter_tests ${test_case})
endforeach()

add_executable(execution_order_state_machine_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/execution/order_state_machine_test.cpp
)

target_compile_features(execution_order_state_machine_tests PRIVATE cxx_std_20)

target_link_libraries(execution_order_state_machine_tests PRIVATE
    engine_execution
)

foreach(test_case
    OrderStateMachine_RejectsInvalidTransition
    OrderStateMachine_AllowsCreatedToPlanned
    OrderStateMachine_AllowsAckedToFilled
    OrderStateMachine_TerminalStateCannotTransition
)
    add_test(NAME ${test_case} COMMAND execution_order_state_machine_tests ${test_case})
endforeach()

add_executable(execution_plan_state_machine_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/execution/plan_state_machine_test.cpp
)

target_compile_features(execution_plan_state_machine_tests PRIVATE cxx_std_20)

target_link_libraries(execution_plan_state_machine_tests PRIVATE
    engine_execution
)

foreach(test_case
    PlanStateMachine_RejectsInvalidTransition
    PlanStateMachine_AllowsCreatedToPlanned
    PlanStateMachine_AllowsAckedToFilled
    PlanStateMachine_PartialFillCanBecomeHedgeRequired
    PlanStateMachine_TerminalStateCannotTransition
)
    add_test(NAME ${test_case} COMMAND execution_plan_state_machine_tests ${test_case})
endforeach()

add_executable(execution_fill_tracker_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/execution/fill_tracker_test.cpp
)

target_compile_features(execution_fill_tracker_tests PRIVATE cxx_std_20)

target_link_libraries(execution_fill_tracker_tests PRIVATE
    engine_execution
)

foreach(test_case
    FillTracker_TracksFullFill
    FillTracker_TracksPartialFill
    FillTracker_ComputesAveragePrice
    FillTracker_DetectsAllFilled
    FillTracker_DetectsAnyPartial
)
    add_test(NAME ${test_case} COMMAND execution_fill_tracker_tests ${test_case})
endforeach()

add_executable(execution_partial_fill_policy_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/execution/partial_fill_policy_test.cpp
)

target_compile_features(execution_partial_fill_policy_tests PRIVATE cxx_std_20)

target_link_libraries(execution_partial_fill_policy_tests PRIVATE
    engine_execution
)

foreach(test_case
    PartialFillPolicy_MultiLegPartialRequiresHedge
    PartialFillPolicy_AllFilledNoAction
    PartialFillPolicy_NoFillNoAction
    PartialFillPolicy_SingleLegPartialCanCancelRemaining
)
    add_test(NAME ${test_case} COMMAND execution_partial_fill_policy_tests ${test_case})
endforeach()

add_executable(execution_cancel_manager_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/execution/cancel_manager_test.cpp
)

target_compile_features(execution_cancel_manager_tests PRIVATE cxx_std_20)

target_link_libraries(execution_cancel_manager_tests PRIVATE
    engine_execution
)

foreach(test_case
    CancelManager_CancelsOpenPaperOrders
    CancelManager_DoesNotCancelFilledOrders
    CancelManager_LiveDisabledReturnsUnavailable
)
    add_test(NAME ${test_case} COMMAND execution_cancel_manager_tests ${test_case})
endforeach()

add_executable(execution_report_publisher_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/execution/execution_report_publisher_test.cpp
)

target_compile_features(execution_report_publisher_tests PRIVATE cxx_std_20)

target_link_libraries(execution_report_publisher_tests PRIVATE
    engine_execution
)

foreach(test_case
    CapturingExecutionPublisher_CapturesReport
    CapturingExecutionPublisher_PreservesOrder
    JsonlExecutionReportWriter_WritesValidJsonLine
    JsonlExecutionReportWriter_EscapesStrings
    ReservationDispositionPublisher_CapturesDisposition
)
    add_test(NAME ${test_case} COMMAND execution_report_publisher_tests ${test_case})
endforeach()

add_executable(execution_metrics_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/execution/execution_metrics_test.cpp
)

target_compile_features(execution_metrics_tests PRIVATE cxx_std_20)

target_link_libraries(execution_metrics_tests PRIVATE
    engine_execution
)

foreach(test_case
    ExecutionMetrics_DefaultCountersZero
    ExecutionMetrics_HasPlanCounters
    ExecutionMetrics_HasChildCounters
    ExecutionMetrics_HasReportAndReservationCounters
    ExecutionMetrics_ObserveSubmitLatency
    ExecutionMetrics_ObserveFillSimulationLatency
)
    add_test(NAME ${test_case} COMMAND execution_metrics_tests ${test_case})
endforeach()

add_executable(execution_gateway_workflow_tests
    ${CMAKE_SOURCE_DIR}/tests/integration/execution/execution_gateway_workflow_test.cpp
)

target_compile_features(execution_gateway_workflow_tests PRIVATE cxx_std_20)

target_link_libraries(execution_gateway_workflow_tests PRIVATE
    engine_execution
)

foreach(test_case
    ExecutionGateway_FillsPaperAtomicPlan
    ExecutionGateway_FailsWhenOneLegInsufficient
    ExecutionGateway_GeneratesReleaseDispositionOnFailure
    ExecutionGateway_GeneratesConsumeDispositionOnFill
    ExecutionGateway_MultiLegPartialGoesHedgeRequiredInSequentialMode
    ExecutionGateway_DeterministicReports
)
    add_test(NAME ${test_case} COMMAND execution_gateway_workflow_tests ${test_case})
endforeach()

add_executable(verify_execution_workflow_tests
    ${CMAKE_SOURCE_DIR}/tests/integration/execution/verify_execution_workflow_test.cpp
)

target_compile_features(verify_execution_workflow_tests PRIVATE cxx_std_20)

target_link_libraries(verify_execution_workflow_tests PRIVATE
    engine_execution
)

target_compile_definitions(verify_execution_workflow_tests PRIVATE
    POLYTOPE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    VERIFY_EXECUTION_WORKFLOW_BIN="$<TARGET_FILE:verify_execution_workflow>"
)

add_dependencies(verify_execution_workflow_tests verify_execution_workflow)

foreach(test_case
    VerifyExecutionWorkflow_PositiveFillsPlan
    VerifyExecutionWorkflow_InsufficientDepthFailsAndReleases
    VerifyExecutionWorkflow_ExpiredIntentDoesNotSubmit
    VerifyExecutionWorkflow_DuplicateIdempotencyRejected
    VerifyExecutionWorkflow_PartialSequentialRequiresHedge
    VerifyExecutionWorkflow_LiveAdapterDisabledDoesNotFill
    VerifyExecutionWorkflow_DeterministicHash
    VerifyExecutionWorkflow_EmitsSummary
)
    add_test(NAME ${test_case} COMMAND verify_execution_workflow_tests ${test_case})
endforeach()
