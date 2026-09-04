// SPDX-License-Identifier: Apache-2.0
// What this machine's ASIO drivers say about themselves.
//
//   asio_probe                enumerate only, which is safe everywhere
//   asio_probe --open         ...and open each one, OUT OF PROCESS
//   asio_probe --open --input ...asking each for its INPUT channels too
//   asio_probe --open-one X   open exactly one, in this process
//
// Enumeration is the default because opening an ASIO driver runs its code
// inside this process, and a driver whose hardware is absent may take the
// process with it. On the machine this was written on, two of five do exactly
// that: one crashes when it is instantiated and another inside init().
//
// So --open does what a DAW does and spawns a CHILD for each driver. A
// crashing driver kills the child, the parent says which one it was, and the
// scan carries on. That is not a workaround -- it is the only way to survey
// hardware drivers you did not write, and it is why REAPER scans plugins the
// same way.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>

#include <sonore/audio_asio.h>
#include <sonore/special.h>

template <typename... A>
static void say(const char* fmt, A... a) {
  // With no arguments the "format" is plain text, and clang-cl rightly
  // refuses to pass a non-literal to printf without any.
  if constexpr (sizeof...(A) == 0) std::fputs(fmt, stdout);
  else std::printf(fmt, a...);
  // Flushed every line on purpose: a crash with buffered output loses exactly
  // the line that would have said which driver crashed.
  std::fflush(stdout);
}

int main(int argc, char** argv) {
#if defined(_WIN32)
  bool open = false;
  const char* openOne = nullptr;
  // So the input side can be turned off and the SAME measurement taken again.
  // A block count is only meaningful against another block count.
  bool noInput = true;
  // OFF by default, and the reason is measured rather than cautious: asking
  // Realtek ASIO for input drops its callback rate from 27 blocks in 250ms to
  // 3, an eight-fold starvation of the OUTPUT that had been working. A survey
  // whose default made every driver look broken would be a worse survey.
  bool withInput = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--open") == 0) open = true;
    else if (std::strcmp(argv[i], "--input") == 0) withInput = true;
    else if (std::strcmp(argv[i], "--open-one") == 0 && i + 1 < argc) openOne = argv[++i];
    else if (std::strcmp(argv[i], "--input-too") == 0) noInput = false;
  }

  // The child. Everything dangerous happens here, in a process whose death
  // costs nothing.
  if (openOne) {
    sonore::asio::AsioOutput out;
    double phase = 0.0;
    // What the input side actually delivered. A driver can open its inputs
    // and hand back nothing at all -- a disconnected microphone is silence
    // and so is a conversion that reads the wrong format -- so the two are
    // told apart by asking whether ANY sample moved, over a quarter second.
    std::atomic<int> blocks{0};
    std::atomic<int> nonSilentBlocks{0};
    const bool started =
        out.start(openOne, [&](const float* in, float* io, uint32_t frames,
                               uint32_t channels) {
          ++blocks;
          if (in) {
            for (uint32_t i = 0; i < frames * channels; ++i)
              if (in[i] != 0.0f) {
                ++nonSilentBlocks;
                break;
              }
          }
          const double step = 440.0 / out.sampleRate();
          for (uint32_t i = 0; i < frames; ++i) {
            // Quiet on purpose. This makes real sound on somebody's speakers,
            // and a test that is loud is a test people disable.
            const float v = 0.02f * sonore::fastmath::sinTurns((float) phase);
            phase += step;
            if (phase >= 1.0) phase -= 1.0;
            for (uint32_t c = 0; c < channels; ++c) io[i * channels + c] = v;
          }
        }, /*withInput=*/!noInput);
    if (!started) {
      say("  %-32s %s\n", openOne, out.error().c_str());
      return 0;
    }
    say("  %-32s OPEN at %.0f Hz, %u frames\n", openOne, out.sampleRate(), out.bufferFrames());
    Sleep(250);
    out.stop();
    say("  %-32s formats out=%ld in=%ld\n", openOne, out.format(), out.inputFormat());
    say("  %-32s input=%s, %d block(s), %d carrying signal\n", openOne,
        out.hasInput() ? "opened" : (noInput ? "not asked for" : "none"), blocks.load(),
        nonSilentBlocks.load());
    say("  %-32s closed cleanly\n", openOne);
    return 0;
  }

  const auto drivers = sonore::asio::listDrivers();
  say("%zu ASIO driver(s) installed\n", drivers.size());
  for (const auto& d : drivers) say("  %-32s %s\n", d.name.c_str(), d.clsid.c_str());

  if (drivers.empty()) {
    // A correct answer, and the one every CI runner gives. A test that failed
    // here would fail on every machine without an audio interface.
    say("\nno ASIO drivers on this machine, which is not a failure\n");
    return 0;
  }

  if (!open) {
    say("\nenumerated only. --open instantiates them, which can crash on a\n");
    say("driver whose hardware is absent -- that is the driver's behaviour, and\n");
    say("the reason a host opens the one a user picked rather than all of them.\n");
    return 0;
  }

  // One child per driver. The parent never touches a driver at all, so a
  // driver that crashes takes a process that was expendable.
  for (const auto& d : drivers) {
    char command[1024];
    std::snprintf(command, sizeof(command), "\"%s\" --open-one \"%s\"%s", argv[0],
                  d.name.c_str(), withInput ? " --input-too" : "");

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, command, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si,
                        &pi)) {
      say("  %-32s could not be probed (CreateProcess failed)\n", d.name.c_str());
      continue;
    }
    // Bounded: a driver that hangs is as unhelpful as one that crashes, and
    // the answer for both is to stop waiting and say so.
    const DWORD waited = WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 0;
    if (waited == WAIT_TIMEOUT) {
      TerminateProcess(pi.hProcess, 1);
      say("  %-32s HUNG -- gave up after 15 seconds\n", d.name.c_str());
    } else {
      GetExitCodeProcess(pi.hProcess, &code);
      if (code != 0)
        say("  %-32s CRASHED in its own process (code 0x%08lx)\n", d.name.c_str(),
            (unsigned long) code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
#else
  (void) argc;
  (void) argv;
  say("ASIO is a Windows API; there is nothing to enumerate here.\n");
#endif
  return 0;
}
