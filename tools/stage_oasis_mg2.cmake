# Stage source packs or the local PBR overlay without modifying tracked Git GLBs.
if(NOT DEFINED SOURCE_ROOT OR NOT DEFINED TARGET_ROOT)
  message(FATAL_ERROR "SOURCE_ROOT and TARGET_ROOT required")
endif()
get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_local_pack "${_repo}/.oasis-local/mg2_runtime")
# Python is already the interpreter required by the PBR pipeline. Search
# its actual installation paths before falling back to PATH aliases.
set(_python "")
if(DEFINED OASIS_PYTHON AND EXISTS "${OASIS_PYTHON}")
  set(_python "${OASIS_PYTHON}")
elseif(DEFINED ENV{OASIS_PYTHON} AND EXISTS "$ENV{OASIS_PYTHON}")
  set(_python "$ENV{OASIS_PYTHON}")
endif()
if(NOT _python)
  foreach(_candidate
    "${_repo}/.venv/Scripts/python.exe"
    "D:/gitlink/Apps/Python312/python.exe"
    "D:/gitlink/Apps/Python313/python.exe"
    "$ENV{LOCALAPPDATA}/Programs/Python/Python313/python.exe"
    "$ENV{LOCALAPPDATA}/Programs/Python/Python312/python.exe")
    if(EXISTS "${_candidate}")
      set(_python "${_candidate}")
      break()
    endif()
  endforeach()
endif()
if(NOT _python)
  find_program(_python_found NAMES python.exe python python3)
  if(_python_found)
    set(_python "${_python_found}")
  endif()
endif()
if(NOT _python)
  message(FATAL_ERROR "MG2 staging needs Python 3.10+, present for pbr_lot1.bat")
endif()

# A previous pbr_lot1 run can contain real CC0 scans at a higher revision
# than main, while main has newer geometry and room recipes. Merge by asset
# identity first, preserving acquired scans and adding the latest authored GLBs.
set(_stage_source "${SOURCE_ROOT}")
if(NOT PREPARE_PBR AND EXISTS "${_local_pack}/pack.json")
  execute_process(
    COMMAND "${_python}" "${CMAKE_CURRENT_LIST_DIR}/mg2_overlay_reconcile.py"
            "${SOURCE_ROOT}" "${_local_pack}"
    RESULT_VARIABLE _merge_result
    OUTPUT_VARIABLE _merge_out
    ERROR_VARIABLE _merge_err
  )
  if(NOT _merge_result EQUAL 0)
    message(FATAL_ERROR "MG2 local PBR merge failed:\n${_merge_out}\n${_merge_err}")
  endif()
  if(_merge_out)
    message(STATUS "${_merge_out}")
  endif()
  set(_stage_source "${_local_pack}")
endif()
get_filename_component(_stage_abs "${_stage_source}" ABSOLUTE)
get_filename_component(_target_abs "${TARGET_ROOT}" ABSOLUTE)
get_filename_component(_local_abs "${_local_pack}" ABSOLUTE)
set(_authoring_overlay FALSE)
if("${_target_abs}" STREQUAL "${_local_abs}")
  set(_authoring_overlay TRUE)
endif()

# Release staging excludes source-only files, but the authoring overlay retains
# the acquisition catalogues needed by the local PBR import tool.
if(NOT _authoring_overlay AND IS_DIRECTORY "${TARGET_ROOT}")
  file(GLOB_RECURSE _already_staged LIST_DIRECTORIES FALSE
    RELATIVE "${TARGET_ROOT}" "${TARGET_ROOT}/*")
  foreach(_relative IN LISTS _already_staged)
    if(_relative MATCHES "^(pack\\.json|runtime\\.json|room_profiles_v1\\.json|placement_recipe_v2\\.json)$"
       OR _relative MATCHES "^(geometry|meta|provenance|recipes)/")
      continue()
    endif()
    file(REMOVE "${TARGET_ROOT}/${_relative}")
  endforeach()
  file(REMOVE "${TARGET_ROOT}/.mg2_source_revision.json")
  file(REMOVE_RECURSE "${TARGET_ROOT}/hero_sources" "${TARGET_ROOT}/.pbr_cache")
endif()

if(NOT "${_stage_abs}" STREQUAL "${_target_abs}")
  file(GLOB_RECURSE _paths RELATIVE "${_stage_source}" "${_stage_source}/*")
  foreach(_relative IN LISTS _paths)
    if(_relative MATCHES "^\\.pbr_cache(/|$)" OR _relative MATCHES "^hero_sources(/|$)"
       OR _relative MATCHES "\\.md$")
      continue()
    endif()
    if(NOT _authoring_overlay
       AND NOT (_relative MATCHES "^(pack\\.json|runtime\\.json|room_profiles_v1\\.json|placement_recipe_v2\\.json)$"
                OR _relative MATCHES "^(geometry|meta|provenance|recipes)/"))
      continue()
    endif()
    set(_src "${_stage_source}/${_relative}")
    if(NOT IS_DIRECTORY "${_src}")
      set(_dst "${TARGET_ROOT}/${_relative}")
      configure_file("${_src}" "${_dst}" COPYONLY)
    endif()
  endforeach()
endif()
execute_process(
  COMMAND "${_python}" "${CMAKE_CURRENT_LIST_DIR}/mg2_hero_repair.py" "${TARGET_ROOT}"
  RESULT_VARIABLE _result
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err
)
if(NOT _result EQUAL 0)
  message(FATAL_ERROR "MG2 staged GLB validation failed:\n${_out}\n${_err}")
endif()
string(STRIP "${_out}" _out)
message(STATUS "${_out}")
