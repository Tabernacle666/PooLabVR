# Refresh only the deployed read-only fallback from the authoritative source.
# SAVE never writes this directory; user profiles remain in LocalAppData.
file(GLOB_RECURSE profiles RELATIVE "${SOURCE}" "${SOURCE}/*.json")
foreach(profile IN LISTS profiles)
    if(profile MATCHES "(^|/)system-default\\.json$")
        get_filename_component(parent "${DESTINATION}/${profile}" DIRECTORY)
        file(MAKE_DIRECTORY "${parent}")
        file(COPY_FILE "${SOURCE}/${profile}" "${DESTINATION}/${profile}" ONLY_IF_DIFFERENT)
    endif()
endforeach()
