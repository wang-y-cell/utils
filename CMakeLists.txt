cmake_minimum_required(VERSION 3.16)
project(utils VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- header-only library ----
find_package(Threads REQUIRED)

add_library(utils INTERFACE)
add_library(utils::utils ALIAS utils)

target_include_directories(utils INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(utils INTERFACE cxx_std_20)
target_link_libraries(utils INTERFACE Threads::Threads)

function(utils_add_exe name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE utils::utils)
endfunction()

# ---- tests (vendored GoogleTest, no network fetch) ----
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE) # 不安装 GoogleTest
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE) # 不构建 GoogleMock
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE) # 强制使用共享 CRT
add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)

enable_testing()
include(GoogleTest)

file(GLOB UTILS_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
add_executable(utils_tests ${UTILS_TEST_SOURCES})
target_link_libraries(utils_tests PRIVATE utils::utils GTest::gtest_main)
gtest_discover_tests(utils_tests DISCOVERY_MODE PRE_TEST)

# ---- demos（按主题分组）----
utils_add_exe(demo_expected      demo/reliability/result/expected_demo.cpp)
utils_add_exe(demo_retry         demo/reliability/retry/demo.cpp)
utils_add_exe(demo_deadline      demo/reliability/time/demo.cpp)
utils_add_exe(demo_executor      demo/concurrency/executor/demo.cpp)
utils_add_exe(demo_thread_pool   demo/concurrency/thread_pool/demo.cpp)
utils_add_exe(demo_signal        demo/concurrency/signal_and_slots/sample.cpp)
utils_add_exe(demo_channel       demo/concurrency/channel/demo.cpp)
utils_add_exe(demo_memory        demo/memory/demo.cpp)

add_custom_target(demos DEPENDS
  demo_expected
  demo_retry
  demo_deadline
  demo_executor
  demo_thread_pool
  demo_signal
  demo_channel
  demo_memory
)

# ---- install (header-only package) ----
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS utils
  EXPORT utilsTargets
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY
  concurrency
  reliability
  memory
  utils
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h"
)

install(EXPORT utilsTargets
  FILE utilsTargets.cmake
  NAMESPACE utils::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/utils
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/utilsConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/utilsConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/utils
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/utilsConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/utilsConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/utilsConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/utils
)
