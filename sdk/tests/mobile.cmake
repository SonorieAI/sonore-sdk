# The examples and suites a phone or tablet build makes: included by
# CMakeLists.txt IN PLACE OF the desktop ones when the target is Android or
# iOS. The desktop list builds every format and drives each with a host,
# windows and devices a phone does not have; this one builds what runs there.
#
# Android: every program runs on a device or an emulator through adb
# (tests/android_run.cmake, as ctest's emulator), so `ctest` on the build
# machine is the whole loop:
#
#   cmake -G Ninja -S sdk -B build-android
#         -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake
#         -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-28
#   cmake --build build-android && ctest --test-dir build-android
#
# Android 9 (API 28) is the floor: posix_spawn, which the process, preset and
# dialog code uses, reached Android's libc there, and AAudio (API 26) with it.

enable_testing()

set(_sonore_area_tests dsp_maths spectrum midi_osc core_crypto audio_io core_data gui_widgets audio_sources
    gui_infra system modules javascript app_infra)

set(SONORE_IOS_BUILD FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(SONORE_IOS_BUILD TRUE)
endif()

if(SONORE_IOS_BUILD)
  # Every program runs in a booted simulator, through simctl
  # (tests/ios_run.cmake): SONORE_IOS_SIMULATOR names the device (a UDID, or
  # "booted" for the one that is).
  set(SONORE_IOS_SIMULATOR "booted" CACHE STRING "The simulator ctest runs the iOS tests in")
  set(_sonore_data "${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  set(CMAKE_CROSSCOMPILING_EMULATOR "${CMAKE_COMMAND}" "-DSIMULATOR=${SONORE_IOS_SIMULATOR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ios_run.cmake")
  # CoreAudioKit beside what CMakeLists.txt links for iOS: the AUv3 wrapper's
  # view controller, which the self-tests and the apps compile in.
  target_link_libraries(sonore_sdk INTERFACE "-framework CoreAudioKit")
elseif(ANDROID)
  if(ANDROID_PLATFORM_LEVEL AND ANDROID_PLATFORM_LEVEL LESS 28)
    message(FATAL_ERROR "The Sonore SDK needs Android 9 (API 28) or later: configure with "
                        "-DANDROID_PLATFORM=android-28 (or higher)")
  endif()
  find_program(SONORE_ADB adb
    HINTS "$ENV{ANDROID_HOME}/platform-tools" "$ENV{ANDROID_SDK_ROOT}/platform-tools"
          "${ANDROID_NDK}/../../platform-tools")
  set(SONORE_DEVICE_DIR "/data/local/tmp/sonore")
  set(_sonore_data "${SONORE_DEVICE_DIR}/data")
  # Every executable below runs on the device: ctest prefixes this.
  set(CMAKE_CROSSCOMPILING_EMULATOR "${CMAKE_COMMAND}" "-DADB=${SONORE_ADB}"
      "-DDATA=${CMAKE_CURRENT_SOURCE_DIR}/tests/data" "-DDEVICE_DIR=${SONORE_DEVICE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/android_run.cmake")
endif()

add_executable(sonore_sdk_tests tests/sdk_tests.cpp)
target_link_libraries(sonore_sdk_tests PRIVATE sonore_sdk Threads::Threads)
target_compile_definitions(sonore_sdk_tests PRIVATE SONORE_TEST_DATA_DIR="${_sonore_data}"
                                                    SONORE_TEST_PROJECT_VERSION="${PROJECT_VERSION}")
target_include_directories(sonore_sdk_tests PRIVATE "${SONORE_SDK_DIR}/third_party/vst3")
add_test(NAME sdk_tests COMMAND sonore_sdk_tests)

add_executable(no_rtti_test tests/no_rtti_test.cpp)
target_link_libraries(no_rtti_test PRIVATE sonore_sdk)
target_compile_options(no_rtti_test PRIVATE -fno-rtti)
add_test(NAME no_rtti COMMAND no_rtti_test)

foreach(_area ${_sonore_area_tests})
  add_executable(${_area}_test tests/${_area}_test.cpp)
  target_link_libraries(${_area}_test PRIVATE sonore_sdk Threads::Threads)
  add_test(NAME ${_area} COMMAND ${_area}_test)
endforeach()

add_executable(images_test tests/images_test.cpp)
target_link_libraries(images_test PRIVATE sonore_sdk Threads::Threads)
target_compile_definitions(images_test PRIVATE SONORE_TEST_DATA_DIR="${_sonore_data}")
add_test(NAME images COMMAND images_test)

add_executable(code_editor_test tests/code_editor_test.cpp)
target_link_libraries(code_editor_test PRIVATE sonore_sdk Threads::Threads)
target_include_directories(code_editor_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
add_test(NAME code_editor COMMAND code_editor_test)

# The same tests run serially as on the desktop: they race threads on purpose.
foreach(_serial audio_sources gui_infra modules)
  if(TEST ${_serial})
    set_tests_properties(${_serial} PROPERTIES RUN_SERIAL TRUE)
  endif()
endforeach()

# ── The examples as apps ─────────────────────────────────────────────────────
# Each standalone is two things here: the program, which renders and checks
# itself on the device (--verify, as on every desktop), and the app -- on
# Android an APK (sonore_add_standalone), whose <target>Install target puts it
# on the attached device and starts it.
sonore_add_standalone(SonoreSaturatorApp SOURCES examples/saturator/plugin.cpp
  NAME "Sonore Saturator" BUNDLE_ID com.sonorie.saturator VERSION 1.0.0)
sonore_add_standalone(SonoreSynthApp SOURCES examples/synth/plugin.cpp
  NAME "Sonore Synth" BUNDLE_ID com.sonorie.synth VERSION 1.0.0)
add_test(NAME standalone_effect COMMAND SonoreSaturatorApp --verify)
add_test(NAME standalone_instrument COMMAND SonoreSynthApp --verify)

# iOS: each example's AUv3 inside its app, and the AUv3 wrapper driven in
# process in the simulator by the same self-test CI's Mac runs.
if(SONORE_IOS_BUILD)
  sonore_add_auv3(SonoreSaturatorAUv3 SOURCES examples/saturator/plugin.cpp CONTAINER SonoreSaturatorApp
    SUBTYPE "Sat3" MANUFACTURER "Sonr" VENDOR "Sonorie" NAME "Sonore Saturator"
    BUNDLE_ID com.sonorie.saturator)
  sonore_add_auv3(SonoreSynthAUv3 SOURCES examples/synth/plugin.cpp CONTAINER SonoreSynthApp
    SUBTYPE "Syn3" MANUFACTURER "Sonr" VENDOR "Sonorie" TYPE "aumu" NAME "Sonore Synth"
    BUNDLE_ID com.sonorie.synth)
  foreach(_ex saturator synth arp)
    add_executable(auv3_selftest_${_ex} tests/auv3_selftest.cpp examples/${_ex}/plugin.cpp)
    target_link_libraries(auv3_selftest_${_ex} PRIVATE sonore_sdk)
    target_compile_definitions(auv3_selftest_${_ex} PRIVATE SONORE_BUILD_AUV3=1)
  endforeach()
  add_test(NAME auv3_effect COMMAND auv3_selftest_saturator --effect --params 4 --presets 3 --subtype Sat9)
  add_test(NAME auv3_instrument COMMAND auv3_selftest_synth --instrument --params 22 --presets 0 --subtype Syn9)
  add_test(NAME auv3_midi_out COMMAND auv3_selftest_arp --instrument --midi-out --params 3 --subtype Arp9)
endif()
