cmake_minimum_required(VERSION 3.20)

project(SmartNICControlFabric
        VERSION 1.0.0
        DESCRIPTION "Vendor-neutral control-plane runtime for programmable SmartNIC functions"
        HOMEPAGE_URL "https://github.com/summonlabs/SmartNIC-Control-Fabric"
        LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(SNCF_TOPLEVEL ON)
else()
  set(SNCF_TOPLEVEL OFF)
endif()
message(STATUS "SmartNIC Control Fabric: top-level build = ${SNCF_TOPLEVEL}")

option(SNCF_BUILD_TESTS "Build the SmartNIC Control Fabric test suite" ${SNCF_TOPLEVEL})
option(SNCF_BUILD_TOOLS "Build the sncf command line tools" ${SNCF_TOPLEVEL})
option(SNCF_STRICT_WARNINGS "Apply the project strict warning set to first-party targets" ON)
option(SNCF_ENABLE_ASAN "Build first-party targets with AddressSanitizer" OFF)

if(SNCF_BUILD_TESTS)
  enable_testing()
endif()

find_package(Threads REQUIRED)

# Keep archives and executables in separate directories: the tool executable is
# called sncf and must never be able to overwrite the sncf archive.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- strict warnings (first-party targets only; never exported) -------------
function(sncf_apply_strict_warnings target)
  if(NOT SNCF_STRICT_WARNINGS)
    return()
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion
      -Wold-style-cast -Wcast-qual -Wdouble-promotion -Wformat=2 -Wundef)
  endif()
endfunction()

function(sncf_apply_asan target)
  if(NOT SNCF_ENABLE_ASAN)
    return()
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /fsanitize=address)
    target_link_options(${target} PRIVATE /fsanitize=address)
  else()
    target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endif()
endfunction()

# --- library ---------------------------------------------------------------
add_library(sncf
  src/authority.cpp
  src/canonical.cpp
  src/crash.cpp
  src/digest.cpp
  src/evidence.cpp
  src/explain.cpp
  src/fabric.cpp
  src/fabric_ops.cpp
  src/ids.cpp
  src/lifecycle.cpp
  src/model.cpp
  src/model_codec.cpp
  src/persistence.cpp
  src/protocol.cpp
  src/service.cpp
  src/status.cpp
  src/time.cpp
  src/transport.cpp
  src/version.cpp)
add_library(sncf::sncf ALIAS sncf)

target_include_directories(sncf
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(sncf PUBLIC cxx_std_20)
set_target_properties(sncf PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

target_link_libraries(sncf PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(sncf PRIVATE ws2_32)
endif()

sncf_apply_strict_warnings(sncf)
sncf_apply_asan(sncf)

# --- command line tools ----------------------------------------------------
if(SNCF_BUILD_TOOLS)
  add_executable(sncf_cli tools/sncf_cli.cpp)
  # The tool is named sncf, so its (empty) import library must not be allowed to
  # land on top of the sncf archive.
  set_target_properties(sncf_cli PROPERTIES
    OUTPUT_NAME sncf
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  target_link_libraries(sncf_cli PRIVATE sncf::sncf)
  sncf_apply_strict_warnings(sncf_cli)
  sncf_apply_asan(sncf_cli)
endif()

# --- tests -----------------------------------------------------------------
if(SNCF_BUILD_TESTS)
  add_executable(sncf_tests
    tests/test_main.cpp
    tests/test_canonical.cpp
    tests/test_digest.cpp
    tests/test_lifecycle.cpp
    tests/test_persistence.cpp
    tests/test_fabric_lifecycle.cpp
    tests/test_fabric_authority.cpp
    tests/test_fabric_evidence.cpp
    tests/test_property.cpp
    tests/test_adversarial.cpp
    tests/test_restart.cpp
    tests/test_concurrency.cpp
    tests/test_transport.cpp
    tests/test_explain_export.cpp
    tests/test_benchmark.cpp
    tests/test_hardening.cpp)
  target_link_libraries(sncf_tests PRIVATE sncf::sncf Threads::Threads)
  target_include_directories(sncf_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  sncf_apply_strict_warnings(sncf_tests)
  sncf_apply_asan(sncf_tests)
  if(WIN32)
    target_link_libraries(sncf_tests PRIVATE ws2_32)
  endif()

  # The multi-process transport proof spawns the real sncf tool as a separate
  # operating-system process; make sure it exists before the suite runs.
  if(SNCF_BUILD_TOOLS)
    add_dependencies(sncf_tests sncf_cli)
  endif()

  # No timeout is configured: every test synchronises on completed work, so a
  # hang is a defect rather than a skipped test.
  add_test(NAME sncf_tests COMMAND sncf_tests)
  set_tests_properties(sncf_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

# --- installation ----------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS sncf
  EXPORT sncfTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(SNCF_BUILD_TOOLS)
  install(TARGETS sncf_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

set(SNCF_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/sncf)

install(EXPORT sncfTargets
  FILE sncfTargets.cmake
  NAMESPACE sncf::
  DESTINATION ${SNCF_CMAKE_INSTALL_DIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sncfConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/sncfConfig.cmake
  INSTALL_DESTINATION ${SNCF_CMAKE_INSTALL_DIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/sncfConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/sncfConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/sncfConfigVersion.cmake
  DESTINATION ${SNCF_CMAKE_INSTALL_DIR})
