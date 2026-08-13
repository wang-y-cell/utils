cmake_minimum_required(VERSION 3.16)
project(utils VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive-)
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

# utils_add_exe(utils_main main.cpp)
utils_add_exe(smoke_glue tests/smoke_glue.cpp)

# ---- demos ----
utils_add_exe(demo_expected      demo/result/expected_demo.cpp)
utils_add_exe(demo_scope_guard   demo/scope_guard/demo.cpp)
utils_add_exe(demo_functional    demo/functional/demo.cpp)
utils_add_exe(demo_executor      demo/executor/demo.cpp)
utils_add_exe(demo_thread_pool   demo/thread_pool/demo.cpp)
utils_add_exe(demo_signal        demo/signal_and_slots/sample.cpp)
utils_add_exe(demo_deadline      demo/time/demo.cpp)
utils_add_exe(demo_retry         demo/retry/demo.cpp)
utils_add_exe(demo_channel       demo/channel/demo.cpp)
utils_add_exe(demo_config        demo/config/demo.cpp)
utils_add_exe(demo_log           demo/log/demo.cpp)
utils_add_exe(demo_sql           demo/sql/demo.cpp)

add_custom_target(demos DEPENDS
  demo_expected
  demo_scope_guard
  demo_functional
  demo_executor
  demo_thread_pool
  demo_signal
  demo_deadline
  demo_retry
  demo_channel
  demo_config
  demo_log
  demo_sql
)

# ---- install (header-only package) ----
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS utils
  EXPORT utilsTargets
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY
  component
  adapter
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
