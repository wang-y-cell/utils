cmake_minimum_required(VERSION 3.10)
project(utils)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(utils main.cpp)

target_include_directories(utils PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
