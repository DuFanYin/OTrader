# db_sqlite: the engine's storage: contracts, strategies, orders, trades and holdings in one SQLite
# file live, in memory in a backtest.
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

add_library(db_sqlite engine_db_sqlite.cpp)
target_include_directories(db_sqlite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(db_sqlite PUBLIC model PRIVATE SQLite::SQLite3 Threads::Threads)
