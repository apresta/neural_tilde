cmake_minimum_required(VERSION 3.18)
project(neural_tilde VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(MAX_VERSION 9)

set(MIN_DEVKIT_PATH "${CMAKE_SOURCE_DIR}/deps/min-devkit" CACHE PATH
  "Path to Min-DevKit")
set(MIN_API_PATH "${MIN_DEVKIT_PATH}/source/min-api" CACHE PATH
  "Path to Min API")
set(NAM_CORE_PATH "${CMAKE_SOURCE_DIR}/deps/NeuralAmpModelerCore" CACHE PATH
  "Path to NeuralAmpModelerCore")
set(AUDIO_DSP_TOOLS_PATH "${NAM_CORE_PATH}/Dependencies/AudioDSPTools" CACHE PATH
  "Path to AudioDSPTools")
set(MAX_EXTERNALS_DIR
  "$ENV{HOME}/Documents/Max ${MAX_VERSION}/Packages/neural/externals"
  CACHE PATH "Path to write Max externals to")
set(LLVM_MINGW_PREFIX "${CMAKE_SOURCE_DIR}/deps/llvm-mingw" CACHE PATH
  "Path to llvm-mingw cross-compiler")

option(SUPERBUILD "Build macOS and Windows externals in one pass" ON)

if(SUPERBUILD AND NOT CMAKE_CROSSCOMPILING)
  include(ExternalProject)

  set(_fwd
    -DSUPERBUILD=OFF
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_EXPORT_COMPILE_COMMANDS=${CMAKE_EXPORT_COMPILE_COMMANDS}
    -DMIN_DEVKIT_PATH=${MIN_DEVKIT_PATH}
    -DNAM_CORE_PATH=${NAM_CORE_PATH}
    -DAUDIO_DSP_TOOLS_PATH=${AUDIO_DSP_TOOLS_PATH}
    -DMAX_EXTERNALS_DIR=${MAX_EXTERNALS_DIR}
  )

  ExternalProject_Add(neural_tilde_mac
    PREFIX       ${CMAKE_BINARY_DIR}/mac
    SOURCE_DIR   ${CMAKE_SOURCE_DIR}
    BINARY_DIR   ${CMAKE_BINARY_DIR}/mac/build
    CMAKE_ARGS   ${_fwd}
    BUILD_ALWAYS TRUE
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
  )

  ExternalProject_Add(neural_tilde_win
    PREFIX       ${CMAKE_BINARY_DIR}/win
    SOURCE_DIR   ${CMAKE_SOURCE_DIR}
    BINARY_DIR   ${CMAKE_BINARY_DIR}/win/build
    CMAKE_ARGS   ${_fwd}
    CMAKE_CACHE_ARGS
      -DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_SOURCE_DIR}/toolchain-mingw.cmake
      -DLLVM_MINGW_PREFIX:PATH=${LLVM_MINGW_PREFIX}
    BUILD_ALWAYS TRUE
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
  )

  return()
endif()

# Dependency checks.

if(NOT EXISTS "${MIN_API_PATH}/include/c74_min.h")
  if(EXISTS "${MIN_DEVKIT_PATH}/.gitmodules" AND
      NOT EXISTS "${MIN_API_PATH}/include")
    message(FATAL_ERROR
      "min-devkit submodule found but its own submodules are not initialized.")
  else()
    message(FATAL_ERROR
      "min-devkit not found at '${MIN_DEVKIT_PATH}'")
  endif()
endif()

if(NOT EXISTS "${NAM_CORE_PATH}/CMakeLists.txt")
  message(FATAL_ERROR
    "NeuralAmpModelerCore not found at '${NAM_CORE_PATH}'")
endif()

if(NOT EXISTS "${AUDIO_DSP_TOOLS_PATH}/dsp/ResamplingContainer/ResamplingContainer.h")
  message(FATAL_ERROR
    "AudioDSPTools not found at '${AUDIO_DSP_TOOLS_PATH}'")
endif()

set(NAM_ENABLE_A2_FAST    ON  CACHE BOOL "" FORCE)
set(NAM_SAMPLE_FLOAT      ON  CACHE BOOL "" FORCE)
set(NAM_USE_INLINE_GEMM   ON  CACHE BOOL "" FORCE)

add_subdirectory(${NAM_CORE_PATH} ${CMAKE_BINARY_DIR}/NAMCore EXCLUDE_FROM_ALL)

set(MIN_INCLUDES
  "${MIN_API_PATH}/include"
  "${MIN_API_PATH}/max-sdk-base/c74support"
  "${MIN_API_PATH}/max-sdk-base/c74support/max-includes"
  "${MIN_API_PATH}/max-sdk-base/c74support/msp-includes"
)

add_library(neural_tilde MODULE
  neural_tilde.cpp
  $<$<BOOL:${WIN32}>:win_jit_stub.cpp>
  $<$<BOOL:${WIN32}>:neural_tilde.def>
)

set_target_properties(neural_tilde PROPERTIES
  PREFIX      ""
  OUTPUT_NAME "neural~"
)

# Platform-specific settings.

if(APPLE)
  set_target_properties(neural_tilde PROPERTIES
    BUNDLE              TRUE
    BUNDLE_EXTENSION    "mxo"
    MACOSX_BUNDLE_BUNDLE_VERSION        "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING  "${PROJECT_VERSION}"
  )
  target_link_options(neural_tilde PRIVATE -undefined dynamic_lookup -flto)
  target_link_libraries(neural_tilde PRIVATE "-framework CoreFoundation")
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
  target_compile_options(neural_tilde PRIVATE -flto)
endif()

if(WIN32)
  set_target_properties(neural_tilde PROPERTIES SUFFIX ".mxe64")
  target_link_options(neural_tilde PRIVATE
    -stdlib=libc++
    -Wl,-Bstatic,-lc++,-lunwind,-lwinpthread,-Bdynamic
    -Wl,--strip-all
    -Wl,--gc-sections
    -flto
  )
  target_link_libraries(neural_tilde PRIVATE
    "${MIN_API_PATH}/max-sdk-base/c74support/max-includes/x64/MaxAPI.lib"
    "${MIN_API_PATH}/max-sdk-base/c74support/msp-includes/x64/MaxAudio.lib"
  )
  target_compile_options(neural_tilde PRIVATE
    -include ${CMAKE_SOURCE_DIR}/win_prefix.h
    -stdlib=libc++
    -ffunction-sections
    -fdata-sections
    -flto
  )
endif()

set(NEURAL_TILDE_OPT_DEFS NDEBUG)

set(NEURAL_TILDE_OPT_FLAGS
  -O3
  -ffast-math
  -fno-finite-math-only
  -funroll-loops
)

target_compile_definitions(neural_tilde PRIVATE NOMINMAX ${NEURAL_TILDE_OPT_DEFS})

target_compile_options(neural_tilde PRIVATE
  ${NEURAL_TILDE_OPT_FLAGS}
  -Wall
  -Wextra
  -Wno-unused-parameter
  -Wno-deprecated-literal-operator
)

file(GLOB_RECURSE NAM_SOURCES
  "${NAM_CORE_PATH}/NAM/*.cpp"
)

add_library(namcore OBJECT ${NAM_SOURCES})

target_compile_definitions(namcore PUBLIC
  ${NEURAL_TILDE_OPT_DEFS}
  $<$<BOOL:${NAM_ENABLE_A2_FAST}>:NAM_ENABLE_A2_FAST>
  $<$<BOOL:${NAM_SAMPLE_FLOAT}>:NAM_SAMPLE_FLOAT>
  $<$<BOOL:${NAM_USE_INLINE_GEMM}>:NAM_USE_INLINE_GEMM>
)

target_compile_options(namcore PRIVATE ${NEURAL_TILDE_OPT_FLAGS})

if(APPLE OR WIN32)
  target_compile_options(namcore PRIVATE -flto)
endif()

target_include_directories(neural_tilde SYSTEM PRIVATE
  "${NAM_CORE_PATH}/NAM"
  "${NAM_CORE_PATH}/NAM/wavenet"
  "${NAM_CORE_PATH}/Dependencies/eigen"
  "${NAM_CORE_PATH}/Dependencies/nlohmann"
  "${AUDIO_DSP_TOOLS_PATH}/dsp"
  ${MIN_INCLUDES}
)

target_include_directories(namcore SYSTEM PUBLIC
  "${NAM_CORE_PATH}"
  "${NAM_CORE_PATH}/NAM/wavenet"
  "${NAM_CORE_PATH}/Dependencies/eigen"
  "${NAM_CORE_PATH}/Dependencies/nlohmann"
  "${AUDIO_DSP_TOOLS_PATH}/dsp"
)

target_link_libraries(neural_tilde PRIVATE namcore)

install(TARGETS neural_tilde DESTINATION "${MAX_EXTERNALS_DIR}")
