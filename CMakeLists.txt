cmake_minimum_required(VERSION 3.21)

project(protoIO
    VERSION 0.2.2
    DESCRIPTION "Shared input and output (files, processes, TCP, UDP, TLS, HTTP) for the protoCore runtimes"
    HOMEPAGE_URL "https://github.com/gamarino/protoIO"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Top-level builds get tests and install rules; a runtime that pulls protoIO
# in with add_subdirectory() gets only the library.
option(PROTOIO_BUILD_TESTS "Build the GoogleTest suite" ${PROJECT_IS_TOP_LEVEL})
option(PROTOIO_INSTALL "Generate install and packaging rules" ${PROJECT_IS_TOP_LEVEL})
option(PROTOIO_TSAN "Build everything with ThreadSanitizer" OFF)
# CI turns this on, so the library stays free of warnings (/W4 with MSVC,
# -Wall -Wextra -Wpedantic elsewhere). Off by default: a newer compiler's new
# warning must not break a runtime's build.
option(PROTOIO_WARNINGS_AS_ERRORS "Treat the library's compiler warnings as errors" OFF)

# The static library is linked into position-independent executables (the
# default on Debian and Ubuntu), so its code must be position independent.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(PROTOIO_TSAN)
    add_compile_options(-fsanitize=thread -g -O1 -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

# Debian multiarch library directory (lib/x86_64-linux-gnu) whatever the
# prefix, so the package installs where the distribution expects it and
# find_package() searches it.
if(NOT DEFINED CMAKE_INSTALL_LIBDIR AND CMAKE_LIBRARY_ARCHITECTURE)
    set(CMAKE_INSTALL_LIBDIR "lib/${CMAKE_LIBRARY_ARCHITECTURE}" CACHE PATH "Object code libraries")
endif()
include(GNUInstallDirs)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

if(WIN32)
    # Files and processes have a Windows implementation of their own; the
    # rest share their source, with the differences in #ifdef _WIN32 blocks
    # and src/platform_win.cpp.
    set(PROTOIO_PLATFORM_SOURCES src/file_win.cpp src/process_win.cpp src/platform_win.cpp)
else()
    set(PROTOIO_PLATFORM_SOURCES src/file.cpp src/process.cpp)
endif()

add_library(protoio STATIC
    src/error.cpp
    src/stream.cpp
    src/net.cpp
    src/http.cpp
    ${PROTOIO_PLATFORM_SOURCES}
)
add_library(protoIO::protoio ALIAS protoio)
set_target_properties(protoio PROPERTIES EXPORT_NAME protoio OUTPUT_NAME protoio)
target_include_directories(protoio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(protoio PUBLIC cxx_std_20)
target_compile_definitions(protoio PRIVATE PROTOIO_VERSION="${PROJECT_VERSION}")
if(MSVC)
    # /utf-8: the sources are UTF-8. The CRT's "use the _s variant" and POSIX
    # name warnings do not apply to portable code.
    target_compile_options(protoio PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(protoio PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
                                               WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    target_compile_options(protoio PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(PROTOIO_WARNINGS_AS_ERRORS)
    target_compile_options(protoio PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/WX,-Werror>)
endif()
# PUBLIC: consumers link the static archive, so they need its dependencies.
target_link_libraries(protoio PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
if(WIN32)
    # crypt32: the system certificate stores (net.cpp).
    target_link_libraries(protoio PUBLIC ws2_32 crypt32)
endif()

# ---------------------------------------------------------------- package config
#
# Two ways to consume protoIO with find_package(protoIO 0.2 CONFIG):
#   * installed: <prefix>/lib/<arch>/cmake/protoIO/
#   * from a build tree, without installing: -DprotoIO_DIR=<protoIO>/build_release
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/protoIOConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)
configure_file(cmake/protoIOConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/protoIOConfig.cmake @ONLY)
export(TARGETS protoio NAMESPACE protoIO:: FILE ${CMAKE_CURRENT_BINARY_DIR}/protoIOTargets.cmake)

if(PROTOIO_INSTALL)
    set(PROTOIO_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/protoIO)
    install(TARGETS protoio EXPORT protoIOTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY include/protoio DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT protoIOTargets NAMESPACE protoIO:: DESTINATION ${PROTOIO_CMAKE_DIR})
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/protoIOConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/protoIOConfigVersion.cmake
        DESTINATION ${PROTOIO_CMAKE_DIR})
    install(FILES LICENSE README.md CHANGELOG.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

    # cpack -G DEB  ->  protoio-dev_<version>_<arch>.deb
    set(CPACK_PACKAGE_NAME "protoio-dev")
    set(CPACK_PACKAGE_VENDOR "Gustavo Marino")
    set(CPACK_PACKAGE_CONTACT "Gustavo Marino <gamarino@gmail.com>")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION} (static library and headers)")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
    if(WIN32)
        set(CPACK_GENERATOR "ZIP")  # cpack  ->  protoio-dev-<version>-win64.zip
    else()
        set(CPACK_GENERATOR "DEB")
    endif()
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Gustavo Marino <gamarino@gmail.com>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "libdevel")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libssl-dev")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE ${PROJECT_HOMEPAGE_URL})
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    include(CPack)
endif()

# ------------------------------------------------------------------------- tests
if(PROTOIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
