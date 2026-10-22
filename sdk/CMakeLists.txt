# SPDX-License-Identifier: Apache-2.0
# Sonore SDK: our own audio plugin framework.
#
# No framework dependency. The third-party code is vendored under third_party/:
# the CLAP, VST3 (C ABI) and LV2 headers, and the minimp3 and stb_vorbis
# decoders (see THIRD_PARTY_NOTICES.md). Everything in include/sonore is ours.
#
#   add_subdirectory(sdk)
#   sonore_add_clap(MyPlugin SOURCES plugin.cpp)
#
# produces MyPlugin.clap, ready to drop in the host's CLAP folder; see
# sonore_add_plugin() in cmake/SonoreSDKHelpers.cmake for every format from
# one compile.
cmake_minimum_required(VERSION 3.21)
# The first number this project has carried. 1.0.0 because it has been the
# only framework behind a shipping product since 2026-08-25, validated by the
# industry tools on every format it claims; a 0.x would be false modesty on a
# library that people are already selling plugins built with.
project(SonoreSDK VERSION 1.0.0 LANGUAGES CXX)

# ── The SDK itself: header-only, so an INTERFACE target ──────────────────────
set(SONORE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "Sonore SDK root")
add_library(sonore_sdk INTERFACE)
# BUILD_INTERFACE / INSTALL_INTERFACE: a source-tree path baked into an
# exported target is a configure error at install(EXPORT), and rightly: a
# consumer's machine has no C:/dev/sonore-sdk. Installed, both header sets
# land in one include dir, so one INSTALL_INTERFACE entry covers them.
target_include_directories(sonore_sdk INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/clap/include>"
  "$<INSTALL_INTERFACE:include>")
target_compile_features(sonore_sdk INTERFACE cxx_std_17)

# MSVC mangles UTF-8 literals without /utf-8, and sellers build with stock MSVC.
#
# Both flags are this BUILD's, not the export's. Written into the installed
# targets they were the decision of whatever compiler configured the install:
# an SDK installed with MSVC handed /utf-8 and /bigobj to a MinGW g++ project,
# which read them as file names. SonoreSDKConfig.cmake adds them again by the
# consumer's own compiler.
if(MSVC)
  target_compile_options(sonore_sdk INTERFACE $<BUILD_INTERFACE:/utf-8>)
  # A Debug object holds a section per inline function, and COFF counts them
  # in sixteen bits: sdk_tests stopped compiling in Debug (C1128) -- the
  # configuration Visual Studio builds when none is named -- and a plugin
  # compiling every format into one object is heading the same way. /bigobj
  # only widens the count.
  target_compile_options(sonore_sdk INTERFACE $<BUILD_INTERFACE:/bigobj>)
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

# ── The webview interface ────────────────────────────────────────────────────
# A plugin with a page of its own (SONORE_UI_HTML) shows it in an OS webview.
# Windows uses Edge WebView2, whose SDK is fetched at configure time (see
# cmake/FetchWebView2.cmake) rather than vendored: the static loader is ~10 MB
# per architecture. SONORE_GUI=OFF leaves the webview out, and the download
# with it, which is what CI without network wants; the SDK-drawn native editor
# is not part of this switch and still opens.
option(SONORE_GUI "Build the webview interface" ON)

if(SONORE_GUI AND WIN32)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchWebView2.cmake")
  # uiautomationcore is the accessibility provider (gfx/uia_win32.h) and
  # oleaut32 the BSTR/SAFEARRAY helpers it answers with. Both ship with
  # Windows. MSVC picks them up from #pragma comment(lib) in the header too;
  # this is here so a build with any other compiler works the same.
  # The WebView2 target is fetched into THIS build and is not exportable: a
  # target file cannot carry a .lib that lives in our _deps. BUILD_INTERFACE
  # here, and SonoreSDKConfig.cmake runs the same fetch in the consumer's
  # build and links it there, so an installed SDK gets WebView2 the way we do.
  target_link_libraries(sonore_sdk INTERFACE $<BUILD_INTERFACE:sonore_webview2> uiautomationcore oleaut32)
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
if(APPLE)
  # The audio and MIDI headers call CoreAudio, AudioToolbox and CoreMIDI
  # directly (system_volume.h, system_audio_format.h, the device and MIDI
  # backends), so anything that includes them links these, GUI or not: the
  # first macOS CI run failed to link audio_io_test without them. The plugin
  # helpers name them as well; the linker takes each once.
  target_link_libraries(sonore_sdk INTERFACE "-framework AudioToolbox" "-framework AudioUnit"
    "-framework CoreAudio" "-framework CoreMIDI" "-framework CoreFoundation" objc)
endif()

# ── The sonore_add_* helpers ─────────────────────────────────────────────────
# In their own module so that an INSTALLED SDK ships them: find_package()
# users get sonore_add_clap() and friends from SonoreSDKConfig.cmake, which
# includes the same file. See the note at the top of it.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SonoreSDKHelpers.cmake")

# The example builds only when the SDK is the top-level project, so a generated
# plugin embedding the SDK never pays for it.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # The saturator and the synth -- the two every validator and host test
  # exercises -- are built the way the farm builds a plugin: ONE compile, one
  # bundle per format (sonore_add_plugin). The rest stay one target per
  # format, so both ways of using the helpers are proven on every run.
  sonore_add_plugin(SonoreSaturator SOURCES examples/saturator/plugin.cpp
    CLAP SonoreSaturator VST3 SonoreSaturatorVST3 AU SonoreSaturatorAU LV2 SonoreSaturatorLV2
    AU_SUBTYPE "Sat1" AU_MANUFACTURER "Sonr" AU_VENDOR "Sonorie")
  sonore_add_plugin(SonoreSynth SOURCES examples/synth/plugin.cpp
    CLAP SonoreSynth VST3 SonoreSynthVST3 AU SonoreSynthAU LV2 SonoreSynthLV2
    AU_SUBTYPE "Syn1" AU_TYPE "aumu" AU_MANUFACTURER "Sonr" AU_VENDOR "Sonorie")
  sonore_add_clap(SonoreDucker SOURCES examples/ducker/plugin.cpp)
  sonore_add_clap(SonoreTrim SOURCES examples/trim/plugin.cpp)
  sonore_add_clap(SonoreSplitter SOURCES examples/splitter/plugin.cpp)
  sonore_add_clap(SonoreArp SOURCES examples/arp/plugin.cpp)
  sonore_add_clap(SonoreSampler SOURCES examples/sampler/plugin.cpp)
  sonore_add_clap(SonoreReverb SOURCES examples/reverb/plugin.cpp)
  # The two examples that put the newer pieces together. The showcase is a
  # plugin with a NATIVE editor of its own -- APVTS attachments, menus, an SVG
  # logo, a JPEG backdrop, a typed curve -- built like the saturator: one
  # compile, every format. The player is an APPLICATION, not a plugin: a
  # DocumentWindow, the AudioSource chain and the OS's audio device.
  sonore_add_plugin(SonoreShowcase SOURCES examples/showcase/plugin.cpp
    CLAP SonoreShowcase VST3 SonoreShowcaseVST3 AU SonoreShowcaseAU LV2 SonoreShowcaseLV2
    AU_SUBTYPE "Shw1" AU_MANUFACTURER "Sonr" AU_VENDOR "Sonorie")
  add_executable(SonorePlayer examples/player/main.cpp)
  target_link_libraries(SonorePlayer PRIVATE sonore_sdk)
  if(WIN32)
    # WASAPI is COM; shell32 tells the OS about recent files.
    target_link_libraries(SonorePlayer PRIVATE ole32 shell32)
  elseif(APPLE)
    target_link_libraries(SonorePlayer PRIVATE "-framework AudioToolbox" "-framework AudioUnit"
      "-framework CoreAudio" "-framework CoreFoundation" "-framework Cocoa" objc)
  elseif(UNIX)
    target_link_libraries(SonorePlayer PRIVATE ${CMAKE_DL_LIBS} pthread)
  endif()

  enable_testing()

  # The guides' code (sdk/docs/guides), built -- and run where a block says
  # so -- with the tests, so a guide whose code stopped compiling fails here.
  include("${CMAKE_CURRENT_SOURCE_DIR}/tests/doc_snippets.cmake")

  # Not a ctest: it measures, and a timing assertion on shared CI hardware is
  # a flaky test rather than a useful one. Run it by hand, and run it twice
  # (once built -DSONORE_NO_SIMD) to compare both speed AND checksums.
  add_executable(simd_bench tests/simd_bench.cpp)
  target_link_libraries(simd_bench PRIVATE sonore_sdk)

  # What simd_bench left to a person, as a test: the same program built with
  # and without SONORE_NO_SIMD must print the same hash of its float bits.
  add_executable(simd_identity tests/simd_identity.cpp)
  target_link_libraries(simd_identity PRIVATE sonore_sdk)
  add_executable(simd_identity_scalar tests/simd_identity.cpp)
  target_link_libraries(simd_identity_scalar PRIVATE sonore_sdk)
  target_compile_definitions(simd_identity_scalar PRIVATE SONORE_NO_SIMD=1)
  add_test(NAME simd_identity
           COMMAND "${CMAKE_COMMAND}" "-DVECTOR=$<TARGET_FILE:simd_identity>"
                   "-DSCALAR=$<TARGET_FILE:simd_identity_scalar>"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/simd_identity.cmake")

  # The toolkit in two translation units of one program, which the vendored
  # codecs' C linkage once made fail to link.
  add_executable(two_tu_test tests/two_tu_a.cpp tests/two_tu_b.cpp)
  target_link_libraries(two_tu_test PRIVATE sonore_sdk)
  add_test(NAME two_translation_units COMMAND two_tu_test)
  # The Audio Unit plist, configured with a vendor and name that need
  # escaping, read as XML: no host registers a component whose plist is not.
  add_test(NAME au_plist
           COMMAND "${CMAKE_COMMAND}" "-DSDK=${CMAKE_CURRENT_SOURCE_DIR}"
                   "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/au_plist_check.plist"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/au_plist_check.cmake")
  # The SDK as a stranger gets it: this build installed into a fresh prefix,
  # and a project that knows only find_package(SonoreSDK) builds a program, a
  # CLAP and a VST3 against it. The installed layout broke three times with
  # every other test green.
  add_test(NAME install_consume
           COMMAND "${CMAKE_COMMAND}" "-DBUILD=${CMAKE_CURRENT_BINARY_DIR}"
                   "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/install_consume" "-DCONFIG=$<CONFIG>"
                   "-DGEN=${CMAKE_GENERATOR}" "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                   "-DTOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                   "-DPLUGIN=${CMAKE_CURRENT_SOURCE_DIR}/examples/trim/plugin.cpp"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_consume.cmake")
  set_tests_properties(install_consume PROPERTIES TIMEOUT 900)
  # cmake/sonore_new_plugin.cmake, run as a new user runs it, and its project
  # built and tested.
  add_test(NAME new_plugin
           COMMAND "${CMAKE_COMMAND}" "-DSDK=${CMAKE_CURRENT_SOURCE_DIR}"
                   "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/new_plugin" "-DCONFIG=$<CONFIG>"
                   "-DGEN=${CMAKE_GENERATOR}" "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                   "-DTOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/new_plugin.cmake")
  set_tests_properties(new_plugin PROPERTIES TIMEOUT 900)
  # The widgets with RTTI off, which a dynamic_cast once made throw.
  add_executable(no_rtti_test tests/no_rtti_test.cpp)
  target_link_libraries(no_rtti_test PRIVATE sonore_sdk)
  target_compile_options(no_rtti_test PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>)
  add_test(NAME no_rtti COMMAND no_rtti_test)
  # One program per area: these replace operator new to count allocations,
  # start threads and processes, or talk to the OS, so they stay out of
  # sdk_tests. Each prints its checks and fails on any.
  find_package(Threads REQUIRED)
  foreach(_area dsp_maths midi_osc core_crypto audio_io core_data gui_widgets audio_sources gui_infra system modules opengl mac_input javascript app_infra app_windows mac_host)
    add_executable(${_area}_test tests/${_area}_test.cpp)
    target_link_libraries(${_area}_test PRIVATE sonore_sdk Threads::Threads)
    add_test(NAME ${_area} COMMAND ${_area}_test)
  endforeach()
  # images reads its fixtures from tests/data; code_editor shares sdk_tests'
  # temp_path.h.
  add_executable(images_test tests/images_test.cpp)
  target_link_libraries(images_test PRIVATE sonore_sdk Threads::Threads)
  target_compile_definitions(images_test PRIVATE SONORE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  add_test(NAME images COMMAND images_test)
  add_executable(code_editor_test tests/code_editor_test.cpp)
  target_link_libraries(code_editor_test PRIVATE sonore_sdk Threads::Threads)
  target_include_directories(code_editor_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  add_test(NAME code_editor COMMAND code_editor_test)
  set_tests_properties(opengl PROPERTIES TIMEOUT 300)
  if(UNIX AND NOT APPLE)
    target_link_libraries(opengl_test PRIVATE dl)
  endif()
  # Box2DRenderer against a Box2D the user points at: nothing is vendored.
  set(SONORE_BOX2D_DIR "" CACHE PATH "A Box2D source tree (3.1+ or 2.4) for box2d_renderer_test")
  if(SONORE_BOX2D_DIR)
    if(EXISTS "${SONORE_BOX2D_DIR}/include/box2d/id.h")
      enable_language(C)
      file(GLOB _b2 "${SONORE_BOX2D_DIR}/src/*.c")
    else()
      file(GLOB_RECURSE _b2 "${SONORE_BOX2D_DIR}/src/*.cpp")
    endif()
    add_library(sonore_box2d STATIC ${_b2})
    set_target_properties(sonore_box2d PROPERTIES C_STANDARD 17)
    target_include_directories(sonore_box2d PUBLIC "${SONORE_BOX2D_DIR}/include" PRIVATE "${SONORE_BOX2D_DIR}/src")
    add_executable(box2d_renderer_test tests/box2d_renderer_test.cpp)
    target_link_libraries(box2d_renderer_test PRIVATE sonore_sdk sonore_box2d)
    add_test(NAME box2d_renderer COMMAND box2d_renderer_test)
  endif()
  if(APPLE)
    target_link_libraries(mac_host_test PRIVATE "-framework AppKit" objc)
    # Run on a Mac, in a GUI session: what the AU host, the workgroups, the
    # accessibility bridge and the input methods find there. Never run here.
    add_executable(mac_host_selftest tests/mac_host_selftest.cpp)
    target_link_libraries(mac_host_selftest PRIVATE sonore_sdk "-framework AudioToolbox"
                          "-framework CoreAudio" "-framework CoreFoundation" "-framework AppKit" objc)
    add_test(NAME mac_host_selftest COMMAND mac_host_selftest)
    add_executable(mac_input_selftest tests/mac_input_selftest.cpp)
    target_link_libraries(mac_input_selftest PRIVATE sonore_sdk)
  elseif(UNIX)
    target_link_libraries(mac_host_test PRIVATE dl)
  endif()
  # plugin_state loads a LADSPA gain built here, through the host it tests.
  add_library(ladspa_gain_plugin MODULE tests/ladspa_gain_plugin.cpp)
  target_include_directories(ladspa_gain_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  add_executable(plugin_state_test tests/plugin_state_test.cpp)
  target_link_libraries(plugin_state_test PRIVATE sonore_sdk Threads::Threads)
  add_test(NAME plugin_state COMMAND plugin_state_test $<TARGET_FILE:ladspa_gain_plugin>)
  if(UNIX AND NOT APPLE)
    target_link_libraries(plugin_state_test PRIVATE dl)
  endif()
  if(WIN32)
    # windows.h first, min and max left as macros, then the SDK.
    add_executable(windows_minmax_test tests/windows_minmax_test.cpp)
    target_link_libraries(windows_minmax_test PRIVATE sonore_sdk)
    add_test(NAME windows_minmax COMMAND windows_minmax_test)
    # And at /W4 /WX, because it is the one translation unit that holds many
    # of the headers at once. The examples' objects below reach only what they
    # include, and two headers can clash only together: crypto.h's Blowfish
    # table, a local kPi, hid audio.h's sonore::kPi (C4459), and a project at
    # /W4 /WX that included both did not compile.
    if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options(windows_minmax_test PRIVATE /W4 /WX)
    endif()
  endif()
  if(MSVC)
    # The macOS build of a plugin (every format, the Cocoa editor, au_view.h)
    # and of the standalone, compiled here on every build: macOS CI is not
    # running, and the headers that choose a platform take _WIN32 first, so
    # nothing else here reads their Mac halves. __APPLE__ defined, _WIN32
    # undefined, Apple's declarations from tests/au_shim. Compile-only; see
    # tests/mac_syntax_plugin.cpp for what it does and does not prove.
    # One target each: both define the plugin's exports, and a library made
    # of the two objects warned of the duplicates (LNK4006).
    foreach(_mac plugin standalone audio_io modules opengl app_infra app_windows mac_input mac_host showcase player)
      add_library(sonore_mac_syntax_${_mac} OBJECT tests/mac_syntax_${_mac}.cpp)
      target_compile_features(sonore_mac_syntax_${_mac} PRIVATE cxx_std_17)
      target_include_directories(sonore_mac_syntax_${_mac} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${SONORE_SDK_DIR}/third_party/clap/include"
        "${SONORE_SDK_DIR}/third_party/vst3" "${SONORE_SDK_DIR}/third_party/lv2/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/au_shim" "${CMAKE_CURRENT_SOURCE_DIR}/tests/au_shim/win"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/au_shim/msvc")
      target_compile_definitions(sonore_mac_syntax_${_mac} PRIVATE __APPLE__=1
        SONORE_APPLE_SYNTAX_CHECK=1 _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
      # /bigobj: these link nothing, so they do not get sonore_sdk's, and a
      # Debug build of the plugin, the standalone and the showcase passes
      # 65,279 sections (C1128 on CI's Windows-Debug job).
      target_compile_options(sonore_mac_syntax_${_mac} PRIVATE /U_WIN32 /W4 /bigobj
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/tests/au_shim/msvc/posix_compat.h")
    endforeach()
    # The system layer's POSIX halves (sockets, spawn, mmap), read as macOS.
    add_library(sonore_system_syntax OBJECT tests/system_syntax.cpp)
    target_compile_features(sonore_system_syntax PRIVATE cxx_std_17)
    target_include_directories(sonore_system_syntax PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/tests/au_shim"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/au_shim/win" "${CMAKE_CURRENT_SOURCE_DIR}/tests/au_shim/msvc")
    target_compile_definitions(sonore_system_syntax PRIVATE __APPLE__=1
      SONORE_APPLE_SYNTAX_CHECK=1 _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    target_compile_options(sonore_system_syntax PRIVATE /U_WIN32 /W4
      "/FI${CMAKE_CURRENT_SOURCE_DIR}/tests/au_shim/msvc/posix_compat.h")
  endif()

  add_executable(sonore_sdk_tests tests/sdk_tests.cpp)
  target_link_libraries(sonore_sdk_tests PRIVATE sonore_sdk)
  # thread_pool.h uses std::thread, which on glibc needs pthread at LINK time
  # even though the header compiles without it. MSVC needs nothing.
  find_package(Threads REQUIRED)
  target_link_libraries(sonore_sdk_tests PRIVATE Threads::Threads)
  target_compile_definitions(sonore_sdk_tests
    PRIVATE SONORE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
            SONORE_TEST_PROJECT_VERSION="${PROJECT_VERSION}")
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
    # The paths themselves, not the target's INTERFACE_INCLUDE_DIRECTORIES:
    # that property carries $<BUILD_INTERFACE:>/$<INSTALL_INTERFACE:> since the
    # SDK became installable, and add_test evaluates the install half to an
    # EMPTY list element -- a bare "/I" that clang-cl then feeds the NEXT
    # argument (the WebView2 include) as its directory. WebView2.h not found.
    set(_arm64_inc "${CMAKE_CURRENT_SOURCE_DIR}/include"
                   "${CMAKE_CURRENT_SOURCE_DIR}/third_party/clap/include")
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
  # Every fuzzer runs a shorter walk in Debug, where it is twenty times
  # slower: the Debug suite is there for what only a debug build checks --
  # the STL's iterator checks, asserts -- not for depth. A fuzzer that hangs
  # fails at ten minutes (SONORE_FUZZ_TESTS below), not ctest's default
  # twenty-five.
  add_test(NAME fuzz_parsers COMMAND fuzz_parsers --iterations $<IF:$<CONFIG:Debug>,30,120>)
  # The state loader every format shares, fed damaged sessions through CLAP's
  # state extension with the ducker compiled in.
  add_executable(fuzz_state tests/fuzz_state.cpp)
  target_link_libraries(fuzz_state PRIVATE sonore_sdk)
  # As every test with a plugin compiled in: the wrapper's backends dlopen and
  # its handovers start threads, and glibc before 2.34 keeps both apart.
  if(UNIX AND NOT APPLE)
    target_link_libraries(fuzz_state PRIVATE dl pthread)
  endif()
  add_test(NAME fuzz_state COMMAND fuzz_state)
  # Every order of calls the CLAP specification allows, walked at random --
  # rates, block sizes down to zero, resets, flushes and state loads at any
  # point -- against three examples that between them have notes, a sidechain
  # and aux outputs.
  foreach(fx synth ducker splitter)
    add_executable(fuzz_calls_${fx} tests/fuzz_calls.cpp)
    target_link_libraries(fuzz_calls_${fx} PRIVATE sonore_sdk)
    target_compile_definitions(fuzz_calls_${fx} PRIVATE
      SONORE_FUZZ_PLUGIN="${CMAKE_CURRENT_SOURCE_DIR}/examples/${fx}/plugin.cpp")
    if(UNIX AND NOT APPLE)
      target_link_libraries(fuzz_calls_${fx} PRIVATE dl pthread)
    endif()
    add_test(NAME fuzz_calls_${fx} COMMAND fuzz_calls_${fx} --steps $<IF:$<CONFIG:Debug>,2000,20000>)
  endforeach()
  # The native generic editor under random hands: mouse, wheel, keys and
  # resizes through the MouseRouter a window uses, every kind of row.
  add_executable(fuzz_ui tests/fuzz_ui.cpp)
  target_link_libraries(fuzz_ui PRIVATE sonore_sdk)
  add_test(NAME fuzz_ui COMMAND fuzz_ui --steps $<IF:$<CONFIG:Debug>,1500,10000>)
  # ...and the rest of the toolkit: text fields, lists, tables, a tree, tabs,
  # a viewport, collapsible sections, range sliders and a keyboard together.
  add_executable(fuzz_widgets tests/fuzz_widgets.cpp)
  target_link_libraries(fuzz_widgets PRIVATE sonore_sdk)
  # 2000 at least: it requires the events to have reached eight of its ten
  # kinds of callback, and 800 sometimes do not.
  add_test(NAME fuzz_widgets COMMAND fuzz_widgets --steps $<IF:$<CONFIG:Debug>,2000,4000>)
  # The drawing API fed what paint code computes when it goes wrong -- NaN,
  # infinities, 1e30, zero and negative sizes -- through every call, with a
  # watchdog for a call that never returns. Seconds here; the default 20000
  # frames, and scripts/sdk-sanitize.mjs, go deeper.
  add_executable(fuzz_graphics tests/fuzz_graphics.cpp)
  target_link_libraries(fuzz_graphics PRIVATE sonore_sdk)
  if(UNIX AND NOT APPLE)
    target_link_libraries(fuzz_graphics PRIVATE pthread)
  endif()
  add_test(NAME fuzz_graphics COMMAND fuzz_graphics --steps $<IF:$<CONFIG:Debug>,300,1500>)

  # Area fuzzers: random input through each area's parsers and widgets.
  add_executable(fuzz_midi_osc tests/fuzz_midi_osc.cpp)
  target_link_libraries(fuzz_midi_osc PRIVATE sonore_sdk Threads::Threads)
  add_test(NAME fuzz_midi_osc COMMAND fuzz_midi_osc --cases $<IF:$<CONFIG:Debug>,4000,24000>)
  add_executable(fuzz_core_crypto tests/fuzz_core_crypto.cpp)
  target_link_libraries(fuzz_core_crypto PRIVATE sonore_sdk Threads::Threads)
  add_test(NAME fuzz_core_crypto COMMAND fuzz_core_crypto --iterations $<IF:$<CONFIG:Debug>,300,1500>)
  add_executable(fuzz_audio_io tests/fuzz_audio_io.cpp)
  target_link_libraries(fuzz_audio_io PRIVATE sonore_sdk Threads::Threads)
  add_test(NAME fuzz_audio_io COMMAND fuzz_audio_io)
  add_executable(fuzz_core_data tests/fuzz_core_data.cpp)
  target_link_libraries(fuzz_core_data PRIVATE sonore_sdk Threads::Threads)
  add_test(NAME fuzz_core_data COMMAND fuzz_core_data)
  add_executable(fuzz_plugin_state tests/fuzz_plugin_state.cpp)
  target_link_libraries(fuzz_plugin_state PRIVATE sonore_sdk Threads::Threads)
  add_test(NAME fuzz_plugin_state COMMAND fuzz_plugin_state)
  if(UNIX AND NOT APPLE)
    target_link_libraries(fuzz_plugin_state PRIVATE dl)
  endif()
  # User preset files from a folder anybody can write to: the container, the
  # header-only listing reader, imports (.vstpreset too), loads into a probe
  # plugin, and the file-name rules on arbitrary bytes.
  add_executable(fuzz_user_presets tests/fuzz_user_presets.cpp)
  target_link_libraries(fuzz_user_presets PRIVATE sonore_sdk Threads::Threads)
  add_test(NAME fuzz_user_presets COMMAND fuzz_user_presets)
  if(UNIX AND NOT APPLE)
    target_link_libraries(fuzz_user_presets PRIVATE dl)
  endif()
  add_executable(fuzz_gui_widgets tests/fuzz_gui_widgets.cpp)
  target_link_libraries(fuzz_gui_widgets PRIVATE sonore_sdk Threads::Threads)
  add_test(NAME fuzz_gui_widgets COMMAND fuzz_gui_widgets --steps $<IF:$<CONFIG:Debug>,2000,4000>)
  add_executable(fuzz_images tests/fuzz_images.cpp)
  target_link_libraries(fuzz_images PRIVATE sonore_sdk Threads::Threads)
  target_compile_definitions(fuzz_images PRIVATE SONORE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  add_test(NAME fuzz_images COMMAND fuzz_images --iterations $<IF:$<CONFIG:Debug>,20,60>)
  add_executable(fuzz_gui_infra tests/fuzz_gui_infra.cpp)
  target_link_libraries(fuzz_gui_infra PRIVATE sonore_sdk Threads::Threads)
  add_test(NAME fuzz_gui_infra COMMAND fuzz_gui_infra 20000)
  add_executable(fuzz_code_editor tests/fuzz_code_editor.cpp)
  target_link_libraries(fuzz_code_editor PRIVATE sonore_sdk Threads::Threads)
  add_test(NAME fuzz_code_editor COMMAND fuzz_code_editor --steps 20000 --seed 1)
  add_executable(fuzz_system tests/fuzz_system.cpp)
  target_link_libraries(fuzz_system PRIVATE sonore_sdk Threads::Threads)
  add_test(NAME fuzz_system COMMAND fuzz_system)
  add_executable(fuzz_modules tests/fuzz_modules.cpp)
  target_link_libraries(fuzz_modules PRIVATE sonore_sdk Threads::Threads)
  add_test(NAME fuzz_modules COMMAND fuzz_modules)
  add_executable(fuzz_javascript tests/fuzz_javascript.cpp)
  target_link_libraries(fuzz_javascript PRIVATE sonore_sdk Threads::Threads)
  add_test(NAME fuzz_javascript COMMAND fuzz_javascript)
  add_executable(fuzz_app_infra tests/fuzz_app_infra.cpp)
  target_link_libraries(fuzz_app_infra PRIVATE sonore_sdk Threads::Threads)
  add_test(NAME fuzz_app_infra COMMAND fuzz_app_infra --iterations $<IF:$<CONFIG:Debug>,3000,20000>)

  # The wasm ABI's pooled instances, compiled natively: two handles in one
  # module, each with its own MIDI inlet and key, rendered out of order.
  add_executable(wasm_pool_test tests/wasm_pool_test.cpp)
  target_link_libraries(wasm_pool_test PRIVATE sonore_sdk)
  add_test(NAME wasm_pool COMMAND wasm_pool_test)

  # The sampler example's sample handover, with a loading thread and a
  # rendering thread running against each other. Compiles the example's DSP
  # into the test (SONORE_EXAMPLE_DSP_ONLY), the way rt_safety_test does.
  add_executable(sampler_stress tests/sampler_stress_test.cpp)
  target_link_libraries(sampler_stress PRIVATE sonore_sdk)
  if(UNIX AND NOT APPLE)
    target_link_libraries(sampler_stress PRIVATE dl pthread)
  endif()
  add_test(NAME sampler_handover COMMAND sampler_stress 3)

  # The reverb example's impulse handover, the same way: state loads on one
  # thread while another renders.
  add_executable(reverb_stress tests/reverb_stress_test.cpp)
  target_link_libraries(reverb_stress PRIVATE sonore_sdk)
  if(UNIX AND NOT APPLE)
    target_link_libraries(reverb_stress PRIVATE dl pthread)
  endif()
  add_test(NAME reverb_handover COMMAND reverb_stress 3)

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
  # A state loaded on the main thread while audio runs reaches a block whole
  # or not at all. One second, two threads.
  add_executable(state_race_test tests/state_race_test.cpp)
  target_link_libraries(state_race_test PRIVATE sonore_sdk)
  if(UNIX AND NOT APPLE)
    target_link_libraries(state_race_test PRIVATE dl pthread)
  endif()
  add_test(NAME state_race COMMAND state_race_test 1)

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
                                      --expect-aux-outs 2 --expect-linked-stereo)
  add_test(NAME clap_midi_out COMMAND clap_host_test $<TARGET_FILE:SonoreArp>
                                      --expect-midi-out)
  add_test(NAME clap_sampler COMMAND clap_host_test $<TARGET_FILE:SonoreSampler>)
  add_test(NAME clap_reverb COMMAND clap_host_test $<TARGET_FILE:SonoreReverb>)
  # The showcase's own editor, opened by a host in a real window: the native
  # editor with a plugin's content in it rather than the generic one.
  add_test(NAME clap_showcase COMMAND clap_host_test $<TARGET_FILE:SonoreShowcase> --expect-native-editor)
  # A 64-bit DSP that emits MIDI and moves its latency, tail and state -- the
  # four things the double-precision path used to skip, and no example both
  # speaks doubles and does.
  sonore_add_clap(SonoreDoubleFixture SOURCES tests/double_fixture.cpp)
  add_test(NAME clap_64bit_parity COMMAND clap_host_test $<TARGET_FILE:SonoreDoubleFixture>
                                          --expect-midi-out --expect-64bit-parity)

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
  # A preset root of its own: the CLAP build's presets are files now, the
  # user's folder among them, and a developer's own presets must not change
  # the lists three formats are compared on.
  set_tests_properties(hosting PROPERTIES ENVIRONMENT
                       "SONORE_PRESET_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-presets")
  # Two instances of every plugin built here, set differently and run
  # interleaved as two tracks are, each against itself alone: a static shared
  # between instances is one track's sound leaking into another's.
  add_executable(instances_test tests/instances_test.cpp)
  target_link_libraries(instances_test PRIVATE sonore_sdk)
  sonore_enable_hosting(instances_test)
  add_test(NAME instances COMMAND instances_test "${SONORE_PLUGIN_DIR}")
  # Every plugin built here, in every format, walked at random through every
  # order of calls the host allows: prepare and release at any rate, block
  # size and width, blocks with MIDI, resets, parameters, state, restarts.
  add_executable(fuzz_hosted tests/fuzz_hosted.cpp)
  target_link_libraries(fuzz_hosted PRIVATE sonore_sdk)
  sonore_enable_hosting(fuzz_hosted)
  add_test(NAME fuzz_hosted COMMAND fuzz_hosted "${SONORE_PLUGIN_DIR}" --steps $<IF:$<CONFIG:Debug>,300,3000>)
  set_tests_properties(fuzz_parsers fuzz_state fuzz_calls_synth fuzz_calls_ducker fuzz_calls_splitter
                       fuzz_ui fuzz_widgets fuzz_graphics fuzz_hosted fuzz_midi_osc
                       fuzz_core_crypto fuzz_audio_io fuzz_core_data fuzz_plugin_state fuzz_user_presets
                       fuzz_gui_widgets fuzz_images fuzz_gui_infra fuzz_code_editor fuzz_system fuzz_modules
                       fuzz_javascript fuzz_app_infra
                       PROPERTIES TIMEOUT 600)
  # Every kind of MIDI a hosted VST3 emits, kept by the host.
  add_executable(vst3_host_midi_test tests/vst3_host_midi_test.cpp)
  target_link_libraries(vst3_host_midi_test PRIVATE sonore_sdk)
  target_include_directories(vst3_host_midi_test PRIVATE
    "${SONORE_SDK_DIR}/third_party/lv2/include" "${SONORE_SDK_DIR}/third_party/vst3")
  if(UNIX AND NOT APPLE)
    target_link_libraries(vst3_host_midi_test PRIVATE dl)
  endif()
  add_test(NAME vst3_host_midi COMMAND vst3_host_midi_test "${SONORE_PLUGIN_DIR}")
  if(WIN32)
    # A web editor with no WebView2 runtime must fall back to the native one.
    add_executable(webview_fallback_test tests/webview_fallback_test.cpp)
    target_link_libraries(webview_fallback_test PRIVATE sonore_sdk)
    target_include_directories(webview_fallback_test PRIVATE
      "${SONORE_SDK_DIR}/third_party/lv2/include" "${SONORE_SDK_DIR}/third_party/vst3")
    add_test(NAME webview_fallback COMMAND webview_fallback_test "${SONORE_PLUGIN_DIR}")

    # What a web editor's page may do on a real WebView2: stay put, open no
    # windows, and come up at all past what NavigateToString takes.
    add_executable(webview_page_test tests/webview_page_test.cpp)
    target_link_libraries(webview_page_test PRIVATE sonore_sdk)
    add_test(NAME webview_page COMMAND webview_page_test)

    # A licensed build that is not activated shows its activation overlay --
    # the only place to enter a code -- on every open of its web editor.
    add_executable(licence_overlay_test tests/licence_overlay_test.cpp)
    target_link_libraries(licence_overlay_test PRIVATE sonore_sdk)
    add_test(NAME licence_overlay COMMAND licence_overlay_test)

    # A WASAPI stream whose device is unplugged reports it and stops, driven
    # through a fake client that fails the way an invalidated one does.
    add_executable(wasapi_lost_test tests/wasapi_lost_test.cpp)
    target_link_libraries(wasapi_lost_test PRIVATE sonore_sdk)
    add_test(NAME wasapi_lost COMMAND wasapi_lost_test)
  endif()

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
  sonore_add_rt_test(rt_showcase  SOURCES examples/showcase/plugin.cpp)
  foreach(rt saturator synth ducker splitter arp sampler reverb trim guiprobe showcase)
    add_test(NAME rt_safety_${rt} COMMAND rt_${rt})
  endforeach()

  # The VST3 wrapper compiled into a test, as above, so its Instance can be
  # read: the editor's load meter and scope must be fed, at the real rate.
  # The CLAP wrapper compiled into a test the same way, its Instance read
  # after driving it through the C API.
  add_executable(clap_wrapper_test tests/clap_wrapper_test.cpp)
  target_link_libraries(clap_wrapper_test PRIVATE sonore_sdk)
  add_test(NAME clap_wrapper COMMAND clap_wrapper_test)
  add_executable(clap_preset_test tests/clap_preset_test.cpp)
  target_link_libraries(clap_preset_test PRIVATE sonore_sdk)
  add_test(NAME clap_preset COMMAND clap_preset_test)
  add_executable(clap_notes_test tests/clap_notes_test.cpp)
  target_link_libraries(clap_notes_test PRIVATE sonore_sdk)
  add_test(NAME clap_notes COMMAND clap_notes_test)

  add_executable(vst3_feeds_test tests/vst3_feeds_test.cpp)
  target_link_libraries(vst3_feeds_test PRIVATE sonore_sdk)
  target_include_directories(vst3_feeds_test PRIVATE "${SONORE_SDK_DIR}/third_party/vst3")
  add_test(NAME vst3_feeds COMMAND vst3_feeds_test)
  # Note offsets, zero-sample calls and blocks past the promised maximum.
  add_executable(vst3_events_test tests/vst3_events_test.cpp)
  target_link_libraries(vst3_events_test PRIVATE sonore_sdk)
  target_include_directories(vst3_events_test PRIVATE "${SONORE_SDK_DIR}/third_party/vst3")
  add_test(NAME vst3_events COMMAND vst3_events_test)
  # A DSP that reports its own state changes, in both precisions.
  add_executable(vst3_dirty_test tests/vst3_dirty_test.cpp)
  target_link_libraries(vst3_dirty_test PRIVATE sonore_sdk Threads::Threads)
  target_include_directories(vst3_dirty_test PRIVATE "${SONORE_SDK_DIR}/third_party/vst3")
  add_test(NAME vst3_dirty COMMAND vst3_dirty_test)
  # A note effect emitting every kind of MIDI message.
  add_executable(vst3_midiout_test tests/vst3_midiout_test.cpp)
  target_link_libraries(vst3_midiout_test PRIVATE sonore_sdk)
  target_include_directories(vst3_midiout_test PRIVATE "${SONORE_SDK_DIR}/third_party/vst3")
  add_test(NAME vst3_midiout COMMAND vst3_midiout_test)
  # A note-on's own tuning, in cents, reaching an expressive DSP.
  add_executable(vst3_tuning_test tests/vst3_tuning_test.cpp)
  target_link_libraries(vst3_tuning_test PRIVATE sonore_sdk)
  target_include_directories(vst3_tuning_test PRIVATE "${SONORE_SDK_DIR}/third_party/vst3")
  add_test(NAME vst3_tuning COMMAND vst3_tuning_test)
  # The MIDI stack's portable half against the inputs a review broke it with.
  add_executable(midi_stack_test tests/midi_stack_test.cpp)
  target_link_libraries(midi_stack_test PRIVATE sonore_sdk)
  # It opens the MIDI ports, so it needs the backends' libraries.
  if(APPLE)
    target_link_libraries(midi_stack_test PRIVATE
      "-framework CoreMIDI" "-framework CoreFoundation" objc)
  elseif(UNIX)
    target_link_libraries(midi_stack_test PRIVATE dl pthread)
  elseif(WIN32)
    target_link_libraries(midi_stack_test PRIVATE winmm)
  endif()
  add_test(NAME midi_stack COMMAND midi_stack_test)
  # PluginGraph against in-memory nodes whose latency the test moves.
  add_executable(graph_unit_test tests/graph_unit_test.cpp)
  target_link_libraries(graph_unit_test PRIVATE sonore_sdk)
  target_include_directories(graph_unit_test PRIVATE "${SONORE_SDK_DIR}/third_party/lv2/include"
                                                    "${SONORE_SDK_DIR}/third_party/vst3")
  if(UNIX)
    target_link_libraries(graph_unit_test PRIVATE dl)
  endif()
  add_test(NAME graph_unit COMMAND graph_unit_test)
  # The headers as a user's project includes them: all of <windows.h> first,
  # and plugin_cache.h with no VST3 SDK on the path (a CLAP-only host). Built
  # WITHOUT third_party/vst3 on purpose; it passes by compiling.
  add_executable(build_shapes_test tests/build_shapes_test.cpp)
  target_link_libraries(build_shapes_test PRIVATE sonore_sdk)
  if(UNIX)
    target_link_libraries(build_shapes_test PRIVATE dl)
  endif()
  add_test(NAME build_shapes COMMAND build_shapes_test)

  # The SAME sources, built as VST3 and driven by a VST3 host. Normalised
  # parameters, COM refcounting and bus arrangements are where a wrapper goes
  # silently wrong, so each is asserted against the shipped artifact.
  sonore_add_vst3(SonoreDuckerVST3 SOURCES examples/ducker/plugin.cpp)
  sonore_add_vst3(SonoreTrimVST3 SOURCES examples/trim/plugin.cpp)
  sonore_add_vst3(SonoreSplitterVST3 SOURCES examples/splitter/plugin.cpp)
  sonore_add_vst3(SonoreArpVST3 SOURCES examples/arp/plugin.cpp)
  sonore_add_vst3(SonoreSamplerVST3 SOURCES examples/sampler/plugin.cpp)
  sonore_add_vst3(SonoreReverbVST3 SOURCES examples/reverb/plugin.cpp)

  # macOS only; built and auval-validated by the SDK workflow on a Mac.
  sonore_add_au(SonoreDuckerAU SUBTYPE "Duk1" MANUFACTURER "Sonr" VENDOR "Sonorie" SOURCES examples/ducker/plugin.cpp)
  sonore_add_au(SonoreTrimAU SUBTYPE "Trm1" MANUFACTURER "Sonr" VENDOR "Sonorie" SOURCES examples/trim/plugin.cpp)
  sonore_add_au(SonoreSplitterAU SUBTYPE "Spl1" MANUFACTURER "Sonr" VENDOR "Sonorie" SOURCES examples/splitter/plugin.cpp)
  sonore_add_au(SonoreArpAU SUBTYPE "Arp1" MANUFACTURER "Sonr" VENDOR "Sonorie" TYPE "aumu" SOURCES examples/arp/plugin.cpp)
  sonore_add_au(SonoreSamplerAU SUBTYPE "Smp1" MANUFACTURER "Sonr" VENDOR "Sonorie" TYPE "aumu" SOURCES examples/sampler/plugin.cpp)
  sonore_add_au(SonoreReverbAU SUBTYPE "Rvb1" MANUFACTURER "Sonr" VENDOR "Sonorie" TYPE "aufx" SOURCES examples/reverb/plugin.cpp)
  sonore_add_auv3(SonoreSaturatorAUv3)

  add_executable(vst3_host_test tests/vst3_host_test.cpp)
  target_link_libraries(vst3_host_test PRIVATE sonore_sdk)
  target_include_directories(vst3_host_test PRIVATE "${SONORE_SDK_DIR}/third_party/vst3")
  if(UNIX AND NOT APPLE)
    target_link_libraries(vst3_host_test PRIVATE dl)
  endif()
  add_test(NAME vst3_effect COMMAND vst3_host_test $<TARGET_FILE:SonoreSaturatorVST3>)
  add_test(NAME vst3_instrument COMMAND vst3_host_test $<TARGET_FILE:SonoreSynthVST3>
                                        --expect-mpe --expect-pressure
           --expect-native-editor)
  add_test(NAME vst3_sidechain COMMAND vst3_host_test $<TARGET_FILE:SonoreDuckerVST3>
                                       --expect-sidechain)
  # Trim also carries the native-editor drag: its generic editor has no
  # parameter groups, so its first slider is at a place a test can find.
  add_test(NAME vst3_channels COMMAND vst3_host_test $<TARGET_FILE:SonoreTrimVST3>
                                      --expect-channels 1 8
                                      --expect-native-editor --drag-native-editor)
  add_test(NAME vst3_aux_outs COMMAND vst3_host_test $<TARGET_FILE:SonoreSplitterVST3>
                                      --expect-aux-outs 2)
  add_test(NAME vst3_midi_out COMMAND vst3_host_test $<TARGET_FILE:SonoreArpVST3>
                                      --expect-midi-out)
  add_test(NAME vst3_sampler COMMAND vst3_host_test $<TARGET_FILE:SonoreSamplerVST3>
                                     --expect-mpe)
  add_test(NAME vst3_reverb COMMAND vst3_host_test $<TARGET_FILE:SonoreReverbVST3>)
  add_test(NAME vst3_showcase COMMAND vst3_host_test $<TARGET_FILE:SonoreShowcaseVST3> --expect-native-editor)

  # sonore/host.h gives back every VST3 reference it takes: a plugin counting
  # its own live DSPs reads 1 loaded and 0 unloaded. Built outside the scanned
  # folder, where exports_test and every scan hold plugins to the shipped
  # ones' rules.
  sonore_add_vst3(SonoreLifetimeFixture SOURCES tests/lifetime_fixture.cpp)
  set_target_properties(SonoreLifetimeFixture PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  add_executable(vst3_lifetime_test tests/vst3_lifetime_test.cpp)
  target_link_libraries(vst3_lifetime_test PRIVATE sonore_sdk)
  target_include_directories(vst3_lifetime_test PRIVATE
    "${SONORE_SDK_DIR}/third_party/lv2/include" "${SONORE_SDK_DIR}/third_party/vst3")
  if(UNIX AND NOT APPLE)
    target_link_libraries(vst3_lifetime_test PRIVATE dl)
  endif()
  if(APPLE)
    set(_lifetime_fixture "$<TARGET_BUNDLE_DIR:SonoreLifetimeFixture>")
  else()
    set(_lifetime_fixture "$<TARGET_FILE:SonoreLifetimeFixture>")
  endif()
  add_test(NAME vst3_lifetime COMMAND vst3_lifetime_test "${_lifetime_fixture}")

  # A CLAP editor that reports itself destroyed, written against the raw API
  # because the SDK's wrapper never does: host.h must answer with destroy().
  add_library(SonoreGuiClosedFixture MODULE tests/gui_closed_fixture.cpp)
  target_include_directories(SonoreGuiClosedFixture PRIVATE
    "${SONORE_SDK_DIR}/third_party/clap/include")
  set_target_properties(SonoreGuiClosedFixture PROPERTIES
    PREFIX "" SUFFIX ".clap"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  add_executable(host_gui_closed_test tests/host_gui_closed_test.cpp)
  target_link_libraries(host_gui_closed_test PRIVATE sonore_sdk)
  target_include_directories(host_gui_closed_test PRIVATE
    "${SONORE_SDK_DIR}/third_party/lv2/include" "${SONORE_SDK_DIR}/third_party/vst3")
  if(UNIX AND NOT APPLE)
    target_link_libraries(host_gui_closed_test PRIVATE dl)
  endif()
  add_test(NAME host_gui_closed
           COMMAND host_gui_closed_test "$<TARGET_FILE:SonoreGuiClosedFixture>")

  # A CLAP plugin that counts flush() calls landing inside process(), against
  # the raw API: host.h must never make one while audio runs.
  add_library(SonoreFlushFixture MODULE tests/flush_fixture.cpp)
  target_include_directories(SonoreFlushFixture PRIVATE
    "${SONORE_SDK_DIR}/third_party/clap/include")
  set_target_properties(SonoreFlushFixture PROPERTIES
    PREFIX "" SUFFIX ".clap"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  add_executable(host_flush_test tests/host_flush_test.cpp)
  target_link_libraries(host_flush_test PRIVATE sonore_sdk)
  target_include_directories(host_flush_test PRIVATE
    "${SONORE_SDK_DIR}/third_party/lv2/include" "${SONORE_SDK_DIR}/third_party/vst3")
  if(UNIX AND NOT APPLE)
    target_link_libraries(host_flush_test PRIVATE dl pthread)
  endif()
  add_test(NAME host_flush
           COMMAND host_flush_test "$<TARGET_FILE:SonoreFlushFixture>")

  # Two CLAP plugins in one file, 250 and 4000 parameters, each reporting
  # every parameter every block: the host's id-to-position lookup must not
  # scale with the count.
  add_library(SonoreParamsFixture MODULE tests/params_fixture.cpp)
  target_include_directories(SonoreParamsFixture PRIVATE
    "${SONORE_SDK_DIR}/third_party/clap/include")
  set_target_properties(SonoreParamsFixture PROPERTIES
    PREFIX "" SUFFIX ".clap"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  add_executable(host_params_test tests/host_params_test.cpp)
  target_link_libraries(host_params_test PRIVATE sonore_sdk)
  target_include_directories(host_params_test PRIVATE
    "${SONORE_SDK_DIR}/third_party/lv2/include" "${SONORE_SDK_DIR}/third_party/vst3")
  if(UNIX AND NOT APPLE)
    target_link_libraries(host_params_test PRIVATE dl pthread)
  endif()
  add_test(NAME host_params
           COMMAND host_params_test "$<TARGET_FILE:SonoreParamsFixture>")

  # CLAP plugins with a sidechain, stereo-only and mono-only ports, each
  # counting every block whose buffers do not match what it declared.
  add_library(SonoreBusFixture MODULE tests/bus_fixture.cpp)
  target_include_directories(SonoreBusFixture PRIVATE
    "${SONORE_SDK_DIR}/third_party/clap/include")
  set_target_properties(SonoreBusFixture PROPERTIES
    PREFIX "" SUFFIX ".clap"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  add_executable(host_bus_test tests/host_bus_test.cpp)
  target_link_libraries(host_bus_test PRIVATE sonore_sdk)
  target_include_directories(host_bus_test PRIVATE
    "${SONORE_SDK_DIR}/third_party/lv2/include" "${SONORE_SDK_DIR}/third_party/vst3")
  if(UNIX AND NOT APPLE)
    target_link_libraries(host_bus_test PRIVATE dl pthread)
  endif()
  add_test(NAME host_bus
           COMMAND host_bus_test "$<TARGET_FILE:SonoreBusFixture>")

  # A VST3 binary that is nothing but its module entry and exit, counting
  # both: host.h must call the exit once per entry that succeeded.
  add_library(SonoreModuleFixture MODULE tests/module_fixture.cpp)
  set_target_properties(SonoreModuleFixture PROPERTIES
    PREFIX "" SUFFIX ".vst3"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  # A bare .vst3 file on every platform: describeFile() knows a VST3 by its
  # extension, and these were named .so on Linux, so the scan never entered
  # them there. Being .vst3, exported_symbols reads them as plugins, and on
  # Linux gcc exports the standard library's instantiations without a list.
  if(UNIX AND NOT APPLE)
    _sonore_plugin_exports(SonoreModuleFixture GetPluginFactory ModuleEntry ModuleExit)
  endif()
  # VST3 processors written against the C API -- one with a sidechain bus,
  # one that takes stereo only -- counting every block whose buses differ
  # from the arrangements they agreed to.
  add_library(SonoreVst3BusFixture MODULE tests/vst3_bus_fixture.cpp)
  target_include_directories(SonoreVst3BusFixture PRIVATE "${SONORE_SDK_DIR}/third_party/vst3")
  set_target_properties(SonoreVst3BusFixture PROPERTIES
    PREFIX "" SUFFIX ".vst3"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  if(UNIX AND NOT APPLE)
    _sonore_plugin_exports(SonoreVst3BusFixture GetPluginFactory ModuleEntry ModuleExit)
  endif()
  add_executable(vst3_bus_test tests/vst3_bus_test.cpp)
  target_link_libraries(vst3_bus_test PRIVATE sonore_sdk)
  target_include_directories(vst3_bus_test PRIVATE
    "${SONORE_SDK_DIR}/third_party/lv2/include" "${SONORE_SDK_DIR}/third_party/vst3")
  if(UNIX AND NOT APPLE)
    target_link_libraries(vst3_bus_test PRIVATE dl)
  endif()
  add_test(NAME vst3_bus COMMAND vst3_bus_test "$<TARGET_FILE:SonoreVst3BusFixture>")

  # A VST3 plugin with a separate controller class, against the C API: the
  # controller's edits must reach the processor and the halves be connected.
  add_library(SonoreVst3SplitFixture MODULE tests/vst3_split_fixture.cpp)
  target_include_directories(SonoreVst3SplitFixture PRIVATE "${SONORE_SDK_DIR}/third_party/vst3")
  set_target_properties(SonoreVst3SplitFixture PROPERTIES
    PREFIX "" SUFFIX ".vst3"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
  if(UNIX AND NOT APPLE)
    _sonore_plugin_exports(SonoreVst3SplitFixture GetPluginFactory ModuleEntry ModuleExit)
  endif()
  add_executable(vst3_split_test tests/vst3_split_test.cpp)
  target_link_libraries(vst3_split_test PRIVATE sonore_sdk)
  target_include_directories(vst3_split_test PRIVATE
    "${SONORE_SDK_DIR}/third_party/lv2/include" "${SONORE_SDK_DIR}/third_party/vst3")
  if(UNIX AND NOT APPLE)
    target_link_libraries(vst3_split_test PRIVATE dl)
  endif()
  add_test(NAME vst3_split COMMAND vst3_split_test "$<TARGET_FILE:SonoreVst3SplitFixture>")

  add_executable(vst3_module_test tests/vst3_module_test.cpp)
  target_link_libraries(vst3_module_test PRIVATE sonore_sdk)
  target_include_directories(vst3_module_test PRIVATE
    "${SONORE_SDK_DIR}/third_party/lv2/include" "${SONORE_SDK_DIR}/third_party/vst3")
  if(UNIX AND NOT APPLE)
    target_link_libraries(vst3_module_test PRIVATE dl)
  endif()
  add_test(NAME vst3_module
           COMMAND vst3_module_test "$<TARGET_FILE:SonoreModuleFixture>")

  # A raw LV2 bundle with audio, CV and control ports that records the bundle
  # path it is given and any port left unconnected at run().
  set(_lv2_ports_bundle "${CMAKE_CURRENT_BINARY_DIR}/fixtures/SonorePortsFixture.lv2")
  add_library(SonorePortsFixture MODULE tests/lv2_ports_fixture.cpp)
  target_include_directories(SonorePortsFixture PRIVATE "${SONORE_SDK_DIR}/third_party/lv2/include")
  set_target_properties(SonorePortsFixture PROPERTIES
    PREFIX ""
    # A generator expression, so multi-config generators do not add a
    # per-configuration folder between the binary and its manifest.
    LIBRARY_OUTPUT_DIRECTORY "$<1:${_lv2_ports_bundle}>")
  # It sits in an LV2 bundle, so exported_symbols reads it as a plugin, and
  # gcc exported its std::vector<uint32_t> instantiations without the list.
  _sonore_plugin_exports(SonorePortsFixture lv2_descriptor lv2ui_descriptor)
  file(GENERATE OUTPUT "${_lv2_ports_bundle}/manifest.ttl" CONTENT
"@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
<urn:sonore:test:lv2-ports> a lv2:Plugin ;
  lv2:binary <$<TARGET_FILE_NAME:SonorePortsFixture>> ;
  rdfs:seeAlso <ports.ttl> .
")
  file(WRITE "${_lv2_ports_bundle}/ports.ttl"
"@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix doap: <http://usefulinc.com/ns/doap#> .
<urn:sonore:test:lv2-ports> a lv2:Plugin ;
  doap:name \"Sonore LV2 Ports Fixture\" ;
  lv2:port [ a lv2:AudioPort , lv2:InputPort ; lv2:index 0 ; lv2:symbol \"in\" ; lv2:name \"In\" ] ,
    [ a lv2:AudioPort , lv2:OutputPort ; lv2:index 1 ; lv2:symbol \"out\" ; lv2:name \"Out\" ] ,
    [ a lv2:CVPort , lv2:InputPort ; lv2:index 2 ; lv2:symbol \"mod\" ; lv2:name \"Mod\" ] ,
    [ a lv2:CVPort , lv2:OutputPort ; lv2:index 3 ; lv2:symbol \"env\" ; lv2:name \"Env\" ] ,
    [ a lv2:ControlPort , lv2:InputPort ; lv2:index 4 ; lv2:symbol \"gain\" ; lv2:name \"Gain\" ;
      lv2:default 1.0 ; lv2:minimum 0.0 ; lv2:maximum 2.0 ] .
")
  add_executable(lv2_ports_test tests/lv2_ports_test.cpp)
  target_link_libraries(lv2_ports_test PRIVATE sonore_sdk)
  target_include_directories(lv2_ports_test PRIVATE
    "${SONORE_SDK_DIR}/third_party/lv2/include" "${SONORE_SDK_DIR}/third_party/vst3")
  if(UNIX AND NOT APPLE)
    target_link_libraries(lv2_ports_test PRIVATE dl)
  endif()
  add_dependencies(lv2_ports_test SonorePortsFixture)
  add_test(NAME lv2_ports COMMAND lv2_ports_test "${_lv2_ports_bundle}")

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
  sonore_add_standalone(SonoreShowcaseApp SOURCES examples/showcase/plugin.cpp)
  add_test(NAME standalone_showcase COMMAND SonoreShowcaseApp --verify)
  # The showcase compiled into a test, as rt_safety does it: its DSP, its curve
  # compiler, and its editor rendered into a Bitmap and driven -- menus, the
  # ColourSelector, a drag, undo, presets -- with four lambdas for a host.
  add_executable(showcase_test tests/showcase_test.cpp)
  target_link_libraries(showcase_test PRIVATE sonore_sdk)
  add_test(NAME showcase COMMAND showcase_test)
  # User presets: the file format and its damage, names for every filesystem,
  # the folder's operations, the showcase compiled in as a CLAP (and its VST3
  # wrapper, for the class id a .vstpreset carries) with its preset bar driven
  # headless, CLAP preset discovery and preset-load, and presets loaded while
  # audio runs with no allocation on the audio thread.
  add_executable(user_presets_test tests/user_presets_test.cpp)
  target_link_libraries(user_presets_test PRIVATE sonore_sdk Threads::Threads)
  target_include_directories(user_presets_test PRIVATE "${SONORE_SDK_DIR}/third_party/vst3")
  if(UNIX AND NOT APPLE)
    target_link_libraries(user_presets_test PRIVATE dl)
  endif()
  add_test(NAME user_presets COMMAND user_presets_test)
  # The player, headless: a null device, every sample checked, the window's
  # content drawn into a Bitmap.
  add_test(NAME player_verify
           COMMAND SonorePlayer --verify "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/tonal.wav")
  add_test(NAME standalone_render_seconds
           COMMAND "${CMAKE_COMMAND}" "-DAPP=$<TARGET_FILE:SonoreSaturatorApp>"
                   "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/render_seconds.wav"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/render_seconds.cmake")
  add_test(NAME standalone_options
           COMMAND "${CMAKE_COMMAND}" "-DAPP=$<TARGET_FILE:SonoreSynthApp>"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/standalone_options.cmake")
  # The standalone's audio path on a fresh thread, as after a device switch,
  # under a counting operator new. The splitter has aux buses: the most scratch.
  add_executable(standalone_rt_test tests/standalone_rt_test.cpp)
  target_link_libraries(standalone_rt_test PRIVATE sonore_sdk)
  if(WIN32)
    target_link_libraries(standalone_rt_test PRIVATE ole32 shell32)
  elseif(UNIX AND NOT APPLE)
    target_link_libraries(standalone_rt_test PRIVATE dl pthread)
  endif()
  add_test(NAME standalone_rt COMMAND standalone_rt_test)
  # The output guard: a probe that emits NaN or infinity on demand, compiled
  # with the CLAP, VST3 and LV2 wrappers and the standalone into one binary
  # under a counting operator new -- and again with guardOutput = false, when
  # the NaN has to come through untouched. Each in a directory of its own:
  # both capture stderr to a file and run --verify, which writes its own.
  foreach(_guard output_guard output_guard_off)
    add_executable(${_guard}_test tests/output_guard_test.cpp)
    target_link_libraries(${_guard}_test PRIVATE sonore_sdk)
    target_include_directories(${_guard}_test PRIVATE
      "${SONORE_SDK_DIR}/third_party/vst3" "${SONORE_SDK_DIR}/third_party/lv2/include")
    if(WIN32)
      target_link_libraries(${_guard}_test PRIVATE ole32 shell32)
    elseif(UNIX AND NOT APPLE)
      target_link_libraries(${_guard}_test PRIVATE dl pthread)
    endif()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${_guard}_run")
    add_test(NAME ${_guard} COMMAND ${_guard}_test
             WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${_guard}_run")
  endforeach()
  target_compile_definitions(output_guard_off_test PRIVATE SONORE_TEST_GUARD_OFF=1)
  add_test(NAME standalone_unicode_path
           COMMAND "${CMAKE_COMMAND}" "-DAPP=$<TARGET_FILE:SonoreSaturatorApp>"
                   "-DDIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/render_unicode.cmake")

  # LV2, judged like every other format: the BUILT bundle, driven by a host.
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
  add_test(NAME lv2_showcase
           COMMAND lv2_host_test "$<TARGET_FILE_DIR:SonoreShowcaseLV2>/SonoreShowcaseLV2.lv2")
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

  # Effects that are ONLY one width -- mono, and quad -- because every example
  # is stereo-capable and the port wiring had only ever run at two. At width
  # 1 the output port was mistaken for a right input and never connected.
  foreach(_w 1 4)
    sonore_add_lv2(SonoreWidth${_w}LV2 SOURCES tests/width_fixture.cpp)
    target_compile_definitions(SonoreWidth${_w}LV2 PRIVATE SONORE_FIXTURE_WIDTH=${_w})
    target_compile_definitions(SonoreWidth${_w}LV2_ttlgen PRIVATE SONORE_FIXTURE_WIDTH=${_w})
    add_test(NAME lv2_width_${_w} COMMAND lv2_host_test
             "$<TARGET_FILE_DIR:SonoreWidth${_w}LV2>/SonoreWidth${_w}LV2.lv2" --expect-width ${_w})
  endforeach()

  # An EFFECT that reads MIDI -- a note gate -- must get a MIDI input in LV2
  # as it does in CLAP and VST3, or it never hears the note that opens it.
  sonore_add_lv2(SonoreMidiGateLV2 SOURCES tests/midi_effect_fixture.cpp)
  # The LV2 worker against the host's thread, and two responses queued: the
  # wrapper compiled in, so the interface state it ends on can be read.
  add_executable(lv2_worker_test tests/lv2_worker_test.cpp)
  target_link_libraries(lv2_worker_test PRIVATE sonore_sdk)
  target_include_directories(lv2_worker_test PRIVATE "${SONORE_SDK_DIR}/third_party/lv2/include")
  if(UNIX)
    target_link_libraries(lv2_worker_test PRIVATE pthread)
  endif()
  add_test(NAME lv2_worker COMMAND lv2_worker_test)
  # The host's timeline into a DSP that takes the transport: time:Position.
  add_executable(lv2_transport_test tests/lv2_transport_test.cpp)
  target_link_libraries(lv2_transport_test PRIVATE sonore_sdk)
  target_include_directories(lv2_transport_test PRIVATE "${SONORE_SDK_DIR}/third_party/lv2/include")
  add_test(NAME lv2_transport COMMAND lv2_transport_test)
  # Note times at and past the end of a run, sliced and not.
  add_executable(lv2_events_test tests/lv2_events_test.cpp)
  target_link_libraries(lv2_events_test PRIVATE sonore_sdk)
  target_include_directories(lv2_events_test PRIVATE "${SONORE_SDK_DIR}/third_party/lv2/include")
  add_test(NAME lv2_events COMMAND lv2_events_test)
  # The bypass's dry path when the latency moves while running.
  add_executable(lv2_bypass_test tests/lv2_bypass_test.cpp)
  target_link_libraries(lv2_bypass_test PRIVATE sonore_sdk)
  target_include_directories(lv2_bypass_test PRIVATE "${SONORE_SDK_DIR}/third_party/lv2/include")
  add_test(NAME lv2_bypass COMMAND lv2_bypass_test)
  add_test(NAME lv2_midi_effect COMMAND lv2_host_test
           "$<TARGET_FILE_DIR:SonoreMidiGateLV2>/SonoreMidiGateLV2.lv2" --expect-midi-in)

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

  # ── Warning-clean at the level a seller's project may build at ────────────
  #
  # The SDK is headers compiled inside the USER's translation units, under the
  # user's warning flags. At /W4 the standalone used to emit 108 warnings --
  # 105 from the vendored codecs, 3 of the SDK's own -- so a project built
  # with /WX could not compile its standalone at all. These two objects are the
  # synth compiled as every plugin format and as a standalone, at /W4 /WX:
  # a warning anywhere in what they reach fails the build. MSVC proper only,
  # because that is the compiler this was measured against.
  if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_library(sonore_w4_plugin OBJECT examples/synth/plugin.cpp)
    target_link_libraries(sonore_w4_plugin PRIVATE sonore_sdk)
    target_compile_definitions(sonore_w4_plugin PRIVATE SONORE_BUILD_VST3=1 SONORE_BUILD_LV2=1)
    target_include_directories(sonore_w4_plugin PRIVATE
      "${SONORE_SDK_DIR}/third_party/vst3" "${SONORE_SDK_DIR}/third_party/lv2/include")
    target_compile_options(sonore_w4_plugin PRIVATE /W4 /WX)

    add_library(sonore_w4_standalone OBJECT examples/synth/plugin.cpp)
    target_link_libraries(sonore_w4_standalone PRIVATE sonore_sdk)
    target_compile_definitions(sonore_w4_standalone PRIVATE SONORE_BUILD_STANDALONE=1)
    target_compile_options(sonore_w4_standalone PRIVATE /W4 /WX)

    # The two examples built from the newer pieces: the showcase as every
    # plugin format, and the player application.
    add_library(sonore_w4_showcase OBJECT examples/showcase/plugin.cpp)
    target_link_libraries(sonore_w4_showcase PRIVATE sonore_sdk)
    target_compile_definitions(sonore_w4_showcase PRIVATE SONORE_BUILD_VST3=1 SONORE_BUILD_LV2=1)
    target_include_directories(sonore_w4_showcase PRIVATE
      "${SONORE_SDK_DIR}/third_party/vst3" "${SONORE_SDK_DIR}/third_party/lv2/include")
    target_compile_options(sonore_w4_showcase PRIVATE /W4 /WX)
    add_library(sonore_w4_player OBJECT examples/player/main.cpp)
    target_link_libraries(sonore_w4_player PRIVATE sonore_sdk)
    target_compile_options(sonore_w4_player PRIVATE /W4 /WX)

    # And a HOST: host.h, the graph and the plugin cache, with VST3 and LV2
    # hosting compiled in. Neither object above reaches them.
    add_library(sonore_w4_host OBJECT tests/host_w4.cpp)
    target_link_libraries(sonore_w4_host PRIVATE sonore_sdk)
    sonore_enable_hosting(sonore_w4_host)
    target_compile_options(sonore_w4_host PRIVATE /W4 /WX)

    # The same at /sdl and without _CRT_SECURE_NO_WARNINGS, which the target
    # defines for its users and which hid the SDK's own C4996s from these.
    add_library(sonore_sdl_plugin OBJECT tests/sdl_w4_test.cpp)
    target_link_libraries(sonore_sdl_plugin PRIVATE sonore_sdk)
    target_compile_definitions(sonore_sdl_plugin PRIVATE SONORE_BUILD_VST3=1 SONORE_BUILD_LV2=1)
    target_include_directories(sonore_sdl_plugin PRIVATE
      "${SONORE_SDK_DIR}/third_party/vst3" "${SONORE_SDK_DIR}/third_party/lv2/include")
    target_compile_options(sonore_sdl_plugin PRIVATE /W4 /WX /sdl)

    add_library(sonore_sdl_standalone OBJECT tests/sdl_w4_test.cpp)
    target_link_libraries(sonore_sdl_standalone PRIVATE sonore_sdk)
    target_compile_definitions(sonore_sdl_standalone PRIVATE SONORE_BUILD_STANDALONE=1)
    target_compile_options(sonore_sdl_standalone PRIVATE /W4 /WX /sdl)
  endif()

  # ── Tests that measure the machine, run alone ─────────────────────────────
  #
  # These count callbacks against the display, handles across editor opens,
  # reads against a writer flat out, and wait on real devices and WebView2
  # pages. Each waits for what it checks, but a run at ctest -j beside other
  # tests starved them of the CPU they were measuring and failed them while
  # alone they passed every time: RUN_SERIAL keeps ctest from starting
  # anything else beside them.
  foreach(_serial audio_sources gui_infra modules editor_soak lv2_ui_bridge wasapi_lost
                  licence_overlay webview_page webview_fallback)
    if(TEST ${_serial})
      set_tests_properties(${_serial} PROPERTIES RUN_SERIAL TRUE)
    endif()
  endforeach()
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

# ── Installing ───────────────────────────────────────────────────────────────
# Only when this is the top-level project: a consumer that add_subdirectory()s
# the SDK has its own install rules and should not inherit ours. What gets
# installed is what a header-only library is -- the headers, the vendored
# format headers, the CMake helpers -- plus an exported target, so that
#
#   find_package(SonoreSDK 1.0 REQUIRED)
#   target_link_libraries(myplugin PRIVATE SonoreSDK::sonore_sdk)
#
# works from a build that never saw this source tree. Until now the only way
# to use the SDK was a git checkout beside your own code, which is how the
# build farm uses it and is not how a stranger expects a library to arrive.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  install(TARGETS sonore_sdk EXPORT SonoreSDKTargets)
  install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(DIRECTORY third_party/clap/include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  # The helpers reach these as ${SONORE_SDK_DIR}/third_party/<x> and
  # ${SONORE_SDK_DIR}/cmake/<y>, so they are installed under those names.
  # install(DIRECTORY) keeps only a path's LAST component: listing
  # third_party/vst3 with DESTINATION share/SonoreSDK lands at
  # share/SonoreSDK/vst3, and the first consumer build could not find
  # vst3_c_api.h. The CLAP built, which is how it nearly shipped.
  install(DIRECTORY third_party/lv2 third_party/vst3
          DESTINATION "${CMAKE_INSTALL_DATADIR}/SonoreSDK/third_party")
  # The two codecs audiofile_codecs.h compiles in. Not under share/: they are
  # #included by a header, so they go where that header can find them --
  # beside it, which is the second of the two paths it tries. Left out, the
  # installed SDK built a .clap and failed on the first standalone.
  install(FILES third_party/minimp3.h third_party/stb_vorbis.c
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/sonore/third_party")
  install(DIRECTORY cmake DESTINATION "${CMAKE_INSTALL_DATADIR}/SonoreSDK")
  install(EXPORT SonoreSDKTargets
          NAMESPACE SonoreSDK::
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SonoreSDK")
  # ARCH_INDEPENDENT: there is no binary in the package, so the pointer size
  # of whatever configured the install is no business of the project finding
  # it. Without it the version file recorded that size, and an SDK installed
  # from the usual 64-bit configure turned away a 32-bit project -- an x86
  # plugin, -A Win32 -- with "considered but not accepted: version: 1.0.0
  # (64bit)" and nothing else to go on.
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/SonoreSDKConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)
  set(SONORE_DATA_DIR "${CMAKE_INSTALL_DATADIR}/SonoreSDK")
  configure_package_config_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SonoreSDKConfig.cmake.in"
                                "${CMAKE_CURRENT_BINARY_DIR}/SonoreSDKConfig.cmake"
                                INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SonoreSDK"
                                PATH_VARS SONORE_DATA_DIR)
  # The licences travel with what they cover: the SDK's own, the notices for
  # what it vendors, and CLAP's MIT licence for the headers installed beside
  # ours. An installed prefix carried the CLAP headers and none of these.
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE" "${CMAKE_CURRENT_SOURCE_DIR}/../NOTICE"
                "${CMAKE_CURRENT_SOURCE_DIR}/../THIRD_PARTY_NOTICES.md"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/SonoreSDK")
  install(FILES third_party/clap/LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/SonoreSDK/third_party/clap")
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/SonoreSDKConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/SonoreSDKConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SonoreSDK")
endif()
