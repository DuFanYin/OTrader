# marketdata_live: the live quotes, whatever the feed (MarketDataFeed; IB's is adapters/ib):
# subscriptions, the quote book, snapshots for the engine.
add_library(marketdata_live engine_market_data.cpp quote_book.cpp)
target_include_directories(marketdata_live PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(marketdata_live PUBLIC model)
