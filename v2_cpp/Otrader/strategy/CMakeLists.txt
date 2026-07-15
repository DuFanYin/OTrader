add_library(strategies STATIC
  strategy_registry.cpp
  template.cpp
  factory/straddletest.cpp
  factory/ironcondortest.cpp
  factory/iv_mean_revert.cpp
  factory/straddle_inventory_scalper.cpp
)

target_include_directories(strategies PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/factory
  ${CMAKE_CURRENT_SOURCE_DIR}/../core
  ${CMAKE_CURRENT_SOURCE_DIR}/../utilities
)
