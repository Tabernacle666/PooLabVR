# Applied to the pinned upstream source before its CMake projects are evaluated.
set(path "${PHYSX_SOURCE}/physx/source/compiler/cmakegpu/CMakeLists.txt")
file(READ "${path}" content)
string(REGEX REPLACE "GENERATE_ARCH_CODE_LIST\\(SASS \"[0-9,]+\" PTX \"[0-9]+\"\\)"
    "GENERATE_ARCH_CODE_LIST(SASS \"120\" PTX \"120\")" content "${content}")
if(NOT content MATCHES "GENERATE_ARCH_CODE_LIST\\(SASS \"120\" PTX \"120\"\\)")
    message(FATAL_ERROR "PhysX CUDA build changed: SM120 patch cannot be verified")
endif()
file(WRITE "${path}" "${content}")
