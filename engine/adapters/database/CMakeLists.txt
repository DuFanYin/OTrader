# The storage (core::Database): SQLite, a file, live. A backtest keeps what it needs in memory
# (backtest_database.hpp, header only).
find_package(SQLite3 REQUIRED)

add_library(sqlite_database sqlite_database.cpp)
find_package(Threads REQUIRED)
target_link_libraries(sqlite_database PUBLIC database PRIVATE SQLite::SQLite3 Threads::Threads)
