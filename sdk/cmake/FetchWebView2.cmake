# SPDX-License-Identifier: Apache-2.0
# Fetch Microsoft's WebView2 SDK at configure time.
#
# Not vendored into the repo: the static loader is ~10 MB per architecture, and
# a NuGet download is one cacheable HTTP GET, the same deal any fetched
# dependency makes. The SDK itself is BSD-3-Clause (redistributable in
# source and binary), so shipping the resulting binary is unrestricted.
#
# Defines: sonore_webview2 (INTERFACE): headers + the STATIC loader, so a built
# .clap has no WebView2Loader.dll to ship beside it. The Edge WebView2 *runtime*
# is a separate machine-level component that Windows 11 ships by default; when
# it is missing the plugin degrades to a message instead of failing to load.

if(TARGET sonore_webview2)
  return()
endif()

set(SONORE_WEBVIEW2_VERSION "1.0.2903.40" CACHE STRING "WebView2 SDK version")
# The package's SHA-256, checked by file(DOWNLOAD). Every seller build and the
# build farm run this script against nuget.org, and a pinned version number is
# not integrity: a hash is. Bumping the version means bumping this beside it
# (`sha256sum` of the .nupkg); setting it EMPTY skips the check for a version
# whose hash is not known yet, loudly, rather than failing to configure.
set(SONORE_WEBVIEW2_SHA256
    "ef128016dd1e51c59178c827ed5b8aa3322c57afa8675d930f8109505542ad74"
    CACHE STRING "SHA-256 of the WebView2 SDK package (empty = unchecked)")
# WHERE the SDK is unpacked, and it is worth an option: the default is inside
# the build tree, so every build on the farm re-downloads the same 10 MB from
# nuget.org. That is slow, and it makes a transient network failure a FAILED
# BUILD -- which is exactly what happened: a marketplace plugin shipped without
# its Windows binary because this GET did not come back. Point
# SONORE_WEBVIEW2_DIR (or the env var) at a persistent directory and the farm
# fetches once, the same deal FETCHCONTENT_BASE_DIR already gets for JUCE.
if(NOT SONORE_WEBVIEW2_DIR AND DEFINED ENV{SONORE_WEBVIEW2_DIR})
  set(SONORE_WEBVIEW2_DIR "$ENV{SONORE_WEBVIEW2_DIR}" CACHE PATH "WebView2 SDK cache directory")
endif()
if(SONORE_WEBVIEW2_DIR)
  # One directory PER VERSION. The cache is shared by every build on a farm and
  # outlives version bumps: unversioned, a bumped SONORE_WEBVIEW2_VERSION found
  # the old SDK already unpacked, skipped the download, and built against it.
  set(_wv2_root "${SONORE_WEBVIEW2_DIR}/${SONORE_WEBVIEW2_VERSION}")
else()
  set(_wv2_root "${CMAKE_BINARY_DIR}/_deps/webview2")
endif()
set(_wv2_zip "${_wv2_root}/webview2.zip")
set(_wv2_url
    "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${SONORE_WEBVIEW2_VERSION}")

# The loader is per-architecture; pick the one matching this build.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]64" OR CMAKE_GENERATOR_PLATFORM MATCHES "ARM64")
    set(_wv2_arch "arm64")
  else()
    set(_wv2_arch "x64")
  endif()
else()
  set(_wv2_arch "x86")
endif()
set(_wv2_lib "${_wv2_root}/build/native/${_wv2_arch}/WebView2LoaderStatic.lib")

# Serialised across processes: several farm builds sharing one cache would
# otherwise download into and extract over the same directory at once, and a
# build could read a half-extracted SDK. Held until the SDK is complete.
file(MAKE_DIRECTORY "${_wv2_root}")
file(LOCK "${_wv2_root}/.fetch.lock" GUARD PROCESS TIMEOUT 900 RESULT_VARIABLE _wv2_lock)
if(NOT _wv2_lock EQUAL 0)
  message(WARNING "WebView2 SDK cache lock not acquired (${_wv2_lock}); continuing unlocked")
endif()

# COMPLETE means the header AND this architecture's loader. Checking the header
# alone let an extraction interrupted half way -- header written, library not
# -- pass for a finished one, and every later configure then stopped at "static
# loader not found" until somebody deleted the directory by hand.
if(NOT EXISTS "${_wv2_root}/build/native/include/WebView2.h" OR NOT EXISTS "${_wv2_lib}")
  message(STATUS "Fetching WebView2 SDK ${SONORE_WEBVIEW2_VERSION}")
  file(MAKE_DIRECTORY "${_wv2_root}")
  # RETRIED, because one HTTP GET against a third party is not a build system.
  # This was a single attempt with no timeout: nuget.org hiccupped during a
  # marketplace build and the plugin was published with no Windows binary at all
  # (the buyer got Linux only). A download that fails three times with a pause
  # between them is a real outage worth failing on; failing on the first is just
  # brittleness.
  if(NOT SONORE_WEBVIEW2_SHA256)
    message(WARNING "WebView2 SDK download is NOT integrity-checked "
                    "(SONORE_WEBVIEW2_SHA256 is empty)")
  endif()
  set(_wv2_code 1)
  foreach(_wv2_try RANGE 1 3)
    if(SONORE_WEBVIEW2_SHA256)
      file(DOWNLOAD "${_wv2_url}" "${_wv2_zip}" SHOW_PROGRESS STATUS _wv2_status
           INACTIVITY_TIMEOUT 60 EXPECTED_HASH SHA256=${SONORE_WEBVIEW2_SHA256})
    else()
      file(DOWNLOAD "${_wv2_url}" "${_wv2_zip}" SHOW_PROGRESS STATUS _wv2_status
           INACTIVITY_TIMEOUT 60)
    endif()
    list(GET _wv2_status 0 _wv2_code)
    if(_wv2_code EQUAL 0)
      break()
    endif()
    list(GET _wv2_status 1 _wv2_msg)
    message(STATUS "WebView2 SDK download attempt ${_wv2_try}/3 failed: ${_wv2_msg}")
    # A partial or hash-mismatched file must not be retried into place.
    file(REMOVE "${_wv2_zip}")
    if(_wv2_try LESS 3)
      execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 5)
    endif()
  endforeach()
  if(NOT _wv2_code EQUAL 0)
    list(GET _wv2_status 1 _wv2_msg)
    message(FATAL_ERROR "WebView2 SDK download failed after 3 attempts: ${_wv2_msg}\n"
                        "It is only needed for a plugin with a web page of its own. Offline, either "
                        "configure with -DSONORE_GUI=OFF (no webview; the native editor still opens), "
                        "or set SONORE_WEBVIEW2_DIR (CMake or environment) to a directory where an "
                        "earlier, online configure kept the download.")
  endif()
  file(ARCHIVE_EXTRACT INPUT "${_wv2_zip}" DESTINATION "${_wv2_root}")
  # The archive has served its purpose, and on a shared cache it is 10 MB per
  # version kept for nothing.
  file(REMOVE "${_wv2_zip}")
endif()
if(_wv2_lock EQUAL 0)
  file(LOCK "${_wv2_root}/.fetch.lock" RELEASE)
endif()

if(NOT EXISTS "${_wv2_lib}")
  message(FATAL_ERROR "WebView2 static loader not found for ${_wv2_arch}: ${_wv2_lib}")
endif()

add_library(sonore_webview2 INTERFACE)
target_include_directories(sonore_webview2 INTERFACE "${_wv2_root}/build/native/include")
target_link_libraries(sonore_webview2 INTERFACE "${_wv2_lib}" version shlwapi)
target_compile_definitions(sonore_webview2 INTERFACE SONORE_HAS_WEBVIEW2=1)
message(STATUS "WebView2 SDK ready (${_wv2_arch})")
