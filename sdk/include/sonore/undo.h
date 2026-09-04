// SPDX-License-Identifier: Apache-2.0
//
// UndoHistory: undo and redo for a plugin's own editor.
//
// ── What this is not ────────────────────────────────────────────────────────
//
// It is NOT undo for parameter automation. A host already owns that: a knob
// moved through the parameter API is recorded by the DAW, appears in its undo
// stack, and is undone with the DAW's own shortcut. A plugin keeping a second
// stack for the same edits would fight it, and the user would discover which
// one their keyboard was talking to by losing work.
//
// It IS undo for everything the host cannot see -- the things that live in a
// StateBag. Which sample is loaded in which slot, the shape somebody drew in
// an envelope editor, the step pattern in a sequencer. A host has no concept
// of those, so nothing undoes them unless the plugin does.
//
// ── Snapshots, not commands ─────────────────────────────────────────────────
//
// Whole-state snapshots rather than an undoable-action interface. A generated
// plugin's editor is a web page written by a language model; asking it to
// implement a correct inverse for every edit is asking for the class of bug
// where redo produces something that was never on screen. Storing what the
// state WAS cannot get that wrong.
//
// The cost is memory, bounded by a step limit and by the fact that a StateBag
// holds names and paths rather than audio.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sonore {

/**
 * A bounded stack of state snapshots.
 *
 * `begin()` records where the state was BEFORE an edit, which is the only
 * order that works: a snapshot taken afterwards has already lost the thing
 * undo is supposed to restore.
 */
class UndoHistory {
public:
  struct Step {
    std::string state;
    std::string label;
    /** Whatever clock the caller uses -- milliseconds since the program
     *  started is typical. Only compared against other stamps, never read as a
     *  date, so its origin is the caller's business. */
    uint64_t stamp = 0;
  };

  explicit UndoHistory(size_t maxSteps = 64) : maxSteps_(maxSteps < 1 ? 1 : maxSteps) {}

  /** Forget everything and treat `state` as the starting point. */
  void reset(const std::string& state) {
    steps_.clear();
    position_ = 0;
    current_ = state;
  }

  /**
   * Record an edit that is about to happen.
   *
   * `previous` is the state before it. `label` names the edit -- "load sample",
   * "draw envelope" -- and is what coalescing compares.
   *
   * `coalesceWindow` merges consecutive edits with the SAME label that arrive
   * within that many stamp units of each other. Dragging a shape in an editor
   * fires a hundred times a second, and a hundred undo steps for one gesture
   * is an undo stack nobody can get out of.
   */
  void push(const std::string& previous, const std::string& label, uint64_t stamp = 0,
            uint64_t coalesceWindow = 0) {
    // A redo branch that is no longer reachable. Editing after undoing
    // discards it, which is what every editor does: keeping it would mean
    // answering "redo to which future".
    if (position_ < steps_.size()) steps_.resize(position_);

    if (coalesceWindow > 0 && !steps_.empty()) {
      Step& last = steps_.back();
      // Only forward in time. A stamp that went backwards is a caller mixing
      // clocks, and merging on it would swallow an unrelated edit.
      if (last.label == label && stamp >= last.stamp && stamp - last.stamp <= coalesceWindow) {
        // The EARLIER snapshot is kept. Undo should land where the gesture
        // started, not one frame into it.
        last.stamp = stamp;
        position_ = steps_.size();
        return;
      }
    }

    steps_.push_back(Step{previous, label, stamp});
    if (steps_.size() > maxSteps_) steps_.erase(steps_.begin());
    position_ = steps_.size();
  }

  bool canUndo() const { return position_ > 0; }
  bool canRedo() const { return position_ < steps_.size(); }

  /** The label of the edit undo would reverse, for a menu item that can say
   *  "Undo load sample" rather than just "Undo". */
  std::string undoLabel() const { return canUndo() ? steps_[position_ - 1].label : std::string(); }
  std::string redoLabel() const { return canRedo() ? steps_[position_].label : std::string(); }

  /**
   * Step back. `state` is what the state is NOW -- it is stored so redo can
   * return to it -- and the return value is what to restore.
   *
   * Returns an empty string and changes nothing when there is nothing to undo.
   * A caller that does not check gets a no-op rather than a cleared plugin.
   */
  std::string undo(const std::string& state) {
    if (!canUndo()) return std::string();
    --position_;
    std::string restore = steps_[position_].state;
    // Swap: the slot now holds the future rather than the past, which is what
    // makes one array serve both directions.
    steps_[position_].state = state;
    return restore;
  }

  std::string redo(const std::string& state) {
    if (!canRedo()) return std::string();
    std::string restore = steps_[position_].state;
    steps_[position_].state = state;
    ++position_;
    return restore;
  }

  size_t size() const { return steps_.size(); }
  size_t position() const { return position_; }

private:
  std::vector<Step> steps_;
  size_t position_ = 0;
  size_t maxSteps_;
  std::string current_;
};

} // namespace sonore
