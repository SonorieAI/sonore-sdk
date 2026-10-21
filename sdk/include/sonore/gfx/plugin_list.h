// SPDX-License-Identifier: Apache-2.0
//
// PluginListComponent: the table a host shows of the plugins it knows about.
//
// ── What it is a view of ────────────────────────────────────────────────────
//
// A PluginCache, which already is the list: what scans found, what was
// blacklisted after killing a scan, saved to a text file a person can read.
// This component owns none of that. It draws the cache as a table -- name,
// format, category, manufacturer -- and its buttons are the cache's own
// operations: scan, remove, forget a blacklisting. Every change is written
// back to the cache file straight away when it has one, because a list that
// only remembered its edits until the host crashed would be remembering them
// for the one event the cache exists to survive.
//
// ── Scanning, in this process ───────────────────────────────────────────────
//
// The reference can scan in a child process. This SDK does not start child
// processes from a plugin host library (see the feature map's
// ChildProcessCoordinator row), so a scan here is the in-process one with the
// dead man's switch: plugin_cache.h writes the path being scanned to disk
// before opening it, and a host that died inside a plugin's code finds the
// mark on its next start and blacklists that file. Blacklisted files are
// listed below the plugins, so a user can see what was set aside and give it
// another chance.
//
// ── Sorting belongs to the rows ─────────────────────────────────────────────
//
// TableListBox reports a header click and leaves the order to its owner,
// which is this: the rows are a permutation of the cache's list, re-sorted
// whenever the list changes, so a scan that adds forty plugins does not throw
// away the column the user was sorting by.
#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "../plugin_cache.h"
#include "table.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * A host's table of known plugins, over a PluginCache, with Scan and Remove
 * buttons -- the reference's PluginListComponent, scanning in-process. The
 * file comment says what it is a view of and how a crashing scan is survived.
 *
 * The cache is REFERENCED and must outlive the component; with a cache path,
 * every change is saved to it at once. A scan runs on the calling thread and
 * opens each new or changed plugin file there, so the Scan button blocks the
 * message thread for as long as those take to load. Call refresh() after changing the
 * cache from anywhere else. Message thread only.
 */
class PluginListComponent : public Widget {
public:
  /** The columns, in order. */
  enum Column {
    kNameColumn = 0,     ///< The plugin's name; a blacklisted file's file name.
    kFormatColumn,       ///< "CLAP", "VST3" and so on, as the cache has it.
    kCategoryColumn,     ///< categoryOf(); "Blacklisted" for a blacklisted file.
    kManufacturerColumn, ///< The vendor.
    kNumColumns          ///< How many columns there are; not a column.
  };

  /** The strip under the table holding Scan and Remove, logical units;
   *  dropped when the component is less than twice this tall. */
  static constexpr float kButtonBarHeight = 30.0f;

  /**
   * `list` must outlive this component. `cachePath` is where the list is
   * saved after every change and where a scan writes its dead man's mark;
   * empty means the list is kept in memory only and a scan is unprotected,
   * which is right for a test and wrong for a host.
   */
  explicit PluginListComponent(host::PluginCache& list, std::string cachePath = std::string())
      : list_(list), cachePath_(std::move(cachePath)) {
    setAccessibilityIgnored(true);
    table_.setColumns({{"Name", 220.0f}, {"Format", 70.0f}, {"Category", 110.0f},
                       {"Manufacturer", 160.0f}});
    table_.setAccessibleName("Plugins");
    table_.getCellText = [this](int row, int column) { return cellText(row, column); };
    table_.onSortChanged = [this](int column, bool forwards) {
      sortColumn_ = column;
      sortForwards_ = forwards;
      sortRows();
      table_.updateContent();
    };
    table_.onRowChosen = [this](int row) {
      if (const host::PluginDescription* d = pluginForRow(row))
        if (onPluginChosen) onPluginChosen(*d);
    };
    scanButton_.onClick = [this]() { scanFor(scanDirectories_); };
    removeButton_.onClick = [this]() { removeSelected(); };
    addChild(&table_);
    addChild(&scanButton_);
    addChild(&removeButton_);
    refresh();
  }

  /** The table, owned by this component: for its selection, or a test.
   *  Its cell text, sort and row-chosen callbacks are this component's. */
  TableListBox& table() { return table_; }
  /** The Scan button: scanFor(scanDirectories()). */
  Button& scanButton() { return scanButton_; }
  /** The Remove button: removeSelected(). */
  Button& removeButton() { return removeButton_; }

  /** Where the Scan button looks. Each format's files in each folder are
   *  scanned: the bundle formats through pluginFilesIn, LADSPA through its
   *  own filter. */
  void setScanDirectories(std::vector<std::string> directories) {
    scanDirectories_ = std::move(directories);
  }
  /** The folders setScanDirectories() set; none by default, so Scan finds
   *  nothing until it is called. */
  const std::vector<std::string>& scanDirectories() const { return scanDirectories_; }

  /** Rebuild the rows from the cache, keeping the sort. Call it after
   *  changing the cache from elsewhere. */
  void refresh() {
    rows_.clear();
    for (size_t i = 0; i < list_.plugins().size(); ++i) rows_.push_back(Row{(int) i, std::string()});
    for (const std::string& path : list_.blacklist()) rows_.push_back(Row{-1, path});
    sortRows();
    table_.setNumRows((int) rows_.size());
    table_.updateContent();
  }

  /** The cache's plugins plus its blacklisted files, one row each, as of
   *  the last refresh. */
  int numRows() const { return (int) rows_.size(); }

  /** The plugin a row shows, or null for a blacklisted file or a row out of
   *  range. */
  const host::PluginDescription* pluginForRow(int row) const {
    if (row < 0 || row >= (int) rows_.size() || rows_[(size_t) row].plugin < 0) return nullptr;
    const auto& plugins = list_.plugins();
    const size_t index = (size_t) rows_[(size_t) row].plugin;
    return index < plugins.size() ? &plugins[index] : nullptr;
  }

  /** The row is a blacklisted file, listed below the plugins; false out of
   *  range. */
  bool rowIsBlacklisted(int row) const {
    return row >= 0 && row < (int) rows_.size() && rows_[(size_t) row].plugin < 0;
  }

  /** What one cell says. A blacklisted file shows its file name where a
   *  plugin's name would be and says why it is there in the category
   *  column -- it has no format or maker the host ever learned. */
  std::string cellText(int row, int column) const {
    if (row < 0 || row >= (int) rows_.size()) return std::string();
    const Row& r = rows_[(size_t) row];
    if (r.plugin < 0) {
      if (column == kNameColumn) return fileNameOf(r.blacklisted);
      if (column == kCategoryColumn) return "Blacklisted";
      return std::string();
    }
    const host::PluginDescription& d = list_.plugins()[(size_t) r.plugin];
    switch (column) {
      case kNameColumn: return d.name;
      case kFormatColumn: return d.format;
      case kCategoryColumn: return categoryOf(d);
      case kManufacturerColumn: return d.vendor;
      default: return std::string();
    }
  }

  /** Sort by a column, as clicking its header does. Plugins sort among
   *  themselves; blacklisted files stay at the bottom, in path order, where
   *  the reference keeps them. */
  void sortBy(int column, bool forwards) { table_.setSort(column, forwards); }

  /**
   * Scan these folders for every format this host can load, through the
   * cache: unchanged files are not reopened, blacklisted ones are not opened
   * at all, and a file that kills the process is blacklisted next time.
   * Returns how many plugins the folders hold.
   */
  size_t scanFor(const std::vector<std::string>& directories) {
    std::vector<std::string> files;
    for (const std::string& directory : directories) {
      for (const std::string& f : host::pluginFilesIn(directory)) files.push_back(f);
      for (const std::string& f : host::LADSPAPluginFormat::filesIn(directory)) files.push_back(f);
    }
    return scanFiles(files);
  }

  /** The same for files named one by one -- a file dropped on the list. */
  size_t scanFiles(const std::vector<std::string>& files) {
    const char* cache = cachePath_.empty() ? nullptr : cachePath_.c_str();
    const size_t found = host::scanFilesCached(files, list_, cache).size();
    changed();
    return found;
  }

  /**
   * Remove one row. A plugin leaves the list (and its file is scanned afresh
   * next time, so it comes back if it is still there -- see
   * PluginCache::removePlugin). A blacklisted file is FORGIVEN: removing it
   * from the blacklist is the only thing removing that row can mean.
   */
  bool removeRow(int row) {
    if (row < 0 || row >= (int) rows_.size()) return false;
    const Row r = rows_[(size_t) row];
    bool removed = false;
    if (r.plugin < 0) {
      removed = list_.forgive(r.blacklisted);
    } else if (r.plugin < (int) list_.plugins().size()) {
      const host::PluginDescription d = list_.plugins()[(size_t) r.plugin];
      removed = list_.removePlugin(d.path, d.id);
    }
    if (removed) changed();
    return removed;
  }

  /** removeRow() on the table's selected row; the selection then moves to
   *  the row that took its place. False when nothing is selected or nothing
   *  was removed. */
  bool removeSelected() {
    const int row = table_.selectedRow();
    const bool removed = removeRow(row);
    // The selection stays on the same POSITION, which is now the next row --
    // so pressing Remove repeatedly walks down the list, as it does in every
    // list a person has ever pruned.
    if (removed && numRows() > 0) table_.setSelectedRow((std::min)(row, numRows() - 1), false);
    return removed;
  }

  /** Remove every plugin whose file is no longer on disk. */
  size_t removeMissing() {
    std::vector<host::PluginDescription> gone;
    for (const host::PluginDescription& d : list_.plugins())
      if (!host::stampOf(d.path).valid()) gone.push_back(d);
    for (const host::PluginDescription& d : gone) list_.removePlugin(d.path, d.id);
    if (!gone.empty()) changed();
    return gone.size();
  }

  /** Every plugin off the list; the blacklist is kept, as the reference's
   *  "clear list" keeps it -- forgetting what crashed is a separate
   *  decision. */
  void clearList() {
    const std::vector<host::PluginDescription> all = list_.plugins();
    for (const host::PluginDescription& d : all) list_.removePlugin(d.path, d.id);
    changed();
  }

  /** Forgive every blacklisted file: each is opened again by the next scan
   *  that finds it. The plugins stay. */
  void clearBlacklist() {
    const std::vector<std::string> all = list_.blacklist();
    for (const std::string& path : all) list_.forgive(path);
    changed();
  }

  /** A double click or Return on a plugin row. */
  std::function<void(const host::PluginDescription&)> onPluginChosen;
  /** After anything this component did to the list. */
  std::function<void()> onListChanged;

  /**
   * One word for what a plugin is, from whatever its format calls it.
   *
   * CLAP lists feature words ("audio-effect;delay;stereo"), VST3 joins
   * subcategories with bars ("Fx|Delay"), LV2 names RDF classes
   * ("...lv2core#DelayPlugin"), and LADSPA says nothing at all. The first
   * word that is not about the plugin's plumbing -- effect or instrument,
   * mono or stereo -- is the category; a plugin that names none is an
   * "Instrument" or an "Effect".
   */
  static std::string categoryOf(const host::PluginDescription& d) {
    static const char* const kPlumbing[] = {
        "audio-effect", "audio_effect", "fx", "instrument", "instrumentplugin", "synth",
        "synthesizer", "note-effect", "stereo", "mono", "surround", "ambisonic", "ladspa"};
    std::string word;
    size_t pos = 0;
    while (pos <= d.features.size()) {
      size_t end = d.features.find_first_of(";|", pos);
      if (end == std::string::npos) end = d.features.size();
      std::string token = d.features.substr(pos, end - pos);
      pos = end + 1;
      // An LV2 class is a URI: the name is after the '#', and the "Plugin"
      // every class name ends in says nothing.
      const size_t hash = token.find('#');
      if (hash != std::string::npos) token = token.substr(hash + 1);
      if (token.size() > 6 && token.compare(token.size() - 6, 6, "Plugin") == 0)
        token.resize(token.size() - 6);
      if (token.empty() || token.find('=') != std::string::npos) continue;
      std::string lower = token;
      for (char& c : lower)
        if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
      bool plumbing = false;
      for (const char* p : kPlumbing) plumbing = plumbing || lower == p;
      if (plumbing) continue;
      word = token;
      break;
    }
    if (word.empty()) return d.isInstrument ? "Instrument" : "Effect";
    if (word[0] >= 'a' && word[0] <= 'z') word[0] = (char) (word[0] - 'a' + 'A');
    for (char& c : word)
      if (c == '-' || c == '_') c = ' ';
    return word;
  }

  /** The table above, Scan and Remove in the strip below it. */
  void resized() override {
    const Rect b = localBounds();
    const float bar = b.h > kButtonBarHeight * 2.0f ? kButtonBarHeight : 0.0f;
    table_.setBounds({0.0f, 0.0f, b.w, b.h - bar});
    const float w = 90.0f, gap = 6.0f, y = b.h - bar + 3.0f, h = bar - 6.0f;
    scanButton_.setBounds({gap, y, w, h > 0.0f ? h : 0.0f});
    removeButton_.setBounds({gap * 2.0f + w, y, w, h > 0.0f ? h : 0.0f});
  }

  /** Fills with the look and feel's background, behind the buttons. */
  void paint(Graphics& g) override {
    g.setColour(lookAndFeel().background());
    g.fillRect(localBounds());
  }

private:
  /** One table row, in display order: a plugin, by its index in the cache's
   *  list, or a blacklisted file. Indices, so a re-sort moves nothing but
   *  these. */
  struct Row {
    int plugin;              // index into the cache's list, or -1
    std::string blacklisted; // the file, when plugin is -1
  };

  void changed() {
    if (!cachePath_.empty()) list_.save(cachePath_.c_str());
    refresh();
    if (onListChanged) onListChanged();
  }

  static std::string fileNameOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }

  /** Case-insensitive: "aether" and "Zebra" belong in that order whatever
   *  the vendor's taste in capitals. */
  static int compareText(const std::string& a, const std::string& b) {
    const size_t n = (std::min)(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
      char x = a[i], y = b[i];
      if (x >= 'A' && x <= 'Z') x = (char) (x - 'A' + 'a');
      if (y >= 'A' && y <= 'Z') y = (char) (y - 'A' + 'a');
      if (x != y) return (unsigned char) x < (unsigned char) y ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
  }

  void sortRows() {
    const int column = sortColumn_ >= 0 && sortColumn_ < kNumColumns ? sortColumn_ : kNameColumn;
    const bool forwards = sortColumn_ >= 0 ? sortForwards_ : true;
    // Stable, with the name and then the path as tie-breaks, so equal
    // categories list alphabetically and the same list always draws the same
    // way -- a table that shuffled its ties on every refresh would look like
    // it was changing under the user.
    std::stable_sort(rows_.begin(), rows_.end(), [&](const Row& a, const Row& b) {
      if ((a.plugin < 0) != (b.plugin < 0)) return a.plugin >= 0; // plugins first
      if (a.plugin < 0) return compareText(a.blacklisted, b.blacklisted) < 0;
      const host::PluginDescription& x = list_.plugins()[(size_t) a.plugin];
      const host::PluginDescription& y = list_.plugins()[(size_t) b.plugin];
      int c = compareText(textFor(x, column), textFor(y, column));
      if (!forwards) c = -c;
      if (c == 0) c = compareText(x.name, y.name);
      if (c == 0) c = compareText(x.path, y.path);
      return c < 0;
    });
  }

  static std::string textFor(const host::PluginDescription& d, int column) {
    switch (column) {
      case kFormatColumn: return d.format;
      case kCategoryColumn: return categoryOf(d);
      case kManufacturerColumn: return d.vendor;
      default: return d.name;
    }
  }

  host::PluginCache& list_;
  std::string cachePath_;
  std::vector<std::string> scanDirectories_;
  std::vector<Row> rows_;
  int sortColumn_ = -1;
  bool sortForwards_ = true;
  TableListBox table_;
  Button scanButton_{"Scan"};
  Button removeButton_{"Remove"};
};

} // namespace gfx
} // namespace sonore
