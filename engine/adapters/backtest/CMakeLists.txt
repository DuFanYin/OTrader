# The backtest's side of the outside world.
#   gateway_backtest: fills the backtest's orders at its quotes, reporting as a live venue does.
#   marketdata_backtest: a day's Parquet file, read, its OCC codes translated, as snapshots.
#   simulation: days of Parquet replayed as a live market and a venue filling at them, for the live
#     engine without a broker (entry_simulate); with the live runtime only.

# Arrow/Parquet: Ubuntu's Arrow config would also look for lz4's, which it does not ship.
set(CMAKE_DISABLE_FIND_PACKAGE_lz4 TRUE)
find_package(Arrow REQUIRED)
find_package(Parquet REQUIRED)
foreach(lib Arrow::arrow Parquet::parquet)
  if(NOT TARGET ${lib})
    if(TARGET ${lib}_shared)
      add_library(${lib} INTERFACE IMPORTED)
      target_link_libraries(${lib} INTERFACE ${lib}_shared)
    elseif(TARGET ${lib}_static)
      add_library(${lib} INTERFACE IMPORTED)
      target_link_libraries(${lib} INTERFACE ${lib}_static)
    else()
      message(FATAL_ERROR "${lib} not found (nor ${lib}_shared, ${lib}_static)")
    endif()
  endif()
endforeach()

add_library(gateway_backtest engine_gateway_backtest.cpp)
target_include_directories(gateway_backtest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gateway_backtest PUBLIC gateway)

add_library(marketdata_backtest engine_data_historical.cpp parquet_loader.cpp occ_utils.cpp)
target_include_directories(marketdata_backtest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(marketdata_backtest PUBLIC model Arrow::arrow Parquet::parquet)

if(TARGET marketdata_live)
  add_library(simulation simulation.cpp)
  target_include_directories(simulation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(simulation PUBLIC gateway_backtest marketdata_backtest marketdata_live)
endif()
