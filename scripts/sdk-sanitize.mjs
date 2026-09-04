// SPDX-License-Identifier: Apache-2.0
// The SDK under the sanitizers: every example built as .clap, .vst3 and .lv2
// with AddressSanitizer + UndefinedBehaviorSanitizer and driven by the same
// host tests verify:sdk runs; the DSP unit suite the same way; the parser
// fuzzer at depth; and the sampler's sample handover under ASan and then
// ThreadSanitizer.
//
//   npm run verify:sanitize
//
// Separate from verify:sdk because it is SLOW -- the instrumented builds take
// most of an hour on a laptop -- and because it needs gcc: on Windows it runs
// under WSL (Ubuntu), on Linux natively, and on macOS not at all yet. It exists
// because the first time this was done by hand it found what the 3134 measured
// checks, the validators and REAPER had all walked past: a Component that
// could be copied over a live one, a rasteriser segfault on NaN geometry, an
// SVG parser that looped forever, a FLAC overflow, a VST3 host test lying about
// its own array. None of those has a reference file, a validator, or a host
// that would have said anything.
//
// What a failure looks like: "SANITIZE FAILED" with the legs that failed, and
// the sanitizer's own report above it with a stack. A hang in a fuzzed parser
// exits 99 and names the input it wrote next to the binary.
import { spawnSync } from "node:child_process";
import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const sdk = join(root, "sdk");
const log = (s) => process.stdout.write(s + "\n");

// The whole leg is one bash script. Written to a file and run by path, never
// passed on the command line: wsl.exe re-parses what it is given, and a `$`
// or a quote in an inline command arrives mangled or empty. (The same trap
// that bit sdk-verify.mjs, kept out of reach here by construction.)
function script(sdkPath) {
  return `
set -o pipefail
S=${sdkPath}
INC="-I$S/include -I$S/third_party/clap/include"
VINC="-I$S/third_party/vst3"
LINC="-I$S/third_party/lv2/include"
O=$HOME/sonore-sanitize
mkdir -p $O; cd $O
# -O1 keeps the sanitizer's stacks readable; recover=all lets one report not
# hide the next. The visibility flags are the ones CMake gives every plugin
# target -- without them several plugins in one process bind each other's
# inline descriptors, which is a build mistake that looks exactly like a host
# bug (every plugin scans under the first one's name).
SAN="-std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fsanitize-recover=all"
printf '%s' "{ global: clap_entry; GetPluginFactory; ModuleEntry; ModuleExit; bundleEntry; bundleExit; lv2_descriptor; lv2ui_descriptor; lv2_lib_descriptor; local: *; };" > "$O/sonore_exports.map"
VIS="-fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,--version-script=$O/sonore_exports.map"
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=0
# tests/ubsan.supp names the third-party findings that were looked at and
# left, with their reasons; anything not in it is a failure.
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0:suppressions=$S/tests/ubsan.supp
failed=""
fail() { failed="$failed\\n  $1"; }

build() { # out src extra...
  local out=$1 src=$2; shift 2
  g++ $SAN "$@" -o $out $src 2>&1 | grep -E "error" | head -8
  [ -f $out ] || fail "build: $out"
}
# name, then the command. The verdict is the exit code AND the absence of any
# sanitizer report in the output -- a test can pass while ASan is reporting,
# because recover=all lets it, and that report is the finding.
run() {
  local name=$1; shift
  "$@" > log_$name.txt 2>&1; local code=$?
  local n; n=$(grep -c "runtime error\\|ERROR: AddressSanitizer\\|WARNING: ThreadSanitizer" log_$name.txt)
  local last; last=$(grep -E "checks,|PASSED|FAILED|HANG" log_$name.txt | tail -1)
  printf -- "--- %-16s exit=%s sanitizer=%s  %s\\n" "$name" "$code" "$n" "$last"
  if [ "$code" != 0 ] || [ "$n" != 0 ]; then
    fail "$name (exit $code, $n sanitizer lines; see $O/log_$name.txt)"
    grep -m4 -A8 "runtime error\\|ERROR: AddressSanitizer\\|WARNING: ThreadSanitizer" log_$name.txt | head -48
  fi
}

echo "== building every example as CLAP, VST3 and LV2 under ASan+UBSan (this is the slow part)"
for ex in saturator synth ducker trim splitter arp sampler reverb guiprobe; do
  case $ex in saturator) N=Saturator;; synth) N=Synth;; ducker) N=Ducker;; trim) N=Trim;; splitter) N=Splitter;; arp) N=Arp;; sampler) N=Sampler;; reverb) N=Reverb;; guiprobe) N=GuiProbe;; esac
  build $O/Sonore$N.clap $S/examples/$ex/plugin.cpp $VIS -fPIC -shared $INC -ldl
  build $O/Sonore$N.vst3 $S/examples/$ex/plugin.cpp $VIS -fPIC -shared -DSONORE_BUILD_VST3 $INC $VINC -ldl
  mkdir -p $O/\${N}LV2.lv2
  build $O/\${N}LV2.lv2/\${N}LV2.so $S/examples/$ex/plugin.cpp $VIS -fPIC -shared -DSONORE_BUILD_LV2 $INC $LINC -ldl
  build $O/\${ex}_ttlgen $S/examples/$ex/plugin.cpp -DSONORE_BUILD_LV2 -DSONORE_LV2_TTLGEN $INC $LINC -ldl
  [ -f $O/\${ex}_ttlgen ] && $O/\${ex}_ttlgen $O/\${N}LV2.lv2 \${N}LV2.so > /dev/null 2>&1
  echo "  built $ex"
done
echo "== building the hosts, the suites and the apps"
build $O/clap_host_test $S/tests/clap_host_test.cpp $INC -ldl
build $O/vst3_host_test $S/tests/vst3_host_test.cpp $INC $VINC -ldl
build $O/lv2_host_test $S/tests/lv2_host_test.cpp $INC $LINC -ldl
build $O/sdk_tests $S/tests/sdk_tests.cpp -pthread -DSONORE_TEST_DATA_DIR="\\"$S/tests/data\\"" $INC $VINC
build $O/fuzz_parsers $S/tests/fuzz_parsers.cpp -pthread -DSONORE_TEST_DATA_DIR="\\"$S/tests/data\\"" $INC $VINC $LINC -ldl
build $O/sampler_stress $S/tests/sampler_stress_test.cpp -pthread $INC -ldl
build $O/SaturatorApp $S/examples/saturator/plugin.cpp -DSONORE_BUILD_STANDALONE $INC -ldl -lpthread
build $O/SynthApp $S/examples/synth/plugin.cpp -DSONORE_BUILD_STANDALONE $INC -ldl -lpthread
build $O/ArpApp $S/examples/arp/plugin.cpp -DSONORE_BUILD_STANDALONE $INC -ldl -lpthread

echo "== the DSP unit suite"
run sdk_tests $O/sdk_tests
echo "== CLAP"
run clap_saturator $O/clap_host_test $O/SonoreSaturator.clap
run clap_synth $O/clap_host_test $O/SonoreSynth.clap
run clap_ducker $O/clap_host_test $O/SonoreDucker.clap --expect-sidechain
run clap_trim $O/clap_host_test $O/SonoreTrim.clap --expect-channels 1 8
run clap_splitter $O/clap_host_test $O/SonoreSplitter.clap --expect-aux-outs 2
run clap_arp $O/clap_host_test $O/SonoreArp.clap --expect-midi-out
run clap_sampler $O/clap_host_test $O/SonoreSampler.clap
run clap_reverb $O/clap_host_test $O/SonoreReverb.clap
run clap_guiprobe $O/clap_host_test $O/SonoreGuiProbe.clap --emits-dc --expect-editor-limits 517 341 1200 800
echo "== VST3"
run vst3_saturator $O/vst3_host_test $O/SonoreSaturator.vst3
run vst3_synth $O/vst3_host_test $O/SonoreSynth.vst3 --expect-mpe
run vst3_ducker $O/vst3_host_test $O/SonoreDucker.vst3 --expect-sidechain
run vst3_trim $O/vst3_host_test $O/SonoreTrim.vst3 --expect-channels 1 8
run vst3_splitter $O/vst3_host_test $O/SonoreSplitter.vst3 --expect-aux-outs 2
run vst3_arp $O/vst3_host_test $O/SonoreArp.vst3 --expect-midi-out
run vst3_sampler $O/vst3_host_test $O/SonoreSampler.vst3 --expect-mpe
run vst3_reverb $O/vst3_host_test $O/SonoreReverb.vst3
echo "== LV2"
run lv2_saturator $O/lv2_host_test $O/SaturatorLV2.lv2
run lv2_synth $O/lv2_host_test $O/SynthLV2.lv2
run lv2_ducker $O/lv2_host_test $O/DuckerLV2.lv2 --expect-sidechain
run lv2_splitter $O/lv2_host_test $O/SplitterLV2.lv2 --expect-aux-outs 2
run lv2_arp $O/lv2_host_test $O/ArpLV2.lv2 --expect-midi-out
run lv2_guiprobe $O/lv2_host_test $O/GuiProbeLV2.lv2 --expect-probe
echo "== standalone --verify"
run app_saturator $O/SaturatorApp --verify
run app_synth $O/SynthApp --verify
run app_arp $O/ArpApp --verify

echo "== the parsers, at depth"
# A corrupt Ogg header asks stb_vorbis for an absurd allocation; that is
# upstream's and not memory-unsafe, and ASan's default is to abort the whole
# process on it. Returning null instead is what production malloc does.
ASAN_OPTIONS=detect_leaks=0:halt_on_error=0:allocator_may_return_null=1 \\
  run fuzz_parsers $O/fuzz_parsers --iterations 3000

echo "== the sampler handover and the two-thread plugin probe: ASan, then ThreadSanitizer"
run sampler_asan $O/sampler_stress 5
build $O/concurrency_asan $S/tests/concurrency_test.cpp -pthread $INC -ldl
run concurrency_asan $O/concurrency_asan 4
g++ -std=c++17 -O1 -g -fsanitize=thread -pthread $INC -o $O/concurrency_tsan $S/tests/concurrency_test.cpp -ldl 2>&1 | grep -E "error" | head -5
g++ -std=c++17 -O1 -g -fsanitize=thread -pthread $INC -o $O/sampler_stress_tsan $S/tests/sampler_stress_test.cpp -ldl 2>&1 | grep -E "error" | head -5
if [ -f $O/sampler_stress_tsan ]; then
  # ThreadSanitizer refuses to start under the address-space randomisation
  # of a recent kernel ("unexpected memory mapping"); setarch -R turns it off
  # for this one process where the tool exists.
  if command -v setarch > /dev/null; then
    TSAN_OPTIONS=halt_on_error=0 run sampler_tsan setarch $(uname -m) -R $O/sampler_stress_tsan 5
    [ -f $O/concurrency_tsan ] && TSAN_OPTIONS=halt_on_error=0 run concurrency_tsan setarch $(uname -m) -R $O/concurrency_tsan 4
  else
    TSAN_OPTIONS=halt_on_error=0 run sampler_tsan $O/sampler_stress_tsan 5
    [ -f $O/concurrency_tsan ] && TSAN_OPTIONS=halt_on_error=0 run concurrency_tsan $O/concurrency_tsan 4
  fi
  [ -f $O/concurrency_tsan ] || fail "build: concurrency_tsan"
else
  fail "build: sampler_stress_tsan"
fi

if [ -n "$failed" ]; then
  printf "\\nSANITIZE FAILED:$failed\\n"
  exit 1
fi
echo
echo "SONORE SDK SANITIZE PASSED"
`;
}

const isWin = process.platform === "win32";
if (!isWin && process.platform !== "linux") {
  log("SKIPPED: verify:sanitize runs under gcc on Linux (natively) or on Windows through WSL; " +
      "there is no macOS leg yet.");
  process.exit(0);
}

const buildDir = join(sdk, "build");
mkdirSync(buildDir, { recursive: true });
const scriptPath = join(buildDir, "sanitize.sh");

let sdkPath = sdk.replace(/\\/g, "/");
let scriptArg = scriptPath;
if (isWin) {
  const toWsl = (p) =>
    "/mnt/" + p.replace(/^([A-Za-z]):/, (_, d) => d.toLowerCase()).replace(/\\/g, "/");
  sdkPath = toWsl(sdk);
  scriptArg = toWsl(scriptPath);
}
writeFileSync(scriptPath, script(sdkPath).replace(/\r\n/g, "\n"), "utf8");

log("── sanitize (ASan+UBSan on every wrapper, the suites, the fuzzer; TSan on the handover) ──");
const started = Date.now();
const r = isWin
  ? spawnSync("wsl", ["-d", "Ubuntu", "--", "bash", "-lc", "bash " + scriptArg], { stdio: "inherit" })
  : spawnSync("bash", [scriptPath], { stdio: "inherit" });
const minutes = ((Date.now() - started) / 60000).toFixed(1);
if (r.status !== 0) {
  log(`\nSDK SANITIZE FAILED (${minutes} min)`);
  process.exit(1);
}
log(`\n(${minutes} min)`);
