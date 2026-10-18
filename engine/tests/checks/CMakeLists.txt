# Checks run by hand on synthetic days (each file's header says how).
foreach(check test_hedge_backtest test_backtest_data)
  add_executable(${check} ${check}.cpp)
  target_include_directories(${check} PRIVATE ${CMAKE_SOURCE_DIR})
  target_link_libraries(${check} PRIVATE backtest)
  optra_strategy_host(${check} LOAD)
endforeach()
