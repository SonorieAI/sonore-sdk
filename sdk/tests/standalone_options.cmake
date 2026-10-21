# SPDX-License-Identifier: Apache-2.0
# A device option given something that is not a place in a list must say so.
# `--midi-input Keystation` was read with atoi as 0: the first MIDI port was
# opened, and remembered over the user's own choice, without a word; NaN and
# 1e30 went straight into an int cast, which is undefined behaviour. Each
# must name the option on stderr -- and still run (--verify renders offline,
# so no device is opened), because a bad option is a warning, not a stop.
#
#   cmake -DAPP=<standalone> -P standalone_options.cmake

set(failures "")
foreach(case "--midi-input;Keystation" "--device;nan" "--device;1e30" "--input-device;2.5"
             "--midi-out;-7" "--midi-input")
  list(GET case 0 option)
  execute_process(COMMAND "${APP}" --verify ${case} RESULT_VARIABLE rc
                  OUTPUT_QUIET ERROR_VARIABLE err)
  string(REPLACE ";" " " shown "${case}")
  if(NOT rc EQUAL 0)
    list(APPEND failures "'${shown}' stopped the run (exit ${rc}) instead of warning")
  elseif(NOT err MATCHES "ignoring ${option}")
    list(APPEND failures "'${shown}' was taken without a word (stderr: ${err})")
  endif()
endforeach()

# And a real index is taken quietly.
execute_process(COMMAND "${APP}" --verify --midi-input 0 --device 0 RESULT_VARIABLE rc
                OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR err MATCHES "ignoring")
  list(APPEND failures "'--midi-input 0 --device 0' was refused (exit ${rc}, stderr: ${err})")
endif()

if(failures)
  string(REPLACE ";" "\n  " failures "${failures}")
  message(FATAL_ERROR "device options:\n  ${failures}")
endif()
message(STATUS "every malformed device index is reported, and a real one is taken")
