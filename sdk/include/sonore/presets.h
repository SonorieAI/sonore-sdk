// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: factory presets.
//
// A plugin with eight knobs and no starting points is a plugin most people
// never hear at its best. Presets are plain data on the descriptor: a name and
// the parameter values, in the contract's index order: the same order the DSP
// and the UI already agree on, so nothing new has to be kept in step.
//
// Deliberately NOT a file format. Presets ship compiled into the binary, which
// means they cannot go missing, cannot drift from the parameter set that
// shipped with them, and cost nothing to load. The host's own preset system
// handles user-saved states through the state extension we already implement.
#pragma once
#include <cstddef>
#include "plugin.h"

namespace sonore {

/** One factory preset. `values` must have exactly the plugin's parameter count,
 *  in index order; a mismatch is refused at load rather than partially applied. */
struct Preset {
  const char* name = "";
  const float* values = nullptr;
  int numValues = 0;
};

/** Apply a preset to a parameter array. Returns false, changing nothing, when
 *  the preset does not match the current contract, which is what stops a stale
 *  preset from writing garbage into half the controls. */
inline bool applyPreset(const Preset& preset, const ParamInfo* params, int numParams,
                        float* out) {
  if (!preset.values || preset.numValues != numParams || !params || !out) return false;
  for (int i = 0; i < numParams; ++i) out[i] = clampToRange(params[i], preset.values[i]);
  return true;
}

} // namespace sonore
