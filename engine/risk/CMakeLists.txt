add_library(engine_risk STATIC
    ExposureManager.cpp
    FeeModel.cpp
    KillSwitch.cpp
    RiskEngine.cpp
    VWAPSimulator.cpp
    core/RiskEngine.cpp
    core/RiskPipeline.cpp
    core/RiskWorkflow.cpp
    guards/DuplicateIntentGuard.cpp
    guards/EdgeGuard.cpp
    guards/ExposureGuard.cpp
    guards/IRiskGuard.cpp
    guards/InventoryGuard.cpp
    guards/IntentExpiryGuard.cpp
    guards/KillSwitchGuard.cpp
    guards/MarketStateGuard.cpp
    guards/MaxLossGuard.cpp
    guards/PartialFillGuard.cpp
    guards/RateLimitGuard.cpp
    guards/SnapshotFreshnessGuard.cpp
    ledger/ExposureLedger.cpp
    ledger/InventoryLedger.cpp
    ledger/ReservationBook.cpp
    ledger/RiskLedger.cpp
    metrics/RiskMetrics.cpp
    publish/ApprovedIntentPublisher.cpp
    publish/CapturingRiskPublisher.cpp
    publish/HashOnlyRiskPublisher.cpp
    publish/JsonlRiskDecisionWriter.cpp
    publish/RiskDecisionPublisher.cpp
    reprice/CostRevalidator.cpp
    reprice/FeeRevalidator.cpp
    reprice/LatencyRevalidator.cpp
    reprice/SlippageRevalidator.cpp
    reprice/VWAPRevalidator.cpp
    validate/IntentEvidenceVerifier.cpp
    validate/IntentValidator.cpp
)

target_compile_features(engine_risk PUBLIC cxx_std_20)

target_include_directories(engine_risk PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(engine_risk PUBLIC
    engine_signal
)

add_executable(verify_risk_workflow
    tools/verify_risk_workflow.cpp
)

target_compile_features(verify_risk_workflow PRIVATE cxx_std_20)

target_include_directories(verify_risk_workflow PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(verify_risk_workflow PRIVATE
    engine_risk
)

set_target_properties(verify_risk_workflow PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
