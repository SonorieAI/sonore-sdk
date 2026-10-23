// SPDX-License-Identifier: Apache-2.0
// The system layer: streams and files (streams.h, file_types.h), addresses,
// sockets, URLs and HTTP (net.h), child processes, locks and libraries
// (process.h), pipes, framed connections, coordinator/worker pairs and
// service discovery (ipc.h).
//
// Everything is exercised for real, on this machine, with no fixture outside
// the test: the HTTP cases run against a small HTTP/1.1 server this program
// starts on the loopback (redirect chains, a POST body, a 404, a server too
// slow for the timeout, one that never answers so the request is cancelled,
// a chunked body, a megabyte with progress); the child-process and IPC cases
// launch THIS executable again in a child mode (--child <mode>), so no helper
// program is needed. One real HTTPS request to example.com shows the platform
// TLS path; it is skipped, and says so, when the machine is offline.
//
// Known answers where they exist: RFC 3986's own resolution examples
// (section 5.4, all 42), RFC 5952's IPv6 text forms, the Windows argv
// quoting round trip through CommandLineToArgvW itself, and on Windows the
// Winsock/WinHTTP/iphlpapi layouts net.h declares by hand, checked against
// the SDK's headers at compile time.
//
//   system_test             everything that stays on this machine
//   system_test --network   also multicast delivery, which binds 0.0.0.0
//                           (on Windows that can raise a firewall prompt)
#if defined(_WIN32)
// The real Winsock headers FIRST (before anything includes windows.h), only so
// the layouts net.h declares can be compared with them below.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <iphlpapi.h>
#include <winhttp.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#endif

#include <sonore/file_types.h>
#include <sonore/ipc.h>
#include <sonore/net.h>
#include <sonore/process.h>
#include <sonore/streams.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace sonore;

// ── Compile-time: net.h's hand-written Windows ABI against the SDK's own ─────
#if defined(_WIN32)
namespace nd = sonore::netdetail;
// The CRT's offsetof is a cast expression clang does not accept as a constant;
// the builtin it is made of is one for both compilers.
#define ABI_OFFSET(type, member) __builtin_offsetof(type, member)
static_assert(sizeof(nd::WsSockAddrIn) == sizeof(sockaddr_in), "sockaddr_in");
static_assert(sizeof(nd::WsSockAddrIn6) == sizeof(sockaddr_in6), "sockaddr_in6");
static_assert(ABI_OFFSET(nd::WsSockAddrIn6, addr) == ABI_OFFSET(sockaddr_in6, sin6_addr), "sin6_addr");
static_assert(ABI_OFFSET(nd::WsSockAddrIn6, scopeId) == ABI_OFFSET(sockaddr_in6, sin6_scope_id), "sin6_scope_id");
static_assert(ABI_OFFSET(nd::WsSockAddrIn, addr) == ABI_OFFSET(sockaddr_in, sin_addr), "sin_addr");
static_assert(sizeof(nd::WsAddrInfo) == sizeof(ADDRINFOA), "ADDRINFOA");
static_assert(ABI_OFFSET(nd::WsAddrInfo, canonName) == ABI_OFFSET(ADDRINFOA, ai_canonname), "ai_canonname");
static_assert(ABI_OFFSET(nd::WsAddrInfo, addr) == ABI_OFFSET(ADDRINFOA, ai_addr), "ai_addr");
static_assert(ABI_OFFSET(nd::WsAddrInfo, next) == ABI_OFFSET(ADDRINFOA, ai_next), "ai_next");
static_assert(sizeof(nd::WsTimeval) == sizeof(timeval), "timeval");
static_assert(sizeof(nd::WsFdSet) == sizeof(fd_set), "fd_set");
static_assert(sizeof(nd::WsIpMreq) == sizeof(ip_mreq), "ip_mreq");
static_assert(sizeof(nd::WsIpv6Mreq) == sizeof(ipv6_mreq), "ipv6_mreq");
static_assert(sizeof(nd::WsSocketAddress) == sizeof(SOCKET_ADDRESS), "SOCKET_ADDRESS");
static_assert(ABI_OFFSET(nd::WsUnicastAddress, next) == ABI_OFFSET(IP_ADAPTER_UNICAST_ADDRESS_LH, Next), "Next");
static_assert(ABI_OFFSET(nd::WsUnicastAddress, address) == ABI_OFFSET(IP_ADAPTER_UNICAST_ADDRESS_LH, Address), "Address");
static_assert(ABI_OFFSET(nd::WsUnicastAddress, onLinkPrefixLength) ==
                  ABI_OFFSET(IP_ADAPTER_UNICAST_ADDRESS_LH, OnLinkPrefixLength), "OnLinkPrefixLength");
static_assert(ABI_OFFSET(nd::WsAdapterAddresses, next) == ABI_OFFSET(IP_ADAPTER_ADDRESSES_LH, Next), "Next");
static_assert(ABI_OFFSET(nd::WsAdapterAddresses, adapterName) == ABI_OFFSET(IP_ADAPTER_ADDRESSES_LH, AdapterName), "AdapterName");
static_assert(ABI_OFFSET(nd::WsAdapterAddresses, firstUnicastAddress) ==
                  ABI_OFFSET(IP_ADAPTER_ADDRESSES_LH, FirstUnicastAddress), "FirstUnicastAddress");
static_assert(ABI_OFFSET(nd::WsAdapterAddresses, physicalAddress) == ABI_OFFSET(IP_ADAPTER_ADDRESSES_LH, PhysicalAddress),
              "PhysicalAddress");
static_assert(ABI_OFFSET(nd::WsAdapterAddresses, physicalAddressLength) ==
                  ABI_OFFSET(IP_ADAPTER_ADDRESSES_LH, PhysicalAddressLength), "PhysicalAddressLength");
static_assert(ABI_OFFSET(nd::WsAdapterAddresses, ifType) == ABI_OFFSET(IP_ADAPTER_ADDRESSES_LH, IfType), "IfType");
static_assert(ABI_OFFSET(nd::WsAdapterAddresses, operStatus) == ABI_OFFSET(IP_ADAPTER_ADDRESSES_LH, OperStatus), "OperStatus");
static_assert(nd::kAfInet == AF_INET && nd::kAfInet6 == AF_INET6 && nd::kAfUnspec == AF_UNSPEC, "AF_");
static_assert(nd::kSockStream == SOCK_STREAM && nd::kSockDgram == SOCK_DGRAM, "SOCK_");
static_assert(nd::kIpprotoTcp == IPPROTO_TCP && nd::kIpprotoUdp == IPPROTO_UDP && nd::kIpprotoIp == IPPROTO_IP &&
                  nd::kIpprotoIpv6 == IPPROTO_IPV6, "IPPROTO_");
static_assert(nd::kSolSocket == SOL_SOCKET && nd::kSoReuseAddr == SO_REUSEADDR && nd::kSoBroadcast == SO_BROADCAST &&
                  nd::kSoError == SO_ERROR && nd::kSoExclusiveAddrUse == SO_EXCLUSIVEADDRUSE, "SO_");
static_assert(nd::kTcpNoDelay == TCP_NODELAY, "TCP_NODELAY");
static_assert(nd::kIpMulticastLoop == IP_MULTICAST_LOOP && nd::kIpAddMembership == IP_ADD_MEMBERSHIP &&
                  nd::kIpDropMembership == IP_DROP_MEMBERSHIP, "IP_");
static_assert(nd::kIpv6MulticastLoop == IPV6_MULTICAST_LOOP && nd::kIpv6JoinGroup == IPV6_JOIN_GROUP &&
                  nd::kIpv6LeaveGroup == IPV6_LEAVE_GROUP && nd::kIpv6V6Only == IPV6_V6ONLY, "IPV6_");
static_assert(nd::kAiPassive == AI_PASSIVE && nd::kAiNumericHost == AI_NUMERICHOST, "AI_");
static_assert(nd::kFionbio == (long) FIONBIO && nd::kSioUdpConnReset == (unsigned long) _WSAIOW(IOC_VENDOR, 12), "ioctls");
static_assert(nd::kWsaEWouldBlock == WSAEWOULDBLOCK && nd::kWsaEInProgress == WSAEINPROGRESS &&
                  nd::kWsaEMsgSize == WSAEMSGSIZE && nd::kWsaEConnReset == WSAECONNRESET, "WSAE");
static_assert(nd::kShutdownBoth == SD_BOTH, "SD_BOTH");
static_assert(nd::kIfTypeSoftwareLoopback == IF_TYPE_SOFTWARE_LOOPBACK && nd::kIfTypeTunnel == IF_TYPE_TUNNEL &&
                  nd::kIfOperStatusUp == IfOperStatusUp, "IF_");
static_assert(nd::kWinHttpAccessDefaultProxy == WINHTTP_ACCESS_TYPE_DEFAULT_PROXY &&
                  nd::kWinHttpAccessAutomaticProxy == WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, "access");
static_assert(nd::kWinHttpFlagSecure == WINHTTP_FLAG_SECURE && nd::kWinHttpFlagEscapeDisable == WINHTTP_FLAG_ESCAPE_DISABLE,
              "flags");
static_assert(nd::kWinHttpOptionReceiveResponseTimeout == WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, "response timeout");
static_assert(nd::kWinHttpOptionDisableFeature == WINHTTP_OPTION_DISABLE_FEATURE &&
                  nd::kWinHttpDisableRedirects == WINHTTP_DISABLE_REDIRECTS, "redirects");
static_assert(nd::kWinHttpOptionDecompression == WINHTTP_OPTION_DECOMPRESSION &&
                  nd::kWinHttpDecompressionAll == WINHTTP_DECOMPRESSION_FLAG_ALL, "decompression");
static_assert(nd::kWinHttpAddReqAdd == WINHTTP_ADDREQ_FLAG_ADD && nd::kWinHttpAddReqReplace == WINHTTP_ADDREQ_FLAG_REPLACE,
              "addreq");
static_assert(nd::kWinHttpQueryStatusCode == WINHTTP_QUERY_STATUS_CODE &&
                  nd::kWinHttpQueryRawHeadersCrlf == WINHTTP_QUERY_RAW_HEADERS_CRLF &&
                  nd::kWinHttpQueryFlagNumber == WINHTTP_QUERY_FLAG_NUMBER, "query");
static_assert(nd::kErrorWinHttpTimeout == ERROR_WINHTTP_TIMEOUT &&
                  nd::kErrorWinHttpNameNotResolved == ERROR_WINHTTP_NAME_NOT_RESOLVED &&
                  nd::kErrorWinHttpCancelled == ERROR_WINHTTP_OPERATION_CANCELLED &&
                  nd::kErrorWinHttpCannotConnect == ERROR_WINHTTP_CANNOT_CONNECT &&
                  nd::kErrorWinHttpConnectionError == ERROR_WINHTTP_CONNECTION_ERROR &&
                  nd::kErrorWinHttpSecureFailure == ERROR_WINHTTP_SECURE_FAILURE, "errors");
#endif

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  std::fflush(stdout);
}

void section(const char* name) { std::printf("\n-- %s\n", name); }

using Clock = std::chrono::steady_clock;
long long msSince(Clock::time_point t) {
  return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::vector<uint8_t> bytesOf(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }
std::string textOf(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

std::string hexOf(const std::string& s) {
  static const char hex[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : s) {
    out.push_back(hex[c >> 4]);
    out.push_back(hex[c & 15]);
  }
  return out;
}

std::filesystem::path u8(const std::string& s) { return std::filesystem::u8path(s); }

std::string readWholeFile(const std::string& path) {
  FileInputStream in(path);
  return in.openedOk() ? in.readEntireStreamAsString() : std::string("<unreadable>");
}

/** Replace the file at `path` with `content`. A FileOutputStream opens at
 *  the END of a file that exists, where truncate() cuts nothing: without the
 *  setPosition(0) this appended. */
void writeWholeFile(const std::string& path, const std::string& content) {
  FileOutputStream out(path);
  out.setPosition(0);
  out.truncate();
  out.writeText(content);
}

// ═══════════════════════════════════════════════════════════════════════════
// Child modes: this executable, relaunched
// ═══════════════════════════════════════════════════════════════════════════

/** This process's own arguments as UTF-8, exactly as they were passed: on
 *  Windows through CommandLineToArgvW, since main's argv is in the ANSI code
 *  page and would mangle the very characters being tested. */
std::vector<std::string> ownArguments(int argc, char** argv) {
  std::vector<std::string> out;
#if defined(_WIN32)
  (void) argc;
  (void) argv;
  int count = 0;
  LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
  for (int i = 0; i < count; ++i) out.push_back(pathio::narrow(wide[i]));
  LocalFree(wide);
#else
  for (int i = 0; i < argc; ++i) out.push_back(argv[i]);
#endif
  return out;
}

struct TestWorker : ChildProcessWorker {
  std::mutex mutex;
  std::condition_variable cv;
  bool lost = false;
  void handleConnectionMade() override { sendMessageToCoordinator(bytesOf("hello from worker")); }
  void handleMessageFromCoordinator(const std::vector<uint8_t>& message) override {
    const std::string text = textOf(message);
    if (text == "crash") std::_Exit(3); // dies mid-conversation, as a crash does
    sendMessageToCoordinator(bytesOf("echo:" + text));
  }
  void handleConnectionLost() override {
    std::lock_guard<std::mutex> lock(mutex);
    lost = true;
    cv.notify_all();
  }
};

int runChild(const std::string& mode, const std::vector<std::string>& args, const std::vector<std::string>& all) {
#if defined(_WIN32)
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
#endif
  if (mode == "args") {
    for (const std::string& a : args) std::printf("ARG:%s\n", hexOf(a).c_str());
    return 0;
  }
  if (mode == "streams") {
    std::fputs("to-out\n", stdout);
    std::fflush(stdout);
    std::fputs("to-err\n", stderr);
    std::fflush(stderr);
    return 7;
  }
  if (mode == "flood") {
    // More than any pipe buffer, before anyone reads: the parent must drain.
    std::string line(1023, 'x');
    line.push_back('\n');
    for (int i = 0; i < 2048; ++i) std::fwrite(line.data(), 1, line.size(), stdout);
    return 0;
  }
  if (mode == "count") {
    for (int i = 0; i < 50; ++i) {
      std::printf("line %d\n", i);
      std::fflush(stdout);
    }
    return 0;
  }
  if (mode == "sleep") {
    sleepMs(30000);
    return 0;
  }
  if (mode == "hold-lock" && !args.empty()) {
    InterProcessLock lock(args[0]);
    if (!lock.enter(5000)) return 4;
    std::printf("LOCKED\n");
    std::fflush(stdout);
    sleepMs(1500);
    lock.exit();
    return 0;
  }
  if (mode == "worker" && !args.empty()) {
    TestWorker worker;
    if (!worker.initialiseFromArguments(all, args[0], 3000)) return 2;
    std::unique_lock<std::mutex> lock(worker.mutex);
    worker.cv.wait_for(lock, std::chrono::seconds(30), [&] { return worker.lost; });
    return 0;
  }
  if (mode == "silent-worker" && !args.empty()) {
    // Connects to the coordinator's pipe and then says nothing at all, never
    // pinging: what a hung worker looks like from the outside.
    const std::string prefix = args[0] + ":";
    for (const std::string& a : all)
      if (a.compare(0, prefix.size(), prefix) == 0) {
        NamedPipe pipe;
        if (!pipe.openExisting(a.substr(prefix.size()))) return 5;
        sleepMs(15000);
        return 0;
      }
    return 6;
  }
  return 99;
}

// ═══════════════════════════════════════════════════════════════════════════
// Streams and files
// ═══════════════════════════════════════════════════════════════════════════

void testStreams() {
  section("streams");
  MemoryOutputStream out;
  out.writeInt(0x01020304);
  out.writeIntBigEndian(0x01020304);
  out.writeShort((int16_t) -2);
  out.writeInt64BigEndian(0x0102030405060708LL);
  out.writeFloat(1.5f);
  out.writeDoubleBigEndian(-0.25);
  out.writeString("hi");
  out.writeBool(true);
  const std::vector<uint8_t> bytes = out.getBytes();
  const uint8_t expectHead[8] = {4, 3, 2, 1, 1, 2, 3, 4};
  check(bytes.size() == 4 + 4 + 2 + 8 + 4 + 8 + 3 + 1 && std::memcmp(bytes.data(), expectHead, 8) == 0,
        "typed writes lay out little- and big-endian bytes exactly");
  MemoryInputStream in(bytes.data(), bytes.size(), false);
  const bool round = in.readInt() == 0x01020304 && in.readIntBigEndian() == 0x01020304 && in.readShort() == -2 &&
                     in.readInt64BigEndian() == 0x0102030405060708LL && in.readFloat() == 1.5f &&
                     in.readDoubleBigEndian() == -0.25 && in.readString() == "hi" && in.readBool();
  check(round && in.isExhausted(), "...and read back to the same values, to the last byte");
  check(in.readInt() == 0 && in.read(nullptr, 4) == 0, "reading past the end gives zeros, never garbage");

  // Placeholder size filled in afterwards: write at a position before the end.
  MemoryOutputStream chunk;
  chunk.writeInt(0);
  chunk.writeText("payload");
  chunk.setPosition(0);
  chunk.writeInt(7);
  check(chunk.getDataSize() == 11 && chunk.getData()[0] == 7 && chunk.toString().substr(4) == "payload",
        "an output stream overwrites at a position before its end (a size filled in later)");

  const std::string lines = "one\ntwo\r\nthree\rfour";
  MemoryInputStream text(lines.data(), lines.size(), true);
  const std::string l1 = text.readNextLine(), l2 = text.readNextLine(), l3 = text.readNextLine(),
                    l4 = text.readNextLine();
  check(l1 == "one" && l2 == "two" && l3 == "three" && l4 == "four" && text.isExhausted(),
        "readNextLine ends lines at \\n, \\r\\n and a lone \\r");

  // BufferedInputStream against direct reads, with seeks inside and outside
  // its window: every byte must agree.
  std::vector<uint8_t> big(100000);
  for (size_t i = 0; i < big.size(); ++i) big[i] = (uint8_t) (i * 7 + i / 251);
  MemoryInputStream source(big.data(), big.size(), false);
  BufferedInputStream buffered(source, 1000);
  uint32_t state = 12345;
  bool agree = true;
  int64_t pos = 0;
  for (int step = 0; step < 2000 && agree; ++step) {
    state = state * 1664525u + 1013904223u;
    if ((state >> 28) == 0) {
      pos = (int64_t) ((state >> 4) % big.size());
      agree = buffered.setPosition(pos);
    }
    uint8_t piece[3000];
    const int want = 1 + (int) ((state >> 8) % 2999);
    const int got = buffered.read(piece, want);
    const int expected = (int) (std::min)((int64_t) want, (int64_t) big.size() - pos);
    agree = agree && got == expected && std::memcmp(piece, big.data() + pos, (size_t) got) == 0;
    pos += got;
    if (pos >= (int64_t) big.size()) {
      pos = 0;
      buffered.setPosition(0);
    }
  }
  check(agree, "BufferedInputStream returns what the source holds through 2000 random reads and seeks");
  buffered.setPosition(10);
  check(buffered.peekByte() == (char) big[10] && buffered.getPosition() == 10, "peekByte does not consume");

  SubregionStream window(source, 1000, 50);
  std::vector<uint8_t> got;
  window.readIntoVector(got);
  check(window.getTotalLength() == 50 && got.size() == 50 && std::memcmp(got.data(), big.data() + 1000, 50) == 0 &&
            window.isExhausted(),
        "SubregionStream reads exactly its window of the source");
  window.setPosition(40);
  check(window.read(got.data(), 100) == 10 && window.getPosition() == 50, "...and stops at the window's end");

  // A reader walking chunks -- an id, a 64-bit size, the body -- over a file
  // whose size field is damaged. position + size wrapped negative, the
  // stream took that as 0, and the walk began again from the first chunk:
  // a hang on one bad header.
  MemoryOutputStream damaged;
  damaged.writeInt(0x61746164);
  damaged.writeInt64(INT64_MAX);
  damaged.writeText("tail");
  MemoryInputStream chunks(damaged.getData(), damaged.getDataSize(), false);
  int visited = 0;
  while (!chunks.isExhausted() && visited < 100) {
    chunks.readInt();
    chunks.skipNextBytes(chunks.readInt64());
    ++visited;
  }
  check(visited == 1 && chunks.getPosition() == (int64_t) damaged.getDataSize(),
        "a skip by a size near INT64_MAX lands at the end, not back at the start");
}

void testFiles(const std::string& dir) {
  section("files");
  const std::string path = joinPath(dir, "Percussión 音.bin");
  {
    FileOutputStream out(path);
    check(out.openedOk(), "FileOutputStream creates a file by a UTF-8 path with accents and CJK");
    out.writeText("hello");
  }
  {
    FileOutputStream out(path);
    check(out.getPosition() == 5, "an existing file opens at its END, as the reference's does");
    out.writeText(" world");
  }
  check(readWholeFile(path) == "hello world", "...so a second stream appends");
  {
    FileOutputStream out(path);
    out.setPosition(5);
    check(out.truncate(), "truncate() cuts at the position");
  }
  check(readWholeFile(path) == "hello", "...leaving what was before it");

  FileInputStream in(path);
  check(in.openedOk() && in.getTotalLength() == 5, "FileInputStream knows the length");
  in.setPosition(1);
  char c[2] = {0, 0};
  check(in.read(c, 2) == 2 && c[0] == 'e' && c[1] == 'l', "...and reads from a position");
  in.skipNextBytes(INT64_MAX);
  check(in.getPosition() == 5 && in.isExhausted(),
        "a skip by a size near INT64_MAX lands at the file's end, not back at its start");
  check(FileInputStream(joinPath(dir, "missing")).failedToOpen(), "a missing file fails to open, and says so");

  FileInputSource src(path, true);
  auto s1 = src.createInputStream();
  check(s1 && s1->readEntireStreamAsString() == "hello" && src.hashCode() == FileInputSource(path, true).hashCode() &&
            src.hashCode() != FileInputSource(path + "x", true).hashCode(),
        "FileInputSource reopens the file, and hashes by path (and time)");

  // TemporaryFile: write beside, then swap in; the target is never half-written.
  // writeWholeFile replaces what is there, as every use of it here assumes.
  const std::string target = joinPath(dir, "preset.json");
  writeWholeFile(target, "a longer first version");
  writeWholeFile(target, "old");
  check(readWholeFile(target) == "old", ("writing a whole file again replaces it (\"" + readWholeFile(target) + "\")").c_str());
  {
    TemporaryFile temp = TemporaryFile::forTarget(target);
    check(temp.getFile() != target && parentPath(temp.getFile()) == dir &&
              fileName(temp.getFile()).find("preset_temp") == 0,
          "TemporaryFile::forTarget names a sibling of the target");
    writeWholeFile(temp.getFile(), "new contents");
    check(readWholeFile(target) == "old", "...the target is untouched while the temporary is written");
    check(temp.overwriteTargetFileWithTemporary() && readWholeFile(target) == "new contents" &&
              !fileExists(temp.getFile()),
          "...and swapped in whole");
  }
  std::string abandoned;
  {
    TemporaryFile temp = TemporaryFile::forTarget(target);
    abandoned = temp.getFile();
    writeWholeFile(abandoned, "half a save");
  }
  check(!fileExists(abandoned) && readWholeFile(target) == "new contents",
        "a TemporaryFile never swapped in is deleted, and the target keeps its last good contents");
  TemporaryFile inTemp = TemporaryFile::inTemporaryDirectory(".wav");
  check(parentPath(inTemp.getFile()) == temporaryDirectory() && fileExtension(inTemp.getFile()) == ".wav",
        "TemporaryFile::inTemporaryDirectory lands in the system temp folder with the suffix");

  // MemoryMappedFile: the whole file, and a range starting at an offset that
  // is not on an allocation boundary.
  const std::string mapped = joinPath(dir, "mapped.bin");
  std::string content(200000, '\0');
  for (size_t i = 0; i < content.size(); ++i) content[i] = (char) ((i * 31 + 7) & 0xff);
  writeWholeFile(mapped, content);
  {
    MemoryMappedFile all(mapped);
    check(all.isMapped() && all.getSize() == content.size() &&
              std::memcmp(all.getData(), content.data(), content.size()) == 0,
          "MemoryMappedFile maps the whole file, byte-exact");
    MemoryMappedFile range(mapped, 70001, 1000);
    check(range.isMapped() && range.getSize() == 1000 && range.getStart() == 70001 &&
              std::memcmp(range.getData(), content.data() + 70001, 1000) == 0,
          "...and a range at an unaligned offset points at exactly that byte");
    MemoryMappedFile tail(mapped, 199990, 1000);
    check(tail.getSize() == 10, "...a range past the end is clipped to the file");
    // A length that runs start + length past int64's top: the sum wrapped
    // negative, the end landed before the start and nothing was mapped.
    MemoryMappedFile huge(mapped, 70001, INT64_MAX);
    check(huge.isMapped() && huge.getSize() == content.size() - 70001 &&
              std::memcmp(huge.getData(), content.data() + 70001, 16) == 0,
          "...a length too large to add to the start maps to the end of the file");
    // Zero bytes maps nothing, wherever it starts: at an unaligned start the
    // allocation boundary below it was mapped, and getData() came back
    // non-null with a size of 0.
    MemoryMappedFile none(mapped, 70001, 0), noneAligned(mapped, 0, 0);
    check(!none.isMapped() && none.getData() == nullptr && none.getSize() == 0 && !noneAligned.isMapped(),
          "...and a zero-length range maps nothing, at any start");
  }
  {
    MemoryMappedFile rw(mapped, 0, 4, MemoryMappedFile::readWrite);
    if (rw.isMapped()) std::memcpy(rw.getData(), "EDIT", 4);
  }
  check(readWholeFile(mapped).substr(0, 4) == "EDIT", "a read-write mapping writes through to the file");
  check(!MemoryMappedFile(joinPath(dir, "missing")).isMapped(), "a missing file maps to nothing");

  // Wildcards.
  struct Case {
    const char* name;
    const char* pattern;
    bool expect;
  } cases[] = {{"kick.wav", "*.wav", true},     {"KICK.WAV", "*.wav", true},      {"kick.wave", "*.wav", false},
               {"kick.wav", "k?ck.*", true},    {"kck.wav", "k?ck.*", false},     {"é.wav", "?.wav", true},
               {"a.b.c", "*.*.*", true},        {"abc", "a*b*c*", true},          {"ab", "a*b*c", false},
               {"", "*", true},                 {"", "?", false},                 {"aaaaaaaaaaaaaaaaaaab", "*a*a*a*a*a*c", false},
               {"x", "", false},                {"snare 01.aif", "snare ??.aif", true}};
  bool wildOk = true;
  for (const Case& t : cases) {
    const bool got = WildcardFileFilter::matchesWildcard(t.name, t.pattern);
    if (got != t.expect) {
      std::printf("    wildcard '%s' vs '%s': got %d\n", t.name, t.pattern, (int) got);
      wildOk = false;
    }
  }
  check(wildOk, "matchesWildcard: '*', '?' (one code point), case ignored, 14 cases");
  WildcardFileFilter audio("*.wav; *.aif*", "*", "Audio");
  check(audio.isFileSuitable("C:\\x\\Loop.AIFF") && !audio.isFileSuitable("/x/notes.txt") &&
            audio.isDirectorySuitable("/x/Drums") && audio.getDescription() == "Audio",
        "WildcardFileFilter takes '; '-separated lists and matches the file NAME");
  struct DialogLike {
    std::string label = "MIDI";
    std::string patterns = "*.mid;*.midi";
  };
  check(WildcardFileFilter::fromDialogFilter(DialogLike()).isFileSuitable("song.MIDI"),
        "...and is built from the file dialog's FileFilter");

  // FileSearchPath over a real tree.
  const std::string lib = joinPath(dir, "lib");
  pathio::makeDirectory(lib.c_str());
  pathio::makeDirectory(joinPath(lib, "Drums").c_str());
  pathio::makeDirectory(joinPath(joinPath(lib, "Drums"), "Kicks").c_str());
  writeWholeFile(joinPath(lib, "a.wav"), "a");
  writeWholeFile(joinPath(lib, "readme.txt"), "r");
  writeWholeFile(joinPath(joinPath(lib, "Drums"), "snare.wav"), "s");
  writeWholeFile(joinPath(joinPath(joinPath(lib, "Drums"), "Kicks"), "kick.WAV"), "k");
  FileSearchPath search;
  search.add(lib);
  search.addIfNotAlreadyThere(joinPath(lib, "Drums"));
  check(!search.addIfNotAlreadyThere(lib + pathSeparator()), "addIfNotAlreadyThere ignores a trailing separator");
  check(search.findChildFiles(true, "*.wav").size() == 5,
        "findChildFiles finds the nested folder's two files twice before removeRedundantPaths...");
  search.removeRedundantPaths();
  const auto found = search.findChildFiles(true, "*.wav");
  check(search.getNumPaths() == 1 && found.size() == 3, "...and three files after it, recursively, case ignored");
  check(search.findChildFiles(false, "*.wav").size() == 1, "...one without recursion");
  search.add(joinPath(dir, "unplugged drive"));
  const FileSearchPath reread(search.toString());
  check(reread.getNumPaths() == 2 && reread[1] == joinPath(dir, "unplugged drive"),
        "toString and the constructor round-trip the list");
  search.removeNonexistentPaths();
  check(search.getNumPaths() == 1 && search.isFileInPath(joinPath(joinPath(lib, "Drums"), "snare.wav"), true) &&
            !search.isFileInPath(joinPath(joinPath(lib, "Drums"), "snare.wav"), false),
        "removeNonexistentPaths drops the missing folder; isFileInPath with and without recursion");

  // The volume list and its detector.
  const auto volumes = mountedVolumes();
#if defined(_WIN32)
  check(std::find(volumes.begin(), volumes.end(), std::string("C:\\")) != volumes.end(), "mountedVolumes includes C:\\");
#else
  check(!volumes.empty(), "mountedVolumes lists something");
#endif
  MountedVolumeListChangeDetector detector;
  std::mutex fakeMutex;
  std::vector<std::string> fake{"A", "B"};
  std::atomic<int> changes{0};
  detector.listVolumes = [&] {
    std::lock_guard<std::mutex> lock(fakeMutex);
    return fake;
  };
  detector.onChange = [&] { ++changes; };
  detector.start(20);
  sleepMs(120);
  const int before = changes.load();
  {
    std::lock_guard<std::mutex> lock(fakeMutex);
    fake.push_back("E");
  }
  const auto t0 = Clock::now();
  while (changes.load() == before && msSince(t0) < 2000) sleepMs(5);
  detector.stop();
  check(before == 0 && changes.load() == 1 && detector.getLastVolumeList().size() == 3,
        "MountedVolumeListChangeDetector reports a volume arriving once, and not before");
  {
    // Deleted from its own callback: a watcher waiting for one drive, then
    // done. The destructor's stop() cannot join the thread it is running on
    // and detached it, and the thread went round its loop again into the
    // freed object (AddressSanitizer: heap-use-after-free, its mutex).
    static std::atomic<int> polls{0};
    static std::atomic<bool> gone{false};
    auto* oneShot = new MountedVolumeListChangeDetector();
    oneShot->listVolumes = [] { return std::vector<std::string>(1, polls.fetch_add(1) == 0 ? "A" : "B"); };
    oneShot->onChange = [oneShot] {
      delete oneShot;
      gone = true;
    };
    oneShot->start(10);
    const auto t1 = Clock::now();
    while (!gone.load() && msSince(t1) < 2000) sleepMs(5);
    sleepMs(100); // two more poll periods, for a thread that went on
    check(gone.load() && polls.load() == 2,
          "a detector deleted from its own onChange: its thread ends without touching it again");
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// Addresses
// ═══════════════════════════════════════════════════════════════════════════

void testAddresses() {
  section("addresses");
  struct Text {
    const char* in;
    const char* out; // null: must be refused
  } table[] = {
      {"192.0.2.1", "192.0.2.1"},
      {"0.0.0.0", "0.0.0.0"},
      {"255.255.255.255", "255.255.255.255"},
      {"256.1.1.1", nullptr},
      {"1.2.3", nullptr},
      {"1.2.3.4.5", nullptr},
      {"01.2.3.4", nullptr},
      {" 1.2.3.4", nullptr},
      {"1..2.3", nullptr},
      // RFC 5952 section 4's examples, and its recommendations
      {"2001:db8:0:0:0:0:2:1", "2001:db8::2:1"},
      {"2001:0db8:0000:0000:0000:0000:0002:0001", "2001:db8::2:1"},
      {"2001:db8:0:1:1:1:1:1", "2001:db8:0:1:1:1:1:1"},
      {"2001:0:0:1:0:0:0:1", "2001:0:0:1::1"},
      {"2001:db8:0:0:1:0:0:1", "2001:db8::1:0:0:1"},
      {"2001:DB8::1", "2001:db8::1"},
      {"::", "::"},
      {"::1", "::1"},
      {"1::", "1::"},
      {"1:2:3:4:5:6:7::", "1:2:3:4:5:6:7:0"},
      {"::ffff:192.0.2.128", "::ffff:192.0.2.128"},
      {"::ffff:c000:280", "::ffff:192.0.2.128"},
      {"fe80::1:2", "fe80::1:2"},
      {":::", nullptr},
      {"1::2::3", nullptr},
      {"1:2:3:4:5:6:7:8:9", nullptr},
      {"12345::", nullptr},
      {"1:", nullptr},
      {":1", nullptr},
      {"1:2:3:4:5:6:7:1.2.3.4", nullptr},
      {"1.2.3.4::", nullptr},
      {"fe80::1%eth0", nullptr},
      {"[::1]", nullptr},
      {"g::1", nullptr},
  };
  int good = 0;
  for (const Text& t : table) {
    IPAddress ip;
    const bool parsed = IPAddress::parse(t.in, ip);
    const bool ok = t.out ? (parsed && ip.toString() == t.out) : !parsed;
    if (ok) ++good;
    else std::printf("    %s -> %s (parsed %d)\n", t.in, parsed ? ip.toString().c_str() : "-", (int) parsed);
  }
  char line[160];
  std::snprintf(line, sizeof(line), "IPv4/IPv6 parse and RFC 5952 format: %d of %d cases", good, (int) (sizeof(table) / sizeof(table[0])));
  check(good == (int) (sizeof(table) / sizeof(table[0])), line);

  check(IPAddress("127.0.0.1").isLoopback() && IPAddress("::1").isLoopback() && IPAddress("::ffff:127.1.2.3").isLoopback() &&
            IPAddress("239.1.2.3").isMulticast() && IPAddress("ff02::1").isMulticast() &&
            IPAddress("169.254.0.9").isLinkLocal() && IPAddress("fe80::1").isLinkLocal() &&
            IPAddress::broadcast().isBroadcast() && IPAddress(0x7f000001u) == IPAddress::local(),
        "loopback, multicast, link-local and broadcast are recognised in both families");
  const IPAddress mapped = IPAddress::convertIPv4AddressToIPv4Mapped(IPAddress(10, 1, 2, 3));
  check(mapped.toString() == "::ffff:10.1.2.3" && IPAddress::convertIPv4MappedAddressToIPv4(mapped) == IPAddress(10, 1, 2, 3) &&
            mapped.toIPv4() == 0x0a010203u,
        "IPv4-mapped conversion both ways");

  const MACAddress mac("AA:bb-0C.dd:ee:0F");
  check(mac.toString() == "aa-bb-0c-dd-ee-0f" && mac.toString(":") == "aa:bb:0c:dd:ee:0f" &&
            mac.toInt64() == 0xaabb0cddee0fLL && MACAddress("12:34").isNull(),
        "MACAddress parses any separators and formats with the one asked for");

  const auto all = IPAddress::getAllAddresses(true);
  std::string list;
  for (const IPAddress& a : all) list += a.toString() + " ";
  std::printf("    this machine: %s\n", list.c_str());
  bool hasV4 = false;
  for (const IPAddress& a : all) hasV4 = hasV4 || !a.isIPv6;
  check(!all.empty() && hasV4, "getAllAddresses lists this machine's addresses, IPv4 among them");
  const IPAddress local = IPAddress::getLocalAddress();
  check(!local.isNull(), "getLocalAddress gives an address");
  // A broadcast for every IPv4 address on a subnet; none for a point-to-point
  // /32 (a VPN's) -- so check the ones that have one, and that one exists
  // wherever a LAN address does.
  int withBroadcast = 0, consistent = 0;
  for (const IPAddress& a : all) {
    if (a.isIPv6 || a.isLoopback()) continue;
    const IPAddress bcast = IPAddress::getInterfaceBroadcastAddress(a);
    std::printf("    %s broadcasts on %s\n", a.toString().c_str(), bcast.toString().c_str());
    if (bcast.isNull()) continue;
    ++withBroadcast;
    // The broadcast is the address with its host bits set: OR-ing the address
    // in changes nothing, and it is never the address itself.
    if ((bcast.toIPv4() | a.toIPv4()) == bcast.toIPv4() && bcast != a) ++consistent;
  }
  if (withBroadcast > 0)
    check(consistent == withBroadcast, "getInterfaceBroadcastAddress gives each subnet's broadcast address");
  const auto macs = MACAddress::getAllAddresses();
  std::printf("    %d adapter MAC address(es)%s%s\n", (int) macs.size(), macs.empty() ? "" : ", first ",
              macs.empty() ? "" : macs[0].toString().c_str());
  bool macsOk = true;
  for (const MACAddress& m : macs) macsOk = macsOk && !m.isNull();
  check(macsOk, "MACAddress::getAllAddresses lists no null addresses");
}

// ═══════════════════════════════════════════════════════════════════════════
// URLs
// ═══════════════════════════════════════════════════════════════════════════

void testUrls() {
  section("URL");
  const URL u("HTTP://user:pw@Example.com:8080/a/b%20c.wav?x=1&y=two+words&z=%26#frag");
  check(u.getScheme() == "http" && u.getUserInfo() == "user:pw" && u.getHost() == "Example.com" && u.getPort() == 8080 &&
            u.getPath() == "/a/b%20c.wav" && u.getQuery() == "x=1&y=two+words&z=%26" && u.getFragment() == "frag",
        "components split as RFC 3986 Appendix B does (scheme lowercased, the rest as written)");
  check(u.getFileName() == "b c.wav" && u.getParameterValue("y") == "two words" && u.getParameterValue("z") == "&" &&
            u.getParameterNames().size() == 3,
        "getFileName and getParameters decode");
  check(u.toString() == "http://user:pw@Example.com:8080/a/b%20c.wav?x=1&y=two+words&z=%26#frag" &&
            u.toString(false) == "http://user:pw@Example.com:8080/a/b%20c.wav#frag",
        "toString recomposes it, with or without the query");
  const URL v6("https://[2001:DB8::1]:443/x");
  check(v6.getHost() == "2001:db8::1" && v6.getOrigin() == "https://[2001:db8::1]:443" && v6.isWellFormed(),
        "an IPv6 literal host comes without its brackets, and goes back with them");
  check(URL("http://example.com").getPortOrDefault() == 80 && URL("https://example.com").getPortOrDefault() == 443 &&
            URL("https://example.com:8443").getPortOrDefault() == 8443,
        "getPortOrDefault knows the schemes' ports");

  struct Formed {
    const char* text;
    bool expect;
  } formed[] = {{"http://example.com/a?b#c", true},   {"mailto:someone@example.com", true},
                {"urn:isbn:0451450523", true},         {"http://example.com/a b", false},
                {"http:///nohost", false},             {"http://example.com:99999/", false},
                {"http://example.com:8x/", false},     {"http://[::1/", false},
                {"http://[1.2.3.4]/", false},          {"relative/path", false},
                {"http://ex%ample.com/", false},       {"http://ex%41mple.com/%7e", true},
                {"ftp://ftp.example.com/file.txt", true}};
  int formedOk = 0;
  for (const Formed& f : formed) {
    if (URL(f.text).isWellFormed() == f.expect) ++formedOk;
    else std::printf("    isWellFormed(%s) wrong\n", f.text);
  }
  check(formedOk == (int) (sizeof(formed) / sizeof(formed[0])), "isWellFormed: 13 valid and invalid URIs");

  // RFC 3986 section 5.4: every normal and abnormal example.
  const URL base("http://a/b/c/d;p?q");
  const char* resolution[][2] = {
      {"g:h", "g:h"},           {"g", "http://a/b/c/g"},          {"./g", "http://a/b/c/g"},
      {"g/", "http://a/b/c/g/"}, {"/g", "http://a/g"},             {"//g", "http://g"},
      {"?y", "http://a/b/c/d;p?y"}, {"g?y", "http://a/b/c/g?y"},  {"#s", "http://a/b/c/d;p?q#s"},
      {"g#s", "http://a/b/c/g#s"}, {"g?y#s", "http://a/b/c/g?y#s"}, {";x", "http://a/b/c/;x"},
      {"g;x", "http://a/b/c/g;x"}, {"g;x?y#s", "http://a/b/c/g;x?y#s"}, {"", "http://a/b/c/d;p?q"},
      {".", "http://a/b/c/"},   {"./", "http://a/b/c/"},          {"..", "http://a/b/"},
      {"../", "http://a/b/"},   {"../g", "http://a/b/g"},         {"../..", "http://a/"},
      {"../../", "http://a/"},  {"../../g", "http://a/g"},
      // abnormal
      {"../../../g", "http://a/g"}, {"../../../../g", "http://a/g"}, {"/./g", "http://a/g"},
      {"/../g", "http://a/g"},  {"g.", "http://a/b/c/g."},        {".g", "http://a/b/c/.g"},
      {"g..", "http://a/b/c/g.."}, {"..g", "http://a/b/c/..g"},   {"./../g", "http://a/b/g"},
      {"./g/.", "http://a/b/c/g/"}, {"g/./h", "http://a/b/c/g/h"}, {"g/../h", "http://a/b/c/h"},
      {"g;x=1/./y", "http://a/b/c/g;x=1/y"}, {"g;x=1/../y", "http://a/b/c/y"},
      {"g?y/./x", "http://a/b/c/g?y/./x"}, {"g?y/../x", "http://a/b/c/g?y/../x"},
      {"g#s/./x", "http://a/b/c/g#s/./x"}, {"g#s/../x", "http://a/b/c/g#s/../x"}, {"http:g", "http:g"}};
  int resolved = 0;
  const int total = (int) (sizeof(resolution) / sizeof(resolution[0]));
  for (const auto& r : resolution) {
    const std::string got = base.resolve(r[0]).toString();
    if (got == r[1]) ++resolved;
    else std::printf("    resolve(%s) = %s, expected %s\n", r[0], got.c_str(), r[1]);
  }
  char text[128];
  std::snprintf(text, sizeof(text), "RFC 3986 5.4 reference resolution: %d of %d examples", resolved, total);
  check(resolved == total, text);
  check(URL("a:/b").resolve("..//x").getPath() == "//x" && URL("a:/b").resolve("..//x").toString() == "a:/.//x" &&
            URL(URL("a:/b").resolve("..//x").toString()).getPath() == "/.//x",
        "a resolved path that starts \"//\" without an authority is written so it reads back as a path");

  check(URL::addEscapeChars("a b&c=d/é(x)", true) == "a%20b%26c%3Dd%2F%C3%A9(x)" &&
            URL::addEscapeChars("a b&c=d/é(x)", true, false) == "a%20b%26c%3Dd%2F%C3%A9%28x%29" &&
            URL::addEscapeChars("a b&c:d@e", false) == "a%20b&c:d@e",
        "addEscapeChars: parameters keep only unreserved (+brackets), path segments keep sub-delimiters");
  check(URL::removeEscapeChars("a%20b%c3%a9%zz%4") == "a b\xC3\xA9%zz%4" && URL::removeEscapeChars("a+b") == "a+b",
        "removeEscapeChars decodes valid escapes, leaves broken ones and '+' alone");
  const URL api("https://api.example.com/v1");
  const URL child = api.getChildURL("samples/my kick.wav").withParameter("q", "a&b c").withParameter("n", "1");
  check(child.toString() == "https://api.example.com/v1/samples/my%20kick.wav?q=a%26b%20c&n=1",
        "getChildURL adds path parts escaped; withParameter appends encoded pairs");
  check(URL("http://h/a/b/").getChildURL("/c%20d").toString() == "http://h/a/b/c%20d" &&
            URL("http://h/a/b/c?x#y").getParentURL().toString() == "http://h/a/b" &&
            URL("http://h/a?x=1").withoutParameters().toString() == "http://h/a" &&
            URL("http://h/a").withNewSubPath("z y").toString() == "http://h/z%20y",
        "exactly one '/' between parts, existing escapes kept; getParentURL, withoutParameters, withNewSubPath");
}

// ═══════════════════════════════════════════════════════════════════════════
// Sockets
// ═══════════════════════════════════════════════════════════════════════════

void testSockets() {
  section("TCP and UDP");
  StreamingSocket listener;
  check(listener.createListener(0) && listener.getBoundPort() > 0 && listener.getHostName() == "127.0.0.1",
        "a listener on port 0 gets a port from the system, on the loopback by default");
  const int port = listener.getBoundPort();
  std::unique_ptr<StreamingSocket> accepted;
  std::thread acceptor([&] { accepted = listener.waitForNextConnection(3000); });
  StreamingSocket client;
  const bool connected = client.connect("localhost", port, 2000);
  acceptor.join();
  check(connected && accepted && accepted->isConnected() && client.isLocal(), "connect by name, accept on the other side");
#if defined(_WIN32)
  {
    // Found in review: Winsock's sockets are inheritable handles, so a child
    // started with bInheritHandles (system(), a host's own helper) kept a
    // listener closed here listening. POSIX opens them close-on-exec.
    DatagramSocket udp;
    udp.bindToPort(0);
    bool inheritable = false;
    for (intptr_t raw : {listener.getRawSocketHandle(), client.getRawSocketHandle(),
                         accepted ? accepted->getRawSocketHandle() : (intptr_t) -1, udp.getRawSocketHandle()}) {
      DWORD flags = 0;
      if (raw != -1 && GetHandleInformation((HANDLE) raw, &flags) && (flags & HANDLE_FLAG_INHERIT)) inheritable = true;
    }
    check(!inheritable, "no socket (listener, connected, accepted, UDP) is an inheritable handle");
  }
#endif
  if (connected && accepted) {
    check(client.write("ping", 4) == 4, "write sends all of it");
    char buf[16] = {0};
    check(accepted->read(buf, 4, true) == 4 && std::memcmp(buf, "ping", 4) == 0, "...and read receives it");
    std::vector<uint8_t> big(1 << 20), back(1 << 20);
    for (size_t i = 0; i < big.size(); ++i) big[i] = (uint8_t) (i ^ (i >> 9));
    std::thread writer([&] { accepted->write(big.data(), (int) big.size()); });
    const int got = client.read(back.data(), (int) back.size(), true);
    writer.join();
    check(got == (int) big.size() && back == big, "a megabyte arrives whole with blockUntilSpecifiedAmountHasArrived");
    const auto t0 = Clock::now();
    const int idle = client.waitUntilReady(true, 150);
    check(idle == 0 && msSince(t0) >= 100, "waitUntilReady times out when nothing comes");
#if defined(_WIN32)
    // Found in review (round 28): waiting to read also asked select() about
    // the exception set, where Windows reports an urgent (MSG_OOB) byte --
    // which recv never returns. waitUntilReady said "ready" at once, and the
    // read that followed blocked until ordinary data came. (POSIX's poll
    // reports urgent data as POLLPRI, which is not asked for.)
    netdetail::winsock().send((netdetail::SocketHandle) accepted->getRawSocketHandle(), "!", 1, MSG_OOB);
    sleepMs(50);
    const auto t1 = Clock::now();
    const int urgent = client.waitUntilReady(true, 150);
    check(urgent == 0 && msSince(t1) >= 100, "an out-of-band byte alone is nothing to read: waitUntilReady times out");
    accepted->write("x", 1);
    check(client.waitUntilReady(true, 2000) == 1 && client.read(buf, 1, false) == 1 && buf[0] == 'x',
          "...and ordinary data after it is ready, and read");
#endif
    accepted->close();
    check(client.read(buf, 4, false) == 0 && !client.isConnected(), "read returns 0 once the peer has closed");
  }
  listener.close();
  StreamingSocket nobody;
  check(!nobody.connect("127.0.0.1", port, 1000), "connecting to a closed port fails");

  StreamingSocket v6;
  if (v6.createListener(0, "::1")) {
    StreamingSocket c6;
    std::unique_ptr<StreamingSocket> a6;
    std::thread acc([&] { a6 = v6.waitForNextConnection(3000); });
    const bool ok = c6.connect("[::1]", v6.getBoundPort(), 2000);
    acc.join();
    check(ok && a6 && a6->getPeerAddress() == IPAddress::local(true), "TCP over IPv6 loopback, the peer reported as ::1");
  } else {
    std::printf("  ---- no IPv6 loopback here: IPv6 TCP not exercised ----\n");
  }

  DatagramSocket receiver, sender;
  check(receiver.bindToPort(0) && receiver.getBoundPort() > 0, "a UDP socket binds the loopback on a free port");
  char dgram[64] = {0};
  check(receiver.read(dgram, sizeof(dgram), false) == 0, "a non-blocking read with nothing waiting returns 0 at once");
  check(sender.write("127.0.0.1", receiver.getBoundPort(), "hello udp", 9) == 9, "write sends one datagram");
  std::string from;
  int fromPort = 0;
  check(receiver.waitUntilReady(true, 2000) == 1 &&
            receiver.read(dgram, sizeof(dgram), true, from, fromPort) == 9 && std::memcmp(dgram, "hello udp", 9) == 0 &&
            from == "127.0.0.1" && fromPort > 0,
        "...it arrives with its sender's address and port");
  sender.write("localhost", receiver.getBoundPort(), "0123456789", 10);
  check(receiver.waitUntilReady(true, 2000) == 1 && receiver.read(dgram, 4, true) == 4 &&
            std::memcmp(dgram, "0123", 4) == 0 && receiver.read(dgram, sizeof(dgram), false) == 0,
        "a datagram longer than the buffer is cut to fit and the rest is gone");
  // An ICMP "port unreachable" from a send to nobody must not break receiving.
  DatagramSocket lonely;
  lonely.bindToPort(0);
  DatagramSocket gone;
  gone.bindToPort(0);
  const int deadPort = gone.getBoundPort();
  gone.shutdown();
  lonely.write("127.0.0.1", deadPort, "x", 1);
  sleepMs(50);
  sender.write("127.0.0.1", lonely.getBoundPort(), "after", 5);
  check(lonely.waitUntilReady(true, 2000) == 1 && lonely.read(dgram, sizeof(dgram), true) == 5,
        "a send to a closed port does not fail the next receive (Windows' WSAECONNRESET)");

  DatagramSocket group;
  group.bindToPort(0);
  check(group.joinMulticast("239.255.42.99") && group.leaveMulticast("239.255.42.99") && !group.joinMulticast("10.0.0.1"),
        "joinMulticast/leaveMulticast on a group, and a unicast address refused");
}

void testMulticastDelivery() {
  section("multicast delivery (--network)");
  DatagramSocket in, out;
  in.setEnablePortReuse(true);
  if (!in.bindToPort(0, "0.0.0.0") || !in.joinMulticast("239.255.42.98")) {
    check(false, "binding 0.0.0.0 and joining a group");
    return;
  }
  in.setMulticastLoopbackEnabled(true);
  out.setMulticastLoopbackEnabled(true);
  out.write("239.255.42.98", in.getBoundPort(), "mcast", 5);
  char buf[16];
  check(in.waitUntilReady(true, 2000) == 1 && in.read(buf, sizeof(buf), true) == 5, "a multicast datagram loops back");
}

// ═══════════════════════════════════════════════════════════════════════════
// HTTP, against a server of our own
// ═══════════════════════════════════════════════════════════════════════════

/** A small HTTP/1.1 server: one request per connection, a thread each. */
class TestHttpServer {
public:
  ~TestHttpServer() { stop(); }

  bool start(const std::string& address = "127.0.0.1") {
    if (!listener_.createListener(0, address)) return false;
    port_ = listener_.getBoundPort();
    acceptor_ = std::thread([this] {
      while (!stop_.load()) {
        std::unique_ptr<StreamingSocket> client = listener_.waitForNextConnection(50);
        if (!client) continue;
        std::shared_ptr<StreamingSocket> shared(std::move(client));
        std::lock_guard<std::mutex> lock(mutex_);
        workers_.emplace_back([this, shared] { serve(*shared); });
      }
    });
    return true;
  }
  void stop() {
    stop_ = true;
    if (acceptor_.joinable()) acceptor_.join();
    std::vector<std::thread> workers;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      workers.swap(workers_);
    }
    for (std::thread& t : workers) t.join();
    listener_.close();
  }
  int port() const { return port_; }
  std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }
  std::string lastTarget() {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastTarget_;
  }

private:
  static bool readLine(StreamingSocket& s, std::string& line) {
    line.clear();
    for (;;) {
      if (s.waitUntilReady(true, 5000) != 1) return false;
      char c;
      if (s.read(&c, 1, true) != 1) return false;
      if (c == '\n') break;
      if (c != '\r') line.push_back(c);
      if (line.size() > 8192) return false;
    }
    return true;
  }

  void send(StreamingSocket& s, int code, const char* reason, const std::string& body, const std::string& extra,
            bool head) {
    std::string out = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\nConnection: close\r\nX-Server: sonore-test\r\n" + extra +
                      "\r\n";
    if (!head) out += body;
    s.write(out.data(), (int) out.size());
  }

  /** Waits up to ms, returning early if the server stops or the client goes. */
  bool linger(StreamingSocket& s, int ms) {
    const auto t0 = Clock::now();
    while (msSince(t0) < ms) {
      if (stop_.load()) return false;
      if (s.waitUntilReady(true, 20) == 1) {
        char c;
        if (s.read(&c, 1, false) <= 0) return false; // the client went
      }
    }
    return true;
  }

  void serve(StreamingSocket& s) {
    std::string requestLine, headerLine;
    if (!readLine(s, requestLine)) return;
    std::map<std::string, std::string> headers;
    std::string headerText;
    while (readLine(s, headerLine) && !headerLine.empty()) {
      const size_t colon = headerLine.find(':');
      if (colon == std::string::npos) continue;
      std::string name = headerLine.substr(0, colon);
      for (char& c : name) c = asciiLowerChar(c);
      std::string value = headerLine.substr(colon + 1);
      while (!value.empty() && value[0] == ' ') value.erase(0, 1);
      headers[name] = value;
      headerText += name + ": " + value + "\n";
    }
    const size_t sp1 = requestLine.find(' '), sp2 = requestLine.rfind(' ');
    if (sp1 == std::string::npos || sp2 <= sp1) return;
    const std::string method = requestLine.substr(0, sp1);
    const std::string target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      lastTarget_ = target;
    }
    std::string body;
    if (headers.count("content-length")) {
      body.resize((size_t) std::atoll(headers["content-length"].c_str()));
      if (!body.empty() && s.read(&body[0], (int) body.size(), true) != (int) body.size()) return;
    }
    const bool head = method == "HEAD";
    if (target == "/hello") send(s, 200, "OK", "Hello, world", "Content-Type: text/plain\r\n", head);
    else if (target == "/headers") send(s, 200, "OK", headerText, "", head);
    else if (target == "/echo") send(s, 200, "OK", method + " " + body, "", head);
    else if (target == "/method") send(s, 200, "OK", method + (body.empty() ? "" : " with a body"), "", head);
    else if (target == "/auth") send(s, 200, "OK", headers["authorization"], "", head);
    else if (target.compare(0, 6, "/path/") == 0) send(s, 200, "OK", target, "", head);
    else if (target.compare(0, 10, "/redirect/") == 0) {
      const int n = std::atoi(target.c_str() + 10);
      if (n <= 0) send(s, 200, "OK", "landed", "", head);
      else send(s, 302, "Found", "", "Location: /redirect/" + std::to_string(n - 1) + "\r\n", head);
    } else if (target == "/abs-redirect") send(s, 301, "Moved", "", "Location: " + base() + "/hello\r\n", head);
    else if (target == "/rel-redirect/a/b") send(s, 302, "Found", "", "Location: ../../hello\r\n", head);
    else if (target == "/see-other") send(s, 303, "See Other", "", "Location: /method\r\n", head);
    else if (target == "/temp-redirect") send(s, 307, "Temporary Redirect", "", "Location: /echo\r\n", head);
    else if (target == "/missing") send(s, 404, "Not Found", "not here", "", head);
    else if (target == "/slow") {
      if (linger(s, 3000)) send(s, 200, "OK", "too late", "", head);
    } else if (target == "/stall") {
      linger(s, 10000);
    } else if (target == "/drip") {
      const std::string top = "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\nConnection: close\r\n\r\n";
      s.write(top.data(), (int) top.size());
      for (int i = 0; i < 100 && !stop_.load(); ++i) {
        if (s.write("0123456789", 10) != 10) break;
        if (!linger(s, 100)) break;
      }
    } else if (target == "/chunked") {
      const std::string out = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                              "9;ext=1\r\nchunk-one\r\nA\r\n|chunk-two\r\n6\r\n|three\r\n0\r\nX-Trailer: t\r\n\r\n";
      s.write(out.data(), (int) out.size());
    } else if (target == "/chunked-plain") {
      const std::string out = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                              "9\r\nchunk-one\r\nA\r\n|chunk-two\r\n6\r\n|three\r\n0\r\n\r\n";
      s.write(out.data(), (int) out.size());
    } else if (target == "/big") {
      std::string data(1 << 20, '\0');
      for (size_t i = 0; i < data.size(); ++i) data[i] = (char) ((i * 7 + i / 251) & 0xff);
      send(s, 200, "OK", data, "Content-Type: application/octet-stream\r\n", head);
    } else send(s, 404, "Not Found", "no route", "", head);
    linger(s, 200); // let the client read before the connection goes
  }

  StreamingSocket listener_;
  std::thread acceptor_;
  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::atomic<bool> stop_{false};
  int port_ = 0;
  std::string lastTarget_;
};

std::string fetch(const URL& url, const URL::InputStreamOptions& options, int* status = nullptr,
                  WebInputStream** keep = nullptr) {
  auto stream = std::make_unique<WebInputStream>(url, options);
  if (!stream->connect()) {
    if (status) *status = -1;
    return "<" + stream->getErrorMessage() + ">";
  }
  if (status) *status = stream->getStatusCode();
  const std::string body = stream->readEntireStreamAsString();
  if (keep) *keep = stream.release();
  return body;
}

void testHttp(const std::string& dir) {
  section("HTTP (WinHTTP / NSURLSession / libcurl) against a local server");
  TestHttpServer server;
  if (!server.start()) {
    check(false, "the test's HTTP server starts on the loopback");
    return;
  }
  const std::string base = server.base();
  using Options = URL::InputStreamOptions;

  {
    WebInputStream s(URL(base + "/hello"));
    check(s.connect() && s.getStatusCode() == 200 && s.getTotalLength() == 12 &&
              s.getResponseHeader("x-server") == "sonore-test" && s.readEntireStreamAsString() == "Hello, world" &&
              s.isExhausted(),
          "GET: status, Content-Length, a header by name (case ignored), the body");
  }
  int status = 0;
  const std::string echoedHeaders = fetch(URL(base + "/headers"), Options().withExtraHeader("X-Test", "42").withExtraHeaders("X-Other: a b\r\nX-Third: 3"), &status);
  check(status == 200 && echoedHeaders.find("x-test: 42") != std::string::npos &&
            echoedHeaders.find("x-other: a b") != std::string::npos && echoedHeaders.find("x-third: 3") != std::string::npos,
        "extra request headers arrive at the server");
  check(fetch(URL(base + "/echo"), Options().withPostData("a=1&b=two").withExtraHeader("Content-Type", "application/x-www-form-urlencoded")) ==
            "POST a=1&b=two",
        "POST sends its body");
  check(fetch(URL(base + "/echo"), Options().withPostData(std::string("put body")).withHttpRequestCmd("PUT")) == "PUT put body",
        "another method, with a body");
  {
    WebInputStream head(URL(base + "/hello"), Options().withHttpRequestCmd("HEAD"));
    check(head.connect() && head.getStatusCode() == 200 && head.getTotalLength() == 12 && head.readEntireStreamAsString().empty(),
          "HEAD: the length without the body");
  }
  {
    WebInputStream s(URL(base + "/redirect/3"));
    check(s.connect() && s.getStatusCode() == 200 && s.readEntireStreamAsString() == "landed" &&
              s.getNumRedirectsFollowed() == 3 && s.getFinalURL().getPath() == "/redirect/0",
          "a chain of three redirects is followed to the end");
  }
  {
    WebInputStream s(URL(base + "/redirect/3"), Options().withNumRedirectsToFollow(1));
    check(s.connect() && s.getStatusCode() == 302 && s.getNumRedirectsFollowed() == 1 &&
              s.getResponseHeader("Location") == "/redirect/1",
          "...and stopped where asked, the last 3xx handed back as the response");
  }
  check(fetch(URL(base + "/abs-redirect"), Options()) == "Hello, world" &&
            fetch(URL(base + "/rel-redirect/a/b"), Options()) == "Hello, world",
        "absolute and relative (\"../../hello\") Locations resolve as RFC 3986 says");
  check(fetch(URL(base + "/see-other"), Options().withPostData("x")) == "GET",
        "303 after a POST becomes a GET without the body");
  check(fetch(URL(base + "/temp-redirect"), Options().withPostData("kept")) == "POST kept",
        "307 repeats the POST with its body");
  status = 0;
  check(fetch(URL(base + "/missing"), Options(), &status) == "not here" && status == 404,
        "404 is a response: connected, the status, and the server's body");
  check(fetch(URL(base + "/path/a b/é"), Options()) == "/path/a%20b/%C3%A9" && server.lastTarget() == "/path/a%20b/%C3%A9",
        "characters a request line cannot carry are escaped on the way out");
  check(fetch(URL("http://user:pa%20ss@127.0.0.1:" + std::to_string(server.port()) + "/auth"), Options()) ==
            "Basic " + netdetail::base64("user:pa ss"),
        "credentials in the URL become a Basic Authorization header");
  {
    // Each part apart, and said: the first macOS run failed this check with
    // nothing to show which of the three it was.
    struct Fetched {
      bool connected;
      int64_t total;
      std::string body, error;
    };
    const auto chunked = [&](const char* path) {
      WebInputStream s(URL(base + path));
      Fetched f;
      f.connected = s.connect();
      f.total = f.connected ? s.getTotalLength() : -2;
      f.body = f.connected ? s.readEntireStreamAsString() : std::string();
      f.error = s.getErrorMessage();
      return f;
    };
    const std::string decoded = "chunk-one|chunk-two|three";
    char why[512];
    const Fetched plain = chunked("/chunked-plain");
    std::snprintf(why, sizeof(why), "a chunked body arrives decoded, length unknown (connected %d, length %lld, body \"%.60s\")",
                  plain.connected ? 1 : 0, (long long) plain.total, plain.body.c_str());
    check(plain.connected && plain.total == -1 && plain.body == decoded, why);
    // And with a chunk extension and a trailer, both legal HTTP/1.1, which
    // WinHTTP and libcurl decode. NSURLSession is Apple's own parser, and it
    // refused this response outright (connected 0) while the plain one above
    // is the question the SDK can answer for. What no backend may ever do is
    // hand over a body other than the one sent -- so on macOS it is decoded
    // exactly or refused with a reason, and the reason is printed.
    const Fetched odd = chunked("/chunked");
    const bool exact = odd.connected && odd.total == -1 && odd.body == decoded;
    std::snprintf(why, sizeof(why),
                  "...and with a chunk extension and a trailer (connected %d, length %lld, body \"%.60s\"%s%.160s)",
                  odd.connected ? 1 : 0, (long long) odd.total, odd.body.c_str(),
                  odd.error.empty() ? "" : ", refused: ", odd.error.c_str());
#if defined(__APPLE__)
    check(exact || (!odd.connected && !odd.error.empty()), why);
#else
    check(exact, why);
#endif
  }
  {
    std::vector<int64_t> seen;
    int64_t totalSeen = 0;
    WebInputStream s(URL(base + "/big"), Options().withProgressCallback([&](int64_t done, int64_t total) {
                       seen.push_back(done);
                       totalSeen = total;
                       return true;
                     }));
    std::vector<uint8_t> data;
    s.connect();
    s.readIntoVector(data);
    bool pattern = data.size() == (1u << 20);
    for (size_t i = 0; pattern && i < data.size(); ++i) pattern = data[i] == (uint8_t) ((i * 7 + i / 251) & 0xff);
    check(pattern && !seen.empty() && seen.back() == (1 << 20) && totalSeen == (1 << 20),
          "a megabyte arrives intact, progress counting up to the total");
  }
  {
    WebInputStream s(URL(base + "/big"), Options().withProgressCallback([](int64_t done, int64_t) { return done < 100000; }));
    std::vector<uint8_t> data;
    s.connect();
    s.readIntoVector(data);
    check(data.size() >= 100000 && data.size() < (1u << 20) && s.wasCancelled(),
          "a progress callback returning false stops the download");
  }
  {
    const auto t0 = Clock::now();
    WebInputStream s(URL(base + "/slow"), Options().withConnectionTimeoutMs(400));
    const bool ok = s.connect();
    const long long took = msSince(t0);
    std::printf("    slow server: %s after %lld ms\n", s.getErrorMessage().c_str(), took);
    check(!ok && took < 2500, "a server slower than the timeout fails the request in time");
  }
  {
    WebInputStream s(URL(base + "/stall"), Options().withConnectionTimeoutMs(20000));
    std::thread canceller([&] {
      sleepMs(300);
      s.cancel();
    });
    const auto t0 = Clock::now();
    const bool ok = s.connect();
    const long long took = msSince(t0);
    canceller.join();
    std::printf("    stalled server: cancelled after %lld ms (%s)\n", took, s.getErrorMessage().c_str());
    check(!ok && s.wasCancelled() && took < 2500, "cancel() from another thread ends a request waiting for its response");
  }
  {
    WebInputStream s(URL(base + "/drip"), Options().withConnectionTimeoutMs(20000));
    char buf[64];
    const bool ok = s.connect() && s.read(buf, 10) > 0;
    std::thread canceller([&] {
      sleepMs(250);
      s.cancel();
    });
    const auto t0 = Clock::now();
    std::vector<uint8_t> rest;
    s.readIntoVector(rest);
    const long long took = msSince(t0);
    canceller.join();
    check(ok && rest.size() < 990 && took < 2500, "...and one in the middle of its body");
  }
  {
    const std::string target = joinPath(dir, "download.bin");
    writeWholeFile(target, "previous");
    const URL::DownloadResult missing = URL(base + "/missing").downloadToFile(target);
    check(!missing.succeeded && missing.statusCode == 404 && readWholeFile(target) == "previous",
          "downloadToFile refuses a 404 and leaves the target as it was");
    const URL::DownloadResult big = URL(base + "/big").downloadToFile(target);
    const std::string got = readWholeFile(target);
    check(big.succeeded && big.bytesWritten == (1 << 20) && got.size() == (1u << 20) &&
              (uint8_t) got[300000] == (uint8_t) ((300000 * 7 + 300000 / 251) & 0xff),
          "...and replaces it whole when the download succeeds");
    const URL::DownloadResult stopped =
        URL(base + "/big").downloadToFile(target, Options().withProgressCallback([](int64_t d, int64_t) { return d < 50000; }));
    check(!stopped.succeeded && readWholeFile(target).size() == (1u << 20),
          "...and a cancelled download leaves the previous file in place");
  }
  check(URL(base + "/hello").readEntireTextStream() == "Hello, world" && !URL(base + "/hello").createInputStream()->isError(),
        "readEntireTextStream and createInputStream");
  check(!URL("http://127.0.0.1:1/").createInputStream(Options().withConnectionTimeoutMs(2000)),
        "createInputStream gives null when nothing answers");
  check(fetch(URL("ftp://127.0.0.1/x"), Options()).find("only http and https") != std::string::npos,
        "a scheme other than http(s) is refused by name");
  {
    // Found in review: a line break in a header value went out as a second
    // header (WinHTTP takes headers as one CRLF-separated block), and a port
    // past 65535 was cut to 16 bits -- port + 65536 reached this server.
    WebInputStream split(URL(base + "/headers"), Options().withExtraHeader("X-Test", "a\r\nX-Second: b"));
    WebInputStream badName(URL(base + "/headers"), Options().withExtraHeader("X Test", "a"));
    WebInputStream badMethod(URL(base + "/method"), Options().withHttpRequestCmd("GET /other"));
    check(!split.connect() && !badName.connect() && !badMethod.connect() && split.getStatusCode() == 0 &&
              split.getErrorMessage().find("header") != std::string::npos,
          "a header value with a line break, a header name or method that is not a token: refused, nothing sent");
    const std::string before = server.lastTarget();
    WebInputStream wrapped(URL("http://127.0.0.1:" + std::to_string(server.port() + 65536) + "/hello?wrapped"),
                           Options().withConnectionTimeoutMs(2000));
    check(!wrapped.connect() && server.lastTarget() == before && wrapped.getErrorMessage().find("port") != std::string::npos,
          "a URL whose port is past 65535 is refused, not sent to the port it wraps to");
  }

#if defined(_WIN32)
  {
    // Found in review: with a WinHTTP session per request and none left open
    // between them, WinHTTP kept one handle per request for good -- 1078
    // after a thousand downloads. Counted as editor_soak_test counts them.
    // A server on ONE thread, answering in turn: TestHttpServer keeps a
    // thread (and its handle) per connection until it stops, which would be
    // counted against the client.
    auto handleCount = [] {
      DWORD n = 0;
      GetProcessHandleCount(GetCurrentProcess(), &n);
      return (long) n;
    };
    StreamingSocket plain;
    plain.createListener(0);
    std::atomic<bool> done{false};
    std::thread answerer([&] {
      while (!done.load()) {
        std::unique_ptr<StreamingSocket> c = plain.waitForNextConnection(50);
        if (!c) continue;
        char request[2048];
        if (c->waitUntilReady(true, 2000) == 1) c->read(request, (int) sizeof(request), false);
        const std::string reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        c->write(reply.data(), (int) reply.size());
        c->waitUntilReady(true, 200);
      }
    });
    const URL plainUrl("http://127.0.0.1:" + std::to_string(plain.getBoundPort()) + "/");
    for (int i = 0; i < 10; ++i) plainUrl.readEntireTextStream();
    sleepMs(200);
    const long before = handleCount();
    bool allOk = true;
    for (int i = 0; i < 150; ++i) allOk = plainUrl.readEntireTextStream() == "ok" && allOk;
    sleepMs(200);
    const long grew = handleCount() - before;
    done = true;
    answerer.join();
    std::printf("    150 sequential requests: %ld handles more\n", grew);
    check(allOk && grew < 40, "sequential requests do not leave a handle each behind");
  }
#endif

  TestHttpServer server6;
  if (server6.start("::1")) {
    check(fetch(URL("http://[::1]:" + std::to_string(server6.port()) + "/hello"), Options()) == "Hello, world",
          "HTTP to an IPv6 literal host");
  }
  server.stop();
}

void testRealHttps() {
  section("one real HTTPS request (network-dependent)");
  WebInputStream s(URL("https://example.com/"), URL::InputStreamOptions().withConnectionTimeoutMs(8000));
  if (!s.connect()) {
    std::printf("  SKIPPED: example.com unreachable (%s) -- offline, or no route out\n", s.getErrorMessage().c_str());
    return;
  }
  const std::string body = s.readEntireStreamAsString();
  check(s.getStatusCode() == 200 && body.find("Example Domain") != std::string::npos,
        "https://example.com/ through the platform's TLS: 200 and the page");
}

// ═══════════════════════════════════════════════════════════════════════════
// Processes
// ═══════════════════════════════════════════════════════════════════════════

std::string g_self;

void testProcesses() {
  section("ChildProcess, InterProcessLock, DynamicLibrary, DeletedAtShutdown");
  check(!g_self.empty() && fileName(g_self).find("system_test") != std::string::npos,
        "currentExecutablePath is this program");
  check(currentModulePath() == g_self, "currentModulePath, from an executable, is the same file");

  const std::vector<std::string> tricky = {"plain",       "two words", "",           "quote\"inside",
                                           "trailing\\",  "back\\\\\"q", "tab\there",  "\\\\server\\share\\",
                                           "ünïcødé 音", "a;b&c|d>e",  "%PATH%",     "$HOME `x`"};
  {
    std::vector<std::string> args{g_self, "--child", "args"};
    args.insert(args.end(), tricky.begin(), tricky.end());
    ChildProcess p;
    check(p.start(args, ChildProcess::wantStdOut), "a child starts with twelve awkward arguments");
    const std::string out = p.readAllProcessOutput();
    p.waitForProcessToFinish(5000);
    std::string expect;
    for (const std::string& a : tricky) expect += "ARG:" + hexOf(a) + "\n";
    check(out == expect && p.getExitCode() == 0,
          "...each arrives byte-for-byte: empty, quotes, trailing backslashes, UTF-8, shell metacharacters unexpanded");
  }
  {
    ChildProcess p;
    p.start({g_self, "--child", "streams"});
    check(p.waitForProcessToFinish(5000) && p.getExitCode() == 7 && p.getStandardOutput() == "to-out\n" &&
              p.getStandardError() == "to-err\n" && !p.isRunning(),
          "stdout and stderr captured apart, the exit code returned");
    const std::string merged = p.readAllProcessOutput();
    check(merged.size() == 14 && merged.find("to-out\n") != std::string::npos && merged.find("to-err\n") != std::string::npos,
          "...and both in readAllProcessOutput");
  }
  {
    ChildProcess p;
    p.start({g_self, "--child", "flood"}, ChildProcess::wantStdOut);
    const bool finished = p.waitForProcessToFinish(10000);
    check(finished && p.getStandardOutput().size() == 2048u * 1024u,
          "a child writing 2 MB before anyone reads finishes (drained, not deadlocked)");
  }
  {
    ChildProcess p;
    p.start({g_self, "--child", "count"}, ChildProcess::wantStdOut);
    std::string streamed;
    char buf[7];
    int n;
    while ((n = p.readProcessOutput(buf, (int) sizeof(buf))) > 0) streamed.append(buf, (size_t) n);
    check(streamed.find("line 0\n") == 0 && streamed.find("line 49\n") != std::string::npos,
          "readProcessOutput streams output in small reads until the end");
  }
  {
    ChildProcess p;
    p.start({g_self, "--child", "sleep"}, 0);
    const auto t0 = Clock::now();
    check(p.isRunning() && !p.waitForProcessToFinish(200) && msSince(t0) >= 150, "waitForProcessToFinish times out on a running child");
    check(p.kill() && !p.isRunning() && p.getExitCode() == 137, "kill() ends it at once; the exit code is 137");
  }
  {
    const auto t0 = Clock::now();
    int64_t pid = 0;
    {
      ChildProcess p;
      p.start(std::string("\"") + g_self + "\" --child sleep", 0);
      pid = p.getProcessId();
    }
    check(pid != 0 && msSince(t0) < 5000, "start(string) splits without a shell; the destructor kills a running child");
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, (DWORD) pid);
    const bool gone = !h || WaitForSingleObject(h, 2000) == WAIT_OBJECT_0;
    if (h) CloseHandle(h);
    check(gone, "...and it is really gone");
#endif
  }
  {
    ChildProcess p;
    check(!p.start({joinPath(temporaryDirectory(), "no-such-program-here.exe")}), "a program that does not exist does not start");
  }
#if defined(_WIN32)
  {
    // Found in review: a batch file is run by cmd.exe, which does not split
    // a command line by the C runtime's rules -- the quoting above no longer
    // kept an argument one argument. It is refused, however it is spelled.
    const std::string bat = joinPath(temporaryDirectory(), "sonore-review-" + uuidToString(makeUuid()).substr(0, 8) + ".bat");
    writeWholeFile(bat, "@echo off\r\n");
    ChildProcess p;
    check(!p.start({bat, "x"}) && processdetail::isWindowsBatchFile("C:\\a\\run.CMD") &&
              processdetail::isWindowsBatchFile("run.bat. .") && !processdetail::isWindowsBatchFile("run.exe") &&
              !processdetail::isWindowsBatchFile("bat"),
          "a .bat or .cmd program is refused (case and trailing dots or spaces ignored); others are not");
    pathio::remove(bat.c_str());
  }
#endif

  // InterProcessLock: two objects in one process, then a child holding it.
  const std::string lockName = "sonore system_test " + uuidToString(makeUuid());
  {
    InterProcessLock a(lockName), b(lockName);
    check(a.enter(0) && a.enter(0), "enter() nests within one object");
    bool otherThreadGot = true;
    std::thread other([&] { otherThreadGot = b.enter(100); });
    other.join();
    check(!otherThreadGot, "...another holder (another thread) is kept out");
    a.exit();
    a.exit();
    std::thread again([&] {
      otherThreadGot = b.enter(1000);
      if (otherThreadGot) b.exit();
    });
    again.join();
    check(otherThreadGot, "...and gets it once the first has fully exited");
  }
  {
    ChildProcess holder;
    holder.start({g_self, "--child", "hold-lock", lockName}, ChildProcess::wantStdOut);
    char buf[16] = {0};
    const int n = holder.readProcessOutput(buf, 7);
    InterProcessLock mine(lockName);
    const bool blocked = !mine.enter(100);
    const auto t0 = Clock::now();
    const bool later = mine.enter(5000);
    std::printf("    the child released the lock after %lld ms more\n", msSince(t0));
    check(n == 7 && std::memcmp(buf, "LOCKED\n", 7) == 0 && blocked && later,
          "InterProcessLock keeps another PROCESS out until it lets go");
    if (later) mine.exit();
    holder.waitForProcessToFinish(5000);
  }

  {
#if defined(_WIN32)
    DynamicLibrary lib("kernel32.dll");
    const char* fn = "GetTickCount";
#elif defined(__APPLE__)
    DynamicLibrary lib("/usr/lib/libSystem.B.dylib");
    const char* fn = "getpid";
#else
    DynamicLibrary lib("libc.so.6");
    const char* fn = "getpid";
#endif
    check(lib.isOpen() && lib.getFunction(fn) != nullptr && lib.getFunction("no_such_symbol_xyz") == nullptr,
          "DynamicLibrary opens a system library and finds a function in it");
    DynamicLibrary moved = std::move(lib);
    check(!lib.isOpen() && moved.isOpen() && !DynamicLibrary("no-such-library-xyz").isOpen(),
          "...moves, and a missing library does not open");
  }

  {
    static std::vector<int> order;
    struct Thing : DeletedAtShutdown {
      int id;
      explicit Thing(int i) : id(i) {}
      ~Thing() override { order.push_back(id); }
    };
    order.clear();
    new Thing(1);
    Thing* two = new Thing(2);
    new Thing(3);
    delete two;
    check(DeletedAtShutdown::getNumAlive() == 2, "a DeletedAtShutdown deleted early leaves the list");
    DeletedAtShutdown::deleteAll();
    check(order.size() == 3 && order[0] == 2 && order[1] == 3 && order[2] == 1 && DeletedAtShutdown::getNumAlive() == 0,
          "deleteAll deletes the rest, newest first");
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// IPC
// ═══════════════════════════════════════════════════════════════════════════

struct Collector : InterprocessConnection {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::vector<uint8_t>> messages;
  std::atomic<int> made{0}, lost{0};
  bool echo = false;
  explicit Collector(bool echoBack = false) : echo(echoBack) {}
  ~Collector() override { disconnect(); }
  void connectionMade() override {
    ++made;
    cv.notify_all();
  }
  void connectionLost() override {
    ++lost;
    cv.notify_all();
  }
  void messageReceived(const std::vector<uint8_t>& m) override {
    if (echo) sendMessage(m);
    std::lock_guard<std::mutex> lock(mutex);
    messages.push_back(m);
    cv.notify_all();
  }
  bool waitFor(size_t count, int ms) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return messages.size() >= count; });
  }
  bool waitLost(int ms) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return lost.load() > 0; });
  }
  bool waitMade(int ms) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return made.load() > 0; });
  }
};

/** Answers every message with megabyte replies until one fails to send: a
 *  request answered from messageReceived, on the connection's own thread. */
struct Replier : InterprocessConnection {
  std::atomic<int> replying{0};
  ~Replier() override { disconnect(); }
  void connectionMade() override {}
  void connectionLost() override {}
  void messageReceived(const std::vector<uint8_t>&) override {
    const std::vector<uint8_t> reply(1 << 20, 7);
    ++replying;
    while (sendMessage(reply)) ++replying;
  }
};

/** Reconnects from inside connectionLost, as a client that keeps itself
 *  connected might write it. */
struct Reconnector : InterprocessConnection {
  int port = 0;
  std::atomic<int> made{0}, lost{0}, reconnected{0};
  ~Reconnector() override { disconnect(); }
  void connectionMade() override { ++made; }
  void connectionLost() override {
    ++lost;
    if (connectToSocket("127.0.0.1", port, 1000)) ++reconnected;
  }
  void messageReceived(const std::vector<uint8_t>&) override {}
};

struct EchoServer : InterprocessConnectionServer {
  std::mutex mutex;
  std::vector<std::unique_ptr<Collector>> connections;
  ~EchoServer() override {
    stop();
    std::lock_guard<std::mutex> lock(mutex);
    connections.clear();
  }
  InterprocessConnection* createConnectionObject() override {
    std::lock_guard<std::mutex> lock(mutex);
    connections.push_back(std::make_unique<Collector>(true));
    return connections.back().get();
  }
  Collector* first() {
    std::lock_guard<std::mutex> lock(mutex);
    return connections.empty() ? nullptr : connections[0].get();
  }
};

/** Keeps what arrives; slow to handle "wait" (300 ms) and "stall" (2 s), as
 *  a worker busy with a request is. */
struct SlowHandler : InterprocessConnection {
  std::mutex mutex;
  std::vector<std::vector<uint8_t>> messages;
  std::atomic<int> made{0}, lost{0};
  ~SlowHandler() override { disconnect(); }
  void connectionMade() override { ++made; }
  void connectionLost() override { ++lost; }
  void messageReceived(const std::vector<uint8_t>& m) override {
    {
      std::lock_guard<std::mutex> lock(mutex);
      messages.push_back(m);
    }
    if (textOf(m) == "wait") sleepMs(300);
    if (textOf(m) == "stall") sleepMs(2000);
  }
  size_t count() {
    std::lock_guard<std::mutex> lock(mutex);
    return messages.size();
  }
};

/** Takes one client and stops listening, from createConnectionObject. */
struct OneClientServer : InterprocessConnectionServer {
  std::mutex mutex;
  std::vector<std::unique_ptr<Collector>> connections;
  std::atomic<int> created{0};
  ~OneClientServer() override {
    stop();
    std::lock_guard<std::mutex> lock(mutex);
    connections.clear();
  }
  InterprocessConnection* createConnectionObject() override {
    ++created;
    stop(); // on the server's own thread
    std::lock_guard<std::mutex> lock(mutex);
    connections.push_back(std::make_unique<Collector>(true));
    return connections.back().get();
  }
};

std::vector<std::vector<uint8_t>> testMessages() {
  std::vector<std::vector<uint8_t>> out;
  for (size_t size : {(size_t) 0, (size_t) 1, (size_t) 100, (size_t) 70000, (size_t) 1 << 20, (size_t) 5}) {
    std::vector<uint8_t> m(size);
    for (size_t i = 0; i < size; ++i) m[i] = (uint8_t) (i * 13 + size);
    out.push_back(m);
  }
  return out;
}

void testPipes() {
  section("NamedPipe");
  const std::string name = "sonore-test-" + uuidToString(makeUuid());
  NamedPipe server, client, other;
  check(server.createNewPipe(name, true) && !other.createNewPipe(name, true),
        "createNewPipe, and mustNotExist refuses a second server of the same name");
  check(!NamedPipe().openExisting("sonore-no-such-pipe-" + uuidToString(makeUuid())), "openExisting fails when there is no pipe");
  check(client.openExisting(name) && client.isOpen(), "openExisting connects to it");
  check(client.write("hello", 5, 1000) == 5, "the client writes");
  char buf[64] = {0};
  check(server.read(buf, 5, 2000) == 5 && std::memcmp(buf, "hello", 5) == 0 && server.isConnected(),
        "...the server reads (connecting as it does)");
  check(server.write("world!", 6, 1000) == 6 && client.read(buf, 6, 2000) == 6 && std::memcmp(buf, "world!", 6) == 0,
        "and back the other way");
  auto t0 = Clock::now();
  check(server.readSome(buf, sizeof(buf), 150) == 0 && msSince(t0) >= 100, "readSome times out with nothing to read");
  client.close();
  check(server.readSome(buf, sizeof(buf), 2000) == -1 && !server.isConnected(), "the server sees the client go");
  NamedPipe second;
  check(second.openExisting(name) && second.write("again", 5, 1000) == 5 && server.read(buf, 5, 2000) == 5 &&
            std::memcmp(buf, "again", 5) == 0,
        "...and serves the next client");
  std::thread closer([&] {
    sleepMs(200);
    server.close();
  });
  t0 = Clock::now();
  const int r = server.readSome(buf, sizeof(buf), 10000);
  closer.join();
  check(r == -1 && msSince(t0) < 2000, "close() from another thread wakes a blocked read");
}

void testConnections() {
  section("InterprocessConnection");
  EchoServer server;
  check(server.beginWaitingForSocket(0) && server.getBoundPort() > 0, "a connection server listens on the loopback");
  Collector client;
  check(client.connectToSocket("127.0.0.1", server.getBoundPort(), 2000) && client.waitMade(2000),
        "a client connects over TCP");
  const auto messages = testMessages();
  bool sent = true;
  for (const auto& m : messages) sent = sent && client.sendMessage(m);
  const bool all = client.waitFor(messages.size(), 10000);
  bool same = all;
  if (all)
    for (size_t i = 0; i < messages.size(); ++i) same = same && client.messages[i] == messages[i];
  check(sent && same, "six messages from empty to a megabyte, echoed back whole and in order");
  Collector* remote = server.first();
  client.disconnect();
  check(remote && remote->waitLost(3000) && client.lost.load() == 1, "disconnect: both ends report connectionLost, once");

  // A peer that does not speak the protocol: the frame's magic is wrong.
  StreamingSocket raw;
  raw.connect("127.0.0.1", server.getBoundPort(), 2000);
  sleepMs(100);
  Collector* victim = nullptr;
  {
    std::lock_guard<std::mutex> lock(server.mutex);
    if (server.connections.size() >= 2) victim = server.connections[1].get();
  }
  const std::vector<uint8_t> garbage = {1, 2, 3, 4, 5, 0, 0, 0, 'x', 'x', 'x', 'x', 'x'};
  raw.write(garbage.data(), (int) garbage.size());
  check(victim && victim->waitLost(3000) && victim->messages.empty(),
        "a frame with the wrong magic ends the connection without delivering anything");

  {
    // Found in review: disconnect() took the write lock while another thread
    // held it, blocked sending to a peer that had stopped reading -- and
    // waited for ever. The peer here accepts and never reads.
    StreamingSocket deaf;
    deaf.createListener(0);
    std::unique_ptr<StreamingSocket> deafPeer;
    std::thread acceptor([&] { deafPeer = deaf.waitForNextConnection(3000); });
    Collector stuck;
    const bool joined = stuck.connectToSocket("127.0.0.1", deaf.getBoundPort(), 2000);
    acceptor.join();
    std::thread sender([&] {
      const std::vector<uint8_t> big(1 << 20, 7);
      while (stuck.sendMessage(big)) {
      }
    });
    sleepMs(500); // the peer's buffers fill; the sender blocks
    std::atomic<bool> returned{false};
    std::thread stopper([&] {
      stuck.disconnect();
      returned = true;
    });
    const auto t0 = Clock::now();
    while (!returned.load() && msSince(t0) < 3000) sleepMs(10);
    const bool inTime = returned.load();
    if (!inTime) deafPeer.reset(); // free the sender, so the test can end
    stopper.join();
    sender.join();
    check(joined && inTime, "disconnect() returns while another thread is blocked sending to a peer that does not read");
  }
  {
    // Found in review (round 28): the sender blocked there may be the
    // connection's own thread, replying from messageReceived -- and
    // disconnect() waited for that thread before waking it, for ever.
    StreamingSocket deaf;
    deaf.createListener(0);
    std::unique_ptr<StreamingSocket> deafPeer;
    std::thread acceptor([&] { deafPeer = deaf.waitForNextConnection(3000); });
    Replier replier;
    const bool joined = replier.connectToSocket("127.0.0.1", deaf.getBoundPort(), 2000);
    acceptor.join();
    const std::vector<uint8_t> request = ipcdetail::encodeFrame(0xf2b49e2cu, "?", 1);
    if (deafPeer) deafPeer->write(request.data(), (int) request.size()); // one request; then it never reads
    const auto t0 = Clock::now();
    while (replier.replying.load() == 0 && msSince(t0) < 3000) sleepMs(10);
    sleepMs(500); // the peer's buffers fill; the reply blocks
    std::atomic<bool> returned{false};
    std::thread stopper([&] {
      replier.disconnect();
      returned = true;
    });
    const auto t1 = Clock::now();
    while (!returned.load() && msSince(t1) < 3000) sleepMs(10);
    const bool inTime = returned.load();
    if (!inTime) deafPeer.reset(); // free the reply, so the test can end
    stopper.join();
    check(joined && replier.replying.load() > 0 && inTime,
          "disconnect() returns while the connection's own thread is blocked replying to a peer that does not read");
  }
  {
    // Found in review (round 28): a reconnect from inside connectionLost
    // ended the process. From the connection's own thread, start() assigned
    // a thread over the one still running it (std::terminate); from the
    // disconnect() that called connectionLost, that disconnect() then
    // released the NEW socket under the new thread (an access violation).
    StreamingSocket listener;
    listener.createListener(0);
    Reconnector keeper;
    keeper.port = listener.getBoundPort();
    std::unique_ptr<StreamingSocket> peer;
    std::thread acceptor([&] { peer = listener.waitForNextConnection(3000); });
    const bool joined = keeper.connectToSocket("127.0.0.1", keeper.port, 2000);
    acceptor.join();
    auto waitFor = [](const std::atomic<int>& n, int value) {
      const auto t0 = Clock::now();
      while (n.load() < value && msSince(t0) < 3000) sleepMs(10);
      return n.load() >= value;
    };
    const bool madeOnce = waitFor(keeper.made, 1);
    peer.reset(); // the server drops it: connectionLost on the connection's thread
    const bool lostOnce = waitFor(keeper.lost, 1);
    sleepMs(200);
    const bool refusedThere = lostOnce && keeper.reconnected.load() == 0;
    check(joined && madeOnce && refusedThere,
          "a reconnect from connectionLost on the connection's own thread is refused, not a crash");

    std::thread acceptor2([&] { peer = listener.waitForNextConnection(3000); });
    const bool again = keeper.connectToSocket("127.0.0.1", keeper.port, 2000);
    acceptor2.join();
    const bool madeTwice = waitFor(keeper.made, 2);
    keeper.disconnect(); // connectionLost on this thread, inside disconnect()
    sleepMs(200);
    check(again && madeTwice && keeper.lost.load() == 2 && keeper.reconnected.load() == 0 && !keeper.isConnected(),
          "...and from connectionLost inside disconnect(); a reconnect from outside a callback still works");
  }
  {
    // Found in review (round 28): a server that takes one client and stops
    // listening calls stop() from createConnectionObject, on the server's
    // own thread -- which then joined itself: std::terminate.
    OneClientServer single;
    const bool listening = single.beginWaitingForSocket(0);
    const int port = single.getBoundPort();
    Collector first;
    const bool firstIn = first.connectToSocket("127.0.0.1", port, 2000) && first.waitMade(2000);
    const std::vector<uint8_t> hello = bytesOf("hello");
    const bool echoed = first.sendMessage(hello) && first.waitFor(1, 3000) && first.messages[0] == hello;
    sleepMs(100);
    StreamingSocket second;
    const bool secondIn = second.connect("127.0.0.1", port, 1000);
    check(listening && firstIn && echoed && single.created.load() == 1 && !secondIn,
          "stop() from createConnectionObject: the one client is served, and nobody after it");
  }

  // The same over a named pipe.
  const std::string name = "sonore-ipc-test-" + uuidToString(makeUuid());
  Collector pipeServer(true), pipeClient;
  check(pipeServer.createPipe(name, 2000) && pipeClient.connectToPipe(name, 2000) && pipeServer.waitMade(2000) &&
            pipeClient.waitMade(2000),
        "createPipe and connectToPipe: both report connectionMade");
  bool pipeSent = true;
  for (const auto& m : messages) pipeSent = pipeSent && pipeClient.sendMessage(m);
  const bool pipeAll = pipeClient.waitFor(messages.size(), 10000);
  bool pipeSame = pipeAll;
  if (pipeAll)
    for (size_t i = 0; i < messages.size(); ++i) pipeSame = pipeSame && pipeClient.messages[i] == messages[i];
  check(pipeSent && pipeSame, "...and echo the same six messages over the pipe");
  pipeClient.disconnect();
  check(pipeServer.waitLost(3000), "...and the server end sees the client go");

  {
    // Found in review (round 28): a pipe send that timed out after the peer
    // had taken the start of its frame -- in the same read as the message
    // before it, whose handler is slow -- returned false and the connection
    // went on. The peer read the next messages as the rest of that frame:
    // each reported sent, none delivered, and a megabyte nobody sent
    // delivered instead. (Windows reports such a write as nothing written.)
    const std::string slowName = "sonore-ipc-test-" + uuidToString(makeUuid());
    SlowHandler sender, receiver;
    const bool up = sender.createPipe(slowName, 1000) && receiver.connectToPipe(slowName, 2000);
    const auto t0 = Clock::now();
    while ((sender.made.load() == 0 || receiver.made.load() == 0) && msSince(t0) < 3000) sleepMs(10);
    sender.sendMessage(bytesOf("wait"));
    while (receiver.count() == 0 && msSince(t0) < 3000) sleepMs(5);
    sender.sendMessage(bytesOf("stall")); // read with the start of the next frame, then 2 s in its handler
    // A megabyte. Linux's and Windows' local buffers cannot take it while
    // the reader is busy, so the send is cut short, which is what this is
    // here to exercise. macOS's can: its first run saw the megabyte AND every
    // chunk after it accepted within the timeout, 24 messages delivered
    // whole. Sending more is not the answer: the run that sent 32 MB lost the
    // macOS runner ("lost communication with the server") for the rest of its
    // job, which is what kernel buffers shared with the network running dry
    // would look like. So both outcomes are checked, each for what it must
    // guarantee, and the log says which one this machine produced.
    const std::vector<uint8_t> big(1 << 20, 'B'), later = bytesOf("later"), chunk(64 * 1024, 'z');
    const bool bigSent = sender.sendMessage(big); // times out after 1 s, part of it taken
    const bool laterSent = sender.sendMessage(later);
    int chunksSent = 0;
    for (int i = 0; i < 20; ++i) chunksSent += sender.sendMessage(chunk) ? 1 : 0;
    const auto t1 = Clock::now();
    while ((sender.lost.load() == 0 || receiver.lost.load() == 0) && msSince(t1) < 5000) sleepMs(20);
    std::lock_guard<std::mutex> lock(receiver.mutex);
    bool onlySent = true, laterArrived = false;
    int chunksArrived = 0;
    for (const auto& m : receiver.messages) {
      onlySent = onlySent && (textOf(m) == "wait" || textOf(m) == "stall" || m == later || m == chunk || m == big);
      laterArrived = laterArrived || m == later;
      chunksArrived += m == chunk ? 1 : 0;
    }
    // The numbers that decide both lines, printed with them: the first macOS
    // run failed both with nothing to say whether the pipe came up, whether
    // the megabyte fitted into the socket's buffers whole, or which end
    // never saw the connection go.
    char state[256];
    std::snprintf(state, sizeof(state),
                  " (up %d, big sent %d, later sent %d, chunks sent %d, lost %d/%d, received %zu, only sent %d)",
                  up ? 1 : 0, bigSent ? 1 : 0, laterSent ? 1 : 0, chunksSent, sender.lost.load(),
                  receiver.lost.load(), receiver.messages.size(), onlySent ? 1 : 0);
    if (!bigSent) {
      check(up && onlySent && (!laterSent || laterArrived),
            (std::string("a pipe send cut short: nothing is delivered that was not sent, nothing reported sent goes missing") + state).c_str());
      check(!laterSent && chunksSent == 0 && sender.lost.load() == 1 && receiver.lost.load() == 1,
            (std::string("...because it ends the connection, at both ends") + state).c_str());
    } else {
      // Nothing was cut short, so nothing may be lost either: every message
      // reported sent arrived whole, nothing arrived that was not sent, and
      // the connection is still up at both ends.
      check(up && onlySent && laterSent && laterArrived && chunksSent == 20 && chunksArrived == 20 &&
                sender.lost.load() == 0 && receiver.lost.load() == 0,
            (std::string("a pipe send the buffers took whole: everything reported sent arrived, whole, and nothing else") + state).c_str());
      std::printf("  ---- the local socket's buffers took the whole megabyte here, so a send cut short was not "
                  "reached; that path is checked on Windows and Linux ----\n");
    }
  }
}

struct TestCoordinator : ChildProcessCoordinator {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::string> received;
  int lostCount = 0;
  bool killWhenLost = false;
  ~TestCoordinator() override { killWorkerProcess(); }
  void handleMessageFromWorker(const std::vector<uint8_t>& message) override {
    std::lock_guard<std::mutex> lock(mutex);
    received.push_back(textOf(message));
    cv.notify_all();
  }
  void handleConnectionLost() override {
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++lostCount;
    }
    cv.notify_all();
    if (killWhenLost) killWorkerProcess(); // from the connection's own thread
  }
  bool waitReceived(size_t n, int ms) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return received.size() >= n; });
  }
  bool waitLost(int ms) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return lostCount > 0; });
  }
};

void testCoordinator() {
  section("ChildProcessCoordinator / ChildProcessWorker");
  const std::string uid = "sonore-system-test";
  {
    TestCoordinator c;
    const auto t0 = Clock::now();
    check(c.launchWorkerProcess(g_self, uid, 5000, 0, {"--child", "worker", uid}),
          "a worker (this program, relaunched) is launched and connects");
    std::printf("    connected in %lld ms\n", msSince(t0));
    for (int i = 0; i < 5; ++i) c.sendMessageToWorker(bytesOf("m" + std::to_string(i)));
    const bool got = c.waitReceived(6, 5000);
    bool inOrder = got && c.received[0] == "hello from worker";
    for (int i = 0; got && i < 5; ++i) inOrder = inOrder && c.received[(size_t) i + 1] == "echo:m" + std::to_string(i);
    check(inOrder, "messages both ways, in order (the worker speaks first)");
    ChildProcess* worker = c.getWorkerProcess();
    c.killWorkerProcess();
    check(c.lostCount == 0 && c.getWorkerProcess() == nullptr && worker != nullptr,
          "killWorkerProcess ends it without calling handleConnectionLost");
  }
  {
    TestCoordinator c;
    c.launchWorkerProcess(g_self, uid, 5000, 0, {"--child", "worker", uid});
    c.waitReceived(1, 3000);
    const auto t0 = Clock::now();
    c.sendMessageToWorker(bytesOf("crash"));
    const bool lost = c.waitLost(5000);
    const long long took = msSince(t0);
    ChildProcess* p = c.getWorkerProcess();
    const bool exited = p && p->waitForProcessToFinish(5000);
    std::printf("    crash noticed after %lld ms\n", took);
    check(lost && took < 2000 && exited && p->getExitCode() == 3,
          "a worker that crashes is noticed at once (the pipe ends), its exit code readable");
  }
  {
    TestCoordinator c;
    const bool launched = c.launchWorkerProcess(g_self, uid, 800, 0, {"--child", "silent-worker", uid});
    const auto t0 = Clock::now();
    const bool lost = c.waitLost(5000);
    std::printf("    silent worker dropped after %lld ms (timeout 800)\n", msSince(t0));
    check(launched && lost && msSince(t0) < 3000, "a worker that connects and then says nothing is dropped by the watchdog");
  }
  {
    TestCoordinator c;
    c.killWhenLost = true;
    c.launchWorkerProcess(g_self, uid, 5000, 0, {"--child", "worker", uid});
    c.waitReceived(1, 3000);
    c.sendMessageToWorker(bytesOf("crash"));
    check(c.waitLost(5000), "killWorkerProcess called from inside handleConnectionLost is safe");
    sleepMs(100);
  }
  {
    TestCoordinator c;
    const auto t0 = Clock::now();
    const bool launched = c.launchWorkerProcess(g_self, uid, 800, 0, {"--child", "sleep"});
    check(!launched && msSince(t0) < 3000 && c.getWorkerProcess() == nullptr,
          "a program that never connects: launch fails after the timeout, nothing left running");
  }
}

void testServiceDiscovery() {
  section("NetworkServiceDiscovery");
  DatagramSocket probe;
  probe.bindToPort(0);
  const int port = probe.getBoundPort();
  probe.shutdown();
  NetworkServiceDiscovery::AvailableServiceList list("com.sonore.test", port, "127.0.0.1", 600);
  std::atomic<int> changes{0};
  list.setChangeCallback([&] { ++changes; });
  check(list.isListening(), "a service list listens (here on the loopback, so no firewall is asked)");
  std::string id;
  {
    NetworkServiceDiscovery::Advertiser ad("com.sonore.test", "Studio A", port, 4242, 100, IPAddress::local());
    NetworkServiceDiscovery::Advertiser otherType("com.sonore.other", "Not ours", port, 1, 100, IPAddress::local());
    id = ad.getInstanceID();
    const auto t0 = Clock::now();
    while (list.getServices().empty() && msSince(t0) < 3000) sleepMs(10);
    const auto services = list.getServices();
    check(services.size() == 1 && services[0].instanceID == id && services[0].description == "Studio A" &&
              services[0].port == 4242 && services[0].address == IPAddress::local() && changes.load() >= 1,
          "an advertiser is found: its id, name, port and the address it came from; another type is ignored");
  }
  const auto t1 = Clock::now();
  while (!list.getServices().empty() && msSince(t1) < 4000) sleepMs(20);
  check(list.getServices().empty() && msSince(t1) < 3000, "...and expires once it stops advertising");
  std::vector<uint8_t> forged = bytesOf("SONORE-NSD 1\ntype=com.sonore.test\nid=x\nport=99999\n");
  ipcdetail::Advertisement ad;
  check(!ipcdetail::parseAdvertisement(forged.data(), forged.size(), ad), "an advertisement with a port out of range is refused");
  // Found by fuzz_system: a carriage return inside an id was accepted, which
  // no advertiser could announce (the encoder turns it into a space).
  std::vector<uint8_t> carriage = bytesOf("SONORE-NSD 1\ntype=com.sonore.test\nid=a\rb\nport=1\n");
  const std::string encoded = ipcdetail::encodeAdvertisement("t\x01", "a\rb", 1, "x\x7f");
  check(!ipcdetail::parseAdvertisement(carriage.data(), carriage.size(), ad) &&
            ipcdetail::parseAdvertisement((const uint8_t*) encoded.data(), encoded.size(), ad) && ad.instanceID == "a b" &&
            ad.type == "t " && ad.description == "x ",
        "control characters: refused when received, turned to spaces when sent");
}

} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args = ownArguments(argc, argv);
  for (size_t i = 0; i + 1 < args.size(); ++i)
    if (args[i] == "--child") return runChild(args[i + 1], std::vector<std::string>(args.begin() + (long) i + 2, args.end()), args);

  bool network = false;
  for (const std::string& a : args) network = network || a == "--network";
  g_self = currentExecutablePath();

  const std::string dir = joinPath(temporaryDirectory(), "sonore-system-test-" + uuidToString(makeUuid()).substr(0, 8));
  pathio::makeDirectory(dir.c_str());

  testStreams();
  testFiles(dir);
  testAddresses();
  testUrls();
  testSockets();
  if (network) testMulticastDelivery();
  testHttp(dir);
  testRealHttps();
  testProcesses();
  testPipes();
  testConnections();
  testCoordinator();
  testServiceDiscovery();

  std::error_code ec;
  std::filesystem::remove_all(u8(dir), ec);
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  std::printf("%s\n", g_failures ? "SYSTEM TESTS FAILED" : "SYSTEM TESTS PASSED");
  return g_failures ? 1 : 0;
}
