# model: the trading data model, not engines: orders, trades, contracts, events, portfolios and
# their structure, option symbols, combos; Greeks and implied vol from quant/.

add_library(model
  object.cpp
  symbol.cpp
  utility.cpp
  portfolio.cpp
  portfolio_structure.cpp
  combo_builder.cpp
)
target_include_directories(model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(model PUBLIC utilities_cpp quant)
