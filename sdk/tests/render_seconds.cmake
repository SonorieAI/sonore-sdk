# SPDX-License-Identifier: Apache-2.0
# `App --render out.wav 3` must render three seconds -- the seconds after the
# path are documented in standalone.h and the README, and were ignored: only
# the path was read, and every render was the default two seconds.
#
#   cmake -DAPP=<standalone> -DOUT=<file.wav> -P render_seconds.cmake
#
# The file is a 32-bit float stereo WAV at 48 kHz: 44 header bytes and eight
# per frame, so its size says how long it is without anything to parse it.
execute_process(COMMAND "${APP}" --render "${OUT}" 3 RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "${APP} --render exited ${rc}")
endif()
file(SIZE "${OUT}" size)
file(REMOVE "${OUT}")
math(EXPR three "44 + 48000 * 3 * 8")
math(EXPR threeAndAHalf "44 + 48000 * 7 * 4")
if(size LESS three OR size GREATER threeAndAHalf)
  message(FATAL_ERROR "--render out.wav 3 wrote ${size} bytes; three seconds is ${three}")
endif()
message(STATUS "--render out.wav 3 wrote ${size} bytes: three seconds")

# A MIDI file decides the length of a render it is played into, and that
# length is held to the limit --seconds is. data/far_note.mid is 37 bytes:
# a note-on, and its note-off one delta of 0x0FFFFFFF ticks later at a tick a
# quarter -- four years. --render and --verify reserved the whole render up
# front and died of it (0xC0000409). Refused now, exit 2, with the reason.
foreach(mode "--render;${OUT}" "--verify")
  execute_process(COMMAND "${APP}" ${mode} --midi-in-file "${CMAKE_CURRENT_LIST_DIR}/data/far_note.mid"
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  file(REMOVE "${OUT}")
  list(GET mode 0 shown)
  if(NOT rc EQUAL 2 OR NOT err MATCHES "at most 3600 seconds")
    message(FATAL_ERROR "${shown} with a four-year MIDI file exited ${rc} (want 2; stderr: ${err})")
  endif()
endforeach()
message(STATUS "a MIDI file longer than a render may be is refused, not rendered")
