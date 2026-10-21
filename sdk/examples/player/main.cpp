// SPDX-License-Identifier: Apache-2.0
// Sonore SDK example -- the player: an APPLICATION, not a plugin.
//
// Nothing here is hosted. The program owns its process, its window, its
// settings file and its audio device, the way a librarian, an editor or a
// test tool does -- which is the other half of the SDK:
//
//   Application + SONORE_START_APPLICATION   main(), the message loop, one
//                                            instance (a second launch hands
//                                            its file to the first)
//   DocumentWindow + MenuBarModel            the window and its menus
//   readAudioFile + enableSystemAudioFormats WAV/AIFF/FLAC/MP3/Ogg built in,
//                                            M4A/WMA from the OS, opted into
//   MemoryAudioSource -> AudioTransportSource -> AudioSourcePlayer -> device
//                                            the pull chain, into WASAPI
//                                            (Core Audio, ALSA/JACK elsewhere)
//   AudioThumbnail + AudioThumbnailCache     the waveform, drawn by WaveformView
//   ApplicationProperties                    the last folder, the volume, the
//                                            loop switch, the recent files
//   RecentlyOpenedFilesList                  File menu, and the OS's own list
//   FileDialog (the reference's FileChooser) the open dialog
//   NativeMessageBox                         errors, in the platform's box
//
//   SonorePlayer [file]                      the application
//   SonorePlayer --verify <file> [--snapshot out.bmp]
//                                            headless: no window, no device.
//                                            The same engine is played through
//                                            a null device and every sample
//                                            checked; the window's content is
//                                            drawn into a Bitmap and looked
//                                            at. The exit code is the verdict.
#include <sonore/application.h>
#include <sonore/application_properties.h>
#include <sonore/audio_source.h>
#include <sonore/audiofile.h>
#include <sonore/file_dialog.h>
#include <sonore/files.h>
#include <sonore/recent_files.h>
#include <sonore/system_audio_format.h>
#include <sonore/thumbnail.h>
#include <sonore/thumbnail_cache.h>
#include <sonore/gfx/audio_display.h>
#include <sonore/gfx/document_window.h>
#include <sonore/gfx/lookandfeel_versions.h>
#include <sonore/gfx/message_box.h>
#include <sonore/gfx/system_font.h>
#include <sonore/gfx/widgets.h>

#if defined(_WIN32)
#include <sonore/audio_wasapi.h>
#elif defined(__APPLE__)
#include <sonore/audio_coreaudio.h>
#else
#include <sonore/audio_linux.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace player {

namespace gfx = sonore::gfx;

#if defined(_WIN32)
using OutputDevice = sonore::standalone::WasapiOutput;
#elif defined(__APPLE__)
using OutputDevice = sonore::standalone::CoreAudioOutput;
#else
using OutputDevice = sonore::standalone::LinuxOutput;
#endif

/** The block size the player is prepared for. A device may hand it more at
 *  a time -- WASAPI's shared mode fills whatever is free -- and the
 *  AudioSourcePlayer cuts that into pieces of this size. */
static const int kBlock = 512;

/**
 * A device with no hardware behind it: blocks are pulled when the caller
 * asks. The same surface as the real backends (open, sampleRate, channels,
 * run, stop), so Engine::start() cannot tell them apart -- which is the point:
 * --verify plays through exactly the path the application does.
 */
struct NullDevice {
  double rate = 48000.0;
  uint32_t numChannels = 2;
  std::function<void(float*, uint32_t, uint32_t)> render;
  std::string none;

  bool open(std::function<void(float*, uint32_t, uint32_t)> fn) {
    render = std::move(fn);
    return true;
  }
  double sampleRate() const { return rate; }
  uint32_t channels() const { return numChannels; }
  bool run() { return true; }
  void stop() {}
  const std::string& error() const { return none; }

  /** One block, as a device thread would ask for it. */
  void pull(float* interleaved, uint32_t frames) { render(interleaved, frames, numChannels); }
};

/** A decoded file and everything made from it. */
struct OpenFile {
  std::string path;
  sonore::WavData data;
  std::unique_ptr<sonore::MemoryAudioSource> source;
  sonore::AudioThumbnail thumbnail;
};

/**
 * The audio: a transport over the open file, played by an AudioSourcePlayer
 * into whichever device it was started on. Knows nothing about windows.
 */
class Engine {
public:
  Engine() = default;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  ~Engine() {
    // Outside in: nothing may be pulling from the file when it goes.
    player_.setSource(nullptr);
    transport_.setSource(nullptr);
  }

  /** Open the device, prepare for ITS rate, then start it -- in that order,
   *  or the first callback races an unprepared chain. */
  template <typename Device>
  bool start(Device& device, std::string& error) {
    if (!device.open(player_.renderFunction())) {
      error = device.error();
      return false;
    }
    deviceRate_ = device.sampleRate();
    player_.audioDeviceAboutToStart(deviceRate_, kBlock, (int) device.channels());
    player_.setSource(&transport_);
    if (!device.run()) {
      error = device.error();
      return false;
    }
    return true;
  }

  /**
   * [message thread] Decode `path` and make it what plays. Decoding happens
   * here, on the message thread, because a player opens one file at a time
   * and the user is waiting for it; a sampler loading in the background would
   * put this on a worker. False, with a reason, leaves the old file playing.
   */
  bool open(const std::string& path, std::string& error) {
    std::unique_ptr<OpenFile> next(new OpenFile());
    next->path = path;
    if (!sonore::readAudioFile(path.c_str(), &next->data) || next->data.numChannels == 0 ||
        next->data.numFrames() == 0) {
      error = "This file could not be decoded: it is not WAV, AIFF, FLAC, MP3 or Ogg, and the "
              "system has no decoder for it either.";
      return false;
    }
    next->source.reset(new sonore::MemoryAudioSource(next->data, looping_));

    // The picture: from the cache when this exact file (path, size, date)
    // was drawn before, else built from the samples and remembered.
    const uint64_t key = sonore::AudioThumbnailCache::hashFile(path.c_str());
    if (!thumbnails_.loadThumb(next->thumbnail, key)) {
      // Straight from the decoded samples, interleaved as they came: pulling
      // them apart into a vector per channel first was most of the cost.
      next->thumbnail.buildInterleaved(next->data.samples.data(), next->data.numChannels,
                                       next->data.numFrames(), 2048);
      thumbnails_.storeThumb(next->thumbnail, key);
    }

    // Resampled only when the file and the device disagree about the rate.
    const double fileRate = (double) next->data.sampleRate;
    transport_.setSource(next->source.get(), 0, nullptr, fileRate == deviceRate_ ? 0.0 : fileRate,
                         (int) next->data.numChannels);
    file_ = std::move(next); // the old file goes only now, after the swap
    return true;
  }

  void setLooping(bool loop) {
    looping_ = loop;
    transport_.setLooping(loop);
  }
  bool isLooping() const { return looping_; }

  sonore::AudioTransportSource& transport() { return transport_; }
  sonore::AudioSourcePlayer& player() { return player_; }
  const OpenFile* file() const { return file_.get(); }
  sonore::AudioThumbnailCache& thumbnails() { return thumbnails_; }

private:
  sonore::AudioTransportSource transport_;
  sonore::AudioSourcePlayer player_;
  sonore::AudioThumbnailCache thumbnails_;
  std::unique_ptr<OpenFile> file_;
  double deviceRate_ = 0.0;
  bool looping_ = false;
};

inline std::string formatTime(double seconds) {
  if (!(seconds > 0.0)) seconds = 0.0;
  const int minutes = (int) (seconds / 60.0);
  char text[32];
  std::snprintf(text, sizeof(text), "%d:%04.1f", minutes, seconds - minutes * 60.0);
  return text;
}

/**
 * The window's content: the controls, the waveform, and the menus (it is the
 * MenuBarModel). Owns no audio -- it drives an Engine it is handed -- and no
 * window, so --verify draws it into a Bitmap exactly as the window would.
 */
class PlayerView : public gfx::Widget, public gfx::MenuBarModel, private sonore::ChangeListener {
public:
  PlayerView(Engine& engine, sonore::PropertiesFile* settings) : engine_(engine), settings_(settings) {
    if (settings_) {
      recent_.restoreFromString(settings_->getValue("recentFiles"));
      engine_.setLooping(settings_->getBoolValue("loop", false));
    }
    engine_.transport().addChangeListener(this);

    open_.onClick = [this]() { browse(); };
    play_.onClick = [this]() { togglePlay(); };
    stop_.onClick = [this]() { stop(); };
    loop_.setToggleable(true);
    loop_.setToggled(engine_.isLooping(), false);
    loop_.onToggle = [this](bool on) { setLooping(on); };
    open_.setTooltip("Open an audio file");
    loop_.setTooltip("Play the file round and round");

    // Seeking: the slider follows playback on the tick, except while the
    // hand is on it -- otherwise the playhead fights the drag.
    position_.setStyle(gfx::Slider::Style::LinearHorizontal);
    position_.setTooltip("Where in the file");
    position_.onDragStart = [this]() { seeking_ = true; };
    position_.onDragEnd = [this]() { seeking_ = false; };
    position_.onValueChange = [this](float v) {
      if (seeking_) engine_.transport().setPosition(v * engine_.transport().getLengthInSeconds());
    };
    waveform_.onClick = [this](float at) {
      engine_.transport().setPosition(at * engine_.transport().getLengthInSeconds());
    };
    waveform_.setTooltip("Click to jump there");

    // Volume as a fader: 0..1 on the slider is -60..+6 dB, with 0 silence.
    volume_.setStyle(gfx::Slider::Style::LinearHorizontal);
    volume_.setTooltip("Volume");
    volume_.onValueChange = [this](float v) { setVolume(v); };
    volume_.setValue(settings_ ? (float) settings_->getDoubleValue("volume", 0.85) : 0.85f);
    volume_.setDefaultValue(0.85f);

    timeLabel_.setJustify(gfx::Justify::Right);
    for (gfx::Component* c : std::initializer_list<gfx::Component*>{
             &open_, &play_, &stop_, &loop_, &fileLabel_, &timeLabel_, &waveform_, &position_,
             &volumeLabel_, &volume_})
      addChild(c);
    volumeLabel_.setText("Volume");
    showFile();
  }

  ~PlayerView() override { engine_.transport().removeChangeListener(this); }

  /** [message thread] Open `path`; a failure is said in a message box. */
  bool openFile(const std::string& path, bool showErrors = true) {
    std::string error;
    if (!engine_.open(path, error)) {
      if (showErrors)
        gfx::NativeMessageBox::showMessageBoxAsync(gfx::MessageBoxIconType::WarningIcon,
                                                   "Could not open " + sonore::fileName(path), error, this);
      recent_.removeFile(path);
      saveRecent();
      return false;
    }
    recent_.addFile(path);
    sonore::RecentlyOpenedFilesList::registerRecentFileNatively(path);
    saveRecent();
    if (settings_) settings_->setValue("lastFolder", sonore::parentPath(path));
    showFile();
    return true;
  }

  /** The window's clock: follow the transport, and let the settings file
   *  save itself a few seconds after the last change. */
  void tick(double elapsedMs) {
    engine_.transport().dispatchPendingChanges();
    const double length = engine_.transport().getLengthInSeconds();
    const double now = engine_.transport().getCurrentPosition();
    const float at = length > 0.0 ? (float) (now / length) : 0.0f;
    if (!seeking_) position_.setValue(at, false);
    waveform_.setPlayhead(engine_.file() ? at : -1.0f);
    const std::string time = formatTime(now) + " / " + formatTime(length);
    if (time != timeLabel_.text()) timeLabel_.setText(time);
    if (settings_) settings_->advance(elapsedMs);
  }

  void resized() override {
    const gfx::Rect r = localBounds();
    const float pad = 12.0f, row = 28.0f;
    open_.setBounds({pad, pad, 80.0f, row});
    play_.setBounds({pad + 88.0f, pad, 80.0f, row});
    stop_.setBounds({pad + 176.0f, pad, 70.0f, row});
    loop_.setBounds({pad + 254.0f, pad, 70.0f, row});
    timeLabel_.setBounds({r.w - pad - 160.0f, pad, 160.0f, row});
    fileLabel_.setBounds({pad + 336.0f, pad, r.w - pad * 2.0f - 336.0f - 168.0f, row});
    const float bottom = r.h - pad - row;
    waveform_.setBounds({pad, pad + row + 10.0f, r.w - pad * 2.0f, bottom - (pad + row + 10.0f) - 44.0f});
    position_.setBounds({pad, bottom - 36.0f, r.w - pad * 2.0f, 24.0f});
    volumeLabel_.setBounds({pad, bottom, 64.0f, row});
    volume_.setBounds({pad + 70.0f, bottom, 220.0f, row});
  }

  void paint(gfx::Graphics& g) override { g.fillAll(lookAndFeel().background()); }

  // ── The menus ─────────────────────────────────────────────────────────────

  enum MenuIds { kOpen = 1, kClearRecent, kQuit, kPlayPause, kStop, kLoop, kRewind, kAbout, kRecentBase = 100 };

  std::vector<std::string> getMenuBarNames() override { return {"&File", "&Playback", "&Help"}; }

  gfx::PopupMenu getMenuForIndex(int index, const std::string&) override {
    gfx::PopupMenu menu;
    if (index == 0) {
      menu.addItem(kOpen, "Open...");
      if (recent_.getNumFiles() > 0) {
        menu.addSeparator();
        menu.addSectionHeader("Recent");
        // Files deleted since are left out rather than offered and failed.
        recent_.createPopupMenuItems(menu, kRecentBase, false, true);
        menu.addItem(kClearRecent, "Clear recent files");
      }
      menu.addSeparator();
      menu.addItem(kQuit, "Quit");
    } else if (index == 1) {
      const bool loaded = engine_.file() != nullptr;
      menu.addItem(kPlayPause, engine_.transport().isPlaying() ? "Pause" : "Play", loaded);
      menu.addItem(kStop, "Stop", loaded);
      menu.addItem(kRewind, "Back to the start", loaded);
      menu.addSeparator();
      menu.addItem(kLoop, "Loop", true, engine_.isLooping());
    } else {
      menu.addItem(kAbout, "About Sonore Player");
    }
    return menu;
  }

  void menuItemSelected(int id, int) override {
    if (id >= kRecentBase) openFile(recent_.getFile(id - kRecentBase));
    else if (id == kOpen) browse();
    else if (id == kClearRecent) {
      recent_.clear();
      sonore::RecentlyOpenedFilesList::clearRecentFilesNatively();
      saveRecent();
    } else if (id == kQuit) sonore::Application::quit();
    else if (id == kPlayPause) togglePlay();
    else if (id == kStop) stop();
    else if (id == kRewind) engine_.transport().setPosition(0.0);
    else if (id == kLoop) setLooping(!engine_.isLooping());
    else if (id == kAbout)
      gfx::NativeMessageBox::showMessageBoxAsync(gfx::MessageBoxIconType::InfoIcon, "Sonore Player",
                                                 "A small audio player, built on the Sonore SDK.", this);
  }

  // For --verify, which looks at what a user would see.
  gfx::Slider& positionSlider() { return position_; }
  gfx::WaveformView& waveform() { return waveform_; }
  const sonore::RecentlyOpenedFilesList& recentFiles() const { return recent_; }

private:
  void browse() {
    const std::string folder = settings_ ? settings_->getValue("lastFolder") : std::string();
    const std::vector<sonore::FileFilter> filters = {
        {"Audio files", "*.wav;*.aiff;*.aif;*.flac;*.mp3;*.ogg;*.m4a;*.aac;*.wma"}, {"All files", "*.*"}};
    void* window = nullptr;
    if (gfx::TopLevelWindow* top = gfx::TopLevelWindow::findTopLevelWindowFor(this))
      window = top->getWindowHandle();
    const std::string chosen = sonore::FileDialog::openFile("Open audio file", filters, folder, window);
    if (!chosen.empty()) openFile(chosen);
  }

  void togglePlay() {
    if (!engine_.file()) return browse();
    sonore::AudioTransportSource& t = engine_.transport();
    if (t.isPlaying()) t.stop();
    else {
      // From the top once the end was reached, as every player does.
      if (!engine_.isLooping() && t.getCurrentPosition() >= t.getLengthInSeconds() - 0.01)
        t.setPosition(0.0);
      t.start();
    }
  }

  void stop() {
    engine_.transport().stop();
    engine_.transport().setPosition(0.0);
  }

  void setLooping(bool on) {
    engine_.setLooping(on);
    loop_.setToggled(on, false);
    if (settings_) settings_->setValue("loop", on);
  }

  void setVolume(float fader) {
    const float gain = fader <= 0.0f ? 0.0f : std::pow(10.0f, (-60.0f + 66.0f * fader) / 20.0f);
    engine_.player().setGain(gain);
    if (settings_) settings_->setValue("volume", (double) fader);
  }

  void saveRecent() {
    if (settings_) settings_->setValue("recentFiles", recent_.toString());
  }

  void showFile() {
    const OpenFile* f = engine_.file();
    fileLabel_.setText(f ? sonore::fileName(f->path) : std::string("No file -- File > Open..."));
    waveform_.setThumbnail(f ? &f->thumbnail : nullptr);
    play_.setLabel(engine_.transport().isPlaying() ? "Pause" : "Play");
  }

  /** The transport started, stopped or reached the end: on the UI tick. */
  void changeListenerCallback(sonore::ChangeBroadcaster*) override {
    play_.setLabel(engine_.transport().isPlaying() ? "Pause" : "Play");
  }

  Engine& engine_;
  sonore::PropertiesFile* settings_;
  sonore::RecentlyOpenedFilesList recent_;
  gfx::Button open_{"Open..."}, play_{"Play"}, stop_{"Stop"}, loop_{"Loop"};
  gfx::Label fileLabel_, timeLabel_, volumeLabel_;
  gfx::WaveformView waveform_;
  gfx::Slider position_, volume_;
  bool seeking_ = false;
};

/** The window: a DocumentWindow with the view as its content and its menus. */
class MainWindow : public gfx::DocumentWindow {
public:
  MainWindow(PlayerView& view, bool onDesktop)
      : DocumentWindow("Sonore Player", scheme().getUIColour(Scheme::windowBackground), allButtons, onDesktop),
        view_(view) {
    setLookAndFeel(&look_);
    if (auto face = gfx::systemTypeface()) setFont(gfx::Font(face, 14.0f));
    setMenuBar(&view_);
    setContentNonOwned(&view_, false);
    setResizable(true, false);
    setResizeLimits(560.0f, 300.0f, 4000.0f, 3000.0f);
  }
  ~MainWindow() override {
    setMenuBar(nullptr);
    clearContentComponent();
  }

  void closeButtonPressed() override { sonore::Application::quit(); }
  void windowTick(double elapsedMs) override { view_.tick(elapsedMs); }

private:
  using Scheme = gfx::LookAndFeel_V4::ColourScheme;
  static Scheme scheme() { return gfx::LookAndFeel_V4::getDarkColourScheme(); }

  gfx::LookAndFeel_V4 look_{scheme()};
  PlayerView& view_;
};

// ── --verify ─────────────────────────────────────────────────────────────────

int g_failures = 0;
void check(bool ok, const char* what) {
  if (!ok) ++g_failures;
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

std::string tempFolder() {
#if defined(_WIN32)
  std::string t = sonore::pathio::environment("TEMP");
#else
  std::string t = sonore::pathio::environment("TMPDIR");
  if (t.empty()) t = "/tmp";
#endif
  return t;
}

/** Pull `blocks` blocks from the null device into `out`, interleaved. */
void pullBlocks(NullDevice& device, int blocks, std::vector<float>& out) {
  std::vector<float> block((size_t) kBlock * device.numChannels);
  out.clear();
  for (int b = 0; b < blocks; ++b) {
    device.pull(block.data(), (uint32_t) kBlock);
    out.insert(out.end(), block.begin(), block.end());
  }
}

/** The largest difference between what came out and `gain` times the file,
 *  output frame i being file frame (first + i) wrapped by `wrap` if > 0. */
float worstAgainstFile(const std::vector<float>& out, const sonore::WavData& file, size_t skip, size_t first,
                       float gain, size_t wrap = 0) {
  const size_t channels = file.numChannels;
  float worst = 0.0f;
  for (size_t i = skip; i < out.size() / 2; ++i) {
    size_t frame = first + i;
    if (wrap) frame %= wrap;
    if (frame >= file.numFrames()) break;
    for (size_t c = 0; c < 2; ++c) {
      const float want = gain * file.samples[frame * channels + (c < channels ? c : 0)];
      worst = (std::max)(worst, std::fabs(out[i * 2 + c] - want));
    }
  }
  return worst;
}

int verify(const std::string& path, const std::string& snapshot) {
  std::printf("player --verify %s\n", path.c_str());
  sonore::enableSystemAudioFormats();
  sonore::WavData file;
  check(sonore::readAudioFile(path.c_str(), &file) && file.numFrames() > (size_t) kBlock * 8,
        "the file decodes, and is long enough to play");
  if (g_failures) return 1;
  const double seconds = (double) file.numFrames() / file.sampleRate;
  char msg[240];

  // ── Played at the file's own rate: every sample checked ──
  {
    Engine engine;
    NullDevice device;
    device.rate = file.sampleRate;
    std::string error;
    check(engine.start(device, error), "the engine starts on the null device");
    check(engine.open(path, error), "and opens the file");
    sonore::AudioTransportSource& t = engine.transport();
    std::snprintf(msg, sizeof(msg), "the transport reports the file's length (%.3f s)", t.getLengthInSeconds());
    check(std::fabs(t.getLengthInSeconds() - seconds) < 1e-6, msg);

    std::vector<float> out;
    pullBlocks(device, 2, out);
    float loudest = 0.0f;
    for (float v : out) loudest = (std::max)(loudest, std::fabs(v));
    check(loudest == 0.0f, "stopped, it plays silence");

    t.start();
    pullBlocks(device, 6, out);
    // The first kFadeSamples are the fade-in that keeps a start from clicking.
    std::snprintf(msg, sizeof(msg), "playing, it is the file sample for sample after the fade-in (worst %.2g)",
                  worstAgainstFile(out, file, sonore::AudioTransportSource::kFadeSamples, 0, 1.0f));
    check(worstAgainstFile(out, file, sonore::AudioTransportSource::kFadeSamples, 0, 1.0f) < 1e-6f, msg);

    engine.player().setGain(0.5f);
    pullBlocks(device, 2, out);
    // The player ramps a gain change across one block; the second is settled.
    check(worstAgainstFile(out, file, (size_t) kBlock, (size_t) kBlock * 6, 0.5f) < 1e-6f,
          "a volume change reaches the samples, ramped over one block");
    engine.player().setGain(1.0f);
    pullBlocks(device, 1, out);

    // A seek made while stopped: the next start plays from there.
    t.stop();
    pullBlocks(device, 2, out); // the fade-out
    const size_t from = file.numFrames() / 3;
    t.setPosition((double) from / file.sampleRate);
    t.start();
    pullBlocks(device, 3, out);
    check(worstAgainstFile(out, file, sonore::AudioTransportSource::kFadeSamples, from, 1.0f) < 1e-6f,
          "a seek moves playback to the exact frame");

    // Looping: past the end, the start again, seamlessly.
    t.stop();
    pullBlocks(device, 2, out);
    engine.setLooping(true);
    const size_t nearEnd = file.numFrames() - (size_t) kBlock * 2;
    t.setPosition((double) nearEnd / file.sampleRate);
    t.start();
    pullBlocks(device, 6, out);
    check(worstAgainstFile(out, file, sonore::AudioTransportSource::kFadeSamples, nearEnd, 1.0f,
                           file.numFrames()) < 1e-6f,
          "looping wraps from the last frame to the first without a gap");

    // Not looping: the end stops the transport and says so on the UI tick.
    engine.setLooping(false);
    t.setPosition((double) (file.numFrames() - (size_t) kBlock) / file.sampleRate);
    bool told = false;
    sonore::LambdaChangeListener listener([&told]() { told = true; });
    t.dispatchPendingChanges(); // the start and stop above were heard; only the end is news
    t.addChangeListener(&listener);
    pullBlocks(device, 4, out);
    t.dispatchPendingChanges();
    t.removeChangeListener(&listener);
    check(t.hasStreamFinished() && !t.isPlaying() && told,
          "the end of the file stops the transport, and a listener hears it");

    // The waveform, and its cache.
    const sonore::AudioThumbnail& thumb = engine.file()->thumbnail;
    float peak = 0.0f;
    for (float v : file.samples) peak = (std::max)(peak, std::fabs(v));
    check(thumb.numBuckets() > 0 && std::fabs(thumb.peak() - peak) < 1e-6f,
          "the thumbnail's peak is the file's peak");
    sonore::AudioThumbnail cached;
    check(engine.thumbnails().loadThumb(cached, sonore::AudioThumbnailCache::hashFile(path.c_str())) &&
              cached == thumb,
          "and the cache hands the same one back for the same file");

    // ── The window's content, drawn without a window ──
    std::string settingsPath = sonore::joinPath(tempFolder(), "sonore-player-verify.settings");
    sonore::pathio::remove(settingsPath.c_str());
    sonore::PropertiesFile::Options options;
    options.applicationName = "SonorePlayerVerify";
    options.millisecondsBeforeSaving = 0; // save on every change, for the reload below
    sonore::PropertiesFile settings(settingsPath, options);
    {
      PlayerView view(engine, &settings);
      check(view.openFile(path, false), "the view opens the file through the engine");
      view.openFile(path, false); // twice: still one entry in the recent list
      check(view.recentFiles().getNumFiles() == 1, "opening a file again moves it to the top, not in twice");
      MainWindow window(view, /*onDesktop=*/false);
      window.setBounds({0.0f, 0.0f, 820.0f, 420.0f});
      t.setPosition(seconds * 0.5);
      view.tick(33.0);
      check(std::fabs(view.positionSlider().value() - 0.5f) < 0.01f &&
                std::fabs(view.waveform().playhead() - 0.5f) < 0.01f,
            "the position slider and the playhead follow the transport");
      gfx::Bitmap picture(820, 420);
      gfx::Graphics g(picture);
      window.paintTree(g);
      // The waveform is drawn in the accent wherever there is sound.
      const gfx::Rect wave = view.waveform().bounds();
      const gfx::Colour accent = window.lookAndFeel().accent();
      int lit = 0;
      for (int y = (int) wave.y + 24; y < (int) wave.bottom(); ++y)
        for (int x = (int) wave.x; x < (int) wave.right(); ++x) {
          const gfx::PremulColour p = picture.pixelAt(x, y);
          if (std::abs((int) p.r - (int) accent.r) < 40 && std::abs((int) p.g - (int) accent.g) < 40 &&
              std::abs((int) p.b - (int) accent.b) < 40)
            ++lit;
        }
      std::snprintf(msg, sizeof(msg), "the window draws the waveform (%d pixels of it)", lit);
      check(lit > 2000, msg);
      if (!snapshot.empty()) {
        // A BMP, because it is twenty lines and needs no encoder.
        FILE* f = sonore::pathio::open(snapshot.c_str(), "wb");
        if (f) {
          const int w = picture.width(), h = picture.height();
          unsigned char header[54] = {'B', 'M'};
          auto put = [&header](int at, uint32_t v) {
            for (int i = 0; i < 4; ++i) header[at + i] = (unsigned char) (v >> (8 * i));
          };
          put(2, 54u + (uint32_t) (w * h * 4));
          put(10, 54);
          put(14, 40);
          put(18, (uint32_t) w);
          put(22, (uint32_t) h);
          header[26] = 1;
          header[28] = 32;
          std::fwrite(header, 1, 54, f);
          for (int y = h - 1; y >= 0; --y)
            for (int x = 0; x < w; ++x) {
              const gfx::PremulColour p = picture.pixelAt(x, y);
              const unsigned char px[4] = {p.b, p.g, p.r, 255};
              std::fwrite(px, 1, 4, f);
            }
          std::fclose(f);
          std::printf("  ---- the window, as drawn: %s ----\n", snapshot.c_str());
        }
      }
    }
    // Remembered: a new run reads back what this one left.
    settings.setValue("volume", 0.42);
    settings.saveIfNeeded();
    sonore::PropertiesFile reread(settingsPath, options);
    check(std::fabs(reread.getDoubleValue("volume", 0.0) - 0.42) < 1e-9 &&
              reread.getValue("lastFolder") == sonore::parentPath(path) &&
              reread.getValue("recentFiles") == path,
          "the volume, the last folder and the recent files survive a restart");
    sonore::pathio::remove(settingsPath.c_str());
  }

  // ── Played at another rate: the transport resamples ──
  {
    Engine engine;
    NullDevice device;
    device.rate = file.sampleRate == 48000 ? 44100.0 : 48000.0;
    std::string error;
    engine.start(device, error);
    check(engine.open(path, error), "opened again, on a device at another rate");
    std::snprintf(msg, sizeof(msg), "the length in seconds is the same at %.0f Hz (%.4f s)", device.rate,
                  engine.transport().getLengthInSeconds());
    check(std::fabs(engine.transport().getLengthInSeconds() - seconds) < 0.002, msg);
    engine.transport().start();
    std::vector<float> out;
    const int blocks = (int) (seconds * device.rate * 0.8) / kBlock;
    pullBlocks(device, blocks, out);
    double sumOut = 0.0, sumIn = 0.0;
    bool finite = true;
    const size_t frames = out.size() / 2;
    for (size_t i = 0; i < out.size(); ++i) {
      finite = finite && std::isfinite(out[i]);
      sumOut += (double) out[i] * out[i];
    }
    const size_t fileFrames = (size_t) ((double) frames * file.sampleRate / device.rate);
    for (size_t i = 0; i < fileFrames * file.numChannels && i < file.samples.size(); ++i)
      sumIn += (double) file.samples[i] * file.samples[i];
    const double rmsOut = std::sqrt(sumOut / (double) out.size());
    const double rmsIn = std::sqrt(sumIn / (double) (fileFrames * file.numChannels));
    std::snprintf(msg, sizeof(msg), "and it sounds the same: RMS %.4f against the file's %.4f", rmsOut, rmsIn);
    check(finite && std::fabs(rmsOut / rmsIn - 1.0) < 0.05, msg);
  }

  std::printf(g_failures == 0 ? "PLAYER VERIFY PASSED\n" : "PLAYER VERIFY FAILED (%d)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}

// ── The application ──────────────────────────────────────────────────────────

/** True when the command line asks for --verify: decided before anything
 *  else, because a verifying run must neither open a window nor hand its
 *  command line to a player that is already open. */
inline bool verifying() {
  for (const std::string& a : sonore::Application::getCommandLineParameterArray())
    if (a == "--verify") return true;
  return false;
}

class PlayerApp : public sonore::Application {
public:
  const std::string getApplicationName() override { return "Sonore Player"; }
  const std::string getApplicationVersion() override { return "1.0.0"; }
  bool moreThanOneInstanceAllowed() override { return verifying(); }

  void initialise(const std::string&) override {
    const std::vector<std::string> args = getCommandLineParameterArray();
    if (verifying()) {
      std::string file, snapshot;
      for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--verify") file = args[i + 1];
        if (args[i] == "--snapshot") snapshot = args[i + 1];
      }
      setApplicationReturnValue(file.empty() ? 2 : verify(file, snapshot));
      quit();
      return;
    }

    // Opt in to the OS's decoders: an application decides that, a plugin
    // that must open identically everywhere would not.
    sonore::enableSystemAudioFormats();
    sonore::PropertiesFile::Options options;
    options.applicationName = "SonorePlayer";
    options.folderName = "Sonorie";
    properties_.setStorageParameters(options);

    view_.reset(new PlayerView(engine_, properties_.getUserSettings()));
    std::string error;
    if (!engine_.start(device_, error))
      gfx::NativeMessageBox::showMessageBoxAsync(gfx::MessageBoxIconType::WarningIcon, "No audio output",
                                                 "The audio device would not start: " + error +
                                                     "\nFiles still open; nothing will be heard.");
    window_.reset(new MainWindow(*view_, /*onDesktop=*/true));
    window_->centreWithSize(820.0f, 420.0f);
    window_->setVisible(true);
    for (const std::string& a : args)
      if (!a.empty() && a[0] != '-') view_->openFile(a);
  }

  void shutdown() override {
    window_.reset();
    view_.reset();
    device_.stop();
    properties_.saveIfNeeded();
  }

  /** A second launch -- a file opened from the desktop -- lands here, as the
   *  command line it was started with: one path, quoted if it has a space. */
  void anotherInstanceStarted(const std::string& commandLine) override {
    if (!view_) return;
    std::string path = commandLine;
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
    if (!path.empty() && path[0] != '-') view_->openFile(path);
    if (window_) window_->toFront(true);
  }

private:
  // Declared in the order they must die in reverse: the device stops before
  // the engine it pulls from goes, the window before the view it shows.
  sonore::ApplicationProperties properties_;
  Engine engine_;
  OutputDevice device_;
  std::unique_ptr<PlayerView> view_;
  std::unique_ptr<MainWindow> window_;
};

} // namespace player

SONORE_START_APPLICATION(player::PlayerApp)
