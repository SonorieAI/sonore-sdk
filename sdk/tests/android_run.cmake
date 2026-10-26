# Run one test program on an Android device or emulator, as ctest's emulator.
#
# tests/mobile.cmake sets this as CMAKE_CROSSCOMPILING_EMULATOR, so a plain
# `ctest` on the build machine runs every Android test where it can run:
#
#   cmake -DADB=<adb> -DDATA=<host tests/data> -DDEVICE_DIR=/data/local/tmp/sonore
#         -P android_run.cmake <program> [args...]
#
# The program is pushed beside the others, the fixture folder once, and the
# program run there with a writable TMPDIR and HOME -- an Android shell has no
# /tmp and no home -- and its exit status is this script's. ANDROID_SERIAL in
# the environment picks the device when several are attached, as for adb.
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
  message(FATAL_ERROR "android_run.cmake: no program to run")
endif()
set(_program "${CMAKE_ARGV${_first}}")
set(_args "")
math(EXPR _next "${_first} + 1")
if(NOT _next GREATER _last)
  foreach(_i RANGE ${_next} ${_last})
    # Each argument single-quoted for the device's shell.
    string(REPLACE "'" "'\\''" _quoted "${CMAKE_ARGV${_i}}")
    string(APPEND _args " '${_quoted}'")
  endforeach()
endif()
if(NOT ADB)
  message(FATAL_ERROR "android_run.cmake: ADB is not set (no adb was found at configure time)")
endif()
if(NOT DEVICE_DIR)
  set(DEVICE_DIR "/data/local/tmp/sonore")
endif()

get_filename_component(_name "${_program}" NAME)
execute_process(COMMAND "${ADB}" shell "mkdir -p ${DEVICE_DIR}/tmp ${DEVICE_DIR}/home"
                RESULT_VARIABLE _r)
if(NOT _r EQUAL 0)
  message(FATAL_ERROR "android_run.cmake: no device answered adb (is an emulator running?)")
endif()
# The fixtures, once per device: a marker says they are there.
if(DATA)
  execute_process(COMMAND "${ADB}" shell "test -f ${DEVICE_DIR}/data/.pushed" RESULT_VARIABLE _have)
  if(NOT _have EQUAL 0)
    execute_process(COMMAND "${ADB}" push "${DATA}/." "${DEVICE_DIR}/data" OUTPUT_QUIET RESULT_VARIABLE _r)
    if(NOT _r EQUAL 0)
      message(FATAL_ERROR "android_run.cmake: could not push the fixtures in ${DATA}")
    endif()
    execute_process(COMMAND "${ADB}" shell "touch ${DEVICE_DIR}/data/.pushed")
  endif()
endif()
execute_process(COMMAND "${ADB}" push "${_program}" "${DEVICE_DIR}/${_name}" OUTPUT_QUIET RESULT_VARIABLE _r)
if(NOT _r EQUAL 0)
  message(FATAL_ERROR "android_run.cmake: could not push ${_program}")
endif()
# An emulator is no benchmark machine: the speed checks are skipped there, as
# they are on CI (sdk_tests' costJudged() reads CI). A real device judges them.
set(_env "TMPDIR=${DEVICE_DIR}/tmp HOME=${DEVICE_DIR}/home")
execute_process(COMMAND "${ADB}" shell "getprop ro.boot.qemu; getprop ro.kernel.qemu"
                OUTPUT_VARIABLE _qemu OUTPUT_STRIP_TRAILING_WHITESPACE)
if(_qemu MATCHES "1" OR DEFINED ENV{CI})
  string(APPEND _env " CI=1")
endif()
execute_process(
  COMMAND "${ADB}" shell "cd ${DEVICE_DIR} && chmod 755 ./${_name} && ${_env} ./${_name}${_args}"
  RESULT_VARIABLE _status)
if(NOT _status EQUAL 0)
  message(FATAL_ERROR "${_name} exited with ${_status} on the device")
endif()
