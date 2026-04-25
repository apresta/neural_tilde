set(CMAKE_SYSTEM_NAME    Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED LLVM_MINGW_PREFIX)
  set(LLVM_MINGW_PREFIX "${CMAKE_CURRENT_LIST_DIR}/deps/llvm-mingw")
endif()

if(NOT EXISTS "${LLVM_MINGW_PREFIX}/bin/x86_64-w64-mingw32-clang")
  message(FATAL_ERROR
    "llvm-mingw not found at LLVM_MINGW_PREFIX='${LLVM_MINGW_PREFIX}'.\n"
    "Run build.sh to fetch it automatically, or set -DLLVM_MINGW_PREFIX= "
    "to point at an existing installation.")
endif()

set(CMAKE_C_COMPILER   ${LLVM_MINGW_PREFIX}/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW_PREFIX}/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER  ${LLVM_MINGW_PREFIX}/bin/x86_64-w64-mingw32-windres)

set(CMAKE_SYSROOT ${LLVM_MINGW_PREFIX}/x86_64-w64-mingw32)

set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW_PREFIX}/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
