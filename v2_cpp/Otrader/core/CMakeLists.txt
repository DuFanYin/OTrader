cmake_minimum_required(VERSION 3.16)

if(NOT TARGET utilities_cpp)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../utilities ${CMAKE_BINARY_DIR}/utilities)
endif()

# Portfolio structure: load_contracts → portfolios + contracts. Shared by backtest and live.
add_library(portfolio_structure_shared portfolio_structure.cpp)
target_include_directories(portfolio_structure_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(portfolio_structure_shared PUBLIC utilities_cpp)

add_library(position_engine_shared engine_position.cpp)
target_include_directories(position_engine_shared PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../proto
)
target_link_libraries(position_engine_shared PUBLIC utilities_cpp otrader_proto)

add_library(execution_engine_shared engine_execution.cpp)
target_include_directories(execution_engine_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(execution_engine_shared PUBLIC utilities_cpp)

add_library(hedge_engine_shared engine_hedge.cpp)
target_include_directories(hedge_engine_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hedge_engine_shared PUBLIC utilities_cpp)

# 统一策略引擎（core）：RuntimeAPI 注入，backtest/live 共用；支持从 JSON 加载策略默认配置
add_library(option_strategy_engine_shared engine_option_strategy.cpp)
target_include_directories(option_strategy_engine_shared PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../strategy
)
target_link_libraries(option_strategy_engine_shared PUBLIC utilities_cpp strategies nlohmann_json::nlohmann_json)

# Log engine (shared): used by live and backtest
add_library(log_engine_shared engine_log.cpp)
target_include_directories(log_engine_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../utilities)
target_link_libraries(log_engine_shared PUBLIC utilities_cpp)
