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
set(_wv2_root "${CMAKE_BINARY_DIR}/_deps/webview2")
set(_wv2_zip "${_wv2_root}/webview2.zip")
set(_wv2_url
    "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${SONORE_WEBVIEW2_VERSION}")

if(NOT EXISTS "${_wv2_root}/build/native/include/WebView2.h")
  message(STATUS "Fetching WebView2 SDK ${SONORE_WEBVIEW2_VERSION}")
  file(MAKE_DIRECTORY "${_wv2_root}")
  if(SONORE_WEBVIEW2_SHA256)
    file(DOWNLOAD "${_wv2_url}" "${_wv2_zip}" SHOW_PROGRESS STATUS _wv2_status
         EXPECTED_HASH SHA256=${SONORE_WEBVIEW2_SHA256})
  else()
    message(WARNING "WebView2 SDK download is NOT integrity-checked "
                    "(SONORE_WEBVIEW2_SHA256 is empty)")
    file(DOWNLOAD "${_wv2_url}" "${_wv2_zip}" SHOW_PROGRESS STATUS _wv2_status)
  endif()
  list(GET _wv2_status 0 _wv2_code)
  if(NOT _wv2_code EQUAL 0)
    list(GET _wv2_status 1 _wv2_msg)
    message(FATAL_ERROR "WebView2 SDK download failed: ${_wv2_msg}")
  endif()
  file(ARCHIVE_EXTRACT INPUT "${_wv2_zip}" DESTINATION "${_wv2_root}")
endif()

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
if(NOT EXISTS "${_wv2_lib}")
  message(FATAL_ERROR "WebView2 static loader not found for ${_wv2_arch}: ${_wv2_lib}")
endif()

add_library(sonore_webview2 INTERFACE)
target_include_directories(sonore_webview2 INTERFACE "${_wv2_root}/build/native/include")
target_link_libraries(sonore_webview2 INTERFACE "${_wv2_lib}" version shlwapi)
target_compile_definitions(sonore_webview2 INTERFACE SONORE_HAS_WEBVIEW2=1)
message(STATUS "WebView2 SDK ready (${_wv2_arch})")
