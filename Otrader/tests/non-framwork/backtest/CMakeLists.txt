# Backtest 测试：依赖 backtest_engines，include 以 Otrader 为根
set(OTRADER_ROOT ${CMAKE_SOURCE_DIR})
add_executable(test_backtest_data test_backtest_data.cpp)
target_include_directories(test_backtest_data PRIVATE ${OTRADER_ROOT})
target_link_libraries(test_backtest_data PRIVATE backtest_engines)

add_executable(test_backtest test_backtest.cpp)
target_include_directories(test_backtest PRIVATE ${OTRADER_ROOT})
target_link_libraries(test_backtest PRIVATE backtest_engines)

add_executable(test_backtest_pos test_backtest_pos.cpp)
target_include_directories(test_backtest_pos PRIVATE ${OTRADER_ROOT})
target_link_libraries(test_backtest_pos PRIVATE backtest_engines)

add_executable(test_backtest_order test_backtest_order.cpp)
target_include_directories(test_backtest_order PRIVATE ${OTRADER_ROOT})
target_link_libraries(test_backtest_order PRIVATE backtest_engines)
