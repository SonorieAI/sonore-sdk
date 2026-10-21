// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: which host loaded this plugin, and as which format.
//
// ── Why this exists after all ───────────────────────────────────────────────
//
// The feature map once called this deliberately absent: a plugin branching on
// a host's NAME grows a code path per DAW that nobody can test, and outlives
// the bug it was written for by years. That is still the right first answer,
// and it is why every host difference this SDK knows about is asked of the
// host as a CAPABILITY in the wrappers -- does it offer a context menu, does
// it answer a resize request -- rather than by name.
//
// It stopped being the whole answer for two reasons. Code ported from the
// reference asks isReaper() and isLogic() and has nowhere to go without this
// class. And some host differences are not capabilities at all: a host that
// reports a block size and then exceeds it, a host that calls process before
// activate. There is no question to ask; there is only knowing who you are
// talking to. HostInfo -- the name a CLAP host gives in clap_host, a VST3
// host through IHostApplication, an AU host through
// kAudioUnitProperty_AUHostIdentifier, the standalone for itself -- was the
// first half of that, and a name a host chooses for itself is not reliable:
// LV2 says nothing, an AU host need not, and two hosts may call themselves
// "Plugin Host".
//
// ── What it looks at, in order ──────────────────────────────────────────────
//
//   1. The macOS bundle identifier of the process, which Apple guarantees is
//      unique and stable across a product's renames: com.apple.logic10 is
//      Logic whatever the app is called this year.
//   2. The EXECUTABLE -- its file name and its path -- which is what the
//      reference uses everywhere and the only thing available for an LV2
//      instance, or an AU one whose host does not name itself. A host that
//      runs plugins in a helper process is seen as that process: Bitwig's
//      BitwigPluginHost64 still says "bitwig", REAPER's reaper_host64 still
//      says "reaper".
//   3. The name the host reported, when the executable said nothing useful.
//      Except for this SDK's own standalone, whose report is read before
//      anything else: its executable is named after the plugin, and a plugin
//      called "Sonar Delay" is not running in SONAR.
//
// On Windows the version comes from the executable's version resource, read
// through version.dll loaded at run time so nobody has to add version.lib to
// a link line for a header. On macOS it is CFBundleShortVersionString. Linux
// has no standard place, so it is whatever the host reported.
//
// ── What it sees only when told ─────────────────────────────────────────────
//
// Logic on Apple silicon runs Audio Units in AUHostingService, a process
// that is not Logic and is not called Logic. The AU wrapper reads the name
// the host sets in kAudioUnitProperty_AUHostIdentifier and hands it over as
// HostInfo, so a PluginHostType made from that classifies by the reported
// name (rule 3). A host that never sets the property leaves the process
// unknown rather than guessed. The version there is still the process's own
// bundle version where it has one, as everywhere on macOS.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

#include "audio.h" // HostInfo
#include "path_io.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace sonore {

namespace hosttype_detail {

/** An address inside THIS module -- the plugin binary, or the executable when
 *  the code is linked into one -- so the module can be asked for its path. One
 *  per module because it is an inline function's static. */
inline const void* moduleAnchor() {
  static const char anchor = 0;
  return &anchor;
}

inline std::string lowerAscii(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
  return s;
}

inline bool startsWith(const std::string& s, const char* prefix) {
  return s.compare(0, std::strlen(prefix), prefix) == 0;
}

inline bool endsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

/** The file name, lowercased, without a Windows ".exe". */
inline std::string leafOf(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  std::string leaf = lowerAscii(slash == std::string::npos ? path : path.substr(slash + 1));
  if (endsWith(leaf, ".exe")) leaf.resize(leaf.size() - 4);
  return leaf;
}

#if defined(_WIN32)
/** VS_FIXEDFILEINFO, field for field. Spelled out rather than taken from
 *  winver.h because whether windows.h brings that in depends on macros the
 *  includer chose. The layout is fixed by the resource format. */
struct FixedFileInfo {
  uint32_t signature, structVersion, fileVersionMS, fileVersionLS, productVersionMS,
      productVersionLS, fileFlagsMask, fileFlags, fileOS, fileType, fileSubtype, fileDateMS,
      fileDateLS;
};
#endif

} // namespace hosttype_detail

/**
 * The host this plugin is running in, as far as can be told.
 *
 * Cheap to construct but not free -- it reads the executable's path and, on
 * Windows, its version resource -- so a plugin makes one when it starts, or
 * in setHostInfo(), and keeps it. Main thread: nothing here belongs in
 * process().
 */
class PluginHostType {
public:
  /** The hosts the reference names, plus the ones that have mattered since. */
  enum HostType {
    UnknownHost,       ///< nothing recognised the process or its report
    AbletonLive,       ///< Ableton Live
    AdobeAudition,     ///< Adobe Audition
    AdobePremierePro,  ///< Adobe Premiere Pro
    AppleGarageBand,   ///< GarageBand
    AppleLogic,        ///< Logic Pro
    AppleMainStage,    ///< MainStage
    Ardour,            ///< Ardour (Mixbus has its own value)
    Audacity,          ///< Audacity
    AudioPluginHost,   ///< the reference's AudioPluginHost
    AULab,             ///< Apple's AU Lab
    AUVal,             ///< Apple's auval validator
    AvidProTools,      ///< Pro Tools
    BitwigStudio,      ///< Bitwig Studio, its plugin-host process included
    CakewalkByBandlab, ///< Cakewalk by BandLab
    CakewalkSonar,     ///< SONAR
    Carla,             ///< Carla
    ClapValidator,     ///< clap-validator
    DaVinciResolve,    ///< DaVinci Resolve
    DigitalPerformer,  ///< MOTU Digital Performer
    FinalCut,          ///< Final Cut Pro
    FruityLoops,       ///< FL Studio, its bridge processes included
    Lmms,              ///< LMMS
    MagixSamplitude,   ///< Samplitude
    MagixSequoia,      ///< Sequoia
    Maschine,          ///< Native Instruments Maschine
    MergingPyramix,    ///< Merging Technologies Pyramix
    Mixbus,            ///< Harrison Mixbus
    MuseReceptor,      ///< Muse Receptor
    Pluginval,         ///< pluginval
    Qtractor,          ///< Qtractor
    Reaper,            ///< REAPER, its plugin-host processes included
    Reason,            ///< Reason
    Renoise,           ///< Renoise
    SADiE,             ///< SADiE
    SonoreStandalone,  ///< this SDK's own standalone wrapper
    SteinbergCubase,   ///< Cubase, and its 32-bit bridge (see isCubaseBridged)
    SteinbergNuendo,   ///< Nuendo
    SteinbergTestHost, ///< Steinberg's VST3 test host and validator
    SteinbergWavelab,  ///< WaveLab
    StudioOne,         ///< Studio One
    Tracktion,         ///< Tracktion
    TracktionWaveform, ///< Waveform, Tracktion's later name
    VBVSTScanner,      ///< VBVSTScanner
    ViennaEnsemblePro, ///< Vienna Ensemble Pro
    WaveBurner,        ///< WaveBurner
    Zrythm,            ///< Zrythm
  };

  /** Which of this SDK's wrappers the code is running in. */
  enum class PluginFormat {
    Undefined,   ///< loaded from a file none of the rules recognise
    Clap,        ///< a .clap
    Vst3,        ///< a .vst3
    AudioUnit,   ///< an Audio Unit v2, in a .component
    AudioUnitV3, ///< an Audio Unit v3, in a .appex
    Lv2,         ///< inside a .lv2 bundle
    Standalone   ///< linked into the executable: the standalone or an application
  };

  /** The host of THIS process. */
  PluginHostType() { *this = fromProcess(currentExecutablePath(), mainBundleIdentifier()); }

  /** The host of this process, with what the host said about itself -- the
   *  HostInfo a DSP is handed in setHostInfo(). Its name is used where the
   *  executable says nothing; its version where the executable has none. */
  explicit PluginHostType(const HostInfo& reported) {
    *this = fromProcess(currentExecutablePath(), mainBundleIdentifier(), reported.name,
                        reported.version);
  }

  /**
   * Classify a given executable. What the constructors do with the current
   * process, made callable with any path -- which is how a host that runs
   * other programs can ask about one, and how the rules are tested.
   *
   * On Windows the file's version resource is read when the file exists.
   */
  static PluginHostType fromProcess(const std::string& executablePath,
                                    const std::string& bundleIdentifier = std::string(),
                                    const std::string& reportedName = std::string(),
                                    const std::string& reportedVersion = std::string()) {
    PluginHostType h(0);
    h.path_ = executablePath;
    h.bundleId_ = bundleIdentifier;
    h.reportedName_ = reportedName;
    h.type = classify(executablePath, bundleIdentifier, reportedName, &h.bridged_);
#if defined(_WIN32)
    h.version_ = readVersionResource(executablePath, &h.productName_);
#elif defined(__APPLE__)
    if (!bundleIdentifier.empty() && bundleIdentifier == mainBundleIdentifier())
      h.version_ = mainBundleVersion();
#endif
    if (h.version_.empty()) h.version_ = reportedVersion;
    h.major_ = majorVersionOf(h.version_);
    // A number in the executable's own name -- "Cubase12", "Live 11" -- when
    // nothing better said which version this is.
    if (h.major_ == 0) h.major_ = majorVersionOf(hosttype_detail::leafOf(executablePath));
    return h;
  }

  /** What kind of host. Public, as the reference has it. */
  HostType type = UnknownHost;

  // ── The checks, by the reference's names ─────────────────────────────────
  ///@{
  /** True when `type` is the host the name says (see HostType). Where that
   *  is not plain: isPremiere() is Adobe Premiere Pro; isFruityLoops() FL
   *  Studio; isBitwig() the same as isBitwigStudio(); isSonar() either
   *  Cakewalk; isTracktion() Tracktion or Waveform; isSteinberg() Cubase,
   *  Nuendo, WaveLab or the test host; isCubase7orLater() is also true when
   *  the version is unknown. */
  bool isAbletonLive() const { return type == AbletonLive; }
  bool isAdobeAudition() const { return type == AdobeAudition; }
  bool isPremiere() const { return type == AdobePremierePro; }
  /** Mixbus is Ardour's engine under another name, with Ardour's quirks. */
  bool isArdour() const { return type == Ardour || type == Mixbus; }
  bool isMixbus() const { return type == Mixbus; }
  bool isAudacity() const { return type == Audacity; }
  bool isAudioPluginHost() const { return type == AudioPluginHost; }
  bool isAULab() const { return type == AULab; }
  bool isAUVal() const { return type == AUVal; }
  bool isBitwigStudio() const { return type == BitwigStudio; }
  bool isBitwig() const { return isBitwigStudio(); }
  bool isCarla() const { return type == Carla; }
  bool isClapValidator() const { return type == ClapValidator; }
  bool isCubase() const { return type == SteinbergCubase; }
  bool isCubase7orLater() const { return isCubase() && (major_ == 0 || major_ >= 7); }
  /** Cubase's 32-bit bridge, which ran old plugins in VSTBridgeApp. */
  bool isCubaseBridged() const { return isCubase() && bridged_; }
  bool isDaVinciResolve() const { return type == DaVinciResolve; }
  bool isDigitalPerformer() const { return type == DigitalPerformer; }
  bool isFinalCut() const { return type == FinalCut; }
  bool isFruityLoops() const { return type == FruityLoops; }
  bool isGarageBand() const { return type == AppleGarageBand; }
  bool isLmms() const { return type == Lmms; }
  bool isLogic() const { return type == AppleLogic; }
  bool isMainStage() const { return type == AppleMainStage; }
  bool isMaschine() const { return type == Maschine; }
  bool isMuseReceptor() const { return type == MuseReceptor; }
  bool isNuendo() const { return type == SteinbergNuendo; }
  bool isPluginval() const { return type == Pluginval; }
  bool isProTools() const { return type == AvidProTools; }
  bool isPyramix() const { return type == MergingPyramix; }
  bool isQtractor() const { return type == Qtractor; }
  bool isReaper() const { return type == Reaper; }
  bool isReason() const { return type == Reason; }
  bool isRenoise() const { return type == Renoise; }
  bool isSADiE() const { return type == SADiE; }
  bool isSamplitude() const { return type == MagixSamplitude; }
  bool isSequoia() const { return type == MagixSequoia; }
  bool isSonar() const { return type == CakewalkSonar || type == CakewalkByBandlab; }
  bool isSonoreStandalone() const { return type == SonoreStandalone; }
  bool isSteinberg() const {
    return isCubase() || isNuendo() || isWavelab() || isSteinbergTestHost();
  }
  bool isSteinbergTestHost() const { return type == SteinbergTestHost; }
  bool isStudioOne() const { return type == StudioOne; }
  bool isTracktion() const { return type == Tracktion || type == TracktionWaveform; }
  bool isVBVSTScanner() const { return type == VBVSTScanner; }
  bool isViennaEnsemblePro() const { return type == ViennaEnsemblePro; }
  bool isWaveBurner() const { return type == WaveBurner; }
  bool isWavelab() const { return type == SteinbergWavelab; }
  /** WaveLab 6 and earlier, which the reference treats apart. */
  bool isWavelabLegacy() const { return isWavelab() && major_ > 0 && major_ < 7; }
  bool isZrythm() const { return type == Zrythm; }
  ///@}

  /**
   * The host's name for a person to read: "REAPER", "Bitwig Studio". For a
   * host this class does not know, the product name from its version
   * resource, or the name it reported, or "Unknown" -- never an empty string,
   * because this ends up in support emails.
   */
  const char* getHostDescription() const {
    if (type != UnknownHost) return describe(type);
    // Tested as C strings, which is how they are returned: a name the host
    // sent starting with a NUL is a std::string that is not empty and a
    // description that is.
    if (productName_.c_str()[0]) return productName_.c_str();
    if (reportedName_.c_str()[0]) return reportedName_.c_str();
    return "Unknown";
  }

  /** The executable that was classified. */
  const std::string& getHostPath() const { return path_; }
  /** The macOS bundle identifier, empty elsewhere. */
  const std::string& getBundleIdentifier() const { return bundleId_; }
  /** "7.56", "12.0.70": from the version resource or bundle, else reported.
   *  Empty when nothing says. */
  const std::string& getHostVersion() const { return version_; }
  /** The first number of the version, 0 when unknown. */
  int getMajorVersion() const { return major_; }

  // ── The plugin's side ────────────────────────────────────────────────────

  /**
   * Which wrapper this code is running in, from the file it was loaded from.
   *
   * Asked of the BINARY rather than of a wrapper, because one plugin binary
   * of this SDK carries more than one format -- the CLAP wrapper is in every
   * build, and the VST3 binary exports clap_entry too -- so which entry point
   * the host actually called is only knowable from what the host loaded: a
   * .vst3 is loaded as a VST3, a .component as an Audio Unit.
   */
  static PluginFormat getPluginLoadedAs() {
    return pluginFormatFromModulePath(currentModulePath(), currentExecutablePath());
  }

  /** The rule getPluginLoadedAs() applies, given the two paths. */
  static PluginFormat pluginFormatFromModulePath(const std::string& modulePath,
                                                 const std::string& executablePath) {
    if (modulePath.empty()) return PluginFormat::Undefined;
    std::string m = hosttype_detail::lowerAscii(modulePath);
    std::string e = hosttype_detail::lowerAscii(executablePath);
    for (char& c : m)
      if (c == '\\') c = '/';
    for (char& c : e)
      if (c == '\\') c = '/';
    if (!e.empty() && m == e) return PluginFormat::Standalone;
    if (hosttype_detail::endsWith(m, ".clap")) return PluginFormat::Clap;
    if (m.find(".component/") != std::string::npos) return PluginFormat::AudioUnit;
    if (m.find(".appex/") != std::string::npos) return PluginFormat::AudioUnitV3;
    if (m.find(".vst3/") != std::string::npos || hosttype_detail::endsWith(m, ".vst3"))
      return PluginFormat::Vst3;
    if (m.find(".lv2/") != std::string::npos) return PluginFormat::Lv2;
    if (m.find(".clap/") != std::string::npos) return PluginFormat::Clap;
    if (hosttype_detail::endsWith(m, ".exe") || m.find(".app/contents/macos/") != std::string::npos)
      return PluginFormat::Standalone;
    return PluginFormat::Undefined;
  }

  /** "CLAP", "VST3", "AU", "AUv3", "LV2" or "Standalone"; "Undefined" for
   *  Undefined. A static string. */
  static const char* getPluginFormatName(PluginFormat format) {
    switch (format) {
      case PluginFormat::Clap: return "CLAP";
      case PluginFormat::Vst3: return "VST3";
      case PluginFormat::AudioUnit: return "AU";
      case PluginFormat::AudioUnitV3: return "AUv3";
      case PluginFormat::Lv2: return "LV2";
      case PluginFormat::Standalone: return "Standalone";
      default: return "Undefined";
    }
  }

  // ── The rules ─────────────────────────────────────────────────────────────

  /**
   * The classification itself, a pure function of three strings. Bundle
   * identifiers first, then the executable's name and path, then the name the
   * host reported. `bridged` is set for Cubase's 32-bit bridge process.
   */
  static HostType classify(const std::string& executablePath, const std::string& bundleIdentifier,
                           const std::string& reportedName, bool* bridged = nullptr) {
    using namespace hosttype_detail;
    if (bridged) *bridged = false;
    const std::string bundle = lowerAscii(bundleIdentifier);
    std::string path = lowerAscii(executablePath);
    for (char& c : path)
      if (c == '\\') c = '/';
    const std::string file = leafOf(executablePath);
    const std::string reported = lowerAscii(reportedName);

    enum Field { kBundle, kPath, kFile, kReported };
    enum Match { kContains, kStarts, kEquals };
    struct Rule {
      HostType type;
      Field field;
      Match match;
      const char* needle;
    };
    // ORDER MATTERS: the first rule that matches wins, so the narrower name
    // comes before the broader one -- Waveform before Tracktion, Mixbus
    // before Ardour, clap-validator before Steinberg's "validator".
    static const Rule kRules[] = {
        // This SDK's standalone, FIRST: it is the one host whose report is
        // certain (no other program says this), and the one whose executable
        // says nothing reliable -- it is named after the plugin it carries,
        // and a plugin may be called anything. Behind the executable rules,
        // the standalone of "Sonar Delay" classified as SONAR, of "Waveform
        // Shaper" as Waveform, of "Resolver" as DaVinci Resolve.
        {SonoreStandalone, kReported, kStarts, "sonore standalone"},
        {AppleLogic, kBundle, kStarts, "com.apple.logic"},
        {AppleGarageBand, kBundle, kStarts, "com.apple.garageband"},
        {AppleMainStage, kBundle, kStarts, "com.apple.mainstage"},
        {FinalCut, kBundle, kStarts, "com.apple.finalcut"},
        {WaveBurner, kBundle, kStarts, "com.apple.waveburner"},
        {AULab, kBundle, kContains, "aulab"},
        {AvidProTools, kBundle, kStarts, "com.avid.protools"},
        {AbletonLive, kBundle, kStarts, "com.ableton.live"},
        {BitwigStudio, kBundle, kStarts, "com.bitwig."},
        {SteinbergCubase, kBundle, kStarts, "com.steinberg.cubase"},
        {SteinbergNuendo, kBundle, kStarts, "com.steinberg.nuendo"},
        {SteinbergWavelab, kBundle, kStarts, "com.steinberg.wavelab"},
        {SteinbergTestHost, kBundle, kContains, "plugintesthost"},
        {StudioOne, kBundle, kStarts, "com.presonus.studioone"},
        {FruityLoops, kBundle, kStarts, "com.image-line.flstudio"},
        {Reaper, kBundle, kStarts, "com.cockos.reaper"},
        {DigitalPerformer, kBundle, kContains, "digitalperformer"},
        {DaVinciResolve, kBundle, kContains, "davinciresolve"},
        {Renoise, kBundle, kContains, "renoise"},
        {Reason, kBundle, kContains, ".reason"},
        {TracktionWaveform, kBundle, kContains, "tracktion.waveform"},
        {AdobeAudition, kBundle, kStarts, "com.adobe.audition"},
        {AdobePremierePro, kBundle, kStarts, "com.adobe.premiere"},
        {Mixbus, kBundle, kContains, "mixbus"},
        {Ardour, kBundle, kContains, "ardour"},
        {ViennaEnsemblePro, kBundle, kContains, "vienna"},
        {Maschine, kBundle, kContains, "maschine"},
        {Audacity, kBundle, kContains, "audacity"},
        {Pluginval, kBundle, kContains, "pluginval"},

        {FinalCut, kPath, kContains, "final cut pro"},
        {AbletonLive, kPath, kContains, "ableton"},
        {AdobePremierePro, kPath, kContains, "adobe premiere"},
        {AdobeAudition, kFile, kContains, "audition"},
        {AppleGarageBand, kPath, kContains, "garageband"},
        {AppleMainStage, kPath, kContains, "mainstage"},
        {AppleLogic, kPath, kContains, "logic pro"},
        {AvidProTools, kFile, kContains, "protools"},
        {AvidProTools, kPath, kContains, "pro tools"},
        {CakewalkSonar, kFile, kContains, "sonar"},
        {CakewalkByBandlab, kFile, kEquals, "cakewalk"},
        {MuseReceptor, kFile, kContains, "guitar core"},
        {TracktionWaveform, kFile, kStarts, "waveform"},
        {Tracktion, kFile, kContains, "tracktion"},
        {Reaper, kFile, kContains, "reaper"},
        {SteinbergCubase, kFile, kContains, "vstbridgeapp"},
        {SteinbergCubase, kFile, kContains, "cubase"},
        {SteinbergNuendo, kFile, kContains, "nuendo"},
        {SteinbergWavelab, kFile, kContains, "wavelab"},
        {ClapValidator, kFile, kContains, "clap-validator"},
        {SteinbergTestHost, kFile, kContains, "plugintesthost"},
        {SteinbergTestHost, kFile, kEquals, "validator"},
        {FruityLoops, kFile, kContains, "fl studio"},
        {FruityLoops, kFile, kContains, "flstudio"},
        {FruityLoops, kFile, kEquals, "fl"},
        {FruityLoops, kFile, kEquals, "fl64"},
        {FruityLoops, kFile, kContains, "ilbridge"},
        {FruityLoops, kFile, kContains, "osxfl"},
        {MergingPyramix, kPath, kContains, "merging technologies"},
        {MergingPyramix, kFile, kContains, "pyramix"},
        {MagixSamplitude, kFile, kContains, "samplitude"},
        {MagixSequoia, kFile, kContains, "sequoia"},
        {Renoise, kFile, kContains, "renoise"},
        {DaVinciResolve, kFile, kContains, "resolve"},
        {BitwigStudio, kFile, kContains, "bitwig"},
        {SADiE, kFile, kContains, "sadie"},
        {Pluginval, kFile, kContains, "pluginval"},
        {AudioPluginHost, kFile, kContains, "audiopluginhost"},
        {ViennaEnsemblePro, kFile, kContains, "vienna ensemble"},
        {StudioOne, kPath, kContains, "studio one"},
        {DigitalPerformer, kPath, kContains, "digital performer"},
        {WaveBurner, kFile, kContains, "waveburner"},
        {AULab, kFile, kContains, "au lab"},
        {AUVal, kFile, kStarts, "auval"},
        {Mixbus, kFile, kContains, "mixbus"},
        {Ardour, kFile, kContains, "ardour"},
        {Maschine, kFile, kContains, "maschine"},
        {VBVSTScanner, kFile, kContains, "vbvstscanner"},
        {Reason, kFile, kStarts, "reason"},
        {Audacity, kFile, kContains, "audacity"},
        {Carla, kFile, kStarts, "carla"},
        {Zrythm, kFile, kContains, "zrythm"},
        {Qtractor, kFile, kContains, "qtractor"},
        {Lmms, kFile, kEquals, "lmms"},

        // What the host said about itself, when nothing above recognised the
        // process. (The standalone's own report is the first rule of all.)
        {Reaper, kReported, kContains, "reaper"},
        {BitwigStudio, kReported, kContains, "bitwig"},
        {SteinbergCubase, kReported, kContains, "cubase"},
        {SteinbergNuendo, kReported, kContains, "nuendo"},
        {SteinbergWavelab, kReported, kContains, "wavelab"},
        {StudioOne, kReported, kContains, "studio one"},
        {AbletonLive, kReported, kContains, "ableton"},
        {AbletonLive, kReported, kEquals, "live"},
        {FruityLoops, kReported, kContains, "fl studio"},
        {Mixbus, kReported, kContains, "mixbus"},
        {Ardour, kReported, kContains, "ardour"},
        {TracktionWaveform, kReported, kContains, "waveform"},
        {Tracktion, kReported, kContains, "tracktion"},
        {Renoise, kReported, kContains, "renoise"},
        {Reason, kReported, kStarts, "reason"},
        {AppleLogic, kReported, kContains, "logic"},
        {AppleGarageBand, kReported, kContains, "garageband"},
        {AvidProTools, kReported, kContains, "pro tools"},
        {DigitalPerformer, kReported, kContains, "digital performer"},
        {Audacity, kReported, kContains, "audacity"},
        {Carla, kReported, kStarts, "carla"},
        {ClapValidator, kReported, kContains, "clap-validator"},
        {Pluginval, kReported, kContains, "pluginval"},
        {Zrythm, kReported, kContains, "zrythm"},
        {Qtractor, kReported, kContains, "qtractor"},
        {Lmms, kReported, kContains, "lmms"},
    };
    for (const Rule& rule : kRules) {
      const std::string& field = rule.field == kBundle ? bundle
                                 : rule.field == kPath ? path
                                 : rule.field == kFile ? file
                                                       : reported;
      if (field.empty()) continue;
      const bool hit = rule.match == kContains ? field.find(rule.needle) != std::string::npos
                       : rule.match == kStarts ? startsWith(field, rule.needle)
                                               : field == rule.needle;
      if (!hit) continue;
      if (bridged && rule.type == SteinbergCubase && rule.field == kFile &&
          std::strcmp(rule.needle, "vstbridgeapp") == 0)
        *bridged = true;
      return rule.type;
    }
    return UnknownHost;
  }

  /** A host type's name for a person to read, as getHostDescription()
   *  gives it for a known host; "Unknown" for UnknownHost. A static
   *  string. */
  static const char* describe(HostType t) {
    switch (t) {
      case AbletonLive: return "Ableton Live";
      case AdobeAudition: return "Adobe Audition";
      case AdobePremierePro: return "Adobe Premiere";
      case AppleGarageBand: return "GarageBand";
      case AppleLogic: return "Logic Pro";
      case AppleMainStage: return "MainStage";
      case Ardour: return "Ardour";
      case Audacity: return "Audacity";
      case AudioPluginHost: return "AudioPluginHost";
      case AULab: return "AU Lab";
      case AUVal: return "auval";
      case AvidProTools: return "Pro Tools";
      case BitwigStudio: return "Bitwig Studio";
      case CakewalkByBandlab: return "Cakewalk";
      case CakewalkSonar: return "SONAR";
      case Carla: return "Carla";
      case ClapValidator: return "clap-validator";
      case DaVinciResolve: return "DaVinci Resolve";
      case DigitalPerformer: return "Digital Performer";
      case FinalCut: return "Final Cut Pro";
      case FruityLoops: return "FL Studio";
      case Lmms: return "LMMS";
      case MagixSamplitude: return "Samplitude";
      case MagixSequoia: return "Sequoia";
      case Maschine: return "Maschine";
      case MergingPyramix: return "Pyramix";
      case Mixbus: return "Mixbus";
      case MuseReceptor: return "Muse Receptor";
      case Pluginval: return "pluginval";
      case Qtractor: return "Qtractor";
      case Reaper: return "REAPER";
      case Reason: return "Reason";
      case Renoise: return "Renoise";
      case SADiE: return "SADiE";
      case SonoreStandalone: return "Sonore Standalone";
      case SteinbergCubase: return "Cubase";
      case SteinbergNuendo: return "Nuendo";
      case SteinbergTestHost: return "Steinberg VST3 test host";
      case SteinbergWavelab: return "WaveLab";
      case StudioOne: return "Studio One";
      case Tracktion: return "Tracktion";
      case TracktionWaveform: return "Waveform";
      case VBVSTScanner: return "VBVSTScanner";
      case ViennaEnsemblePro: return "Vienna Ensemble Pro";
      case WaveBurner: return "WaveBurner";
      case Zrythm: return "Zrythm";
      default: return "Unknown";
    }
  }

  /** The leading whole number of a version string or a file name: "12.0.70"
   *  is 12, "cubase13" is 13, "live 11 suite" is 11. 0 when there is none, or
   *  it is not a plausible version. */
  static int majorVersionOf(const std::string& text) {
    size_t i = 0;
    while (i < text.size() && !(text[i] >= '0' && text[i] <= '9')) ++i;
    int value = 0;
    int digits = 0;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 6; ++i, ++digits)
      value = value * 10 + (text[i] - '0');
    // A four-digit run is a year -- "Live 2024", a build number -- not a
    // major version anybody compares against.
    return value > 0 && value < 1000 ? value : 0;
  }

  // ── The process ───────────────────────────────────────────────────────────

  /** The running executable, as a UTF-8 path. Empty if the OS will not say. */
  static std::string currentExecutablePath() {
#if defined(_WIN32)
    return modulePath(nullptr);
#elif defined(__APPLE__)
    char first[1024];
    uint32_t size = sizeof(first);
    if (_NSGetExecutablePath(first, &size) == 0) return canonicalPath(first);
    std::string big(size, '\0');
    if (_NSGetExecutablePath(&big[0], &size) == 0) return canonicalPath(big.c_str());
    return std::string();
#else
    char buffer[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n <= 0) return std::string();
    buffer[n] = '\0';
    return canonicalPath(buffer);
#endif
  }

  /** The binary this code is linked into: the plugin's own file inside a
   *  host, the executable in a standalone. */
  static std::string currentModulePath() {
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR) hosttype_detail::moduleAnchor(), &module))
      return std::string();
    return modulePath(module);
#else
    Dl_info info{};
    if (!dladdr(hosttype_detail::moduleAnchor(), &info) || !info.dli_fname) return std::string();
    // Canonical, like the executable's path: for the main program the loader
    // reports the path it was STARTED by -- "./plugin_state_test" -- which
    // never compares equal to /proc/self/exe, and a standalone would not
    // know itself.
    return canonicalPath(info.dli_fname);
#endif
  }

#if !defined(_WIN32)
  /** realpath(), or the path as given when it cannot be resolved. */
  static std::string canonicalPath(const char* path) {
    if (!path || !*path) return std::string();
    char* resolved = ::realpath(path, nullptr);
    if (!resolved) return std::string(path);
    std::string out(resolved);
    std::free(resolved);
    return out;
  }
#endif

  /** The process's bundle identifier on macOS; empty everywhere else, and for
   *  a process that is not an application bundle. */
  static std::string mainBundleIdentifier() {
#if defined(__APPLE__)
    CFBundleRef bundle = CFBundleGetMainBundle();
    if (!bundle) return std::string();
    return cfString(CFBundleGetIdentifier(bundle));
#else
    return std::string();
#endif
  }

#if defined(__APPLE__)
  static std::string mainBundleVersion() {
    CFBundleRef bundle = CFBundleGetMainBundle();
    if (!bundle) return std::string();
    CFTypeRef value = CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("CFBundleShortVersionString"));
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return std::string();
    return cfString((CFStringRef) value);
  }
#endif

  /**
   * A Windows executable's version: its ProductVersion string, or failing
   * that "major.minor[.build[.revision]]" from VS_FIXEDFILEINFO; and its
   * ProductName when `productName` is given. Empty when the file has no
   * version resource. Elsewhere, always empty.
   */
  static std::string readVersionResource(const std::string& path, std::string* productName = nullptr) {
    if (productName) productName->clear();
#if defined(_WIN32)
    if (path.empty()) return std::string();
    // Loaded, not linked: a header that needed version.lib on every user's
    // link line would be a header that fails to link for everyone who did not
    // read this comment. From System32 only: version.dll is not one of the
    // KnownDLLs, and by bare name a copy in the host's folder would load.
    HMODULE lib = LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) return std::string();
    typedef DWORD(WINAPI * SizeFn)(LPCWSTR, LPDWORD);
    typedef BOOL(WINAPI * InfoFn)(LPCWSTR, DWORD, DWORD, LPVOID);
    typedef BOOL(WINAPI * QueryFn)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    auto getSize = (SizeFn) (void*) GetProcAddress(lib, "GetFileVersionInfoSizeW");
    auto getInfo = (InfoFn) (void*) GetProcAddress(lib, "GetFileVersionInfoW");
    auto query = (QueryFn) (void*) GetProcAddress(lib, "VerQueryValueW");
    std::string version;
    if (getSize && getInfo && query) {
      const std::wstring wide = pathio::widenPath(path.c_str());
      DWORD ignored = 0;
      const DWORD size = getSize(wide.c_str(), &ignored);
      if (size > 0 && size < (64u << 20)) {
        std::string block(size, '\0');
        if (getInfo(wide.c_str(), 0, size, &block[0])) {
          void* fixedPtr = nullptr;
          UINT fixedLen = 0;
          if (query(block.data(), L"\\", &fixedPtr, &fixedLen) && fixedPtr &&
              fixedLen >= sizeof(hosttype_detail::FixedFileInfo)) {
            hosttype_detail::FixedFileInfo fixed;
            std::memcpy(&fixed, fixedPtr, sizeof(fixed));
            if (fixed.signature == 0xFEEF04BDu) {
              const unsigned a = fixed.fileVersionMS >> 16, b = fixed.fileVersionMS & 0xffffu,
                             c = fixed.fileVersionLS >> 16, d = fixed.fileVersionLS & 0xffffu;
              char text[64];
              if (d) std::snprintf(text, sizeof(text), "%u.%u.%u.%u", a, b, c, d);
              else if (c) std::snprintf(text, sizeof(text), "%u.%u.%u", a, b, c);
              else std::snprintf(text, sizeof(text), "%u.%u", a, b);
              version = text;
            }
          }
          // The string table is keyed by language and code page, named in the
          // Translation entry; the first one listed is the product's own.
          void* translation = nullptr;
          UINT translationLen = 0;
          if (query(block.data(), L"\\VarFileInfo\\Translation", &translation, &translationLen) &&
              translation && translationLen >= 4) {
            uint16_t pair[2];
            std::memcpy(pair, translation, sizeof(pair));
            auto text = [&](const wchar_t* name) {
              wchar_t key[96];
              std::swprintf(key, 96, L"\\StringFileInfo\\%04x%04x\\%ls", (unsigned) pair[0],
                            (unsigned) pair[1], name);
              void* value = nullptr;
              UINT valueLen = 0;
              if (!query(block.data(), key, &value, &valueLen) || !value || valueLen == 0)
                return std::string();
              return pathio::narrow((const wchar_t*) value);
            };
            if (productName) *productName = text(L"ProductName");
            // The version AS THE PRODUCT WRITES IT, which is the one its users
            // know: REAPER's binary field says 7.5.6 where its own about box,
            // and this string, say 7.56. Cut at the first space, where Windows
            // components append a build branch in brackets.
            std::string written = text(L"ProductVersion");
            const size_t space = written.find(' ');
            if (space != std::string::npos) written.resize(space);
            if (!written.empty() && written[0] >= '0' && written[0] <= '9') version = written;
          }
        }
      }
    }
    FreeLibrary(lib);
    return version;
#else
    (void) path;
    return std::string();
#endif
  }

private:
  explicit PluginHostType(int) {}

#if defined(_WIN32)
  /** A module's path, wide from the OS and UTF-8 out, with the buffer grown
   *  until the path fits: MAX_PATH is not a limit on where a host may be
   *  installed. */
  static std::string modulePath(HMODULE module) {
    std::wstring buffer(260, L'\0');
    for (int attempt = 0; attempt < 8; ++attempt) {
      const DWORD n = GetModuleFileNameW(module, &buffer[0], (DWORD) buffer.size());
      if (n == 0) return std::string();
      if (n < buffer.size()) {
        buffer.resize(n);
        return pathio::narrow(buffer.c_str());
      }
      buffer.resize(buffer.size() * 2);
    }
    return std::string();
  }
#endif

#if defined(__APPLE__)
  static std::string cfString(CFStringRef s) {
    if (!s) return std::string();
    char buffer[512];
    if (CFStringGetCString(s, buffer, (CFIndex) sizeof(buffer), kCFStringEncodingUTF8))
      return std::string(buffer);
    return std::string();
  }
#endif

  std::string path_;
  std::string bundleId_;
  std::string reportedName_;
  std::string productName_;
  std::string version_;
  int major_ = 0;
  bool bridged_ = false;
};

} // namespace sonore
