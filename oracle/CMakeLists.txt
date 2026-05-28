option(ORACLE_ENABLE_LLM "Enable live LLM rule extraction" OFF)

set(ORACLE_PLACEHOLDER_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/OracleModule.cpp
)

add_library(oracle_core STATIC
    OracleJson.cpp
    ${ORACLE_PLACEHOLDER_SOURCE}
)
target_compile_features(oracle_core PUBLIC cxx_std_20)
target_include_directories(oracle_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(ORACLE_ENABLE_LLM)
    target_compile_definitions(oracle_core PUBLIC ORACLE_ENABLE_LLM=1)
else()
    target_compile_definitions(oracle_core PUBLIC ORACLE_ENABLE_LLM=0)
endif()

foreach(component
    ingestion
    rules
    llm
    compiler
    enumerate
    payoff
    bundles
    artifact
    tools
)
    add_library(oracle_${component} STATIC
        ${ORACLE_PLACEHOLDER_SOURCE}
    )
    target_link_libraries(oracle_${component} PUBLIC oracle_core)
endforeach()

target_sources(oracle_ingestion PRIVATE
    ingestion/MarketApiClient.cpp
    ingestion/MarketDescriptionLoader.cpp
    ingestion/MarketUniverseBuilder.cpp
)

target_sources(oracle_rules PRIVATE
    rules/ManualRuleEditor.cpp
    rules/Rulebook.cpp
    rules/RuleValidator.cpp
)

target_sources(oracle_llm PRIVATE
    llm/ClaudeRuleExtractor.cpp
    llm/StubRuleExtractor.cpp
)

target_link_libraries(oracle_llm PUBLIC
    oracle_rules
)

target_sources(oracle_compiler PRIVATE
    compiler/BooleanConstraintBuilder.cpp
    compiler/ConstraintCompiler.cpp
    compiler/MatrixBuilder.cpp
)

target_link_libraries(oracle_compiler PUBLIC
    oracle_rules
)

target_sources(oracle_enumerate PRIVATE
    enumerate/FeasibilityChecker.cpp
    enumerate/StateEnumerator.cpp
)

target_link_libraries(oracle_enumerate PUBLIC
    oracle_compiler
)

target_sources(oracle_payoff PRIVATE
    payoff/PayoffMatrixBuilder.cpp
)

target_link_libraries(oracle_payoff PUBLIC
    oracle_enumerate
)

target_sources(oracle_bundles PRIVATE
    bundles/BundleHash.cpp
    bundles/BundleValidator.cpp
    bundles/CandidateBundle.cpp
    bundles/CandidateBundleGenerator.cpp
)

target_link_libraries(oracle_bundles PUBLIC
    oracle_payoff
)

target_sources(oracle_artifact PRIVATE
    artifact/ArtifactChecksum.cpp
    artifact/ArtifactExporter.cpp
    artifact/ArtifactLoader.cpp
)

target_link_libraries(oracle_artifact PUBLIC
    oracle_bundles
)

add_executable(fetch_market_universe
    tools/fetch_market_universe.cpp
)

target_link_libraries(fetch_market_universe PRIVATE
    oracle_ingestion
)

set_target_properties(fetch_market_universe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_executable(verify_oracle_workflow
    tools/verify_oracle_workflow.cpp
)

target_link_libraries(verify_oracle_workflow PRIVATE
    oracle_artifact
    oracle_ingestion
)

set_target_properties(verify_oracle_workflow PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
