# SPDX-License-Identifier: Apache-2.0
# The SDK as a stranger gets it: installed, then found.
#
# Every test here builds against the source tree. A project that uses the
# SDK after `cmake --install` sees another layout -- headers under include/,
# the vendored format headers and the helpers under share/SonoreSDK, the
# codecs beside audiofile.h -- and that layout broke three times without a
# test noticing: every find_package() consumer failed to compile, the VST3
# headers landed one directory off, and the codecs were not installed at all.
# So: install this build into a fresh prefix, configure a project that knows
# nothing but find_package(SonoreSDK), and build what a plugin author builds
# first -- a program reading audio, a CLAP, a VST3, an LV2 bundle with its
# generated metadata, and a standalone -- then run the program, and the
# standalone's own offline check (--verify needs no device and no display).
# And the other end: a host program, set up with sonore_enable_hosting(),
# must describe the CLAP, the VST3 and the LV2 it just built. And the project
# generator, run from the install, makes an effect that builds against it.
#
# Offline on purpose (SONORE_GUI=OFF): the installed config honours it, and a
# test that downloads is a test that fails on an airplane.
#
# Run as: cmake -DBUILD=<sdk build dir> -DWORK=<scratch dir> -DCONFIG=<Release>
#               -DGEN=<generator> [-DPLATFORM=<x64>] [-DTOOLSET=<ClangCL>]
#               -DPLUGIN=<plugin.cpp>
#               -P install_consume.cmake

foreach(arg BUILD WORK CONFIG GEN PLUGIN)
  if(NOT DEFINED ${arg})
    message(FATAL_ERROR "install_consume.cmake needs -D${arg}=...")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
set(prefix "${WORK}/prefix")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD}" --config "${CONFIG}" --prefix "${prefix}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "cmake --install failed (${rc}):\n${out}")
endif()

# Headers and CMake only, so a project of either pointer size takes the same
# install. Written with the default version file, an SDK installed from a
# 64-bit configure turned a 32-bit project (-A Win32, an x86 plugin) away
# with "considered but not accepted: ... version: 1.0.0 (64bit)". The
# version file find_package reads, read here as it would be for both sizes.
file(GLOB_RECURSE versionFile "${prefix}/SonoreSDKConfigVersion.cmake")
if(NOT versionFile)
  message(FATAL_ERROR "the install has no SonoreSDKConfigVersion.cmake")
endif()
function(_accepts bits result)
  set(CMAKE_SIZEOF_VOID_P ${bits})
  set(PACKAGE_FIND_VERSION 1.0)
  set(PACKAGE_FIND_VERSION_MAJOR 1)
  include("${versionFile}")
  if(PACKAGE_VERSION_COMPATIBLE AND NOT PACKAGE_VERSION_UNSUITABLE)
    set(${result} TRUE PARENT_SCOPE)
  else()
    set(${result} "FALSE (version ${PACKAGE_VERSION})" PARENT_SCOPE)
  endif()
endfunction()
foreach(bits 4 8)
  _accepts(${bits} accepted)
  if(NOT accepted STREQUAL "TRUE")
    math(EXPR pointerBits "${bits} * 8")
    message(FATAL_ERROR "find_package(SonoreSDK 1.0) from a ${pointerBits}-bit project "
                        "refuses this install: ${accepted}")
  endif()
endforeach()

set(project "${WORK}/consumer")
file(MAKE_DIRECTORY "${project}")
file(WRITE "${project}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.21)
project(SonoreConsumer LANGUAGES CXX)
find_package(SonoreSDK 1.0 REQUIRED)
add_executable(consumer_check main.cpp)
target_link_libraries(consumer_check PRIVATE SonoreSDK::sonore_sdk)
sonore_add_clap(ConsumerClap SOURCES \"\${PLUGIN}\")
sonore_add_vst3(ConsumerVst3 SOURCES \"\${PLUGIN}\")
sonore_add_lv2(ConsumerLv2 SOURCES \"\${PLUGIN}\")
sonore_add_standalone(ConsumerApp SOURCES \"\${PLUGIN}\")
add_executable(consumer_host host.cpp)
target_link_libraries(consumer_host PRIVATE SonoreSDK::sonore_sdk)
sonore_enable_hosting(consumer_host)
")
file(WRITE "${project}/host.cpp" "// A plugin host that only knows the installed SDK.
#include <sonore/host.h>
#include <cstdio>
int main(int argc, char** argv) {
  int failures = 0;
  for (int i = 1; i < argc; ++i) {
    const auto found = sonore::host::describeFile(argv[i]);
    std::printf(\"%s %zu plugin(s) in %s\\n\", found.empty() ? \"FAIL\" : \"ok  \", found.size(), argv[i]);
    if (found.empty()) ++failures;
  }
  return failures == 0 && argc > 1 ? 0 : 1;
}
")
file(WRITE "${project}/main.cpp" "// A program that only knows the installed SDK.
#include <sonore/audiofile.h>
#include <sonore/dsp.h>
#include <sonore/version.h>
#include <cstdio>
int main() {
  sonore::WavData none;
  const bool absent = !sonore::readAudioFile(\"sonore-consumer-missing.wav\", &none);
  sonore::Biquad lowpass;
  lowpass.setSampleRate(48000.0f);
  lowpass.lowpass(1000.0f, 0.7071f);
  float y = 0.0f;
  for (int i = 0; i < 64; ++i) y = lowpass.process(1.0f);
  const bool ok = absent && y > 0.0f && y < 1.5f;
  std::printf(\"%s the installed SDK %s compiles, links and runs in a project of its own\\n\",
              ok ? \"ok  \" : \"FAIL\", SONORE_SDK_VERSION_STRING);
  return ok ? 0 : 1;
}
")

set(configure "${CMAKE_COMMAND}" -S "${project}" -B "${project}/build" -G "${GEN}"
    "-DCMAKE_PREFIX_PATH=${prefix}" -DSONORE_GUI=OFF "-DPLUGIN=${PLUGIN}")
if(PLATFORM)
  list(APPEND configure -A "${PLATFORM}")
endif()
# The toolset too: on a -T ClangCL build the consumer was built by MSVC.
if(TOOLSET)
  list(APPEND configure -T "${TOOLSET}")
endif()
if(NOT GEN MATCHES "Visual Studio|Xcode|Multi-Config")
  list(APPEND configure "-DCMAKE_BUILD_TYPE=${CONFIG}")
endif()
execute_process(COMMAND ${configure} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "configuring a find_package(SonoreSDK) project failed (${rc}):\n${out}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${project}/build" --config "${CONFIG}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "building against the installed SDK failed (${rc}):\n${out}")
endif()

file(GLOB_RECURSE programs "${project}/build/consumer_check" "${project}/build/consumer_check.exe")
list(FILTER programs EXCLUDE REGEX "CMakeFiles")
if(NOT programs)
  message(FATAL_ERROR "consumer_check was not built")
endif()
list(GET programs 0 program)
execute_process(COMMAND "${program}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
message(STATUS "${out}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the consumer program failed (${rc})")
endif()

# Directories too: on macOS a .clap is a bundle, and a file-only glob never
# saw it. With directories listed, the glob also answers every one it walked.
file(GLOB_RECURSE clap LIST_DIRECTORIES true "${project}/build/*ConsumerClap.clap")
list(FILTER clap INCLUDE REGEX "ConsumerClap[.]clap$")
file(GLOB_RECURSE vst3 LIST_DIRECTORIES true "${project}/build/*ConsumerVst3.vst3")
# With directories listed, the glob also answers every directory it walked.
list(FILTER vst3 INCLUDE REGEX "ConsumerVst3[.]vst3$")
file(GLOB_RECURSE ttl "${project}/build/manifest.ttl")
list(FILTER ttl INCLUDE REGEX "ConsumerLv2[.]lv2/manifest[.]ttl$")
if(NOT clap)
  message(FATAL_ERROR "no ConsumerClap.clap was produced")
endif()
if(NOT vst3)
  message(FATAL_ERROR "no ConsumerVst3.vst3 was produced")
endif()
if(NOT ttl)
  message(FATAL_ERROR "no ConsumerLv2.lv2 bundle with its generated manifest was produced")
endif()

# The host, describing what this project built: the .clap file, the .vst3
# bundle (the shortest match is the bundle, not the module inside it) and the
# .lv2 bundle that holds the manifest.
list(GET clap 0 clapFile)
set(vst3Bundle "")
foreach(candidate IN LISTS vst3)
  string(LENGTH "${candidate}" length)
  if(NOT vst3Bundle OR length LESS vst3Length)
    set(vst3Bundle "${candidate}")
    set(vst3Length ${length})
  endif()
endforeach()
list(GET ttl 0 manifest)
get_filename_component(lv2Bundle "${manifest}" DIRECTORY)
file(GLOB_RECURSE hosts "${project}/build/consumer_host" "${project}/build/consumer_host.exe")
list(FILTER hosts EXCLUDE REGEX "CMakeFiles")
if(NOT hosts)
  message(FATAL_ERROR "consumer_host was not built")
endif()
list(GET hosts 0 host)
execute_process(COMMAND "${host}" "${clapFile}" "${vst3Bundle}" "${lv2Bundle}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
message(STATUS "${out}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "a host built with sonore_enable_hosting() did not describe all three formats")
endif()

file(GLOB_RECURSE apps "${project}/build/ConsumerApp" "${project}/build/ConsumerApp.exe"
     "${project}/build/*/ConsumerApp" "${project}/build/*/ConsumerApp.exe")
list(FILTER apps EXCLUDE REGEX "CMakeFiles")
if(NOT apps)
  message(FATAL_ERROR "the standalone ConsumerApp was not built")
endif()
list(GET apps 0 app)
execute_process(COMMAND "${app}" --verify WORKING_DIRECTORY "${project}/build"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the standalone's --verify failed (${rc}):\n${out}")
endif()
# The project generator, run from where the install put it: it must find no
# checkout there and write a find_package project, which then builds against
# this prefix and passes its own self-check.
set(generated "${WORK}/TapeFuzz")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DNAME=Tape Fuzz" "-DVENDOR=Acme Audio"
                        "-DDIR=${generated}"
                        -P "${prefix}/share/SonoreSDK/cmake/sonore_new_plugin.cmake"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the installed sonore_new_plugin.cmake failed (${rc}):\n${out}")
endif()
file(READ "${generated}/CMakeLists.txt" generatedLists)
if(NOT generatedLists MATCHES "set[(]SONORE_SDK_SOURCE \"\" CACHE")
  message(FATAL_ERROR "the installed generator pointed its project at a checkout:\n${out}")
endif()
string(REPLACE "${project}" "${generated}" genConfigure "${configure}")
list(FILTER genConfigure EXCLUDE REGEX "^-DPLUGIN=")
execute_process(COMMAND ${genConfigure} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "configuring the generated project against the install failed (${rc}):\n${out}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${generated}/build" --config "${CONFIG}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "building the generated project against the install failed (${rc}):\n${out}")
endif()
get_filename_component(_bin "${CMAKE_COMMAND}" DIRECTORY)
find_program(CTEST ctest HINTS "${_bin}" NO_DEFAULT_PATH)
if(NOT CTEST)
  find_program(CTEST ctest)
endif()
execute_process(COMMAND "${CTEST}" --test-dir "${generated}/build" -C "${CONFIG}" --output-on-failure
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
# Newer CTest prints "100% tests passed out of 1", with no ", 0 tests failed":
# the summary is matched in both forms, or a run that passed reads as failed.
if(NOT rc EQUAL 0 OR NOT out MATCHES "100% tests passed(, 0 tests failed)? out of 1")
  message(FATAL_ERROR "the generated project's self-check failed against the install (${rc}):\n${out}")
endif()

message(STATUS "the installed SDK built a program, a CLAP, a VST3, an LV2, a standalone and a "
               "host in a project of its own; the standalone verified itself and the host "
               "described all three formats; the installed project generator's effect built "
               "and verified itself")
