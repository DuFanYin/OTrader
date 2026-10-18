# The strategy framework. Strategies see the engine only through StrategyContext.
add_library(strategies STATIC
  strategy_registry.cpp
  strategy.cpp
)
target_include_directories(strategies PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../utilities
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(strategies PUBLIC model ${CMAKE_DL_LIBS})

# The strategy API: a hash of the headers strategies compile against, in every program and
# strategy library; a library with another is refused (StrategyRegistry::load).
file(GLOB STRATEGY_API_HEADERS CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../model/*.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../utilities/*.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../quant/kernel.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../quant/screener/*.h
)
set(STRATEGY_API ${CMAKE_CURRENT_BINARY_DIR}/strategy_api.hpp)
add_custom_command(
  OUTPUT ${STRATEGY_API}
  COMMAND sh -c "printf '#define OPTRA_STRATEGY_API \"%s\"\\n' $(cat \"$@\" | sha256sum | cut -c1-16) > ${STRATEGY_API}" sh ${STRATEGY_API_HEADERS}
  DEPENDS ${STRATEGY_API_HEADERS}
  VERBATIM)
add_custom_target(strategy_api DEPENDS ${STRATEGY_API})
add_dependencies(strategies strategy_api)
target_include_directories(strategies PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

# Each .cpp in a folder of the repository's strategy/ (demo, research, paper, live; the server's
# tests make folders of their own) is a module linking nothing (it calls into its host).
# demo goes to build/strategies, the rest to a subfolder each: linked in build/strategies-linked,
# then renamed into place with its .code beside it (code_identity.cmake), so a program loading the
# folder never sees half a library. This is the one way a strategy is built (the server too).
set(STRATEGY_SOURCES ${CMAKE_SOURCE_DIR}/../strategy)
set(STRATEGY_LIBRARIES)
# The strategy API's headers compiled once, for every strategy to reuse: a strategy compiles in about
# a second instead of four.
add_library(strategy_pch OBJECT ${CMAKE_SOURCE_DIR}/cmake/empty.cpp)
target_link_libraries(strategy_pch PRIVATE $<COMPILE_ONLY:strategies>)
set_target_properties(strategy_pch PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_precompile_headers(strategy_pch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/strategy.hpp)
add_dependencies(strategy_pch strategy_api)
file(GLOB folders LIST_DIRECTORIES true RELATIVE ${STRATEGY_SOURCES} CONFIGURE_DEPENDS
     ${STRATEGY_SOURCES}/*)
foreach(folder ${folders})
  if(NOT IS_DIRECTORY ${STRATEGY_SOURCES}/${folder})
    continue()
  endif()
  file(GLOB sources CONFIGURE_DEPENDS ${STRATEGY_SOURCES}/${folder}/*.cpp)
  if(folder STREQUAL "demo")
    set(output ${CMAKE_BINARY_DIR}/strategies)
  else()
    set(output ${CMAKE_BINARY_DIR}/strategies/${folder})
  endif()
  foreach(source ${sources})
    get_filename_component(name ${source} NAME_WE)
    set(library strategy_${folder}_${name})
    add_library(${library} MODULE ${source})
    target_link_libraries(${library} PRIVATE $<COMPILE_ONLY:strategies>)
    target_include_directories(${library} PRIVATE ${STRATEGY_SOURCES})
    target_precompile_headers(${library} REUSE_FROM strategy_pch)
    add_dependencies(${library} strategy_api)
    set_target_properties(${library} PROPERTIES
      PREFIX "" OUTPUT_NAME ${name}
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/strategies-linked/${folder})
    # The folder is made only at configure time; it may have been removed since.
    add_custom_command(TARGET ${library} PRE_LINK
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/strategies-linked/${folder}
      VERBATIM)
    add_custom_command(TARGET ${library} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -DOUT=${output}/${name}.so.code
              "-DGROUPS=strategy:$<JOIN:$<TARGET_OBJECTS:${library}>,$<COMMA>>" -DLIST=strategy
              -DENGINE=${CMAKE_SOURCE_DIR} -DSTRATEGY=${STRATEGY_SOURCES}
              -DBUILT=$<TARGET_FILE:${library}> -DFINAL=${output}/${name}.so
              -P ${CMAKE_SOURCE_DIR}/cmake/code_identity.cmake
      VERBATIM)
    list(APPEND STRATEGY_LIBRARIES ${library})
  endforeach()
endforeach()
add_custom_target(optra_strategies DEPENDS ${STRATEGY_LIBRARIES})

# A host exports its symbols and whole-archives everything strategies may call. LOAD makes a test
# load the build's strategies at start.
function(optra_strategy_host target)
  cmake_parse_arguments(HOST "LOAD" "" "" ${ARGN})
  target_link_libraries(${target} PRIVATE strategies)
  set_target_properties(${target} PROPERTIES
    ENABLE_EXPORTS ON
    LINK_LIBRARY_OVERRIDE_strategies WHOLE_ARCHIVE
    LINK_LIBRARY_OVERRIDE_model WHOLE_ARCHIVE
    LINK_LIBRARY_OVERRIDE_utilities_cpp WHOLE_ARCHIVE
    LINK_LIBRARY_OVERRIDE_quant WHOLE_ARCHIVE)
  add_dependencies(${target} optra_strategies)
  if(NOT target MATCHES "^entry_")
    target_compile_definitions(${target} PRIVATE
      OPTRA_STRATEGY_LIBRARIES="${CMAKE_BINARY_DIR}/strategies")
  endif()
  if(HOST_LOAD)
    target_sources(${target} PRIVATE ${CMAKE_SOURCE_DIR}/tests/strategy_libraries.cpp)
  endif()
endfunction()
