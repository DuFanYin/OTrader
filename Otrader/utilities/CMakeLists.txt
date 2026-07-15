cmake_minimum_required(VERSION 3.16)
project(utilities_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(utilities_cpp
  combo_builder.hpp
  combo_builder.cpp
  constant.hpp
  mpsc_ring.hpp
  object.hpp
  object.cpp
  utility.hpp
  utility.cpp
  portfolio.hpp
  portfolio.cpp
  base_engine.hpp
  base_engine.cpp
  black_scholes.hpp
  spsc_ring.hpp
  black_scholes.cpp
  ../thirdparty/lets_be_rational/src/LetsBeRational.cpp
  ../thirdparty/lets_be_rational/src/normaldistribution.cpp
  ../thirdparty/lets_be_rational/src/rationalcubic.cpp
  ../thirdparty/lets_be_rational/src/erf_cody.cpp
)
target_include_directories(utilities_cpp PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/lets_be_rational/src
)
