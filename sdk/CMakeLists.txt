# SPDX-License-Identifier: Apache-2.0
# Sonore SDK: our own audio plugin framework.
#
# No framework dependency: the only third-party code is the CLAP headers (MIT),
# vendored under third_party/clap. Everything else in include/sonore is ours.
#
#   add_subdirectory(sdk)
#   sonore_add_clap(MyPlugin SOURCES plugin.cpp)
#
# produces MyPlugin.clap, ready to drop in the host's CLAP folder.
cmake_minimum_required(VERSION 3.21)
project(SonoreSDK LANGUAGES CXX)

# ── The SDK itself: header-only, so an INTERFACE target ──────────────────────
set(SONORE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "Sonore SDK root")
add_library(sonore_sdk INTERFACE)
target_include_directories(sonore_sdk INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/clap/include")
target_compile_features(sonore_sdk INTERFACE cxx_std_17)

# MSVC mangles UTF-8 literals without /utf-8, and sellers build with stock MSVC.
if(MSVC)
  target_compile_options(sonore_sdk INTERFACE /utf-8)
  # fopen/getenv/sscanf are the portable calls, and the CRT's "unsafe" notices
  # about them are noise MSVC hides in its default project settings but
  # clang-cl prints 400 times over.
  target_compile_definitions(sonore_sdk INTERFACE _CRT_SECURE_NO_WARNINGS)
endif()

# advapi32 is the registry, and the registry is where a licensed build reads
# MachineGuid (sonore/license_runtime.h). Linked for EVERY Windows build, not
# just a GUI one: the licence gate is what the DSP path uses, and a headless
# marketplace build needs it exactly as much.
if(WIN32)
  target_link_libraries(sonore_sdk INTERFACE advapi32)
endif()

# ── The native interface ─────────────────────────────────────────────────────
# The plugin's face is the generated HTML in an OS webview. Windows uses Edge
# WebView2, whose SDK is fetched at configure time (see cmake/FetchWebView2.cmake)
# rather than vendored: the static loader is ~10 MB per architecture.
# SONORE_GUI=OFF builds a headless plugin (the DSP still works; the host shows
# its own generic controls), which is what CI without network wants.
option(SONORE_GUI "Build the native webview interface" ON)

if(SONORE_GUI AND WIN32)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchWebView2.cmake")
  # uiautomationcore is the accessibility provider (gfx/uia_win32.h) and
  # oleaut32 the BSTR/SAFEARRAY helpers it answers with. Both ship with
  # Windows. MSVC picks them up from #pragma comment(lib) in the header too;
  # this is here so a build with any other compiler works the same.
  target_link_libraries(sonore_sdk INTERFACE sonore_webview2 uiautomationcore oleaut32)
elseif(SONORE_GUI AND APPLE)
  # WKWebView is driven through the Objective-C runtime, so there is no .mm file
  # and no mixed-language build: just the frameworks.
  target_link_libraries(sonore_sdk INTERFACE
    "-framework WebKit" "-framework Cocoa" "-framework Foundation" objc)
elseif(SONORE_GUI AND UNIX)
  # GTK and WebKitGTK are dlopened at RUNTIME (see webview_gtk.h): webkit2gtk
  # 4.1 and 4.0 cannot share a process, and linking either would make the plugin
  # refuse to load on half the distributions. Only libdl is needed at link time,
  # which means the SDK also builds on a machine with no GTK dev packages.
  target_link_libraries(sonore_sdk INTERFACE ${CMAKE_DL_LIBS})
elseif(SONORE_GUI)
  message(STATUS "Sonore GUI: no webview backend for this platform yet: "
                 "plugins build headless (hosts show generic controls)")
endif()

# ── sonore_add_clap(<target> SOURCES a.cpp [b.cpp ...]) ──────────────────────
# A CLAP is a plain shared library with a .clap extension exporting clap_entry.
# That is the whole format: no framework runtime, no code generation step.
# ── The export table, decided by the linker rather than left to visibility ──
#
# -fvisibility=hidden hides OUR symbols. It does not hide libstdc++'s: the
# standard library marks its own templates default-visibility, so every plugin
# built with gcc still exported std::vector<float>::_M_fill_assign and the
# typeinfo of shared_ptr's control block -- tests/exports_test.cpp read them
# back out of every Linux binary. Two plugins in one host resolving each
# other's instantiations is harmless until one of them was built against a
# different libstdc++, and then it is not. A version script (a symbol list on
# macOS) makes the format's entry points the ONLY exports.
#
# EACH TARGET GETS ITS OWN LIST, and that is not tidiness. One shared list
# naming every format's entry points was written here on the belief that "a
# name in the list that a binary does not define is simply ignored". That is
# true of Apple's linker -- which is why macOS CI stayed green -- and FALSE of
# the cctools ld64 the osxcross cross-builder uses, where a listed name the
# object file does not define is a hard error:
#
#   Undefined symbols for architecture arm64:
#     "_GetPluginFactory", referenced from:
#        -exported_symbol[s_list] command line option
#
# So the first macOS build the farm ever ran failed to LINK the CLAP and the
# VST3 of a plugin that had compiled cleanly. A list must name exactly what
# its target defines, and the caller passes them because the caller is the
# format helper that knows. (The GNU version script does tolerate absent
# names, but there is no reason for the two branches to mean different
# things.) The file is per-target too: several formats share one binary dir.
function(_sonore_plugin_exports TARGET)
  if(NOT ARGN)
    message(FATAL_ERROR "_sonore_plugin_exports(${TARGET}): name the entry points")
  endif()
  if(APPLE)
    set(_list "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_exports.txt")
    set(_txt "")
    foreach(_s IN LISTS ARGN)
      string(APPEND _txt "_${_s}\n")
    endforeach()
    file(WRITE "${_list}" "${_txt}")
    target_link_options(${TARGET} PRIVATE "-Wl,-exported_symbols_list,${_list}")
  elseif(UNIX)
    set(_map "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_exports.map")
    string(JOIN "; " _globals ${ARGN})
    file(WRITE "${_map}" "{ global: ${_globals}; local: *; };\n")
    target_link_options(${TARGET} PRIVATE "-Wl,--version-script=${_map}")
  endif()
endfunction()

function(sonore_add_clap TARGET)
  cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "sonore_add_clap(${TARGET}): SOURCES is required")
  endif()

  add_library(${TARGET} MODULE ${ARG_SOURCES})
  target_link_libraries(${TARGET} PRIVATE sonore_sdk)
  set_target_properties(${TARGET} PROPERTIES
    PREFIX ""
    SUFFIX ".clap"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

  if(APPLE)
    # macOS wants a bundle; the host looks for Contents/MacOS/<name>.
    set_target_properties(${TARGET} PROPERTIES
      BUNDLE TRUE
      BUNDLE_EXTENSION "clap"
      MACOSX_BUNDLE_BUNDLE_NAME "${TARGET}"
      MACOSX_BUNDLE_GUI_IDENTIFIER "com.sonorie.${TARGET}")
    _sonore_plugin_exports(${TARGET} clap_entry)
  elseif(UNIX)
    # -Bsymbolic keeps our symbols ours when several plugins share a host process.
    target_link_options(${TARGET} PRIVATE -Wl,-Bsymbolic)
    _sonore_plugin_exports(${TARGET} clap_entry)
  endif()
endfunction()

# ── sonore_add_vst3(<target> SOURCES a.cpp [...]) ────────────────────────────
# A VST3 is a shared library exporting GetPluginFactory, shipped inside a bundle
# directory whose layout the host scans. The SAME sources as the CLAP target,

# ── sonore_add_rt_test(<target> SOURCES a.cpp) ───────────────────────────────
# The real-time safety audit. Unlike the other host tests this does NOT load a
# built artifact: it compiles the plugin's own source into the test binary, so
# that replacing global operator new sees the wrapper's and the DSP's
# allocations. A test that dlopen'd the plugin would see neither -- the module
# brings its own allocator -- and would pass while proving nothing.
function(sonore_add_rt_test TARGET)
  cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "sonore_add_rt_test(${TARGET}): SOURCES is required")
  endif()
  add_executable(${TARGET} tests/rt_safety_test.cpp ${ARG_SOURCES})
  target_link_libraries(${TARGET} PRIVATE sonore_sdk)
  # The GUI compiles in as it does for a real plugin, but is never created:
  # a window is main-thread work by contract and is not what is measured here.
endfunction()

# with SONORE_BUILD_VST3 defined: one DSP, two formats, no drift.
function(sonore_add_vst3 TARGET)
  cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "sonore_add_vst3(${TARGET}): SOURCES is required")
  endif()

  add_library(${TARGET} MODULE ${ARG_SOURCES})
  target_link_libraries(${TARGET} PRIVATE sonore_sdk)
  target_compile_definitions(${TARGET} PRIVATE SONORE_BUILD_VST3=1)
  target_include_directories(${TARGET} PRIVATE
    "${SONORE_SDK_DIR}/third_party/vst3")
  set_target_properties(${TARGET} PROPERTIES
    PREFIX ""
    SUFFIX ".vst3"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

  # The bundle layout hosts actually scan. Windows and Linux use an architecture
  # folder; macOS uses a real bundle with an Info.plist.
  # The folder name is the TARGET architecture, and the VST3 spec spells each
  # one: x86_64-win / arm64-win / x86-win, x86_64-linux / aarch64-linux / ...
  # A hardcoded x86_64 built on an ARM box lands the binary in a folder the
  # host never scans -- installed, correct, and invisible.
  if(APPLE)
    set(_arch "MacOS")
  else()
    _sonore_vst3_arch(_arch)
  endif()

  if(APPLE)
    set_target_properties(${TARGET} PROPERTIES
      BUNDLE TRUE
      BUNDLE_EXTENSION "vst3"
      MACOSX_BUNDLE_BUNDLE_NAME "${TARGET}"
      MACOSX_BUNDLE_GUI_IDENTIFIER "com.sonorie.${TARGET}")
    _sonore_plugin_exports(${TARGET} clap_entry GetPluginFactory bundleEntry bundleExit)
    _sonore_adhoc_sign(${TARGET})
  else()
    if(UNIX)
      target_link_options(${TARGET} PRIVATE -Wl,-Bsymbolic)
      _sonore_plugin_exports(${TARGET} clap_entry GetPluginFactory ModuleEntry ModuleExit)
    endif()
    # Assemble <target>.vst3/Contents/<arch>/<target>.vst3 beside the raw module,
    # so the build output can be dropped straight into a VST3 folder.
    set(_bundle "$<TARGET_FILE_DIR:${TARGET}>/${TARGET}.vst3.bundle/Contents/${_arch}")
    add_custom_command(TARGET ${TARGET} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_bundle}"
      COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${TARGET}>" "${_bundle}/${TARGET}.vst3"
      COMMENT "Assembling the ${TARGET} VST3 bundle")
  endif()
endfunction()

# ── _sonore_vst3_arch(<out-var>) ─────────────────────────────────────────────
# The VST3 bundle's Contents/<arch> folder for a Windows or Linux build, from
# the processor CMake is actually compiling for (which under cross-compilation
# is not the machine's). Names per the VST3 SDK's "plug-in format structure".
function(_sonore_vst3_arch OUT)
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _p)
  if(WIN32)
    if(_p MATCHES "^(arm64|aarch64)$")
      set(_a "arm64-win")
    elseif(_p MATCHES "^(arm64ec)$")
      set(_a "arm64ec-win")
    elseif(_p MATCHES "^(x86|i[3-6]86)$")
      set(_a "x86-win")
    else()
      set(_a "x86_64-win")
    endif()
  else()
    if(_p MATCHES "^(aarch64|arm64)$")
      set(_a "aarch64-linux")
    elseif(_p MATCHES "^arm")
      set(_a "armv7l-linux")
    elseif(_p MATCHES "^(i[3-6]86|x86)$")
      set(_a "i386-linux")
    else()
      set(_a "x86_64-linux")
    endif()
  endif()
  set(${OUT} "${_a}" PARENT_SCOPE)
endfunction()

# ── _sonore_adhoc_sign(<target>) ─────────────────────────────────────────────
# An AD-HOC code signature on a macOS BUNDLE. The linker already signs the
# Mach-O on Apple Silicon, but that is not the same seal: macOS validates the
# BUNDLE, and a bundle with no Contents/_CodeSignature is skipped in SILENCE.
# From the outside that is indistinguishable from a broken plugin -- the
# .component sits installed in ~/Library/Audio/Plug-Ins/Components, `auval -a`
# never lists it, and asking for it by type/subtype/manufacturer answers -50
# (paramErr) on the name and the version. That is the exact failure the first
# macOS CI run reported, with a correct Info.plist and a correct binary.
#
# `--sign -` is the ad-hoc identity: no certificate, no Apple account, no
# network, and enough for the registrar and for a host on the build machine.
# Shipping to OTHER people still needs a Developer ID signature and
# notarisation -- this replaces neither.
function(_sonore_adhoc_sign TARGET)
  if(NOT APPLE)
    return()
  endif()
  # codesign ships with macOS and exists nowhere else. Cross-building for
  # Darwin from Linux (builder/darwin) reached this POST_BUILD step and died
  # on `codesign: No such file or directory` -- make Error 127, which reports
  # a target that had linked perfectly as a build failure. On that path the
  # signature comes from rcodesign after the build (builder/worker.mjs), so
  # the absence is expected: say so out loud and step aside.
  find_program(SONORE_CODESIGN codesign)
  if(NOT SONORE_CODESIGN)
    message(STATUS "Sonore: no codesign on this machine (cross build) -- "
                   "${TARGET} is left for the builder to ad-hoc sign")
    return()
  endif()
  add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND "${SONORE_CODESIGN}" --force --sign - --timestamp=none
            "$<TARGET_BUNDLE_DIR:${TARGET}>"
    COMMENT "Ad-hoc signing the ${TARGET} bundle")
endfunction()

# ── sonore_add_au(<target> SOURCES a.cpp [...]) ──────────────────────────────
# An Audio Unit is a macOS bundle whose Info.plist advertises the component in
# an AudioComponents array; the host reads that array to find the factory
# function. Same sources again, with SONORE_BUILD_AU defined.
#
# Built and auval-validated by the SDK workflow (.github/workflows/sdk.yml) on
# a macOS runner since 2026-09-01. Written without a Mac to compile on, and the
# first green run was the START of the work: see the au_wrapper.h banner.
function(sonore_add_au TARGET)
  if(NOT APPLE)
    return()
  endif()
  cmake_parse_arguments(ARG "" "SUBTYPE;MANUFACTURER;TYPE" "SOURCES" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "sonore_add_au(${TARGET}): SOURCES is required")
  endif()
  if(NOT ARG_SUBTYPE)
    set(ARG_SUBTYPE "Snr1")
  endif()
  if(NOT ARG_MANUFACTURER)
    set(ARG_MANUFACTURER "Sonr")
  endif()
  # aufx = effect, aumu = instrument. Hosts file the two in different menus,
  # so an instrument declared as an effect is simply never found by a player.
  if(NOT ARG_TYPE)
    set(ARG_TYPE "aufx")
  endif()

  add_library(${TARGET} MODULE ${ARG_SOURCES})
  # CoreMIDI is here for the MIDI-OUTPUT path: au_wrapper.h builds a
  # MIDIPacketList for a plugin that produces MIDI (the arp example does), and
  # MIDIPacketListInit/Add are real symbols in that framework, not macros.
  target_link_libraries(${TARGET} PRIVATE sonore_sdk
    "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreAudio"
    "-framework CoreFoundation" "-framework CoreMIDI")
  target_compile_definitions(${TARGET} PRIVATE SONORE_BUILD_AU=1)
  set_target_properties(${TARGET} PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "component"
    XCODE_ATTRIBUTE_WRAPPER_EXTENSION "component"
    MACOSX_BUNDLE_BUNDLE_NAME "${TARGET}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.sonorie.${TARGET}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}-Info.plist")

  # The AudioComponents entry IS the registration: a host that cannot read it
  # never discovers the plugin, however correct the code is.
  set(SONORE_AU_NAME "${TARGET}")
  set(SONORE_AU_SUBTYPE "${ARG_SUBTYPE}")
  set(SONORE_AU_MANUFACTURER "${ARG_MANUFACTURER}")
  set(SONORE_AU_TYPE "${ARG_TYPE}")
  configure_file("${SONORE_SDK_DIR}/cmake/AudioUnit-Info.plist.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}-Info.plist" @ONLY)

  # Without this the component is installed and never registered -- see
  # _sonore_adhoc_sign for what that looks like in an auval log.
  _sonore_adhoc_sign(${TARGET})
endfunction()

# ── sonore_add_standalone(<target> SOURCES a.cpp [...]) ──────────────────────
# A runnable app from the same plugin source: window + webview faceplate + OS
# audio. Also the CI-friendly offline modes (--verify / --render out.wav),
# which need no audio device and no display.
function(sonore_add_standalone TARGET)
  cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "sonore_add_standalone(${TARGET}): SOURCES is required")
  endif()

  add_executable(${TARGET} ${ARG_SOURCES})
  target_link_libraries(${TARGET} PRIVATE sonore_sdk)
  target_compile_definitions(${TARGET} PRIVATE SONORE_BUILD_STANDALONE=1)

  if(WIN32)
    # A console app on purpose: the offline modes print their verdicts, and an
    # interactive run simply also has a console. WASAPI needs COM.
    target_link_libraries(${TARGET} PRIVATE ole32)
  elseif(APPLE)
    # CoreMIDI for the live MIDI in/out devices the standalone opens; the
    # Linux equivalent (ALSA) is dlopened, which is why only this platform
    # needs the framework named here.
    target_link_libraries(${TARGET} PRIVATE
      "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreAudio"
      "-framework CoreMIDI" "-framework Cocoa" objc)
  elseif(UNIX)
    # ALSA and GTK are dlopened; only dl and threads are linked.
    target_link_libraries(${TARGET} PRIVATE ${CMAKE_DL_LIBS} pthread)
  endif()
endfunction()

# ── sonore_add_lv2(<target> SOURCES a.cpp [...]) ─────────────────────────────
# An LV2 bundle: <target>.lv2/ holding the binary plus manifest.ttl and
# plugin.ttl. The TTLs are GENERATED by a companion executable built from the
# same source, so the metadata can never disagree with connect_port(), which
# in a real host is a crash, not a warning.
function(sonore_add_lv2 TARGET)
  cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "sonore_add_lv2(${TARGET}): SOURCES is required")
  endif()

  set(_bundle "$<TARGET_FILE_DIR:${TARGET}>/${TARGET}.lv2")

  add_library(${TARGET} MODULE ${ARG_SOURCES})
  target_link_libraries(${TARGET} PRIVATE sonore_sdk)
  target_compile_definitions(${TARGET} PRIVATE SONORE_BUILD_LV2=1)
  target_include_directories(${TARGET} PRIVATE "${SONORE_SDK_DIR}/third_party/lv2/include")
  set_target_properties(${TARGET} PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  if(UNIX AND NOT APPLE)
    target_link_options(${TARGET} PRIVATE -Wl,-Bsymbolic)
    _sonore_plugin_exports(${TARGET} clap_entry lv2_descriptor lv2ui_descriptor)
  endif()

  add_executable(${TARGET}_ttlgen ${ARG_SOURCES})
  target_link_libraries(${TARGET}_ttlgen PRIVATE sonore_sdk)
  target_compile_definitions(${TARGET}_ttlgen PRIVATE SONORE_BUILD_LV2=1 SONORE_LV2_TTLGEN=1)
  target_include_directories(${TARGET}_ttlgen PRIVATE "${SONORE_SDK_DIR}/third_party/lv2/include")

  add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_bundle}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${TARGET}>"
            "${_bundle}/$<TARGET_FILE_NAME:${TARGET}>"
    COMMAND ${TARGET}_ttlgen "${_bundle}" "$<TARGET_FILE_NAME:${TARGET}>"
    COMMENT "Assembling the ${TARGET}.lv2 bundle")
  add_dependencies(${TARGET} ${TARGET}_ttlgen)
endfunction()

# ── sonore_add_aax(<target> ...): a hook, not an implementation ─────────────
# AAX cannot be built here and pretending otherwise would be worse than absent:
# Avid's SDK is distributed only under their developer agreement (there is no
# public header, unlike VST3's C ABI), and shipping binaries must be signed
# with PACE tools. When the SDK exists locally, set SONORE_AAX_SDK_ROOT and
# implement the wrapper against it; until then this function says exactly why
# it is skipping, so nobody mistakes silence for support.
function(sonore_add_aax TARGET)
  if(NOT DEFINED SONORE_AAX_SDK_ROOT OR NOT EXISTS "${SONORE_AAX_SDK_ROOT}")
    message(STATUS "sonore_add_aax(${TARGET}): SKIPPED: the AAX SDK requires an "
                   "Avid developer agreement (developer.avid.com) and is not present. "
                   "Set SONORE_AAX_SDK_ROOT once obtained.")
    return()
  endif()
  message(FATAL_ERROR "sonore_add_aax(${TARGET}): SONORE_AAX_SDK_ROOT is set but the "
                      "AAX wrapper is not implemented yet: implement it against the "
                      "SDK now that it is available.")
endfunction()

# ── VST2 ─────────────────────────────────────────────────────────────────────
# There is no VST2 target and there must not be one. Steinberg ENDED VST2
# licensing in October 2018: the SDK is no longer distributed, and no plugin
# licensed after that date may legally ship in VST2 form. Frameworks that
# "support VST2" support it only for holders of the pre-2018 SDK.
#
# This is a legal wall, not a missing feature, and it is encoded here so that
# nobody adds it later mistaking its absence for an oversight. Every host that
# still loads VST2 also loads VST3 or CLAP.
function(sonore_add_vst2 TARGET)
  message(FATAL_ERROR
          "sonore_add_vst2(${TARGET}): VST2 is deliberately unsupported. Steinberg ended "
          "VST2 licensing in October 2018 and no longer distributes the SDK, so a plugin "
          "licensed after that date cannot legally ship as VST2. Use VST3 or CLAP.")
endfunction()

# ── AUv3 ─────────────────────────────────────────────────────────────────────
# AUv2 (sonore_add_au) is what Logic, Live, Cubase and every macOS DESKTOP host
# actually load, and it is what we build. AUv3 is a different thing: an app
# EXTENSION (.appex) shipped inside a containing application, which is what iOS
# requires and what sandboxed / App Store distribution requires.
#
# It cannot be built from CMake alone: it needs an Xcode extension target, an
# NSExtension Info.plist and a host application to embed it, and it cannot be
# compile-checked anywhere but a Mac. So the hook skips LOUDLY rather than
# pretending, exactly like AAX and ASIO.
function(sonore_add_auv3 TARGET)
  if(NOT APPLE)
    return()
  endif()
  if(NOT DEFINED SONORE_AUV3_CONTAINER_APP)
    message(STATUS "sonore_add_auv3(${TARGET}): SKIPPED: AUv3 is an app EXTENSION and "
                   "needs a containing application plus an Xcode extension target. "
                   "AUv2 (sonore_add_au) already covers every macOS desktop host; AUv3 "
                   "matters for iOS and App Store distribution. Set "
                   "SONORE_AUV3_CONTAINER_APP once that container exists.")
    return()
  endif()
  message(FATAL_ERROR "sonore_add_auv3(${TARGET}): SONORE_AUV3_CONTAINER_APP is set but the "
                      "AUv3 extension target is not implemented yet: implement it against "
                      "a real Mac now that there is somewhere to embed it.")
endfunction()

# ── ARA (Audio Random Access) ────────────────────────────────────────────────
# Celemony's ARA SDK is APACHE 2.0: permissive, and compatible with everything
# this SDK stands for. So unlike AAX, ARA is not blocked by anyone's lawyers:
# it is simply a large API that only earns its keep for a plugin that reads and
# writes the host's TIMELINE (Melodyne's category: pitch correction, stem
# separation, transcription). A saturator or a synth has nothing to say to it.
#
# The reason it is a hook rather than an implementation is verification, not
# licensing: an ARA plugin can only be proven inside an ARA host (Studio One,
# Logic, Cakewalk), and shipping thousands of lines nobody can run is how the
# two macOS bugs in this SDK's history got written. Implement it when there is
# a plugin type that needs it AND a host to prove it in.
function(sonore_add_ara TARGET)
  if(NOT DEFINED SONORE_ARA_SDK_ROOT OR NOT EXISTS "${SONORE_ARA_SDK_ROOT}")
    message(STATUS "sonore_add_ara(${TARGET}): SKIPPED: the ARA SDK (Apache 2.0, "
                   "github.com/Celemony/ARA_SDK) is not present. It matters only for "
                   "timeline-aware plugins; set SONORE_ARA_SDK_ROOT when building one.")
    return()
  endif()
  message(FATAL_ERROR "sonore_add_ara(${TARGET}): SONORE_ARA_SDK_ROOT is set but the ARA "
                      "binding is not implemented yet: implement it against the SDK, and "
                      "prove it in a real ARA host before shipping.")
endfunction()

# ── Plugin HOSTING ───────────────────────────────────────────────────────────
# Out of scope by design. Sonorie generates plugins; it does not load other
# people's. Hosting means implementing the host side of every format, sandboxing
# third-party code and inheriting its crashes: a different product, not a
# missing feature of this one.

# ── ASIO ─────────────────────────────────────────────────────────────────────
# Windows pro users expect ASIO in a standalone, but Steinberg's ASIO SDK
# cannot be redistributed: it comes with a licence agreement each developer
# accepts themselves. Same treatment as AAX: the hook is here, the code is
# written only against the real SDK, and the absence is LOUD rather than a
# silently missing feature. Meanwhile WASAPI in shared mode is what the
# standalone uses, which is fine for auditioning and wrong for tracking.
function(sonore_enable_asio TARGET)
  if(NOT WIN32)
    return()
  endif()
  if(NOT DEFINED SONORE_ASIO_SDK_ROOT OR NOT EXISTS "${SONORE_ASIO_SDK_ROOT}")
    message(STATUS "sonore_enable_asio(${TARGET}): SKIPPED: Steinberg's ASIO SDK is not "
                   "redistributable and is not present. Download it from "
                   "steinberg.net/developers and set SONORE_ASIO_SDK_ROOT to enable "
                   "low-latency ASIO output; WASAPI is used until then.")
    return()
  endif()
  message(FATAL_ERROR "sonore_enable_asio(${TARGET}): SONORE_ASIO_SDK_ROOT is set but the "
                      "ASIO backend is not implemented yet: implement it against the SDK "
                      "now that it is available.")
endfunction()

# The example builds only when the SDK is the top-level project, so a generated
# plugin embedding the SDK never pays for it.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  sonore_add_clap(SonoreSaturator SOURCES examples/saturator/plugin.cpp)
  sonore_add_clap(SonoreSynth SOURCES examples/synth/plugin.cpp)
  sonore_add_clap(SonoreDucker SOURCES examples/ducker/plugin.cpp)
  sonore_add_clap(SonoreTrim SOURCES examples/trim/plugin.cpp)
  sonore_add_clap(SonoreSplitter SOURCES examples/splitter/plugin.cpp)
  sonore_add_clap(SonoreArp SOURCES examples/arp/plugin.cpp)
  sonore_add_clap(SonoreSampler SOURCES examples/sampler/plugin.cpp)
  sonore_add_clap(SonoreReverb SOURCES examples/reverb/plugin.cpp)

  enable_testing()

  # Not a ctest: it measures, and a timing assertion on shared CI hardware is
  # a flaky test rather than a useful one. Run it by hand, and run it twice:
  # once built -DSONORE_NO_SIMD: to compare both speed AND checksums.
  add_executable(simd_bench tests/simd_bench.cpp)
  target_link_libraries(simd_bench PRIVATE sonore_sdk)

  add_executable(sonore_sdk_tests tests/sdk_tests.cpp)
  target_link_libraries(sonore_sdk_tests PRIVATE sonore_sdk)
  # thread_pool.h uses std::thread, which on glibc needs pthread at LINK time
  # even though the header compiles without it. MSVC needs nothing.
  find_package(Threads REQUIRED)
  target_link_libraries(sonore_sdk_tests PRIVATE Threads::Threads)
  target_compile_definitions(sonore_sdk_tests
    PRIVATE SONORE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  target_include_directories(sonore_sdk_tests PRIVATE "${SONORE_SDK_DIR}/third_party/vst3")
  if(APPLE)
    # The MIDI-output tests reach CoreMIDI (MIDIClientCreate and friends), and
    # the file dialog reaches AppKit through the Objective-C runtime. Neither
    # is dlopened the way the Linux backends are, so both must be LINKED.
    target_link_libraries(sonore_sdk_tests PRIVATE
      "-framework CoreMIDI" "-framework CoreFoundation" "-framework AppKit" objc)
  endif()
  add_test(NAME sdk_tests COMMAND sonore_sdk_tests)

  # ── Windows on Arm, parsed ─────────────────────────────────────────────────
  # No ARM64 CRT is installed on the box that develops this, so nothing here
  # can LINK for Windows on Arm -- and the denormal guard shipped GNU inline
  # assembly under _M_ARM64, which cl.exe rejects outright, for as long as
  # nothing compiled for that target. clang-cl parses for any architecture it
  # was built with, so whenever it IS the compiler (the build farm's, and the
  # Windows-clangcl CI job) every translation unit that reaches a
  # platform-specific line is parsed for aarch64-pc-windows-msvc as a test.
  # See tests/arm64_syntax.cmake for what that proves and what it cannot.
  if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"
     AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]64")
    get_target_property(_arm64_inc sonore_sdk INTERFACE_INCLUDE_DIRECTORIES)
    set(_arm64_defs "_CRT_SECURE_NO_WARNINGS")
    if(TARGET sonore_webview2)
      get_target_property(_wv2_inc sonore_webview2 INTERFACE_INCLUDE_DIRECTORIES)
      list(APPEND _arm64_inc ${_wv2_inc})
      list(APPEND _arm64_defs "SONORE_HAS_WEBVIEW2=1")
    endif()
    add_test(NAME windows_arm64_syntax
      COMMAND "${CMAKE_COMMAND}" "-DCXX=${CMAKE_CXX_COMPILER}"
              "-DSDK=${CMAKE_CURRENT_SOURCE_DIR}" "-DINCLUDES=${_arm64_inc}"
              "-DDEFS=${_arm64_defs}"
              -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/arm64_syntax.cmake")
  endif()

  # Every parser that reads bytes it did not write, under mutation. The quick
  # pass here is seconds; scripts/sdk-sanitize.mjs runs the same binary at
  # depth under ASan+UBSan, which is where it found what it has found.
  add_executable(fuzz_parsers tests/fuzz_parsers.cpp)
  target_link_libraries(fuzz_parsers PRIVATE sonore_sdk)
  target_compile_definitions(fuzz_parsers
    PRIVATE SONORE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  target_include_directories(fuzz_parsers PRIVATE
    "${SONORE_SDK_DIR}/third_party/vst3" "${SONORE_SDK_DIR}/third_party/lv2/include")
  if(UNIX AND NOT APPLE)
    target_link_libraries(fuzz_parsers PRIVATE dl pthread)
  endif()
  add_test(NAME fuzz_parsers COMMAND fuzz_parsers)

  # The sampler example's sample handover, with a loading thread and a
  # rendering thread running against each other. Compiles the example's DSP
  # into the test (SONORE_EXAMPLE_DSP_ONLY), the way rt_safety_test does.
  add_executable(sampler_stress tests/sampler_stress_test.cpp)
  target_link_libraries(sampler_stress PRIVATE sonore_sdk)
  if(UNIX AND NOT APPLE)
    target_link_libraries(sampler_stress PRIVATE dl pthread)
  endif()
  add_test(NAME sampler_handover COMMAND sampler_stress 3)

  # The two threads a plugin lives on, against each other: state loads,
  # preset loads, parameter reads, render-mode flips and interface edits on
  # the main thread while process() runs. The saturator is compiled INTO the
  # test, like rt_safety_test, so nothing hides behind a module boundary.
  # Three seconds here; scripts/sdk-sanitize.mjs runs it under TSan and ASan.
  # (One source: the test includes the example's plugin.cpp.)
  add_executable(concurrency_test tests/concurrency_test.cpp)
  target_link_libraries(concurrency_test PRIVATE sonore_sdk)
  if(UNIX AND NOT APPLE)
    target_link_libraries(concurrency_test PRIVATE dl pthread)
  endif()
  add_test(NAME concurrency_clap COMMAND concurrency_test 3)

  # Dependency-free licence verification: proves sonore/license.h accepts what
  # Sonorie server signs (fixed vectors minted by the server's own code) and
  # rejects wrong email/product/machine/expired. If it fails, the plugin and the
  # server have drifted and licensed plugins would reject valid keys.
  add_executable(license_test tests/license_test.cpp)
  target_link_libraries(license_test PRIVATE sonore_sdk)
  add_test(NAME license COMMAND license_test)

  # Loads a BUILT .clap the way a DAW does, so what is tested is the shipped
  # artifact rather than a second build of the same source.
  add_executable(clap_host_test tests/clap_host_test.cpp)
  target_link_libraries(clap_host_test PRIVATE sonore_sdk)
  if(UNIX AND NOT APPLE)
    target_link_libraries(clap_host_test PRIVATE dl)
  endif()
  add_test(NAME clap_effect COMMAND clap_host_test $<TARGET_FILE:SonoreSaturator>
           --expect-web-editor)
  add_test(NAME clap_instrument COMMAND clap_host_test $<TARGET_FILE:SonoreSynth>
           --expect-native-editor)
  add_test(NAME clap_sidechain COMMAND clap_host_test $<TARGET_FILE:SonoreDucker>
                                       --expect-sidechain)
  add_test(NAME clap_channels COMMAND clap_host_test $<TARGET_FILE:SonoreTrim>
                                      --expect-channels 1 8)
  add_test(NAME clap_aux_outs COMMAND clap_host_test $<TARGET_FILE:SonoreSplitter>
                                      --expect-aux-outs 2)
  add_test(NAME clap_midi_out COMMAND clap_host_test $<TARGET_FILE:SonoreArp>
                                      --expect-midi-out)
  add_test(NAME clap_sampler COMMAND clap_host_test $<TARGET_FILE:SonoreSampler>)
  add_test(NAME clap_reverb COMMAND clap_host_test $<TARGET_FILE:SonoreReverb>)

  # ── Hosting ────────────────────────────────────────────────────────────────
  # sonore/host.h pointed at our own build output: it scans, loads, drives and
  # round-trips state on every .clap the build produced.
  add_executable(host_test tests/host_test.cpp)
  target_link_libraries(host_test PRIVATE sonore_sdk)
  target_include_directories(host_test PRIVATE "${SONORE_SDK_DIR}/third_party/lv2/include")
  # The VST3 half of host.h compiles only where Steinberg's C API is reachable.
  target_include_directories(host_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3)
  # The folder that CONTAINS the plugins, which is NOT TARGET_FILE_DIR on
  # macOS: a .clap there is a BUNDLE, so that expression points INSIDE it
  # (X.clap/Contents/MacOS) and the scan sees one bare binary and no plugins
  # at all. It worked on Windows and Linux because a .clap is a plain file
  # there -- and those were the only platforms that ever ran these tests.
  if(CMAKE_CONFIGURATION_TYPES)
    set(SONORE_PLUGIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
  else()
    set(SONORE_PLUGIN_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  add_test(NAME hosting COMMAND host_test "${SONORE_PLUGIN_DIR}")

  # What every built plugin EXPORTS, read from the binary: the entry points
  # its format needs and nothing else. Hidden visibility and -Bsymbolic are
  # flags, and a flag that stops being applied fails silently -- two plugins
  # in one host resolving each other's inline descriptor -- so the artifact is
  # read back rather than the flag trusted.
  add_executable(exports_test tests/exports_test.cpp)
  target_compile_features(exports_test PRIVATE cxx_std_17)
  add_test(NAME exported_symbols COMMAND exports_test "${SONORE_PLUGIN_DIR}")

  # The editor opened and closed until a leak would show. WebView2's COM
  # objects leaked on every open once, found by reading; this measures the
  # process's handles and private bytes across cycles instead.
  add_executable(editor_soak_test tests/editor_soak_test.cpp)
  target_link_libraries(editor_soak_test PRIVATE sonore_sdk)
  target_include_directories(editor_soak_test PRIVATE "${SONORE_SDK_DIR}/third_party/lv2/include")
  target_include_directories(editor_soak_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3)
  if(WIN32)
    target_link_libraries(editor_soak_test PRIVATE psapi)
    add_test(NAME editor_soak COMMAND editor_soak_test "${SONORE_PLUGIN_DIR}/SonoreGuiProbe.clap" 12)
  endif()

  # Render a file through a plugin with our own host: the reference half of
  # scripts/daw-render.mjs, which renders the same file in REAPER and compares.
  add_executable(render_tool tests/render_tool.cpp)
  target_link_libraries(render_tool PRIVATE sonore_sdk)
  target_include_directories(render_tool PRIVATE "${SONORE_SDK_DIR}/third_party/lv2/include")
  target_include_directories(render_tool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3)

  # The plugin graph, built from our own plugins: a chain, a parallel split,
  # and the delay compensation that is the whole reason it is a graph.
  add_executable(graph_test tests/graph_test.cpp)
  target_link_libraries(graph_test PRIVATE sonore_sdk)
  target_include_directories(graph_test PRIVATE "${SONORE_SDK_DIR}/third_party/lv2/include")
  add_test(NAME graph COMMAND graph_test "${SONORE_PLUGIN_DIR}")
  set_tests_properties(hosting PROPERTIES DEPENDS "clap_effect")

  # ── Real-time safety ───────────────────────────────────────────────────────
  # One binary per plugin, each compiling that plugin's source into itself so
  # global operator new can count what the wrapper and the DSP allocate.
  sonore_add_rt_test(rt_saturator SOURCES examples/saturator/plugin.cpp)
  sonore_add_rt_test(rt_synth     SOURCES examples/synth/plugin.cpp)
  sonore_add_rt_test(rt_ducker    SOURCES examples/ducker/plugin.cpp)
  sonore_add_rt_test(rt_splitter  SOURCES examples/splitter/plugin.cpp)
  sonore_add_rt_test(rt_arp       SOURCES examples/arp/plugin.cpp)
  sonore_add_rt_test(rt_sampler   SOURCES examples/sampler/plugin.cpp)
  sonore_add_rt_test(rt_reverb    SOURCES examples/reverb/plugin.cpp)
  # trim is the one example with a 64-bit path, so it is the one that makes the
  # audit's double-precision leg run at all; guiprobe is here because a fixture
  # that allocates on the audio thread would poison every GUI test that uses it.
  sonore_add_rt_test(rt_trim      SOURCES examples/trim/plugin.cpp)
  sonore_add_rt_test(rt_guiprobe  SOURCES examples/guiprobe/plugin.cpp)
  foreach(rt saturator synth ducker splitter arp sampler reverb trim guiprobe)
    add_test(NAME rt_safety_${rt} COMMAND rt_${rt})
  endforeach()

  # The SAME sources, built as VST3 and driven by a VST3 host. Normalised
  # parameters, COM refcounting and bus arrangements are where a wrapper goes
  # silently wrong, so each is asserted against the shipped artifact.
  sonore_add_vst3(SonoreSaturatorVST3 SOURCES examples/saturator/plugin.cpp)
  sonore_add_vst3(SonoreSynthVST3 SOURCES examples/synth/plugin.cpp)
  sonore_add_vst3(SonoreDuckerVST3 SOURCES examples/ducker/plugin.cpp)
  sonore_add_vst3(SonoreTrimVST3 SOURCES examples/trim/plugin.cpp)
  sonore_add_vst3(SonoreSplitterVST3 SOURCES examples/splitter/plugin.cpp)
  sonore_add_vst3(SonoreArpVST3 SOURCES examples/arp/plugin.cpp)
  sonore_add_vst3(SonoreSamplerVST3 SOURCES examples/sampler/plugin.cpp)
  sonore_add_vst3(SonoreReverbVST3 SOURCES examples/reverb/plugin.cpp)

  # macOS only; built and auval-validated by the SDK workflow on a Mac.
  sonore_add_au(SonoreSaturatorAU SUBTYPE "Sat1" SOURCES examples/saturator/plugin.cpp)
  sonore_add_au(SonoreSynthAU SUBTYPE "Syn1" TYPE "aumu" SOURCES examples/synth/plugin.cpp)
  sonore_add_au(SonoreDuckerAU SUBTYPE "Duk1" SOURCES examples/ducker/plugin.cpp)
  sonore_add_au(SonoreTrimAU SUBTYPE "Trm1" SOURCES examples/trim/plugin.cpp)
  sonore_add_au(SonoreSplitterAU SUBTYPE "Spl1" SOURCES examples/splitter/plugin.cpp)
  sonore_add_au(SonoreArpAU SUBTYPE "Arp1" TYPE "aumu" SOURCES examples/arp/plugin.cpp)
  sonore_add_au(SonoreSamplerAU SUBTYPE "Smp1" TYPE "aumu" SOURCES examples/sampler/plugin.cpp)
  sonore_add_au(SonoreReverbAU SUBTYPE "Rvb1" TYPE "aufx" SOURCES examples/reverb/plugin.cpp)
  sonore_add_auv3(SonoreSaturatorAUv3)

  add_executable(vst3_host_test tests/vst3_host_test.cpp)
  target_link_libraries(vst3_host_test PRIVATE sonore_sdk)
  target_include_directories(vst3_host_test PRIVATE "${SONORE_SDK_DIR}/third_party/vst3")
  if(UNIX AND NOT APPLE)
    target_link_libraries(vst3_host_test PRIVATE dl)
  endif()
  add_test(NAME vst3_effect COMMAND vst3_host_test $<TARGET_FILE:SonoreSaturatorVST3>)
  add_test(NAME vst3_instrument COMMAND vst3_host_test $<TARGET_FILE:SonoreSynthVST3>
                                        --expect-mpe
           --expect-native-editor)
  add_test(NAME vst3_sidechain COMMAND vst3_host_test $<TARGET_FILE:SonoreDuckerVST3>
                                       --expect-sidechain)
  add_test(NAME vst3_channels COMMAND vst3_host_test $<TARGET_FILE:SonoreTrimVST3>
                                      --expect-channels 1 8)
  add_test(NAME vst3_aux_outs COMMAND vst3_host_test $<TARGET_FILE:SonoreSplitterVST3>
                                      --expect-aux-outs 2)
  add_test(NAME vst3_midi_out COMMAND vst3_host_test $<TARGET_FILE:SonoreArpVST3>
                                      --expect-midi-out)
  add_test(NAME vst3_sampler COMMAND vst3_host_test $<TARGET_FILE:SonoreSamplerVST3>
                                     --expect-mpe)
  add_test(NAME vst3_reverb COMMAND vst3_host_test $<TARGET_FILE:SonoreReverbVST3>)

  # The standalone, judged by its own offline render: no device, no display,
  # so these run anywhere ctest does.
  sonore_add_standalone(SonoreSaturatorApp SOURCES examples/saturator/plugin.cpp)
  sonore_add_standalone(SonoreSynthApp SOURCES examples/synth/plugin.cpp)
  # The arpeggiator: the only example whose output is NOTES, and therefore the
  # only one that proves the standalone can play them anywhere.
  sonore_add_standalone(SonoreArpApp SOURCES examples/arp/plugin.cpp)
  sonore_enable_asio(SonoreSaturatorApp)
  sonore_enable_asio(SonoreSynthApp)

  add_test(NAME standalone_effect COMMAND SonoreSaturatorApp --verify)
  add_test(NAME standalone_instrument COMMAND SonoreSynthApp --verify)
  add_test(NAME standalone_midi_out COMMAND SonoreArpApp --verify)

  # LV2, judged like every other format: the BUILT bundle, driven by a host.
  sonore_add_lv2(SonoreSaturatorLV2 SOURCES examples/saturator/plugin.cpp)
  sonore_add_lv2(SonoreSynthLV2 SOURCES examples/synth/plugin.cpp)
  sonore_add_lv2(SonoreDuckerLV2 SOURCES examples/ducker/plugin.cpp)
  sonore_add_lv2(SonoreSplitterLV2 SOURCES examples/splitter/plugin.cpp)
  sonore_add_lv2(SonoreArpLV2 SOURCES examples/arp/plugin.cpp)
  sonore_add_lv2(SonoreSamplerLV2 SOURCES examples/sampler/plugin.cpp)
  sonore_add_lv2(SonoreReverbLV2 SOURCES examples/reverb/plugin.cpp)
  # The probe as LV2 too: it is the one plugin that REPORTS what it was told,
  # which is the only way to prove lv2:freeWheeling reaches a DSP.
  sonore_add_lv2(SonoreGuiProbeLV2 SOURCES examples/guiprobe/plugin.cpp)

  add_executable(lv2_host_test tests/lv2_host_test.cpp)
  target_link_libraries(lv2_host_test PRIVATE sonore_sdk)
  target_include_directories(lv2_host_test PRIVATE "${SONORE_SDK_DIR}/third_party/lv2/include")
  if(UNIX AND NOT APPLE)
    target_link_libraries(lv2_host_test PRIVATE dl)
  endif()
  add_test(NAME lv2_effect
           COMMAND lv2_host_test "$<TARGET_FILE_DIR:SonoreSaturatorLV2>/SonoreSaturatorLV2.lv2")
  add_test(NAME lv2_instrument
           COMMAND lv2_host_test "$<TARGET_FILE_DIR:SonoreSynthLV2>/SonoreSynthLV2.lv2")
  add_test(NAME lv2_sidechain COMMAND lv2_host_test
                                      $<TARGET_FILE_DIR:SonoreDuckerLV2>/SonoreDuckerLV2.lv2
                                      --expect-sidechain)
  add_test(NAME lv2_aux_outs COMMAND lv2_host_test
                                     $<TARGET_FILE_DIR:SonoreSplitterLV2>/SonoreSplitterLV2.lv2
                                     --expect-aux-outs 2)
  add_test(NAME lv2_midi_out COMMAND lv2_host_test
                                     $<TARGET_FILE_DIR:SonoreArpLV2>/SonoreArpLV2.lv2
                                     --expect-midi-out)
  # The LV2 interface, embedded in a real window and driven by a real message
  # loop -- the same shape as the CLAP and VST3 GUI bridge tests.
  add_executable(lv2_ui_test tests/lv2_ui_test.cpp)
  target_link_libraries(lv2_ui_test PRIVATE sonore_sdk)
  add_test(NAME lv2_ui_bridge COMMAND lv2_ui_test
                                      $<TARGET_FILE_DIR:SonoreGuiProbeLV2>/SonoreGuiProbeLV2.lv2)

  add_test(NAME lv2_freewheel COMMAND lv2_host_test
                                      $<TARGET_FILE_DIR:SonoreGuiProbeLV2>/SonoreGuiProbeLV2.lv2
                                      --expect-probe)

  # The GUI probe's page drives a parameter on load, so this test fails unless
  # the ENTIRE bridge works: a silently blank webview passes every contract
  # check but not this one. Windows only for now: it is the only backend.
  if(SONORE_GUI AND WIN32)
    sonore_add_clap(SonoreGuiProbe SOURCES examples/guiprobe/plugin.cpp)
    add_test(NAME clap_gui_bridge
             COMMAND clap_host_test $<TARGET_FILE:SonoreGuiProbe> --expect-bridge)

    # The same proof for VST3: the editor attaches to a real window, the page
    # drives a parameter, and the host hears about it through IComponentHandler.
    sonore_add_vst3(SonoreGuiProbeVST3 SOURCES examples/guiprobe/plugin.cpp)
    add_test(NAME vst3_gui_bridge
             COMMAND vst3_host_test $<TARGET_FILE:SonoreGuiProbeVST3> --expect-bridge)

    # Photographs a built plugin's real interface. Not a ctest: it needs a
    # desktop session, and its output is for a human to look at.
    add_executable(gui_shot tests/gui_shot.cpp)
    target_link_libraries(gui_shot PRIVATE sonore_sdk)

    # The webview resource-usage instrument. Like simd_bench it is NOT a
    # ctest: memory numbers on shared CI hardware are noise with a pass mark.
    # Run it by hand, before and after a change, against the built .claps:
    #   webview_bench Release/SonoreSaturator.clap Release/SonoreGuiProbe.clap 6
    add_executable(webview_bench tests/webview_bench.cpp)
    target_link_libraries(webview_bench PRIVATE sonore_sdk psapi)
  endif()

  # The native UI's window, driven with real Win32 messages.
  #
  # A ctest, unlike gui_shot: it creates its window WITHOUT WS_VISIBLE and
  # posts its own input, so it needs no desktop session and nothing appears on
  # screen. Everything else about the native UI is arithmetic and lives in
  # sdk_tests; this is the part where only the operating system can say
  # whether it works -- and the first run proved it, by catching a release
  # order that cancelled every click before its mouse-up arrived.
  #
  # WIN32-guarded, and it has to be: it includes window_win32.h, which #errors
  # on anything else, and it links gdi32/user32/shell32. It sat outside the
  # guard above and so was declared on every platform -- which nothing noticed
  # while Linux CI was not running.
  if(WIN32)
    add_executable(native_window_test tests/native_window_test.cpp)
    target_link_libraries(native_window_test PRIVATE sonore_sdk gdi32 user32 shell32)
    add_test(NAME native_window COMMAND native_window_test)
  endif()
endif()

# The demo executable and the ASIO probe are SDK-internal, like the examples and
# tests above -- a plugin that add_subdirectory()'s the SDK must never build them
# (their sources aren't even vendored into an export). So they live under the
# same top-level guard: they exist only when the SDK is the project being built.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # The native UI, rendered to a file so a person can LOOK at it.
  #
  # Not a test and deliberately not one: every rule the widgets follow is
  # asserted in sdk_tests against pixels, and no assertion answers "does this
  # look like something a musician would open". That question needs eyes, and
  # eyes need an image.
  add_executable(gfx_demo tests/gfx_demo.cpp)
  target_link_libraries(gfx_demo PRIVATE sonore_sdk)

  # ASIO, which only exists on Windows and only matters for the standalone.
  #
  # A ctest rather than a tool, and one that PASSES on a machine with no ASIO
  # drivers: enumerating zero of them is a correct answer, and a test that
  # failed there would fail on every CI runner and teach people to ignore it.
  if(WIN32)
    add_executable(asio_probe tests/asio_probe.cpp)
    target_link_libraries(asio_probe PRIVATE sonore_sdk ole32)
    add_test(NAME asio_drivers COMMAND asio_probe)
  endif()
endif()
