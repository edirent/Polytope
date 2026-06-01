# CMake generated Testfile for 
# Source directory: /home/edirent/Polytope/apps/paper_backend
# Build directory: /home/edirent/Polytope/build-release-native/apps/paper_backend
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[ReadOnlyApi_AllowsGet]=] "/home/edirent/Polytope/build-release-native/apps/paper_backend/paper_backend_tests" "ReadOnlyApi_AllowsGet")
set_tests_properties([=[ReadOnlyApi_AllowsGet]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/edirent/Polytope/apps/paper_backend/CMakeLists.txt;52;add_test;/home/edirent/Polytope/apps/paper_backend/CMakeLists.txt;0;")
add_test([=[ReadOnlyApi_RejectsPost]=] "/home/edirent/Polytope/build-release-native/apps/paper_backend/paper_backend_tests" "ReadOnlyApi_RejectsPost")
set_tests_properties([=[ReadOnlyApi_RejectsPost]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/edirent/Polytope/apps/paper_backend/CMakeLists.txt;52;add_test;/home/edirent/Polytope/apps/paper_backend/CMakeLists.txt;0;")
add_test([=[ReadOnlyApi_RejectsPut]=] "/home/edirent/Polytope/build-release-native/apps/paper_backend/paper_backend_tests" "ReadOnlyApi_RejectsPut")
set_tests_properties([=[ReadOnlyApi_RejectsPut]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/edirent/Polytope/apps/paper_backend/CMakeLists.txt;52;add_test;/home/edirent/Polytope/apps/paper_backend/CMakeLists.txt;0;")
add_test([=[ReadOnlyApi_RejectsDelete]=] "/home/edirent/Polytope/build-release-native/apps/paper_backend/paper_backend_tests" "ReadOnlyApi_RejectsDelete")
set_tests_properties([=[ReadOnlyApi_RejectsDelete]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/edirent/Polytope/apps/paper_backend/CMakeLists.txt;52;add_test;/home/edirent/Polytope/apps/paper_backend/CMakeLists.txt;0;")
add_test([=[Sse_DoesNotBlockRuntime]=] "/home/edirent/Polytope/build-release-native/apps/paper_backend/paper_backend_tests" "Sse_DoesNotBlockRuntime")
set_tests_properties([=[Sse_DoesNotBlockRuntime]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/edirent/Polytope/apps/paper_backend/CMakeLists.txt;52;add_test;/home/edirent/Polytope/apps/paper_backend/CMakeLists.txt;0;")
