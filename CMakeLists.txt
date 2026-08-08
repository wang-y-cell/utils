cmake_minimum_required(VERSION 3.10)
project(utils)

set(CXX_STANDARD_REQUIRED 20)
set(SRC thread_pool/src/thread_pool.cpp)

add_executable(utils main.cpp ${SRC})

target_include_directories(utils PRIVATE thread_pool/include/)
