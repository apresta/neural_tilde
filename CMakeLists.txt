cmake_minimum_required(VERSION 3.18)
project(neural_tilde VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(MAX_VERSION 9)

set(MIN_DEVKIT_PATH "${CMAKE_SOURCE_DIR}/deps/min-devkit" CACHE PATH
  "Path to Min-DevKit")
set(MIN_API_PATH "${MIN_DEVKIT_PATH}/source/min-api" CACHE PATH
  "Path to Min API")
set(NEURAL_AUDIO_PATH "${CMAKE_SOURCE_DIR}/deps/NeuralAudio" CACHE PATH
  "Path to NeuralAudio")
set(LANCZOS_RESAMPLER_PATH
  "${CMAKE_SOURCE_DIR}/deps/AudioDSPTools/dsp/ResamplingContainer/Dependencies"
  CACHE PATH "Path to LanczosResampler.h")

set(MAX_EXTERNALS_DIR
  "$ENV{HOME}/Documents/Max ${MAX_VERSION}/Packages/neural/externals"
  CACHE PATH "Path to write Max externals to")

set(LLVM_MINGW_PREFIX "${CMAKE_SOURCE_DIR}/deps/llvm-mingw" CACHE PATH
  "Path to llvm-mingw cross-compiler")

option(NT_SUPERBUILD "Build macOS and Windows externals in one pass" ON)

if(NT_SUPERBUILD AND NOT CMAKE_CROSSCOMPILING)
  include(ExternalProject)

  set(_fwd
    -DNT_SUPERBUILD=OFF
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_EXPORT_COMPILE_COMMANDS=${CMAKE_EXPORT_COMPILE_COMMANDS}
    -DMIN_DEVKIT_PATH=${MIN_DEVKIT_PATH}
    -DNEURAL_AUDIO_PATH=${NEURAL_AUDIO_PATH}
    -DLANCZOS_RESAMPLER_PATH=${LANCZOS_RESAMPLER_PATH}
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

if(NOT EXISTS "${LANCZOS_RESAMPLER_PATH}/LanczosResampler.h")
  message(FATAL_ERROR
    "LanczosResampler.h not found at "
    "LANCZOS_RESAMPLER_PATH='${LANCZOS_RESAMPLER_PATH}'.\n"
    "Add the submodule and run: git submodule update --init --recursive")
endif()

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

if(NOT EXISTS "${NEURAL_AUDIO_PATH}/CMakeLists.txt")
  message(FATAL_ERROR
    "NeuralAudio not found at '${NEURAL_AUDIO_PATH}'.\n"
    "Run: git submodule update --init --recursive")
endif()

set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
add_compile_options(-Wno-deprecated-literal-operator)
add_subdirectory(${NEURAL_AUDIO_PATH} ${CMAKE_BINARY_DIR}/NeuralAudio)
set(CMAKE_WARN_DEPRECATED "${_saved_warn_deprecated}" CACHE BOOL "" FORCE)

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


# Compiler options.

if(APPLE)
  set_target_properties(neural_tilde PROPERTIES
    BUNDLE              TRUE
    BUNDLE_EXTENSION    "mxo"
    MACOSX_BUNDLE_BUNDLE_VERSION        "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING  "${PROJECT_VERSION}"
  )

  target_link_options(neural_tilde PRIVATE -undefined dynamic_lookup)

  target_link_libraries(neural_tilde PRIVATE "-framework CoreFoundation")

  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
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

target_compile_definitions(neural_tilde PRIVATE NOMINMAX)

target_compile_options(neural_tilde PRIVATE
  -O3
  -ffast-math
  -fno-finite-math-only
  -Wall
  -Wextra
  -Wno-unused-parameter
  -Wno-deprecated-literal-operator
)

target_include_directories(neural_tilde SYSTEM PRIVATE
  "${NEURAL_AUDIO_PATH}/NeuralAudio"
  "${NEURAL_AUDIO_PATH}/NeuralAudio/deps/RTNeural/modules/json"
  ${MIN_INCLUDES}
  ${LANCZOS_RESAMPLER_PATH}
)

target_link_libraries(neural_tilde PRIVATE NeuralAudio)

install(TARGETS neural_tilde DESTINATION "${MAX_EXTERNALS_DIR}")
