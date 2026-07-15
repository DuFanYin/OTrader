# Live 组件测试：不连 TWS，仅验证 EventEngine + MainEngine 及各子引擎能否加载
# 依赖 engines_cpp（由 live/CMakeLists.txt → live/engines 定义）
if(NOT TARGET engines_cpp)
  return()
endif()

set(OTRADER_ROOT ${CMAKE_SOURCE_DIR})

add_executable(test_live_components test_live_components.cpp)
target_include_directories(test_live_components PRIVATE ${OTRADER_ROOT})
target_link_libraries(test_live_components PRIVATE engines_cpp)

add_executable(test_live_market test_live_market.cpp)
target_include_directories(test_live_market PRIVATE ${OTRADER_ROOT})
target_link_libraries(test_live_market PRIVATE engines_cpp)

add_executable(test_live_strategy_straddle test_live_strategy_straddle.cpp)
target_include_directories(test_live_strategy_straddle PRIVATE ${OTRADER_ROOT})
target_link_libraries(test_live_strategy_straddle PRIVATE engines_cpp)
