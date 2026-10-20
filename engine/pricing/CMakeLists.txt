# pricing: a frame's quotes made into a priced frame (parity forwards, IVs, Greeks), for live
# market data and the backtest's days alike; no state, no threads of its own.
add_library(pricing pricing.cpp)
target_link_libraries(pricing PUBLIC model quant utilities_cpp)
