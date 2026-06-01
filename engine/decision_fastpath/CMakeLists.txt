add_library(engine_decision_fastpath STATIC
    core/DifferentialVerifier.cpp
    core/EventLocalDecisionPipeline.cpp
    core/FastPathGate.cpp
    kernel/FixedBuyBundleKernelScalar.cpp
    kernel/FixedShapeKernelSpec.cpp
)

target_compile_features(engine_decision_fastpath PUBLIC cxx_std_20)

target_include_directories(engine_decision_fastpath PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(engine_decision_fastpath PUBLIC
    engine_execution
)
