// SPDX-License-Identifier: Apache-2.0
// Verify the Sonore SDK end to end: configure, build both example plugins,
// run the DSP unit tests, drive the BUILT .clap binaries as a host, and (when
// emscripten is available) compile the same DSP to wasm and run it.
//
//   npm run verify:sdk
//
// The wasm half is skipped with a loud note when em++ isn't on PATH, so the
// suite still runs on a machine without the emsdk, but never silently, because
// "preview matches the product" is the property that half proves.
import { execFileSync, spawnSync } from "node:child_process";
import { cpSync, existsSync, mkdirSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const sdk = join(root, "sdk");
const build = join(sdk, "build");

const log = (s) => process.stdout.write(s + "\n");
let failed = false;
/** Which legs actually failed. Kept because "SDK VERIFY FAILED" on its own, at
 *  the bottom of four thousand lines of validator output every one of which
 *  says SUCCESS, is a message that costs half an hour to act on. */
const failures = [];

function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { stdio: "inherit", shell: true, ...opts });
  if (r.status !== 0) {
    failed = true;
    // Enough of the command to tell one g++ invocation from another, without
    // printing a screenful of -I flags.
    const shown = [cmd, ...args].join(" ").replace(/\s+/g, " ").slice(0, 140);
    const how = r.status === null ? "signal " + r.signal : "exit " + r.status;
    failures.push(how + ": " + shown);
  }
  return r.status === 0;
}

log("── configure ─────────────────────────────────────────────────────────────");
// Let CMake pick the generator: the dev box has VS, the Linux builders have ninja/make.
if (!run("cmake", ["-S", `"${sdk}"`, "-B", `"${build}"`, "-DCMAKE_BUILD_TYPE=Release"])) {
  log("\nSDK VERIFY FAILED (configure)");
  process.exit(1);
}

log("\n── build ─────────────────────────────────────────────────────────────────");
if (!run("cmake", ["--build", `"${build}"`, "--config", "Release"])) {
  log("\nSDK VERIFY FAILED (build)");
  process.exit(1);
}

log("\n── tests (DSP units + CLAP host, effect and instrument) ──────────────────");
run("ctest", ["--test-dir", `"${build}"`, "-C", "Release", "--output-on-failure"]);

log("\n── feature map (what we claim against the reference, checked) ─────────────");
// Cheap, and it fails when a capability this project says it has stops being
// in the headers. The question it answers -- how far from the reference -- has been
// answered from memory here twice and was wrong both times.
run("node", [`"${join(root, "scripts", "feature-map.mjs")}"`]);
run("node", [`"${join(root, "scripts", "backend-surface.mjs")}"`]);
run("node", [`"${join(root, "scripts", "audio-thread-rules.mjs")}"`]);
// The map's other direction. feature-map checks that everything CLAIMED is
// still in the headers; this checks that everything in the headers is claimed.
// It was written after I set out to add a ProcessorChain and found one already
// in shaping.h, complete, with a ProcessorDuplicator beside it and neither
// mentioned in the map -- ten minutes from shipping a second one. It found 27
// more capabilities the map already had and had never named.
run("node", [`"${join(root, "scripts", "unclaimed.mjs")}"`]);
// The THIRD direction, and the one the other two structurally cannot see.
//
// feature-map checks the map against our headers; unclaimed checks our headers
// against the map. Both are closed loops between the map and our own code, so
// neither can notice a class the reference has that nobody here has ever thought about
// -- which meant "0 gaps" only ever meant "0 among the rows somebody wrote
// down", a caveat repeated in commit after commit and never mechanised.
//
// It was not hypothetical. Walking six gui_basics groups against the map
// by hand found thirty-six classes with no row at all, while the map was
// reporting one real gap. It could not have reported them: a class nobody has
// written down cannot be counted as missing.
//
// The bar is deliberately low -- the name must be DECIDED about, and "out of
// scope, because..." closes a name as well as building it does. What this
// refuses is silence.
run("node", [`"${join(root, "scripts", "reference-parity.mjs")}"`]);

// ── au_wrapper.h, read by a compiler ────────────────────────────────────────
//
// The AU wrapper is the one format nothing here can build; macOS CI builds and
// validates it on every push. This leg compiles it against a shim of Apple's
// declarations, so a typo is caught here in seconds instead of a CI round
// trip. It proves internal consistency, NOT that the ABI is right; see
// sdk/tests/au_shim/README.md. The first successful parse found a genuine
// error: the factory referring to a type declared inside a namespace it was
// not in, which is exactly the class of error a syntax check is for.
log("\n── au_wrapper.h, compiled against a shim of Apple\'s headers ──");
run("g++", [
  "-std=c++17", "-fsyntax-only", "-DSONORE_APPLE_SYNTAX_CHECK",
  `-I"${join(sdk, "include")}"`,
  `-I"${join(sdk, "third_party", "clap", "include")}"`,
  `-I"${join(sdk, "tests", "au_shim")}"`,
  // The dlfcn shim exists only where there is no real one. On Linux the
  // genuine header is used, because a shim shadowing a truth that was
  // available is the whole failure mode this directory has to avoid.
  ...(process.platform === "win32" ? [`-I"${join(sdk, "tests", "au_shim", "win")}"`] : []),
  `"${join(sdk, "tests", "au_syntax.cpp")}"`,
]);
log("  ok   it parses, and every name it uses exists");

// The macOS DEVICE backends, same technique. CoreAudio output and input and
// the CoreMIDI branch of midi_input.h are another ~600 lines nothing had
// compiled -- including HAL device enumeration and a SysEx path both written
// blind.
//
// The platform selection is forced: midi_input.h picks _WIN32 first, so the
// first version of this check compiled the WINDOWS backend and reported
// success for CoreMIDI code it had never read.
run("g++", [
  "-std=c++17", "-fsyntax-only", "-DSONORE_APPLE_SYNTAX_CHECK",
  `-I"${join(sdk, "include")}"`,
  `-I"${join(sdk, "tests", "au_shim")}"`,
  ...(process.platform === "win32" ? [`-I"${join(sdk, "tests", "au_shim", "win")}"`] : []),
  `"${join(sdk, "tests", "apple_syntax.cpp")}"`,
]);
log("  ok   CoreAudio and CoreMIDI parse too");

// The macOS UI files. webview_cocoa.h is what every Mac plugin's editor is
// made of today and window_cocoa.h is the native peer meant to replace it;
// neither had ever been read by a compiler either.
//
// au_view.h is NOT in this check: it needs clap_wrapper.h, which selects its
// webview backend and window API on _WIN32 first, so the chain would compile
// the Windows branches and report success for macOS code it never read. Named
// here rather than quietly counted -- see the note at the top of
// cocoa_syntax.cpp.
run("g++", [
  "-std=c++17", "-fsyntax-only", "-DSONORE_APPLE_SYNTAX_CHECK",
  `-I"${join(sdk, "include")}"`,
  `-I"${join(sdk, "tests", "au_shim")}"`,
  ...(process.platform === "win32" ? [`-I"${join(sdk, "tests", "au_shim", "win")}"`] : []),
  `"${join(sdk, "tests", "cocoa_syntax.cpp")}"`,
]);
log("  ok   the Cocoa peer and the WKWebView host parse (au_view.h still does not)");

// A rule no compiler reports until an unrelated include reorders the world:
// windows.h defines min and max as MACROS, so a header that includes it without
// NOMINMAX breaks whatever is included after it. gfx/viewport.h used std::max
// happily for months and stopped compiling the day plugin_editor.h gained one
// more include -- in a file nobody had touched, three headers away from the
// cause. Four of ours were unguarded; the check found the fourth after I had
// fixed three by hand and believed I was done.
log("\n── headers that reach for windows.h disarm it first ──");
run("node", [`"${join(root, "scripts", "header-rules.mjs")}"`]);

// The same shape of rule one level up: files.h added sonore::detail::widen
// where file_dialog.h already had one. Two inline definitions of a symbol is
// an ODR violation that only fails to compile when both headers reach one
// translation unit -- otherwise the linker picks one and the other caller
// quietly gets the wrong function. Whether it breaks is a property of the
// CONSUMER's include list, which is not something this repository can see.
log("\n── no two headers define the same symbol ──");
run("node", [`"${join(root, "scripts", "namespace-rules.mjs")}"`]);

log("\n── clang-cl (LLVM's MSVC mode -- the compiler the Windows build farm uses) ──");
// The marketplace's Windows binaries are cross-built with clang-cl + lld-link,
// not MSVC, so the suite is built and run with that compiler too. Portable
// LLVM in C:\\dev\\llvm (or SONORE_LLVM_DIR, or
// on PATH), Ninja in C:\\dev\\ninja (or SONORE_NINJA, or on PATH), and the
// MSVC headers and libraries through vcvars64 of the newest Visual Studio.
// Skips loudly naming what is missing; SONORE_SKIP_CLANGCL=1 skips it on
// purpose. The tests run against the clang-cl build too -- a compiler that
// compiles everything and miscompiles one thing is the one to be afraid of.
if (process.platform !== "win32") {
  log("  SKIPPED: a Windows-only leg");
} else if (process.env.SONORE_SKIP_CLANGCL) {
  log("  SKIPPED: SONORE_SKIP_CLANGCL is set");
} else {
  const whereFirst = (name) => {
    const r = spawnSync("where", [name], { stdio: "pipe" });
    const line = (r.stdout || "").toString().split(/\r?\n/).find((l) => l.trim());
    return line ? line.trim() : null;
  };
  const findClangCl = () => {
    for (const dir of [process.env.SONORE_LLVM_DIR, "C:\\dev\\llvm"].filter(Boolean)) {
      if (existsSync(join(dir, "bin", "clang-cl.exe"))) return join(dir, "bin");
      if (existsSync(dir))
        for (const sub of readdirSync(dir))
          if (existsSync(join(dir, sub, "bin", "clang-cl.exe"))) return join(dir, sub, "bin");
    }
    const onPath = whereFirst("clang-cl");
    return onPath ? dirname(onPath) : null;
  };
  const findNinja = () => {
    for (const p of [process.env.SONORE_NINJA, "C:\\dev\\ninja\\ninja.exe"].filter(Boolean))
      if (existsSync(p)) return dirname(p);
    const onPath = whereFirst("ninja");
    return onPath ? dirname(onPath) : null;
  };
  const vswhere = "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
  const vsPath = existsSync(vswhere)
    ? (spawnSync(vswhere, ["-latest", "-property", "installationPath"], { stdio: "pipe" }).stdout || "").toString().trim()
    : "";
  const vcvars = vsPath ? join(vsPath, "VC", "Auxiliary", "Build", "vcvars64.bat") : "";
  const llvmBin = findClangCl();
  const ninjaDir = findNinja();
  const missing = [];
  if (!llvmBin) missing.push("clang-cl (portable LLVM in C:\\dev\\llvm, SONORE_LLVM_DIR, or PATH)");
  if (!ninjaDir) missing.push("ninja (C:\\dev\\ninja\\ninja.exe, SONORE_NINJA, or PATH)");
  if (!vcvars || !existsSync(vcvars)) missing.push("vcvars64.bat (a Visual Studio with the C++ workload)");
  if (missing.length) {
    log("  SKIPPED: missing: " + missing.join("; "));
  } else {
    const buildClang = join(sdk, "build-clangcl");
    const cmd = join(sdk, "build", "clangcl-leg.cmd");
    mkdirSync(join(sdk, "build"), { recursive: true });
    writeFileSync(cmd, [
      "@echo off",
      `call "${vcvars}" >nul`,
      `set PATH=${llvmBin};${ninjaDir};%PATH%`,
      `cd /d "${sdk}"`,
      `cmake -S . -B "${buildClang}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link -DCMAKE_MT=llvm-mt`,
      "if errorlevel 1 exit /b 1",
      `cmake --build "${buildClang}"`,
      "",
    ].join("\r\n"), "utf8");
    log(`  clang-cl from ${llvmBin}`);
    if (run("cmd", ["/c", `"${cmd}"`])) {
      log("  ok   every target builds with clang-cl + lld-link");
      if (run("ctest", ["--test-dir", `"${buildClang}"`, "--output-on-failure"]))
        log("  ok   and the whole suite passes against that build");
    }
  }
}

log("\n── wasm (the same DSP in the browser ABI) ────────────────────────────────");
let haveEmcc = false;
try {
  execFileSync("em++", ["--version"], { stdio: "pipe", shell: true });
  haveEmcc = true;
} catch {
  haveEmcc = false;
}
if (haveEmcc) {
  run("node", [`"${join(sdk, "tests", "wasm_build_test.mjs")}"`]);
} else {
  log("  SKIPPED: em++ is not on PATH.");
  log("  This half proves the preview and the shipped binary run the SAME DSP.");
  log("  On the dev box: $env:PATH=\"C:\\dev\\emsdk;C:\\dev\\emsdk\\upstream\\emscripten;$env:PATH\"");
}

// ── The other platform ───────────────────────────────────────────────────────
// Building on ONE OS hides real bugs: MSVC leaks <cstddef>/<cstdlib> through
// other headers where clang and gcc do not, and a Windows-only build once let
// `nativeWindowApi()` return "win32" on Linux, which would have meant no
// exported Linux plugin ever showed a UI. So when a Linux toolchain is reachable
// (WSL on the dev box, native everywhere else) the SDK is built and tested there
// too. The Linux backend dlopens GTK at runtime precisely so this needs no
// GTK dev packages.
if (process.platform === "win32") {
  log("\n── linux cross-check (WSL) ───────────────────────────────────────────────");
  // No shell: the command has to reach bash as ONE argument, and cmd's quoting
  // would split it.
  const probe = spawnSync("wsl", ["-d", "Ubuntu", "--", "bash", "-lc", "g++ --version"], {
    stdio: "pipe",
  });
  if (probe.status !== 0) {
    log("  SKIPPED: no WSL Ubuntu with g++ available.");
  } else {
    const sdkPath = "/mnt/" + sdk.replace(/^([A-Za-z]):/, (_, d) => d.toLowerCase()).replace(/\\/g, "/");
    // No shell variables and no quotes: wsl.exe re-parses the command line it
    // is given, and either one silently arrives empty.
    const inc = `-I${sdkPath}/include -I${sdkPath}/third_party/clap/include`;
    const vstInc = `-I${sdkPath}/third_party/vst3`;
    const lv2Inc = `-I${sdkPath}/third_party/lv2/include`;
    const out = "~/sonolinux";

    // The X11 peer needs <X11/Xlib.h> to COMPILE, though nothing links libX11 --
    // it is opened at runtime, so a plugin still loads on a Wayland-only box.
    //
    // Two places are checked. The system prefix is where libx11-dev puts it. The
    // second is for a machine with no root: `apt-get download libx11-dev
    // x11proto-dev libxcb1-dev libxau-dev libxdmcp-dev` followed by `dpkg-deb -x`
    // into ~/xdev/root needs no privileges at all, and is how this leg runs here.
    const x11Probe = spawnSync(
      "wsl", ["-d", "Ubuntu", "--", "bash", "-lc",
              "if [ -f /usr/include/X11/Xlib.h ]; then echo /usr; " +
              "elif [ -f $HOME/xdev/root/usr/include/X11/Xlib.h ]; then echo $HOME/xdev/root/usr; " +
              "else echo none; fi"],
      { stdio: "pipe" });
    const x11Prefix = (x11Probe.stdout || "").toString().trim();
    const haveX11 = x11Prefix !== "" && x11Prefix !== "none";
    const x11Inc = haveX11 && x11Prefix !== "/usr" ? `-I${x11Prefix}/include` : "";
    const x11Lib = haveX11 && x11Prefix !== "/usr" ? `-L${x11Prefix}/lib/x86_64-linux-gnu` : "";
    const x11Run = haveX11 && x11Prefix !== "/usr"
      ? `LD_LIBRARY_PATH=${x11Prefix}/lib/x86_64-linux-gnu ` : "";

    // JACK needs NO header to compile -- the backend dlopens every symbol, so
    // the test builds on a box that has never heard of JACK and decides at
    // runtime. What it needs is libjack present, and a server running.
    //
    // Same no-root trick as X11: `apt-get download jackd2 libjack-jackd2-0
    // libopus0 libsamplerate0 libdb5.3t64` and `dpkg-deb -x` into ~/jackdev/root
    // gives a real server and a real client library without touching the system.
    const jackProbe = spawnSync(
      "wsl", ["-d", "Ubuntu", "--", "bash", "-lc",
              "if [ -f $HOME/jackdev/root/usr/lib/x86_64-linux-gnu/libjack.so.0 ]; then echo $HOME/jackdev/root/usr; " +
              "elif ldconfig -p 2>/dev/null | grep -q 'libjack.so.0'; then echo /usr; " +
              "else echo none; fi"],
      { stdio: "pipe" });
    const jackPrefix = (jackProbe.stdout || "").toString().trim();
    const haveJack = jackPrefix !== "" && jackPrefix !== "none";
    // Whether it is OUR unpacked copy, which is the only one this gate is
    // allowed to start a server from. See the leg below for why that matters.
    const jackLocal = haveJack && jackPrefix !== "/usr";
    const jackLib = `${jackPrefix}/lib/x86_64-linux-gnu`;
    const jackRun = jackLocal ? `LD_LIBRARY_PATH=${jackLib} ` : "";
    // The export list every Linux plugin links against (see
    // _sonore_plugin_exports in sdk/CMakeLists.txt): written here, on this
    // side, because the WSL command line below may carry no quotes.
    writeFileSync(join(sdk, "build", "sonore_exports.map"), "{ global: clap_entry; GetPluginFactory; ModuleEntry; ModuleExit; bundleEntry; bundleExit; lv2_descriptor; lv2ui_descriptor; lv2_lib_descriptor; local: *; };\n");
    const script = [
      // pipefail is not decoration. Every test below ends in `| tail -3`, and
      // without it a pipeline reports TAIL's status -- which is always zero.
      // `set -e` then has nothing to act on, so a failing clap_host_test, a
      // failing vst3_host_test, a failing lv2_host_test and a failing
      // sdk_tests all printed their failure and let the gate carry on. The
      // whole Linux leg has been unable to fail since it was written.
      "set -e -o pipefail",
      `mkdir -p ${out}`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -o ${out}/SonoreSaturator.clap ${sdkPath}/examples/saturator/plugin.cpp ${inc} -ldl`,
      // With the X11 headers where they are available, so the peer is compiled
      // rather than merely written. Without them the same source builds the
      // webview path and says why -- both are real shipping configurations.
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -o ${out}/SonoreSynth.clap ${sdkPath}/examples/synth/plugin.cpp ${inc} ${x11Inc} -ldl`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -o ${out}/SonoreDucker.clap ${sdkPath}/examples/ducker/plugin.cpp ${inc} -ldl`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -o ${out}/SonoreTrim.clap ${sdkPath}/examples/trim/plugin.cpp ${inc} -ldl`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -o ${out}/SonoreSplitter.clap ${sdkPath}/examples/splitter/plugin.cpp ${inc} -ldl`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -o ${out}/SonoreArp.clap ${sdkPath}/examples/arp/plugin.cpp ${inc} -ldl`,
      // The sampler and the reverb. ctest has run both since they existed and
      // this leg never did, which is how the synth's --expect-mpe went missing
      // here without anybody noticing: two lists of the same thing, one of
      // them maintained.
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -o ${out}/SonoreSampler.clap ${sdkPath}/examples/sampler/plugin.cpp ${inc} -ldl`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -o ${out}/SonoreReverb.clap ${sdkPath}/examples/reverb/plugin.cpp ${inc} -ldl`,
      // sdk_tests asserts our channel roles against VST3's speaker bits, so it
      // needs that header and the committed audio vectors.
      `g++ -std=c++17 -O2 -pthread -DSONORE_TEST_DATA_DIR='"${sdkPath}/tests/data"' -o ${out}/sdk_tests ${sdkPath}/tests/sdk_tests.cpp ${inc} ${vstInc}`,
      `g++ -std=c++17 -O2 -o ${out}/clap_host_test ${sdkPath}/tests/clap_host_test.cpp ${inc} -ldl`,
      // VST3 from the SAME sources, and a VST3 host to drive it.
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -DSONORE_BUILD_VST3 -o ${out}/SonoreSaturator.vst3 ${sdkPath}/examples/saturator/plugin.cpp ${inc} ${vstInc} -ldl`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -DSONORE_BUILD_VST3 -o ${out}/SonoreSynth.vst3 ${sdkPath}/examples/synth/plugin.cpp ${inc} ${vstInc} -ldl`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -DSONORE_BUILD_VST3 -o ${out}/SonoreDucker.vst3 ${sdkPath}/examples/ducker/plugin.cpp ${inc} ${vstInc} -ldl`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -DSONORE_BUILD_VST3 -o ${out}/SonoreTrim.vst3 ${sdkPath}/examples/trim/plugin.cpp ${inc} ${vstInc} -ldl`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -DSONORE_BUILD_VST3 -o ${out}/SonoreSplitter.vst3 ${sdkPath}/examples/splitter/plugin.cpp ${inc} ${vstInc} -ldl`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -DSONORE_BUILD_VST3 -o ${out}/SonoreArp.vst3 ${sdkPath}/examples/arp/plugin.cpp ${inc} ${vstInc} -ldl`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -DSONORE_BUILD_VST3 -o ${out}/SonoreSampler.vst3 ${sdkPath}/examples/sampler/plugin.cpp ${inc} ${vstInc} -ldl`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -DSONORE_BUILD_VST3 -o ${out}/SonoreReverb.vst3 ${sdkPath}/examples/reverb/plugin.cpp ${inc} ${vstInc} -ldl`,
      `g++ -std=c++17 -O2 -o ${out}/vst3_host_test ${sdkPath}/tests/vst3_host_test.cpp ${inc} ${vstInc} -ldl`,
      // The standalone, exercised through its offline modes (no device, no display).
      `g++ -std=c++17 -O2 -DSONORE_BUILD_STANDALONE -o ${out}/SaturatorApp ${sdkPath}/examples/saturator/plugin.cpp ${inc} -ldl -lpthread`,
      `g++ -std=c++17 -O2 -DSONORE_BUILD_STANDALONE -o ${out}/SynthApp ${sdkPath}/examples/synth/plugin.cpp ${inc} -ldl -lpthread`,
      // LV2: bundle + generated TTL + our host driving the built artifact.
      `mkdir -p ${out}/SatLV2.lv2 ${out}/SynthLV2.lv2`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -DSONORE_BUILD_LV2 -o ${out}/SatLV2.lv2/SatLV2.so ${sdkPath}/examples/saturator/plugin.cpp ${inc} ${lv2Inc} -ldl`,
      `g++ -std=c++17 -O2 -DSONORE_BUILD_LV2 -DSONORE_LV2_TTLGEN -o ${out}/sat_ttlgen ${sdkPath}/examples/saturator/plugin.cpp ${inc} ${lv2Inc} -ldl`,
      `${out}/sat_ttlgen ${out}/SatLV2.lv2 SatLV2.so`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -DSONORE_BUILD_LV2 -o ${out}/SynthLV2.lv2/SynthLV2.so ${sdkPath}/examples/synth/plugin.cpp ${inc} ${lv2Inc} -ldl`,
      `g++ -std=c++17 -O2 -DSONORE_BUILD_LV2 -DSONORE_LV2_TTLGEN -o ${out}/synth_ttlgen ${sdkPath}/examples/synth/plugin.cpp ${inc} ${lv2Inc} -ldl`,
      `${out}/synth_ttlgen ${out}/SynthLV2.lv2 SynthLV2.so`,
      `mkdir -p ${out}/DuckerLV2.lv2`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -DSONORE_BUILD_LV2 -o ${out}/DuckerLV2.lv2/DuckerLV2.so ${sdkPath}/examples/ducker/plugin.cpp ${inc} ${lv2Inc} -ldl`,
      `g++ -std=c++17 -O2 -DSONORE_BUILD_LV2 -DSONORE_LV2_TTLGEN -o ${out}/ducker_ttlgen ${sdkPath}/examples/ducker/plugin.cpp ${inc} ${lv2Inc} -ldl`,
      `${out}/ducker_ttlgen ${out}/DuckerLV2.lv2 DuckerLV2.so`,
      `mkdir -p ${out}/SplitLV2.lv2`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -DSONORE_BUILD_LV2 -o ${out}/SplitLV2.lv2/SplitLV2.so ${sdkPath}/examples/splitter/plugin.cpp ${inc} ${lv2Inc} -ldl`,
      `g++ -std=c++17 -O2 -DSONORE_BUILD_LV2 -DSONORE_LV2_TTLGEN -o ${out}/split_ttlgen ${sdkPath}/examples/splitter/plugin.cpp ${inc} ${lv2Inc} -ldl`,
      `${out}/split_ttlgen ${out}/SplitLV2.lv2 SplitLV2.so`,
      `mkdir -p ${out}/ArpLV2.lv2`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -DSONORE_BUILD_LV2 -o ${out}/ArpLV2.lv2/ArpLV2.so ${sdkPath}/examples/arp/plugin.cpp ${inc} ${lv2Inc} -ldl`,
      `g++ -std=c++17 -O2 -DSONORE_BUILD_LV2 -DSONORE_LV2_TTLGEN -o ${out}/arp_ttlgen ${sdkPath}/examples/arp/plugin.cpp ${inc} ${lv2Inc} -ldl`,
      `${out}/arp_ttlgen ${out}/ArpLV2.lv2 ArpLV2.so`,
      // The GUI probe, built here for ONE reason: it is the only plugin that
      // uses the optional port properties, so it is the only bundle whose
      // turtle exercises them. A property the generator emits and no
      // validator ever reads is a property nobody has checked -- which is
      // how an undeclared prefix once made it into every bundle.
      `mkdir -p ${out}/ProbeLV2.lv2`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -o ${out}/SonoreGuiProbe.clap ${sdkPath}/examples/guiprobe/plugin.cpp ${inc} -ldl`,
      `g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=${sdkPath}/build/sonore_exports.map -DSONORE_BUILD_LV2 -o ${out}/ProbeLV2.lv2/ProbeLV2.so ${sdkPath}/examples/guiprobe/plugin.cpp ${inc} ${lv2Inc} -ldl`,
      `g++ -std=c++17 -O2 -DSONORE_BUILD_LV2 -DSONORE_LV2_TTLGEN -o ${out}/probe_ttlgen ${sdkPath}/examples/guiprobe/plugin.cpp ${inc} ${lv2Inc} -ldl`,
      `${out}/probe_ttlgen ${out}/ProbeLV2.lv2 ProbeLV2.so`,
      `grep -q "pprops:notOnGUI" ${out}/ProbeLV2.lv2/plugin.ttl && echo "  lv2: a hidden parameter says so in its port properties"`,
      `g++ -std=c++17 -O2 -o ${out}/lv2_host_test ${sdkPath}/tests/lv2_host_test.cpp ${inc} ${lv2Inc} -ldl`,
      // The export tables of everything just built: entry points and nothing else.
      `g++ -std=c++17 -O1 -o ${out}/exports_test ${sdkPath}/tests/exports_test.cpp`,
      `${out}/exports_test ${out} | tail -3`,
      `${out}/sdk_tests | tail -3`,
      // ── The same tests, under AddressSanitizer ──────────────────────────
      //
      // Added because it immediately found a use-after-free that had been in
      // MouseRouter since it was written: the router holds raw pointers to the
      // components it has focused, hovered and captured, and NOTHING told it
      // when one was destroyed. A plugin removing a focused control left it
      // writing into freed memory on the next key press.
      //
      // The MSVC build never noticed -- a different allocator, and no such
      // check. That is the argument for this leg: the Windows build is the one
      // most people run, and it is the one least able to say whether the memory
      // is sound.
      //
      // -O1 rather than -O2, because ASan's reports are far more legible with
      // frame pointers and less inlining, and this leg exists to be READ when
      // it fails.
      `g++ -std=c++17 -O1 -g -fsanitize=address -pthread -DSONORE_TEST_DATA_DIR='"${sdkPath}/tests/data"' -o ${out}/sdk_tests_asan ${sdkPath}/tests/sdk_tests.cpp ${inc} ${vstInc}`,
      `${out}/sdk_tests_asan | tail -2`,
      // The X11 peer, against whatever display this box has. It skips loudly
      // rather than failing where there is none -- a headless builder is a
      // legitimate place to build a plugin, and a test that failed there would
      // fail on every CI runner and teach people to ignore it.
      ...(haveX11
            ? [`g++ -std=c++17 -O2 ${x11Inc} -o ${out}/x11_window_test ${sdkPath}/tests/x11_window_test.cpp ${inc} ${x11Lib} -lX11 -ldl`,
               `${x11Run}${out}/x11_window_test | tail -4`]
            : ['echo "  x11: SKIPPED -- no <X11/Xlib.h>. apt-get install libx11-dev, or apt-get download it and dpkg-deb -x into ~/xdev/root"']),
      // ── JACK, against a real server ──────────────────────────────────────
      //
      // The label matters most on a backend like this one, because
      // JACK inverts the shape of every other output we have: there is no
      // render loop and no thread of ours -- the server calls US, on a thread
      // it created, and being late xruns every client in the graph. Nothing
      // about that is exercised by compiling it.
      //
      // The server is started ONLY from an unpacked-in-home copy, and only in
      // dummy mode. On a machine with a system JACK this gate connects to
      // whatever is already running and starts nothing: launching a server
      // there would reconfigure the audio of a box somebody is using, which is
      // the same reason the backend itself passes JackNoStartServer.
      ...(haveJack
            ? [`g++ -std=c++17 -O2 -o ${out}/jack_test ${sdkPath}/tests/jack_test.cpp ${inc} -ldl -lpthread`,
               // The trap fires however the script ends, including the failure
               // paths -- a gate that left a stray audio server behind on every
               // red run would be worse than no gate.
               ...(jackLocal
                     ? [// A stale marker from a run that was killed before its trap
                        // could fire would make THIS run kill a server it did
                        // not start. Cleared first, so the marker only ever
                        // means "the server below is ours".
                        "rm -f /tmp/sonore-jackd-ours",
                        // `|| true` is load-bearing: set -e is on, an EXIT trap
                        // runs under it, and a pkill that matches nothing exits
                        // 1 -- which would turn every green run red at the very
                        // last moment, from the cleanup.
                        "trap 'if [ -f /tmp/sonore-jackd-ours ]; then pkill -x jackd || true; rm -f /tmp/sonore-jackd-ours; fi' EXIT",
                        `if ! pgrep -x jackd > /dev/null 2>&1; then ` +
                          `LD_LIBRARY_PATH=${jackLib}:${jackLib}/jack ` +
                          `JACK_DRIVER_DIR=${jackLib}/jack ` +
                          `nohup ${jackPrefix}/bin/jackd -d dummy -r 48000 -p 256 > /tmp/sonore-jackd.log 2>&1 & ` +
                          `touch /tmp/sonore-jackd-ours; sleep 2; ` +
                          `echo "  jack: started a dummy server from ${jackPrefix} for this run"; fi`]
                     : []),
               `${jackRun}${out}/jack_test | tail -4`]
            : ['echo "  jack: SKIPPED -- no libjack.so.0. apt-get install libjack-jackd2-dev, or apt-get download jackd2 libjack-jackd2-0 libopus0 libsamplerate0 libdb5.3t64 and dpkg-deb -x into ~/jackdev/root"']),
      `${out}/clap_host_test ${out}/SonoreSaturator.clap | tail -3`,
      `${out}/clap_host_test ${out}/SonoreSynth.clap | tail -3`,
      `${out}/clap_host_test ${out}/SonoreDucker.clap --expect-sidechain | tail -3`,
      `${out}/clap_host_test ${out}/SonoreTrim.clap --expect-channels 1 8 | tail -3`,
      `${out}/clap_host_test ${out}/SonoreSplitter.clap --expect-aux-outs 2 | tail -3`,
      `${out}/clap_host_test ${out}/SonoreArp.clap --expect-midi-out | tail -3`,
      `${out}/clap_host_test ${out}/SonoreSampler.clap | tail -3`,
      `${out}/clap_host_test ${out}/SonoreReverb.clap | tail -3`,
      // The probe declares 517x341..1200x800 -- odd numbers no default could
      // produce. Every wrapper used to answer 320x200..8192x8192 for every
      // plugin, so this is the run that can tell "the host was told what this
      // plugin declared" apart from "the host was told the SDK constant".
      `${out}/clap_host_test ${out}/SonoreGuiProbe.clap --emits-dc --expect-editor-limits 517 341 1200 800 | tail -3`,
      `${out}/vst3_host_test ${out}/SonoreSaturator.vst3 | tail -3`,
      // --expect-mpe, which ctest has passed here since the synth learned MPE
      // and this leg never did. The test asserts the OPPOSITE without it -- "a
      // DSP that does not play expressively declares none" -- so it has been
      // failing every run and reporting it into a pipeline whose status
      // nobody read.
      `${out}/vst3_host_test ${out}/SonoreSynth.vst3 --expect-mpe | tail -3`,
      `${out}/vst3_host_test ${out}/SonoreDucker.vst3 --expect-sidechain | tail -3`,
      `${out}/vst3_host_test ${out}/SonoreTrim.vst3 --expect-channels 1 8 | tail -3`,
      `${out}/vst3_host_test ${out}/SonoreSplitter.vst3 --expect-aux-outs 2 | tail -3`,
      `${out}/vst3_host_test ${out}/SonoreArp.vst3 --expect-midi-out | tail -3`,
      `${out}/vst3_host_test ${out}/SonoreSampler.vst3 --expect-mpe | tail -3`,
      `${out}/vst3_host_test ${out}/SonoreReverb.vst3 | tail -3`,
      `${out}/SaturatorApp --verify`,
      `${out}/SynthApp --verify`,
      `${out}/lv2_host_test ${out}/SatLV2.lv2 | tail -3`,
      `${out}/lv2_host_test ${out}/SynthLV2.lv2 | tail -3`,
      `${out}/lv2_host_test ${out}/DuckerLV2.lv2 --expect-sidechain | tail -3`,
      `${out}/lv2_host_test ${out}/SplitLV2.lv2 --expect-aux-outs 2 | tail -3`,
      `${out}/lv2_host_test ${out}/ArpLV2.lv2 --expect-midi-out | tail -3`,
      // The LV2 industry validators, when the user-local toolchain exists
      // (built once from source into ~/.local -- lv2lint + lilv tools +
      // sord_validate; WSL has no root, so apt was never an option). lv2lint
      // is what MOD & friends gate submissions on; lv2_validate is strict RDF
      // against the spec schemas. The spec dirs join LV2_PATH so lilv can
      // resolve the class taxonomy. NO $vars or $() here -- this line is
      // evaluated twice on its way through wsl.exe, and an expanded $PATH
      // containing "(x86)" re-parses as a syntax error. Tildes survive, so
      // tildes it is, and the two URIs are spelled out statically.
      "if [ -x ~/.local/bin/lv2lint ]; then " +
        "export LD_LIBRARY_PATH=~/.local/lib/x86_64-linux-gnu:~/.local/lib && " +
        "export PATH=~/.local/bin:/usr/bin:/bin && " +
        // A clean dir of symlinks: pointing lilv at ~/sonolinux itself would
        // make it probe every stray binary there as a bundle and bury the
        // report in noise.
        `rm -rf ${out}/lv2check && mkdir -p ${out}/lv2check && ` +
        `ln -s ${out}/SatLV2.lv2 ${out}/SynthLV2.lv2 ${out}/DuckerLV2.lv2 ${out}/SplitLV2.lv2 ${out}/ArpLV2.lv2 ${out}/ProbeLV2.lv2 ${out}/lv2check/ && ` +
        `export LV2_PATH=${out}/lv2check:~/.local/lib/x86_64-linux-gnu/lv2:~/.local/lib/lv2 && ` +
        "lv2lint urn:sonorie:com.sonorie.example.saturator && " +
        "lv2lint urn:sonorie:com.sonorie.example.synth && " +
        "lv2lint urn:sonorie:com.sonorie.example.ducker && " +
        "lv2lint urn:sonorie:com.sonorie.example.splitter && " +
        "lv2lint urn:sonorie:com.sonorie.example.arp && " +
        // The probe last, because it is the one carrying the port properties
        // no other bundle uses.
        "lv2lint urn:sonorie:com.sonorie.test.guiprobe && " +
        `! lv2_validate ${out}/ProbeLV2.lv2/manifest.ttl ${out}/ProbeLV2.lv2/plugin.ttl 2>&1 | grep "error:" && ` +
        // presets.ttl is validated too, and separately: it is a file a host
        // only opens when a user goes looking for presets, so a syntax error
        // in it is invisible until the moment someone wants one.
        // NOT `lv2_validate ... > /dev/null &&`. sord_validate prints its
        // errors and then exits ZERO, so an && chain walks straight past
        // them: this gate reported a clean LV2 leg for as long as it has
        // existed while every synth bundle was failing a required-property
        // check. Grepping the output is the only signal it actually gives,
        // and `!` in front turns a match back into a failure. Errors print
        // on their way past, which is the point -- a silent validator is
        // worse than no validator.
        `! lv2_validate ${out}/SatLV2.lv2/manifest.ttl ${out}/SatLV2.lv2/plugin.ttl ${out}/SatLV2.lv2/presets.ttl 2>&1 | grep "error:" && ` +
        // …and lilv is asked to actually LIST them, because a file that
        // parses is not the same as a preset a host can find. lv2info reads
        // the bundle the way a plugin browser does.
        // `grep -q`, NOT. It stops reading the moment it matches, lv2info gets
        // SIGPIPE, and under pipefail the pipeline reports 141 -- so this
        // passed for as long as lv2info's output was short enough to finish
        // writing before grep exited, and started failing the day the plugin
        // grew two more ports. Plain grep reads to EOF and cannot do that.
        `lv2info urn:sonorie:com.sonorie.example.saturator | grep "Warm Glue" > /dev/null && ` +
        `echo "  lv2: factory presets visible to lilv" && ` +
        `! lv2_validate ${out}/SynthLV2.lv2/manifest.ttl ${out}/SynthLV2.lv2/plugin.ttl 2>&1 | grep "error:" && ` +
        `! lv2_validate ${out}/DuckerLV2.lv2/manifest.ttl ${out}/DuckerLV2.lv2/plugin.ttl 2>&1 | grep "error:" && ` +
        `! lv2_validate ${out}/SplitLV2.lv2/manifest.ttl ${out}/SplitLV2.lv2/plugin.ttl 2>&1 | grep "error:" && ` +
        `! lv2_validate ${out}/ArpLV2.lv2/manifest.ttl ${out}/ArpLV2.lv2/plugin.ttl 2>&1 | grep "error:" && ` +
        "echo LV2-VALIDATORS-PASSED; " +
      "else echo SKIPPED-lv2lint-not-found-in-WSL; fi",
    ].join(" && ");
    const r = spawnSync("wsl", ["-d", "Ubuntu", "--", "bash", "-lc", script], {
      stdio: "inherit",
    });
    if (r.status !== 0) {
      failed = true;
      // Recorded, not just flagged. This leg sets the failure directly rather
      // than through run(), so for a long time it could turn the gate red
      // while the summary at the bottom listed nothing at all -- which is
      // exactly the message the failure list was added to prevent, and it
      // happened here first.
      failures.push(`exit ${r.status === null ? "signal " + r.signal : r.status}: ` +
                    "the Linux leg (WSL). Its output is above, in order; the failing " +
                    "command is the last one that printed.");
    }
  }
}

// ── Industry validators ──────────────────────────────────────────────────────────────
// Our host tests prove OUR reading of the specs; clap-validator and pluginval
// prove the industry's. They caught a real one (state load without a
// clap_host_params::rescan), so they gate the verify whenever the tools are
// present. Fetch them into sdk/tools/:
//   clap-validator: https://github.com/free-audio/clap-validator/releases
//   pluginval:      https://github.com/Tracktion/pluginval/releases
if (process.platform === "win32") {
  log("\n── validators (clap-validator + pluginval, the industry's spec reading) ───");
  const clapValidator = join(sdk, "tools", "clap-validator.exe");
  if (existsSync(clapValidator)) {
    // The GUI probe is on this list even though it is a test fixture, and
    // BECAUSE it is: it carries the awkward declarations no shipping example
    // has -- a hidden parameter, one that refuses automation, named steps, a
    // latency that moves, a tail that moves. Those are exactly what a
    // validator is for, and the plugin exercising them was the one plugin
    // never handed to one.
    for (const name of ["SonoreSaturator", "SonoreSynth", "SonoreDucker", "SonoreTrim",
                        "SonoreSplitter", "SonoreArp", "SonoreSampler", "SonoreReverb",
                        "SonoreGuiProbe"]) {
      // preset-discovery-crawl/load are EXCLUDED, and not because we fail them:
      // clap-validator 0.4.1 deadlocks itself on any provider that declares
      // more than one preset. Its begin_preset() holds a MutexGuard on
      // `result` and then calls flush_preset(), which locks the same
      // non-reentrant mutex, so the second preset hangs the validator, not
      // the plugin. Verified by declaring a single preset: the same test
      // passes in 1 ms. Drop this exclusion once the tool is fixed upstream.
      run(`"${clapValidator}"`, [
        "validate",
        "-x", '"preset-discovery-(crawl|load)"',
        `"${join(build, "Release", name + ".clap")}"`,
      ]);
    }
  } else {
    log("  SKIPPED clap-validator: sdk/tools/clap-validator.exe not found.");
  }
  const pluginval = join(sdk, "tools", "pluginval.exe");
  if (existsSync(pluginval)) {
    // pluginval wants the canonical Name.vst3/Contents/... folder; our build
    // keeps it under Name.vst3.bundle so the flat .vst3 DLL can share the dir.
    // ── A stage directory PER PROCESS ──────────────────────────────────────
    //
    // This was one fixed path that each run deleted before reusing, and on
    // Windows that turned a leftover file handle into a dead gate: rmSync threw
    // EPERM on a staged .vst3 the previous pluginval still held open, the
    // exception escaped, and the run ended with no verdict printed at all. A
    // gate that can stop in the middle is one whose silence looks like success
    // -- and it did, because the invocation piped it into `tail` and read
    // tail's exit code.
    //
    // The fix is not a retry loop. It is not needing to delete anything: a
    // directory named after this process cannot be held by another one.
    const stage = join(tmpdir(), `sonore-pluginval-${process.pid}`);
    mkdirSync(stage, { recursive: true });

    // Old stage directories from runs that crashed or were killed. Best
    // effort, every failure ignored: tidying temp is not worth failing a
    // verification over, which is exactly the mistake being fixed here.
    try {
      for (const old of readdirSync(tmpdir())) {
        if (!old.startsWith("sonore-pluginval-")) continue;
        if (old === `sonore-pluginval-${process.pid}`) continue;
        try {
          rmSync(join(tmpdir(), old), { recursive: true, force: true });
        } catch {}
      }
      // The pre-per-process name, left behind by any build older than this.
      try {
        rmSync(join(tmpdir(), "sonore-pluginval"), { recursive: true, force: true });
      } catch {}
    } catch {}
    for (const name of ["SonoreSaturatorVST3", "SonoreSynthVST3", "SonoreDuckerVST3",
                        "SonoreTrimVST3", "SonoreSplitterVST3", "SonoreArpVST3",
                        "SonoreSamplerVST3", "SonoreReverbVST3", "SonoreGuiProbeVST3"]) {
      const staged = join(stage, name + ".vst3");
      cpSync(join(build, "Release", name + ".vst3.bundle"), staged, { recursive: true });
      run(`"${pluginval}"`, ["--strictness-level", "10", "--validate", `"${staged}"`]);
    }
    // Best effort again. pluginval has loaded and unloaded these DLLs and
    // Windows does not always release them the instant the process exits;
    // whatever is left is swept by the next run.
    try {
      rmSync(stage, { recursive: true, force: true });
    } catch {}
  } else {
    log("  SKIPPED pluginval: sdk/tools/pluginval.exe not found.");
  }
}

// ── Format matrix ────────────────────────────────────────────────────────────
// What actually got BUILT, from the artifacts on disk rather than from a claim
// in a readme. A format that silently stopped being produced would otherwise
// keep being advertised.
log("\n── format matrix (artifacts on disk, not claims) ──────────────────────");
{
  const releaseDir = join(build, "Release");
  const dir = existsSync(releaseDir) ? releaseDir : build;
  let names = [];
  try {
    names = readdirSync(dir);
  } catch {
    names = [];
  }
  const has = (test) => names.filter(test).length;
  const rows = [
    ["CLAP", has((n) => n.endsWith(".clap")), "built + host-tested + clap-validator"],
    ["VST3", has((n) => n.endsWith(".vst3.bundle")), "built + host-tested + pluginval S10"],
    ["LV2", has((n) => n.endsWith(".lv2")), "built + host-tested + lv2lint (Linux leg)"],
    ["Standalone", has((n) => /App(\.exe)?$/.test(n)), "built + offline --verify"],
    ["AUv2", 0, "macOS only: built and auval'd in CI, never on this box"],
    ["AUv3", 0, "app EXTENSION: needs a containing app + Xcode target (iOS / App Store)"],
    ["AAX", 0, "blocked: Avid developer agreement + PACE signing"],
    ["VST2", 0, "deliberately never: Steinberg ended licensing in 2018"],
  ];
  for (const [name, count, note] of rows) {
    const mark = count > 0 ? `${String(count).padStart(2)} built` : "      --";
    log(`  ${name.padEnd(11)} ${mark}   ${note}`);
  }
}

log("");
if (failed) {
  log("SDK VERIFY FAILED");
  for (const f of failures) log("  " + f);
  process.exit(1);
}
log("SONORE SDK VERIFY PASSED");
