cmake_minimum_required(VERSION 3.16)
project(utils)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_TYPE Debug)

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

function(utils_add_exe name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(UNIX)
    target_link_libraries(${name} PRIVATE pthread)
  endif()
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
