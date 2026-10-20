# Research tools: on the offline data (data/INVENTORY.md §2.3, §2.4), the NBBO ticks and the trades
# read a contract or a row group at a time, what an order would have got on them and where trades
# print in the NBBO; on the backtest days, option structures valued, the dealers' gamma and the
# underlying, through the engine's own day reading and pricing. Not part of the trading
# engine: nothing here is linked into core, a backtest or live. entry_research is its program.
add_library(research offline_data.cpp fills.cpp structures.cpp)
target_include_directories(research PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(research PUBLIC Arrow::arrow Parquet::parquet historical_data model
                      PRIVATE nlohmann_json::nlohmann_json)
