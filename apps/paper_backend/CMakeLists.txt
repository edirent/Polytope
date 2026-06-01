add_library(paper_backend_lib STATIC
    DashboardApiRoutes.cpp
    ReadOnlyHttpServer.cpp
    SsePublisher.cpp
)

target_compile_features(paper_backend_lib PUBLIC cxx_std_20)

target_include_directories(paper_backend_lib PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
    ${CMAKE_SOURCE_DIR}/apps
)

target_link_libraries(paper_backend_lib PUBLIC
    engine_paper
)

add_executable(paper_backend
    main.cpp
)

target_compile_features(paper_backend PRIVATE cxx_std_20)

target_link_libraries(paper_backend PRIVATE
    paper_backend_lib
    Boost::system
    Threads::Threads
)

set_target_properties(paper_backend PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_executable(paper_backend_tests
    ${CMAKE_SOURCE_DIR}/tests/unit/paper_backend/read_only_backend_test.cpp
)

target_compile_features(paper_backend_tests PRIVATE cxx_std_20)

target_link_libraries(paper_backend_tests PRIVATE
    paper_backend_lib
)

foreach(test_case
    ReadOnlyApi_AllowsGet
    ReadOnlyApi_RejectsPost
    ReadOnlyApi_RejectsPut
    ReadOnlyApi_RejectsDelete
    Sse_DoesNotBlockRuntime
)
    add_test(NAME ${test_case} COMMAND paper_backend_tests ${test_case})
endforeach()
