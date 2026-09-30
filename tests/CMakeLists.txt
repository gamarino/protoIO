include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# Keep GoogleTest out of the install and the package.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# A ThreadSanitizer binary needs ASLR off on recent kernels (`setarch -R`),
# so under TSan the tests are listed when ctest runs, inside that wrapper,
# rather than right after the build.
if(PROTOIO_TSAN)
    set(PROTOIO_DISCOVERY_MODE PRE_TEST)
else()
    set(PROTOIO_DISCOVERY_MODE POST_BUILD)
endif()

# One binary per module, so the ThreadSanitizer run can pick the stream, net
# and process suites by label.
function(protoio_test name label)
    add_executable(${name} ${name}.cpp test_support.cpp)
    target_link_libraries(${name} PRIVATE protoIO::protoio GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    gtest_discover_tests(${name} DISCOVERY_MODE ${PROTOIO_DISCOVERY_MODE} PROPERTIES LABELS ${label} TIMEOUT 60 DISCOVERY_TIMEOUT 30)
endfunction()

protoio_test(test_stream  stream)
protoio_test(test_file    file)
protoio_test(test_process process)
protoio_test(test_net     net)
protoio_test(test_http    http)
