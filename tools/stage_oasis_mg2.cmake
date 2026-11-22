# Stage the best authored MG2 pack without modifying tracked Git GLBs.
# PREPARE_PBR=ON creates a fresh local working copy for PBR authoring.
if(NOT DEFINED SOURCE_ROOT OR NOT DEFINED TARGET_ROOT)
  message(FATAL_ERROR "SOURCE_ROOT and TARGET_ROOT required")
endif()
get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_local_pack "${_repo}/.oasis-local/mg2_runtime")
set(_stage_source "${SOURCE_ROOT}")
if(NOT PREPARE_PBR AND EXISTS "${_local_pack}/pack.json")
  set(_stage_source "${_local_pack}")
endif()
file(GLOB_RECURSE _paths RELATIVE "${_stage_source}" "${_stage_source}/*")
foreach(_relative IN LISTS _paths)
  if(_relative MATCHES "^\\.pbr_cache(/|$)")
    continue()
  endif()
  set(_src "${_stage_source}/${_relative}")
  if(NOT IS_DIRECTORY "${_src}")
    set(_dst "${TARGET_ROOT}/${_relative}")
    configure_file("${_src}" "${_dst}" COPYONLY)
  endif()
endforeach()
# Rebuild only invalid hero GLBs at runtime stage, never tracked sources.
find_program(_node NAMES node node.exe REQUIRED)
execute_process(
  COMMAND "${_node}" "${CMAKE_CURRENT_LIST_DIR}/mg2_hero_repair.mjs" "${TARGET_ROOT}"
  RESULT_VARIABLE _hero_result
  OUTPUT_VARIABLE _hero_stdout
  ERROR_VARIABLE _hero_stderr
)
if(NOT _hero_result EQUAL 0)
  message(FATAL_ERROR "MG2 hero staging failed:\n${_hero_stdout}\n${_hero_stderr}")
endif()
message(STATUS "${_hero_stdout}")
