# Sonore SDK example: the folder examples/three serves, composed at configure.
#
# The page and its module come from examples/three/ui; three.js comes from npm,
# PINNED and checked against the SHA-512 npm publishes for it, so the library
# is not carried in this repository and a tampered download is refused. The
# same route the WebView2 SDK takes (cmake/FetchWebView2.cmake).
#
# Sets SONORE_THREE_UI to the composed folder, or leaves it empty -- and the
# example skipped, with a warning -- when the download cannot be had. A build
# with no network still builds everything else.

set(_three_version 0.186.1)
set(_three_sha512
    6e515ea9be3dc110921941a3ee0b697e74861f2da5c0393de11854992d5cfe14dbcbbd24bc285b5a9909e0f9b5dfdd0bab3cef4e6ce05ca1cf11a7c9c026fc8c)
set(_three_dir "${CMAKE_BINARY_DIR}/_deps/three-${_three_version}")
set(SONORE_THREE_UI "")

if(NOT EXISTS "${_three_dir}/package/build/three.module.js")
  file(MAKE_DIRECTORY "${_three_dir}")
  message(STATUS "Fetching three.js ${_three_version} for examples/three")
  # A name of this run's own: a parallel build re-runs configure in several
  # processes at once, and two downloads into one file let one extract what
  # the other was truncating.
  string(RANDOM LENGTH 10 _three_run)
  set(_three_tgz "${_three_dir}/three-${_three_run}.tgz")
  set(_three_tmp "${_three_dir}/unpack-${_three_run}")
  file(DOWNLOAD "https://registry.npmjs.org/three/-/three-${_three_version}.tgz" "${_three_tgz}"
       STATUS _three_status TLS_VERIFY ON)
  list(GET _three_status 0 _three_code)
  set(_three_ok FALSE)
  if(_three_code EQUAL 0)
    # Checked here rather than with EXPECTED_HASH, which fails the whole
    # configure: a bad download skips one example, it does not stop the build.
    file(SHA512 "${_three_tgz}" _three_got)
    if(_three_got STREQUAL _three_sha512)
      file(ARCHIVE_EXTRACT INPUT "${_three_tgz}" DESTINATION "${_three_tmp}"
           PATTERNS package/LICENSE package/build/three.module.js package/build/three.core.js
                    package/examples/jsm/environments/RoomEnvironment.js)
      # Into place in one step; a run that finds another got there first
      # keeps that one.
      if(NOT EXISTS "${_three_dir}/package/build/three.module.js")
        file(REMOVE_RECURSE "${_three_dir}/package") # what an interrupted run left
        file(RENAME "${_three_tmp}/package" "${_three_dir}/package" RESULT _three_moved)
      endif()
      file(REMOVE_RECURSE "${_three_tmp}")
      set(_three_ok TRUE)
    else()
      message(WARNING "examples/three: three.js ${_three_version} did not match its SHA-512; skipped")
    endif()
  else()
    list(GET _three_status 1 _three_why)
    message(WARNING "examples/three: could not fetch three.js (${_three_why}); skipped")
  endif()
  file(REMOVE "${_three_tgz}")
  if(NOT _three_ok)
    return()
  endif()
endif()

# The folder: the example's own files, then three.js where the import map
# names it. configure_file, so an edit to the page re-runs configure.
set(SONORE_THREE_UI "${CMAKE_BINARY_DIR}/examples/three_ui")
file(GLOB _three_own CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/ui/*")
foreach(_f IN LISTS _three_own)
  get_filename_component(_name "${_f}" NAME)
  configure_file("${_f}" "${SONORE_THREE_UI}/${_name}" COPYONLY)
endforeach()
set(_v "${SONORE_THREE_UI}/vendor/three")
configure_file("${_three_dir}/package/build/three.module.js" "${_v}/three.module.js" COPYONLY)
configure_file("${_three_dir}/package/build/three.core.js" "${_v}/three.core.js" COPYONLY)
configure_file("${_three_dir}/package/examples/jsm/environments/RoomEnvironment.js"
               "${_v}/addons/environments/RoomEnvironment.js" COPYONLY)
# three.js is MIT: its licence travels with it, in the plugin.
configure_file("${_three_dir}/package/LICENSE" "${_v}/LICENSE" COPYONLY)
