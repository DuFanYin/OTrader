# quant: the numerics, no trading and no I/O; OpenMP, bit-identical whatever the thread count.
find_package(OpenMP REQUIRED)

# LetsBeRational: scripts/fetch.sh lets_be_rational (not in git).
set(LETS_BE_RATIONAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/lets_be_rational/src)
if(NOT EXISTS ${LETS_BE_RATIONAL_DIR}/lets_be_rational.cpp)
  message(FATAL_ERROR "LetsBeRational not found in ${LETS_BE_RATIONAL_DIR}; run scripts/fetch.sh lets_be_rational")
endif()
set(LETS_BE_RATIONAL_SOURCES
  ${LETS_BE_RATIONAL_DIR}/lets_be_rational.cpp
  ${LETS_BE_RATIONAL_DIR}/normaldistribution.cpp
  ${LETS_BE_RATIONAL_DIR}/rationalcubic.cpp
  ${LETS_BE_RATIONAL_DIR}/erf_cody.cpp
)
set_source_files_properties(${LETS_BE_RATIONAL_SOURCES} PROPERTIES COMPILE_DEFINITIONS NO_XL_API)

add_library(quant STATIC
  numerics/utils.cpp
  simulation/path.cpp
  simulation/sobol.cpp
  pricing/black.cpp
  pricing/monte_carlo.cpp
  pricing/trinomial_lattice.cpp
  pricing/lsm_american.cpp
  pricing/greeks.cpp
  pricing/pde.cpp
  pricing/hedging.cpp
  pricing/implied_vol.cpp
  pricing/heston.cpp
  pricing/svi.cpp
  optimizer/bayesian.cpp
  stats/moments.cpp
  stats/stochastic_checks.cpp
  stats/selection.cpp
  stats/features.cpp
  ${LETS_BE_RATIONAL_SOURCES}
)
set_target_properties(quant PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(quant PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# PRIVATE: the trading engine gets no OpenMP flags, only the runtime.
target_link_libraries(quant PRIVATE OpenMP::OpenMP_CXX)

# The research tools' C library for the Python server (BUILD_ANALYTICS)
if(BUILD_ANALYTICS)
  add_subdirectory(analytics)
endif()
