# SPDX-License-Identifier: Apache-2.0
# The guides' code, built: every ```cpp block under sdk/docs whose first line
# is `// test-compile` or `// test-run` becomes a translation unit here.
#
#   include("${CMAKE_CURRENT_SOURCE_DIR}/tests/doc_snippets.cmake")
#
# from sdk/CMakeLists.txt, after enable_testing() and the helpers. What a
# block becomes depends on what it is:
#
#   a plugin (it includes <sonore/formats.h>)
#       a CLAP and a standalone application, both built; `// test-run` adds a
#       ctest running the standalone's --verify -- the same self-check a
#       generated project runs on itself;
#   a program (it has a main function)
#       an executable, linked for hosting; `// test-run` runs it as a ctest;
#   anything else
#       an object library: compiled, never linked.
#
# ── Why ─────────────────────────────────────────────────────────────────────
#
# A guide's code is the part a reader copies, and code in prose rots without
# anybody noticing: the API moves, the guide does not, and the first person to
# find out is the one who pasted it. Built with the tests, a guide whose code
# stopped compiling fails the build instead.
#
# The blocks are read at configure time, and every guide is a configure
# dependency, so editing a guide re-runs the extraction on the next build. A
# file is rewritten only when its code changed, so an edit to a guide's prose
# recompiles nothing.
#
# Strings, not lists, throughout: C++ is full of semicolons, and a CMake list
# would split the code at every one of them.

file(GLOB_RECURSE _sonore_doc_pages CONFIGURE_DEPENDS "${SONORE_SDK_DIR}/docs/*.md")
set(_sonore_doc_dir "${CMAKE_CURRENT_BINARY_DIR}/doc_snippets")
file(MAKE_DIRECTORY "${_sonore_doc_dir}")
find_package(Threads REQUIRED)
set(_sonore_doc_count 0)

foreach(_page IN LISTS _sonore_doc_pages)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_page}")
  file(READ "${_page}" _text)
  string(REPLACE "\r\n" "\n" _text "${_text}")
  get_filename_component(_stem "${_page}" NAME_WE)
  string(MAKE_C_IDENTIFIER "${_stem}" _stem)
  set(_n 0)
  while(TRUE)
    string(FIND "${_text}" "```cpp\n// test-" _at)
    if(_at EQUAL -1)
      break()
    endif()
    math(EXPR _at "${_at} + 7") # past "```cpp\n"
    string(SUBSTRING "${_text}" ${_at} -1 _text)
    string(FIND "${_text}" "\n```" _end)
    if(_end EQUAL -1)
      message(FATAL_ERROR "${_page}: a ```cpp block marked // test-... is never closed")
    endif()
    string(SUBSTRING "${_text}" 0 ${_end} _code)
    math(EXPR _end "${_end} + 4")
    string(SUBSTRING "${_text}" ${_end} -1 _text)
    math(EXPR _n "${_n} + 1")

    set(_run OFF)
    string(FIND "${_code}" "// test-run\n" _marker)
    if(_marker EQUAL 0)
      set(_run ON)
    else()
      string(FIND "${_code}" "// test-compile\n" _marker)
      if(NOT _marker EQUAL 0)
        message(FATAL_ERROR "${_page}: block ${_n} starts with neither // test-run nor // test-compile")
      endif()
    endif()

    set(_name "doc_${_stem}_${_n}")
    set(_file "${_sonore_doc_dir}/${_name}.cpp")
    set(_old "")
    if(EXISTS "${_file}")
      file(READ "${_file}" _old)
    endif()
    if(NOT _old STREQUAL "${_code}\n")
      file(WRITE "${_file}" "${_code}\n")
    endif()

    # Each test runs in a folder of its own: --verify writes its files into
    # the working directory, and two at once must not share them. The preset
    # root keeps everything a test touches inside the build tree.
    set(_work "${_sonore_doc_dir}/${_name}.run")
    file(MAKE_DIRECTORY "${_work}")

    string(FIND "${_code}" "#include <sonore/formats.h>" _plugin)
    string(REGEX MATCH "int main\\(" _main "${_code}")
    if(NOT _plugin EQUAL -1)
      sonore_add_clap(${_name} SOURCES "${_file}")
      sonore_add_standalone(${_name}_app SOURCES "${_file}")
      set(_targets ${_name} ${_name}_app)
      if(_run)
        add_test(NAME ${_name} COMMAND ${_name}_app --verify WORKING_DIRECTORY "${_work}")
      endif()
    elseif(_main)
      add_executable(${_name} "${_file}")
      target_link_libraries(${_name} PRIVATE sonore_sdk Threads::Threads)
      sonore_enable_hosting(${_name})
      set(_targets ${_name})
      if(_run)
        add_test(NAME ${_name} COMMAND ${_name} WORKING_DIRECTORY "${_work}")
      endif()
    else()
      add_library(${_name} OBJECT "${_file}")
      target_link_libraries(${_name} PRIVATE sonore_sdk)
      set(_targets ${_name})
    endif()
    if(_run)
      set_tests_properties(${_name} PROPERTIES ENVIRONMENT "SONORE_PRESET_ROOT=${_work}/presets")
    endif()
    # A guide's code is held to the warning level the SDK's own /W4 targets
    # keep (sonore_w4_*): a reader's build should not open with warnings from
    # the code they pasted.
    if(MSVC)
      foreach(_t IN LISTS _targets)
        target_compile_options(${_t} PRIVATE /W4 /WX)
      endforeach()
    endif()
    math(EXPR _sonore_doc_count "${_sonore_doc_count} + 1")
  endwhile()
endforeach()

message(STATUS "Sonore SDK: ${_sonore_doc_count} code blocks from the guides are built as tests")
