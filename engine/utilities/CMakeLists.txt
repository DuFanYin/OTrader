cmake_minimum_required(VERSION 3.16)
project(utilities_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generic support, nothing about trading: logging, the engines' base, rings, a queue, pools, a
# thread pool, a futex wake-up.
add_library(utilities_cpp
  base_engine.hpp
  base_engine.cpp
  log_level.hpp
  logger.hpp
  mpsc_ring.hpp
  mpsc_queue.hpp
  object_pool.hpp
  thread_pool.hpp
  wakeup.hpp
)
target_include_directories(utilities_cpp PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
