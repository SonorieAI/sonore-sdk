# SPDX-License-Identifier: Apache-2.0
# Sonore SDK -- Windows on Arm, parsed by the compiler this box does have.
#
# ── Why this exists ──────────────────────────────────────────────────────────
#
# The denormal guard carried GNU inline assembly under _M_ARM64. cl.exe has no
# inline assembly on ARM64 at all, so every plugin built with Visual Studio for
# Windows on Arm stopped at that line -- and nothing here noticed, because the
# machine this SDK is developed on has no ARM64 CRT and could not have linked
# a single ARM64 object. The VST3 bundle layout had already been fixed to put
# an ARM64 binary where a host looks for one; the binary itself would never
# have been produced.
#
# clang-cl can PARSE for any target it was built with, and parsing is where
# that class of mistake lives: -fsyntax-only instantiates every template and
# checks every intrinsic and every #if branch for aarch64-pc-windows-msvc,
# while asking nothing of a linker. So whenever the compiler is clang-cl --
# the build farm's compiler, and the Windows-clangcl CI job -- every
# translation unit that reaches a platform-specific line is parsed for
# Windows on Arm as one ctest. What it cannot prove, stated plainly: that
# the result links, or runs. That needs a Mac-shaped promise nobody here can
# make yet -- an ARM64 Windows runner.
#
# Run by CMakeLists.txt as
#   cmake -DCXX=<clang-cl> -DSDK=<sdk dir> -DINCLUDES=<list> -DDEFS=<list>
#         -P tests/arm64_syntax.cmake
if(NOT CXX OR NOT SDK)
  message(FATAL_ERROR "arm64_syntax.cmake needs -DCXX and -DSDK")
endif()

# Each entry is "source|DEFINE;DEFINE": the same plugin source for every
# format it builds as, the tests whose headers only Windows compiles, and the
# suite that includes almost everything else.
set(_units
  "tests/sdk_tests.cpp|SONORE_TEST_DATA_DIR=\"${SDK}/tests/data\""
  "examples/saturator/plugin.cpp|"
  "examples/saturator/plugin.cpp|SONORE_BUILD_VST3=1"
  "examples/saturator/plugin.cpp|SONORE_BUILD_LV2=1"
  "examples/saturator/plugin.cpp|SONORE_BUILD_STANDALONE=1"
  "examples/synth/plugin.cpp|"
  "examples/sampler/plugin.cpp|"
  "examples/arp/plugin.cpp|"
  "examples/splitter/plugin.cpp|"
  "examples/trim/plugin.cpp|"
  "examples/guiprobe/plugin.cpp|"
  "examples/guiprobe/plugin.cpp|SONORE_BUILD_VST3=1"
  "tests/native_window_test.cpp|"
  "tests/clap_host_test.cpp|"
  "tests/vst3_host_test.cpp|"
  "tests/editor_soak_test.cpp|"
  "tests/rt_safety_test.cpp|"
  "tests/concurrency_test.cpp|"
  "tests/fuzz_parsers.cpp|SONORE_TEST_DATA_DIR=\"${SDK}/tests/data\""
  "tests/gfx_demo.cpp|"
  "tests/asio_probe.cpp|")

set(_include_flags)
foreach(_dir IN LISTS INCLUDES)
  list(APPEND _include_flags "/I${_dir}")
endforeach()
list(APPEND _include_flags "/I${SDK}/third_party/vst3" "/I${SDK}/third_party/lv2/include")
set(_define_flags)
foreach(_def IN LISTS DEFS)
  list(APPEND _define_flags "/D${_def}")
endforeach()

set(_failed 0)
set(_count 0)
foreach(_unit IN LISTS _units)
  string(REPLACE "|" ";" _parts "${_unit}")
  list(GET _parts 0 _src)
  list(LENGTH _parts _n)
  set(_extra)
  if(_n GREATER 1)
    list(GET _parts 1 _extra_raw)
    if(_extra_raw)
      list(APPEND _extra "/D${_extra_raw}")
    endif()
  endif()
  math(EXPR _count "${_count} + 1")
  execute_process(
    COMMAND "${CXX}" --target=aarch64-pc-windows-msvc /nologo /std:c++17 /EHsc /utf-8
            -fsyntax-only ${_include_flags} ${_define_flags} ${_extra} "${SDK}/${_src}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
  if(_result EQUAL 0)
    message(STATUS "  ok   ${_src} ${_extra}")
  else()
    message(STATUS "  FAIL ${_src} ${_extra}")
    message(STATUS "${_out}${_err}")
    set(_failed 1)
  endif()
endforeach()

if(_failed)
  message(FATAL_ERROR "Windows on Arm: at least one translation unit does not parse for "
                      "aarch64-pc-windows-msvc (see above)")
endif()
message(STATUS "Windows on Arm: ${_count} translation units parse for aarch64-pc-windows-msvc")
