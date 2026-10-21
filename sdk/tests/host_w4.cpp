// SPDX-License-Identifier: Apache-2.0
// The host side of the SDK, compiled at /W4 /WX (see the sonore_w4_* targets
// in CMakeLists.txt). The plugin and standalone gates never included host.h,
// and a shadowed name in its VST3 process (C4456) reached a program built at
// /W4 before anything here noticed. Every header a host program uses, with
// the VST3 and LV2 halves compiled in.
#include <sonore/graph.h>
#include <sonore/host.h>
#include <sonore/plugin_cache.h>

int main() {
  sonore::host::PluginCache cache;
  return sonore::host::scanDirectory("does-not-exist").empty() ? 0 : 1;
}
