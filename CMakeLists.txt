cmake_minimum_required(VERSION 3.30)
cmake_policy(SET CMP0091 NEW)
project(OASIS LANGUAGES C CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Windows MSVC x64 required")
endif()
set(OASIS_MSAA_SAMPLES 1 CACHE STRING "")
if(NOT OASIS_MSAA_SAMPLES MATCHES "^(1|4)$")
    message(FATAL_ERROR "OASIS_MSAA_SAMPLES must be 1 or 4")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "" FORCE)
include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Do not update fetched Git dependencies" FORCE)
option(OASIS_REFRESH_DEPENDENCIES "Explicitly re-fetch third-party dependencies (turn OFF again afterwards)" OFF)
set(_vendor_packages openxr agility dxc cgltf ktx)
set(_vendor_complete TRUE)
foreach(_package IN LISTS _vendor_packages)
    set(_package_dir "${FETCHCONTENT_BASE_DIR}/${_package}-src")
    if(NOT IS_DIRECTORY "${_package_dir}")
        set(_vendor_complete FALSE)
        break()
    endif()
    file(GLOB _package_contents "${_package_dir}/*")
    if(NOT _package_contents)
        set(_vendor_complete FALSE)
        break()
    endif()
endforeach()
if(NOT OASIS_REFRESH_DEPENDENCIES AND _vendor_complete
   AND EXISTS "${CMAKE_BINARY_DIR}/CMakeFiles/generate.stamp")
    set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "Reuse already configured local dependency sources" FORCE)
    message(STATUS "OASIS dependencies: reusing cached sources, download disabled")
else()
    set(FETCHCONTENT_FULLY_DISCONNECTED OFF CACHE BOOL "Allow missing/explicitly refreshed third-party sources" FORCE)
    message(STATUS "OASIS dependencies: initial provisioning or explicit refresh (network permitted)")
endif()
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SPECIFICATION OFF CACHE BOOL "" FORCE)
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.63.zip DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(openxr)
FetchContent_Declare(agility URL https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/1.619.6 DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(dxc URL https://www.nuget.org/api/v2/package/Microsoft.Direct3D.DXC/1.9.2607.13 DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(cgltf URL https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.15.zip DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(agility dxc cgltf)
set(KTX_FEATURE_TESTS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_TOOLS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_DOC OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(KTX_FEATURE_GL_UPLOAD OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_VK_UPLOAD OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_LOADTEST_APPS OFF CACHE STRING "" FORCE)
FetchContent_Declare(ktx GIT_REPOSITORY https://github.com/KhronosGroup/KTX-Software.git GIT_TAG v4.4.2 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(ktx)
file(GLOB_RECURSE AGILITY_HEADERS "${agility_SOURCE_DIR}/*d3d12.h")
file(GLOB_RECURSE AGILITY_FILES "${agility_SOURCE_DIR}/*.dll")
file(GLOB_RECURSE DXC_HEADERS "${dxc_SOURCE_DIR}/*dxcapi.h")
list(GET DXC_HEADERS 0 DXC_HEADER)
get_filename_component(DXC_INCLUDE "${DXC_HEADER}" DIRECTORY)
file(GLOB_RECURSE DXC_FILES "${dxc_SOURCE_DIR}/*.exe" "${dxc_SOURCE_DIR}/*.dll")
list(GET AGILITY_HEADERS 0 AGILITY_HEADER)
get_filename_component(AGILITY_INCLUDE "${AGILITY_HEADER}" DIRECTORY)
function(pick_x64 output name)
    string(TOLOWER "${name}" wanted)
    foreach(candidate IN LISTS ARGN)
        get_filename_component(base "${candidate}" NAME)
        string(TOLOWER "${base}" base)
        string(TOLOWER "${candidate}" path)
        if(base STREQUAL wanted AND path MATCHES "(^|[/\\])(x64|win64|amd64|windows-x64)([/\\]|$)")
            set(${output} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "x64 ${name} missing")
endfunction()
pick_x64(D3D12CORE_DLL D3D12Core.dll ${AGILITY_FILES})
pick_x64(DXC_EXE dxc.exe ${DXC_FILES})
pick_x64(DXCOMPILER_DLL dxcompiler.dll ${DXC_FILES})
pick_x64(DXIL_DLL dxil.dll ${DXC_FILES})
set(SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
function(shader source entry profile)
    set(shader_source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source}")
    set(output "${SHADER_DIR}/${entry}.cso")
    set(material_define "")
    if(ARGC GREATER 3)
        set(output "${SHADER_DIR}/${entry}_M${ARGV3}.cso")
        set(material_define "-DOASIS_MATERIAL_MASK=${ARGV3}")
    endif()
    set(shader_deps shaders/common.hlsli)
    if(source STREQUAL "scene.hlsl")
        list(APPEND shader_deps shaders/reflection_hit.hlsli)
    endif()
    add_custom_command(OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_DIR}"
        COMMAND "${DXC_EXE}" -E ${entry} -T ${profile} -HV 2021 -D OASIS_MSAA_SAMPLES=${OASIS_MSAA_SAMPLES} ${material_define} -O3 -Zpr -Qstrip_debug -Qstrip_reflect -I "${CMAKE_CURRENT_SOURCE_DIR}/shaders" -Fo "${output}" "${shader_source}"
        DEPENDS "${shader_source}" ${shader_deps} VERBATIM)
    set(SHADER_OUTPUTS ${SHADER_OUTPUTS} "${output}" PARENT_SCOPE)
endfunction()

shader(compute.hlsl MergeReflection cs_6_6)
shader(fxaa.hlsl FxaaMain cs_6_6)
shader(scene.hlsl MeshMain ms_6_6)
shader(scene.hlsl ShadowMain ms_6_6)
shader(scene.hlsl PixelMain ps_6_6)
foreach(mask RANGE 0 31)
    shader(scene.hlsl PixelMain ps_6_6 ${mask})
endforeach()
shader(scene.hlsl DepthPixel ps_6_6)
shader(present.hlsl Fullscreen vs_6_6)
shader(present.hlsl Tonemap ps_6_6)
foreach(entry CullMain BatchOpaqueMain TileBatchMark TileBatchScan TileBatchOffsetScan TileBatchOffsetFinalize TileBatchEmit RayMapFillMain RayBuildMain DepthMain HiZMain ClusterSpatialMain LightBvhPartialBounds LightBvhGlobalBounds LightBvhMakeMorton LightBvhBitonic LightBvhRefitLeaves LightBvhRefitParents DenoiseMain DeformMain DeformBoundsMain HotDataMain)
    shader(compute.hlsl ${entry} cs_6_6)
endforeach()
add_custom_target(OASISShaders DEPENDS ${SHADER_OUTPUTS})
set(OASIS_PIPELINE_INPUT "DX12-SM66-RS11-Agility6196-DXC1.9-Zpr-O3-MSAA${OASIS_MSAA_SAMPLES}-${CMAKE_CXX_COMPILER_ID}${CMAKE_CXX_COMPILER_VERSION}")
set(OASIS_PIPELINE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/Graphics.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/Graphics.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsInternal.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsFrameGraph.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsFrameGraph.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsResources.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsSetup.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsDomains.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsRender.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsResidentTiles.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsRayTracing.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsPrepare.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsLighting.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GpuRayScene.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GpuRayScene.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GpuSceneCache.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GpuSceneCache.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GpuMemory.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GpuMemory.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/RuntimeConfig.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GpuPublication.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
file(GLOB_RECURSE OASIS_PIPELINE_SHADER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.hlsli")
list(APPEND OASIS_PIPELINE_SOURCES ${OASIS_PIPELINE_SHADER_SOURCES})
list(SORT OASIS_PIPELINE_SOURCES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OASIS_PIPELINE_SOURCES})
foreach(source IN LISTS OASIS_PIPELINE_SOURCES)
    file(SHA256 "${source}" source_hash)
    string(APPEND OASIS_PIPELINE_INPUT "${source_hash}")
endforeach()
string(SHA256 OASIS_PIPELINE_CACHE_KEY "${OASIS_PIPELINE_INPUT}")
configure_file(src/core/BuildConfig.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/BuildConfig.h" @ONLY)
add_library(Core STATIC
    src/core/Agility.cpp src/core/InputMapping.cpp src/core/InputSystem.cpp src/core/XR.cpp src/core/Graphics.cpp src/core/GraphicsFrameGraph.cpp src/core/GraphicsResources.cpp src/core/GraphicsSetup.cpp src/core/GraphicsDomains.cpp src/core/GraphicsRender.cpp src/core/GraphicsResidentTiles.cpp src/core/GraphicsRayTracing.cpp src/core/GraphicsPrepare.cpp src/core/GraphicsLighting.cpp
    src/core/Assets.cpp src/core/SpectacleGeometry.cpp src/core/Jobs.cpp
    src/core/GpuMemory.cpp src/core/GpuSceneCache.cpp src/core/GpuRayScene.cpp src/core/WorldRegistry.cpp
    src/core/Animation.cpp )
target_include_directories(Core BEFORE PUBLIC "${AGILITY_INCLUDE}")
target_include_directories(Core PUBLIC src/core "${CMAKE_CURRENT_BINARY_DIR}/generated" PRIVATE "${cgltf_SOURCE_DIR}")
target_compile_definitions(Core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00 KHRONOS_STATIC _HAS_EXCEPTIONS=0)
target_compile_options(Core PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:/W4;/utf-8;/permissive-;/O2;/GL;/GR-;/EHs-c-;/arch:AVX2>")
target_link_libraries(Core PUBLIC ktx d3d12 dxgi dxguid ole32 shell32 user32 advapi32 Synchronization windowscodecs openxr_loader)
target_link_options(Core INTERFACE /INCLUDE:D3D12SDKVersion /INCLUDE:D3D12SDKPath)


add_executable(OASIS WIN32
    src/app/main.cpp src/app/VrInputMapping.cpp src/app/DesktopInput.h src/app/DesktopOutput.h)
target_include_directories(OASIS PRIVATE "${DXC_INCLUDE}" src/app)
target_link_libraries(OASIS PRIVATE Core xinput9_1_0)
add_dependencies(OASIS OASISShaders)
target_link_options(OASIS PRIVATE /LTCG /OPT:REF /OPT:ICF)
set_target_properties(OASIS PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:OASIS>")
set(OASIS_RUNTIME_STAGE_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:OASIS>/D3D12" "$<TARGET_FILE_DIR:OASIS>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${D3D12CORE_DLL}" "$<TARGET_FILE_DIR:OASIS>/D3D12/D3D12Core.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/app/input.ini" "$<TARGET_FILE_DIR:OASIS>/input.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/assets/oasis_logo.glb" "$<TARGET_FILE_DIR:OASIS>/oasis_logo.glb"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:OASIS>/dxcompiler.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DXIL_DLL}" "$<TARGET_FILE_DIR:OASIS>/dxil.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ktx>" "$<TARGET_FILE_DIR:OASIS>/ktx.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${SHADER_DIR}" "$<TARGET_FILE_DIR:OASIS>/shaders"
)
add_custom_command(TARGET OASIS POST_BUILD
    ${OASIS_RUNTIME_STAGE_COMMANDS}
    VERBATIM)
add_custom_target(StageOASIS
    ${OASIS_RUNTIME_STAGE_COMMANDS}
    DEPENDS OASIS
    VERBATIM)

add_custom_target(OASISPackage)
add_dependencies(OASISPackage OASIS StageOASIS)
add_custom_target(OASISRuntime DEPENDS OASISPackage)
