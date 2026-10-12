# quant: the numerics, nothing about trading and no I/O — pricing (Black-Scholes, Monte Carlo,
# binomial and trinomial trees, finite differences, Longstaff-Schwartz, Heston), Greeks, implied
# vol, hedging simulations, paths, statistics, an optimizer, the option screener. Parallel with
# OpenMP, bit-identical whatever the thread count. The trading engine (model/) uses its Black
# Greeks and implied vol.
find_package(OpenMP REQUIRED)

# LetsBeRational (Peter Jäckel's implied Black volatility) is not in git:
# scripts/fetch_lets_be_rational.sh puts it in thirdparty/lets_be_rational/src.
set(LETS_BE_RATIONAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/lets_be_rational/src)
if(NOT EXISTS ${LETS_BE_RATIONAL_DIR}/lets_be_rational.cpp)
  message(FATAL_ERROR "LetsBeRational not found in ${LETS_BE_RATIONAL_DIR}; run scripts/fetch_lets_be_rational.sh")
endif()
set(LETS_BE_RATIONAL_SOURCES
  ${LETS_BE_RATIONAL_DIR}/lets_be_rational.cpp
  ${LETS_BE_RATIONAL_DIR}/normaldistribution.cpp
  ${LETS_BE_RATIONAL_DIR}/rationalcubic.cpp
  ${LETS_BE_RATIONAL_DIR}/erf_cody.cpp
)
# NO_XL_API leaves out its Excel add-in layer.
set_source_files_properties(${LETS_BE_RATIONAL_SOURCES} PROPERTIES COMPILE_DEFINITIONS NO_XL_API)

add_library(quant STATIC
  numerics/utils.cpp
  simulation/path.cpp
  simulation/sobol.cpp
  pricing/black_scholes.cpp
  pricing/black.cpp
  pricing/monte_carlo.cpp
  pricing/binomial_lattice.cpp
  pricing/trinomial_lattice.cpp
  pricing/lsm_american.cpp
  pricing/greeks.cpp
  pricing/pde.cpp
  pricing/hedging.cpp
  pricing/implied_vol.cpp
  pricing/heston.cpp
  optimizer/nelder_mead.cpp
  stats/moments.cpp
  stats/stochastic_checks.cpp
  screener/option_filter.cpp
  screener/strategies.cpp
  ${LETS_BE_RATIONAL_SOURCES}
)
# Linked into the analytics' shared library, so position-independent.
set_target_properties(quant PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(quant PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# PRIVATE: no OpenMP flags for what links quant (the trading engine has no parallel regions); the
# runtime is still linked in.
target_link_libraries(quant PRIVATE OpenMP::OpenMP_CXX)

# The research tools' workflows on it and their C ABI, for the Python server (BUILD_ANALYTICS)
if(BUILD_ANALYTICS)
  add_subdirectory(analytics)
endif()
