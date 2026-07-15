## Minimal ThriftConfig.cmake for Homebrew thrift.
##
## Why this exists:
## - Homebrew thrift may not ship a CMake package config.
## - Arrow/Parquet's FindThriftAlt.cmake prefers `find_package(Thrift CONFIG)`.
##
## This file makes `find_package(Thrift ...)` succeed in CONFIG mode and provides:
## - Thrift_FOUND
## - THRIFT_COMPILER
## - target thrift::thrift (IMPORTED)

cmake_minimum_required(VERSION 3.16)

set(Thrift_FOUND TRUE)

# Homebrew layout
set(_THRIFT_PREFIX "/opt/homebrew/opt/thrift")
set(Thrift_INCLUDE_DIR "${_THRIFT_PREFIX}/include")

# Prefer shared lib; static is also present if you want to switch later.
set(_THRIFT_LIB "${_THRIFT_PREFIX}/lib/libthrift.dylib")
if(NOT EXISTS "${_THRIFT_LIB}")
  # Fallback to static library
  set(_THRIFT_LIB "${_THRIFT_PREFIX}/lib/libthrift.a")
endif()

# Thrift compiler executable (used by Arrow's FindThriftAlt.cmake)
set(THRIFT_COMPILER "/opt/homebrew/bin/thrift")
if(NOT EXISTS "${THRIFT_COMPILER}")
  set(THRIFT_COMPILER "${_THRIFT_PREFIX}/bin/thrift")
endif()

set(Thrift_LIBRARIES "${_THRIFT_LIB}")
set(Thrift_INCLUDE_DIRS "${Thrift_INCLUDE_DIR}")

if(NOT TARGET thrift::thrift)
  add_library(thrift::thrift UNKNOWN IMPORTED)
  set_target_properties(thrift::thrift PROPERTIES
    IMPORTED_LOCATION "${_THRIFT_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${Thrift_INCLUDE_DIR}"
  )
endif()

