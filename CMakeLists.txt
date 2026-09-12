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
# 使用 tests/main.cpp 作为入口（勿链 gtest_main，否则重复定义 main）
target_link_libraries(utils_tests PRIVATE utils::utils GTest::gtest)
# 测试构建默认开泄漏档 2，便于 dump / 定位用例
target_compile_definitions(utils_tests PRIVATE UTILS_POOL_LEAK_CHECK=2)
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

# ---- microbenchmarks (time_test/) ----
utils_add_exe(time_signal        time_test/signal_and_slots.cpp)
utils_add_exe(time_memory        time_test/memory.cpp)

add_custom_target(time_tests DEPENDS time_signal time_memory)

# 可选：Qt 信号槽对照（需 Qt6 + 匹配的 MinGW，见 time_test/qt_signal_and_slots.cpp 头注释）
set(UTILS_QT_PREFIX "F:/Qt/6.8.3/mingw_64" CACHE PATH "Qt6 prefix for time_qt_signal")
list(APPEND CMAKE_PREFIX_PATH "${UTILS_QT_PREFIX}")
find_package(Qt6 QUIET COMPONENTS Core)
if(Qt6_FOUND)
  add_executable(time_qt_signal
    time_test/qt_signal_and_slots.cpp
    time_test/qt_signal_bench.h
  )
  set_target_properties(time_qt_signal PROPERTIES AUTOMOC ON)
  target_include_directories(time_qt_signal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/time_test
  )
  target_link_libraries(time_qt_signal PRIVATE Qt6::Core)
  target_compile_features(time_qt_signal PRIVATE cxx_std_20)
  # 避免 Qt 的 emit/signals/slots 宏污染其它目标；本目标需要 moc，保留关键词
  # 加入 .gitignore 的 build_qt 由本地对照使用
  add_dependencies(time_tests time_qt_signal)
  message(STATUS "time_qt_signal: Qt6 ${Qt6_VERSION} at ${UTILS_QT_PREFIX}")
else()
  message(STATUS "time_qt_signal: skipped (Qt6 Core not found; set UTILS_QT_PREFIX)")
endif()

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
