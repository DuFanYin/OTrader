# model: the trading data model, not engines: orders, fills, contracts, events, portfolios and
# their structure, option symbols, combos; Greeks and implied vol from quant/.

add_library(model
  object.cpp
  symbol.cpp
  utility.cpp
  portfolio.cpp
  portfolios.cpp
  order_builder.cpp
  daily_table.cpp
  parameters.cpp
)
target_include_directories(model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(model PUBLIC utilities_cpp quant)
