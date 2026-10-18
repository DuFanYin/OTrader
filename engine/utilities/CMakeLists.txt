cmake_minimum_required(VERSION 3.16)
project(utilities_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generic support, nothing about trading: logging and a log source, rings, a queue, pools, a
# thread pool, a futex wake-up, text helpers.
add_library(utilities_cpp
  log_source.hpp
  log_source.cpp
  log_level.hpp
  logger.hpp
  mpsc_ring.hpp
  mpsc_queue.hpp
  object_pool.hpp
  thread_pool.hpp
  wakeup.hpp
  text.hpp
)
target_include_directories(utilities_cpp PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
