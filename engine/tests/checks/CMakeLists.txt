# Checks run by hand or by a script, not by ctest (each needs days made by
# POST /api/data/synthetic or scripts/data/synthetic_to_parquet.py; the flags are in the file's header):
#   test_hedge_backtest  delta hedging end to end against the model that priced the quotes
#   test_backtest_data   a day's chain as the engine reads it, IV included
#                        (app/server/tests/trading/test_days.py checks the IV it reads back)
foreach(check test_hedge_backtest test_backtest_data)
  add_executable(${check} ${check}.cpp)
  target_include_directories(${check} PRIVATE ${CMAKE_SOURCE_DIR})
  target_link_libraries(${check} PRIVATE core_backtest strategy_factory)
endforeach()
