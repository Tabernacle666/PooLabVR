# The renderer can operate without individual visual assets. Never keep a
# stale staged copy that conceals the absence of its source.
if(NOT DEFINED SOURCE_ROOT OR NOT DEFINED TARGET_ROOT)
  message(FATAL_ERROR "SOURCE_ROOT and TARGET_ROOT required")
endif()
foreach(_model oasis_logo.glb phosphor_orbit.glb glove_left.glb glove_right.glb
               basalt_equipped.glb scoria_equipped.glb navette_organes.glb vaisseau2_z31.glb)
  if(EXISTS "${SOURCE_ROOT}/${_model}")
    configure_file("${SOURCE_ROOT}/${_model}" "${TARGET_ROOT}/${_model}" COPYONLY)
  else()
    file(REMOVE "${TARGET_ROOT}/${_model}")
    if(NOT _model STREQUAL "vaisseau2_z31.glb")
      message(WARNING "asset_unavailable source=${_model}")
    endif()
  endif()
endforeach()
