# Sonore SDK: the sonore_add_* helpers.
#
# One file, included from two places: the SDK's own CMakeLists.txt when the
# framework is built from its source tree, and SonoreSDKConfig.cmake when it
# arrives through find_package() from an installed prefix. The functions do not
# know which; they reach the format headers and the plist template through
# SONORE_SDK_DIR, which each includer sets to wherever those files are.
#
# Nothing in here may refer to CMAKE_CURRENT_SOURCE_DIR: from a Config file
# that is the CONSUMER's directory, and a helper that reads it would compile a
# stranger's tree looking for our plist template.
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

# ── One compile, several bundles ─────────────────────────────────────────────
# Each sonore_add_<format>() compiles the plugin source again: the wrappers
# are templates over the DSP, so every format is a full instantiation of the
# same ~25 seconds. Measured on the build farm, a translation unit holding
# CLAP+VST3+AU costs 12% more than one holding a single format -- so
# compiling ONCE with every format defined and linking that object into each
# bundle costs a third of three compiles, and that CPU is what a queue of
# builds competes for. A module built this way carries the other formats'
# code -- on Linux and macOS unexported and unreachable behind the same export
# list the single-format helper writes; on Windows, where there is no list,
# exported too (a .clap also answering GetPluginFactory, which no CLAP host
# asks for). The helpers take OBJECTS <object-library> in
# place of SOURCES for it; sonore_add_plugin() below is the front door.
function(_sonore_module_from TARGET SOURCES OBJECTS HELPER)
  if(OBJECTS AND SOURCES)
    message(FATAL_ERROR "${HELPER}(${TARGET}): SOURCES and OBJECTS are alternatives, not both")
  elseif(OBJECTS)
    if(NOT TARGET ${OBJECTS})
      message(FATAL_ERROR "${HELPER}(${TARGET}): OBJECTS names no target: ${OBJECTS}")
    endif()
    # A module with no sources of its own: the code and the usage
    # requirements (the SDK, the AU frameworks) both come from the object
    # library. Sourceless, so the linker language has to be said.
    add_library(${TARGET} MODULE)
    target_link_libraries(${TARGET} PRIVATE ${OBJECTS})
    set_target_properties(${TARGET} PROPERTIES LINKER_LANGUAGE CXX)
  elseif(SOURCES)
    add_library(${TARGET} MODULE ${SOURCES})
  else()
    message(FATAL_ERROR "${HELPER}(${TARGET}): SOURCES is required")
  endif()
endfunction()

function(sonore_add_clap TARGET)
  cmake_parse_arguments(ARG "" "OBJECTS" "SOURCES" ${ARGN})
  _sonore_module_from(${TARGET} "${ARG_SOURCES}" "${ARG_OBJECTS}" sonore_add_clap)
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
  # Through SONORE_SDK_DIR, like every other path in this file. A bare
  # tests/rt_safety_test.cpp resolves against the CALLER's directory -- the
  # rule at the top of this file -- and a consumer got "cannot find source"
  # for a file of ours. The suite is not installed, so say that instead.
  set(_rt "${SONORE_SDK_DIR}/tests/rt_safety_test.cpp")
  if(NOT EXISTS "${_rt}")
    message(FATAL_ERROR "sonore_add_rt_test(${TARGET}): the real-time audit lives in the "
                        "SDK's own test suite (tests/rt_safety_test.cpp), which only a source "
                        "checkout of the SDK has")
  endif()
  add_executable(${TARGET} "${_rt}" ${ARG_SOURCES})
  target_link_libraries(${TARGET} PRIVATE sonore_sdk)
  # The GUI compiles in as it does for a real plugin, but is never created:
  # a window is main-thread work by contract and is not what is measured here.
endfunction()

# ── sonore_add_vst3(<target> SOURCES a.cpp [...]) ────────────────────────────
# A VST3 is a shared library exporting GetPluginFactory, shipped inside a bundle
# directory whose layout the host scans. The SAME sources as the CLAP target,
# with SONORE_BUILD_VST3 defined: one DSP, two formats, no drift.
function(sonore_add_vst3 TARGET)
  cmake_parse_arguments(ARG "" "OBJECTS" "SOURCES" ${ARGN})
  _sonore_module_from(${TARGET} "${ARG_SOURCES}" "${ARG_OBJECTS}" sonore_add_vst3)
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
    # The bundle a host scans: vst3/<target>.vst3/Contents/<arch>/<binary>,
    # in a folder of its own beside the raw module, so it can be copied into a
    # VST3 folder as it is. Beside the module it cannot be called <target>.vst3
    # -- that is the module's own name -- which is why the first layout below
    # was <target>.vst3.bundle and had to be renamed by hand; it is still
    # assembled, for the tools that know it by that name. The binary inside is
    # <target>.vst3 on Windows and <target>.so on Linux, as the VST3 module
    # format names them; it was .vst3 on both.
    if(UNIX)
      set(_inner "${TARGET}.so")
    else()
      set(_inner "${TARGET}.vst3")
    endif()
    set(_bundle "$<TARGET_FILE_DIR:${TARGET}>/${TARGET}.vst3.bundle/Contents/${_arch}")
    set(_scanned "$<TARGET_FILE_DIR:${TARGET}>/vst3/${TARGET}.vst3/Contents/${_arch}")
    add_custom_command(TARGET ${TARGET} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_bundle}"
      COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${TARGET}>" "${_bundle}/${TARGET}.vst3"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_scanned}"
      COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${TARGET}>" "${_scanned}/${_inner}"
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
    # On Windows CMAKE_SYSTEM_PROCESSOR is the HOST's unless a toolchain file
    # says otherwise: a Visual Studio build with -A Win32 or -A ARM64, or one
    # from an x86 developer prompt, reports AMD64, and a 32-bit plugin went
    # into Contents/x86_64-win -- where a 64-bit host fails to load it and a
    # 32-bit one, looking in x86-win, finds nothing. The compiler knows what
    # it targets (MSVC and clang-cl say x64, X86, ARM64, ARM64EC); failing
    # that the generator's platform, and then the pointer size.
    foreach(_v CMAKE_CXX_COMPILER_ARCHITECTURE_ID MSVC_CXX_ARCHITECTURE_ID CMAKE_GENERATOR_PLATFORM)
      if(NOT "${${_v}}" STREQUAL "")
        string(TOLOWER "${${_v}}" _p)
        break()
      endif()
    endforeach()
    if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND _p MATCHES "^(amd64|x86_64|x64)$")
      set(_p "x86")
    endif()
    if(_p MATCHES "^(arm64|aarch64)$")
      set(_a "arm64-win")
    elseif(_p MATCHES "^(arm64ec)$")
      set(_a "arm64ec-win")
    elseif(_p MATCHES "^(x86|i[3-6]86|win32)$")
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
# Text for an Info.plist, which is XML: a VENDOR of "Smith & Sons" written
# as it was made the file malformed, and a component whose plist cannot be
# read is never registered.
function(_sonore_xml_escape OUT TEXT)
  string(REPLACE "&" "&amp;" TEXT "${TEXT}")
  string(REPLACE "<" "&lt;" TEXT "${TEXT}")
  string(REPLACE ">" "&gt;" TEXT "${TEXT}")
  set(${OUT} "${TEXT}" PARENT_SCOPE)
endfunction()

function(sonore_add_au TARGET)
  if(NOT APPLE)
    return()
  endif()
  cmake_parse_arguments(ARG "" "SUBTYPE;MANUFACTURER;TYPE;VERSION;OBJECTS;NAME;VENDOR;BUNDLE_ID" "SOURCES" ${ARGN})
  # The version goes into the plist twice: as the strings a person reads and
  # as the packed integer (major<<16 | minor<<8 | patch) in the
  # AudioComponents entry, which is the one the registrar CACHES BY. It was a
  # literal 1.0.0 / 65536 for every plugin, so a rebuilt update kept the old
  # registration and Logic could go on loading the previous binary.
  if(NOT ARG_VERSION)
    set(ARG_VERSION "1.0.0")
  endif()
  if(NOT ARG_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "sonore_add_au(${TARGET}): VERSION must be major.minor.patch, got '${ARG_VERSION}'")
  endif()
  string(REPLACE "." ";" _ver "${ARG_VERSION}")
  list(GET _ver 0 _vmaj)
  list(GET _ver 1 _vmin)
  list(GET _ver 2 _vpat)
  math(EXPR SONORE_AU_VERSION_INT "${_vmaj} * 65536 + ${_vmin} * 256 + ${_vpat}")
  set(SONORE_AU_VERSION "${ARG_VERSION}")
  # The identity a host registers the component under, and shows. Every
  # default is Sonorie's own, so a plugin that leaves them out is filed as one
  # of ours, and two of them installed together collide on Snr1/Sonr: said
  # here rather than discovered in a user's plug-in manager.
  set(_missing)
  foreach(_k SUBTYPE MANUFACTURER VENDOR)
    if(NOT ARG_${_k})
      list(APPEND _missing ${_k})
    endif()
  endforeach()
  if(_missing)
    string(REPLACE ";" ", " _missing "${_missing}")
    message(WARNING "sonore_add_au(${TARGET}): no ${_missing} given, so this AU registers with "
                    "Sonorie's defaults (subtype Snr1, manufacturer Sonr, shown as \"Sonorie: ...\"), "
                    "which every AU built without them shares. Pass your own four-character "
                    "SUBTYPE and MANUFACTURER codes and your VENDOR name.")
  endif()
  if(NOT ARG_SUBTYPE)
    set(ARG_SUBTYPE "Snr1")
  endif()
  if(NOT ARG_MANUFACTURER)
    set(ARG_MANUFACTURER "Sonr")
  endif()
  if(NOT ARG_VENDOR)
    set(ARG_VENDOR "Sonorie")
  endif()
  # What a host lists: "<VENDOR>: <NAME>". NAME defaults to the target's name.
  if(NOT ARG_NAME)
    set(ARG_NAME "${TARGET}")
  endif()
  if(NOT ARG_BUNDLE_ID)
    set(ARG_BUNDLE_ID "com.sonorie.${TARGET}")
  endif()
  # aufx = effect, aumu = instrument. Hosts file the two in different menus,
  # so an instrument declared as an effect is simply never found by a player.
  if(NOT ARG_TYPE)
    set(ARG_TYPE "aufx")
  endif()

  _sonore_module_from(${TARGET} "${ARG_SOURCES}" "${ARG_OBJECTS}" sonore_add_au)
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
    MACOSX_BUNDLE_GUI_IDENTIFIER "${ARG_BUNDLE_ID}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}-Info.plist")

  # The AudioComponents entry IS the registration: a host that cannot read it
  # never discovers the plugin, however correct the code is.
  _sonore_xml_escape(SONORE_AU_NAME "${TARGET}")
  _sonore_xml_escape(SONORE_AU_SUBTYPE "${ARG_SUBTYPE}")
  _sonore_xml_escape(SONORE_AU_MANUFACTURER "${ARG_MANUFACTURER}")
  _sonore_xml_escape(SONORE_AU_TYPE "${ARG_TYPE}")
  _sonore_xml_escape(SONORE_AU_VENDOR "${ARG_VENDOR}")
  _sonore_xml_escape(SONORE_AU_DISPLAY_NAME "${ARG_NAME}")
  _sonore_xml_escape(SONORE_AU_BUNDLE_ID "${ARG_BUNDLE_ID}")
  configure_file("${SONORE_SDK_DIR}/cmake/AudioUnit-Info.plist.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}-Info.plist" @ONLY)

  # The factory the plist names, and clap_entry because the same source defines
  # it. Every other format had an export list and the AU did not, so a bundle
  # exported every non-inline symbol it had -- the cross-plugin collision that
  # list exists to prevent, on the format a host loads many of at once.
  _sonore_plugin_exports(${TARGET} clap_entry SonoreAUFactory)
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
    # interactive run simply also has a console. WASAPI needs COM; shell32
    # splits the UTF-16 command line, because argv is not UTF-8 on Windows.
    target_link_libraries(${TARGET} PRIVATE ole32 shell32)
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
  cmake_parse_arguments(ARG "" "OBJECTS" "SOURCES" ${ARGN})

  set(_bundle "$<TARGET_FILE_DIR:${TARGET}>/${TARGET}.lv2")

  _sonore_module_from(${TARGET} "${ARG_SOURCES}" "${ARG_OBJECTS}" sonore_add_lv2)
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

  if(ARG_OBJECTS)
    # The generator is a main() plus the object every format shares -- see
    # sonore_lv2_ttlgen_main in lv2_wrapper.h. Compiling the whole plugin a
    # second time to print two text files was the costliest way to do it.
    add_executable(${TARGET}_ttlgen "${SONORE_SDK_DIR}/cmake/lv2_ttlgen_main.cpp")
    target_link_libraries(${TARGET}_ttlgen PRIVATE ${ARG_OBJECTS})
  else()
    add_executable(${TARGET}_ttlgen ${ARG_SOURCES})
    target_link_libraries(${TARGET}_ttlgen PRIVATE sonore_sdk)
    target_compile_definitions(${TARGET}_ttlgen PRIVATE SONORE_BUILD_LV2=1 SONORE_LV2_TTLGEN=1)
    target_include_directories(${TARGET}_ttlgen PRIVATE "${SONORE_SDK_DIR}/third_party/lv2/include")
  endif()

  add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_bundle}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${TARGET}>"
            "${_bundle}/$<TARGET_FILE_NAME:${TARGET}>"
    COMMENT "Assembling the ${TARGET}.lv2 bundle")
  # The generator writes into the folder it is run IN, named ".", and is never
  # handed the folder's path. On Windows a program's argv arrives in the ANSI
  # code page, so a project under C:\Users\José reached the generator as
  # "Jos\xE9" -- not UTF-8, which is what the SDK's file calls read -- and the
  # build failed on "cannot write .../manifest.ttl", taking every format and
  # the app down with it. The working directory reaches it intact, whatever
  # the path is written in. A separate command, as it needs the folder the
  # one above makes.
  add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${TARGET}_ttlgen . "$<TARGET_FILE_NAME:${TARGET}>"
    WORKING_DIRECTORY "${_bundle}"
    COMMENT "Writing the ${TARGET}.lv2 metadata")
  add_dependencies(${TARGET} ${TARGET}_ttlgen)
endfunction()

# ── sonore_add_plugin(<name> SOURCES a.cpp [CLAP <t>] [VST3 <t>] [AU <t>] [LV2 <t>] [AAX <t>]) ──
#
#   sonore_add_plugin(MyPlugin SOURCES plugin.cpp
#     CLAP MyPlugin  VST3 MyPluginVST3  AU MyPluginAU  LV2 MyPluginLV2  AAX MyPluginAAX
#     AU_SUBTYPE "Mypl" AU_MANUFACTURER "Manu" AU_TYPE aufx  VERSION 1.2.0
#     AU_VENDOR "My Company" AU_NAME "My Plugin" AU_BUNDLE_ID com.mycompany.myplugin
#     AAX_MANUFACTURER "Manu" AAX_PRODUCT "Mypl")
#
# One compile of the source, then one bundle per format named, each the same
# artifact the single-format helper would have produced under that name. AU
# is skipped off macOS as sonore_add_au() is; LV2 builds wherever it is
# named, so a Linux-only LV2 is the caller's if(). The standalone is not here
# on purpose: it is an executable with a main loop of its own, and its object
# cannot be a module's.
#
# AAX is the exception to the one compile: it is compiled on its own by
# sonore_add_aax(), against the developer's copy of Avid's SDK. In the shared
# object, the AAX code would make every other bundle need Avid's library to
# link. Without SONORE_AAX_SDK_ROOT it is skipped, as sonore_add_aax() says.
function(sonore_add_plugin NAME)
  cmake_parse_arguments(ARG "" "CLAP;VST3;AU;LV2;AAX;AU_SUBTYPE;AU_MANUFACTURER;AU_TYPE;AU_VENDOR;AU_NAME;AU_BUNDLE_ID;AAX_MANUFACTURER;AAX_PRODUCT;VERSION"
                        "SOURCES" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "sonore_add_plugin(${NAME}): SOURCES is required")
  endif()
  if(NOT (ARG_CLAP OR ARG_VST3 OR ARG_AU OR ARG_LV2 OR ARG_AAX))
    message(FATAL_ERROR "sonore_add_plugin(${NAME}): name at least one format target (CLAP/VST3/AU/LV2/AAX)")
  endif()
  if(ARG_AAX)
    set(_aax_args)
    foreach(_k MANUFACTURER PRODUCT)
      if(ARG_AAX_${_k})
        list(APPEND _aax_args ${_k} "${ARG_AAX_${_k}}")
      endif()
    endforeach()
    if(ARG_VERSION)
      list(APPEND _aax_args VERSION "${ARG_VERSION}")
    endif()
    sonore_add_aax(${ARG_AAX} SOURCES ${ARG_SOURCES} ${_aax_args})
    if(NOT (ARG_CLAP OR ARG_VST3 OR ARG_AU OR ARG_LV2))
      return()
    endif()
  endif()
  if(ARG_AU AND NOT APPLE)
    set(ARG_AU "")
  endif()

  set(_obj ${NAME}_objects)
  add_library(${_obj} OBJECT ${ARG_SOURCES})
  target_link_libraries(${_obj} PUBLIC sonore_sdk)
  # The properties a module would have had, on the object that goes into one.
  set_target_properties(${_obj} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  if(ARG_VST3)
    target_compile_definitions(${_obj} PRIVATE SONORE_BUILD_VST3=1)
    target_include_directories(${_obj} PRIVATE "${SONORE_SDK_DIR}/third_party/vst3")
  endif()
  if(ARG_LV2)
    target_compile_definitions(${_obj} PRIVATE SONORE_BUILD_LV2=1)
    target_include_directories(${_obj} PRIVATE "${SONORE_SDK_DIR}/third_party/lv2/include")
  endif()
  if(ARG_AU)
    target_compile_definitions(${_obj} PRIVATE SONORE_BUILD_AU=1)
    # PUBLIC: every module built from this object holds the AU code, so every
    # one of them resolves these at link time, not only the .component.
    target_link_libraries(${_obj} PUBLIC
      "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreAudio"
      "-framework CoreFoundation" "-framework CoreMIDI")
  endif()

  if(ARG_CLAP)
    sonore_add_clap(${ARG_CLAP} OBJECTS ${_obj})
  endif()
  if(ARG_VST3)
    sonore_add_vst3(${ARG_VST3} OBJECTS ${_obj})
  endif()
  if(ARG_AU)
    set(_au_args)
    foreach(_k SUBTYPE MANUFACTURER TYPE VENDOR NAME BUNDLE_ID)
      if(ARG_AU_${_k})
        list(APPEND _au_args ${_k} "${ARG_AU_${_k}}")
      endif()
    endforeach()
    if(ARG_VERSION)
      list(APPEND _au_args VERSION "${ARG_VERSION}")
    endif()
    sonore_add_au(${ARG_AU} OBJECTS ${_obj} ${_au_args})
  endif()
  if(ARG_LV2)
    sonore_add_lv2(${ARG_LV2} OBJECTS ${_obj})
  endif()
endfunction()

# ── sonore_add_aax(<target> SOURCES a.cpp [...] MANUFACTURER Manu [PRODUCT Prod]) ──
# An AAX Native plugin for Pro Tools: a module named <target>.aaxplugin, in the
# bundle Pro Tools scans. Same sources again, with SONORE_BUILD_AAX defined.
#
# Built against the developer's OWN copy of Avid's AAX SDK, which this
# repository does not carry: Avid distributes it to registered AAX developers,
# under their licence or the GPLv3. Set SONORE_AAX_SDK_ROOT to the unpacked SDK
# (the folder holding Interfaces/ and Libs/). Without it the target is skipped,
# loudly, and nothing else in the build changes.
#
# MANUFACTURER is the developer's four-character code and PRODUCT this plugin's;
# PRODUCT may be left out, and is then derived from the descriptor's id. Pro
# Tools files the plugin, and every session saves it, under these codes, so
# they are chosen once. Shipping an AAX also needs Avid's approval and a PACE
# signature on every build (wraptool); a developer build of Pro Tools loads
# unsigned plugins, which is how a build is tested before that.
#
# The AAX library is compiled here from the SDK's sources rather than through
# the SDK's own CMake project, which, added to someone else's build, takes over
# its install prefix and adds install rules for Avid's targets.
function(_sonore_aax_library)
  if(TARGET sonore_aax_library)
    return()
  endif()
  set(_sdk "${SONORE_AAX_SDK_ROOT}")
  file(GLOB _sources "${_sdk}/Libs/AAXLibrary/source/*.cpp")
  if(APPLE)
    list(APPEND _sources "${_sdk}/Libs/AAXLibrary/source/AAX_CAutoreleasePool.OSX.mm")
  endif()
  list(APPEND _sources "${_sdk}/Interfaces/ACF/CACFClassFactory.cpp")
  add_library(sonore_aax_library STATIC ${_sources})
  target_include_directories(sonore_aax_library PUBLIC
    "${_sdk}/Interfaces" "${_sdk}/Interfaces/ACF" "${_sdk}/Libs/AAXLibrary/include")
  target_compile_features(sonore_aax_library PUBLIC cxx_std_17)
  set_target_properties(sonore_aax_library PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(MSVC)
    # Avid's sources, Avid's warnings: not ours to fix or to fail on.
    target_compile_options(sonore_aax_library PRIVATE /W0)
  else()
    target_compile_options(sonore_aax_library PRIVATE -w)
    target_compile_options(sonore_aax_library PUBLIC -Wno-multichar)
  endif()
  if(APPLE)
    target_link_libraries(sonore_aax_library PUBLIC "-framework Cocoa")
  endif()
endfunction()

function(sonore_add_aax TARGET)
  if(NOT (WIN32 OR APPLE))
    message(STATUS "sonore_add_aax(${TARGET}): SKIPPED: AAX is for Pro Tools, which runs on "
                   "Windows and macOS")
    return()
  endif()
  if(NOT DEFINED SONORE_AAX_SDK_ROOT OR NOT EXISTS "${SONORE_AAX_SDK_ROOT}/Interfaces/AAX.h")
    message(STATUS "sonore_add_aax(${TARGET}): SKIPPED: no AAX SDK. Avid gives it to registered "
                   "AAX developers (developer.avid.com/aax); set SONORE_AAX_SDK_ROOT to the "
                   "unpacked folder, the one holding Interfaces/ and Libs/")
    return()
  endif()
  cmake_parse_arguments(ARG "" "MANUFACTURER;PRODUCT;VERSION;BUNDLE_ID" "SOURCES" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "sonore_add_aax(${TARGET}): SOURCES is required")
  endif()
  if(NOT ARG_MANUFACTURER)
    message(WARNING "sonore_add_aax(${TARGET}): no MANUFACTURER given, so this AAX registers "
                    "under Sonorie's code (Sonr), which every AAX built without one shares. "
                    "Pass your own four-character MANUFACTURER code.")
    set(ARG_MANUFACTURER "Sonr")
  endif()
  foreach(_k MANUFACTURER PRODUCT)
    if(ARG_${_k} AND NOT ARG_${_k} MATCHES "^[A-Za-z0-9 ]...$")
      message(FATAL_ERROR "sonore_add_aax(${TARGET}): ${_k} is four letters or digits, "
                          "got '${ARG_${_k}}'")
    endif()
  endforeach()
  if(NOT ARG_VERSION)
    set(ARG_VERSION "1.0.0")
  endif()
  if(NOT ARG_BUNDLE_ID)
    set(ARG_BUNDLE_ID "com.sonorie.${TARGET}")
  endif()

  _sonore_aax_library()
  add_library(${TARGET} MODULE ${ARG_SOURCES} "${SONORE_AAX_SDK_ROOT}/Interfaces/AAX_Exports.cpp")
  target_link_libraries(${TARGET} PRIVATE sonore_sdk sonore_aax_library)
  target_compile_definitions(${TARGET} PRIVATE
    SONORE_BUILD_AAX=1
    "SONORE_AAX_MANUFACTURER=\"${ARG_MANUFACTURER}\""
    "SONORE_AAX_PRODUCT=\"${ARG_PRODUCT}\"")
  set_target_properties(${TARGET} PROPERTIES
    PREFIX ""
    SUFFIX ".aaxplugin"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

  if(APPLE)
    set(MACOSX_BUNDLE_INFO_STRING "${ARG_VERSION}")
    set_target_properties(${TARGET} PROPERTIES
      BUNDLE TRUE
      BUNDLE_EXTENSION "aaxplugin"
      MACOSX_BUNDLE_BUNDLE_NAME "${TARGET}"
      MACOSX_BUNDLE_GUI_IDENTIFIER "${ARG_BUNDLE_ID}"
      MACOSX_BUNDLE_BUNDLE_VERSION "${ARG_VERSION}"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${ARG_VERSION}"
      MACOSX_BUNDLE_INFO_STRING "${ARG_VERSION}"
      # Avid's template: package type TDMw, signature PTul -- what Pro Tools
      # looks for in a plug-in bundle.
      MACOSX_BUNDLE_INFO_PLIST "${SONORE_AAX_SDK_ROOT}/cmake/Info.plist.in")
    add_custom_command(TARGET ${TARGET} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E echo_append "TDMwPTul" > "$<TARGET_BUNDLE_DIR:${TARGET}>/Contents/PkgInfo"
      COMMENT "Writing the ${TARGET} AAX PkgInfo")
    _sonore_plugin_exports(${TARGET} clap_entry ACFRegisterPlugin ACFRegisterComponent
      ACFGetClassFactory ACFCanUnloadNow ACFStartup ACFShutdown ACFGetSDKVersion)
    _sonore_adhoc_sign(${TARGET})
  else()
    # The bundle Pro Tools scans: aax/<target>.aaxplugin/Contents/x64/<binary>,
    # in a folder of its own beside the module (which already has that name),
    # ready to copy into C:\Program Files\Common Files\Avid\Audio\Plug-Ins.
    set(_bundle "$<TARGET_FILE_DIR:${TARGET}>/aax/${TARGET}.aaxplugin")
    add_custom_command(TARGET ${TARGET} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_bundle}/Contents/x64"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_bundle}/Contents/Resources"
      COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${TARGET}>"
              "${_bundle}/Contents/x64/${TARGET}.aaxplugin"
      COMMENT "Assembling the ${TARGET} AAX bundle")
  endif()
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
# It cannot be built from CMake alone (it needs an Xcode extension target, an
# NSExtension Info.plist and a host application to embed it) and it cannot be
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
                      "AUv3 extension target is not implemented yet. Implement it against "
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
                      "binding is not implemented yet. Implement it against the SDK, and "
                      "prove it in a real ARA host before shipping.")
endfunction()

# ── sonore_enable_hosting(<target>) ──────────────────────────────────────────
# A program that LOADS plugins with sonore/host.h -- CLAP, VST3 and LV2, scanned,
# cached and run in a graph. CLAP hosting needs nothing more than sonore_sdk;
# VST3 and LV2 hosting are compiled in only where their C headers can be found
# (host.h asks __has_include), and sonore_sdk does not put them on the path, so
# a program that linked it alone hosted CLAP and quietly found no VST3 or LV2
# plugin at all. This adds both, from the source tree or an installed SDK, and
# the loader library host.h opens modules with.
function(sonore_enable_hosting TARGET)
  target_include_directories(${TARGET} PRIVATE
    "${SONORE_SDK_DIR}/third_party/vst3" "${SONORE_SDK_DIR}/third_party/lv2/include")
  if(UNIX)
    target_link_libraries(${TARGET} PRIVATE ${CMAKE_DL_LIBS})
  endif()
endfunction()

# ── ASIO ─────────────────────────────────────────────────────────────────────
# Built in, and has been since audio_asio.h: the standalone talks to ASIO
# drivers through the COM vtable it declares itself, without Steinberg's SDK
# (see that header for what it contains and why). Every Windows standalone
# offers ASIO next to WASAPI already, so there is nothing to enable.
#
# Kept so projects that call it still configure. It used to say ASIO was
# absent and WASAPI the only output -- false since audio_asio.h landed -- and
# to FAIL the configure if SONORE_ASIO_SDK_ROOT was set, claiming the backend
# "is not implemented yet".
function(sonore_enable_asio TARGET)
  if(NOT WIN32)
    return()
  endif()
  message(STATUS "sonore_enable_asio(${TARGET}): nothing to do -- ASIO is built into every "
                 "Windows standalone (sonore/audio_asio.h), no Steinberg SDK needed")
endfunction()
