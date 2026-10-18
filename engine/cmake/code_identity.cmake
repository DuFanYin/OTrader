# Which code a program or a strategy library was built from, written beside it as JSON (<file>.code):
# for each group, the SHA-256 (16 hex digits) of every file of the repository the compiler read for
# the group's object files (the dependency file beside each, <object>.d), by path and content. A strategy's own files
# (under strategy/) count by name, not by folder, so a version installed for an account is the same
# code as the research file it came from. LIST names a strategy library's group: only its files under
# strategy/ count (the engine's are the programs'), and they are listed with their SHA-256, for a
# reader to check they are still the files the library was built from.
#
#   cmake -DOUT=<file>.code -DGROUPS="name:object,object|name:object" -DENGINE=<engine/> -DSTRATEGY=<strategy/>
#         [-DLIST=<group>] [-DBUILT=<file> -DFINAL=<file>] -P code_identity.cmake
#
# With BUILT and FINAL, BUILT is copied to FINAL through a file renamed into place, so a program
# loading the folder never sees half a library; the .code goes beside FINAL.

cmake_minimum_required(VERSION 3.20)

file(REAL_PATH "${ENGINE}" engine_root)
file(REAL_PATH "${STRATEGY}" strategy_root)

function(group_files objects out)
  set(found)
  foreach(object IN LISTS objects)
    file(READ "${object}.d" text)
    string(REPLACE "\\\n" " " text "${text}")
    string(REGEX REPLACE "^[^:]*:" "" text "${text}") # the object the rule is for
    string(REGEX REPLACE "[ \t\r\n]+" ";" paths "${text}")
    foreach(path IN LISTS paths)
      if(IS_ABSOLUTE "${path}")
        file(REAL_PATH "${path}" real)
        list(APPEND found "${real}")
      endif()
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES found)
  set(${out} "${found}" PARENT_SCOPE)
endfunction()

set(json "{}")
string(REPLACE "|" ";" groups "${GROUPS}")
foreach(group IN LISTS groups)
  string(FIND "${group}" ":" colon)
  string(SUBSTRING "${group}" 0 ${colon} name)
  math(EXPR rest "${colon} + 1")
  string(SUBSTRING "${group}" ${rest} -1 objects)
  string(REPLACE "," ";" objects "${objects}")
  group_files("${objects}" files)
  set(lines)
  set(listed "[]")
  foreach(file IN LISTS files)
    cmake_path(IS_PREFIX strategy_root "${file}" in_strategy)
    cmake_path(IS_PREFIX engine_root "${file}" in_engine)
    if(in_strategy)
      get_filename_component(key "${file}" NAME)
    elseif(in_engine AND NOT name STREQUAL "${LIST}") # a strategy's group: its own files only
      file(RELATIVE_PATH key "${engine_root}" "${file}")
      if(key MATCHES "^build/")
        continue() # generated: what it was made from is counted
      endif()
    else()
      continue() # the system's headers
    endif()
    file(SHA256 "${file}" sha)
    list(APPEND lines "${key} ${sha}")
    if(in_strategy AND name STREQUAL "${LIST}")
      file(RELATIVE_PATH from_repo "${strategy_root}/.." "${file}")
      string(JSON n LENGTH "${listed}")
      string(JSON listed SET "${listed}" ${n} "{\"path\": \"${from_repo}\", \"sha256\": \"${sha}\"}")
    endif()
  endforeach()
  list(SORT lines)
  string(JOIN "\n" all ${lines})
  string(SHA256 hash "${all}")
  string(SUBSTRING "${hash}" 0 16 hash)
  string(JSON json SET "${json}" "${name}" "\"${hash}\"")
  if(name STREQUAL "${LIST}")
    string(JSON json SET "${json}" "files" "${listed}")
  endif()
endforeach()

if(DEFINED FINAL)
  get_filename_component(final_dir "${FINAL}" DIRECTORY)
  get_filename_component(final_name "${FINAL}" NAME)
  file(MAKE_DIRECTORY "${final_dir}")
  file(COPY_FILE "${BUILT}" "${final_dir}/.${final_name}.copying")
  file(RENAME "${final_dir}/.${final_name}.copying" "${FINAL}")
endif()
get_filename_component(out_dir "${OUT}" DIRECTORY)
get_filename_component(out_name "${OUT}" NAME)
file(WRITE "${out_dir}/.${out_name}.writing" "${json}\n")
file(RENAME "${out_dir}/.${out_name}.writing" "${OUT}")
