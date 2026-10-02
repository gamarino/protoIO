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
if(MSVC)
    # The tests and protoIO use the DLL C runtime (/MD); so must GoogleTest.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
endif()
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

if(WIN32)
    # Windows lacks the POSIX commands the process tests run (sh, cat, ...):
    # a small program stands in for them.
    add_executable(protoio_testchild testchild.cpp)
    target_link_libraries(protoio_testchild PRIVATE shell32)
    if(MSVC)
        target_compile_options(protoio_testchild PRIVATE /utf-8)
        target_compile_definitions(protoio_testchild PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    # The OpenSSL DLLs go next to the test programs, so they run (and are
    # listed right after the build) without OpenSSL's bin directory on PATH.
    get_filename_component(PROTOIO_OPENSSL_BIN "${OPENSSL_INCLUDE_DIR}/../bin" ABSOLUTE)
    file(GLOB PROTOIO_OPENSSL_DLLS "${PROTOIO_OPENSSL_BIN}/libcrypto*.dll" "${PROTOIO_OPENSSL_BIN}/libssl*.dll")
    # Copied once, by a step every test program depends on (copies from
    # parallel post-build steps would race for the same files).
    add_custom_target(protoio_test_dlls
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROTOIO_OPENSSL_DLLS} $<TARGET_FILE_DIR:protoio_testchild>
        VERBATIM)
endif()

# One binary per module, so the ThreadSanitizer run can pick the stream, net
# and process suites by label.
function(protoio_test name label)
    add_executable(${name} ${name}.cpp test_support.cpp)
    target_link_libraries(${name} PRIVATE protoIO::protoio GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${name} PRIVATE PROTOIO_TEST_VERSION="${PROJECT_VERSION}")
    if(MSVC)
        target_compile_options(${name} PRIVATE /utf-8)
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
                                                   PROTOIO_TESTCHILD="$<TARGET_FILE:protoio_testchild>")
    endif()
    if(WIN32)
        add_dependencies(${name} protoio_testchild protoio_test_dlls)
    endif()
    gtest_discover_tests(${name} DISCOVERY_MODE ${PROTOIO_DISCOVERY_MODE} PROPERTIES LABELS ${label} TIMEOUT 60 DISCOVERY_TIMEOUT 30)
endfunction()

protoio_test(test_stream  stream)
protoio_test(test_file    file)
protoio_test(test_process process)
protoio_test(test_net     net)
protoio_test(test_http    http)
