# SPDX-License-Identifier: Apache-2.0
# The Audio Unit's Info.plist is XML, configured from a template by
# sonore_add_au -- and a plist that does not parse is a component that is
# never registered, with nothing said. Configured here with names a user may
# well have, and checked as a strict parser would read it:
#   * every '&' in it starts an entity: "Smith & Sons" went in as it was;
#   * no comment holds a double hyphen, which XML forbids there: the
#     template's own comment had one.
# Run as: cmake -DSDK=<sdk dir> -DOUT=<file to write> -P au_plist_check.cmake

include("${SDK}/cmake/SonoreSDKHelpers.cmake")

_sonore_xml_escape(SONORE_AU_NAME "Plug")
_sonore_xml_escape(SONORE_AU_SUBTYPE "S&<1")
_sonore_xml_escape(SONORE_AU_MANUFACTURER "Smth")
_sonore_xml_escape(SONORE_AU_TYPE "aufx")
_sonore_xml_escape(SONORE_AU_VENDOR "Smith & Sons <Audio>")
_sonore_xml_escape(SONORE_AU_DISPLAY_NAME "Tone & Tube")
_sonore_xml_escape(SONORE_AU_BUNDLE_ID "com.smith.tone")
set(SONORE_AU_VERSION "1.0.0")
set(SONORE_AU_VERSION_INT 65536)
configure_file("${SDK}/cmake/AudioUnit-Info.plist.in" "${OUT}" @ONLY)
file(READ "${OUT}" xml)

# The entities removed, no '&' may be left. (Not a regex over the matches: a
# CMake list splits each "&amp;" at its semicolon.)
string(REPLACE "&amp;" "" unescaped "${xml}")
string(REPLACE "&lt;" "" unescaped "${unescaped}")
string(REPLACE "&gt;" "" unescaped "${unescaped}")
string(FIND "${unescaped}" "&" rawAmpersand)
if(NOT rawAmpersand EQUAL -1)
  message(FATAL_ERROR "a raw '&' in the Audio Unit plist")
endif()

string(REPLACE "<!--" "" bare "${xml}")
string(REPLACE "-->" "" bare "${bare}")
string(FIND "${bare}" "--" doubleHyphen)
if(NOT doubleHyphen EQUAL -1)
  message(FATAL_ERROR "a double hyphen inside a comment of the Audio Unit plist")
endif()

string(FIND "${xml}" "<string>Smith &amp; Sons &lt;Audio&gt;: Tone &amp; Tube</string>" named)
if(named EQUAL -1)
  message(FATAL_ERROR "the vendor and name did not arrive escaped in the Audio Unit plist")
endif()
message(STATUS "the Audio Unit plist is well-formed XML with any vendor and name")
