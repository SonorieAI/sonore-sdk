// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: <windows.h> first, without NOMINMAX, then the SDK.
//
// windows.h defines min and max as function-like macros unless NOMINMAX is
// defined, and a user's project includes it wherever it likes. Every
// `std::min(` and `Vec4f::min(` in a header after it is then the macro: this
// file did not compile, with 17 errors in simd.h, and nor did any project
// that included windows.h ahead of dsp.h or the editor. Building is the test.
//
// The same windows.h, without WIN32_LEAN_AND_MEAN, also brings in the old
// winsock.h, after which winsock2.h cannot be included: osc.h stopped this
// file with a hundred redefinitions. It now uses winsock.h when that came
// first, so a datagram is sent through it below, not only compiled.
#include <windows.h>

#include <sonore/dsp.h>
#include <sonore/fft.h>
#include <sonore/impulse.h>
#include <sonore/sampler.h>
#include <sonore/gui.h>
#include <sonore/gfx/native_editor.h>
#include <sonore/audio_wasapi.h>
#include <sonore/audio_wasapi_input.h>
#include <sonore/delay.h>
#include <sonore/phase.h>
#include <sonore/matrix.h>
#include <sonore/polynomial.h>
#include <sonore/ump128.h>
#include <sonore/midi_collector.h>
#include <sonore/gfx/mpe_keyboard.h>
#include <sonore/bigint.h>
#include <sonore/crypto.h>
#include <sonore/hash.h>
#include <sonore/system_audio_format.h>
#include <sonore/lame_encoder.h>
#include <sonore/thumbnail_cache.h>
#include <sonore/system_volume.h>
#include <sonore/audio_cd.h>
#include <sonore/xml.h>
#include <sonore/expression.h>
#include <sonore/localisation.h>
#include <sonore/text_diff.h>
#include <sonore/statistics.h>
#include <sonore/leak_detector.h>
#include <sonore/unit_test.h>
#include <sonore/plugin_host_type.h>
#include <sonore/value_tree_state.h>
#include <sonore/gfx/value_tree_state_attachments.h>
#include <sonore/gfx/plugin_list.h>
#include <sonore/gfx/popup_overlay.h>
#include <sonore/gfx/menu_bar.h>
#include <sonore/gfx/colour_selector.h>
#include <sonore/gfx/toolbar.h>
#include <sonore/gfx/side_panel.h>
#include <sonore/gfx/multi_document.h>
#include <sonore/gfx/key_mapping_editor.h>
#include <sonore/gfx/file_chooser.h>
#include <sonore/gfx/jpeg.h>
#include <sonore/gfx/gif.h>
#include <sonore/gfx/image_format.h>
#include <sonore/gfx/image.h>
#include <sonore/gfx/parallelogram.h>
#include <sonore/gfx/font_match.h>
#include <sonore/audio_source.h>
#include <sonore/gfx/component_listeners.h>
#include <sonore/gfx/cached_image.h>
#include <sonore/gfx/paint_diagnostics.h>
#include <sonore/gfx/lookandfeel_versions.h>
#include <sonore/gfx/drawable_component.h>
#include <sonore/gfx/mouse_input.h>
#include <sonore/gfx/text_drag.h>
#include <sonore/gfx/relative_position.h>
#include <sonore/gfx/component_builder.h>
#include <sonore/gfx/movement_watcher.h>
#include <sonore/gfx/animated_position.h>
#include <sonore/gfx/vblank.h>
#include <sonore/gfx/code_editor.h>
#include <sonore/streams.h>
#include <sonore/file_types.h>
#include <sonore/net.h>
#include <sonore/process.h>
#include <sonore/ipc.h>
#include <sonore/analytics.h>
#include <sonore/gfx/button_tracker.h>
#include <sonore/gfx/video.h>
#include <sonore/gfx/maths3d.h>
#include <sonore/gfx/opengl_api.h>
#include <sonore/gfx/opengl.h>
#include <sonore/bluetooth_midi.h>
#include <sonore/gfx/text_input.h>
#include <sonore/gfx/accessibility_mac.h>
#include <sonore/javascript.h>
#include <sonore/message_manager.h>
#include <sonore/application_properties.h>
#include <sonore/file_document.h>
#include <sonore/recent_files.h>
#include <sonore/gfx/preferences_panel.h>
#include <sonore/application.h>
#include <sonore/gfx/document_window.h>
#include <sonore/gfx/message_box.h>
#include <sonore/gfx/progress_window.h>
#include <sonore/gfx/splash_screen.h>
#include <sonore/gfx/system_tray.h>
#include <sonore/gfx/native_embed.h>
#include <sonore/host_au.h>
#include <sonore/audio_workgroup.h>
#include <sonore/apple_block.h>
#include <sonore/gfx/nsview_component.h>
#include <sonore/user_presets.h>
#include <sonore/gfx/preset_bar.h>
#include <sonore/osc.h>
#include <sonore/midi_input.h>
#include <sonore/midi_output.h>

#include <cstdio>

int main() {
  const sonore::simd::Vec4f a = sonore::simd::Vec4f::splat(1.0f), b = sonore::simd::Vec4f::splat(2.0f);
  const bool ok = (a.max)(b).sum() == 8.0f && (std::max)(1, 2) == 2 && max(3, 4) == 4;
  std::printf("  %s windows.h's min and max macros leave the SDK compiling, and still work in user code\n",
              ok ? "ok  " : "FAIL");

  // osc.h on winsock.h: the header chose it, and a message makes the trip.
#if defined(_WINSOCK2API_)
  const bool oldWinsock = false;
#else
  const bool oldWinsock = true;
#endif
  sonore::osc::Receiver rx;
  sonore::osc::Sender tx;
  sonore::osc::Message sent;
  sent.address = "/winsock";
  sent.arguments.push_back(sonore::osc::Argument::makeInt(42));
  bool arrived = false;
  if (rx.open(0) && tx.open("127.0.0.1", rx.port()) && tx.send(sent)) {
    for (int i = 0; i < 100 && !arrived; ++i) {
      const std::vector<sonore::osc::Message> got = rx.poll();
      arrived = got.size() == 1 && got[0].address == "/winsock" && got[0].arguments.size() == 1 &&
                got[0].arguments[0].intValue == 42;
      if (!arrived) Sleep(5);
    }
  } else {
    std::printf("  skip no loopback socket here: %s\n", rx.error().c_str());
    arrived = true;
  }
  std::printf("  %s osc.h compiles after the old winsock.h windows.h brought in, and an OSC message "
              "makes the loopback trip through it\n",
              oldWinsock && arrived ? "ok  " : "FAIL");
  return ok && oldWinsock && arrived ? 0 : 1;
}
