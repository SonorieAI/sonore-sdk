# Sonore SDK

A header-only C++17 framework for audio plugins: one source file builds a CLAP,
a VST3, an Audio Unit, an LV2 bundle, a standalone application and a browser
build of the same DSP. This is its documentation: task-oriented guides, and an
API reference generated from the headers' own comments.

## Guides

1. [Getting started](guides/getting-started.md) -- a generated project, built,
   checked and loaded into a DAW.
2. [Parameters and state](guides/parameters-and-state.md) -- `ParamInfo`, the
   `StateBag`, the APVTS shape, and keeping old sessions loading.
3. [The native editor](guides/native-editor.md) -- `createEditor`, widgets and
   attachments, `LookAndFeel`, the preset bar.
4. [The web editor](guides/web-editor.md) -- a page as the plugin's face, and
   the `window.sonore` bridge.
5. [Presets](guides/presets.md) -- factory presets in the binary, the user's
   own in a folder.
6. [MIDI and MPE](guides/midi-and-mpe.md) -- instruments, voices, per-note
   expression, MIDI out.
7. [Hosting plugins](guides/hosting.md) -- scanning, the scan cache, loading
   and running other people's plugins.
8. [Testing your plugin](guides/testing.md) -- the self-checks, unit tests, the
   validators, a real DAW.
9. [Platforms and formats](guides/platforms-and-formats.md) -- what runs where,
   and what is not available.
10. [Versions and compatibility](guides/versions.md) -- what a version number
    promises.

The SDK's [README](../README.md) is the long version: what is in it, building,
how everything is verified, licensing.

## The API reference, by area

The API reference documents everything public: `namespace sonore` and its named
sub-namespaces, outside any `detail` namespace. Every header is filed under one
area, and its page opens with the header's own top comment -- what the file is
and why it is built the way it is. In the generated pages the areas are listed
under *Modules*.

| Area | What is in it | Start with |
| --- | --- | --- |
| Plugin model | the DSP's types, parameters, state, presets, undo | `plugin.h`, `audio.h`, `state_bag.h` |
| DSP | filters, dynamics, reverbs, synthesis, analysis | `dsp.h` |
| Formats and wrappers | CLAP, VST3, AU, LV2, the standalone, the browser ABI | `formats.h` |
| Hosting plugins | scanning, the scan cache, hosted plugins, a graph | `host.h`, `plugin_cache.h` |
| Native GUI | components, widgets, LookAndFeel, text, images, windows | `gfx/widgets.h`, `gfx/plugin_editor.h` |
| Web editor bridge | the `window.sonore` bridge and the OS webviews | `gui.h` |
| Files and codecs | paths, streams, audio files, images, zip | `files.h`, `audiofile.h` |
| MIDI and OSC | MIDI files, sequences, UMP, MIDI-CI, devices, OSC | `midi_file.h`, `ump.h` |
| System, devices and network | audio devices, threads, processes, sockets, HTTP | `net.h`, `process.h` |
| Applications | the message thread, settings, documents, logs, unit tests | `application.h` |
| Data, text and crypto | JSON, XML, expressions, JavaScript, hashes, RSA | `json.h`, `xml.h` |

## Building the API reference

```
npm run docs:fetch    # Windows, once: Doxygen into sdk/tools/doxygen
npm run docs          # the HTML into sdk/build/docs/html
```

On Linux and macOS install Doxygen with the package manager instead. The run
fails on a Doxygen warning about the headers or these pages, and reports -- but
does not fail on -- public classes and members that have no comment yet
(`sdk/build/docs/undocumented.txt`). CI builds it on every push and keeps the
HTML as an artifact.

## The code in the guides is compiled

Every C++ block in a guide that starts with `// test-run` or `// test-compile`
is extracted by `sdk/tests/doc_snippets.cmake` and built with the SDK's tests.
A block that is a plugin is built as a CLAP and as a standalone application, and
`// test-run` runs the standalone's `--verify` on it; a block with a main
function is built as a program, and `// test-run` runs it. So a guide whose code stops
compiling fails the build, not the reader.
