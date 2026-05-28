add_library(engine_signal STATIC
    BundleScanner.cpp
    EdgeCalculator.cpp
    OpportunityRanker.cpp
    SignalEngine.cpp
    core/SignalEngine.cpp
    core/SignalWorkflow.cpp
    edge/LatencyBufferModel.cpp
    edge/TheoreticalEdgeCalculator.cpp
    pricing/FeeModel.cpp
    pricing/PriceVectorBuilder.cpp
    pricing/VWAPPrecheck.cpp
    publish/CapturingIntentPublisher.cpp
    publish/JsonlIntentWriter.cpp
    publish/PaperIntentPublisher.cpp
    rank/OpportunityRanker.cpp
    reader/FixtureMarketSnapshotReader.cpp
    reader/MarketSnapshotReader.cpp
    reader/MarketStateViewSnapshotReader.cpp
    reader/OracleArtifactReader.cpp
)

target_compile_features(engine_signal PUBLIC cxx_std_20)

target_include_directories(engine_signal PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(engine_signal PUBLIC
    engine_state
    oracle_artifact
)

add_library(signal_tools STATIC
    tools/SignalTools.cpp
)

target_compile_features(signal_tools PUBLIC cxx_std_20)

target_include_directories(signal_tools PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(signal_tools PUBLIC
    engine_signal
)

add_executable(verify_signal_workflow
    tools/verify_signal_workflow.cpp
)

target_compile_features(verify_signal_workflow PRIVATE cxx_std_20)

target_include_directories(verify_signal_workflow PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(verify_signal_workflow PRIVATE
    engine_signal
)

set_target_properties(verify_signal_workflow PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
