cmake_minimum_required(VERSION 3.16)
project(utils)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_executable(utils_main main.cpp)
target_include_directories(utils_main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(smoke_glue tests/smoke_glue.cpp)
target_include_directories(smoke_glue PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(UNIX)
  target_link_libraries(utils_main PRIVATE pthread)
  target_link_libraries(smoke_glue PRIVATE pthread)
endif()

add_executable(signal_sample signal_and_slots/sample.cpp)
target_include_directories(signal_sample PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/signal_and_slots)
if(UNIX)
  target_link_libraries(signal_sample PRIVATE pthread)
endif()
