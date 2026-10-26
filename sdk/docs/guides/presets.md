# Presets

Two kinds: factory presets compiled into the plugin, and the user's own, saved
as files in a folder every DAW the plugin is loaded into can see.

## Factory presets

A `sonore::Preset` is a name and one value per parameter, in the table's order.
The descriptor points at an array of them. They ship inside the binary, so they
cannot go missing or drift from the parameters they were written for; a preset
whose value count does not match the table is refused rather than half applied.

```cpp
// test-run
#define SONORE_NUM_PARAMS 2
#include <sonore/dsp.h>

struct SonoreDsp {
  sonore::Smooth driveSm, outSm;

  void prepare(const sonore::ProcessSpec& spec) {
    driveSm.setup((float) spec.sampleRate, 12.0f);
    outSm.setup((float) spec.sampleRate, 12.0f);
    driveSm.snap(1.0f);
    outSm.snap(1.0f);
  }

  void process(sonore::AudioBlock<float>& io, const float* p) {
    const float out = sonore::dbToGain(p[1]);
    for (size_t i = 0; i < io.getNumSamples(); ++i) {
      const float drive = driveSm.next(p[0]), g = outSm.next(out);
      for (size_t c = 0; c < io.getNumChannels(); ++c) {
        float& x = io.getChannelPointer(c)[i];
        x = std::tanh(x * drive) * g;
      }
    }
  }
};

#include <sonore/plugin.h>
#include <sonore/presets.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"drive", "Drive", "x", 1.0f, 20.0f, 2.0f},
    {"output", "Output", "dB", -24.0f, 12.0f, 0.0f},
};

// One value per parameter, in the table's order: drive, output.
static const float kWarm[SONORE_NUM_PARAMS] = {2.5f, 0.0f};
static const float kCrunch[SONORE_NUM_PARAMS] = {8.0f, -3.0f};
static const float kDestroy[SONORE_NUM_PARAMS] = {18.0f, -6.0f};

static const sonore::Preset kPresets[] = {
    {"Warm", kWarm, SONORE_NUM_PARAMS},
    {"Crunch", kCrunch, SONORE_NUM_PARAMS},
    {"Destroy", kDestroy, SONORE_NUM_PARAMS},
};

static const sonore::PluginDescriptor kDesc = {
    "com.mycompany.presetdrive", "Preset Drive", "My Company", "1.0.0",
    "A drive with three starting points.", "",
    false, kParamTable, SONORE_NUM_PARAMS,
    kPresets, (int) (sizeof(kPresets) / sizeof(kPresets[0])),
};

#include <sonore/formats.h>
```

Each format lists them where its hosts look:

- **CLAP**, through preset discovery: written out as preset files in the OS's
  cache folder, or declared inside the plugin when that folder cannot be
  written.
- **VST3**, as a program list.
- **Audio Units**, as factory presets.
- **LV2**, as `pset:Preset` entries in the bundle's `presets.ttl`.

A web page sees them as `sonore.presets` and loads one with
`sonore.loadPreset(i)`.

## The user's presets

A plugin built with the SDK gets the user's own presets with nothing to write:
the native editor's preset bar saves and browses them, and CLAP hosts find the
folder through preset discovery. (A web page sees the factory list only.)

A user preset is a file holding the plugin's whole state: the same bytes a host
saves in a session. So a sampler's preset names its sample, and the audio file
stays where it is. The state sits inside a small versioned container with a
name, author, comment, category, tags and a checksum.

Each plugin has one folder:

| | |
| --- | --- |
| Windows | `Documents\<Vendor>\<Plugin>\Presets` |
| macOS | `~/Library/Audio/Presets/<Vendor>/<Plugin>` |
| Linux | `$XDG_DATA_HOME/<Vendor>/<Plugin>/presets` (`~/.local/share` when unset) |

The environment variable `SONORE_PRESET_ROOT` moves every plugin's folder under
one root (`<root>/<Vendor>/<Plugin>/Presets`), which is what tests and validators
use.

Each plugin instance has a `sonore::PresetManager` that puts factory and user
presets in one list. It knows previous and next, the preset the user is on
(saved with the session, so a project reopens on it), and whether the sound has
changed since it was loaded. The generic native editor shows it as a
`sonore::gfx::PresetBar` along its top; an editor of your own adds the bar
itself ([the native editor](native-editor.md) shows how).

A preset loads through the same path a host's session restore takes. So a
parameter added in a later version reads as its default from an old preset,
exactly as it does from an old session.

Deleting a preset moves the file to the Recycle Bin or Trash where the platform
has one, and says so when it cannot. Saving writes a temporary file and renames
it, so a crash mid-save leaves the old preset intact.

## Reading the folder yourself

`sonore::UserPresetStore` is the folder on its own, for a tool or a browser of
your own: list, save, rename, delete, import and export.

```cpp
// test-run
#include <sonore/user_presets.h>

#include <cstdio>

int main() {
  sonore::UserPresetStore store;
  store.setFolder(sonore::defaultUserPresetFolder("My Company", "Preset Drive"));
  store.setPluginId("com.mycompany.presetdrive"); // another plugin's files are refused
  store.rescan();
  std::printf("%s\n", store.folder().c_str());
  for (const sonore::UserPresetStore::Entry& e : store.entries())
    std::printf("  %s (%s)\n", e.metadata.name.c_str(), e.metadata.author.c_str());
  for (const sonore::UserPresetStore::Refusal& r : store.refused())
    std::printf("  refused %s: %s\n", r.path.c_str(), r.reason.c_str());
  return 0;
}
```
