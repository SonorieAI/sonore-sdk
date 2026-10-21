# SPDX-License-Identifier: Apache-2.0
# `App --render <path>` must write where the path says, whatever script the
# path is written in. On Windows argv arrives in the ANSI code page, so a path
# through a folder named in Cyrillic reached the file layer as question marks
# and nothing was written; the standalone now reads its command line as UTF-16
# and every file call takes UTF-8.
#
#   cmake -DAPP=<standalone> -DDIR=<scratch folder> -P render_unicode.cmake
#
# CMake keeps strings in UTF-8 and passes them to the process as UTF-16, as
# Explorer and every shell do.
set(folder "${DIR}/sonore-тест-ñ")
file(REMOVE_RECURSE "${folder}")
file(MAKE_DIRECTORY "${folder}")
set(out "${folder}/take.wav")
execute_process(COMMAND "${APP}" --render "${out}" 1 RESULT_VARIABLE rc OUTPUT_QUIET)
if(NOT EXISTS "${out}")
  file(REMOVE_RECURSE "${folder}")
  message(FATAL_ERROR "--render into a Cyrillic folder wrote nothing (exit ${rc})")
endif()
file(SIZE "${out}" size)
file(REMOVE_RECURSE "${folder}")
if(size LESS 1000)
  message(FATAL_ERROR "--render into a Cyrillic folder wrote only ${size} bytes")
endif()
message(STATUS "--render into a Cyrillic folder wrote ${size} bytes")
