// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: files as streams, mapped files, temporary files, wildcard
// filters, search paths, and the list of mounted volumes.
//
// files.h answers "what is in this folder" and path_io.h "open this UTF-8
// path"; this is the layer a plugin reaches for after that, named as the
// reference names it so code written against it ports by renaming types:
//
//   FileInputStream / FileOutputStream   a file as an InputStream/OutputStream
//                                        (streams.h), by UTF-8 path
//   FileInputSource                      a file that can be reopened per use
//   TemporaryFile                        write beside the target, then swap it
//                                        in -- a save that cannot leave half
//                                        a preset behind
//   MemoryMappedFile                     a file's bytes in memory, paged in
//                                        by the system as they are touched
//   WildcardFileFilter                   "*.wav;*.aif" matching
//   FileSearchPath                       a list of folders, searched in order
//   MountedVolumeListChangeDetector      notice a drive arriving or leaving
//
// ── Decisions worth knowing ─────────────────────────────────────────────────
//
// UTF-8 EVERYWHERE. Every path in and out is UTF-8 and reaches the Windows
// wide API through path_io.h, long-path prefix included -- the same rule as
// the rest of the SDK, for the users whose folders are not named in English.
//
// FileOutputStream APPENDS to a file that exists, as the reference's does.
// It is a trap, and it is kept because silently changing what a ported save
// routine does is a worse one: setPosition(0) and truncate() after opening
// (it opens at the end, where truncate() cuts nothing), or write through a
// TemporaryFile -- which is what a save should do anyway.
//
// WILDCARDS IGNORE CASE on every platform, unlike the reference (which
// follows the file system's case rule). "*.WAV" refusing "kick.wav" on Linux
// helps nobody, and files.h's extension filter already ignores case.
//
// MemoryMappedFile is the general form of the mapping system_font.h does for
// font files (mapFileBytes): the same calls, any range, read or read-write.
// A mapped file must not be truncated by someone else while mapped -- POSIX
// raises SIGBUS on the next touch of a page past the new end -- so map files
// the plugin owns (its samples, its bundle), and read anything else.
//
// NOT FOR THE AUDIO THREAD. Opening, reading, writing, renaming and polling
// all do I/O and may block for as long as a disk or a network share takes.
// A touched page of a mapped file can block too: it is a disk read in
// disguise. The detector's callback runs on its own thread.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "files.h"
#include "path_io.h"
#include "streams.h"
#include "uuid.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sonore {

namespace filedetail {

/** 64-bit seeks: plain fseek takes a long, which is 32 bits on Windows, and a
 *  sample library has files past 2 GB. */
inline bool seek(std::FILE* f, int64_t pos, int whence) {
#if defined(_WIN32)
  return _fseeki64(f, pos, whence) == 0;
#else
  return fseeko(f, (off_t) pos, whence) == 0;
#endif
}
inline int64_t tell(std::FILE* f) {
#if defined(_WIN32)
  return (int64_t) _ftelli64(f);
#else
  return (int64_t) ftello(f);
#endif
}

/** Eight random hex digits, for a name nobody else is using. From uuid.h's
 *  entropy, not random.h's deterministic generator: two instances of a plugin
 *  saving at once must not pick the same temporary name. */
inline std::string randomHex(int digits) {
  const Uuid u = makeUuid();
  static const char hex[] = "0123456789abcdef";
  std::string out;
  for (int i = 0; i < digits && i < 32; ++i) out.push_back(hex[(u.bytes[i / 2] >> ((i & 1) ? 0 : 4)) & 15]);
  return out;
}

inline void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace filedetail

/** The system's folder for temporary files, without a trailing separator:
 *  %TEMP% on Windows (GetTempPath), $TMPDIR or /tmp elsewhere. */
inline std::string temporaryDirectory() {
#if defined(_WIN32)
  wchar_t buffer[MAX_PATH + 2];
  const DWORD n = GetTempPathW((DWORD) (sizeof(buffer) / sizeof(buffer[0])), buffer);
  std::string dir = (n > 0 && n < sizeof(buffer) / sizeof(buffer[0])) ? pathio::narrow(buffer) : "C:\\Windows\\Temp";
#else
  std::string dir = pathio::environment("TMPDIR");
  if (dir.empty()) dir = "/tmp";
#endif
  while (dir.size() > 1 && isSeparator(dir.back())) dir.pop_back();
  return dir;
}

/** A file, read as a stream. */
class FileInputStream : public InputStream {
public:
  /** Opens the UTF-8 `path` for reading; openedOk() says whether it
   *  opened. getTotalLength() is the size at this moment: a file that grows
   *  while it is read keeps reporting the old size. */
  explicit FileInputStream(const std::string& path) : path_(path) {
    file_ = pathio::open(path.c_str(), "rb");
    if (!file_) return;
    if (filedetail::seek(file_, 0, SEEK_END)) total_ = filedetail::tell(file_);
    filedetail::seek(file_, 0, SEEK_SET);
    if (total_ < 0) total_ = 0;
  }
  /** Closes the file. */
  ~FileInputStream() override {
    if (file_) std::fclose(file_);
  }
  ///@{
  /** Not copyable: the stream owns an open file. */
  FileInputStream(const FileInputStream&) = delete;
  FileInputStream& operator=(const FileInputStream&) = delete;
  ///@}

  /** The path as given to the constructor. */
  const std::string& getFile() const noexcept { return path_; }
  /** True when the file opened. A stream that did not open reads nothing
   *  and reports a length of 0. */
  bool openedOk() const noexcept { return file_ != nullptr; }
  /** The opposite of openedOk(). */
  bool failedToOpen() const noexcept { return file_ == nullptr; }

  int64_t getTotalLength() override { return total_; }
  bool isExhausted() override { return !file_ || position_ >= total_; }
  /** In bytes from the start. setPosition() may go past the end, where
   *  reads return 0. */
  int64_t getPosition() override { return position_; }
  bool setPosition(int64_t pos) override {
    if (!file_) return false;
    if (pos < 0) pos = 0;
    if (pos == position_) return true;
    if (!filedetail::seek(file_, pos, SEEK_SET)) return false;
    position_ = pos;
    return true;
  }
  int read(void* dest, int maxBytesToRead) override {
    if (!file_ || !dest || maxBytesToRead <= 0) return 0;
    const size_t got = std::fread(dest, 1, (size_t) maxBytesToRead, file_);
    position_ += (int64_t) got;
    return (int) got;
  }
  void skipNextBytes(int64_t numBytesToSkip) override {
    if (numBytesToSkip <= 0) return;
    // A count past what the position can add -- a chunk size read from a
    // damaged file -- wrapped the sum negative and setPosition took it as 0:
    // the skip rewound the file to its start. Past anything a file holds is
    // its end.
    if (numBytesToSkip > INT64_MAX - position_) setPosition(total_ > position_ ? total_ : position_);
    else setPosition(position_ + numBytesToSkip);
  }

private:
  std::string path_;
  std::FILE* file_ = nullptr;
  int64_t total_ = 0;
  int64_t position_ = 0;
};

/** A file, written as a stream. An existing file is opened at its END -- see
 *  the header -- and created when it does not exist (its folder must). */
class FileOutputStream : public OutputStream {
public:
  /** Opens the UTF-8 `path` for writing, positioned at the end of what is
   *  there, or creates it; openedOk() says whether either worked. */
  explicit FileOutputStream(const std::string& path) : path_(path) {
    file_ = pathio::open(path.c_str(), "r+b"); // existing, untouched
    if (!file_) file_ = pathio::open(path.c_str(), "w+b");
    if (file_ && filedetail::seek(file_, 0, SEEK_END)) position_ = filedetail::tell(file_);
  }
  /** Flushes and closes the file. A failure while writing out the last
   *  buffered bytes is not reported. */
  ~FileOutputStream() override {
    if (file_) std::fclose(file_);
  }
  ///@{
  /** Not copyable: the stream owns an open file. */
  FileOutputStream(const FileOutputStream&) = delete;
  FileOutputStream& operator=(const FileOutputStream&) = delete;
  ///@}

  /** The path as given to the constructor. */
  const std::string& getFile() const noexcept { return path_; }
  /** True when the file opened or was created. Writes to a stream that did
   *  not open return false. */
  bool openedOk() const noexcept { return file_ != nullptr; }
  /** The opposite of openedOk(). */
  bool failedToOpen() const noexcept { return file_ == nullptr; }

  /** Cuts the file off at the current position. The stream opens at the
   *  END, where this cuts nothing: to replace a file's contents rather than
   *  append to them, setPosition(0) first. */
  bool truncate() {
    if (!file_) return false;
    std::fflush(file_);
#if defined(_WIN32)
    return _chsize_s(_fileno(file_), position_) == 0;
#else
    return ftruncate(fileno(file_), (off_t) position_) == 0;
#endif
  }

  void flush() override {
    if (file_) std::fflush(file_);
  }
  /** In bytes from the start of the file. */
  int64_t getPosition() override { return position_; }
  /** Moves the write position; false when the file did not open, `pos` is
   *  negative or the seek failed. A position past the end is allowed, and a
   *  write there extends the file. */
  bool setPosition(int64_t pos) override {
    if (!file_ || pos < 0) return false;
    if (!filedetail::seek(file_, pos, SEEK_SET)) return false;
    position_ = pos;
    return true;
  }
  bool write(const void* data, size_t n) override {
    if (n == 0) return true;
    if (!file_ || !data) return false;
    const size_t done = std::fwrite(data, 1, n, file_);
    position_ += (int64_t) done;
    return done == n;
  }

private:
  std::string path_;
  std::FILE* file_ = nullptr;
  int64_t position_ = 0;
};

/** A file as an InputSource: reopened for each stream asked of it. */
class FileInputSource : public InputSource {
public:
  /** For the UTF-8 `path`; nothing is opened until a stream is asked for.
   *  `useFileTimeInHashGeneration` puts the file's modification time into
   *  hashCode(), at the cost of a stat on each call. */
  explicit FileInputSource(const std::string& path, bool useFileTimeInHashGeneration = false)
      : path_(path), useTime_(useFileTimeInHashGeneration) {}

  std::unique_ptr<InputStream> createInputStream() override {
    auto stream = std::make_unique<FileInputStream>(path_);
    if (stream->failedToOpen()) return nullptr;
    return stream;
  }
  std::unique_ptr<InputStream> createInputStreamFor(const std::string& relatedItemPath) override {
    auto stream = std::make_unique<FileInputStream>(joinPath(parentPath(path_), relatedItemPath));
    if (stream->failedToOpen()) return nullptr;
    return stream;
  }
  /** FNV-1a of the path, and of the modification time when asked: the same
   *  file rewritten in place hashes differently, so a cache notices. */
  int64_t hashCode() const override {
    uint64_t h = 14695981039346656037ULL;
    for (char c : path_) h = (h ^ (uint8_t) c) * 1099511628211ULL;
    if (useTime_) {
      const int64_t t = modifiedTime();
      for (int i = 0; i < 8; ++i) h = (h ^ (uint8_t) (t >> (8 * i))) * 1099511628211ULL;
    }
    return (int64_t) h;
  }

private:
  int64_t modifiedTime() const {
#if defined(_WIN32)
    struct _stat64 st {};
    return _wstat64(pathio::widenPath(path_.c_str()).c_str(), &st) == 0 ? (int64_t) st.st_mtime : 0;
#else
    struct stat st {};
    return ::stat(path_.c_str(), &st) == 0 ? (int64_t) st.st_mtime : 0;
#endif
  }
  std::string path_;
  bool useTime_ = false;
};

/**
 * A file to write instead of the real one, swapped in when it is complete.
 *
 * A save that writes the target directly and is interrupted -- a crash, a
 * full disk, a host killed mid-save -- leaves a truncated preset where a good
 * one was. Written beside the target and renamed over it, the target is
 * either the old file or the new one, never half of each: the rename is
 * atomic on one volume on every platform here.
 *
 * The temporary file is deleted when this is destroyed, if it is still there.
 */
class TemporaryFile {
public:
  /** Flags for forTarget() and inTemporaryDirectory(). */
  enum OptionFlags {
    useHiddenFile = 1 ///< name starts with '.': hidden on macOS and Linux, not on Windows
  };

  /** A temporary beside `targetFile`, named after it ("preset_temp3f9a01c2.json"),
   *  for overwriteTargetFileWithTemporary(). */
  static TemporaryFile forTarget(const std::string& targetFile, int optionFlags = 0) {
    TemporaryFile t;
    t.target_ = targetFile;
    const std::string name = fileName(targetFile);
    const size_t dot = name.rfind('.');
    const std::string stem = (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
    const std::string ext = (dot == std::string::npos || dot == 0) ? std::string() : name.substr(dot);
    const std::string prefix = (optionFlags & useHiddenFile) ? "." : "";
    t.temp_ = uniqueName(parentPath(targetFile), prefix + stem + "_temp", ext);
    return t;
  }

  /** A temporary in the system's temporary folder, ending in `suffix`. */
  static TemporaryFile inTemporaryDirectory(const std::string& suffix = std::string(), int optionFlags = 0) {
    TemporaryFile t;
    const std::string prefix = (optionFlags & useHiddenFile) ? ".temp_" : "temp_";
    t.temp_ = uniqueName(temporaryDirectory(), prefix, suffix);
    return t;
  }

  /** Takes over `other`'s names; `other` is left with none and deletes
   *  nothing when it is destroyed. */
  TemporaryFile(TemporaryFile&& other) noexcept
      : temp_(std::move(other.temp_)), target_(std::move(other.target_)) {
    other.temp_.clear();
    other.target_.clear();
  }
  /** Deletes this one's temporary file, if written, then takes over
   *  `other`'s names. */
  TemporaryFile& operator=(TemporaryFile&& other) noexcept {
    if (this != &other) {
      deleteTemporaryFile();
      temp_ = std::move(other.temp_);
      target_ = std::move(other.target_);
      other.temp_.clear();
      other.target_.clear();
    }
    return *this;
  }
  ///@{
  /** Not copyable: two owners would both delete the one temporary file. */
  TemporaryFile(const TemporaryFile&) = delete;
  TemporaryFile& operator=(const TemporaryFile&) = delete;
  ///@}
  /** Deletes the temporary file if it is still there -- a save that never
   *  reached overwriteTargetFileWithTemporary() leaves nothing behind. */
  ~TemporaryFile() { deleteTemporaryFile(); }

  /** Where to write. Nothing is created until somebody writes it. */
  const std::string& getFile() const noexcept { return temp_; }
  /** The file this one replaces; "" for one made by inTemporaryDirectory(). */
  const std::string& getTargetFile() const noexcept { return target_; }

  /**
   * Moves the temporary file over the target, replacing it. False when there
   * is no target, nothing was written, or the move failed -- in which case the
   * target is untouched.
   *
   * Retried for half a second on Windows: a virus scanner or the search
   * indexer opening a file it just saw being written is routine there, and
   * the replace fails for the moment it holds it.
   */
  bool overwriteTargetFileWithTemporary() const {
    if (target_.empty() || temp_.empty() || !fileExists(temp_)) return false;
#if defined(_WIN32)
    const std::wstring from = pathio::widenPath(temp_.c_str());
    const std::wstring to = pathio::widenPath(target_.c_str());
    for (int attempt = 0; attempt < 5; ++attempt) {
      if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
      filedetail::sleepMs(100);
    }
    return false;
#else
    return ::rename(temp_.c_str(), target_.c_str()) == 0;
#endif
  }

  /** Deletes the temporary file; true if it is gone (or was never written). */
  bool deleteTemporaryFile() const {
    if (temp_.empty() || !fileExists(temp_)) return true;
    for (int attempt = 0; attempt < 5; ++attempt) {
      if (pathio::remove(temp_.c_str()) == 0) return true;
      filedetail::sleepMs(50);
    }
    return !fileExists(temp_);
  }

private:
  TemporaryFile() = default;

  static std::string uniqueName(const std::string& folder, const std::string& stem, const std::string& ext) {
    for (int attempt = 0; attempt < 100; ++attempt) {
      const std::string candidate = joinPath(folder, stem + filedetail::randomHex(8) + ext);
      if (!fileExists(candidate)) return candidate;
    }
    return joinPath(folder, stem + filedetail::randomHex(16) + ext);
  }

  std::string temp_;
  std::string target_;
};

/**
 * A file, or a range of one, mapped into memory.
 *
 * getData() is null when the file could not be opened or mapped, or when
 * there is nothing to map: an empty file, or a range of no bytes -- never
 * non-null with a size of 0. A range is clipped to the file -- a mapping
 * never grows a file -- and may start anywhere: the system maps from the
 * allocation boundary below it and getData() points at the byte asked for.
 *
 * `exclusive` keeps other programs from opening the file while it is mapped
 * (a share mode on Windows, an advisory flock elsewhere -- advisory meaning
 * only programs that also ask are kept out).
 */
class MemoryMappedFile {
public:
  /** What the mapping allows. */
  enum AccessMode {
    readOnly, ///< reads only; writing through getData() faults
    readWrite ///< writes through getData() change the file
  };

  /** Maps the whole of the UTF-8 `path`; isMapped() says whether it worked. */
  MemoryMappedFile(const std::string& path, AccessMode mode = readOnly, bool exclusive = false) {
    open(path, 0, -1, mode, exclusive);
  }
  /** Maps `length` bytes from byte `start` (negative: to the end), clipped
   *  to the file. A negative start is 0; a start at or past the end, or a
   *  length of 0, maps nothing. */
  MemoryMappedFile(const std::string& path, int64_t start, int64_t length, AccessMode mode = readOnly,
                   bool exclusive = false) {
    open(path, start, length, mode, exclusive);
  }
  /** Unmaps and closes the file. Pointers into getData() dangle after. */
  ~MemoryMappedFile() { close(); }

  ///@{
  /** Not copyable: the mapping has one owner. */
  MemoryMappedFile(const MemoryMappedFile&) = delete;
  MemoryMappedFile& operator=(const MemoryMappedFile&) = delete;
  ///@}
  /** Takes over `other`'s mapping, which stays where it is in memory, so
   *  pointers into it stay valid; `other` maps nothing after. */
  MemoryMappedFile(MemoryMappedFile&& other) noexcept { take(other); }
  /** Unmaps this one's mapping, then takes over `other`'s. */
  MemoryMappedFile& operator=(MemoryMappedFile&& other) noexcept {
    if (this != &other) {
      close();
      take(other);
    }
    return *this;
  }

  /** The first byte of the range asked for, or null. */
  void* getData() const noexcept { return data_; }
  /** How many bytes from getData() are mapped. */
  size_t getSize() const noexcept { return size_; }
  /** Where getData() is in the file. */
  int64_t getStart() const noexcept { return start_; }
  /** True when getData() is not null. */
  bool isMapped() const noexcept { return data_ != nullptr; }

private:
  void open(const std::string& path, int64_t start, int64_t length, AccessMode mode, bool exclusive) {
    if (start < 0) start = 0;
#if defined(_WIN32)
    const DWORD access = mode == readWrite ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    const DWORD share = exclusive ? 0 : (FILE_SHARE_READ | FILE_SHARE_DELETE | (mode == readWrite ? FILE_SHARE_WRITE : 0));
    HANDLE file = CreateFileW(pathio::widenPath(path.c_str()).c_str(), access, share, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= start) {
      CloseHandle(file);
      return;
    }
    // The length against what is left, never start + length: a length near
    // int64's top wrapped that sum negative, and the range mapped nothing.
    int64_t end = size.QuadPart;
    if (length >= 0 && length < end - start) end = start + length;
    if (end == start) { // no bytes: nothing to map, wherever they start
      CloseHandle(file);
      return;
    }
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const int64_t granularity = info.dwAllocationGranularity ? (int64_t) info.dwAllocationGranularity : 65536;
    const int64_t mapStart = start - start % granularity;
    const int64_t mapLength = end - mapStart;
    if (mapLength <= 0 || (uint64_t) mapLength > (uint64_t) (size_t) -1) {
      CloseHandle(file);
      return;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, mode == readWrite ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file); // the mapping keeps the file open
    if (!mapping) return;
    void* view = MapViewOfFile(mapping, mode == readWrite ? FILE_MAP_WRITE : FILE_MAP_READ,
                               (DWORD) ((uint64_t) mapStart >> 32), (DWORD) ((uint64_t) mapStart & 0xffffffffu),
                               (SIZE_T) mapLength);
    CloseHandle(mapping); // and the view keeps the mapping
    if (!view) return;
    view_ = view;
    viewSize_ = (size_t) mapLength;
#else
    const int fd = ::open(path.c_str(), (mode == readWrite ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0) return;
    if (exclusive && flock(fd, LOCK_EX | LOCK_NB) != 0) {
      ::close(fd);
      return;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (int64_t) st.st_size <= start) {
      ::close(fd);
      return;
    }
    int64_t end = (int64_t) st.st_size;
    if (length >= 0 && length < end - start) end = start + length; // as on Windows
    if (end == start) {
      ::close(fd);
      return;
    }
    const long page = sysconf(_SC_PAGESIZE);
    const int64_t granularity = page > 0 ? (int64_t) page : 4096;
    const int64_t mapStart = start - start % granularity;
    const int64_t mapLength = end - mapStart;
    void* view = MAP_FAILED;
    if (mapLength > 0 && (uint64_t) mapLength <= (uint64_t) (size_t) -1)
      view = mmap(nullptr, (size_t) mapLength, mode == readWrite ? (PROT_READ | PROT_WRITE) : PROT_READ, MAP_SHARED,
                  fd, (off_t) mapStart);
    if (view == MAP_FAILED) {
      ::close(fd);
      return;
    }
    // The descriptor is kept only to hold an exclusive lock: the mapping
    // itself keeps the file alive.
    if (exclusive) fd_ = fd;
    else ::close(fd);
    view_ = view;
    viewSize_ = (size_t) mapLength;
#endif
    data_ = static_cast<char*>(view_) + (start - mapStart);
    size_ = (size_t) (end - start);
    start_ = start;
  }

  void close() noexcept {
#if defined(_WIN32)
    if (view_) UnmapViewOfFile(view_);
#else
    if (view_) munmap(view_, viewSize_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
#endif
    view_ = nullptr;
    viewSize_ = 0;
    data_ = nullptr;
    size_ = 0;
    start_ = 0;
  }

  void take(MemoryMappedFile& other) noexcept {
    view_ = other.view_;
    viewSize_ = other.viewSize_;
    data_ = other.data_;
    size_ = other.size_;
    start_ = other.start_;
#if !defined(_WIN32)
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
    other.view_ = nullptr;
    other.viewSize_ = 0;
    other.data_ = nullptr;
    other.size_ = 0;
    other.start_ = 0;
  }

  void* view_ = nullptr;
  size_t viewSize_ = 0;
  void* data_ = nullptr;
  size_t size_ = 0;
  int64_t start_ = 0;
#if !defined(_WIN32)
  int fd_ = -1;
#endif
};

/**
 * "*.wav;*.aif?" against a file's NAME (not its folder).
 *
 * Patterns are separated by ';' or ',', and spaces around them are ignored,
 * so "*.wav; *.aiff" and the dialog filters' "*.wav;*.aiff" read alike. '*'
 * is any run of characters, '?' any one character -- one UTF-8 code point,
 * not one byte, so "?.wav" matches "é.wav". Case is ignored (ASCII), on every
 * platform: see the header.
 *
 * An empty pattern list matches everything, as the reference's does.
 */
class WildcardFileFilter {
public:
  /** Patterns for files and for directories, in the syntax above (quotes
   *  around a pattern are dropped), and a description for display. An empty
   *  pattern string accepts every name of that kind. */
  WildcardFileFilter(const std::string& fileWildcardPatterns, const std::string& directoryWildcardPatterns,
                     const std::string& filterDescription)
      : files_(split(fileWildcardPatterns)), directories_(split(directoryWildcardPatterns)),
        description_(filterDescription) {}

  /** From one of the file dialog's filters (file_dialog.h's FileFilter, or
   *  anything with `label` and `patterns`): the same patterns, one syntax.
   *  Directories are all accepted, so a browser can still walk into them. */
  template <typename DialogFilter>
  static WildcardFileFilter fromDialogFilter(const DialogFilter& filter) {
    return WildcardFileFilter(filter.patterns, "*", filter.label);
  }

  /** The description given to the constructor, for a dialog or a menu. */
  const std::string& getDescription() const noexcept { return description_; }

  /** Whether the last component of `path` matches one of the file
   *  patterns. Nothing on disk is looked at: the caller knows it is a file. */
  bool isFileSuitable(const std::string& path) const { return matchesAny(fileName(path), files_); }
  /** The same against the directory patterns. */
  bool isDirectorySuitable(const std::string& path) const { return matchesAny(fileName(path), directories_); }

  /** One name against one pattern. Linear in their lengths times the number of
   *  '*'s backtracked over -- never exponential, whatever the pattern: a
   *  filter typed into a search box is untrusted input. */
  static bool matchesWildcard(const std::string& name, const std::string& pattern, bool ignoreCase = true) {
    size_t n = 0, p = 0;
    size_t starP = std::string::npos, starN = 0;
    while (n < name.size()) {
      if (p < pattern.size() && pattern[p] == '*') {
        starP = p++;
        starN = n;
        continue;
      }
      if (p < pattern.size() && pattern[p] == '?') {
        n = nextCodePoint(name, n);
        ++p;
        continue;
      }
      if (p < pattern.size() && same(pattern[p], name[n], ignoreCase)) {
        ++p;
        ++n;
        continue;
      }
      if (starP == std::string::npos) return false;
      // Let the last '*' swallow one more character and try again from there.
      p = starP + 1;
      starN = nextCodePoint(name, starN);
      n = starN;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
  }

private:
  static size_t nextCodePoint(const std::string& s, size_t i) {
    ++i;
    while (i < s.size() && ((uint8_t) s[i] & 0xC0) == 0x80) ++i;
    return i;
  }
  static bool same(char a, char b, bool ignoreCase) {
    return ignoreCase ? asciiLowerChar(a) == asciiLowerChar(b) : a == b;
  }
  static std::vector<std::string> split(const std::string& patterns) {
    std::vector<std::string> out;
    std::string current;
    auto flush = [&] {
      size_t a = 0, b = current.size();
      while (a < b && (current[a] == ' ' || current[a] == '\t' || current[a] == '"')) ++a;
      while (b > a && (current[b - 1] == ' ' || current[b - 1] == '\t' || current[b - 1] == '"')) --b;
      if (b > a) out.push_back(current.substr(a, b - a));
      current.clear();
    };
    for (char c : patterns) {
      if (c == ';' || c == ',') flush();
      else current.push_back(c);
    }
    flush();
    return out;
  }
  static bool matchesAny(const std::string& name, const std::vector<std::string>& patterns) {
    if (patterns.empty()) return true;
    for (const std::string& p : patterns)
      if (matchesWildcard(name, p)) return true;
    return false;
  }

  std::vector<std::string> files_;
  std::vector<std::string> directories_;
  std::string description_;
};

/**
 * An ordered list of folders: where a plugin looks for its impulse responses,
 * a sampler for its libraries. Written and read as one string, the folders
 * separated by ';' -- on every platform, as the reference does, so a saved
 * list moves between machines.
 */
class FileSearchPath {
public:
  /** An empty list. */
  FileSearchPath() = default;
  /** From a ';'-separated string as toString() writes it; a quoted folder
   *  may contain ';'. Blanks around each folder and trailing separators are
   *  dropped, and empty entries are skipped. */
  explicit FileSearchPath(const std::string& pathList) { addPathList(pathList); }

  /** How many folders the list holds. */
  int getNumPaths() const noexcept { return (int) paths_.size(); }
  /** Folder `index` (0-based), or "" when out of range. */
  std::string operator[](int index) const {
    return index >= 0 && index < (int) paths_.size() ? paths_[(size_t) index] : std::string();
  }
  /** Every folder, in search order. Valid until the list is changed. */
  const std::vector<std::string>& getPaths() const noexcept { return paths_; }

  /** The list as one ';'-separated string. A folder containing ';' is quoted. */
  std::string toString() const {
    std::string out;
    for (size_t i = 0; i < paths_.size(); ++i) {
      if (i) out += ';';
      if (paths_[i].find(';') != std::string::npos) out += "\"" + paths_[i] + "\"";
      else out += paths_[i];
    }
    return out;
  }

  /** Adds a folder at insertIndex (the end when out of range). Empty is ignored. */
  void add(const std::string& directory, int insertIndex = -1) {
    const std::string d = tidy(directory);
    if (d.empty()) return;
    if (insertIndex < 0 || insertIndex > (int) paths_.size()) paths_.push_back(d);
    else paths_.insert(paths_.begin() + insertIndex, d);
  }
  /** Adds it unless an equal path is there already; true if it was added. */
  bool addIfNotAlreadyThere(const std::string& directory) {
    const std::string d = tidy(directory);
    if (d.empty()) return false;
    for (const std::string& p : paths_)
      if (samePath(p, d)) return false;
    paths_.push_back(d);
    return true;
  }
  /** Removes folder `index` (0-based); out of range does nothing. */
  void remove(int index) {
    if (index >= 0 && index < (int) paths_.size()) paths_.erase(paths_.begin() + index);
  }
  /** Appends each of `other`'s folders that is not already in this list. */
  void addPath(const FileSearchPath& other) {
    for (const std::string& p : other.paths_) addIfNotAlreadyThere(p);
  }
  /** Removes every folder. */
  void clear() noexcept { paths_.clear(); }

  /** Drops duplicates, and folders inside another folder of the list -- a
   *  recursive search would find their contents twice. */
  void removeRedundantPaths() {
    std::vector<std::string> kept;
    for (size_t i = 0; i < paths_.size(); ++i) {
      bool redundant = false;
      for (size_t j = 0; j < paths_.size() && !redundant; ++j) {
        if (i == j) continue;
        if (samePath(paths_[i], paths_[j])) redundant = j < i; // keep the first of equals
        else if (isInside(paths_[i], paths_[j])) redundant = true;
      }
      if (!redundant) kept.push_back(paths_[i]);
    }
    paths_ = std::move(kept);
  }
  /** Drops folders that do not exist (a library on an unplugged drive). */
  void removeNonexistentPaths() {
    std::vector<std::string> kept;
    for (const std::string& p : paths_)
      if (isDirectory(p)) kept.push_back(p);
    paths_ = std::move(kept);
  }

  /** Every file under the list's folders whose name matches, in list order
   *  and then files.h's sorted order. Recursion stops at 32 levels -- a
   *  symlink that points at its own parent is a loop, not a deep library. */
  std::vector<std::string> findChildFiles(bool searchRecursively, const std::string& wildcardPattern = "*",
                                          bool includeDirectories = false) const {
    std::vector<std::string> out;
    const WildcardFileFilter filter(wildcardPattern, wildcardPattern, std::string());
    for (const std::string& p : paths_) collect(p, searchRecursively, filter, includeDirectories, 0, out);
    return out;
  }

  /** Whether `file` is directly in one of the folders, or anywhere under one. */
  bool isFileInPath(const std::string& file, bool checkRecursively) const {
    const std::string parent = tidy(parentPath(file));
    for (const std::string& p : paths_) {
      if (samePath(parent, p)) return true;
      if (checkRecursively && isInside(file, p)) return true;
    }
    return false;
  }

private:
  void addPathList(const std::string& list) {
    std::string current;
    bool quoted = false;
    for (char c : list) {
      if (c == '"') quoted = !quoted;
      else if (c == ';' && !quoted) {
        add(current);
        current.clear();
      } else current.push_back(c);
    }
    add(current);
  }

  static std::string tidy(const std::string& path) {
    size_t a = 0, b = path.size();
    while (a < b && (path[a] == ' ' || path[a] == '\t')) ++a;
    while (b > a && (path[b - 1] == ' ' || path[b - 1] == '\t')) --b;
    std::string out = path.substr(a, b - a);
    // A trailing separator is dropped, except the root's own.
    while (out.size() > 1 && isSeparator(out.back()) && !(out.size() == 3 && out[1] == ':')) out.pop_back();
    return out;
  }

  /** Paths compared with either separator; case ignored on Windows, whose
   *  file system ignores it. */
  static bool sameChar(char a, char b) {
    if (isSeparator(a) && isSeparator(b)) return true;
#if defined(_WIN32)
    return asciiLowerChar(a) == asciiLowerChar(b);
#else
    return a == b;
#endif
  }
  static bool samePath(const std::string& a, const std::string& b) {
    const std::string x = tidy(a), y = tidy(b);
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i)
      if (!sameChar(x[i], y[i])) return false;
    return true;
  }
  /** Whether `path` is strictly under `folder`. */
  static bool isInside(const std::string& path, const std::string& folder) {
    const std::string p = tidy(path), f = tidy(folder);
    if (f.empty() || p.size() <= f.size()) return false;
    for (size_t i = 0; i < f.size(); ++i)
      if (!sameChar(p[i], f[i])) return false;
    return isSeparator(f.back()) || isSeparator(p[f.size()]);
  }

  static void collect(const std::string& folder, bool recursive, const WildcardFileFilter& filter,
                      bool includeDirectories, int depth, std::vector<std::string>& out) {
    if (depth > 32) return;
    for (const FileEntry& e : listDirectory(folder, {}, true, false)) {
      if (e.isDirectory) {
        if (includeDirectories && filter.isDirectorySuitable(e.path)) out.push_back(e.path);
        if (recursive) collect(e.path, true, filter, includeDirectories, depth + 1, out);
      } else if (filter.isFileSuitable(e.path)) {
        out.push_back(e.path);
      }
    }
  }

  std::vector<std::string> paths_;
};

/**
 * The volumes mounted now, sorted: drive roots on Windows ("C:\\", "D:\\"),
 * "/" and the entries of /Volumes on macOS, and on Linux the mount points in
 * /proc/self/mounts that hold files -- the kernel's own pseudo file systems
 * (proc, sysfs, cgroup, devpts...) and snap packages' squashfs images are left
 * out, since a snap refreshing itself is not a drive arriving.
 */
inline std::vector<std::string> mountedVolumes() {
  std::vector<std::string> out;
#if defined(_WIN32)
  out = rootPaths();
#elif defined(__APPLE__)
  out.push_back("/");
  for (const FileEntry& e : listDirectory("/Volumes", {}, true, false))
    if (e.isDirectory) out.push_back(e.path);
#else
  std::FILE* f = std::fopen("/proc/self/mounts", "r");
  if (f) {
    static const char* const pseudo[] = {"proc", "sysfs", "devtmpfs", "devpts", "cgroup", "cgroup2", "securityfs",
                                         "pstore", "debugfs", "tracefs", "configfs", "fusectl", "mqueue",
                                         "hugetlbfs", "bpf", "autofs", "binfmt_misc", "rpc_pipefs", "nsfs",
                                         "squashfs", "efivarfs", "ramfs", "selinuxfs"};
    char line[4096];
    while (std::fgets(line, sizeof(line), f)) {
      // "device mountpoint type options dump pass", fields separated by
      // spaces; a space inside a field is written as \040.
      std::string fields[3];
      int field = 0;
      for (const char* c = line; *c && *c != '\n' && field < 4; ++c) {
        if (*c == ' ') {
          ++field;
          continue;
        }
        if (field > 2) break;
        if (*c == '\\' && c[1] >= '0' && c[1] <= '7' && c[2] >= '0' && c[2] <= '7' && c[3] >= '0' && c[3] <= '7') {
          fields[field].push_back((char) (((c[1] - '0') << 6) | ((c[2] - '0') << 3) | (c[3] - '0')));
          c += 3;
        } else {
          fields[field].push_back(*c);
        }
      }
      if (fields[1].empty()) continue;
      bool skip = false;
      for (const char* p : pseudo)
        if (fields[2] == p) skip = true;
      if (!skip) out.push_back(fields[1]);
    }
    std::fclose(f);
  }
  if (out.empty()) out.push_back("/");
#endif
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

/**
 * Tells you when a drive is plugged in or removed -- a sample library on an
 * external disk appearing, a browser's list of locations going stale.
 *
 * It POLLS, on its own thread, every pollIntervalMs (a second by default):
 * the list of volumes is compared with the last one. The alternatives are
 * worse for a plugin -- WM_DEVICECHANGE needs a top-level window and the
 * message loop that owns it, which a plugin has neither of, and DiskArbitration
 * and udev are each a library to load for one bit of information. Reading the
 * drive mask costs microseconds.
 *
 * onChange (or an override of mountedVolumeListChanged) runs on the
 * detector's thread, not a UI thread: hand the news over (a UiEventQueue, an
 * atomic flag the editor's tick reads). A subclass that overrides the virtual
 * must call stop() in its own destructor, before its members go -- the thread
 * may be calling it. The callback may delete the detector: the thread then
 * ends without touching it again.
 */
class MountedVolumeListChangeDetector {
public:
  /** Called when the list changes, on the detector's thread. */
  std::function<void()> onChange;
  /** What is compared: mountedVolumes() unless replaced -- with a narrower
   *  list (only removable drives), or in a test, with a fake. Set before
   *  start(). */
  std::function<std::vector<std::string>()> listVolumes;

  /** Not watching yet: call start(). */
  MountedVolumeListChangeDetector() = default;
  /** Stops the thread and waits for it -- or, destroyed from its own
   *  callback, lets it end after the callback. A subclass stops it in its
   *  own destructor (see the class comment). */
  virtual ~MountedVolumeListChangeDetector() { stop(); }
  ///@{
  /** Not copyable: the thread holds a pointer to this object. */
  MountedVolumeListChangeDetector(const MountedVolumeListChangeDetector&) = delete;
  MountedVolumeListChangeDetector& operator=(const MountedVolumeListChangeDetector&) = delete;
  ///@}

  /** Starts watching. The list at this moment is the baseline. */
  void start(int pollIntervalMs = 1000) {
    stop();
    interval_ = pollIntervalMs < 10 ? 10 : pollIntervalMs;
    last_ = current();
    signal_ = std::make_shared<StopSignal>();
    // The thread holds its own reference to the signal: it is what the
    // thread reads after a callback, when the detector may be gone.
    thread_ = std::thread([this, signal = signal_, interval = interval_] { run(*signal, interval); });
  }

  /** Stops watching and waits for the thread to end: at most the poll and
   *  callback in progress. Called from onChange, it returns at once and the
   *  thread ends after the callback. Safe to call when not running. */
  void stop() {
    if (signal_) {
      {
        std::lock_guard<std::mutex> lock(signal_->mutex);
        signal_->stopping = true;
      }
      signal_->wake.notify_all();
      signal_.reset();
    }
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
    else if (thread_.joinable()) thread_.detach();
  }

  /** True between start() and stop(). Call it from the thread that calls
   *  start() and stop(), not from the callback. */
  bool isRunning() const { return thread_.joinable(); }

  /** The list as of the last poll. */
  std::vector<std::string> getLastVolumeList() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_;
  }

  /** Called on the detector's thread when the list changes. Calls onChange. */
  virtual void mountedVolumeListChanged() {
    if (onChange) onChange();
  }

private:
  /** How stop() tells one run of the thread to end. Shared with that thread
   *  rather than a member: a detector deleted from its own callback is
   *  stopped by its destructor, and the thread, back from the callback,
   *  finds that out here instead of in the freed object -- whose mutex it
   *  used to lock. A start() after stop() makes a new one, so a thread
   *  stopped from its callback stays stopped when the callback starts the
   *  detector again. */
  struct StopSignal {
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
  };

  std::vector<std::string> current() const { return listVolumes ? listVolumes() : mountedVolumes(); }

  /** The detector is read only after `signal` says it has not been stopped:
   *  stopped from another thread, its destructor waits for this one, and
   *  from the callback, this is the first thing the thread looks at. */
  void run(StopSignal& signal, int interval) {
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(signal.mutex);
        signal.wake.wait_for(lock, std::chrono::milliseconds(interval), [&signal] { return signal.stopping; });
        if (signal.stopping) return;
      }
      std::vector<std::string> now = current();
      bool changed;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        changed = now != last_;
        if (changed) last_ = std::move(now);
      }
      if (changed) mountedVolumeListChanged();
    }
  }

  mutable std::mutex mutex_; // last_
  std::thread thread_;
  std::shared_ptr<StopSignal> signal_; // the running thread's; null when stopped
  std::vector<std::string> last_;
  int interval_ = 1000;
};

} // namespace sonore
