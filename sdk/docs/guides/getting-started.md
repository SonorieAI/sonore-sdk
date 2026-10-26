# Getting started

From nothing to your own plugin in a DAW (a digital audio workstation, such as
REAPER, Ableton Live or Logic Pro). You write one C++ file, and the SDK builds
it as a CLAP and a VST3 plugin, an Audio Unit on macOS, an LV2 plugin and a
standalone app, and as an AAX for Pro Tools when you give the build Avid's AAX
SDK ([Platforms and formats](platforms-and-formats.md) says how). Step 5 sets
up a coding agent to write the next one with you.

## What you need

- **CMake** 3.21 or newer.
- **A C++17 compiler**: Visual Studio 2022 or newer on Windows, Xcode or its
  command line tools on macOS, GCC on Linux.
- **The SDK.** Clone it, and run every command below from its root folder:

```
git clone https://github.com/SonorieAI/sonore-sdk
cd sonore-sdk
```

## 1. Generate a project

```
cmake -DNAME="My Filter" -DVENDOR="My Company" -P sdk/cmake/sonore_new_plugin.cmake
```

This creates a folder, `MyFilter/`, holding the whole plugin in
`src/plugin.cpp`, plus a `CMakeLists.txt`, a README and a `.gitignore`. It never
writes into a folder that already has anything in it.

| Option | What it does |
| --- | --- |
| `-DTYPE=instrument` | starts an eight-voice synth instead of a filter |
| `-DID=com.mycompany.myplugin` | sets the plugin's id (see below) |
| `-DAU_SUBTYPE=`, `-DAU_MANUFACTURER=` | set the Audio Unit's four-character codes, which the AAX uses too |

**Choose the id now.** The generator prints the plugin's id, here
`com.mycompany.myfilter`. It is how every host and every saved session
recognises the plugin, so changing it later orphans every session that used it.
If the derived id is not the one to keep, generate again with `-DID=`.

The Audio Unit's codes are derived too: the manufacturer code from the vendor,
shared by all your plugins, and the subtype from the id, so each plugin has its
own. Set them yourself to keep the codes of an Audio Unit you have already
shipped.

## 2. Build it, and let it check itself

```
cmake -S MyFilter -B MyFilter/build
cmake --build MyFilter/build --config Release
ctest --test-dir MyFilter/build -C Release
```

The first command configures the project, the second builds every format, and
the third runs the two checks the project comes with:

- `MyFilterApp --verify` renders the plugin offline, with no audio device and
  no window, and fails on silence, NaN (a sample that is not a number) or a
  malformed descriptor.
- The real-time audit, in a project generated from a source checkout like this
  one, fails if anything allocates memory on the audio thread, at any sample
  rate and block size it tries.

## 3. The plugin, all of it

A plugin is one file in four parts:

1. `SonoreDsp`, a struct that processes the audio. `prepare()` runs before
   audio starts, and `process()` runs for every block of samples.
2. The parameter table, `kParamTable`: one row per control.
3. The descriptor, `kDesc`: the plugin's id, name, vendor and version.
4. `#include <sonore/formats.h>` at the end, which makes the file every plugin
   format at once.

This is the effect the generator writes, with its values filled in:

```cpp
// test-run
#define SONORE_NUM_PARAMS 2
#include <sonore/dsp.h>

struct SonoreDsp {
  sonore::SVF filter[2]; // one per channel: a filter is its state
  sonore::Smooth cutoffSm, gainSm;
  bool fresh = true;

  // Called before audio starts and whenever the rate or block size changes.
  // It may also run on the audio thread, so it allocates nothing.
  void prepare(const sonore::ProcessSpec& spec) {
    for (auto& f : filter) {
      f.setSampleRate((float) spec.sampleRate);
      f.reset();
    }
    cutoffSm.setup((float) spec.sampleRate, 20.0f);
    gainSm.setup((float) spec.sampleRate, 20.0f);
    fresh = true;
  }

  // The audio thread: no allocation, no locks, no files.
  void process(sonore::AudioBlock<float>& io, const float* p) {
    const float gain = sonore::dbToGain(p[1]);
    if (fresh) { // the first block starts AT the settings, not gliding in
      cutoffSm.snap(p[0]);
      gainSm.snap(gain);
      fresh = false;
    }
    const size_t channels = io.getNumChannels() < 2 ? io.getNumChannels() : 2;
    for (size_t i = 0; i < io.getNumSamples(); ++i) {
      // Every control that reaches a coefficient or a gain is smoothed, or
      // automation zippers.
      const float hz = cutoffSm.next(p[0]), g = gainSm.next(gain);
      for (size_t c = 0; c < channels; ++c) {
        float& x = io.getChannelPointer(c)[i];
        filter[c].set(hz, 0.707f);
        filter[c].process(x);
        x = filter[c].lp * g;
      }
    }
  }
};

#include <sonore/plugin.h>

// POSITIONAL: a parameter's index is its identity in every saved session.
// Once this ships, only ever append rows.
static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    // id, name, unit, min, max, default -- then the knob's skew, which puts
    // 1 kHz at the middle of its travel instead of five per cent along.
    {"cutoff", "Cutoff", "Hz", 20.0f, 20000.0f, 1000.0f, 0, nullptr, nullptr, 0, true, false, false,
     sonore::skewForCentre(20.0f, 20000.0f, 1000.0f)},
    {"gain", "Gain", "dB", -24.0f, 24.0f, 0.0f},
};

static const sonore::PluginDescriptor kDesc = {
    "com.mycompany.myfilter", "My Filter", "My Company", "0.1.0",
    "An effect.",
    "",
    false, // an effect
    kParamTable, SONORE_NUM_PARAMS,
    nullptr, 0, // no factory presets
    "filter",   // the category hosts file it under
};

#include <sonore/formats.h> // CLAP, and every format this target is built as
```

A few rules hold for every plugin:

- The names are fixed: the DSP is `SonoreDsp`, the descriptor `kDesc`, and
  `SONORE_NUM_PARAMS` is the number of rows in the parameter table.
- `p[i]` in `process()` is parameter `i`'s plain value, in its own units (hertz,
  decibels), held still for the whole block.
- A parameter's position in the table is its identity in saved sessions. Once
  the plugin ships, add new parameters at the end only.
- `process()` runs on the audio thread: no allocation, no locks, no files.
- The same file becomes every format. `formats.h` always includes the CLAP
  wrapper, and the VST3, AU, LV2, AAX or standalone wrapper when the CMake target
  asks for it (`sonore_add_plugin` and `sonore_add_standalone` in the generated
  `CMakeLists.txt`).

## 4. Into a DAW

The build puts the outputs in `MyFilter/build/Release` (in `MyFilter/build`
with a single-configuration generator such as Ninja). Copy each one to where
hosts look for it:

| Output | Copy it to |
| --- | --- |
| `MyFilter.clap` | Windows `C:\Program Files\Common Files\CLAP` (or, per user, `C:\Users\<you>\AppData\Local\Programs\Common\CLAP`); macOS `~/Library/Audio/Plug-Ins/CLAP`; Linux `~/.clap` |
| `vst3/MyFilterVST3.vst3`, a folder (on macOS, `MyFilterVST3.vst3` itself) | Windows `C:\Program Files\Common Files\VST3` (or `C:\Users\<you>\AppData\Local\Programs\Common\VST3`); macOS `~/Library/Audio/Plug-Ins/VST3`; Linux `~/.vst3` |
| `MyFilterAU.component` (macOS) | `~/Library/Audio/Plug-Ins/Components` |
| `MyFilterLV2.lv2` (a folder) | Windows `C:\Users\<you>\AppData\Roaming\LV2`; macOS `~/Library/Audio/Plug-Ins/LV2`; Linux `~/.lv2` |
| `aax/MyFilterAAX.aaxplugin`, a folder (on macOS, `MyFilterAAX.aaxplugin`), only when the build was given Avid's AAX SDK | Windows `C:\Program Files\Common Files\Avid\Audio\Plug-Ins`; macOS `/Library/Application Support/Avid/Audio/Plug-Ins`. A developer build of Pro Tools loads it as built, a retail one only once it is signed |

Then rescan plugins in the DAW. A plugin with no editor of its own opens the
native editor: one control per parameter, drawn by the SDK. To run the plugin
without a DAW, start `MyFilterApp`; `MyFilterApp --help` lists what it can do.

## 5. Write the next one with a coding agent

Coding agents write good C++, but against a library they have not seen they
invent: a class that sounds right and does not exist, or a real class given
another library's method. The SDK ships two things that stop that, for Claude
Code, Codex, Cursor, Antigravity and any other agent that reads skills or
speaks MCP:

- **The skill** goes into the agent's own context: the shape of a plugin, the
  rules that cannot be guessed from the API, and how to work here. Look a name
  up before writing it, start from a generated project, and do not call a
  plugin done until its checks pass.
- **The MCP server** gives the agent tools to look the API up instead of
  guessing: `sonore_search` (does this name exist, and where),
  `sonore_declaration` (its real declaration and comment), `sonore_header`,
  `sonore_headers`, `sonore_example` (a complete working plugin) and
  `sonore_rules`. It runs from your SDK checkout, only ever reads it, and has
  no dependencies.

Both need Node.js: the server runs on it, and the skill installs with `npx`.
The commands below expect the SDK in `$HOME/sonore-sdk`, which reads the same in
bash, zsh and PowerShell. To use the clone from the top of this guide instead,
change that path.

```
git clone https://github.com/SonorieAI/sonore-sdk "$HOME/sonore-sdk"
```

### Claude Code

```
npx skills add SonorieAI/sonore-sdk --skill sonore-sdk -a claude-code -g
claude mcp add --scope user sonore-sdk -- node "$HOME/sonore-sdk/scripts/mcp-server.mjs"
```

Both are installed for your user, so every project gets them. `claude mcp list`
shows whether the server is connected.

### Codex

The Codex CLI, its IDE extension and the ChatGPT desktop app share this setup:

```
npx skills add SonorieAI/sonore-sdk --skill sonore-sdk -a codex -g
codex mcp add sonore-sdk -- node "$HOME/sonore-sdk/scripts/mcp-server.mjs"
```

`codex mcp list` shows the server.

### Cursor, Antigravity and other agents

Install the skill with `-a cursor` or `-a antigravity`:

```
npx skills add SonorieAI/sonore-sdk --skill sonore-sdk -a cursor -g
```

Then add the server to `~/.cursor/mcp.json` (Cursor) or
`~/.gemini/config/mcp_config.json` (Antigravity), merged with anything already
there, and restart the editor. Write the full path to your clone: JSON does not
expand `$HOME`. Any other MCP client takes the same entry:

```json
{
  "mcpServers": {
    "sonore-sdk": {
      "command": "node",
      "args": ["/path/to/sonore-sdk/scripts/mcp-server.mjs"]
    }
  }
}
```

### Try it

Open `MyFilter/` in the agent and ask for something real:

> Add a Mix parameter that blends the filtered signal with the dry input, then
> build the plugin and run ctest.

With the skill and the server in place, the agent should look names up before
it writes them (you will see it call `sonore_search` or `sonore_declaration`),
and build and run the checks before it calls the plugin done. If it does
neither, check that `sonore-sdk` is in its list of MCP servers.

## Or describe it to Sonorie

Rather start from a description than from code? [Sonorie](https://sonorie.com)
turns one into a plugin: describe an effect or a synth and hear it a minute
later. Every plugin Sonorie generates is built on Sonore.

## Where next

- [Parameters and state](parameters-and-state.md): ranges, steps, groups, and
  what a session saves.
- [The native editor](native-editor.md) or [the web editor](web-editor.md): a
  face of its own.
- [Testing your plugin](testing.md): the validators a host vendor runs first.
- [Platforms and formats](platforms-and-formats.md): what runs where.
- [Writing plugins with a coding agent](../../README.md#writing-plugins-with-a-coding-agent),
  in the README: `llms.txt`, and what each MCP tool answers.
