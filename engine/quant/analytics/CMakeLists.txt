# analytics: the research tools' workflows on quant (pricing, scenarios, hedging, validation,
# calibration, the screener), their JSON requests and responses, and libanalytics_c.so, the C ABI
# the Python server (app/server) loads with ctypes.
add_library(analytics STATIC
  pricing_engine.cpp
  scenario_engine.cpp
  hedging_engine.cpp
  validation_engine.cpp
  utility_engine.cpp
  calibration_engine.cpp
  screener_engine.cpp
)
set_target_properties(analytics PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(analytics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(OpenMP REQUIRED)
target_link_libraries(analytics PUBLIC quant nlohmann_json::nlohmann_json OpenMP::OpenMP_CXX)

add_library(analytics_c SHARED c_api.cpp)
target_link_libraries(analytics_c PRIVATE analytics)
