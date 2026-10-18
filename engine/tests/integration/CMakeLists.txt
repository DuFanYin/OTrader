# Integration tests against an IB paper account (test_ib_paper.cpp says how to run them): skipped
# unless OPTRA_IB_PAPER_TESTS=1, labelled ib_paper (`ctest -L ib_paper`).
find_package(SQLite3 REQUIRED) # the tests copy the database through SQLite's backup

add_executable(test_ib_paper_gtest test_ib_paper.cpp)
target_include_directories(test_ib_paper_gtest PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_ib_paper_gtest PRIVATE live SQLite::SQLite3 GTest::gtest_main)
optra_strategy_host(test_ib_paper_gtest LOAD)
target_compile_definitions(test_ib_paper_gtest PRIVATE
  OPTRA_DEFAULT_DB="${CMAKE_SOURCE_DIR}/../data/optra.db"
  OPTRA_IB_PAPER_DIR="${CMAKE_CURRENT_BINARY_DIR}")

include(GoogleTest)
gtest_discover_tests(test_ib_paper_gtest
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS ib_paper RUN_SERIAL TRUE TIMEOUT 900
)
if(TARGET optra_gtest_all)
  add_dependencies(optra_gtest_all test_ib_paper_gtest) # built with the other suites
endif()
