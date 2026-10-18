# Checks run by hand or by a script (tests/checks), and the GTest suites
if(TARGET backtest)
  add_subdirectory(checks)
endif()

# GTest (BUILD_TESTS=ON)
if(BUILD_TESTS)
  find_package(GTest QUIET)
  if(GTest_FOUND)
    add_subdirectory(unit)
    if(TARGET live AND TARGET ib_api) # the IB gateway build
      add_subdirectory(integration)
    endif()
  else()
    message(STATUS "GTest not found; skip tests/unit (apt install libgtest-dev)")
  endif()
endif()
