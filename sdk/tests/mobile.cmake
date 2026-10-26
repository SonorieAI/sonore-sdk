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

set(_sonore_area_tests dsp_maths midi_osc core_crypto audio_io core_data gui_widgets audio_sources
    gui_infra system modules javascript app_infra)

if(ANDROID)
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
else()
  set(_sonore_data "${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
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
