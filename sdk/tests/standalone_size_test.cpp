// SPDX-License-Identifier: Apache-2.0
// Does the standalone window actually refuse to be made unusably small?
//
// The arithmetic is unit-tested in sdk_tests (applyEditorConstraints). What is
// NOT testable there is the Win32 half: WM_GETMINMAXINFO arriving before the
// first WM_SIZE, ptMinTrackSize being in WINDOW pixels rather than client
// pixels, and AdjustWindowRect converting between them against the style the
// window was actually created with. Get any of those wrong and the arithmetic
// is perfect and the window still collapses.
//
// So this drives the BUILT executable the way a user does: launch it, find its
// window, drag it smaller than it is allowed to be, and measure what Windows
// let happen. A resize is refused by the window procedure, not by the caller,
// which is why asking SetWindowPos for 50x50 and reading the result back is
// the whole test.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>

static int failures = 0;

static void check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: standalone_size_test <path-to-app.exe> [minWidth] [minHeight]\n");
    return 2;
  }
  // The plugin's declared minimum, in CLIENT pixels. Passed in rather than
  // read from the descriptor: this test is a host, and a host knows only what
  // it was told.
  const int wantW = argc > 2 ? std::atoi(argv[2]) : 320;
  const int wantH = argc > 3 ? std::atoi(argv[3]) : 200;

  std::printf("\n[standalone window minimum]\n");
  std::printf("  ---- driving %s, expecting a %dx%d client minimum ----\n", argv[1], wantW,
              wantH);

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  char cmd[1024];
  std::snprintf(cmd, sizeof(cmd), "\"%s\"", argv[1]);
  if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    std::printf("  FAIL could not launch the app (error %lu)\n", GetLastError());
    return 1;
  }

  // Its own class name, so this cannot accidentally find some other window.
  HWND hwnd = nullptr;
  for (int i = 0; i < 200 && !hwnd; ++i) {
    hwnd = FindWindowW(L"SonoreStandalone", nullptr);
    if (!hwnd) Sleep(50);
  }
  check(hwnd != nullptr, "the standalone opened a window");

  if (hwnd) {
    RECT before{};
    GetWindowRect(hwnd, &before);
    std::printf("  ---- opened at %ldx%ld (window pixels) ----\n", before.right - before.left,
                before.bottom - before.top);

    // What the minimum SHOULD be once the frame is added to the client area.
    const DWORD style = (DWORD) GetWindowLongPtrW(hwnd, GWL_STYLE);
    RECT frame{0, 0, (LONG) wantW, (LONG) wantH};
    AdjustWindowRect(&frame, style, FALSE);
    const LONG expectW = frame.right - frame.left, expectH = frame.bottom - frame.top;

    // Ask for something absurd. WM_GETMINMAXINFO is what stops it, and it is
    // the window procedure that answers -- so the size that lands is the real
    // verdict rather than anything this test computed.
    SetWindowPos(hwnd, nullptr, 0, 0, 50, 50, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Sleep(120);
    RECT tiny{};
    GetWindowRect(hwnd, &tiny);
    const LONG gotW = tiny.right - tiny.left, gotH = tiny.bottom - tiny.top;

    char msg[240];
    std::snprintf(msg, sizeof(msg),
                  "asked for 50x50 and got %ldx%ld, against the %ldx%ld the declared minimum "
                  "works out to", gotW, gotH, expectW, expectH);
    // Exact rather than "at least": a window larger than the minimum after
    // being asked to shrink means something ELSE refused the resize, which
    // would pass a >= assertion while the limit did nothing.
    check(gotW == expectW && gotH == expectH, msg);

    // And the other direction: a size INSIDE the range is not interfered with,
    // or a minimum that pinned the window would also pass the test above.
    const int roomyW = wantW + 180, roomyH = wantH + 120;
    RECT roomy{0, 0, (LONG) roomyW, (LONG) roomyH};
    AdjustWindowRect(&roomy, style, FALSE);
    SetWindowPos(hwnd, nullptr, 0, 0, roomy.right - roomy.left, roomy.bottom - roomy.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Sleep(120);
    RECT after{};
    GetWindowRect(hwnd, &after);
    std::snprintf(msg, sizeof(msg), "a %dx%d request inside the range lands untouched at %ldx%ld",
                  roomyW, roomyH, after.right - after.left, after.bottom - after.top);
    check(after.right - after.left == roomy.right - roomy.left &&
              after.bottom - after.top == roomy.bottom - roomy.top, msg);
  }

  PostMessageW(hwnd, WM_CLOSE, 0, 0);
  if (WaitForSingleObject(pi.hProcess, 3000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 0);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  std::printf("%s\n", failures == 0 ? "SONORE STANDALONE SIZE TEST PASSED"
                                    : "SONORE STANDALONE SIZE TEST FAILED");
  return failures == 0 ? 0 : 1;
}
