add_library(strategies STATIC
  strategy_registry.cpp
  template.cpp
)

# Strategies see the engine only through StrategyContext: core is not on their include path.
target_include_directories(strategies PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/factory
  ${CMAKE_CURRENT_SOURCE_DIR}/../utilities
)
# template.cpp builds combos (utilities::combo). GNU ld resolves static libraries in link
# order, so the dependency has to be declared for utilities_cpp to come after strategies.
target_link_libraries(strategies PUBLIC model)

# The strategies themselves. Each is its .hpp/.cpp pair under factory/ and registers itself
# (REGISTER_STRATEGY), so adding one touches no other file; CONFIGURE_DEPENDS picks a new file up
# on the next build. Only the static registration refers to a strategy's code, and the linker
# leaves an archive member nobody refers to out, so the objects go into each executable that runs
# strategies: those link strategy_factory.
file(GLOB STRATEGY_FACTORY_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/factory/*.cpp)
add_library(strategy_factory OBJECT ${STRATEGY_FACTORY_SOURCES})
target_link_libraries(strategy_factory PUBLIC strategies)
