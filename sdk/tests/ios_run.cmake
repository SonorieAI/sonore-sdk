# Run one test program in an iOS simulator, as ctest's emulator.
#
# tests/mobile.cmake sets this as CMAKE_CROSSCOMPILING_EMULATOR for an iOS
# build, so `ctest` on the Mac runs every iOS test where it runs:
#
#   cmake -DSIMULATOR=<udid or booted> -P ios_run.cmake <program> [args...]
#
# simctl spawn starts the program inside the booted simulator's environment
# -- iOS's libraries, its sandbox paths -- straight from the build folder,
# which the simulator sees: nothing to install, and its exit status is this
# script's. A simulator must be booted first (xcrun simctl boot <udid>).
cmake_minimum_required(VERSION 3.21)

set(_first -1)
math(EXPR _last "${CMAKE_ARGC} - 1")
foreach(_i RANGE 0 ${_last})
  if(CMAKE_ARGV${_i} STREQUAL "-P")
    math(EXPR _first "${_i} + 2")
    break()
  endif()
endforeach()
if(_first LESS 0 OR _first GREATER _last)
  message(FATAL_ERROR "ios_run.cmake: no program to run")
endif()
set(_command "${CMAKE_ARGV${_first}}")
math(EXPR _next "${_first} + 1")
if(NOT _next GREATER _last)
  foreach(_i RANGE ${_next} ${_last})
    list(APPEND _command "${CMAKE_ARGV${_i}}")
  endforeach()
endif()
if(NOT SIMULATOR)
  set(SIMULATOR booted)
endif()
# CI=1 in the child, as on every CI runner: a simulator on a shared machine is
# no place to judge speed (sdk_tests' costJudged()).
set(ENV{SIMCTL_CHILD_CI} "1")
execute_process(COMMAND xcrun simctl spawn "${SIMULATOR}" ${_command} RESULT_VARIABLE _status)
if(NOT _status EQUAL 0)
  message(FATAL_ERROR "${CMAKE_ARGV${_first}} exited with ${_status} in the simulator")
endif()
