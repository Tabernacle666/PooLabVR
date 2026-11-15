cmake_minimum_required(VERSION 3.30)
cmake_policy(SET CMP0091 NEW)
# Set before compiler detection: command-line/environment defaults cannot select Ada.
project(LaboVR VERSION 1.1.9 LANGUAGES C CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Windows MSVC x64 required")
endif()
set(LAB_MSAA_SAMPLES 1 CACHE STRING "")
if(NOT LAB_MSAA_SAMPLES MATCHES "^(1|4)$")
    message(FATAL_ERROR "LAB_MSAA_SAMPLES must be 1 or 4")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "" FORCE)
include(FetchContent)
# Provision dependencies only when needed. The less strict
# FETCHCONTENT_UPDATES_DISCONNECTED does not disable URL downloads.
# On a previously completed configure, bypass *all* download/update/patch
# steps if every pinned source tree is present. This is important for the Windows Release iteration cycle.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Do not update fetched Git dependencies" FORCE)
option(LABOVR_REFRESH_DEPENDENCIES "Explicitly re-fetch third-party dependencies (turn OFF again afterwards)" OFF)
set(_lab_vendor_packages openxr agility dxc pix cgltf ktx)
set(_lab_vendor_complete TRUE)
foreach(_lab_package IN LISTS _lab_vendor_packages)
    set(_lab_package_dir "${FETCHCONTENT_BASE_DIR}/${_lab_package}-src")
    if(NOT IS_DIRECTORY "${_lab_package_dir}")
        set(_lab_vendor_complete FALSE)
        break()
    endif()
    file(GLOB _lab_package_contents "${_lab_package_dir}/*")
    if(NOT _lab_package_contents)
        set(_lab_vendor_complete FALSE)
        break()
    endif()
endforeach()
if(NOT LABOVR_REFRESH_DEPENDENCIES AND _lab_vendor_complete
   AND EXISTS "${CMAKE_BINARY_DIR}/CMakeFiles/generate.stamp")
    set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "Reuse already configured local dependency sources" FORCE)
    message(STATUS "LaboVR dependencies: reusing cached sources, download disabled")
else()
    set(FETCHCONTENT_FULLY_DISCONNECTED OFF CACHE BOOL "Allow missing/explicitly refreshed third-party sources" FORCE)
    message(STATUS "LaboVR dependencies: initial provisioning or explicit refresh (network permitted)")
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
FetchContent_Declare(pix URL https://www.nuget.org/api/v2/package/WinPixEventRuntime/1.0.240308001 DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(cgltf URL https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.15.zip DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(agility dxc cgltf pix)
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
file(GLOB_RECURSE PIX_HEADERS "${pix_SOURCE_DIR}/*pix3.h")
file(GLOB_RECURSE PIX_FILES "${pix_SOURCE_DIR}/*.lib" "${pix_SOURCE_DIR}/*.dll")
list(GET AGILITY_HEADERS 0 AGILITY_HEADER)
list(GET PIX_HEADERS 0 PIX_HEADER)
get_filename_component(PIX_INCLUDE "${PIX_HEADER}" DIRECTORY)
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
pick_x64(D3D12LAYERS_DLL D3D12SDKLayers.dll ${AGILITY_FILES})
pick_x64(DXC_EXE dxc.exe ${DXC_FILES})
pick_x64(DXCOMPILER_DLL dxcompiler.dll ${DXC_FILES})
pick_x64(DXIL_DLL dxil.dll ${DXC_FILES})
pick_x64(PIX_LIB WinPixEventRuntime.lib ${PIX_FILES})
pick_x64(PIX_DLL WinPixEventRuntime.dll ${PIX_FILES})
add_library(pixruntime SHARED IMPORTED GLOBAL)
set_target_properties(pixruntime PROPERTIES IMPORTED_IMPLIB "${PIX_LIB}" IMPORTED_LOCATION "${PIX_DLL}" INTERFACE_INCLUDE_DIRECTORIES "${PIX_INCLUDE}")
set(SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
function(shader source entry profile)
    set(shader_source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source}")
    set(output "${SHADER_DIR}/${entry}.cso")
    set(material_define "")
    if(ARGC GREATER 3)
        set(output "${SHADER_DIR}/${entry}_M${ARGV3}.cso")
        set(material_define "-DLAB_MATERIAL_MASK=${ARGV3}")
    endif()
    set(shader_deps shaders/common.hlsli shaders/optical_common.hlsli)
    if(source STREQUAL "scene.hlsl")
        list(APPEND shader_deps shaders/reflection_hit.hlsli)
    endif()
    add_custom_command(OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_DIR}"
        COMMAND "${DXC_EXE}" -E ${entry} -T ${profile} -HV 2021 -D LAB_MSAA_SAMPLES=${LAB_MSAA_SAMPLES} ${material_define} -O3 -Zpr -Qstrip_debug -Qstrip_reflect -I "${CMAKE_CURRENT_SOURCE_DIR}/shaders" -Fo "${output}" "${shader_source}"
        DEPENDS "${shader_source}" ${shader_deps} VERBATIM)
    set(SHADER_OUTPUTS ${SHADER_OUTPUTS} "${output}" PARENT_SCOPE)
endfunction()

shader(compute.hlsl MergeReflection cs_6_6)
shader(fxaa.hlsl FxaaMain cs_6_6)
shader(scene.hlsl MeshMain ms_6_6)
shader(scene.hlsl ShadowMain ms_6_6)
shader(scene.hlsl PixelMain ps_6_6)
# Compile the 5-bit texture-presence specialization space once; the renderer
# creates PSOs only for material combinations actually found in the scene.
foreach(mask RANGE 0 31)
    shader(scene.hlsl PixelMain ps_6_6 ${mask})
endforeach()
shader(scene.hlsl DepthPixel ps_6_6)
shader(present.hlsl Fullscreen vs_6_6)
shader(present.hlsl Tonemap ps_6_6)
foreach(entry CullMain BatchOpaqueMain TileBatchMark TileBatchScan TileBatchOffsetScan TileBatchOffsetFinalize TileBatchEmit RayMapFillMain RayBuildMain DepthMain HiZMain ClusterSpatialMain LightBvhPartialBounds LightBvhGlobalBounds LightBvhMakeMorton LightBvhBitonic LightBvhRefitLeaves LightBvhRefitParents DenoiseMain DeformMain DeformBoundsMain HotDataMain)
    shader(compute.hlsl ${entry} cs_6_6)
endforeach()
add_custom_target(LaboVRShaders DEPENDS ${SHADER_OUTPUTS})
# Pipeline cache identity is intentionally independent from release labels and world source.
# It changes only when host GPU pipeline inputs change.
set(LAB_ABI_INPUT "LaboVR-PipelineABI16-Visual1-Scene3-SM66-RS11-DX12-Agility6196-DXC1.9-Zpr-O3-MSAA${LAB_MSAA_SAMPLES}-${CMAKE_CXX_COMPILER_ID}${CMAKE_CXX_COMPILER_VERSION}")
# Pipeline cache invalidation must follow GPU pipeline inputs only.
# Hashing all *.cpp/*.h made an ordinary physics/logging edit reconfigure
# the renderer. Keep the PSO cache conservative for renderer/ABI changes,
# while allowing true incremental builds for world, physics and logging edits.
set(LAB_ABI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/Graphics.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/Graphics.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsInternal.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsFrameGraph.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsFrameGraph.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsResources.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsSetup.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsDomains.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GraphicsRender.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GpuRayScene.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GpuRayScene.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GpuSceneCache.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GpuSceneCache.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GpuMemory.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GpuMemory.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/ConsoleSpec.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/GpuPublication.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cartridge_api/LaboVRRendering.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
file(GLOB_RECURSE LAB_ABI_SHADER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.hlsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.hlsli")
list(APPEND LAB_ABI_SOURCES ${LAB_ABI_SHADER_SOURCES})
list(SORT LAB_ABI_SOURCES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LAB_ABI_SOURCES})
foreach(source IN LISTS LAB_ABI_SOURCES)
    file(SHA256 "${source}" source_hash)
    string(APPEND LAB_ABI_INPUT "${source_hash}")
endforeach()
string(SHA256 LAB_PIPELINE_ABI "${LAB_ABI_INPUT}")
configure_file(src/core/BuildConfig.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/BuildConfig.h" @ONLY)
configure_file(cmake/PipelineIdentity.txt.in "${CMAKE_CURRENT_BINARY_DIR}/generated/PipelineIdentity.txt" @ONLY)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/BuildVersion.h" "${CMAKE_CURRENT_BINARY_DIR}/generated/build_identity.json"
    COMMAND ${CMAKE_COMMAND}
        "-DVERSION_FILE=${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt"
        "-DPIPELINE_FILE=${CMAKE_CURRENT_BINARY_DIR}/generated/PipelineIdentity.txt"
        "-DOUTPUT_HEADER=${CMAKE_CURRENT_BINARY_DIR}/generated/BuildVersion.h"
        "-DOUTPUT_JSON=${CMAKE_CURRENT_BINARY_DIR}/generated/build_identity.json"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateBuildIdentity.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" "${CMAKE_CURRENT_BINARY_DIR}/generated/PipelineIdentity.txt" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateBuildIdentity.cmake"
    VERBATIM)
add_custom_target(LaboVRBuildIdentity DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/generated/BuildVersion.h" "${CMAKE_CURRENT_BINARY_DIR}/generated/build_identity.json")
# Core compiles without a lab include directory or lab source file.
add_library(Core STATIC
    src/core/Agility.cpp src/core/InputMapping.cpp src/core/InputSystem.cpp src/core/XR.cpp src/core/Graphics.cpp src/core/GraphicsFrameGraph.cpp src/core/GraphicsResources.cpp src/core/GraphicsSetup.cpp src/core/GraphicsDomains.cpp src/core/GraphicsRender.cpp
    src/core/Assets.cpp src/core/Jobs.cpp
    src/core/GpuMemory.cpp src/core/GpuSceneCache.cpp src/core/GpuRayScene.cpp src/core/WorldRegistry.cpp
    src/core/Animation.cpp )
target_include_directories(Core BEFORE PUBLIC "${AGILITY_INCLUDE}")
target_include_directories(Core PUBLIC src/core "${CMAKE_CURRENT_BINARY_DIR}/generated" PRIVATE "${cgltf_SOURCE_DIR}")
target_compile_definitions(Core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00 KHRONOS_STATIC _HAS_EXCEPTIONS=0)
target_compile_options(Core PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:/W4;/utf-8;/permissive-;/O2;/GL;/GR-;/EHs-c-;/arch:AVX2>")
target_link_libraries(Core PUBLIC ktx pixruntime d3d12 dxgi dxguid ole32 shell32 user32 advapi32 Synchronization windowscodecs openxr_loader)
# Force the SDK exports out of the static archive just as in the former EXE.
target_link_options(Core INTERFACE /INCLUDE:D3D12SDKVersion /INCLUDE:D3D12SDKPath)

# External cartridges are built and packaged outside ConsoleVR.

add_executable(LaboVR WIN32
    labs/labovr/main.cpp labs/labovr/ClientAssets.cpp labs/labovr/LabInputMapping.cpp)
target_include_directories(LaboVR PRIVATE "${DXC_INCLUDE}" labs/labovr src/cartridge_api)
target_link_libraries(LaboVR PRIVATE Core)
add_dependencies(LaboVR LaboVRShaders LaboVRBuildIdentity)
target_link_options(LaboVR PRIVATE /LTCG /OPT:REF /OPT:ICF)
set_target_properties(LaboVR PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:LaboVR>")
add_custom_command(TARGET LaboVR POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:LaboVR>/D3D12" "$<TARGET_FILE_DIR:LaboVR>/assets" "$<TARGET_FILE_DIR:LaboVR>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${D3D12CORE_DLL}" "$<TARGET_FILE_DIR:LaboVR>/D3D12/D3D12Core.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${D3D12LAYERS_DLL}" "$<TARGET_FILE_DIR:LaboVR>/D3D12/D3D12SDKLayers.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PIX_DLL}" "$<TARGET_FILE_DIR:LaboVR>/WinPixEventRuntime.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/labs/labovr/input.ini" "$<TARGET_FILE_DIR:LaboVR>/input.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:LaboVR>/dxcompiler.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DXIL_DLL}" "$<TARGET_FILE_DIR:LaboVR>/dxil.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ktx>" "$<TARGET_FILE_DIR:LaboVR>/ktx.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${SHADER_DIR}" "$<TARGET_FILE_DIR:LaboVR>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/generated/build_identity.json" "$<TARGET_FILE_DIR:LaboVR>/build_identity.json"
    VERBATIM)
# Fast runtime staging: resource/config/shader changes do not need an EXE relink.
add_custom_target(StageLaboVR
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:LaboVR>/D3D12" "$<TARGET_FILE_DIR:LaboVR>/assets" "$<TARGET_FILE_DIR:LaboVR>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${D3D12CORE_DLL}" "$<TARGET_FILE_DIR:LaboVR>/D3D12/D3D12Core.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${D3D12LAYERS_DLL}" "$<TARGET_FILE_DIR:LaboVR>/D3D12/D3D12SDKLayers.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PIX_DLL}" "$<TARGET_FILE_DIR:LaboVR>/WinPixEventRuntime.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/labs/labovr/input.ini" "$<TARGET_FILE_DIR:LaboVR>/input.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DXCOMPILER_DLL}" "$<TARGET_FILE_DIR:LaboVR>/dxcompiler.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DXIL_DLL}" "$<TARGET_FILE_DIR:LaboVR>/dxil.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ktx>" "$<TARGET_FILE_DIR:LaboVR>/ktx.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${SHADER_DIR}" "$<TARGET_FILE_DIR:LaboVR>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/generated/build_identity.json" "$<TARGET_FILE_DIR:LaboVR>/build_identity.json"
    DEPENDS LaboVR
    VERBATIM)

add_custom_target(LaboVRPackage)
add_dependencies(LaboVRPackage LaboVR StageLaboVR)
add_custom_target(LaboVRRuntime DEPENDS LaboVRPackage)
