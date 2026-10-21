# SPDX-License-Identifier: Apache-2.0
# Start a plugin project: a folder that builds every format, the standalone
# and its self-checks from one src/plugin.cpp.
#
#   cmake -DNAME="My Filter" [-DTYPE=effect|instrument] [-DVENDOR="My Company"]
#         [-DID=com.mycompany.myfilter] [-DDIR=<new folder>] [-DTARGET=MyFilter]
#         [-DAU_SUBTYPE=Myfi] [-DAU_MANUFACTURER=Myco] [-DURL=https://...]
#         [-DDESCRIPTION="..."] [-DSDK=<sdk folder>|installed]
#         -P <sdk>/cmake/sonore_new_plugin.cmake
#
# Only CMake is needed, which anyone building the SDK already has. Run from a
# source checkout, the project points at that checkout (add_subdirectory); run
# from an installed SDK (<prefix>/share/SonoreSDK/cmake), it uses
# find_package. -DSDK overrides either: a path, or `installed`.
#
# The new folder must not exist or must be empty: this never overwrites.

cmake_minimum_required(VERSION 3.21)

function(_fail what)
  message(FATAL_ERROR "sonore_new_plugin: ${what}")
endfunction()

# Every value lands in a C++ string literal and in a CMake argument, so the
# characters either would read as syntax are refused rather than escaped
# twice over.
function(_plain var what)
  if("${${var}}" MATCHES "[\"\\\\;$@\n\r\t]")
    _fail("${what} \"${${var}}\" may not contain \" \\ ; $ @ or a line break")
  endif()
endfunction()

if(NOT DEFINED NAME OR NAME STREQUAL "")
  _fail("-DNAME=\"My Plugin\" is required (what hosts list it as)")
endif()
if(NOT DEFINED TYPE)
  set(TYPE effect)
endif()
if(NOT TYPE MATCHES "^(effect|instrument)$")
  _fail("-DTYPE must be effect or instrument, not \"${TYPE}\"")
endif()
if(NOT DEFINED VENDOR OR VENDOR STREQUAL "")
  set(VENDOR "My Company")
endif()
if(NOT DEFINED URL)
  set(URL "")
endif()
if(NOT DEFINED DESCRIPTION OR DESCRIPTION STREQUAL "")
  if(TYPE STREQUAL "instrument")
    set(DESCRIPTION "An instrument.")
  else()
    set(DESCRIPTION "An effect.")
  endif()
endif()
foreach(v NAME VENDOR URL DESCRIPTION)
  _plain(${v} ${v})
endforeach()

# The target: the name's letters and digits, starting with a letter. Held as
# _target, because a bare TARGET in an if() is the keyword, not the variable.
set(_target "")
if(DEFINED TARGET)
  set(_target "${TARGET}")
endif()
if(_target STREQUAL "")
  string(REGEX REPLACE "[^A-Za-z0-9]" "" _target "${NAME}")
  if(_target STREQUAL "")
    _fail("NAME \"${NAME}\" has no ASCII letter or digit to name the CMake targets "
          "after; pass -DTARGET=MyPlugin as well")
  endif()
  if(_target MATCHES "^[0-9]")
    set(_target "P${_target}")
  endif()
endif()
if(NOT _target MATCHES "^[A-Za-z][A-Za-z0-9_]*$")
  _fail("-DTARGET=\"${_target}\" is not a target name (a letter, then letters, digits, _)")
endif()
set(TARGET "${_target}") # what the templates call it

# The identity: reverse-DNS, unique per product, permanent once shipped.
string(REGEX REPLACE "[^A-Za-z0-9]" "" _vendorWord "${VENDOR}")
string(TOLOWER "${_vendorWord}" _vendorLower)
string(TOLOWER "${TARGET}" _targetLower)
if(_vendorLower STREQUAL "")
  set(_vendorLower "example")
endif()
if(NOT DEFINED ID OR ID STREQUAL "")
  set(ID "com.${_vendorLower}.${_targetLower}")
  set(_idDerived TRUE)
endif()
if(NOT ID MATCHES "^[A-Za-z0-9][A-Za-z0-9_-]*([.][A-Za-z0-9_-]+)+$")
  _fail("-DID=\"${ID}\" is not a reverse-DNS id like com.mycompany.myplugin")
endif()

# The Audio Unit's manufacturer code, from the first four letters and digits
# of the vendor: lower case, then the first LETTER raised. It needs an
# upper-case letter (all-lower-case codes are Apple's), and raising only the
# first character left a vendor that starts with a digit -- 112dB, 8Dio --
# without one, and the generator then refused a value it had made itself.
function(_fourcc out word)
  set(code "${word}xxxx")
  string(SUBSTRING "${code}" 0 4 code)
  string(TOLOWER "${code}" code)
  string(REGEX MATCH "^[0-9]*" digits "${code}")
  string(LENGTH "${digits}" at)
  if(at LESS 4)
    string(SUBSTRING "${code}" 0 ${at} head)
    string(SUBSTRING "${code}" ${at} 1 letter)
    math(EXPR rest "${at} + 1")
    string(SUBSTRING "${code}" ${rest} -1 tail)
    string(TOUPPER "${letter}" letter)
    set(code "${head}${letter}${tail}")
  else()
    string(SUBSTRING "${code}" 0 3 code) # four digits: no letter to raise
    set(code "M${code}")
  endif()
  set(${out} "${code}" PARENT_SCOPE)
endfunction()
# The subtype is the one code that tells a vendor's products apart -- they
# share the manufacturer -- so it comes from the id, which is unique per
# product by definition, and not from the name: four letters of the name made
# "Valhalla Room" and "Valhalla Plate", or "Tape Echo" and "Tape Delay", the
# same Audio Unit (aufx Valh Valh), and macOS registers one component per
# type, subtype and manufacturer, so the second one installed hides the
# first. The name's initial, raised, then three letters and digits of a hash
# of the id: the same id always gives the same code, and two ids share one
# once in 46656 per initial.
function(_subtype out word id)
  string(SUBSTRING "${word}" 0 1 code)
  string(TOUPPER "${code}" code)
  string(SHA256 hash "${id}")
  string(SUBSTRING "${hash}" 0 8 hash)
  math(EXPR n "0x${hash}")
  set(digits "abcdefghijklmnopqrstuvwxyz0123456789")
  foreach(i RANGE 2)
    math(EXPR d "${n} % 36")
    math(EXPR n "${n} / 36")
    string(SUBSTRING "${digits}" ${d} 1 c)
    string(APPEND code "${c}")
  endforeach()
  set(${out} "${code}" PARENT_SCOPE)
endfunction()
if(NOT DEFINED AU_SUBTYPE OR AU_SUBTYPE STREQUAL "")
  _subtype(AU_SUBTYPE "${TARGET}" "${ID}")
endif()
if(NOT DEFINED AU_MANUFACTURER OR AU_MANUFACTURER STREQUAL "")
  if(_vendorWord STREQUAL "")
    set(_vendorWord "Manu")
  endif()
  _fourcc(AU_MANUFACTURER "${_vendorWord}")
endif()
foreach(code AU_SUBTYPE AU_MANUFACTURER)
  if(NOT "${${code}}" MATCHES "^[ -~][ -~][ -~][ -~]$" OR "${${code}}" MATCHES "[\"\\\\;$@]")
    _fail("-D${code}=\"${${code}}\" must be exactly four plain ASCII characters")
  endif()
endforeach()
if(NOT AU_MANUFACTURER MATCHES "[A-Z]")
  _fail("-DAU_MANUFACTURER=\"${AU_MANUFACTURER}\" needs an upper-case letter "
        "(all-lower-case codes are reserved by Apple)")
endif()

if(TYPE STREQUAL "instrument")
  set(AU_TYPE aumu)
  set(KIND instrument)
else()
  set(AU_TYPE aufx)
  set(KIND effect)
endif()
set(VERSION 0.1.0)

# Where the SDK is: this checkout, an installed one, or what -DSDK says.
get_filename_component(_here "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_fromCheckout FALSE)
if(EXISTS "${_here}/CMakeLists.txt" AND EXISTS "${_here}/include/sonore/plugin.h")
  set(_fromCheckout TRUE)
endif()
if(DEFINED SDK AND SDK STREQUAL "installed")
  set(SDK_SOURCE "")
elseif(DEFINED SDK AND NOT SDK STREQUAL "")
  get_filename_component(SDK_SOURCE "${SDK}" ABSOLUTE)
  if(NOT EXISTS "${SDK_SOURCE}/include/sonore/plugin.h")
    _fail("-DSDK=\"${SDK}\" is not the sdk/ folder of a Sonore SDK checkout")
  endif()
elseif(_fromCheckout)
  set(SDK_SOURCE "${_here}")
else()
  set(SDK_SOURCE "")
endif()
if(SDK_SOURCE)
  set(SDK_NOTE "The SDK is the checkout at `${SDK_SOURCE}` (`SONORE_SDK_SOURCE` in CMakeLists.txt; set it to empty to use an installed SDK instead).")
elseif(_fromCheckout)
  # -DSDK=installed, run from a checkout: where it is installed is not known
  # here, and the checkout's parent folder would be a wrong guess.
  set(SDK_NOTE "The SDK is an installed one: if CMake does not find it, add `-DCMAKE_PREFIX_PATH=<where it is installed>` to the first command.")
else()
  get_filename_component(_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
  set(SDK_NOTE "The SDK is an installed one: if CMake does not find it, add `-DCMAKE_PREFIX_PATH=${_prefix}` (where it is installed) to the first command.")
endif()

# The folder.
if(NOT DEFINED DIR OR DIR STREQUAL "")
  set(DIR "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}")
endif()
get_filename_component(DIR "${DIR}" ABSOLUTE)
if(EXISTS "${DIR}" AND NOT IS_DIRECTORY "${DIR}")
  _fail("${DIR} is a file, not a folder; nothing was written")
endif()
# Never over anything. Each file this would write is checked by name first:
# the folder test below is a glob, and a glob reads [2026] in a path as a
# character class -- it found nothing in "Projects [2026]/MyFx", and the
# generator wrote over the plugin.cpp that was there.
foreach(f src CMakeLists.txt README.md .gitignore)
  if(EXISTS "${DIR}/${f}")
    _fail("${DIR}/${f} exists; nothing was written")
  endif()
endforeach()
if(EXISTS "${DIR}")
  # The glob's own syntax escaped, so the path is matched as written.
  string(REGEX REPLACE "([][*?])" "[\\1]" _literal "${DIR}")
  file(GLOB _present "${_literal}/*" "${_literal}/.*")
  if(_present)
    _fail("${DIR} exists and is not empty; nothing was written")
  endif()
endif()

set(_t "${CMAKE_CURRENT_LIST_DIR}/templates")
configure_file("${_t}/${TYPE}.cpp.in" "${DIR}/src/plugin.cpp" @ONLY)
configure_file("${_t}/CMakeLists.txt.in" "${DIR}/CMakeLists.txt" @ONLY)
configure_file("${_t}/README.md.in" "${DIR}/README.md" @ONLY)
configure_file("${_t}/gitignore.in" "${DIR}/.gitignore" @ONLY)

message(STATUS "Created the ${KIND} \"${NAME}\" in ${DIR}")
message(STATUS "  id ${ID}, targets ${TARGET} (CLAP) ${TARGET}VST3 ${TARGET}AU ${TARGET}LV2 ${TARGET}App")
message(STATUS "  Audio Unit ${AU_TYPE} ${AU_SUBTYPE} ${AU_MANUFACTURER}")
if(_idDerived)
  message(STATUS "  The id was made from the vendor and name. It is the plugin's identity "
                 "in every saved session: change it now if it is not the one to keep.")
endif()
if(SDK_SOURCE)
  message(STATUS "  SDK: the checkout at ${SDK_SOURCE}")
else()
  message(STATUS "  SDK: installed (find_package)")
endif()
message(STATUS "Next:")
message(STATUS "  cmake -S \"${DIR}\" -B \"${DIR}/build\"")
message(STATUS "  cmake --build \"${DIR}/build\" --config Release")
message(STATUS "  ctest --test-dir \"${DIR}/build\" -C Release")
