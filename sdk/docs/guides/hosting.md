# Hosting plugins

The other direction: an application that loads other people's plugins.
`sonore/host.h` scans folders, describes what it finds, loads a plugin and
drives it with audio, MIDI, parameters and state.

| Format | |
| --- | --- |
| CLAP | always compiled in |
| VST3, LV2 | compiled in where their C headers are on the include path: `sonore_enable_hosting(<target>)` in CMake arranges it |
| LADSPA | always compiled in: audio and control ports, which is all the format has |
| Audio Units (v2 and v3) | macOS, through `host_au.h`; tested in CI on macOS (`sdk/tests/mac_host_selftest.cpp`, and the hosting tests with the example AUs) |
| VST2 | never |

Without `sonore_enable_hosting` a host still builds, hosts CLAP, and finds no
VST3 or LV2 plugin at all, so call it for every program that hosts.

## Scan, load, run

Scanning means running each file's own code, which is slow and can crash the
host. `sonore::host::scanDirectoryCached` keeps a `sonore::host::PluginCache`:
unchanged files are not opened again, and the path being scanned is written to
the cache file and flushed to disk before the file is opened, so a plugin that
kills the process is blacklisted on the next run instead of killing it again.

```cpp
// test-compile
#include <sonore/plugin_cache.h>

#include <cstdio>
#include <memory>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: scan <plugin folder>\n");
    return 0;
  }
  using namespace sonore::host;

  // [main thread] Scan, remembering what was found and what crashed.
  PluginCache cache;
  cache.load("plugins.cache"); // false the first time: there is nothing yet
  const std::vector<PluginDescription> found = scanDirectoryCached(argv[1], cache, "plugins.cache");
  for (const PluginDescription& d : found)
    std::printf("%-5s %s by %s%s\n", d.format.c_str(), d.name.c_str(), d.vendor.c_str(),
                d.isInstrument ? " (instrument)" : "");
  if (found.empty()) return 0;

  // Load the first, and give it its buffers. Null when the file moved, the id
  // is gone or the plugin refused to start.
  std::unique_ptr<HostedPlugin> plugin = loadPlugin(found[0]);
  if (!plugin || !plugin->prepare(48000.0, 512, 2)) return 1;
  for (int i = 0; i < plugin->numParameters(); ++i)
    std::printf("  %s = %s\n", plugin->parameter(i).name.c_str(),
                plugin->parameterText(i, plugin->parameterValue(i)).c_str());

  // [audio thread, in a real host] One block, in place. process() allocates
  // nothing: prepare() sized every buffer.
  std::vector<float> left(512, 0.0f), right(512, 0.0f);
  float* channels[2] = {left.data(), right.data()};
  sonore::AudioBlock<float> block(channels, 2, 512);
  plugin->process(block);

  // A session: the plugin's own state, to hand back to loadState() later.
  std::vector<uint8_t> state;
  plugin->saveState(state);
  std::printf("latency %u samples, state %zu bytes\n", (unsigned) plugin->latencySamples(), state.size());
  return 0;
}
```

`sonore::host::HostedPlugin` is the same interface whatever the format:
parameters (`numParameters`, `parameterValue`, `setParameterValue`,
`parameterText`), state (`saveState`, `loadState`), presets, latency and tail,
MIDI in and out, bypass, and the plugin's editor (`hasEditor`, `openEditor`
into a native parent window). The threading follows the plugin side's rules:
scanning, loading, `prepare()`, state and parameter reads on the main thread;
`process()` on the audio thread.

## More than one plugin

`sonore::host::PluginGraph` (`graph.h`) runs hosted plugins as a graph:
`addNode()` takes a loaded plugin, `connect()` routes audio and
`connectMidi()` routes the notes one plugin emits into another, and
`process()` runs the graph in dependency order. `gfx/plugin_list.h` is the
table a host shows of the plugins it knows about.

## Building a host

```
add_executable(MyHost main.cpp)
target_link_libraries(MyHost PRIVATE sonore_sdk)   # SonoreSDK::sonore_sdk from find_package
sonore_enable_hosting(MyHost)
```

The SDK's own host tests are built on this header (`sdk/tests/host_test.cpp`,
`sdk/tests/render_tool.cpp`): they load the plugins the SDK builds, as a DAW
would, on every run.
