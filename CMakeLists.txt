# Copyright (C) 2026 Honu Robotics
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.18)
project(EncinoWaves VERSION 0.0.1 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif()


# ---- optional sanitizer build (-DSANITIZE=address|thread|undefined) ----
# Applied to every target (library, tests, benchmark). `address` also turns on
# UBSan and LeakSanitizer; `-fno-sanitize-recover=all` makes UBSan findings
# fatal so CI fails on them instead of merely logging. Used by .github/workflows
# to produce the ASan and TSan jobs; reproduce a job locally with e.g.
#   cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSANITIZE=address
set(SANITIZE "" CACHE STRING
    "Build with a sanitizer: address | thread | undefined (empty = none)")
set_property(CACHE SANITIZE PROPERTY STRINGS "" address thread undefined)
if(SANITIZE)
  # float-cast-overflow is spelled out because GCC's -fsanitize=undefined
  # does not include it (Clang's does); it catches out-of-range
  # float-to-integer casts like the historical SeedFromWavenumber bug.
  if(SANITIZE STREQUAL "address")
    set(_san_flags -fsanitize=address,undefined,float-cast-overflow
        -fno-sanitize-recover=all)
  elseif(SANITIZE STREQUAL "thread")
    set(_san_flags -fsanitize=thread)
  elseif(SANITIZE STREQUAL "undefined")
    set(_san_flags -fsanitize=undefined,float-cast-overflow
        -fno-sanitize-recover=all)
  else()
    message(FATAL_ERROR
            "SANITIZE='${SANITIZE}' is not one of: address, thread, undefined")
  endif()
  message(STATUS "Building with sanitizer: ${SANITIZE}")
  add_compile_options(${_san_flags} -fno-omit-frame-pointer -g)
  add_link_options(${_san_flags})
endif()

# Single source of truth for dependency version floors; reused on the consumer
# side via EncinoWavesConfig.cmake.in so the two never drift.
set(EW_EIGEN3_MIN 3.4)
set(EW_TBB_MIN    2021)
set(EW_IMATH_MIN  3.1)

find_package(Eigen3 ${EW_EIGEN3_MIN} REQUIRED NO_MODULE)  # header-only; Eigen::FFT
find_package(TBB    ${EW_TBB_MIN}    REQUIRED)            # parallel sweeps
find_package(Imath  ${EW_IMATH_MIN}  REQUIRED)            # BSD-3 math types

add_library(${PROJECT_NAME} SHARED
  src/Foundation.cpp
  src/util/Exception.cpp)
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

set_target_properties(${PROJECT_NAME} PROPERTIES
  VERSION     ${PROJECT_VERSION}
  SOVERSION   ${PROJECT_VERSION_MAJOR}
  EXPORT_NAME ${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(${PROJECT_NAME} PUBLIC Eigen3::Eigen TBB::tbb Imath::Imath)

# ---- install: headers, target, and the CMake package config ----
# COMPONENT tags split the install into the `runtime` (shared object) and `dev`
# (headers, .so symlink, CMake config) sets that cmake/Packaging.cmake turns into
# the libencinowaves0 / libencinowaves-dev .deb packages. A plain `cmake --install`
# is unaffected; it still installs every component.
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev
  DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE)

install(TARGETS ${PROJECT_NAME}
  EXPORT   ${PROJECT_NAME}Targets
  LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
           COMPONENT runtime
           NAMELINK_COMPONENT dev
  ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev
  RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(_cfgdir ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME})
install(EXPORT ${PROJECT_NAME}Targets
  FILE ${PROJECT_NAME}Targets.cmake
  NAMESPACE ${PROJECT_NAME}::
  DESTINATION ${_cfgdir}
  COMPONENT dev)

configure_package_config_file(
  cmake/${PROJECT_NAME}Config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
  INSTALL_DESTINATION ${_cfgdir})
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
  DESTINATION ${_cfgdir}
  COMPONENT dev)

# Each binary package needs its own machine-readable copyright (lintian).
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/cmake/deb/copyright
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libencinowaves0
  COMPONENT runtime)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/cmake/deb/copyright
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libencinowaves-dev
  COMPONENT dev)

# The manifest belongs in the install tree so ROS tooling can identify the
# package once it is installed under /opt/ros/<distro>
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/package.xml
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/${PROJECT_NAME}
  COMPONENT dev)

# ---- headless smoke tests (drivers exit non-zero on failure) ----
if(BUILD_TESTING)
  foreach(t test_DirectionalSpreading test_FftwWrapper test_headers
            test_Propagation test_Random)
    add_executable(${t} test/${t}.cc)
    target_link_libraries(${t} PRIVATE ${PROJECT_NAME})
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
endif()

# ---- optional FFT microbenchmark (off by default; needs Google Benchmark) ----
option(ENCINOWAVES_BUILD_BENCHMARKS
       "Build the FFT microbenchmark (requires Google Benchmark)" OFF)
if(ENCINOWAVES_BUILD_BENCHMARKS)
  find_package(benchmark REQUIRED)
  add_executable(bench_FftwWrapper benchmark/bench_FftwWrapper.cc)
  target_link_libraries(bench_FftwWrapper
    PRIVATE ${PROJECT_NAME} benchmark::benchmark)
endif()

# ---- Debian (.deb) packaging via CPack (top-level builds only) ----
# Guarded so add_subdirectory() consumers don't inherit our CPack config.
# (PROJECT_IS_TOP_LEVEL would be cleaner but needs CMake 3.21; floor here is 3.16.)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Packaging.cmake)
endif()
