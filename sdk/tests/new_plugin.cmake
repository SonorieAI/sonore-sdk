# SPDX-License-Identifier: Apache-2.0
# cmake/sonore_new_plugin.cmake, as somebody starting a plugin runs it: make
# an instrument project against this checkout, configure it, build every
# format and the app, and run the project's own ctest -- the app's offline
# self-check and the real-time audit the template registers. A template that
# no longer compiles, or that allocates on the audio thread, is the first
# thing a new user meets, and nothing else here would notice.
# (install_consume makes an effect from the INSTALLED script, which finds the
# SDK with find_package instead.)
#
# And the two refusals: never into a folder that has something in it, and
# never a name that would be read as syntax in the C++ or the CMake it lands in.
#
#   cmake -DSDK=<sdk dir> -DWORK=<scratch dir> -DCONFIG=<Release> -DGEN=<generator>
#         [-DPLATFORM=<x64>] [-DTOOLSET=<ClangCL>] -P new_plugin.cmake

foreach(arg SDK WORK CONFIG GEN)
  if(NOT DEFINED ${arg})
    message(FATAL_ERROR "new_plugin.cmake needs -D${arg}=...")
  endif()
endforeach()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(script "${SDK}/cmake/sonore_new_plugin.cmake")
# In a folder whose name is not ASCII, as a user's often is (C:\Users\José):
# the LV2 bundle step handed that path to its generator on the command line,
# which Windows delivers in the ANSI code page, and the build failed with
# "cannot write .../Jos?/.../manifest.ttl" -- every format, the app and the
# tests with it. Latin, and short, because MSBuild's own file tracker fails on
# a folder outside the ANSI code page and on a path past MAX_PATH.
set(project "${WORK}/Música/GlassPad")

execute_process(COMMAND "${CMAKE_COMMAND}" "-DNAME=Glass Pad" -DTYPE=instrument
                        "-DVENDOR=Acme Audio" "-DDIR=${project}" -P "${script}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the generator failed (${rc}):\n${out}")
endif()
foreach(f CMakeLists.txt src/plugin.cpp README.md .gitignore)
  if(NOT EXISTS "${project}/${f}")
    message(FATAL_ERROR "the generator did not write ${f}:\n${out}")
  endif()
endforeach()
file(READ "${project}/src/plugin.cpp" source)
if(NOT source MATCHES "\"com[.]acmeaudio[.]glasspad\", \"Glass Pad\", \"Acme Audio\"")
  message(FATAL_ERROR "the generated descriptor does not carry the id, name and vendor")
endif()

# Never over something that is there.
file(SHA256 "${project}/src/plugin.cpp" before)
execute_process(COMMAND "${CMAKE_COMMAND}" "-DNAME=Other" "-DDIR=${project}" -P "${script}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
file(SHA256 "${project}/src/plugin.cpp" after)
if(rc EQUAL 0 OR NOT before STREQUAL after)
  message(FATAL_ERROR "the generator wrote into a folder that was not empty")
endif()
# Never a name that is syntax in a C++ string or a CMake argument.
execute_process(COMMAND "${CMAKE_COMMAND}" "-DNAME=Bad \"Quote" "-DDIR=${WORK}/bad" -P "${script}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(rc EQUAL 0 OR EXISTS "${WORK}/bad")
  message(FATAL_ERROR "the generator took a name with a quote in it")
endif()

# The derived identity is the id's. A second product of the vendor whose name
# starts the same way, "Glass Pad Pro", got the Audio Unit "Glass Pad" had --
# its subtype was four letters of the name, aumu Glas Acme for both -- and
# macOS registers one component per type, subtype and manufacturer, so the
# one installed second hid the other. And the same answers make the same
# project: the id, the codes and every file come out again unchanged.
get_filename_component(folder "${project}" DIRECTORY)
set(pro "${folder}/GlassPadPro")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DNAME=Glass Pad Pro" -DTYPE=instrument
                        "-DVENDOR=Acme Audio" "-DDIR=${pro}" -P "${script}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the generator failed on a second project (${rc}):\n${out}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" "-DNAME=Glass Pad" -DTYPE=instrument
                        "-DVENDOR=Acme Audio" "-DDIR=${WORK}/again" -P "${script}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the generator failed the second time round (${rc}):\n${out}")
endif()
function(_au_codes dir out)
  file(STRINGS "${dir}/CMakeLists.txt" line REGEX "AU_SUBTYPE")
  string(REGEX MATCH "AU_SUBTYPE \"[^\"]*\" +AU_MANUFACTURER \"[^\"]*\"" codes "${line}")
  set(${out} "${codes}" PARENT_SCOPE)
endfunction()
_au_codes("${project}" padCodes)
_au_codes("${pro}" proCodes)
if(padCodes STREQUAL "" OR padCodes STREQUAL proCodes)
  message(FATAL_ERROR "\"Glass Pad\" and \"Glass Pad Pro\" were given one Audio Unit: ${padCodes}")
endif()
foreach(f CMakeLists.txt src/plugin.cpp README.md)
  file(SHA256 "${project}/${f}" first)
  file(SHA256 "${WORK}/again/${f}" second)
  if(NOT first STREQUAL second)
    message(FATAL_ERROR "the same answers made a different ${f}")
  endif()
endforeach()

# Built the way its README says, but offline: SONORE_GUI=OFF, as a test that
# downloads is a test that fails on an airplane.
set(configure "${CMAKE_COMMAND}" -S "${project}" -B "${project}/build" -G "${GEN}" -DSONORE_GUI=OFF)
if(PLATFORM)
  list(APPEND configure -A "${PLATFORM}")
endif()
if(TOOLSET)
  list(APPEND configure -T "${TOOLSET}")
endif()
# A Release build is configured as the README writes it, with no build type:
# with a single-configuration generator `--config Release` is ignored, and
# the project has to have chosen Release itself. It was Debug with MSVC (the
# debug runtime: a plugin no machine without Visual Studio loads) and
# unoptimised with gcc and clang. Any other configuration is asked for.
set(singleConfig FALSE)
if(NOT GEN MATCHES "Visual Studio|Xcode|Multi-Config")
  set(singleConfig TRUE)
  if(NOT CONFIG STREQUAL "Release")
    list(APPEND configure "-DCMAKE_BUILD_TYPE=${CONFIG}")
  endif()
endif()
execute_process(COMMAND ${configure} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "configuring the generated project failed (${rc}):\n${out}")
endif()
if(singleConfig)
  file(STRINGS "${project}/build/CMakeCache.txt" buildType REGEX "^CMAKE_BUILD_TYPE:")
  if(NOT buildType MATCHES ":[A-Z]*=${CONFIG}$")
    message(FATAL_ERROR "the generated project, configured for ${CONFIG} as its README says, "
                        "builds \"${buildType}\" with ${GEN}")
  endif()
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${project}/build" --config "${CONFIG}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "building the generated project failed (${rc}):\n${out}")
endif()
get_filename_component(_bin "${CMAKE_COMMAND}" DIRECTORY)
find_program(CTEST ctest HINTS "${_bin}" NO_DEFAULT_PATH)
if(NOT CTEST)
  find_program(CTEST ctest)
endif()
execute_process(COMMAND "${CTEST}" --test-dir "${project}/build" -C "${CONFIG}" --output-on-failure
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0 OR NOT out MATCHES "100% tests passed, 0 tests failed out of 2")
  message(FATAL_ERROR "the generated project's own tests failed (${rc}):\n${out}")
endif()

# The VST3 of a 32-bit Windows build goes in Contents/x86-win, where a 32-bit
# host looks for it. CMake reports the host's processor for a Visual Studio
# -A Win32 build, and the binary went into x86_64-win, where a 64-bit host
# fails to load it and a 32-bit one never looks. Built with the 64-bit
# generator's 32-bit platform, the VST3 alone.
set(win32Note "")
if(GEN MATCHES "Visual Studio" AND ("${PLATFORM}" STREQUAL "" OR "${PLATFORM}" STREQUAL "x64"))
  set(configure32 "${CMAKE_COMMAND}" -S "${project}" -B "${project}/build32" -G "${GEN}" -A Win32
      -DSONORE_GUI=OFF)
  if(TOOLSET)
    list(APPEND configure32 -T "${TOOLSET}")
  endif()
  execute_process(COMMAND ${configure32} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "configuring the generated project for Win32 failed (${rc}):\n${out}")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${project}/build32" --config "${CONFIG}"
                          --target GlassPadVST3
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "building the generated VST3 for Win32 failed (${rc}):\n${out}")
  endif()
  set(bundle "${project}/build32/${CONFIG}/vst3/GlassPadVST3.vst3/Contents")
  if(NOT EXISTS "${bundle}/x86-win/GlassPadVST3.vst3")
    file(GLOB_RECURSE found RELATIVE "${bundle}" "${bundle}/*")
    message(FATAL_ERROR "the Win32 VST3 is not in Contents/x86-win; the bundle holds: ${found}")
  endif()
  set(win32Note "; its Win32 VST3 is in x86-win")
endif()

# Two generated projects under one top-level CMakeLists, the way a vendor
# keeps a folder of plugins: each adds the SDK checkout, and the second one
# stopped the configure at "add_library cannot create target sonore_sdk
# because another target with the same name already exists". Configured
# only: building is what the project above already proved.
file(WRITE "${folder}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.21)
project(Plugins LANGUAGES CXX)
enable_testing()
add_subdirectory(GlassPad)
add_subdirectory(GlassPadPro)
")
string(REPLACE "${project}" "${folder}" both "${configure}")
execute_process(COMMAND ${both} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "two generated projects under one CMakeLists failed to configure (${rc}):\n${out}")
endif()
message(STATUS "a generated instrument built every format and the app, verified itself and "
               "passed the real-time audit${win32Note}; two generated projects configured side by side; "
               "the generator gave two products two Audio Units, made the same project from "
               "the same answers, and refused a full folder and a quoted name")
