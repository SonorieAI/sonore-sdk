// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: byte streams: one interface for a file, a buffer, a URL, or a
// window onto another stream.
//
// ── Why a hierarchy, when everything else here is plain functions ───────────
//
// The rest of this SDK reads files whole: a preset is a few kilobytes, a WAV
// is parsed by audiofile.h straight off the disk, and a vector of bytes is the
// simplest thing that could work. That stops being true the moment the same
// code has to read from more than one KIND of source -- a sample pack that is
// sometimes a file, sometimes a slice of a zip, sometimes an HTTP download --
// and that is the one argument the reference's stream classes make which
// survives contact with a plugin. So: one small abstract InputStream, and the
// sources are FileInputStream (file_types.h), WebInputStream (net.h),
// MemoryInputStream, and two adaptors over any of them, BufferedInputStream
// and SubregionStream. Code that takes an InputStream& reads all of them.
//
// ── The decisions a reader of a stream cares about ──────────────────────────
//
// read() returns the bytes it produced, never a negative number: zero is the
// end, or a failure the stream can describe -- a caller that loops "while
// read() > 0" cannot spin on an error. Multi-byte readers name their byte
// order (readInt is little-endian, readIntBigEndian is not) and assemble
// values from bytes with shifts, never by casting a pointer -- an unaligned
// load is a crash on some ARM cores and undefined behaviour everywhere. A
// reader that runs out of input returns zero for the value; whoever needs to
// know checks isExhausted().
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// A stream is one reader's object. The memory streams never block; the file
// and network ones do I/O and may block for as long as a disk or a server
// takes, so NONE of them is for the audio thread. Read on a loader thread and
// hand the result over (audio_ring.h, the sampler's own streaming).
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace sonore {

/** Something bytes can be read from, in order, with a position. */
class InputStream {
public:
  /** Virtual, because streams are owned and deleted through an InputStream
   *  pointer. A file or network stream closes what it read from. */
  virtual ~InputStream() = default;

  /** The whole length, or -1 when the source does not know (a chunked HTTP
   *  response, a pipe). */
  virtual int64_t getTotalLength() = 0;
  /** True once a read has reached the end, or the stream has failed. */
  virtual bool isExhausted() = 0;
  /** Up to maxBytesToRead bytes into destBuffer: how many arrived, 0 at the
   *  end or on failure. Never negative. */
  virtual int read(void* destBuffer, int maxBytesToRead) = 0;
  /** The read position, in bytes from the start of the stream. */
  virtual int64_t getPosition() = 0;
  /** Moves the read position. A stream that cannot go backwards (a network
   *  response) answers false for any position behind it. */
  virtual bool setPosition(int64_t newPosition) = 0;

  /** What is left, or -1 when the total is unknown. */
  int64_t getNumBytesRemaining() {
    const int64_t total = getTotalLength();
    if (total < 0) return -1;
    const int64_t left = total - getPosition();
    return left > 0 ? left : 0;
  }

  /** Moves forward by reading and discarding: correct for every stream,
   *  including one that cannot seek. Overridden where seeking is cheaper. */
  virtual void skipNextBytes(int64_t numBytesToSkip) {
    char scratch[4096];
    while (numBytesToSkip > 0) {
      const int chunk = (int) (numBytesToSkip < (int64_t) sizeof(scratch) ? numBytesToSkip
                                                                          : (int64_t) sizeof(scratch));
      const int got = read(scratch, chunk);
      if (got <= 0) break;
      numBytesToSkip -= got;
    }
  }

  // ── Typed readers. Zero for whatever the stream could not supply. ──

  ///@{
  /** One fixed-size value: 1 byte for readByte and readBool (any non-zero
   *  byte is true), 2, 4 or 8 for the rest, little-endian unless the name
   *  says BigEndian; floats are IEEE 754 bit patterns. When the stream ends
   *  part-way through a value it gives 0 (false, 0.0), and the bytes it did
   *  read are consumed. */
  char readByte() {
    char c = 0;
    read(&c, 1);
    return c;
  }
  bool readBool() { return readByte() != 0; }
  int16_t readShort() { return (int16_t) readUnsigned(2, false); }
  int16_t readShortBigEndian() { return (int16_t) readUnsigned(2, true); }
  int32_t readInt() { return (int32_t) readUnsigned(4, false); }
  int32_t readIntBigEndian() { return (int32_t) readUnsigned(4, true); }
  int64_t readInt64() { return (int64_t) readUnsigned(8, false); }
  int64_t readInt64BigEndian() { return (int64_t) readUnsigned(8, true); }
  float readFloat() { return bitsToFloat((uint32_t) readUnsigned(4, false)); }
  float readFloatBigEndian() { return bitsToFloat((uint32_t) readUnsigned(4, true)); }
  double readDouble() { return bitsToDouble(readUnsigned(8, false)); }
  double readDoubleBigEndian() { return bitsToDouble(readUnsigned(8, true)); }
  ///@}

  /** A NUL-terminated string, as OutputStream::writeString writes one. */
  std::string readString() {
    std::string out;
    for (;;) {
      char c = 0;
      if (read(&c, 1) != 1 || c == 0) break;
      out.push_back(c);
    }
    return out;
  }

  /** Up to the next "\n" or "\r\n" (not included), or the end. A lone "\r"
   *  ends a line too -- files written on classic Mac OS still turn up in
   *  sample libraries. */
  std::string readNextLine() {
    std::string out;
    for (;;) {
      char c = 0;
      if (read(&c, 1) != 1 || c == '\n') break;
      if (c == '\r') {
        // A following "\n" belongs to this line ending; anything else is the
        // next line's first byte, so step back over it where we can.
        const int64_t here = getPosition();
        char next = 0;
        if (read(&next, 1) == 1 && next != '\n') setPosition(here);
        break;
      }
      out.push_back(c);
    }
    return out;
  }

  /** Appends up to maxNumBytesToRead (all of it, when negative) to dest;
   *  returns how many were appended. */
  size_t readIntoVector(std::vector<uint8_t>& dest, int64_t maxNumBytesToRead = -1) {
    size_t total = 0;
    uint8_t buffer[16384];
    while (maxNumBytesToRead < 0 || (int64_t) total < maxNumBytesToRead) {
      int64_t want = (int64_t) sizeof(buffer);
      if (maxNumBytesToRead >= 0 && maxNumBytesToRead - (int64_t) total < want)
        want = maxNumBytesToRead - (int64_t) total;
      const int got = read(buffer, (int) want);
      if (got <= 0) break;
      dest.insert(dest.end(), buffer, buffer + got);
      total += (size_t) got;
    }
    return total;
  }

  /** Everything from here to the end, as bytes in a string. No decoding: a
   *  caller that wants text and knows the encoding converts it. */
  std::string readEntireStreamAsString() {
    std::vector<uint8_t> bytes;
    readIntoVector(bytes);
    return std::string(bytes.begin(), bytes.end());
  }

private:
  uint64_t readUnsigned(int count, bool bigEndian) {
    uint8_t b[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int have = 0;
    while (have < count) {
      const int got = read(b + have, count - have);
      if (got <= 0) return 0; // a value cut short is no value
      have += got;
    }
    uint64_t v = 0;
    for (int i = 0; i < count; ++i) {
      const int shift = bigEndian ? 8 * (count - 1 - i) : 8 * i;
      v |= (uint64_t) b[i] << shift;
    }
    return v;
  }
  static float bitsToFloat(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
  }
  static double bitsToDouble(uint64_t bits) {
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
  }
};

/** Something bytes can be written to, with a position. */
class OutputStream {
public:
  /** Virtual, because streams are owned and deleted through an OutputStream
   *  pointer. A file stream flushes and closes its file. */
  virtual ~OutputStream() = default;

  /** Pushes anything buffered towards its destination. */
  virtual void flush() = 0;
  /** Moves the write position, in bytes from the start. False where the
   *  stream cannot go there; each stream says which positions it accepts. */
  virtual bool setPosition(int64_t newPosition) = 0;
  /** The write position, in bytes from the start. */
  virtual int64_t getPosition() = 0;
  /** All of numBytes, or false. A short write is a failure, not a count:
   *  callers of a stream that wrote half a header cannot do anything useful
   *  with the half. */
  virtual bool write(const void* dataToWrite, size_t numberOfBytes) = 0;

  ///@{
  /** One fixed-size value, laid out as the InputStream reader of the same
   *  name reads it: 1 byte for writeByte and writeBool (true is 1), 2, 4 or
   *  8 for the rest, little-endian unless the name says BigEndian, floats as
   *  IEEE 754 bit patterns. False when the stream did not take every
   *  byte. */
  bool writeByte(char byte) { return write(&byte, 1); }
  bool writeBool(bool b) { return writeByte(b ? (char) 1 : (char) 0); }
  bool writeShort(int16_t v) { return writeUnsigned((uint16_t) v, 2, false); }
  bool writeShortBigEndian(int16_t v) { return writeUnsigned((uint16_t) v, 2, true); }
  bool writeInt(int32_t v) { return writeUnsigned((uint32_t) v, 4, false); }
  bool writeIntBigEndian(int32_t v) { return writeUnsigned((uint32_t) v, 4, true); }
  bool writeInt64(int64_t v) { return writeUnsigned((uint64_t) v, 8, false); }
  bool writeInt64BigEndian(int64_t v) { return writeUnsigned((uint64_t) v, 8, true); }
  bool writeFloat(float v) { return writeUnsigned(floatBits(v), 4, false); }
  bool writeFloatBigEndian(float v) { return writeUnsigned(floatBits(v), 4, true); }
  bool writeDouble(double v) { return writeUnsigned(doubleBits(v), 8, false); }
  bool writeDoubleBigEndian(double v) { return writeUnsigned(doubleBits(v), 8, true); }
  ///@}

  /** The bytes and a terminating NUL: what InputStream::readString reads. */
  bool writeString(const std::string& text) { return write(text.c_str(), text.size() + 1); }
  /** The bytes alone. */
  bool writeText(const std::string& text) { return write(text.data(), text.size()); }

  /** `count` copies of `byte`, for padding or filling a reserved area.
   *  False at the first write that fails; what went before it stays
   *  written. */
  bool writeRepeatedByte(uint8_t byte, size_t count) {
    uint8_t chunk[256];
    std::memset(chunk, byte, sizeof(chunk));
    while (count > 0) {
      const size_t n = count < sizeof(chunk) ? count : sizeof(chunk);
      if (!write(chunk, n)) return false;
      count -= n;
    }
    return true;
  }

  /** Copies from source until its end, or maxNumBytesToWrite when that is not
   *  negative. Returns how many were written. */
  int64_t writeFromInputStream(InputStream& source, int64_t maxNumBytesToWrite = -1) {
    int64_t total = 0;
    char buffer[16384];
    while (maxNumBytesToWrite < 0 || total < maxNumBytesToWrite) {
      int64_t want = (int64_t) sizeof(buffer);
      if (maxNumBytesToWrite >= 0 && maxNumBytesToWrite - total < want) want = maxNumBytesToWrite - total;
      const int got = source.read(buffer, (int) want);
      if (got <= 0) break;
      if (!write(buffer, (size_t) got)) break;
      total += got;
    }
    return total;
  }

  /** Writes the text's bytes with no terminator, as writeText does. A
   *  failed write is not reported: call writeText where that matters. */
  OutputStream& operator<<(const std::string& text) {
    writeText(text);
    return *this;
  }
  /** The same for a C string; a null pointer writes nothing. */
  OutputStream& operator<<(const char* text) {
    if (text) write(text, std::strlen(text));
    return *this;
  }

private:
  bool writeUnsigned(uint64_t v, int count, bool bigEndian) {
    uint8_t b[8];
    for (int i = 0; i < count; ++i) {
      const int shift = bigEndian ? 8 * (count - 1 - i) : 8 * i;
      b[i] = (uint8_t) ((v >> shift) & 0xff);
    }
    return write(b, (size_t) count);
  }
  static uint32_t floatBits(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
  }
  static uint64_t doubleBits(double d) {
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    return bits;
  }
};

/** Reads a block of memory: either a copy it owns, or someone else's bytes
 *  that must outlive it. */
class MemoryInputStream : public InputStream {
public:
  /** Over sourceData, which must stay valid -- unless keepInternalCopy, in
   *  which case the bytes are copied now and the source may go away. */
  MemoryInputStream(const void* sourceData, size_t sourceDataSize, bool keepInternalCopy) {
    if (keepInternalCopy) {
      const uint8_t* p = static_cast<const uint8_t*>(sourceData);
      if (p && sourceDataSize) copy_.assign(p, p + sourceDataSize);
      data_ = copy_.data();
    } else {
      data_ = static_cast<const uint8_t*>(sourceData);
    }
    size_ = data_ ? sourceDataSize : 0;
  }
  /** Over its own copy of bytes. */
  explicit MemoryInputStream(std::vector<uint8_t> bytes) : copy_(std::move(bytes)) {
    data_ = copy_.data();
    size_ = copy_.size();
  }

  /** The bytes being read: the stream's own copy, or the caller's block. */
  const void* getData() const noexcept { return data_; }
  /** Their size in bytes; 0 when the stream was made over a null pointer. */
  size_t getDataSize() const noexcept { return size_; }

  int64_t getTotalLength() override { return (int64_t) size_; }
  bool isExhausted() override { return position_ >= size_; }
  /** From 0 to getDataSize(). setPosition() goes anywhere, back or
   *  forward, clamped to that range. */
  int64_t getPosition() override { return (int64_t) position_; }
  bool setPosition(int64_t pos) override {
    // Clamped, as a file seek past the end is: the next read returns 0.
    position_ = pos <= 0 ? 0 : ((uint64_t) pos > size_ ? size_ : (size_t) pos);
    return true;
  }
  int read(void* dest, int maxBytesToRead) override {
    if (!dest || maxBytesToRead <= 0 || position_ >= size_) return 0;
    const size_t left = size_ - position_;
    const size_t n = (size_t) maxBytesToRead < left ? (size_t) maxBytesToRead : left;
    std::memcpy(dest, data_ + position_, n);
    position_ += n;
    return (int) n;
  }
  void skipNextBytes(int64_t numBytesToSkip) override {
    // Against what is LEFT, never added to the position: a count near
    // INT64_MAX -- a chunk size read from a damaged file -- wrapped the sum
    // negative, setPosition took that as 0, and the skip went back to the
    // START. A reader walking chunks then read the first one again, for ever.
    if (numBytesToSkip <= 0) return;
    const size_t left = size_ - position_;
    position_ = (uint64_t) numBytesToSkip >= left ? size_ : position_ + (size_t) numBytesToSkip;
  }

private:
  std::vector<uint8_t> copy_;
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t position_ = 0;
};

/** Writes into a growing block of memory -- its own, or a vector the caller
 *  owns and keeps. Writing past the end grows it; writing at a position
 *  before the end overwrites, which is how a size field written as a
 *  placeholder is filled in afterwards. */
class MemoryOutputStream : public OutputStream {
public:
  /** Writes into a vector of its own, read back with getBytes() or
   *  toString(). */
  MemoryOutputStream() : target_(&own_) {}
  /** Writes into `destination`, from its end unless appendToExisting is
   *  false (then from the start, and it is cleared first). */
  explicit MemoryOutputStream(std::vector<uint8_t>& destination, bool appendToExisting = true)
      : target_(&destination) {
    if (!appendToExisting) destination.clear();
    position_ = destination.size();
  }
  ///@{
  /** Not copyable: a copy would write into the same caller's vector, or
   *  point at the other stream's own one. */
  MemoryOutputStream(const MemoryOutputStream&) = delete;
  MemoryOutputStream& operator=(const MemoryOutputStream&) = delete;
  ///@}

  /** The whole target vector -- with what was in it before, when appending
   *  to a caller's vector. Invalidated by the next write that grows it. */
  const uint8_t* getData() const noexcept { return target_->data(); }
  /** The whole target's size in bytes. After a setPosition() back, this is
   *  more than getPosition(). */
  size_t getDataSize() const noexcept { return target_->size(); }
  /** A copy of the whole target. */
  std::vector<uint8_t> getBytes() const { return *target_; }
  /** The whole target as a string of bytes, not decoded. */
  std::string toString() const { return std::string(target_->begin(), target_->end()); }
  /** Empties the target -- a caller's vector too -- and goes back to 0. */
  void reset() noexcept {
    target_->clear();
    position_ = 0;
  }

  void flush() override {}
  /** Where the next write goes, in bytes from the start of the target. */
  int64_t getPosition() override { return (int64_t) position_; }
  /** Anywhere from 0 to the current size; false beyond it, so no gap is
   *  ever filled in with zeros. */
  bool setPosition(int64_t pos) override {
    if (pos < 0 || (uint64_t) pos > target_->size()) return false;
    position_ = (size_t) pos;
    return true;
  }
  bool write(const void* data, size_t n) override {
    if (n == 0) return true;
    if (!data) return false;
    const size_t end = position_ + n;
    if (end < position_) return false; // a size that wraps is not one
    if (end > target_->size()) target_->resize(end);
    std::memcpy(target_->data() + position_, data, n);
    position_ = end;
    return true;
  }

private:
  std::vector<uint8_t> own_;
  std::vector<uint8_t>* target_;
  size_t position_ = 0;
};

/** Reads another stream through a buffer: many small reads (a parser taking a
 *  byte at a time) become few large ones on the source. Seeks inside the
 *  buffered window cost nothing; anywhere else is passed to the source. */
class BufferedInputStream : public InputStream {
public:
  /** Over `source`, which must outlive this. */
  BufferedInputStream(InputStream& source, int bufferSize) : source_(&source) { init(bufferSize); }
  /** Owning the source. */
  BufferedInputStream(std::unique_ptr<InputStream> source, int bufferSize)
      : owned_(std::move(source)), source_(owned_.get()) {
    init(bufferSize);
  }

  /** The next byte without consuming it, or 0 at the end. */
  char peekByte() {
    if (!ensureBuffered()) return 0;
    return (char) buffer_[(size_t) (position_ - bufferStart_)];
  }

  int64_t getTotalLength() override { return source_ ? source_->getTotalLength() : 0; }
  /** The position this stream's reader has reached, which is behind the
   *  source's own by whatever is buffered. */
  int64_t getPosition() override { return position_; }
  bool isExhausted() override { return !ensureBuffered(); }

  bool setPosition(int64_t pos) override {
    if (pos < 0) pos = 0;
    if (pos >= bufferStart_ && pos <= bufferStart_ + (int64_t) bufferLength_) {
      position_ = pos;
      return true;
    }
    // Outside the window: the source moves, and the window empties.
    if (!source_ || !source_->setPosition(pos)) return false;
    position_ = bufferStart_ = source_->getPosition();
    bufferLength_ = 0;
    return position_ == pos;
  }

  int read(void* dest, int maxBytesToRead) override {
    if (!dest || maxBytesToRead <= 0) return 0;
    uint8_t* out = static_cast<uint8_t*>(dest);
    int done = 0;
    while (done < maxBytesToRead) {
      const int want = maxBytesToRead - done;
      // A read larger than the buffer goes straight to the source once the
      // buffered bytes are used: copying it through the buffer would only add
      // a memcpy.
      const int64_t buffered = bufferStart_ + (int64_t) bufferLength_ - position_;
      if (buffered <= 0 && want >= (int) buffer_.size()) {
        if (!source_ || source_->getPosition() != position_) {
          if (!source_ || !source_->setPosition(position_)) break;
        }
        const int got = source_->read(out + done, want);
        if (got <= 0) break;
        done += got;
        position_ += got;
        bufferStart_ = position_;
        bufferLength_ = 0;
        continue;
      }
      if (!ensureBuffered()) break;
      const size_t offset = (size_t) (position_ - bufferStart_);
      const size_t avail = bufferLength_ - offset;
      const size_t n = (size_t) want < avail ? (size_t) want : avail;
      std::memcpy(out + done, buffer_.data() + offset, n);
      done += (int) n;
      position_ += (int64_t) n;
    }
    return done;
  }

private:
  void init(int bufferSize) {
    // At least 16 bytes, so a peek and a short typed read never refill twice;
    // never more than the source is long, when the source knows.
    int64_t size = bufferSize < 16 ? 16 : bufferSize;
    const int64_t total = source_ ? source_->getTotalLength() : -1;
    if (total >= 0 && total + 1 < size) size = total + 1 < 16 ? 16 : total + 1;
    buffer_.resize((size_t) size);
    position_ = bufferStart_ = source_ ? source_->getPosition() : 0;
  }

  /** Whether at least one byte is buffered at position_. Refills if not. */
  bool ensureBuffered() {
    if (position_ >= bufferStart_ && position_ < bufferStart_ + (int64_t) bufferLength_) return true;
    if (!source_) return false;
    if (source_->getPosition() != position_ && !source_->setPosition(position_)) return false;
    bufferStart_ = position_;
    bufferLength_ = 0;
    const int got = source_->read(buffer_.data(), (int) buffer_.size());
    if (got <= 0) return false;
    bufferLength_ = (size_t) got;
    return true;
  }

  std::unique_ptr<InputStream> owned_;
  InputStream* source_ = nullptr;
  std::vector<uint8_t> buffer_;
  int64_t position_ = 0;
  int64_t bufferStart_ = 0;
  size_t bufferLength_ = 0;
};

/** A window onto part of another stream: position 0 here is
 *  startPositionInSourceStream there, and the end is lengthOfSourceStream
 *  later (or the source's end, when that is negative). How one entry of an
 *  uncompressed archive, or one chunk of a RIFF file, is handed to code that
 *  expects a stream of its own. */
class SubregionStream : public InputStream {
public:
  /** Over `source`, which must outlive this. A negative start is taken as
   *  0; a negative length runs to the source's end. Seeks the source to the
   *  window's start. */
  SubregionStream(InputStream& source, int64_t startPositionInSourceStream, int64_t lengthOfSourceStream)
      : source_(&source), start_(startPositionInSourceStream < 0 ? 0 : startPositionInSourceStream),
        length_(lengthOfSourceStream) {
    setPosition(0);
  }
  /** The same, owning the source. */
  SubregionStream(std::unique_ptr<InputStream> source, int64_t startPositionInSourceStream,
                  int64_t lengthOfSourceStream)
      : owned_(std::move(source)), source_(owned_.get()),
        start_(startPositionInSourceStream < 0 ? 0 : startPositionInSourceStream),
        length_(lengthOfSourceStream) {
    setPosition(0);
  }

  int64_t getTotalLength() override {
    const int64_t sourceTotal = source_ ? source_->getTotalLength() : 0;
    if (sourceTotal < 0) return length_; // -1 when neither knows
    const int64_t available = sourceTotal > start_ ? sourceTotal - start_ : 0;
    return length_ < 0 ? available : (length_ < available ? length_ : available);
  }
  /** The source's position less the window's start: the two share one
   *  position, so reading the source elsewhere moves this too. */
  int64_t getPosition() override { return source_ ? source_->getPosition() - start_ : 0; }
  bool setPosition(int64_t pos) override {
    if (!source_) return false;
    if (pos < 0) pos = 0;
    if (length_ >= 0 && pos > length_) pos = length_;
    return source_->setPosition(start_ + pos);
  }
  bool isExhausted() override {
    if (!source_) return true;
    if (length_ >= 0 && getPosition() >= length_) return true;
    return source_->isExhausted();
  }
  int read(void* dest, int maxBytesToRead) override {
    if (!source_ || maxBytesToRead <= 0) return 0;
    if (length_ >= 0) {
      const int64_t left = length_ - getPosition();
      if (left <= 0) return 0;
      if ((int64_t) maxBytesToRead > left) maxBytesToRead = (int) left;
    }
    return source_->read(dest, maxBytesToRead);
  }

private:
  std::unique_ptr<InputStream> owned_;
  InputStream* source_ = nullptr;
  int64_t start_ = 0;
  int64_t length_ = -1;
};

/** Somewhere a stream can be opened from, again and again: the reference's
 *  way for a reader that needs a fresh stream per use (a sample pool
 *  reopening a file per voice) to be handed a file, a URL or a buffer alike.
 *  FileInputSource is in file_types.h. */
class InputSource {
public:
  /** Virtual: a source is owned and deleted through this base. Streams it
   *  made are independent of it and may outlive it. */
  virtual ~InputSource() = default;
  /** A new stream from the start, or null when the source cannot be opened. */
  virtual std::unique_ptr<InputStream> createInputStream() = 0;
  /** A stream for something beside this source -- a file in the same folder
   *  -- named by a relative path. Null where that means nothing. */
  virtual std::unique_ptr<InputStream> createInputStreamFor(const std::string& relatedItemPath) = 0;
  /** Equal for two sources that would read the same bytes, so a cache can be
   *  keyed by it. */
  virtual int64_t hashCode() const = 0;
};

} // namespace sonore
