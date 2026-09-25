# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Site Fabric build.
#
# The project is standalone: it requires no other Summon Software Labs
# repository to build its core, and it has no third-party dependencies.

cmake_minimum_required(VERSION 3.20)

project(
  SiteFabric
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral aggregate network model and authority runtime for one physical site"
  HOMEPAGE_URL "https://github.com/summonlabs/Site-Fabric"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(SITE_FABRIC_IS_TOP_LEVEL ON)
else()
  set(SITE_FABRIC_IS_TOP_LEVEL OFF)
endif()

option(SITE_FABRIC_BUILD_LIBRARY "Build the site_fabric library" ON)
option(SITE_FABRIC_BUILD_TOOLS "Build the controller, member and inspection tools" ON)
option(SITE_FABRIC_BUILD_TESTS "Build the Site Fabric test suites" ON)
option(SITE_FABRIC_BUILD_EXAMPLES "Build the examples" ON)
option(SITE_FABRIC_BUILD_BENCHMARKS "Build the benchmarks" ON)
option(SITE_FABRIC_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(SITE_FABRIC_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Declared after GNUInstallDirs: CMAKE_INSTALL_LIBDIR is only defined once that
# module has run, and an empty component would turn the destination into an
# absolute path outside the install prefix.
set(SITE_FABRIC_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/site_fabric"
    CACHE STRING "Directory for the installed CMake package configuration")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Warning configuration: strict for first-party code only.
# ---------------------------------------------------------------------------

add_library(site_fabric_warnings INTERFACE)
if(MSVC)
  target_compile_options(site_fabric_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  if(SITE_FABRIC_WARNINGS_AS_ERRORS)
    target_compile_options(site_fabric_warnings INTERFACE /WX)
  endif()
  set(SITE_FABRIC_ASAN_AVAILABLE OFF)
  if(SITE_FABRIC_ENABLE_ASAN)
    target_compile_options(site_fabric_warnings INTERFACE /fsanitize=address)
    target_link_options(site_fabric_warnings INTERFACE /fsanitize=address)
    set(SITE_FABRIC_ASAN_AVAILABLE ON)
  endif()
  target_compile_definitions(site_fabric_warnings INTERFACE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX)
else()
  target_compile_options(site_fabric_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor)
  if(SITE_FABRIC_WARNINGS_AS_ERRORS)
    target_compile_options(site_fabric_warnings INTERFACE -Werror)
  endif()
  set(SITE_FABRIC_ASAN_AVAILABLE OFF)
  if(SITE_FABRIC_ENABLE_ASAN)
    target_compile_options(site_fabric_warnings INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(site_fabric_warnings INTERFACE -fsanitize=address)
    set(SITE_FABRIC_ASAN_AVAILABLE ON)
  endif()
endif()

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------

set(SITE_FABRIC_SOURCES
  src/core/canonical.cpp
  src/core/codec.cpp
  src/core/digest.cpp
  src/core/model.cpp
  src/core/snapshot.cpp
  src/core/evidence.cpp
  src/core/capacity.cpp
  src/compose/validate.cpp
  src/compose/compose.cpp
  src/authority/authority.cpp
  src/persistence/persistence.cpp
  src/protocol/protocol.cpp
  src/transport/tcp.cpp
  src/controller/controller.cpp
  src/publisher/publisher.cpp
  src/synthetic/synthetic.cpp
  src/internal/crypto.cpp
  src/internal/files.cpp
  src/internal/log.cpp)

add_library(site_fabric STATIC ${SITE_FABRIC_SOURCES})
add_library(site_fabric::site_fabric ALIAS site_fabric)

target_include_directories(site_fabric
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

# BUILD_INTERFACE keeps the warning interface out of the installed export set:
# consumers of the package never inherit first-party build flags.
target_link_libraries(site_fabric PRIVATE $<BUILD_INTERFACE:site_fabric_warnings>)
target_compile_features(site_fabric PUBLIC cxx_std_20)

set_target_properties(site_fabric PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  EXPORT_NAME site_fabric)

if(WIN32)
  target_link_libraries(site_fabric PRIVATE ws2_32 iphlpapi advapi32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(site_fabric PRIVATE Threads::Threads)
endif()

target_compile_definitions(site_fabric PRIVATE SITE_FABRIC_BUILDING_LIBRARY=1)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

if(SITE_FABRIC_BUILD_TOOLS)
  add_executable(site_fabric_controller tools/controller_main.cpp)
  target_link_libraries(site_fabric_controller PRIVATE site_fabric site_fabric_warnings)

  add_executable(site_fabric_member tools/member_main.cpp)
  target_link_libraries(site_fabric_member PRIVATE site_fabric site_fabric_warnings)

  add_executable(site_fabric_inspect tools/inspect_main.cpp)
  target_link_libraries(site_fabric_inspect PRIVATE site_fabric site_fabric_warnings)

  install(TARGETS site_fabric_controller site_fabric_member site_fabric_inspect
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------------------
# Tests, examples, benchmarks
# ---------------------------------------------------------------------------

if(SITE_FABRIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SITE_FABRIC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(SITE_FABRIC_BUILD_BENCHMARKS)
  add_executable(site_fabric_benchmarks benchmarks/benchmarks.cpp)
  target_link_libraries(site_fabric_benchmarks PRIVATE site_fabric site_fabric_warnings)
endif()

# ---------------------------------------------------------------------------
# Install and package export
# ---------------------------------------------------------------------------

install(TARGETS site_fabric
        EXPORT site_fabricTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT site_fabricTargets
        FILE site_fabricTargets.cmake
        NAMESPACE site_fabric::
        DESTINATION ${SITE_FABRIC_INSTALL_CMAKEDIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/site_fabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/site_fabricConfig.cmake
  INSTALL_DESTINATION ${SITE_FABRIC_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/site_fabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/site_fabricConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/site_fabricConfigVersion.cmake
        DESTINATION ${SITE_FABRIC_INSTALL_CMAKEDIR})

install(FILES LICENSE README.md NOTICE DESTINATION ${CMAKE_INSTALL_DOCDIR})
