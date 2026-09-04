// SPDX-License-Identifier: Apache-2.0
// Compile the macOS device backends. Nothing here runs.
//
// audio_coreaudio.h, audio_coreaudio_input.h and the CoreMIDI branch of
// midi_input.h are ~600 lines that no compiler had ever read -- including the
// HAL device enumeration written blind, and the CoreMIDI SysEx path.
//
// See au_shim/README.md for what a green run proves: internal consistency,
// not ABI correctness.
// audio_ring.h first, exactly as audio_input.h supplies it to whichever
// backend it selects. Neither the WASAPI nor the CoreAudio input header
// includes it themselves -- that is the project's convention, and this TU
// follows it rather than changing production code to suit a test.
#include <sonore/audio_ring.h>

#include <sonore/audio_coreaudio.h>
#include <sonore/audio_coreaudio_input.h>
#include <sonore/midi_input.h>

int main() {
  auto outs = sonore::standalone::CoreAudioOutput::listDevices();
  auto ins = sonore::midiin::Device::listDevices();
  return (int) (outs.size() + ins.size());
}
