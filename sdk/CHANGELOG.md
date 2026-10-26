# Changelog

## Unreleased

From 1.0.0 the SDK follows Semantic Versioning; README's "Versions and
compatibility" says what that promises, and `SONORE_DEPRECATED` marks what a
later major will remove.

Everything since the last release, newest work included. Short hashes point at the commit that made each change.

### Changes that can affect an existing plugin

#### Source compatibility

- `PitchCorrector::latencySamples()` is a member, not `static constexpr`: the latency now depends on the rate and the range's floor. Call `setRange()` before `setSampleRate()` and read the latency after. (48e2d24)
- A project generated from the templates derives its Audio Unit subtype from a hash of the plugin id (the name's initial and three base-36 characters), so two products of one vendor no longer share one. Regenerating a project for an AU that has already shipped needs `-DAU_SUBTYPE=` with the shipped code. (42ae629)
- `Graphics` can no longer be copied: it owns the layers `beginLayer` opens. Nothing in the SDK copied one. `SvgShape::strokeWidth` now includes the scale of the transforms above the shape, as its path always did. (21d574f, 5631ef0)
- The 34 headers dsp.h pulls in (eq.h, tape.h, synth.h and the rest) now stop with an `#error` naming `<sonore/dsp.h>` when included on their own; they never compiled alone. (992eb61)
- `Component` has a virtual `asWidget()`; a class deriving from `Component` that already declares a member of that name will clash. (6229d32)
- `latencySamples()`, `tailSamples()` and `saveState()` declared without `const` are now compile errors (a `static_assert` names the signature to write). They were silently ignored before. Once you add `const`, hosts are told the latency and tail, the state is saved and `loadState()` is called. (f112e94)
- A plugin source should now end in `#include <sonore/formats.h>`. A source that ends in `clap_wrapper.h` alone, as the old README template did, builds only a CLAP under `sonore_add_vst3/au/lv2/standalone`. The VST3 exported only `clap_entry`, the standalone had no entry point, and the LV2 generator and AU had no factory. All examples now use `formats.h`. (f112e94, bee5405)
- A debug build (no `NDEBUG`) now refuses to initialise the CLAP entry when the descriptor is malformed, and prints the reason on stderr. Malformed means a duplicate parameter id, a default outside its range, an inverted channel range, an empty name, or `kDesc.numParams` different from `SONORE_NUM_PARAMS`. The standalone's `--verify` fails on such a descriptor in every build; release builds load as before. (920fec3)
- If your DSP has any callable `reset()`, CLAP's `reset()` and an AU's Reset now call it on the audio thread instead of `prepare()`. Make sure it clears everything that matters, keeps what `prepare()` sized, and does not allocate. Without a `reset()`, `prepare()` is called as before. (a4b0630)
- `Oversampler<Stages, Taps>` now fails a `static_assert` when the cascade's latency is not a whole number of samples (for example `<2, 31>` and `<3, 35>`, which under-reported latency). The default 33 taps and every SDK instantiation are unaffected. (015286b)
- `DryWetMixer::process()` is no longer `const`. A mix changed while audio runs now glides; `snap()` (new) lands it at once. (fa01a80)
- `MidiBuffer::kMaxEvents` went from 64 to 1024, so each buffer is about 16 KB. The SDK keeps every MidiBuffer as a member or static; do the same rather than putting one on the stack. When the buffer is full, it now evicts the latest controller, bend or pressure event to make room for a note-off. (2ea30e4)
- `FirFilter::kMaxTaps` is 4095 (was 4096). An even `designLowpass` tap count is rounded up to odd (64 becomes 65 taps, latency 32), so its latency changes. `designLowpassToSpec` with a stopband under 6 dB now returns 3 taps instead of refusing. (9f65aba)
- `osc::Receiver::open(port)` now binds loopback only. To accept other devices, call `open(port, true)`, ideally behind a setting the user can see. (6223282)
- `GrainEngine::reset()` no longer unloads a loaded sample; the new `unload()` returns to the live buffer. `setParameters()` now replaces non-finite values with defaults. It clamps position, jitter, spread and reverse to 0..1, pitch to ±16 and pitch jitter to 48 semitones. (4dd50ee, 07d61ab)
- The `ImpulseResponse::trimSilence(thresholdDb)` threshold is now measured in dB below the response's own peak, not in dBFS, and a cut end fades over at most 10 ms. With Energy normalisation, the old default of -60 dBFS cut a 3 s reverb at 1.2 s. (29c8085)
- `Convolver::loadImpulse` must not be called from another thread while `process()` runs (the old class comment said it could). Load a second convolver and hand it over, as the reverb example's `Room` does. (6038345, b9a248a)
- `PluginGraph` that is unprepared now outputs silence instead of the uncompensated dry input. This covers a graph never prepared, after a refused re-prepare, and after `connect()`. `needsPrepare()` becomes true after a wiring change, a node's restart request or a node's latency change; re-prepare with audio stopped. Blocks longer than the prepared size are run in slices. (70dbef8, 8eb1fbf)
- Web editor pages, including any frame in them, may only navigate to `about:`, `data:` and `blob:`; http(s), `file:` and dropped files are refused, and `window.open` returns null. A page must carry its own content. (10f7c55, d36e952)
- When a pitch is below the window's reach at the current sample rate, `PitchDetector::frequency()` and `confidence()` now return 0 instead of the clamped floor. Use the new `lowestHz()` to check the reach. (ce99981)
- `osc::Type::Nil` is deprecated; the 'N' tag is `osc::Type::Null`. `<objc/objc.h>` defines `Nil` as a macro, and on a Mac most of the SDK's headers include it, so `Type::Nil` did not compile there. `Nil` still works elsewhere, with a deprecation warning. (004b39e)
- `gainReduction()` means one thing on every dynamics class: decibels of reduction, POSITIVE (6 is 6 dB quieter). `Compressor`, `Expander`, `DeEsser`, `Limiter` and `TruePeakLimiter` returned the signed gain change, negative while reducing, where the hardware models in compressors.h already returned positive decibels, so a meter fed one class fell and fed another sat still. Code that negated the reading, or compared it with a negative number, now reads it the wrong way round; the signed change is `-gainReduction()`. `UpwardCompressor::gainChange()` stays signed, because it can add gain. A `SonoreDsp::gainReduction()` declared without `const` is a compile error, as `latencySamples()` is.

#### Sound

- An MPE plugin (`supportsMpe`) played from a keyboard that is not MPE bends 2 semitones each way, MIDI's default, where it bent 48: `MpeDecoder` gave every channel a zone member's range, announced or not. An RPN 0 on the channel still sets the range; `MpeDecoder::setLegacyBendRange()`, or the DSP's `pitchBendRange()`, changes the default. The example synth and sampler bent a plain keyboard's chord four octaves at full throw.
- `VoiceManager` fades out a stolen voice over 2 ms instead of cutting it off, for a voice with `float render()`; `VoiceManager::render()` mixes the fade in, so a plugin that renders each voice itself hears the steal as before. All Sound Off (CC 120) through `applyPedals()` silences every voice at once, through the voice's `kill()` where it has one; it released them, as All Notes Off (CC 123) still does.
- GCC and Clang no longer contract `a*b + c` into one fused multiply-add: `sonore_sdk` carries `-ffp-contract=off`, and the installed package adds it for the consumer's own GNU-style compiler. Every arm64 CPU has the instruction, and a fused result is rounded once where the written expression rounds twice, so a plugin on Apple Silicon or Linux arm64 computed different bits from its browser build, which can never contract (WebAssembly has no FMA). It now computes the same ones. The difference is in the last bits; on x86 without `-mfma` nothing changes.
- `Chorus`, `Phaser`, `Flanger` and `RingModulator` start at a mix of 0.5, as the reference's chorus and phaser do, where they started dry (0) until the mix was set: a plugin that sets the mix hears no difference, one that never did now hears the effect. The mix law is unchanged (equal power). No example uses them. (9131b53)
- Round 36's DSP fixes that change what a plugin hears (each only in the case named): `cubic()` holds at +/-1 instead of falling back to 0.5625 past full scale, so `Distortion`'s `Curve::Cubic` no longer gets quieter as the drive rises (2176a8e); `Reverb` and `FdnReverb` at a width below 1 blend the sides, and width 0 is mono where it swapped them (ad22b3e); a moved mix glides in 20 ms at the session's rate on each side of `CompensatedDryWetMixer`, `Reverb`, `Chorus`, `Flanger`, `Phaser`, `FdnReverb`, `StereoDelay` and `SpringReverb`, where one shared mixer glided in half that, at a rate it never got (f98c8bb, 8705a7c); a linear-phase `ParametricEq` is redesigned for a new rate (ae3a585); a `MultibandSplitter` keeps its crossovers after a visit to a low rate (c47635e). With the mix held, every example's output is bit-identical.
- `PitchCorrector` moves the voice by pitch-synchronous overlap-add (PSOLA) instead of a 2048-point phase vocoder: every harmonic moves by exactly the ratio at any rate, and the formants stay put. The vocoder's frame could not part a low voice's harmonics: at 96 kHz a 112 Hz voice aimed at 110 had harmonics 2 to 5 16-25 cents sharp, a 63 Hz voice at 48 kHz 17-23 cents off, and at 384 kHz the fundamental 174 cents flat; all are now within 2 cents. Its latency is 2.5 of the longest period it handles, 42 ms with the default 60 Hz floor at every rate (it was 2048 samples). (48e2d24)
- `PlateReverb` is sized for 384 kHz (about 1.5 MB of delay lines, was 740 KB). Above 192 kHz the tank kept its 192 kHz delays, half the plate in time, and a 150 ms predelay stopped at 107 ms. (87685fe)
- **Output that was NaN or infinity is now silence, by default.** A block with a NaN or an infinity on any output port plays as silence on every port, in every format -- CLAP, VST3, AU, LV2, the standalone and the browser build -- instead of reaching the host as full-scale noise; the DSP keeps its state and the next finite block plays as normal. The first such block is reported once (the host's log where it has one, otherwise stderr). The check reads each sample's bits, 12-83 ns a 32-512-frame stereo block. `PluginDescriptor::guardOutput = false` (a new last field) lets the samples through; the standalone still zeroes a non-finite sample before the speakers either way. `--verify` now fails a plugin whose output had to be silenced. (4324386)
Filters and EQ

- Chebyshev Type I and II (CascadedIir) are now one bilinear transform of the prototype, with the edge exactly on the requested frequency. Odd orders were a different filter (order 3, 1 dB ripple: -13.2 dB at its edge instead of -1). An order-8 highpass at 20 kHz now designs instead of being refused. (858c8eb)
- CascadedIir Butterworth corners are no longer clamped to 10 Hz. All cascades, LinkwitzRileyN included, run in double-precision sections. Low corners and high rates now measure as designed: an order-6 lowpass at 2 Hz read -19 dB at its corner. (153a8cb)
- ParametricEq, DynamicEq and PassiveEq now design and run their bands in double. A bell or shelf uses the matched design only when it is closer to the prototype than the cookbook one. Example: a 15 kHz +12 dB shelf at 44.1 kHz read -5.45 dB at 8 kHz and now reads +0.24 dB; a 20 Hz +12 dB Q 4 bell at 192 kHz read +9.4 dB. (b9feda1, f6404a1)
- DynamicEq highpass and notch bands now filter; they were unity. Band frequency is held to 10 Hz..0.45 fs and Q to at least 0.05. PassiveEq shelves now fall back to the cookbook design instead of unity. (3ca0244)
- PassiveEq's first-order low cut and high atten are prewarped, so the 14 kHz hinge reads -2.90 dB at every rate (it read -5.07 dB at 44.1 kHz). (53be73d)
- A ParametricEq linear-phase kernel is now level-matched where the curve is loudest, not at DC. A curve with a highpass or bandpass band played at about -228 dB before. (f253a45)
- Baxandall treble now reads exactly the requested level at 15 kHz at every rate (+10 dB read +10.98 dB at 44.1 kHz). (2316fcd)
- TiltEq and KWeighting run in double-precision sections; the difference shows at very high rates such as 384 kHz. (31c9cc3)
- LinkwitzRiley and MultibandSplitter (and so MultibandDynamics) now use double-precision TPT state-variable sections. A fixed crossover is the same filter. Low splits at high rates now sum flat (up to 3.8 dB off at 192 kHz before), and moving the crossover frequency no longer diverges. (c5c845c, bba9632)
- ChannelMatrix routes mono into 5.1 to the centre only; it also fed left and right at -3 dB. Mono into stereo is unchanged. (1b8ba35)

Dynamics and distortion

- TruePeakLimiter has its own 8x, 64-tap detector with a 0.25 dB margin. Reported latency grows by 24 samples (0.5 ms at 48 kHz). Measured with a 32x reconstruction, clipped noise through a -1 dBTP ceiling comes out at -1.23 dBTP (was -0.04). The sample-peak path is aligned with the inter-sample one. TruePeakMeter's per-sample value now compares points of the same instant, so a click peaks 7 samples later. (b016917, f3c6f7e, c800bb4)
- Limiter now clamps its look-ahead to the delay it actually applies. `Limiter<1024>` at 384 kHz reports 1022 samples (was 1025). (f3c6f7e)
- Distortion's dry/wet is now latency-compensated; it was a comb with a notch at 857 Hz at 48 kHz. A Distortion now starts fully wet (it started fully dry), so call `setMix()` if you relied on the default. Drive and makeup glide in dB, and the drive compensation is sampled at 256 points instead of 64. (eb54681, 73147bd)
- DiodeLadder without saturation now tops out at K = 16.5 (about a 10 dB peak) instead of running away. With saturation on, full resonance reaches K = 17.85 and self-oscillates. (8a35094, 80e7cb8)

Parameter changes while audio runs

- Many controls now glide when changed while audio runs. A value set before the first `process()`, or after `reset()`/`snap()`, still lands at once. Automation that relied on an instant jump will now ramp. The new glides are:
  - DryWetMixer mix, 20 ms at 48 kHz, and every class built on it: StereoDelay, Reverb, PlateReverb, FdnReverb, SpringReverb, Chorus/Flanger, Phaser, RingModulator, CompensatedDryWetMixer;
  - over 10 ms (new `Glide`): compressor input, output and makeup; VCA threshold; opto peak reduction; expander threshold and range; upward compressor threshold; Tube Screamer level;
  - over 20 ms: StereoDelay modulation depth, FdnReverb width and decay, Reverb room size and width, PlateReverb decay, and TapeSaturator wow, flutter, output gain, drive, hysteresis and width;
  - over 50 ms: Chorus/Flanger depth and centre delay, and an FdnReverb size change (a crossfade);
  - the Gain chain stage now ramps over a fixed 10 ms; the ramp used to last one host block.

  (fa01a80, cb0e717, 60a3d95, c12fd8c, 8eba79f, 5c0fa45, 73147bd, fb9ef70)

Reverbs, delays and modulation

- FdnReverb lines stay distinct at any rate. At 192 kHz, seven of the eight lines (all eight at size 1.0) became the same length, which made a comb. The default `MaxDelay` is now 32768 instead of 8192, about 1.1 MB per instance instead of about 320 KB, so the default network sounds as it does at 48 kHz up to 204 kHz. Use `FdnReverb<8, 8192>` to keep the old footprint below 51 kHz. Read-point drift is now 0.083 ms at every rate, and `reset()` restarts the drift. (90ee83a, 51d7bed, 74bd1de, da26c6c)
- Reverb rounds its tunings to whole samples and fixes its damping cutoff in Hz, so the tail no longer depends on the rate. The 8 kHz T60 at 48 kHz was 2.56 s and is now 4.00 s, against 3.84 s at 44.1 kHz. (a7f86ec)
- PlateReverb rounds its tank delays to whole samples, so the top of the tail no longer depends on the rate. (1d850b7)
- SpringReverb keeps its two coils' 1.13 length ratio at high rates. At 192 kHz, from about a third of the size range up, both channels came out identical. Dispersion is now a bandwidth in Hz, so the chirp is the same at every rate. 48 kHz is unchanged. (9d741b6, 44d857d)
- PitchShifter rounds its window to an even number of samples and reports half the window plus 2 as latency; it reported 2 samples too few. (33fe62a)
- Vibrato's centre delay is a whole number of samples, so the reported latency equals the mean delay (25 becomes 26 at the defaults). `setSampleRate()` now sets the LFO rate: a vibrato never given `setRate()` swung 10 cents when asked for 50. The default line is now 8192 samples instead of 4096. (6626c29, 732e5dd)
- FrequencyShifter designs its Hilbert pair for a 240 Hz edge at every rate. The image at 96 and 192 kHz was -39.7 and -19.2 dB and is now -97.1 and -93.6 dB. 48 kHz is unchanged. (d03bf9a)
- PitchCorrector detects on input decimated to about 48 kHz at 88.2 kHz and up; low voices were corrected to the wrong note. A default-constructed corrector now glides as documented instead of snapping. (b4a46aa, 3732fe0)
- PhaseVocoder's peak search starts at bin 1, with a floor 100 dB under the loudest bin, so very low partials shift correctly. (4e5a98a)
- ChannelVocoder inverts every other band so neighbours add where they cross; the ripple through an open bank drops from 10.6 dB to 3.4 dB. (bb62634)
- Phaser and Chorus now reset their LFOs in `prepare()`, and `Lfo::reset()` restarts its random sequence, so renders after a reset null. NoiseGate's `prepare()` now clears its detector. VelvetNoise's `setDensity()` with an unchanged value no longer restarts the pattern. (dfd7fa9)

Oscillators, noise and voices

- PinkNoise uses 16 rows and is pink down to fs/2^17; at 48 kHz it was white below about 375 Hz. TapeSaturator's flutter noise gain changes from 2.0 to 0.7 to keep its 48 kHz depth. `setSeed()` now restarts the whole sequence. (c1a6f1c, 7fdcb5c)
- KarplusStrong's default line is now 16384 samples instead of 4096, enough for 20 Hz up to 327 kHz; at 192 kHz it could not go below 47 Hz. The excitation is scaled so a pluck is equally loud at every rate; it lost 3 dB per doubling of the rate. A reused voice no longer decays with release damping. (84cbae4, caa3b5b, cd32160)
- A fresh UnisonOscillator starts with spread phases; its voices started in phase, 6 dB hot for about 20 ms. HardSyncOscillator's `reset()` clears pending corrections. (95a7dd7)
- Dahdsr continues a retrigger from its current level instead of jumping to zero. (32b6493)
- SampleVoice reads through a Kaiser-windowed sinc by default. For a 44.1 kHz file in a 48 kHz session, the 10 kHz image drops from -26.5 to -103.6 dBc. It costs 1.6-1.8x the old cubic read at 64 voices; `setInterpolation(Interpolation::Cubic)` restores the old read. (9b9027b, b54593d, eb82169)
- `MpeDecoder` sends a channel's pitch bend and pressure to every note on that channel, and a note on a zone member hears its own bend plus the zone's. Before, a plain keyboard's wheel bent only the last note of a chord, so the example synth and sampler, which take their pitch from these events, now bend whole chords. A new note also starts with the bend, pressure and CC 74 sent ahead of it. (145193e, a2a72de, e28998e)

Tape

- JilesAtherton's output gain slope now has a floor of 0.2. The default loop is unchanged, but past hysteresis of about 0.4 a full-scale signal no longer expands: a full-scale tone through TapeSaturator at hysteresis 1 went from +33.9 dB to -1.7 dB. Re-check presets with high hysteresis. (54abaf7)
- TapeSaturator's record pre-emphasis is fitted at every rate; it was up to 4.9 dB hot at 20 kHz at 44.1 kHz, so the top end saturated harder at low rates. The head-loss FIR spans the same time at every rate (131 taps at 96 kHz, 257 at 192 kHz), so its latency in samples grows above 48 kHz. A 5 Hz DC blocker follows playback EQ (-0.07 dB at 40 Hz). (6800c1e, 617e3d1, 0f1a8cc)

Convolution, impulse responses and files

- ImpulseNormalise::Energy now targets unit energy per channel, so a stereo response plays 3.01 dB louder than before, the same as its mono version. (662c165)
- An impulse response resampled to the session rate is scaled by the rate ratio and normalised before conversion, so it is equally loud at every rate (up to +12 dB at 192 kHz before). A 48 kHz file in a 48 kHz session is unchanged. (956b8a6)
- Convolver with no impulse now outputs its input one block late, as `latency()` always reported; it was on time. An impulse longer than the convolver holds is faded over its last eighth instead of cut hard. (ec654aa, 92544bd)
- `readMp3` drops the Xing/Info frame and trims encoder delay and padding. Samples from LAME or ffmpeg MP3s start earlier (2257 samples in the test file, 51 ms at 44.1 kHz) and have their exact length. (ada63b2)
- Resampler widens its kernel with the decimation ratio. At 192 to 48 kHz the alias falls from -17 to -75.3 dB and 20 kHz no longer drops 2.38 dB. `outputLength` is exact. (e21a71f)
- `writeAiff` and `writeFlac` now scale by 2^(N-1), as the readers divide; they scaled by 2^(N-1) - 1. (d46d121)

Numerics

- fastSin and fastCos now meet their stated accuracy (worst error 6.9e-3, now 1.8e-7), as does fastLog (fastGainToDb was up to 0.66 dB off). (f566ae4)
- Every SIMD backend now sums vector lanes in the same order and handles NaN in min and max the same way. Scalar, wasm and 32-bit NEON results move in their last bits to match SSE. (b5b70db, 4c80591)

#### Sessions and state

- The state format is version 6: it remembers which preset is loaded, so a session reopens on it. Every earlier version still loads; a session saved by this build does not load in an older one. A factory preset now loads through the state-restore path, as a user preset does. (3142887)
- **VST3 class ids follow Steinberg's convention, so on Windows they change.** The 16 bytes were the same on every platform, which made their text -- what a DAW's session and a .vstpreset record -- differ between Windows and macOS, so a session moved between them lost the plugin. They are now stored as the VST3 SDK stores a GUID (COM order on Windows, vst3_uid.h), the same text everywhere. A VST3 session saved on Windows with a build from before this change does not find the plugin again; macOS and Linux ids are unchanged, and CLAP, AU and LV2 are not affected. The SDK's own host names VST3 classes by the same text, so a .vstpreset another host wrote matches. (791eb3a)
- `sha256File` returns an empty string when the file cannot be read to its end. It returned the digest of the bytes read before the error, which compares unequal to the real one without saying why. (3434965)
- A stepped parameter that also has a skew now maps linearly in both directions, so normalised values and host automation saved for it land on different steps. Check for any stepped parameter with `skew != 1`. (4b3feb1)
- If the DSP's StateBag in a state blob does not parse, the whole load is now refused before anything is applied. Before, the parameters were applied and the bag dropped, for example a sampler reopened without its sample. A blob with no bag bytes at all still loads. (3d93045)
- StateBag reads back saved `inf`, `-inf` and `nan` as themselves; they read as the fallback. A failed `deserialise()` leaves the bag untouched, and doubles now parse correctly rounded. (d9c6e07, 15051c9)
- VST3: when a host restores a session with `setState` and then `setComponentState` with the same bytes, the state is applied once instead of twice, so `loadState()` runs once. (a19e89c)
- AU: restoring a damaged ClassInfo or `.aupreset` now returns `kAudioUnitErr_InvalidPropertyValue` instead of `noErr`. The saved preset name is restored, and a restore or factory preset load notifies parameter listeners. (d1856be)
- LV2: state is stored as POD only, no longer `LV2_STATE_IS_PORTABLE`, because the DSP's bag can hold file paths. (8a8c5b5)
- CLAP and VST3: the plugin's own bypass has the fixed id `'byps'` (0x62797073), and VST3's program change `'prgm'` (0x7072676D). They were numbered after the parameter table, so a version that appended a parameter moved them and saved bypass automation drove the new parameter. Sessions saved with a plugin built on the old numbering lose their bypass automation once, and their VST3 program-change automation with it; parameter values and state are unaffected. (5fa6fb6)
- LV2 port layout:
  - A main bus wider than two now numbers its port symbols after `l`/`r`; quad builds repeated `in_r` before. (13ca450)
  - An effect whose DSP reads MIDI gains a MIDI input. It comes after the control, enable, latency and freewheel ports and before the audio ports, so the audio ports and everything after them shift by one index. (cc2cca0)
  - A DSP with `setTransport()` gains a `time_in` port. It is appended last, so no other port moves. (a793f43)
- AU identity: an AU built without `SUBTYPE` and `MANUFACTURER` still registers as `Snr1`/`Sonr`, listed as "Sonorie: <target>" with bundle id `com.sonorie.<target>`, and configure now warns about it. `sonore_add_au` takes `NAME`, `VENDOR` and `BUNDLE_ID`, and `TYPE` defaults to `aufx`, so an instrument that omits it is filed as an effect. The defaults are unchanged, so existing builds register as before. Giving a shipped AU new codes changes its registration. (e1e2ccf)
- Windows: paths outside the ANSI code page now reach the real file or folder. Before, files were written to and read from a mojibake-named twin folder, or failed; on a profile with such a name this affected `%APPDATA%`, settings, logs and the licence. Data written to such a twin before the upgrade will not be found at the real path. (e165c22)

#### Host-visible behaviour

- An Audio Unit reads `kAudioUnitProperty_AUHostIdentifier`, so the host's name and version reach `setHostInfo()` as they do in the other formats, and `PluginHostType` can tell Logic inside AUHostingService. Compiled against the AU shims only; not yet run on a Mac. (0d7d72a)
- Round 36: a hosted CLAP plugin whose note port speaks only the CLAP dialect answers `acceptsMidi()` true, as its notes were delivered all along (3924576). On Windows the MIDI and ASIO device lists give UTF-8 names, so a non-ASCII ASIO driver name saved before must be picked again (1fe852b). A `ComboBox` in a scrolling view leaves the mouse wheel to the view, as its `WheelMode` said; `setWheelMode(WheelMode::Always)` gives the old behaviour (93f1cd7). A `UserPresetStore` with no plugin id keeps each preset's own id and refuses to save or import one with none (d1ca7c8).
- A web editor's page on Windows is served at an address of its plugin's own, `https://<plugin id>-<hash>.sonore.invalid/editor.html`, where a page up to 1 MiB had no address (it went by NavigateToString). A page that calls `location.reload()` now reloads, where it came back blank, and `localStorage` and IndexedDB work, kept per plugin and across sessions, where they threw. A page that relied on `location.href` being `about:blank`, or on storage failing, sees the change. Linux and macOS are unchanged. (2207d5b)
- VST3 event buses report 16 MIDI channels (they reported 1). (b1d2b67)
- VST3 mono: the wrapper's default mono bus is `kMono` (was `kSpeakerC`), and speaker arrangements are translated into SDK channel roles. On a mono track the DSP was told its channel was "top side right", which made a ChannelMatrix go silent. (818707e)
- MIDI output:
  - VST3 now sends controllers, channel and poly aftertouch, pitch bend and program change (`kLegacyMIDICCOutEvent`, `kPolyPressureEvent`). It sent only notes and SysEx. (511fd47)
  - Every format orders short messages and SysEx by time. (511fd47, bdd8114, f783792)
  - Every format writes each short message at its own length; a clock went out as `F8 00 00`. (f783792, 9859bb5, 1708ee7)
  - CLAP's 64-bit path now sends MIDI out, latency and tail changes and dirty marks. (bdd8114)
- CLAP preset load writes the parameters directly and calls `rescan(CLAP_PARAM_RESCAN_VALUES)` instead of emitting PARAM_VALUE events, so hosts no longer record a preset load as automation. Presets past 255 parameters now load whole. (75bdac5)
- Named choices (whole-number steps, every step named) set `CLAP_PARAM_IS_ENUM` and appear as menus in AU, through the Indexed unit and value strings. AU now answers parameter text in both directions. (0526674, 584175d)
- Parameter text keeps at least two significant figures ("0.40 ms", not "0 ms") and always uses '.' when printing. Parsing accepts '.' or ','. Only the control's own unit may follow the number ("5 s" on a ms control and "12abc" are refused; "2.3k" on a Hz control works). Bypass text accepts only on, off, 1, 0, true, false, yes or no. (602f870, 0526674)
- LV2 TTL publishes defaults, ranges, preset values and scale points with as many digits as the float needs (6-9); `%g` rounded them. (e319187)
- AU:
  - StreamFormat accepts only 32-bit float, non-interleaved unless mono. (9859bb5)
  - A sidechain reports its own declared width. (e56b867)
  - Latency changes and state changes now fire the Latency and ClassInfo listeners. (d392e70)
  - `kAudioUnitProperty_OfflineRender` and `kAudioUnitProperty_HostCallbacks` are accepted. (705971c, 8a40e79)
- Editor kind: when the WebView2 runtime is missing, web editors open the native editor instead of an error page; the standalone says why its window is empty. (5a45805)
- Keyboard:
  - On Windows, a focused web editor passes character keys the page did not use, such as the space bar, to the host. (db914db)
  - On macOS, the native editor passes keys no widget takes to the next responder. (42a15d5)
  - The native editor takes OS keyboard focus only while text is being typed, then hands it back to the host window. (a850688)
  - Tab and Shift+Tab move focus inside a native editor; Tab goes on to the host only when nothing is focusable. (4f987fe)
- PlateReverb, FdnReverb and SpringReverb `tailSamples()` are computed in double and capped at INT_MAX. A long or frozen decay reported a large negative tail. (92c8786)
- VST3 no longer sends `restartComponent(kLatencyChanged)` at every activation of a plugin with latency. (32cc901)
- MIDI-CI Discovery advertises property exchange (0x08); it advertised protocol negotiation (0x02). (b2369ad)
- The standalone refuses a `--render`, `--play`, `--shot` or `--seconds` length that is not more than 0 and at most 3600 seconds, with exit code 2. (70dcf2c)
- The editors' VU needle (`sonore.vu`, `MeterState::vu()`) moves like a coil: critically damped, starting from rest, 99% of a step in 0.35 s and never past it, as the browser preview's needle does. It was a one-pole, which leaves at full speed: on one programme signal it crossed 9 of a meter face's 100 frames in a repaint where the preview's crossed 5. It now stops at 1, the top of the -20..+3 VU scale, where it went to 1.2. Gain reduction (`sonore.gr`) rides the same movement, so a compressor that moves 12 dB inside one repaint swings the needle instead of jumping it. A NaN or infinite reading cannot leave the movement NaN.

#### Drawing

- `Grid`: an `auto` track beside `fr` tracks is the largest `width` or `height` (plus margin) of an item that sits in it alone, and 0 without one, as in the reference -- `GridItem` gains `width`, `height`, `withWidth()` and `withHeight()`. It was half an even share. Grids with no auto track lay out exactly as before (6,000 random layouts compared). A layout with an auto track beside an `fr` one needs its items' sizes. (12d63e9)
- A rotary `Slider` with `RotaryDrag::Horizontal` shows the left-right cursor, and a cursor the plugin set is no longer replaced by `setRotaryDrag()` or `setStyle()` (`setStyle()` reset it). (3c776da)
- An SVG document that would cost far more than its size now stops at a budget and reports "drawn in part": style rules and gradient stops count against 4M units, and attributes read again on every `<use>` against 64 MB. Ordinary documents render as before (364 real and fixture files, pixel for pixel). (386c8f8, ad99da5, 4c6e61a)
- A dash that runs through the start of a closed outline is one dash turning the corner, as browsers and resvg draw it; it was split in two and capped, which left a notch at that corner. (c635423)
- Text with right-to-left letters, combining marks or joiners is laid out by the bidirectional algorithm and shaped, so its width and look change: Arabic joins and ligates, marks sit on their letters with no width of their own, a letter and its combining accent become the font's precomposed glyph, and Latin inside such a string takes the font's ligatures and contextual forms. Text without any of those is drawn exactly as before. (0961847)
- SVG strokes now use the file's caps, joins and miter limit. SVG's defaults are butt and miter; every stroke was drawn with round ones. Stroke widths and dashes scale with the transforms above the shape and with the scale the editor is drawn at, and `Drawable::draw` clips to the area it is given. (5631ef0)
- A radial gradient under an uneven scale or a skew is an ellipse; it stayed a circle. A gradient position that is not a number paints nothing instead of the first stop, and two stops at one position keep the order they were added in. (21d574f)
- `IconButton` draws an icon's `currentColor` in `LookAndFeel::iconButtonColour` -- the text colour, the accent when toggled, dimmed when disabled. It drew it black. Icons with their own colours are unchanged. (756a0af)

### New

- Synthesisers on `VoiceManager` (dsp.h):
  - a stolen voice fades out over 2 ms under the new note, rendered ahead so the steal does not click (`setSampleRate()` times it);
  - stereo voices, through `render(float& left, float& right)`;
  - Poly, Mono and Legato (`setMode()`), with last, lowest or highest note priority over a stack of held keys, so a trill returns to the key still down;
  - `setPolyphony()`, for a Voices control;
  - `applyMidi()`: notes with their channel, both pedals, All Notes Off and All Sound Off, the pitch wheel at `setPitchBendRange()`, channel pressure and the mod wheel;
  - `renderVoices()`, a whole instrument block: every MIDI event and every note expression (`applyExpression()`) at its frame, the voices rendered between them;
  - `drainEndedNotes()`: each note that has finished sounding, by key, channel and the host's id where `noteOn()` was given one.

  A voice's optional methods (`legatoTo`, `setPitchBend`, `setTuning`, `setPressure`, `setBrightness`, `setModWheel`, `kill`) are found at compile time; a voice with only the four required ones builds as before. `Portamento` (synth.h) glides a pitch over a fixed time or at a rate per octave. A DSP's `float pitchBendRange() const` sets how far a keyboard that is not MPE bends an MPE plugin. The example synth uses all of it: Voices, Mode, Glide, Bend Range and Spread controls, stereo voices, and vibrato on the mod wheel. The native editor docks its on-screen keyboard at the window's foot, where it stays in view however far the controls scroll.
- AAX Native, for Pro Tools (`aax_wrapper.h`; `sonore_add_aax()`, and `AAX` in `sonore_add_plugin` and the project templates): mono and stereo, effects and instruments, MIDI in and out, the key input, aux output stems, latency reported and re-reported as it moves, Pro Tools' master bypass through the shared crossfade, the SNRS state as the plugin's one chunk -- so a session restores the same sound as the CLAP and VST3 builds -- and both editors embedded in Pro Tools' window at the window's DPI. Built against the developer's own copy of Avid's AAX SDK, which `SONORE_AAX_SDK_ROOT` names and the repository never carries; without it the target is skipped and says why. The AAX library is compiled from the SDK's sources rather than through the SDK's CMake project, which would take over a consumer's install prefix. Every example builds on Windows against AAX SDK 2.9.0 with MSVC, without a warning in our code; macOS is not built yet. Not AAX DSP, AudioSuite, page tables or main buses wider than stereo. The feature map's AAX row, a guard against a wrapper written without the SDK, now claims the wrapper.
- Documentation: ten task guides in `docs/guides` -- getting started, parameters and state, presets, the native and the web editor, MIDI and MPE, hosting, testing, platforms and formats, versions -- whose code blocks the test suite builds (at /W4 /WX with MSVC) and, where a block says so, runs. An API reference generated from the headers' own comments, grouped in 11 areas: `npm run docs` (Doxygen 1.9.8; `npm run docs:fetch` on Windows) fails on any warning about the headers or guides, on a broken link, and on a `sonore::` name a guide uses that the reference does not document. CI builds it in a `docs` job and keeps it as an artifact. Every public class has a doc comment, except two in the licence code; it was 324 of 950 without one.
- User presets (user_presets.h): a folder per plugin -- `Documents\<Vendor>\<Plugin>\Presets` on Windows, `~/Library/Audio/Presets/<Vendor>/<Plugin>` on macOS, `$XDG_DATA_HOME/<vendor>/<plugin>/presets` on Linux, `SONORE_PRESET_ROOT` overriding all three -- of files that hold the plugin's state exactly as a host saves it, with a name, author, comment and tags, a version and a checksum. `UserPresetStore` saves, overwrites, renames, deletes to the trash, duplicates, imports (a .vstpreset for the same plugin too), exports and rescans; `PresetManager` lists factory and user presets together, steps through them and knows when the sound has been changed since the load. (3142887, 1f57a31)
- `PresetBar` (gfx/preset_bar.h) in the generic native editor and the showcase: the name with a modified marker, previous and next, save, and a menu of everything above. A preset load is one undo step (`AudioProcessorValueTreeState::editWholeState`), and reaches the host as a state restore does -- CLAP rescans its values, VST3 hears kParamValuesChanged, AU its parameter and preset notifications. (3142887)
- CLAP preset discovery: hosts that index presets see the user folder and the factory presets, and load them through preset-load. clap-validator's preset-discovery tests run again -- on Windows from a `//?/` root, because clap-validator 0.4.1 accepts only locations that start with a slash. (3142887, 1f57a31)
- The browser build counts what its output guard silenced: `sonore_output_guard_trips()` and `sonore_output_guard_trips_at(h)`; a plugin that sets `guardOutput = false` is compiled with `-DSONORE_GUARD_OUTPUT=0`, since the wasm glue never sees the descriptor. LV2 declares `log:log` as an optional feature. (4324386)
- A plugin's own native editor: a DSP that declares `createEditor(const gfx::EditorHost&)` returning a `gfx::AudioProcessorEditor` (plugin_editor.h) gets it in CLAP, VST3 and AU, with the window's tick, resizing, drop-down menus, tooltips and font -- before, a plugin had the webview or the generic editor. (8314c76)
- Two examples that use what the gap work built: `examples/showcase`, a 4x oversampled waveshaper whose editor is the SDK's own widgets -- APVTS attachments, a menu bar of presets and undo, LookAndFeel_V4 schemes, a colour selector, an SVG logo with drawn text over a JPEG, and a curve typed as an expression, compiled off the audio thread into a lookup table; and `examples/player`, a desktop application -- single instance, menu bar, the system decoders, AudioTransportSource and AudioSourcePlayer to the default output, a waveform, recent files and remembered settings, and a --verify mode that plays through a null device and checks every sample. (f342ad0, 630ed39)
- Audio Units hosted (9e7e8b5): host_au.h's `AudioUnitPluginFormat` and `AuPlugin` scan the component registry and load AUs as host.h loads the other formats -- stream formats, rendering with input, MIDI, parameters with listeners, state as ClassInfo, presets, latency and tail, bypass, the Cocoa or AUv3 editor. Compiled for macOS against the repo's shims and NOT run: there is no Mac here. `mac_host_selftest` is the program to run on one first.
- `AudioWorkgroup` and `WorkgroupToken` (5a22831, audio_workgroup.h): a real-time `ThreadPool` whose helper threads join the audio workgroup the device (the standalone) or the AU host gives them, and a DSP that declares `workgroupChanged(const sonore::AudioWorkgroup&)` hears of it. CLAP, VST3 and LV2 have no workgroup to give. Compiled, not run.
- `NSViewComponentWithParent` (584bba8): any NSView -- a hosted AU's editor -- inside a component tree. Compiled, not run.
- macOS VoiceOver (c1574e3, c1fb567): gfx/accessibility_mac.h bridges the accessibility model the Windows UI Automation bridge reads -- one role table for both now -- to NSAccessibility, and a screen reader can press, step, open the menu of and set the value of a control on every platform. Compiled for macOS, not run.
- Input methods on macOS and X11 (7712fbf, c1574e3): the Cocoa view is an NSTextInputClient and the X11 peer opens an XIM context, both feeding `TextInputSession` (gfx/text_input.h), whose composition TextEditor and TextArea draw inline. Compiled, not run.
- `BluetoothMidiDevicePairingDialogue` (a8f8a16, bluetooth_midi.h): the system's own pairing dialogue -- CoreAudioKit's on macOS, Settings' Bluetooth page on Windows.
- `JavascriptEngine` (e0dc81f, javascript.h): an interpreter of a JavaScript subset, written here -- closures, prototypes, template literals, try/catch, the usual built-ins and the reference's additions -- with limits on time, stack, memory and lengths a script cannot catch, and `stop()` from any thread. `JsVar` is the reference's `var`, and `DynamicObject`; both convert to and from `JsonValue`. 40,000 random programs print what node prints.
- Applications (216ea5f, 8a4d503, 4cb37be): `Application` and `SONORE_START_APPLICATION` -- one instance with the command line forwarded, a quit asked for at the end of a session, on Ctrl+C, Cmd-Q or SIGTERM; `MessageManager`, `MessageManagerLock`, `CallbackMessage`, `MessageListener` and `LockingAsyncUpdater` (message_manager.h); `TopLevelWindow`, `ResizableWindow` and `DocumentWindow`, its menu bar a `MenuBarComponent`; `NativeMessageBox`, `MessageBoxOptions` and `ScopedMessageBox`; `ThreadWithProgressWindow`; `SplashScreen`; `SystemTrayIconComponent` with local notifications; `FileBasedDocument`, `ApplicationProperties`, `RecentlyOpenedFilesList` and `PreferencesPanel`; and `HWNDComponent`, `ActiveXControlComponent`, `NSViewComponent` and `XEmbedComponent` for a native control inside a component.
- Application furniture (5820f55, 30b2896, 79d254b): `ColourSelector`; `Toolbar`, `ToolbarButton`, `ToolbarItemFactory` and `ToolbarItemPalette`, customised by dragging items between toolbar and palette and saved as a string; `MenuBarModel`, `MenuBarComponent` and `BurgerMenuComponent` over `PopupMenuOverlay`, a menu drawn inside the editor and driven by the keyboard (F10, Alt+letter, the arrows); `SidePanel`; `MultiDocumentPanel` with tabs or windows; `KeyMappingEditorComponent`; and the file chooser's parts -- `FileListComponent`, `FileTreeComponent`, `FilenameComponent`, `FilePreviewComponent`, `ImagePreviewComponent`, `FileSearchPathListComponent`, `FileChooserDialogBox`. `KeyPress` gains F1 to F12 on all three platforms.
- JPEG and GIF (6de3fbf): `JpegDecoder` -- baseline and progressive, every sampling, restart markers, CMYK and YCCK, EXIF orientation -- matching libjpeg-turbo pixel for pixel on every fixture, and a baseline `JpegEncoder`; `GifDecoder`, every frame composited with its delay and disposal. `ImageFileFormat` tells PNG, JPEG and GIF apart by their bytes. `ImageCache`, `ScaledImage`, `ImageType` / `NativeImageType` / `SoftwareImageType` and `Parallelogram`.
- SVG text (a2caf47): `<text>` and `<tspan>` are drawn, their faces chosen by `FontMatcher` as CSS chooses -- registered faces first, then the system's, variable weights on the axis; `FontMatcher::EmbeddedOnly` draws the same text on every machine. Each run is a `DrawableText`.
- The pull-model `AudioSource` family (245b06d, audio_source.h): memory, file, tone, channel remapping, IIR, reverb, resampling, mixer, `BufferingAudioSource` reading ahead on a `ReadAheadThread`, `AudioTransportSource` and `AudioSourcePlayer`. No callback takes a lock; start, stop and seek are ramped; checked sample for sample against Biquad and Reverb, and to -89 dB against Resampler.
- Component infrastructure (9ccb1a1, 14be044, d85af39, 9cb080a): `setBufferedToImage` -- a component painted once and drawn from its picture, identical at fractional scales -- and `ComponentPaintDiagnostics`; `ComponentListener`, `MouseListener`, `ComponentMovementWatcher`, `MouseInactivityDetector`; pens and fingers -- `MouseEvent` gains pressure, tilt, rotation, eraser and a finger index, from WM_POINTER on Windows and tablet events on macOS; text dragged between components and to and from other applications; `LookAndFeel_V1` to `V4`, V4 with its dark, midnight, grey and light schemes; `DrawableComponent`; relative positioning (`RelativeRectangle`, `MarkerList`) with cycles refused; `ComponentBuilder`, a tree built from JSON and kept in step with it; `SpringEasingOptions`, `AnimatedPosition` with momentum and page snapping, `ViewportDragScroller`; and `VBlankAttachment`, a callback once a display refresh.
- `CodeEditorComponent` (def1940, gfx/code_editor.h) over `CodeDocument` -- lines that keep their own endings, positions that move with edits, undo by transaction -- with `CPlusPlusCodeTokeniser`, `LuaTokeniser` and `XmlTokeniser`. 100,000 lines open in about 30 ms and take a keystroke in about 4.
- The system layer (778758c, 62de506, 603f6d2): streams.h and file_types.h -- the `InputStream`/`OutputStream` family, `FileInputStream`, `TemporaryFile`, `MemoryMappedFile`, `WildcardFileFilter`, `FileSearchPath`, `MountedVolumeListChangeDetector`; net.h -- `IPAddress`, `MACAddress`, `StreamingSocket`, `DatagramSocket`, `URL` by RFC 3986, `WebInputStream` and `URL::downloadToFile` over WinHTTP, NSURLSession or a libcurl found at run time, listeners on the loopback unless told otherwise; process.h -- `ChildProcess` (no shell), `InterProcessLock`, `DynamicLibrary`; ipc.h -- `NamedPipe`, `InterprocessConnection`, `ChildProcessCoordinator`/`Worker`, `NetworkServiceDiscovery`.
- Video, analytics and Box2D (1238796, a54ec3d, 57120c8): gfx/video.h -- `VideoComponent`, `VideoReader` and `CameraDevice`, frames as Bitmaps, through Media Foundation (AVFoundation on macOS), the soundtrack opt-in; analytics.h -- `Analytics`, `ThreadedAnalyticsDestination` (batches, backoff, a rate limit, unsent events kept on disk) and `ButtonTracker`, sending nothing unless a destination is given one; gfx/box2d_renderer.h -- `Box2DRenderer` drawing the plugin's own Box2D 3.1+ or 2.4, nothing vendored.
- OpenGL (c39de0c, db51335): `OpenGLContext` puts a GL surface over a component in the native editor -- on a render thread or the message thread, swap interval, continuous or on-demand repaint, multisampling, `OpenGLPixelFormat`, `OpenGLVersion` 3.2/4.1/4.3 core, shared contexts -- and the component's own painting is drawn over the GL picture. `OpenGLShaderProgram`, `OpenGLTexture`, `OpenGLFrameBuffer`, `OpenGLHelpers`, `OpenGLImageType`, `OpenGLGraphicsContextCustomShader`, `OpenGLAppComponent`; gfx/maths3d.h's `Vector3D`, `Matrix3D`, `Quaternion` and `Draggable3DOrientation`. No GL library is linked: it is loaded when used.
- `AudioProcessorValueTreeState` (value_tree_state.h) over the SDK's own parameters, state and undo: a parameter layout, `getRawParameterValue`'s atomic for the audio thread, listeners called on the message thread, gestures that undo as one step, the state as a StateBag -- and `SliderAttachment`, `ButtonAttachment` and `ComboBoxAttachment` (gfx/value_tree_state_attachments.h) binding the SDK's widgets, so code ported from the reference has its target. (b1c7704)
- `PluginHostType` (plugin_host_type.h): which host loaded the plugin -- from the bundle id, the executable or the name the host reported, with the version from its Windows version resource -- and as which format, read from the module's own path; the reference's `isReaper()`, `isAbletonLive()` and the rest. (451b608)
- LADSPA hosting (`LADSPAPluginFormat`, `LadspaPlugin` in host.h) on the ABI declared from the specification, and `PluginListComponent` (gfx/plugin_list.h): the scanned plugins in a table to sort, rescan and remove, over `PluginCache`, which gains `scanFilesCached` and `removePlugin`. VST2 hosting stays unbuilt: Steinberg no longer licenses its SDK. (0b3445e)
- `XmlElement` and `XmlDocument` (xml.h): UTF-8 XML 1.0 read and written. Nothing external is fetched and no entity expands, so neither XXE nor a billion laughs; size, depth and node count are bounded, and nothing that walks the tree recurses. (258b19f)
- `Expression` (expression.h): arithmetic a user types -- symbols resolved through a `Scope`, the standard functions -- with limits on parsing and evaluation, cycles between symbols detected, and a `toString` that parses back. (b6b2c3b)
- `LocalisedStrings` (localisation.h) reads the reference's translation files; `translate()`, `SONORE_TRANS` and `SONORE_NEEDS_TRANS` look up a process-wide table that can be swapped while it is read. (7b802ee)
- `TextDiff` (text_diff.h): the fewest characters that turn one string into another, by UTF-8 character, within a work budget. (0f34710)
- `StatisticsAccumulator` (statistics.h, mergeable, safe on the audio thread), `SONORE_LEAK_DETECTOR` (leak_detector.h: live instances of a class reported at exit in a debug build, nothing in release) and `UnitTest` / `UnitTestRunner` (unit_test.h) for a plugin's own tests. (a779117)
- The operating system's decoders behind `readAudioFile`: after `enableSystemAudioFormats()` (system_audio_format.h), what the SDK's own decoders do not open -- M4A, AAC, WMA -- goes to Media Foundation on Windows and ExtAudioFile on macOS. Off by default, so a file opens or not by what the plugin contains. (c0c2151)
- `LAMEEncoderAudioFormat` (lame_encoder.h): MP3 written through the user's own lame executable, launched without a shell, its arguments passed exactly, a hang killed, no temporary file left behind. (c371dba)
- `AudioThumbnailCache` (thumbnail_cache.h): waveforms kept by source and bounded by memory; `AudioThumbnail::saveTo` and `loadFrom`. (3839f46)
- `SystemAudioVolume` (system_volume.h): the machine's master gain and mute -- Windows' endpoint volume, CoreAudio's main volume. (63712e3)
- `AudioCDReader` and `AudioCDBurner` (audio_cd.h), on Windows: an audio CD's tracks read as samples, and a disc burned through IMAPI2. Only what needs no drive was run here: the table of contents, the CDDB id, the sector arithmetic and the failures. (4200b0c)
- `BigInteger` (bigint.h): signed integers of any size -- text in bases 2, 8, 10 and 16, little-endian bytes, division with remainder, `exponentModulo`, `inverseModulo`, the greatest common divisor, bit operations. Checked against Python up to 6,144 bits. (1cf40ac)
- `MD5` and `Whirlpool` beside `Sha256` in hash.h (`md5Hex`/`md5File`, `whirlpoolHex`/`whirlpoolFile`), and in crypto.h `BlowFish`, `RSAKey`, `Primes` and `secureRandomBytes`, the operating system's secure random numbers. They are there for compatibility with the reference -- its key strings, Schneier's Blowfish vectors -- and the header says first what they are not: RSA without padding and Blowfish in ECB without authentication keep nothing secret from someone trying. (3434965)
- MIDI 2.0's other packets (ump128.h): Flex Data -- tempo, time and key signature, metronome, chord names, metadata and lyric text across packets -- UMP Stream -- endpoint and function block discovery, info and names, stream configuration, start and end of clip -- and SysEx8 with the Mixed Data Set. Each multi-packet kind has an assembler that keeps interleaved streams apart and refuses a malformed sequence rather than guessing. (0adbce0)
- `MidiMessageCollector` (midi_collector.h): MIDI from any thread placed in the next block at the sample its timestamp implies; lock-free and allocation-free on the audio thread. (14633fb)
- OSC address patterns -- `*`, `?`, `[a-z]`, `[!...]`, `{foo,bar}` -- with `osc::Dispatcher` sending a message to every listener whose address the pattern matches (an exact address is still one lookup), and the `'r'` colour argument as `OSCColour`. (f03672f)
- `MPEKeyboardComponent` (gfx/mpe_keyboard.h): each MPE note drawn moving with its pitch bend and growing with its pressure, its timbre shown; the mouse plays MPE, bend from the horizontal and pressure (or timbre) from the vertical. (e06e6ef)
- `DelayLine::readThiran`, a first-order Thiran allpass read: flat magnitude at every fraction (within 5e-8), the delay exact at DC, its state reseeded when the delay jumps -- the read that does not lowpass a half-sample delay. Each read head keeps a `ThiranState`. (b1167e2)
- `Phase` (phase.h), a phase accumulator in 64-bit fixed point that does not drift: within 4.6e-12 of a cycle after 10^8 samples, where a float accumulator is half a cycle out. (987bffa)
- `Matrix<T>` (matrix.h: pivoted solve, determinant, inverse, Toeplitz and Hankel, in-place forms that do not allocate) and `Polynomial<T>` (polynomial.h). `SpecialFunctions`: the complete elliptic integral K and the Jacobi cde, sne and asne an elliptic filter design is made of. (69ee6ce, 987bffa)
- `TextArea` (gfx/text_area.h): a text field of several lines -- wrapped where Unicode's line breaking algorithm allows, up and down keeping the caret's x, Home and End by line, Page Up and Down, a selection across lines, a vertical scroll that follows the caret, every line break kept as LF whatever the clipboard brought. `Font::wrapLines` gives wrapText's lines as character positions. (8ba751c)
- Emoji are drawn in colour. `Typeface::colourLayers` reads COLR version 0 and CPAL, `Typeface::colourImage` the PNG pictures of sbix and CBDT from the strike HarfBuzz takes, and `Font::draw` draws them -- a picture from the strike the size on the screen wants, through the transform. `emojiTypeface()` is the system's colour emoji face (Segoe UI Emoji, Apple Color Emoji, Noto Color Emoji), and an emoji asked for as one -- by default, or with VS16 -- comes from it, as a browser chooses. (078203b, 0113e72)
- `loadTypefaceFile` maps a font file rather than reading it, and `Typeface::load(bytes, size, keepAlive)` reads a font where its bytes already are -- data compiled into the plugin, without a copy. (adcfd3c)
- `Typeface::variationGlyph` reads a font's variation sequences (cmap format 14), and the shaper takes a character and its variation selector as HarfBuzz does: the font's glyph for the pair where it lists one -- an emoji's emoji or text presentation, an ideograph's variant. (1c1312d)
- `Font::withFeature(tag, value)` turns an OpenType feature on or off for all of a font's text: `withFeature("tnum")` gives a value readout tabular figures, which do not jitter as its digits change; `withFeature("liga", 0)` no ligatures; a value above 1 picks an alternate. They are merged into the shaper's plan as HarfBuzz merges a user's -- 24,000 strings under random sets of features come out as HarfBuzz's. Fractions are automatic, as in HarfBuzz: the digits around U+2044 FRACTION SLASH take numr, frac and dnom. (38b9585)
- `findLineBreaks` (gfx/line_break.h) marks each boundary of a text as no break, a break opportunity or a mandatory break by the Unicode Line Breaking Algorithm, UAX #14 of Unicode 15.0 -- all 7,654 cases of Unicode's LineBreakTest -- and `Font::wrapText` wraps by it. (39a4a1c)
- `Font::layoutCharacters` returns a `CharacterLayout`: each character's left and right edge in a line of text, its direction, and whether a caret may stand before it, from the same walk that draws. (ea87652)
- The Universal Shaping Engine: Balinese, Javanese, Tibetan, Sinhala, Mongolian, N'Ko, Adlam and the other scripts HarfBuzz sends to it are shaped as HarfBuzz shapes them -- its categories, syllables, repha and pre-base forms, reordering and joining forms. 48,000 random syllables across twenty-one scripts come out as HarfBuzz's, but for Batak's two killers, which Unicode 17 recategorised and the UCD 15 tables here do not. With it, the complex-script shaping gap in the feature map is closed. (d2c99bf)
- The Indic, Khmer and Myanmar shapers (`gfx/text_shaper.h`): Devanagari, Bengali, Gurmukhi, Gujarati, Oriya, Tamil, Telugu, Kannada, Malayalam, Khmer and Myanmar are found into syllables and reordered as HarfBuzz does it -- reph, half forms, below and post forms, conjuncts, the left matra -- under both Indic specifications. 27,000 random Indic syllables on Nirmala UI and 20,000 Khmer and Myanmar come out glyph for glyph as HarfBuzz's. (e893eb5)
- Bidirectional text and OpenType shaping. `Font` lays out a string with right-to-left letters by the Unicode Bidirectional Algorithm (`gfx/bidi.h`, `BidiParagraph`; all of Unicode's 91,707 conformance paragraphs pass) and shapes Arabic, Hebrew and text with combining marks through the font's GSUB and GPOS (`gfx/text_shaper.h`, `shapeRun`), as HarfBuzz does: all of 336,000 random strings on the Windows system fonts come out glyph for glyph the same. Each string is shaped once and kept. The Indic, Khmer and Myanmar scripts are not reordered. (0961847)
- Variable fonts: `Typeface` reads fvar, avar, gvar and HVAR -- `variationAxes()`, `locationFor()`, and `appendGlyph`/`advanceWidth` at a location -- and `Font::withVariation("wght", 650)` draws and measures at it. Matched against fontTools on every glyph of Bahnschrift, Segoe UI Variable, Cascadia Code and Sitka. CFF2 is not read, and kerning stays the default design's. (4033d3b)
- `sdk/include/sonore/sonore.d.ts`: TypeScript types for `window.sonore`, installed with the headers; `npm run verify:bridge-types` holds them to the bridge gui.h generates. (7267df6)
- SVG as Figma, Illustrator, Inkscape and the icon sets write it: `<defs>`, `<symbol>`, `<use>` and nested viewports; linear and radial gradients with every unit, transform, spread and focal attribute; `clip-path` and `mask`; group opacity drawn as a layer; dashes, caps, joins and miter limits; `<style>` sheets with type, class and id selectors; `#rgba`, `hsl()`, `rgba()` and the CSS colour names; `currentColor`, drawn in the colour `Drawable::draw(g, area, colour)` is given. Checked against resvg on 332 SVGs and a file per feature. Text, images, patterns, markers, filters and blend modes are not drawn. (5631ef0)
- `Graphics::clipToPath` (an antialiased clip), `beginLayer(opacity)`/`endLayer` (a group put down as one picture), and `beginMask`/`endMask` (what was drawn becomes a clip, by alpha or luminance). (21d574f)
- `StrokeStyle::dashes` and `dashOffset`, cut by `Stroker::dash`. (21d574f)
- `ColourGradient::setSpread` (`GradientSpread`: pad, reflect, repeat), `setFocalPoint`, `clearStops`, and `transformed` for any affine map. (21d574f)
- `colours::byName` knows all 148 CSS colour names; it knew 24. (5631ef0)
- `LookAndFeel::iconButtonColour`. (756a0af)
- `cmake -DNAME="My Filter" [-DTYPE=instrument] -P sdk/cmake/sonore_new_plugin.cmake` starts a plugin project: `src/plugin.cpp` (a filter or an eight-voice synth), a CMakeLists.txt building every format and the standalone with their self-checks as its ctest, and a README. It works from a checkout or an installed SDK and never writes into a folder that has anything in it, brackets in its path included. (53bcd9f, b8d9b66)
- `Oversampled<>::processBlock(io, n)`, and `upsampleBlock()`/`downsampleBlock()` on `HalfBandFilter`: the half-band cascade a block at a time, bit-identical to the per-sample calls and mixable with them. (87234fb)
- `<sonore/formats.h>`: one include that builds every format the CMake helpers request. (f112e94)
- Optional DSP hook `void reset()`, called by CLAP `reset()` and AU Reset on the audio thread. (a4b0630)
- `MidiBuffer::isFull()`, `getNumDropped()` and `clampPositions()`. (2ea30e4, f081772)
- On Windows a screen reader is told, without asking, when the native editor's focus moves to another control and when the focused control's value changes -- automation included -- at most four times a second with the last value always said. (65f0d54)
- `Typeface` reads OpenType fonts with CFF outlines (.otf, and collections of them), CID-keyed CJK fonts included: Type 2 charstrings with subroutines, hints, flex and FDSelect. It refused them all. The fallback faces now include Noto Sans CJK on Linux and Hiragino on macOS. (dacb08c)
- Kerning from GPOS: the 'kern' feature's pair adjustments, listed pairs and class matrices, in Extension lookups too. Fonts with no kern table set their text with no kerning; where a font has both, GPOS is used, Segoe UI included. (44938f0)
- `fallbackTypeface(codepoint)` and `fallbackFacesRead()` in `gfx/system_font.h`, and `Font::withoutFallback()`. (5ec80f8, 0fbbc1f)
- Each entry of a web editor's `sonore.params` carries `readOnly`; `sonore.set`, `begin` and `end` ignore such a parameter. (da4dddf)
- `MidiSequence::fillBufferFrames()`, for whole-frame blocks that tile exactly. (a3e7af6)
- `shortMessageLength()` in `audio.h`: the wire length of any short MIDI message. (1708ee7)
- `Sysex7Assembler` assembles per UMP group; `group()` and `inProgress()`. (926f709)
- MIDI-CI category bits as named constants. (b2369ad)
- `midiout::FileSink::startLive()`: records MIDI to a file from the audio thread without allocating. (86c2f74)
- OSC: an `open(port, fromNetwork)` option; tags `h t d S c r m` and `T F N I` (as `BoolTrue`/`BoolFalse`, ...); datagrams up to 64 KB. (6223282, 98e8d3a)
- `number_text.h`: locale-independent number formatting and parsing. (15051c9)
- `path_io.h`: UTF-8 file operations on Windows, including paths past MAX_PATH. (e165c22, 043d9e8)
- `WavRecorder::header()`: a WAV header that stays valid past 4 GB. (73dcccd)
- `readWav`, `readAiff` and `readMp3` take an optional decoded-size ceiling. (cfd367a, 06b4fa5)
- FLAC: the decoder reads up to 8 channels; the encoder uses Rice parameters up to 30. (728b0a3, b69d6ba)
- `AudioRing::setPrime(n)`: holds reads until n frames are waiting. (bf0f017)
- Biquad: `copyCoefficientsFrom()` and `getCoefficients()`. (1419947, b9feda1)
- `IirSection`, now in `dsp.h`: a double-precision section with lowpass, highpass, allpass, lowShelf and highShelf designs, plus `bilinearSection`/`bilinearFirstOrder` overloads for it. (153a8cb, f6404a1, c5c845c, 31c9cc3)
- `SvfSection`: a TPT state-variable lowpass, highpass and allpass in double, stable under modulation. (bba9632)
- `matchedCoefficients()` and `cookbookCoefficients()` in `eq.h`. (f6404a1)
- `HalfBandFilter::upsample()` and `downsample()` (polyphase). (fe12f99)
- `WindowedSincKernel::warm()`; the Interpolator's constructor calls it. (9a02f6a)
- `Glide`: a 10 ms one-pole control smoother with `snap()`. DryWetMixer gains `snap()` and `setSampleRate(sr, ms)`. (cb0e717, fa01a80)
- `JilesAtherton::setGlide(samples)`. (8eba79f)
- TapeSaturator: `recordEmphasisBuiltDb()`. (6800c1e)
- FdnReverb: `lineLength(i)`, `maxPreDelayMs()` and `modulationDepthMs()`. (90ee83a, 0a7c314, 74bd1de)
- SpringReverb: `transitSamples(coil)`. (9d741b6)
- StereoDelay: `maxTimeMs()`. (0a7c314)
- EarlyReflections: `droppedTaps(channel)`. (c8b59a1)
- Vibrato: `depthLimited()`. (732e5dd)
- `lowestHz()` on KarplusStrong and PitchDetector. (84cbae4, ce99981)
- LoudnessMeter: `resetChannelWeights()`. (26fec78)
- GrainEngine: `unload()`. (4dd50ee)
- `Random::restart()` and `WhiteNoise::reset()`. (dfd7fa9)
- SampleVoice: `setInterpolation(Interpolation::Sinc / Cubic)`. (9b9027b)
- `Convolver::loadImpulse()` returns the length that will play, which is the tail to report. (92544bd)
- TransportInfo: `loopStartBeats`, `loopEndBeats` and `hasLoop`, filled by CLAP, VST3 and AU. VST3 and AU now fill `barNumber`. (09417f6)
- AU passes the host's tempo, position and play state, and the offline-render flag, to the DSP. (8a40e79, 705971c)
- LV2 passes the host transport (`time:Position`) to a DSP with `setTransport()`. (a793f43)
- VST3: per-note pressure and a note-on's tuning reach an MPE DSP as expressions. (f859285, 82b01e7)
- `isNamedChoice()` and `parseSwitchText()` in `plugin.h`. (584175d, 0526674)
- `webviewMayNavigate()` in `gui.h`. WebView2 now serves pages over 1 MiB from memory. (10f7c55, d36e952)
- GUI: `TextEditor::scrollOffset()`, `ListBox::updateContent()`, a free function `easeCurve()`, and `constrainerFor()` taking the editor's natural size. (dbae49c, 7f03301, 28fc6dd, 053494a)
- host.h: `HostedPlugin::takeRestartRequest()` and `PluginGraph::needsPrepare()`. (70dbef8)
- host.h: `HostedPlugin::takeStateDirty()`, true once after a plugin said its state changed in a way no parameter shows; the host now offers CLAP's state extension. (283624d)
- `sonore_add_au` takes `NAME`, `VENDOR` and `BUNDLE_ID`; `sonore_add_plugin` passes `AU_NAME`, `AU_VENDOR` and `AU_BUNDLE_ID`. (e1e2ccf)
- `clapwrap::pluginDescriptorProblem()`: `descriptorProblem()` plus a check that `numParams` matches `SONORE_NUM_PARAMS`. (920fec3)
- `SONORE_STANDALONE_NO_MAIN` lets a test include a standalone build without its entry point. (a091080)
- Gain reduction reaches the interface. A DSP that declares `float gainReduction() const` (forwarding a dynamics class's own is one line) is read by every wrapper: the web editor gets `sonore.gr` and `sonore.onGR(cb)` in CLAP, VST3, AU, the standalone and LV2; the browser build exports `sonore_gain_reduction()` and `sonore_gain_reduction_at(h)`; and an LV2 plugin gets an output control port `meterGr`, placed after every other port so no existing index moves, and declared only by a DSP that reports one. Nothing measured from the output can stand in for it: it cannot tell compression from a gain knob. A DSP without the hook reads 0.
- Each entry of `sonore.params` carries `options`, `curve` and `group`, as the studio's bridge does: `options` lists a stepped control's positions by name (its value, as text, where a step has none) and is null for a continuous control; `curve` is `'log'` for a skewed control and `'linear'` otherwise; `group` is the parameter's group or null. A page approved in the studio and drawing its stepped controls from `options` threw on `undefined` here.

### Faster

- Performance round 32, every change measured before and after, with the output unchanged (bit-identical unless stated):
  - `Oversampler2x`/`4x` per-sample calls 1.4-1.5x faster, block calls about 1.3x: the half-band's polyphase paths read their taps as straight runs. The saturator example's DSP is 1.15x faster, the showcase's 1.25x. (3c89493)
  - `Fft` 1.5-1.6x faster at 512 to 2048 points (the first two stages in one pass, four butterflies at a time after), and so `Convolver`, `Stft`, `PhaseVocoder` and the spectrum analyser with it: a one-partition convolver 1.4x, the reverb example 1.13x. (815e007)
  - `Convolver` keeps only the half of the spectrum it reads: `Convolver<512, 48>` holds 419 KB instead of 809 KB, and the reverb example 0.84 MB instead of 1.62 MB. (5348ca9)
  - `SampleVoice` reads a mono file once for both sides of a stereo output: 1.3-1.4x for such a voice, the sampler example 1.24x at 48 kHz and 1.40x at 192 kHz. (359d2b0)
  - What every format costs around the DSP, per block with a DSP that does nothing: CLAP 233/265/561 ns at 32/64/512 frames to 166/181/372, VST3 235/256/569 to 172/186/385 (64-bit 258/318/1144 to 166/202/562), LV2 215/236/445 to 157/166/294. The editor's meter and scope feed is one pass with an SSE2 double path (RMS within 1.3e-7 relative in float), and the aux-bus tables are no longer zeroed every block. (8c50f0a, 3307feb)
  - The plugin cache: a cold scan 40% faster (830 to 505 ms for 40 plugins) and a scan with nothing changed, as each start of a host makes, six times faster (18 to 3 ms). It wrote the cache twice per plugin and rewrote an unchanged one, and stamped each file twice. (ef414e9)
  - A graph node costs 35-60% less per block: its input is gathered in one pass and its delay lines resolved at prepare time. (d7f2782)
  - The standalone's ASIO buffer switch converts each channel as one run, the format picked once: 2-4x faster conversions in every format, the whole ASIO callback 33-38% cheaper around a DSP that does nothing (587/1874/3679 ns at 64/256/512 frames to 393/1174/2289). `writeSamples`/`readSamples` are new in `asio_format.h`; every float bit pattern and every integer code converts exactly as before. (c4b74d3)
  - The standalone's WASAPI, CoreAudio and ALSA render has its own stereo and mono loops instead of deciding per frame what the channel count already fixed: a device period 21-26% cheaper in stereo, 26-34% in mono. (4cb0607)
  - The standalone no longer zeroes its aux-bus tables every block, as the wrappers already did not: `processBlock` 20-29% cheaper for an instrument or any DSP that takes a `ProcessContext`. (d2ca727)
  - The native editor repaints 2.7x faster at 1x and 3.9x at 2x (the showcase's full repaint 17.3 to 6.4 ms and 57.8 to 15.0 ms), its first frame at 2x takes 28-31 ms instead of 90-125, and an open editor with a moving meter costs the message thread 11% of a core at 200% scaling instead of 31%. The pixels are the same, bit for bit:
    - a translucent fill, a panel and a drawn image blend a run of pixels at a time, and an opaque image's pixels are copied straight across (91beda8);
    - a gradient along one axis works its colour out once per row or column (741876a);
    - a run under one colour blends four pixels at a time with SSE2, exact in 16-bit lanes, with the scalar path on ARM, wasm and under `SONORE_NO_SIMD` (2172172);
    - text and a shape's anti-aliased edges blend a row at a time, the code editor 1.4x faster (f84f041);
    - Windows gets a repaint's pixels converted to BGRA four at a time (df09a9e);
    - the showcase example keeps its status line's font instead of making a new one, with empty glyph caches, every repaint (119ec71).
- Performance round 33, the waits a user sees: a plugin made, its presets listed, an editor opened, an audio file read or written. Each measured before and after; what is saved, restored, decoded and drawn is the same, byte for byte:
  - A VST3 or CLAP instance of a large plugin is made 6-8x faster (256 parameters: VST3 1.2 to 0.15 ms, CLAP with its remote-controls pages 1.8 to 0.3 ms): the parameter groups are worked out once per module instead of once per parameter info or page. An AU's clumps read the same table. (ce6945e)
  - `host.h` loads an LV2 plugin with many ports ten times faster (179 to 17.8 ms for 256 parameters): each question about a port or preset asks an index of the document instead of reading every triple again. (12c6796)
  - A folder of 300 presets lists 2.3x faster (52-64 to 20-30 ms): a preset's header is read through one open on Windows instead of four calls; on Linux and macOS the check before the open stays, so a FIFO named like a preset still cannot block a listing. (1e1be12)
  - `ensureDirectory` asks once whether the folder is there before walking its path, so a CLAP host making an instance with factory presets no longer pays a refused CreateDirectory per level: the saturator through `host.h` 3.4 to 2.0 ms, and with the listing above 1.3 ms. (c78620a)
  - A web editor's first frame shows the plugin's current values: the bridge's parameter table started at the defaults and the controls moved to their real values on the next tick, 30-37 ms later; `bridgeScript()` takes the current values, and every format's editor passes them. (68650af)
  - A web editor reopened within half a minute opens 3.4x faster (410 to 120 ms): WebView2 shut its browser down a quarter of a second after the last editor closed, and every reopen started it again. A hidden webview that never navigates now keeps it running while any editor is open and for 30 s after the last one closes, then lets go; it is released before the module can unload. The trade is memory: WebView2's processes hold 130-170 MB for those 30 s where they held none. `SONORE_WEBVIEW_KEEP_MS` in the environment sets the grace, and 0 restores the old behaviour. (b6f1532)
  - The system font index, built when the first SVG with text is drawn, is read 2-3x faster (13-20 to 4-7.5 ms; the showcase's cold open 44 to 37 ms): only the tables it needs, on up to eight threads on Windows, merged in the same order, safe under the loader lock. (fcc0973)
  - A FLAC decodes 3-4x faster (a 4-minute file 558-764 to 185-243 ms, 300 one-shots 1.3 s to 0.36 s, the sampler example's load 606-713 to 245-331 ms): the bit reader takes eight bytes at a time and a Rice code's zeros are counted in one instruction. The last eight bytes of a file go through the old reader, so a cut or damaged file is refused exactly as before (40,628 of them checked against the base). (a48bff9)
  - A waveform draws 20x faster (2400 px 70-105 to 3.8-5.7 ms): the rasteriser's whole-pixel box path takes the fractional tops and bottoms each column has. (0dee517)
  - `AudioThumbnail::buildInterleaved` builds a thumbnail from interleaved samples without a per-channel copy; the player example opens a 4-minute WAV in 108-111 ms instead of 153-155. (4c6ecc3)
  - `WaveformPeaks::buildFromFile` reads each bucket's frames as one run: 89-103 to 48-50 ms. (960dbf7)
  - Writing a FLAC is twice as fast (1.62-1.66 to 0.70-0.74 s) and an AIFF 2.5x cheaper in CPU, the rest of its time being the disk; the bytes written are the same. (9d18f4a, 0240084)
- Text is drawn from cached glyph coverage -- each glyph rasterised once per size and quarter-pixel phase -- instead of its outline filled on every draw: "12.5 dB" at 14 px 65 -> 7.5 us, a 24-parameter generic editor 3.7 -> 1.25 ms to paint at 1x and 7.3 -> 3.7 ms at 2x. `Graphics::fillCoverage` paints coverage worked out elsewhere, as a path's is painted. (990857c)
- The rasteriser adds a span's wholly covered pixels as two steps rather than one by one in every sub-row, and stores a run of them in a flat opaque colour as one: a slider's 800-pixel track 64 -> 45 us. (d00fea3)
- Wrapping and line breaking in linear time: a long run with nowhere to break -- Thai, a hash, a path -- 136 -> 2.1 ms for 4,400 characters, an 8,000-character word 620 -> 3.5 ms, 4,000 words in a wide box 2.3 s -> 1.6 ms; 80,000 flags no longer take seconds to find their breaks. Plain text wraps exactly as before. (9d8385e)
- The system and fallback faces are mapped, not read into memory: Microsoft YaHei, Yu Gothic, Segoe UI Emoji and Malgun Gothic cost 0.1 MB of private memory where they cost 57 MB in every process that showed a CJK label and an emoji. (adcfd3c)
Measured against the previous build on one machine (MSVC /O2, SSE2), each bit-identical to before -- except an automated `ParametricEq`, whose band now also settles its smoothers on the target, so its next glide sets out from the curve that is playing (a difference below -70 dB, from the glide after a settle on).

- An open web editor at rest no longer wakes its page thirty times a second with the same meter and load values: 1.25-1.62% of a core in the WebView2 processes before, 0.12-0.37% after. (f27ec64)
- The CLAP wrapper's fixed cost per 64-sample block: 409 -> 244 ns, and 836 -> 269 ns for a DSP that reports latency (the scope, the bypass's dry capture, the channel roles). (8599d00)
- `ParametricEq::setBand()` with unchanged values no longer redesigns the band: 3.8x for an 8-band EQ set once a block. `FdnReverb::setParameters()` with unchanged values: 2.5 us -> 4 ns. `Compressor` below its knee: 1.75x. `SampleVoice`'s cubic read: 2.3x. (8599d00)
- `Fft`: 1.9x, so the Convolver is 1.3-1.6x faster, and the STFT, spectrum analyser and phase vocoder with it. (2ae3e96)
- `PlateReverb`: 1.2-1.4x. (148ff1d)
- `Oversampled<>::processBlock`: 1.8x with a tanh shaper, 2.3-2.4x where the filters are the cost. (87234fb)
- Filling and stroking paths: the rasteriser walks only the edges each scanline crosses, not every edge of the path. On a 400x400 canvas a knob's arc 2 px wide takes 0.39 ms (was 2.39), a 400-point waveform 1.6 ms (was 16.8), and the same waveform 24 px wide with round joins 21 ms (was 98). (97f7efd) Then the active list stays in x order between sub-rows and edges wholly beside the clip are counted rather than walked: a 14 px label fills in 72 us (was 118), and that waveform takes 10 ms. (81d3099)
- A whole generic editor's repaint, on opening, resizing or a change of screen: 24 rows at 2x in 6.6 ms (was 24.4), 64 rows at 1x in 9.8 ms (was 21.8). A rectangle on whole pixels -- every background and most panels -- is filled at full coverage directly instead of through the scan, and a flat opaque colour there is stored rather than blended. Bit-identical. (c5e58e2)
- A label's outline: each glyph is flattened once per size and reused, not flattened again on every draw: 26 -> 10 us at 14 px. Bit-identical. (2a77130)

### Smaller

- Round 34, the memory a session holds, each change measured before and after (private bytes and heap, 50 instances through the raw ABIs or the SDK's host), the audio, the saved state and the pixels unchanged:
  - `MidiMessage` stores its status and two data bytes as bytes, which is all its constructor ever let in: every `MidiBuffer` is 10.3 KB instead of 18.5 and a plugin instance 16 KB smaller (24 KB in LV2). The API and every value it returns are the same. (d7ac3b9)
  - The host bypass's dry ring holds floats for a DSP with no 64-bit path, and an instrument keeps none: the saturator as LV2 at 192 kHz 513 to 299 KB, the synth as LV2 152 to 87 KB. (94d1015, 6139c72)
  - An AU's render scratch is as wide as its bus, and its sidechain slices exist only for a DSP that takes one: 32 KB instead of 160 at a 4096-frame slice for a stereo effect (computed; compiled only, not run here). (b21094f)
  - Examples: the reverb lets its impulse response go once the convolvers hold it (192 kHz: 2.15 to 1.28 MB an instance), every sampler on the demo sample plays one shared copy (262 to 69 KB), and the showcase compiles its three fixed curves once per module (24 KB an instance). (d327fb8, 4d83755, 5bc3624)
  - A plugin hosted by `host.h` costs the host 29-37% less: a 50-plugin session's host side CLAP 11.1 to 7.9 MB, VST3 17.6 to 11.4 MB, LV2 29.9 to 18.9 MB. A VST3 instrument's parameter list is shrunk to what it lists (it kept room for its 2080 hidden MIDI controllers), a VST3 or CLAP effect keeps no room for notes it cannot hear, an LV2 plugin's interface queues are made when its editor first opens, and a graph node keeps a MIDI buffer only when it has a MIDI edge (29 to 6 KB a node). The CLAP host also takes at most its queue's 64 parameter changes a block, so a writer racing the block can no longer have changes dropped or push the event list past its reserved room. (4d6e730, f36ac2d, 5e54b93, bcbb399, f9bf984)
  - An open native editor holds one window of pixels instead of two: Windows is handed them through a strip of at most 256 KB, a band of rows at a time, not a full-window BGRA copy kept between paints (the showcase at 2x 12.9 to 7.4 MB, a generic editor 9.3 to 4.9 MB; a whole paint 2245 to 1764 us). (e260c87)
  - A closed editor gives its memory back. The native editor lives as long as the plugin instance and kept its bitmap after closing (the showcase at 2x 12.5 MB, now 0.36 MB; eight instances whose editors were opened once 98 to 0.4 MB of heap); a web editor kept its copies of the page and the bridge (a 1.4 MB page, 1.4 MB an instance). An editor moved to a lower-DPI monitor gives back the larger frame. (d5369ed, 787c109, 430b7e5)

### Documented

- Round 35, the API reference: the public members with no doc comment went from 6,568 of 10,364 to 1,487 of 10,381, in 228 headers -- the plugin and its parameters, state and presets, the DSP library, the native GUI and its widgets, the application windows and images, hosting, audio files and devices, MIDI and OSC, and the core utilities. What is left is mostly the format wrappers' internals and private records. Each comment says what a caller needs and the declaration does not: units, ranges, what happens outside them or on failure, the thread a call belongs to, ownership, and why. Comments only: every header's code is token for token what it was, checked by stripping the comments from both versions and comparing them. Comments that were wrong were corrected on the way: among them `truncate()` on a stream opened for writing, `isInterestedInDrag` (asked on every move, not once), `FlexItem::basis`, the tape and plate latencies and sizes, and `PluginDescription::format`.
- `npm run docs` counts a member documented as part of a `///@{` group as documented; it left such groups out of its count. (ac139bf)

### Fixed

#### Found by the first CI runs on Linux and macOS

- macOS, `NamedPipe`: a server listened with a backlog of one, and macOS refuses a connect to a full queue at once where Linux waits for room, so the hung-up probe of a second `createNewPipe()` took the only place and the real client after it was refused. The backlog is 8; clients are still served one at a time.
- macOS, `MessageManager`: a message thread moved off the main thread (`setCurrentThreadAsMessageThread`) ran AppKit's event loop there, and AppKit aborts the process on that (creating `NSApp` or asking it for events off the main thread). Off the main thread the loop runs that thread's own run loop, where its wake source already was.
- `JavascriptEngine`: an error raised before a run reached its first node -- out of memory on its first allocation -- carried the line and column the PREVIOUS script had reached ("Line 3, column 54" for a one-line `1 + 1`). Each run starts at line 1.
- macOS, tests: the exports check read no Mach-O and so examined no binary at all; it now reads every binary in a `.clap`, `.vst3` or `.component` bundle with the system's `nm`. The AUs are installed and registered before the tests, which load them through the registry. The generated-project checks accept the CTest summary without ", 0 tests failed", which newer CTest leaves out.
- Linux and macOS, `NamedPipe`: `createNewPipe()` checks whether a name is already served by connecting to it, and that probe stayed in the live server's queue. The server accepted it as its client, saw it hang up at the first read, and the real client's bytes waited behind it. A client that has already hung up is now skipped. A write also blocked past its timeout until the whole buffer was taken; it now returns at the timeout. (5861d85)
- Linux, JavaScript: `Math.cbrt` gives exact cubes exactly. glibc's `cbrt(27)` is 3.0000000000000004. (5861d85)
- macOS: the `sonore_sdk` target links AudioToolbox, AudioUnit, CoreAudio, CoreMIDI and CoreFoundation, which its audio and MIDI headers call directly. A program that used them outside a plugin target failed to link. (5861d85)
- `sonore_new_plugin.cmake` stopped on CMake versions that refuse a regular expression matching the empty string. (5861d85)
- Windows: a WebView2 download that failed once and then succeeded on a retry still failed the configure, because `file(DOWNLOAD ... EXPECTED_HASH)` reports a failed attempt as an error. The hash is now checked after each attempt. (004b39e)

#### Found while documenting every header (round 36)

- What the documentation round found while reading every header, each confirmed first (a check that failed on the base, or ASan, or an argument from the code where no tool here can show it) and fixed with the smallest change:
  - Memory: closing a `SidePanel` drawer after the control it would hand the keys back to was deleted read the deleted control (b877788); `PushNotifications` called a listener removed during the permission answer (7cb6366); a `MountedVolumeListChangeDetector` deleted from its own callback left its thread locking freed memory (5ab799e); `NodalCircuit` stamped a node it never made into the matrix, past `kDim` onto the stack, and refuses it now, as `addNode()` past its limit returns -1 rather than ground (563cca1); `PngDecoder::decode(nullptr, n)` read through null, and a palette failure returned a non-empty bitmap (ee909be); `TabbedComponent::tabName` read past its tabs (e6b9c3f); `HuffmanTable::build` wrote past its table for a code length above 15 (5faef76).
  - Threads: `ProgressBar` gains `setSource(const std::atomic<double>&)` for progress written on another thread (366f9ed); `OpenGLContext::setRenderThreadEnabled()` takes effect at the next attach as documented, and the GL thread's id is synchronised (da6b95e); `makeUuid()` is safe on several threads at once (1cae498); a `ThreadWithProgressWindow` launched again no longer has the old run close the new window or take its completion (1f5d37b).
  - Values: a NaN ratio leaves `Compressor` at 1:1 and a NaN ceiling no longer stops either limiter (ae020a5); every `dsp.h` `setSampleRate()` and `Smooth::setup()` take a rate of 0 or below as 48 kHz instead of dividing by it (256a65c); `JsonValue::asFloat()` saturates beyond float's range (a887bac); parameter names and texts are cut at a UTF-8 character boundary (6ecccef); `MemoryMappedFile` maps a range of any length without overflowing, and nothing for zero bytes (d1bd895); `CpuFeatures::fma` is true only where `avx` is, so the OS's AVX state counts (3f28ed2).
  - Set-up: `DiodeLadder` and `SallenKeyFilter` filter, and `Compressor`, `Expander`, `UpwardCompressor` and `TransientShaper` time their detectors, from construction at 48 kHz, where they were silent or instant until prepared (713f2b2, 82274a4); `SampleStage` prepares each processor for its one channel (976efb3).
  - Files and MIDI: a gap longer than a delta can hold is written so the MIDI file reads back (daaf672); `error()` of `AudioFileReader`, `SampleStreamer` and `osc::Receiver` is about the latest open (c7b090c); the standalone's `FileSource` does not keep a file it refused (c762021); `PresetManager::loadFile` names a nameless preset without its extension (47d8275); `VideoReader::close()` forgets the last file's sound (ce694f2); a `Typeface` refused part-way keeps no metrics (419173f).
  - The editor: `fillAll(Colour)` paints the colour while a tiled image is the brush (98444d8); a horizontal `LinearBar` slider shows the left-right cursor (8885e0c); `MidiKeyboard::allNotesOff` ends the note the mouse holds, with `onNoteOff` (7aa27ba); on Windows the mouse's `MouseInputSource` follows the mouse (0bff40b); `Displays::scaleAt()` reports each screen's real scale on Windows, macOS and Linux (93ced0d); a toolbar item with id 0 dragged in again moves instead of doubling (434d48b).
  - Smaller: the window-method FIR designs take a `FirWindowParams`, so Kaiser's beta can be set (9fe9380); `UndoHistory` keeps no copy of the state `reset()` is handed (e2f2cbd); `system_test` replaced the file it meant to (8842dff); two load-meter checks no longer fail on a busy machine (17f87b8).

#### Found inside a real DAW

- An LV2 web editor in REAPER now always receives the plugin's state. The interface asked for it at every idle, and REAPER carried at most one request per editor opened to the plugin, sometimes none, so the page never had it (the REAPER harness's LV2 probe failed 5 runs of 5). The plugin also sends its state once a second of audio, unasked -- a copy into the host's buffer, nothing allocated -- and the interface passes the page only a state that differs from the last, and gives a reloaded page the one it has. (265509f)
- `verify:reaper` checks that the LV2 editor receives the plugin's state. Until now it only checked that the editor could send. guiprobe's page now answers a state without a host name, which LV2 never gives, so the check fails against the LV2 interface from before 506df0e and passes now. (e1baa97)
- LV2: REAPER listed every Sonore LV2 plugin with no vendor, while the CLAP and VST3 builds of the same plugin had one. The TTL put the author on the plugin's `lv2:project`, which only lilv-based hosts read. The plugin is now also a `doap:Project`, with `doap:maintainer` on the plugin itself. The host's LV2 scan now fills `PluginDescription::vendor` the way lilv does (plugin first, then project); it was always empty. `doap:Project` is no longer counted as a feature. (fa5110f)
- LV2: an instrument is `lv2:InstrumentPlugin` whatever its category says. The arpeggiator, filed as a utility, was listed among REAPER's effects while its CLAP and VST3 builds sat with the instruments. (5c897b9)
- LV2: the editor opens at the plugin's own size, times the host's `ui:scaleFactor`. Every LV2 editor opened at a fixed 720x480 device pixels -- the showcase at 125% came up 576x384 instead of 780x500. (8a20672)
- `verify:reaper` drives the LV2 builds too: scan, renders equal to the CLAP build's at 44.1, 48 and 96 kHz, parameters, automation, a reopened session, latency and editors -- 464 checks across the three formats -- and checks after every session that REAPER's LV2 path names only its staging folder. (f747eb5)
- `LookAndFeel::drawWaveform` drew one bucket a pixel from the first channel only: with more buckets than pixels a transient between them vanished, and a sound only in the right channel was not drawn. Every bucket under a pixel, of every channel, now is. (a1bcd74)
- CLAP: a render could reset partway through the bounce. REAPER switches a running plugin to offline mode, and the wrapper answered with a restart request, which REAPER carried out 0.4-1 s into the file -- synth notes cut, a 512-sample hole in the reverb, the arpeggiator silent for the rest of the render. The offline flag is now recorded for the plugin's next activation without asking for a restart. (1b367c8)
- A parameter value within float noise of zero prints as zero: REAPER keeps VST3 parameter positions in single precision, and 0 dB came back as -0.000 dB where the CLAP build said 0.0 dB. (8d965fe)
- `npm run verify:reaper`: every example, as CLAP and VST3, driven inside REAPER in a profile of its own -- scan, renders against our own host's, automation, a session reopened, latency, editors. It replaces verify:daw, verify:daw-render and verify:daw-state, which staged CLAPs in the shared per-user CLAP folder. (9c12e17)

#### The code that closed the gaps, reviewed (round 24)

Six reviews of what was built in parallel, each finding proven by a probe before it was fixed and kept as a regression check.

- Memory safety: a JPEG Huffman table could be written 100 KB past its end by a 225-byte file; a resampler stepping more than 16 frames (a ratio raised after `prepareToPlay`, or a long block) moved memory with a negative size; a LADSPA port that is both audio and control had a block written into one float; JavaScript identifiers of invalid UTF-8 were read past their end; a chain of bound functions under `new` overflowed the stack; `XmlElement` move-assigned from its own child, the OLE drop target after its window closed, `PushNotifications` after its tray icon, `ThreadWithProgressWindow` while `run()` still ran, a code editor, burger menu, colour selector, text editor or key-mapping dialog deleted from its own handler, and `console.log` printing an object a script freed -- each used freed memory.
- Hangs and deadlocks: a mixer input removing itself deadlocked against a change from another thread; `InterprocessConnection::disconnect()` waited for ever on a peer that stopped reading; an indeterminate progress bar two pixels tall never finished painting; the block epoch's wait was missing a fence, so a read-ahead could overwrite a slot being read.
- Security: Winsock and OSC sockets were inheritable, so a child process kept a closed listener open; a CR or LF in an HTTP header value became a second header, and a port over 65535 reached another port; `ChildProcess` ran `.bat` and `.cmd` files through cmd.exe's own argument rules (now refused on Windows); POSIX lock and pipe names lived in a /tmp every user shares (now the user's own runtime directory, or with the uid in the name, never through a link).
- Denial of service: GIF frames larger than their canvas, SVG text position lists, XML writing and attribute comparison, `AudioCDReader::readTrack`, `ImageFileFormat::readFile`, and in JavaScript string search, `console.log`, value-to-text conversion and array, object and JSON growth -- each now bounded, and the engine's time limit and `stop()` reach inside its built-ins.
- Smaller: WinHTTP kept a handle per request once its last session closed; a CD table of contents could claim an address past 99:59:74; a long relative-position expression summed wrong; folders past MAX_PATH looked empty in the file chooser; a slider attachment destroyed mid-drag left the host a gesture with no end and undo stuck; a transport could skip a new file's fade-in where the allocator reused the old chain's address; a plugin whose path holds a tab was never blacklisted after it crashed a scan; a resized or closed window destroyed native windows embedded in it; the vblank window outlived the plugin's DLL.
- Behaviour changes: `JsVar::toString`, `toJson` and `toJsonString` stop at 8 MB; a `console.log` line is capped at `maximumStringLength`; `CodeEditorComponent::onTextChange` fires once per edit (one call for `replaceAll`).

#### Audio sources

- `BufferingAudioSource` no longer plays a block of silence when its read-ahead thread is preempted while publishing the ring's range. The range went through a one-copy seqlock that read as busy for as long as the writer was stopped mid-write, and a block cannot wait: a full verify on a loaded machine saw 512 underrun frames right after `waitForNextAudioBlockReady()` said the block was ready. `SeqBox` keeps two copies now and the published one is never being written; under a writer publishing flat out, reads that found nothing in four tries fell from 60,869 per million to 0-2. (6c4b642)
- `AudioTransportSource` and `AudioSourcePlayer` could deadlock when a source called `setSource` from inside its own block while another thread was also calling `setSource`, or while the player's device was starting or stopping. The other thread held the lock that every control takes while it waited for the block to finish, and the block was waiting for that lock. The reference allows such a call, because its lock is recursive. A block that only asked the transport for its position at that moment hung the same way. Now no wait happens while that lock is held. A source replaced from inside its block is released by the next call from another thread, or by the destructor. Until now it was never released. A source is never released while a block still plays it. (08c26ee)

#### Presets and state

- `AudioProcessorValueTreeState` undo, round 29:
  - An edit and whatever its listeners edit in answer are one undo step, and an undo or redo records nothing. A listener's edit during an undo was a new step that threw away the redo, so a plugin with one linked parameter could never redo; a listener's write to a parameter the step restores is dropped, so undoing a mode that resets a gain puts back the user's gain. Undo and redo are refused while an edit is under way. (245d4ae)
  - Undoing a parameter edit leaves the non-parameter state alone. It put back the whole `state` bag, losing a tab or a sample written there since; a step now restores it only when an `editState` inside it changed it. (58093d6)
  - A preset loaded through `replaceState` keeps the undo history. A DSP whose loadState is `replaceState`, as a processor ported from the reference has, lost every step before a preset load from the preset bar, and undoing the preset emptied the history mid-undo. A session load still starts it again. (c3b71ed)
- A session saved before an update added a parameter no longer reopens with its preset marked modified, in every format. The saved hash covered the older build's parameters only; it is now checked over those, and a session that was on the preset unchanged reopens unchanged. (df279ba)
- The preset bar's Delete deletes the preset it asked about. Delete and Rename kept the preset's position in the list. When the trash refused the file and the store rescanned a folder that had changed meanwhile, "Delete permanently" deleted the preset next to it, and a Rename retry renamed it. Each step now finds the preset by its file. (522768a)
- A preset or session load is not undone by an editor edit still queued. The editor writes a knob at once and also queues the value, so a knob turned just before a load was put back when the queue drained. The preset showed as modified at once, and CLAP hosts were sent the old value as a live edit. A whole-state write now marks earlier queued parameter edits out of date. Gestures and notes still go through. (1bb9070, cd2baef)
- A second preset of a name already at the 200-byte cap gets a unique name. The cap cut the " (2)" off again, so Keep Both, Duplicate and Import saved duplicates, and each attempt spent 0.8 s looping. (970133b)
- `AudioProcessorValueTreeState` undo and redo set only the parameters their step changed. They reset every parameter, and the host recorded the reset values, which undid automation written since. (a98f745)
- `readVstPreset` stops at 256 MB, the cap the SDK's own preset files have. The preset folder opens only regular files, so a FIFO named like a preset can no longer block a rescan on macOS or Linux. (74ad505)

#### Formats and wrappers (all formats)

- A factory preset never writes past the parameter arrays. A release build loads a descriptor that declares more parameters than `SONORE_NUM_PARAMS`, and loading any of its presets overflowed a stack array: in CLAP's preset discovery and the preset bar, in all three VST3 program-change paths, and in AU's preset paths. All of them go through the bounded `clapwrap::factoryPresetValues`, which refuses such a preset as a stale one is refused. (4272168, ad64150)
- A preset loaded from the web page reaches a block whole, in VST3 and AU. The values went in one at a time and were queued again for the audio thread, so blocks ran on half of one preset and half of the other: 429,561 of 2.42 million blocks in the probe. (09fd218, ad64150)
- The first block after activation never runs on a parameter set torn between two sessions by a state loading beside it; under load it did about once in 800,000 blocks. (71a3226)
- The bypass crossfade no longer leaks the processed signal (about -60 dB, at Nyquist) while bypassed, and no longer reads channels the host did not deliver. (b399215, 126b0a3)
- In LV2 and AU, the bypass's dry path follows a latency that changes while running. Those formats now reserve twice the starting latency, at least 100 ms. (f20f681)
- Event times are clamped into the block. A note-off at or past the block end no longer hangs. (f081772)
- A state or preset loaded while audio runs reaches a block whole, never half-applied. (3e2c8e6)
- Every format prepares through `prepareDsp()`, so held editor keys are released and the load meter and licence gate get the real rate. AU skipped this in initialize and reset, CLAP in `activate()`, VST3 in `setActive`, and LV2 in activate; LV2 then re-prepared at 48 kHz/128 frames on every freewheel switch. (684cd07, 32cc901, becd9d4, 5189d27)
- The editor's CPU meter and scope work in VST3, AU and the standalone. The load meter uses the session rate, and its smoothing no longer depends on block size. (1d5baff, 9b4a7f5, 580f2a8, 48b8d87)
- A web page that loads slowly still receives the plugin's state and parameter values. The bridge now sends `ready`. (722ba33)

#### CLAP

- Round 31:
  - A key's pressure sent as a note expression reaches the DSP as poly aftertouch, as VST3's does; it was dropped unless the plugin declared MPE. (7292a2f)
  - The native editor a web editor falls back to keeps the host's scale: at 200% it was twice the size `get_size` reported. (9c7a1da)
- voice-info reports the voice capacity as the voice count, and no longer claims overlapping notes. It reported the voices currently sounding, which is 0 at rest (the extension requires at least 1) and 1 under one note, which a host may read as a mono patch. Notes reach the DSP by key and channel, so a note-off for one note_id also silenced a held note on the same key. `activeVoices()` no longer feeds any format. (c3d2661)
- A size the host accepts from `request_resize` becomes the editor's size. The wrapper waited for a `set_size` call, which clap.gui does not require, so the host's frame grew while the editor and the saved size did not. (b3f855c)
- The native editor's host bounds a write by `SONORE_NUM_PARAMS` as well as the descriptor's count. In a release build whose descriptor declared more parameters than `SONORE_NUM_PARAMS`, which loads by design, dragging the generic editor's last row wrote past the parameter arrays. (da4dddf)
- At HiDPI, `adjust_size` uses logical limits; it approved half the minimum. (d7b90e5)
- Editor edits call `request_flush`, so they reach the host while nothing is processing. The page's Set also writes the parameter itself. (d2b676e)
- A note-off or choke addressed by `note_id` with key or channel -1 releases the right note. A wildcard channel no longer misses MPE notes, and an id no longer silences the whole chord. A note expression addressed by `note_id` alone also reaches its note. (fe68f57, 1ee4a3e)
- An input with zero channels no longer reads past the host's array. (634804e)
- Notes from zero-length calls and from `params.flush()` are kept for the next block. (84186b2)
- A block longer than the promised maximum is processed in slices, with events and transport moved along with each slice. (15caf39)
- Output channels beyond the DSP's width are cleared. (9649779)
- Changes made while the web editor was hidden are shown when it comes back. (7e796e1)

#### VST3

- Round 31:
  - Every point of a MIDI controller's parameter queue reaches the DSP; only the last one did, so a sustain pedal lifted and pressed again within a block arrived as down only and held notes rang on. A block cut into pieces gives each piece its own points. (a1f0505)
  - A state stream whose last read is short still loads. A read that returned bytes with kResultFalse, as the VST3 SDK's own FileStream answers, was dropped, and every example's state failed to load in such a host. (6acb87e)
  - A preset loaded from the web page is a load to the host (kParamValuesChanged), not a burst of edits a host in write, latch or touch mode records as automation. (f74d9e9)
  - The plugin's own value text reads back as the value: non-ASCII text ("90 °", "Groß") and the program parameter's preset names were refused. (c21ae95)
  - A 32-bit Windows build puts its binary in `Contents/x86-win`; it went into `x86_64-win`, where a host does not look for it. (278f5c6)
- The selected preset and the hidden MIDI controller values are atomic. `process()` and the main thread both touched them as plain variables, a data race a ThreadSanitizer build would report. (6aa0266)
- The plugin asks for the cycle it reads (`kNeedCycleMusic`). A host that fills only the fields it is asked for, as Cubase does, gave the DSP no loop. (22918d0)
- A MIDI program change reaches the plugin. VST3 delivers it as a parameter (controller 130 through IMidiMapping), and the wrapper refused that controller: a VST3 build never heard a program change that its CLAP, AU and LV2 builds did. It is now sixteen hidden parameters, one per channel, in an id range of their own, so no existing parameter id moved. The SDK's VST3 host sends program changes the same way. `MidiMessage` gains `isProgramChange()` and `getProgramChangeNumber()`. (0aaf57d)
- The comment on `Plugin::stateDirty` said it was not atomic; it is. (67bbb21)
- Native-editor knobs now reach the host (`beginEdit`/`performEdit`/`endEdit`), so they record automation and mark the project changed. (db82d2e)
- A webview page's Set and LoadPreset update the plugin's own parameters. Each edit is bracketed in `beginEdit`/`endEdit`, and the preset becomes the current program. (5c8f805)
- At HiDPI, `checkSizeConstraint` uses logical limits; the right-click menu opens at the click when scaled. (d7b90e5, 722a5c0)
- A file picked in a page marks the session dirty. 64-bit hosts get dirty marks too. (6a34db1, 175acfc)
- An input bus with zero channels no longer crashes effects. (5f4a7ad)
- Editor keys pressed during a host's zero-sample flush call are played; they were dropped. (5729f44)
- Reference counts are atomic. The component handler is held with a reference. A view released without `removed()` detaches its editor. (ab81b40, 2dcc8fd)
- A block longer than `maxSamplesPerBlock` is sliced, automation points included. A host that sets up with no maximum gets correctly sized buffers. (15caf39)
- A note-off in a zero-sample call reaches the next block. Extra output channels are cleared. (84186b2, 9649779)

#### AU

- Round 31:
  - An initialised unit accepts its slice size or stream format stated again unchanged, as AUBase does; it refused them. (6d9b803)
  - MIDI and SysEx a host sends from another thread reach the next render whole, through a lock-free queue; they were written into the block's buffer while render read it, and notes were lost or doubled (20,000 pairs arrived as 19,614 to 22,603). (f0a76e6)
  - An editor opened a second time shows its page; every open after the first was an empty view. (4bd1e37)
  - Removing a property listener removes every copy of it, and the call without user data matches any; a host could be called back with user data it had freed. (6d585de)
- `MaximumFramesPerSlice` and `SampleRate` are refused on an initialised unit, as AUBase does. A DSP prepared for 512 frames was handed 4096, and the unit reported 96 kHz while running at 44.1. (f0a9b22)
- Render notifications added or removed while rendering are never half-read: the list sits behind the parameters' seqlock. It was an access violation in 3 runs of 3 against the shims. Run on Windows against the Apple shims, not on a Mac. (5280758)
- `render()` no longer puts a 17 KB MIDI packet list on the host's audio-thread stack on every block. (9502bd4)
- The Info.plist is well-formed XML: its template's comment held a double hyphen, and a VENDOR or NAME with `&` or `<` went in unescaped. A test reads it as XML. (cd72a7e, 75f1432)
- The render callback no longer allocates for sidechain and aux buffers. (9859bb5)
- A restored session reports failure, keeps its preset name and refreshes the host's controls. (d1856be)
- A mono sidechain key is pulled as mono, and the DSP still receives a stereo key. (e56b867)

#### LV2

- The interface offers `ui:resize`, so a host whose plugin window is dragged larger (REAPER, Ardour, jalv) can resize the page, within the plugin's editor limits; it stayed the size it opened at. The SDK's LV2 host offers the feature to the interfaces it loads and resizes them. (ac72723)
- Round 31:
  - Parameter group and aux bus names are escaped in the TTL; a quote or a backslash made the file unparseable and the plugin vanished from hosts. (08d8119)
  - A worker answer the host drops no longer leaks what it carried; `cleanup()` frees it. (96da094)
- The web interface talks to its plugin in the atoms hosts actually carry. It wrote each message as a whole Sequence and read only whole Sequences, but `atom:eventTransfer` carries one atom. So in every standard host its requests never reached the plugin and the plugin's state never reached the page. Traced in REAPER, which passed every editor check: 88 requests, none recognised, and the state delivered and dropped. Requests and file loads are now one bare atom with a body (REAPER drops an empty one), and the page reads bare atoms and whole Sequences alike. REAPER, traced after: every request answered. (506df0e)
- The notify port asks the host for room for the state: `rsz:minimumSize 65536`. The state goes to the interface as one JSON message, and with no size asked for, a host picked its own (4 KB in jalv), so a plugin with enough parameters never reached its own page. (641a74f)
- Every port symbol is unique. A parameter whose id the wrapper uses for a port of its own ("enabled", "latency", "midi_in", "in_l"...), and two ids that map onto one symbol ("gain-db" and "gain_db"), gave two ports one symbol, so a host saving or automating one by symbol reached the other. A clash now gets "_2", "_3"..., and every id that was already unique keeps its symbol. (8d26c30)
- MIDI read in a `run()` of zero frames plays at the start of the next run; it was dropped, and a note-off there left the note hanging. (614df15)
- A freewheel switch re-ran `prepare()` from `run()` while the worker's file load or the host's `save()` was inside the DSP: 599 overlapping calls in the probe. It now takes its turn without waiting, and is put off a run when the lock is held. (31d11d9)
- A worker response older than a `restore()` no longer puts the old file back on the interface while the DSP holds the restored one. (049e0f4)
- The freewheel port is marked `pprops:notOnGUI` and `pprops:notAutomatic`, so hosts that read the properties (Ardour, Carla) keep the host's offline switch out of the generic editor and out of automation. REAPER still lists it. (b57bc49)
- Every control with a unit declares it with `units:unit`: LV2's own unit where it has one (dB, Hz, kHz, ms, s, %, cents, semitones, bpm, octaves), else a `units:Unit` with the plugin's symbol, such as the saturator's "x". LV2 has no call that asks the plugin for a value's text, so before this a host could show only "6000" for 6000 Hz. (ffd4d19)
- The SDK's own LV2 host builds a value's text from the bundle: a scale point's label, On or Off for a toggle, else the number and the unit's symbol. It printed the bare number, with "2" for a step named "Fold". It also marks `lv2:integer`, `lv2:enumeration` and `lv2:toggled` controls as stepped. (ffd4d19)
- The web UI survives the host closing it while its file dialog is open. (b85f0e5)
- `ProcessContext::offline` follows the host's freewheel switch; it was always false. (97eed15)
- Effects that are mono-only connect their output; quad effects no longer read past their ports. (13ca450)
- A run longer than 8192 frames delivers MIDI to the slice it falls in. (58015bb)
- MIDI output from a zero-frame run reads as empty. (3680dd5)
- An atom size near 4 GB no longer passes the bounds check. (febd663)
- The worker no longer races the host's save and restore. Successive worker responses no longer overwrite each other. (d1b38db)
- A `time:Position` stamped mid-run is carried forward from its own frame. (2451128)
- The web UI keeps values and state that arrive before the page is up, and a page that asks for the keyboard gets it. (161fec7, 604306b)

#### Standalone

- Round 30:
  - The page is told which keys are held: `sonore.notes`, `isNoteDown` and `onNotes` never moved in the standalone, from a MIDI keyboard, a file or the page. (363feb0)
  - `PluginHostType` reports the standalone whatever its plugin is called; one named "Sonar Delay" was taken for SONAR, and "Reason Compressor", "Waveform Shaper" and "Resolver" likewise. (6929170)
  - On Windows, a fixed-size editor's window is not cut off on a scaled display: at 125% a 775x375 page got a 638x347 window that could not be resized. (4188fb4)
  - A refused device switch (an unplugged interface) leaves the picker saying nothing plays, with a "None" entry to pick the old device again; it kept naming the old device at 48000 Hz. (e151500)
  - `--render` and `--verify` tell the DSP it is offline, as every format's bounce does. (f48dde4)
  - `--render --input` removes the plugin's latency, as a host's bounce does; the file came out late by it, and a plugin with less tail than latency lost that much of the material's end. (2c7b717)
- Round 29, audio devices:
  - ASIO: a second `AsioOutput`, opened or only destroyed, no longer cuts off the one playing. `stop()` cleared the driver's callback target whichever object held it, and the playing driver looped its last two buffers; `open()` now refuses while another object holds the driver. (4c9b9d8)
  - WASAPI capture: a device of more than two channels converts to the session's rate. Windows refuses a plain WAVEFORMATEX past two channels (E_INVALIDARG, measured on this machine's microphone), so a 4-channel array could not open at another rate; it is asked with WAVEFORMATEXTENSIBLE. (823ab5d)
  - WASAPI: `run()` a second time, or after the device was lost, no longer ends the process (`std::terminate`). (dd0370c)
- Offline (`--render`, `--verify`), a MIDI file longer than 3600 seconds is refused with exit code 2. A 37-byte file with one maximal delta made the program try to reserve 51 TB, and it died. (4ac792a)
- An endless plugin tail (a frozen reverb reports INT_MAX) is cut to 30 seconds in an offline render. Added in full, it reserved about 17 GB. (5e3a823)
- Audio devices: a second `open()` stops what the first one opened. It used to leak the device client and leave COM initialised, in WASAPI out and in, CoreAudio in and out, ALSA in and out, JACK and the Linux output. A WASAPI capture whose mix format is not float is refused with a reason instead of being read as float. (7b06b1c, 8cffae4)
- An option whose value is missing or not a number (`--device Speakers`) is reported instead of quietly playing through the default; `--shot` times itself on a 64-bit clock. (c225f4f) `--midi-input` too, which read `Keystation` as port 0 and remembered it; and a device index must be a whole number, so `nan` or `1e30` is reported rather than cast. (a140058)
- ASIO and the WASAPI capture thread uninitialise COM only where they initialised it: a thread already in the other apartment model was torn down on close. (23dcf02)
- `--help` lists every option; an unknown option is reported instead of silently ignored. (3bf74f9)
- An input device number past the list (on macOS, any number but 0) no longer closes the input at the first output switch. (098bc52)
- `--capture` no longer writes silence while the input's cushion fills: a take could be silent from start to end. (3d4a186)
- Devices are found again by name: a lost output, the remembered output and keyboard, and the input that follows the output no longer open whichever device took their place in the list; an input whose microphone went with a headset is closed rather than moved to another microphone. The picker starts from the device actually playing. (25a3733)
- A keyboard swap releases the old keyboard's notes after its last ones, sustain pedal included; switching inputs no longer holds the interface for up to 1.5 s. (6b0ab29)
- The CoreAudio input's cushion is two IO buffers (was 8192 frames, 170 ms); the WASAPI input list keeps an unnamed microphone's place. (fea65ee, 04afdfb)
- ALSA's card entries open through the plug layer (`plughw:`); JACK sizes for PipeWire's largest period and reopens when the graph's rate changes. (774d5d1, 068d205)
- The standalone's console messages are ASCII. (890cb37)
- ALSA and CoreAudio inputs keep a two-block cushion, as WASAPI's does. (02010e3)
- Reopening an input waits for the audio thread to leave the old ring. (87f50d8)
- A mono output device hears both channels; a device switch keeps MIDI time; WASAPI device indices match what opens. (72b2a8f)
- WASAPI input: the fallback format opens the chosen device, not the default microphone. (5e18cbf)
- The DSP is prepared and run at two channels held to the descriptor's range, so a mono-only or a wider-only DSP gets the width it declared. (9653472)
- A live input follows the output to a new rate; it kept converting to the old one and crackled. (5434f39)
- `--capture` reads by the device's clock, so long recordings no longer gain silent gaps, and a device that stops ends the recording. (17d58f2)
- Closing or switching the MIDI keyboard releases held notes (All Notes Off); a capture device that fails to open is freed. (8ddff07)
- With no audio device open, knob moves no longer replay stale values when one opens (a drag to 20 came back as 13.1), and notes played meanwhile no longer sound all at once. Parameters start at their defaults even when no device opens at launch. (70dcf2c)
- ASIO answers host-message selectors per the published interface and handles a driver reset. A device switch no longer resets every knob to its default. (12c6bf5)
- NaN and Inf output is sent to the device as silence, ASIO formats included; it could reach the speakers as a full-scale click. (7fdb646, bb4397f)
- The first blocks after a device switch no longer allocate. Capture callbacks (CoreAudio, WASAPI, ALSA) no longer grow buffers on the audio thread. (a091080, c4b3e72)
- An unplugged output device is reopened on the system default; WASAPI busy-looped on it. A lost input is closed and shown as closed. This covers WASAPI, CoreAudio, ALSA and JACK. (cdbf08e, 434591c, e57fb35, 24a6bf9, 5502984, 14feac1)
- macOS capture uses the input device and runs it at the session rate. (277649f, 7273611)
- Live input (WASAPI) no longer crackles on start; it is primed by two device periods. (bf0f017)
- `--render` and `--shot` honour their seconds argument. (41a9b1b)
- The command line is read as UTF-16 on Windows, so non-ASCII paths work. (e165c22)
- Choosing "None" in the MIDI picker stops MIDI and is remembered. On macOS the picker opens the chosen source. (eccfc9f, 5207ec9)
- `--midi-out-file` no longer allocates on the audio thread. (86c2f74)

#### wasm

- Pooled instances (`SONORE_MAX_INSTANCES > 1`) no longer overwrite handle 0's MIDI and sidechain key. (353ae14)

#### DSP

- Round 30, dynamics, saturation, delays and reverbs:
  - `TapeSaturator` recovers after one NaN or infinite sample. It stuck in the record-EQ state and the hysteresis then turned every sample to zero: finite silence, which the output guard cannot catch. (1903a15)
  - `FdnReverb`: a size change fades the right channel's second tap with the lines; it jumped, a click 25 times the tone's own step. (de553a7)
  - `StereoDelay` switched to Glide in the middle of a crossfade finishes the fade first; it dropped half its output. (e02afc0)
  - `FetCompressor::reset()` lands its input and output glides, as the opto and VCA did, so renders after a reset null. (0606b26)
  - `DeEsser` keeps the frequency it was asked for through a prepare at a low rate; one prepare at 11.025 kHz left an 8 kHz band at 4.96 kHz for good. (676e713)
  - `DynamicEq` band detectors run in double and read the same level at every rate; a 40 Hz low-shelf detector read 2.35 dB high at 384 kHz. (63fffa0)
  - Ratios glide while audio runs, as thresholds do: the VCA compressor (and every MultibandDynamics band), the expander, and the upward compressor's ratio and maximum gain stepped in one sample. (94538bd)
  - `Distortion`'s bias glides with the drive instead of stepping; a fixed bias sounds as before. (21a5179)
  - The phaser's depth glides over 20 ms when turned. (ca23644)
- `VoiceManager`: a key struck again while a pedal holds its last note releases that note first, as the reference does. Each strike took a new voice and the pedal kept the old ones, so a note repeated under the pedal stacked copies of itself: on a sustaining envelope, four strikes were four voices at full level. (a50a031)
- Round 29, synthesis and modulation:
  - `HardSyncOscillator` keeps its wrap correction in range when the pitch moves. It was worked out from the next sample's increment, so a pitch that fell just after a wrap (a legato note two octaves down) gave one sample of 5.78, and a ratio or pitch of 0 gave NaN. (e96262f)
  - `VoiceManager::voiceForNote` returns the newest voice on a key. A key struck again during its release tail found the old voice, so the new note's bend and pressure went to the dying tail; a sampler note re-struck under +5 semitones played at +0. (c692197)
  - `Vibrato` glides a change of rate or depth over about 20 ms. The read point jumped by whole samples: going from 5 Hz to 4 Hz stepped the output 0.41 against a steady 0.13. (c7cf3d1)
- Round 29, spectral, pitch and resampling:
  - `OversamplerIir` and lofi's `OversampledIir` recover after a NaN or infinite sample. The half-band allpass sections kept the raw value in their feedback state, so one bad sample silenced them for good; a 1 kHz tone read rms 0.0000 thirty thousand samples later. (30a4255)
  - A live `GrainEngine` grain at a negative pitch no longer reads across the write head. Reading backwards uses the oldest audio up at 1 + |rate| samples per sample, not one; at pitch -1 the output stepped by 0.21 where the tone's own step is 0.004. (3c889ba)
  - `Interpolator` reads a NaN or infinite speed ratio as unity, as it already read zero. A NaN ratio stuck in the read position and every later call returned NaN; an infinite one read past the end of the input block. (e8b770e)
  - `LoudnessMeter` leaves non-finite samples out. One infinite sample read +inf LUFS for 400 ms (momentary) and 3 s (short-term), and cast infinity to an int to pick its histogram bin, which is undefined behaviour; one NaN blanked its 100 ms block. (f560176)
  - `fillWindow` with one sample gives 1 for every shape; it divided 0 by 0. (40f416c)
- `Dahdsr` converts a stage time to samples without undefined behaviour. A NaN time, or one past INT_MAX samples, was cast straight to int; it is now held in float first, to between one sample and a billion. (cdecc61)
- `SampleVoice`: a NaN pitch offset crashed the sinc read with an access violation, an infinite one hung `advance()`, and 400 semitones took 8 ms a frame. The rate is now clamped (NaN to 0, at most 65536), and an overshoot wraps with one division. (7fee373)
- `Reverb` above 199 kHz: every comb and allpass clamped to the same length, and the room became one comb at 62.5 Hz with left and right identical. The tunings now shrink together, as `FdnReverb` and `SpringReverb` already did. Nothing changes at 192 kHz and below. (3d4f733)
- `FdnReverb`: a size change that arrived mid-fade dropped the old read head, so a turned or automated size knob clicked every block. The change now waits for the fade. (174f688)
- `LoudnessMeter::process(l, r)` on a meter prepared for more than two channels read past its two samples: a stack overflow under ASan, and +9.95 LUFS where EBU 3341's first case reads -23. (f889d0d)
- `DelayLine::read` and `readCubic` keep the read position's whole and fractional parts apart. A line of 2^21 samples read with an eighth of a sample of error, 3.7e-2 off the exact delayed tone; it now matches a short line to the bit. (f0080a5)
- A NaN frequency or rate no longer latches an oscillator's phase. Oscillator, Lfo, the wavetable, FM and hard-sync oscillators and the frequency shifter put out NaN for good, and the chorus, the delay's modulation and the plate stuck at their shortest delay. `clampf` now maps NaN to the bottom of its range; every other value clamps as before, to the bit. (5c185c7)
- `ADSR` and `Dahdsr`: a NaN or infinite sustain held the envelope at NaN and active forever, so the voice never freed. It is now treated as 0. (263ceb8)
- `Screamer::setDrive` with the drive it already has costs nothing: it refactorised the circuit's matrix every call, 1.3-1.5x the cost of a block. (d7927e7)
- `DelayLine::read` and `readCubic` read a NaN delay as the shortest one. It became an index about 2^31 samples outside the buffer. `besselI0` sums up to 400 terms: at 64 it was 0.4% low at x = 100. (987bffa)
- NaN or Inf no longer latch: `flushDenormal` maps them to 0. This affects filters, envelope followers and dynamics, the phaser feedback, TubeStage, ToneStack, Baxandall, NodalCircuit, PhaseVocoder and the meters. (fb9ef70, 503e3e4, 753ca81, 053979e, 04ae60a, 0c87494)
- Calling `setLookahead` on a limiter with the value it already has no longer clears its state. Applied every block, it used to silence 22-95% of the output. (be043df)
- Re-designing CascadedIir or LinkwitzRileyN with the same shape keeps their state. Per-block re-designs used to wreck the signal. (fa4b09d)
- TubeStage: `setCoupling` and `setCathodeBypass` no longer rebuild and reset the stage. Calling `setCircuit` or `setTriode` with the same values is a no-op. A NaN sample or edge settings no longer crash or ring. (10c85c4, 853b344, 053979e)
- TapeSaturator `setParameters` no longer redesigns its filters on every call. (dbe8b26)
- FuzzFace and TubeScreamer are silent after `reset()`. Knobs set before audio starts no longer thump at +7 dBFS. (0c87494, 567412f)
- NodalCircuit:
  - starts settled with a biased inductor's DC current; (bebea87)
  - is zero-initialised before `reset()`; (a53eccd)
  - limits junctions SPICE-style, so fast swings converge; (f4bb3ed)
  - refuses non-finite steps. (0c87494)
- DiodeClipper no longer locks at about 8e10 V when driven hard at 8x. (04f455f)
- The FET compressor's attenuator no longer jumps where its quadratic has no root. (bb6dc40)
- Limiters use a 64-bit sample clock; a click near sample 2^32, about 24.9 h at 48 kHz, passed through. (2c9b03a)
- MultibandDynamics: a bypassed band's detectors keep running. (2ab337c)
- LoudnessMeter: a caller's channel weight survives `prepare()`. (26fec78)
- DeEsser `reset()` clears its crossover. (b3401d1)
- GrainEngine:
  - an empty or short sample no longer hangs the audio thread; (c8af685)
  - NaN or absurd settings no longer read outside the buffer; (07d61ab)
  - grains near the far end of the live buffer no longer splice in new audio. (b5330ed)
- Wavetable no longer indexes past the table at a phase just below zero. (80ed41a)
- Convolver:
  - `reset()` clears the block being output; (178b91b)
  - loading an impulse while running continues the stream; (6038345)
  - the first load and the last unload no longer drop a block to silence. (b80b853)
- StereoDelay: a time set before `prepare()` uses the new rate. (15280e4)
- Smooth no longer sits in subnormals after settling at zero. (8b39463)
- PitchDetector no longer writes past its buffer when used as constructed, and spreads its analysis over the hop instead of one `push()`. (4549dfd, 1fe361c)
- PitchShifter keeps its requested window after a visit to a high sample rate. ModalResonator re-fits its modes after a rate change. (22fc01b, 4fe2655)
- Chorus, Flanger, StereoDelay in glide mode and TapeSaturator apply parameters set before audio starts without gliding in from the defaults. (4df1046)
- FirFilter normalises a short, low-cutoff lowpass to unity at DC; a 3-tap 5 Hz lowpass was -74 dB. (e450c76)
- SpectrumAnalyser reads DC and Nyquist at their level; they read +6.02 dB. (58fe4ea)
- EarlyReflections counts the images its line cannot hold. (c8b59a1)
- SampleStreamer:
  - no longer drops out on entering a loop; (46d6ded)
  - keeps streaming past the loop after a note-off; (436be50)
  - `setLoop` and `clearLoop` no longer race the worker. (436be50)
- Faster, with identical or near-identical output:
  - ADSR and DAHDSR coefficients; (d06a870)
  - Phaser designs; (1419947)
  - unison glide; (67340ab)
  - polyphase oversampler; (fe12f99)
  - Hermitian convolver; (038d5c4)
  - nodal matrices; (2db974b)
  - triode grid current; (bb6e19a)
  - multiband compensation as one allpass; (69fab26)
  - `DelayLine::readCubic` wrap and the pitch-shifter crossfade; (766b251)
  - TruePeakMeter SIMD. (2c7684a)
- The windowed-sinc table is built on construction instead of in the first audio block. (9a02f6a)

#### File I/O

- Round 29, recording and thumbnails:
  - `AudioRecorder::stop()` waits for a block the audio thread is still copying in. The block was lost from its own take and landed at the head of the next one behind silence, and a ring that `start()` grew was freed under the copy (a use-after-free under ASan). A take can now end and the next begin while audio runs. (500f6a3)
  - A write the disk refuses (full, unplugged) ends the take and is reported. `write()` kept returning true, `framesDropped()` stayed 0, and when the disk took writes again the rest of the file was misaligned noise. Each chunk is flushed before it is counted, and the lost frames count as dropped. (3e20d8c)
  - `AudioThumbnail::toJson` prints a decimal point under any locale; under a comma locale it wrote invalid JSON. (7113dab)
  - A NaN or an infinity in a source file no longer loses a whole `AudioThumbnailCache`. It went into the JSON as nan or inf, and the cache, which rightly refuses those, then refused to load at all. NaN builds as silence and infinity as the largest float. (21eb3b5)
- Round 28, data formats and the script engine:
  - `JsonValue`: `node = node["key"]` and `node = node.at(i)` walk down the tree. The copy assignment freed the child while still copying from it. (295f6b8)
  - `XmlElement::setText` may take its text from a child it is about to delete; it read the text after deleting the children. (f23884e)
  - JavaScript: `replace` and `replaceAll` keep `$` substitutions within the string limit and the time limit. A 1 MB string with 1,000 substitutions peaked at 3.3 GB under a 64 MB limit, and one call ran 24 s past a 1 s limit, ignoring `stop()`. (2c0bccc)
  - JavaScript: for-in and `Object.keys`, `values`, `entries`, `assign` and spread over a string no longer build every index key outside the memory meter. An 8 MB string grew the process by 488 MB under a 64 MB limit. `fuzz_javascript` gains a seed for both. (f53e463, 9ce57af)
- Round 27, audio files. Each fix was proven by a probe that failed first, and the probe was kept.
- `SampleStreamer`: a voice that loops or retriggers backwards never reads a half that is being refilled. The worker could slide the window using the position from before the jump and refill the half the voice was reading, so it played samples from later in the file: 0.8 to 1.1 million wrong frames in each of three soak runs. The window base and per-half readable flags are now one atomic word, and the audio thread never waits. (28ab67e)
- `SampleStreamer`: a loop starting just past a 32768-frame half boundary no longer drops out on every pass. Seeks and the loop pin ignored the voice's 144-frame reach behind the start. (2e3a1ba)
- `readMp3` keeps the last frame of an MP3 that ends in an ID3v1 or APE tag, which is most tagged MP3s; they decoded short, and gapless loops came round early. (0c4862d)
- `readOgg` decodes to the end of the stream rather than sizing from the last page's granule. An Ogg captured mid-stream was refused, or zero-filled to hundreds of MB first. (04df6f3)
- A FLAC behind an ID3v2 tag decodes, as libFLAC and ffmpeg decode it; `readFlac` and `readAudioFile` refused it. (72eaacb)
- The streaming `AudioFileReader` walks past an empty chunk (an empty JUNK, an AIFF's empty NAME) and opens 8-bit and 32-bit integer WAVs. `readWav` already did both. (7ea842b, a820522)
- `writeFlac` refuses a rate STREAMINFO cannot hold. A rate of 0 produced a file nothing opens, and 1,096,576 Hz was silently stored as 48 kHz. (a98551c)
- `readAiff` refuses more than 32 channels, as `readWav` does. `readFlac` and the MP3 and Ogg loaders refuse files over 1 GB, as WAV and AIFF already did. (04ea5be)
- `fuzz_parsers` fuzzes the streaming reader, the sample streamer and real WAV chunks (bext, cue, smpl, acid, LIST). (897d396)
- An Ogg Vorbis stream may ask for at most 64 MB of decoder setup: a 17 KB corrupt file found by fuzzing made `stb_vorbis` allocate and touch 1.36 GB before refusing it. (b7d2465)
- The MP3 and Vorbis decoders' macros no longer outlive `audiofile.h`, on any platform, and a user's macros of the same names survive it: a user's own `CHECK()` or `HAVE_SSE`, or own copy of minimp3 or stb_vorbis, now compiles and links beside it -- stb_vorbis included before `audiofile.h` too. (501d46c, f174eaf, 76412c2)
- FLAC encoder:
  - files are no longer corrupted from frame 128 (10.9 s at 48 kHz); (85353ee)
  - 24-bit files are no longer huge and are readable by ffmpeg. (b69d6ba)
- Streaming reader (AudioFileReader):
  - caps channels at 32;
  - checks the AIFF rate and SSND offset;
  - opens WAV and AIFF over 2 GiB on Windows;
  - refuses compressed WAV and AIFF-C, and WAVs with a zero rate;
  - the whole-file reader now accepts AIFF-C "twos". (a3add13, 7b8d12a, 4b0e9a0, cfd367a)
- Readers:
  - `readWav`, `readAiff` and `readMp3` refuse decodes past `kMaxDecodedSamples`; (06b4fa5, cfd367a)
  - `writeAiff` refuses writes over 4 GB and zero rates; (cfd367a)
  - a stb_vorbis comment count can no longer overrun the heap; (bb38152)
  - a 16-bit palette PNG can no longer read past its buffer; (861bf35)
  - 32-bit builds no longer wrap seven parsers' bounds checks. (c82c4b2)
- Recording:
  - recordings interrupted before `stop()` keep their audio, and readers accept a data chunk that runs past the end of the file; (3eb23f8)
  - a recording past 4 GB gets a valid header; (73dcccd)
  - `writeWav` no longer copies the whole buffer. (49a51db)
- MIDI files:
  - `readMidiFile` no longer hangs or overruns on hostile chunk lengths, and sorts tempo maps spread across tracks; (a3e7af6, a92f049)
  - `MidiSequence` blocks tile exactly, so no event is doubled or lost, and `fillBuffer` no longer allocates; (a3e7af6, 78ea6ec)
  - `writeMidiFile` keeps tempo changes at their ticks and writes only channel messages. (8720d24)
- JSON:
  - output is locale-proof, and `asInt` saturates; (15051c9, 194aca5)
  - malformed numbers, `\u` escapes and Base64 are refused, not half-read; (de12691)
  - `jsNumber` never writes inf or nan and prints the fewest digits that read back as the same float. (366c42d, 73e180f)
- Zip slip through overlong UTF-8 is closed. `isSafeEntryName` also refuses NUL, control characters and colons. (e24a44c)
- `WaveformPeaks::deserialise` no longer overflows on a 16-byte file. (1c2c6b6)
- Inflate's fixed-Huffman tables are built once, so they are thread-safe. (50b5305)
- File lists sort the same on every platform. (366c42d)

#### MIDI and OSC

- Round 30, MIDI 2.0 and SysEx:
  - `Sysex7Assembler` never makes a malformed packet part of a message. A packet with a reserved status (4-F) was taken as a continue and its bytes added to the open message; one claiming more than six bytes was skipped, and the end packet closed the message over the hole. Reserved statuses are ignored, as `Sysex8Assembler` does, and an over-long packet abandons the open message and counts it in `dropped()`. (f8aa74d)
  - On Windows, a SysEx the driver saw cut short (a status byte before its F7, a cable pulled mid-dump) no longer costs the input a buffer. The callback handled only MIM_LONGDATA, so a MIM_LONGERROR buffer was never handed back; after two, the input received no SysEx at all, MIDI-CI included, until it was reopened. (164a07e)
  - A UMP packet's message type and its status byte agree, both ways. `toMidi1` and `toSystem` took a type-1 packet holding 0x93 for a note-on and a type-2 packet holding 0xF8 for a clock; `fromMidi1` wrote a clock or a song position as a malformed type-2 packet, and now writes system messages as type 1. (5b7b481)
- Round 28, MIDI clock, sequences, files and MPE:
  - `MidiClock`: the position is the clocks counted, with the tempo only filling in between them. It added up the tempo estimate over time, so a change from 120 to 80 BPM left it two clocks ahead for good. (5b52f4f)
  - `MidiClock` follows a Song Position Pointer (0xF2). A play from anywhere but bar 1 (Stop, pointer, Continue) carried on from the old position. (574ba3c)
  - `MidiClock::prepare()` keeps a running transport. After a host prepared the plugin again, the clock read "stopped at 0" until the sequencer was restarted. (07e854c)
  - `MidiSequence`: a note keeps its note-off after an edit sorts the timeline again. The pairs were indices the sort did not move, so `noteLength()` returned the wrong note's length. (319795e)
  - `MidiSequence::fromMidiFile` walks the tempo map once instead of once per event. A 1.1 MB file with 100,000 tempo changes took 13.5 s to open; it takes 0.03 s, with the same times to the bit. (66dee01)
  - `MidiSequence::transpose()` moves poly pressure with its note; it stayed on the old note number and reached whichever note moved there. (47b3154)
  - `writeMidiFile` writes a tempo the format cannot hold as the nearest it can (3.576 BPM). 2 BPM read back as 4.54 and 0.5 BPM as 120. (28ff3d6)
  - `MpeDecoder` adds a zone's bend to each note's own bend; it replaced it. `MpeKeyboard` draws a note's own bend, since it adds the zone's itself. (a2a72de)
  - `MpeDecoder` gives a new note the bend, pressure and CC 74 its channel received before the note-on, the order MPE controllers and the SDK's own MPE keyboard send them in. They were dropped. (e28998e)
  - `MpeDecoder` sends a channel's bend and pressure to every note sounding on it. It kept one note per channel, so a plain keyboard's wheel bent one note of a chord, and none once that key was released. All Notes Off and All Sound Off (CC 123, 120) clear the channel's notes. (145193e)
- OSC: one datagram of nested bundles no longer overflows the stack. Decoding recursed once per bundle level with no limit: one 64 KB datagram of 3,275 levels crashed a thread with macOS's default 512 KB stack, and anyone who could send a UDP packet to the port could take the process down. Nesting is capped at 32, and a deeper packet is refused. (9017ecc)
- `osc.h` compiles after a plain `<windows.h>` (one without WIN32_LEAN_AND_MEAN), which had already pulled in the old winsock.h; it was 102 redefinition errors. It uses winsock.h when that came first, and parses IPv4 addresses itself with inet_pton's rule (four decimal parts, no leading zeros). `windows_minmax_test` covers osc.h and the MIDI headers. (a20531b)
- `midiout::FileSink::numEvents()` could be read while the drainer appended: counts were torn or went backwards. (e34f4bb)
- A MIDI device that opened after a failed open still reported the old error, on all six input and output backends. (5906af1)
- The Linux MIDI output opens its rawmidi port non-blocking, because `send()` runs on the audio thread. A full port drops the message and counts it in `droppedEvents()`, now on every backend, and `close()` reports the count. (9383eb7)
- The OSC sender and receiver start Winsock once each and clean it up when destroyed; a sender started it on every open and nothing ever stopped it. (0c42524) Neither can be copied (a copy cleaned up a second time), but both move; opening one again closes its first socket instead of leaking it. (fa374d0, 748f5bd)
- A soft MIDI 2.0 note-on no longer narrows to a MIDI 1.0 note-off. Poly pressure and program change now narrow. (cc03e7e)
- MIDI input:
  - SysEx waits for room instead of vanishing; (31c4230)
  - F4, F5 and a stray F7 never reach the DSP; (31c4230)
  - ALSA and CoreMIDI share one byte-stream parser, one per source. (b8f1ab4)
- MIDI output: a second `open()` closes the first. Windows list indexes match open indexes. (d94e04f)
- UMP SysEx7 streams on different groups no longer merge. (926f709)
- MidiClock ignores dropped and duplicated bytes, and needs two clocks before its first reading. (8d373e8)
- The live MIDI input queue leaves what does not fit in the ring for the next block. (2ea30e4)
- An OSC bundle with one broken element delivers nothing. Strings containing NUL encode safely. (a71a315, 98e8d3a)

#### Settings, logging, threads

- Round 29, core utilities:
  - A `ChangeBroadcaster` or a `MultiTimer` may be deleted from inside its own callback. Both kept reading their own members afterwards, a use-after-free under ASan. (0a487fe, 3d59381)
  - A stream skip by a size near INT64_MAX lands at the end instead of wrapping to the start, so a reader walking chunks past a bad header no longer loops for ever. `FileInputStream` too. (e4f4820)
  - A translation file saved as UTF-16 (with its byte-order mark) loads; it loaded nothing and reported nothing. (b165163)
  - `formatDuration` reads "--:--" for NaN, infinity or a length past what it can show, which was undefined behaviour, and -0.4 s reads "0:00", not "-0:00". (5a3501e)
- Round 28, application infrastructure:
  - On Windows, a second launch reaches a first instance that is still in `initialise()`. The command line went out by a broadcast that waits 2 s and skips windows that look hung, so a file opened from Explorer while the app was starting was dropped. It now goes to the first instance's own window and waits up to 30 s. (c815c64)
  - On Windows, closing the console window runs `shutdown()` and the application's destructor. The handler returned at once, and Windows ended the process first. (2bf2327)
  - `ThreadPool` destroys a job's closure before it counts the job as done. `waitForAll()` returned while the captures were still being destroyed on the worker, racing the caller. (76ec3e3)
  - `getCommonSettings(true)` no longer writes an empty file over a machine-wide settings file that did not load (locked by another process, or unparseable). Such a file counts as read-only. (156598d)
  - `PropertiesFile` and `UserSettings` refuse to save a file larger than their own load accepts (16 MB and 4 MB) and leave the file on disk as it was. Before, every value read as its default at the next start, and the next save made the loss permanent. (f1b390e)
  - On Windows, a worker posting with `callAsync` faster than the message thread runs no longer freezes the UI. Windows hands out posted messages before input, paint and timers, so a 10 ms timer did not fire once in a one-second flood. After 50 ms without the queue running dry, the next batch waits behind them. (3e6dd6c)
  - The command line given to `initialise()`, `getCommandLineParameters()` and a second instance follows the Windows argv rules, so it splits back into the same arguments, with CommandLineToArgvW too. An argument with a space that ended in a backslash ("C:\My Dir\") swallowed its closing quote and every argument after it. (6cc2a58)
  - On Windows, `runDispatchLoopUntil` returns at its time while the OS always has a message ready. A window that invalidates itself as it paints kept it dispatching for ever, so a drawn message box or a progress window over it ran on after it was answered. (d5970be)
- Round 28, IPC and sockets:
  - `InterprocessConnection::disconnect()` shuts the socket before it joins the connection's thread. When that thread was blocked, for example replying to a peer that had stopped reading, `disconnect()` and the destructor waited for ever. (a0c027f)
  - A reconnect from inside `connectionLost` is refused instead of crashing. From the connection's own thread it ended in `std::terminate`; from a `disconnect()` it was a use-after-free. (4a2e067)
  - `InterprocessConnectionServer::stop()` may be called from `createConnectionObject`, as a server that takes one client does; it joined its own thread. (277098c)
  - A pipe message that cannot be written whole ends the connection. A timed-out write the peer had half-read left the stream misaligned: later messages were reported sent and never arrived, and the peer delivered a 1 MB message nobody sent. A send that times out now ends the connection even when nothing was written, because Windows cannot tell the two apart. (132bd7e)
  - On POSIX, a named pipe that fails to open closes its wake pipe. A client retrying every 10 ms for a server not yet up leaked about 200 descriptors a second. (0aa3db8)
  - On Windows, an out-of-band byte is not read as data: the socket was in select's exception set when waiting to read, so the wait returned at once and the read blocked. (9f2bc26)
- Concurrent settings saves no longer collide on one temporary file. A save that does not reach the disk no longer replaces the file. (dc881a1, 6732342)
- Logger:
  - is safe for several audio threads at once; it lost lines and could hang the editor; (86bc029)
  - opens the log file once per drain. (cb26757)
- ThreadPool no longer starts a thread per job. (87afad4)

#### GUI, webview and native editor

- An unactivated licensed build shows its activation overlay, the only place to enter a code, on every open of its web editor (CLAP and the standalone). The tick put it into the blank document the page's navigation replaced and marked it shown; the page's ready message now makes the next tick put it into the page. (d03f225)
- Round 30, images and 2D rendering:
  - A grey or RGB PNG with a tRNS colour key (what oxipng and OptiPNG write) draws the key colour transparent, compared at the image's full depth; it drew it opaque. (00ee590)
  - A gradient built a stop at a time costs its stops, not their square: `addStop` sorted the whole list on every add, and a 20,000-stop SVG gradient took 1.3 s (now 0.012 s). (5453bf8)
  - SVG: each `&` looked for its `;` to the end of the text, so 1 MB of them took 15 s (now 0.02 s). (a211ff2)
  - SVG style sheets cost memory and time in proportion to themselves: an 18 KB sheet took 700 MB (now 4 MB), and a long class list under 4,000 class rules took 16 s (now 0.04 s). Rules from one selector list share their declarations, the cascade holds pointers, and rules are indexed by id, class and type. (386c8f8)
  - SVG gradient stops are paid for in every shape they fill: a 760 KB file took 78 s and 3.7 GB (now 0.6 s and under 50 MB). (ad99da5)
  - SVG: a large path drawn again through `<use>` is paid for each time: a 480 KB file took 7.5 s and 960 MB (now 0.27 s and 38 MB). (4c6e61a)
  - Curves are as smooth under an uneven scale or a skew as under an even one. The flattening tolerance used the transform's mean scale, so a curve strayed 0.48 px at a 1:16 scale against the 0.2 px asked for; it uses the largest stretch now. Even scales, rotations and mirrors flatten exactly as before. (8fbb660)
- The editor's queue to the audio thread keeps its last 64 slots for gestures and notes. Knob edits filled it on a host that was not draining it (a track whose processing was suspended), and a key held on the editor's keyboard while a knob turned lost its note-off and sounded for ever; a gesture lost its end. A refused knob edit loses only the host's report of it: the parameter is written on the main thread too. (b89eea0)
- Round 27, the native toolkit:
  - An editor closed with the mouse button down ends the press as a cancel. A dragged slider left the host gesture open (a VST3 beginEdit never ended), and a held key on the generic editor's keyboard never sent its note-off. (70a4410)
  - A UI Automation element a screen reader holds stays bound to its control. It held a position in a snapshot, so after a control above it was removed it read the next control's name, value and id. A removed element now answers UIA_E_ELEMENTNOTAVAILABLE. (280b791)
  - Button, Slider, RangeSlider, TextArea and TreeView may be deleted by their own handlers. Each touched itself afterwards; ASan found the use-after-free with a harness that deletes 31 kinds of control from every handler. (99fa1c9)
  - Hiding a component takes the keyboard focus from it and its children. A shortcut that switched tab pages left the next keys going into a field on the hidden page. (3081a59)
  - A ComboBox rebuilt while its drop-down is open is not written into after it was freed. (a4b3c9e)
  - `Animator` drops a component deleted mid-animation instead of moving freed memory every tick. (38213bb)
- Round 27, the web editor and its bridge:
  - A key the page takes in its own window listener is no longer also sent to the host. (5a463cd)
  - A focused slider or checkbox passes the spacebar to the host; every `<input>` counted as a text field. (3878fb4)
  - A focused text field keeps Delete, the arrows and Ctrl+V; they went to the host. (bc1674c)
  - A page that had taken the keyboard no longer keeps it for the page rebuilt after a close. (23f7d7e)
  - `sonore.format` prints a value near zero as the host does: REAPER's VST3 0 dB read "-0.000 dB" on the page. (47859fc)
  - A stepped control on the page sits on a step, unskewed, where the plugin puts it. A switch set to 0.4 read 0.4, and ratio 5 sat at 0.46 where the host showed 0.21. (6da5fc3)
  - A page that comes up is told which keys are held, so a reopened editor shows them down. (5202a1c)
  - A page string with a NUL in it is refused. With the LV2 worker it loaded the page's own path instead of the chosen file. (d7db336)
  - The editor's meters fall by the time that passed: the Win32 timer ticks at 21 Hz, not 30, and peaks fell at 70% speed. New `MeterState::tickNow()`. (7258c06)
  - On Windows the page follows its window however it was resized, including when an LV2 host resizes the widget directly. (4d89e39)
- Copies of one `Font` may draw on several threads at once, as two editors in one host do. Their shared glyph and shaping caches were unlocked: with four threads, the release build crashed with heap corruption in 3 runs of 3, and ASan found a use-after-free. One mutex is now shared by a Font and its copies, taken once per call. Measuring plain labels takes no lock, and single-threaded timings did not change. (2357195)
- `NativeWindow` (Windows) can be neither copied nor moved. A copy freed the window's accessibility root twice. (5744484)
- `FileBrowserComponent::selectionIsDirectory()` is true for an opened empty folder. (bd26999)
- `ValueBox`: text refused when focus leaves ends the edit and reverts, as Escape does; the field used to stay open. Its owner's `onTextEntered` may delete the box. (567ae52)
- `PreferencesPanel`: a page may switch pages from its own handler. The old page was freed under the handler, a use-after-free. It now leaves the panel at once and is freed at the next paint. (1c7ebca)
- `CommandInfo{id, name, category}` is a complete initialiser to gcc and clang's -Wextra. (bc33559)
- A user feature the Indic plan did not have went into a stage of its own after the presentation forms, where HarfBuzz merges it with them; and the shapers' overrides now follow the caller's features, as HarfBuzz's do -- clig=0 had turned Khmer's required ligatures off, calt=1 let Hangul's calt reach the jamo. (aceb17d)
- A hostile colour font is bounded: no more than 256 COLR layers a glyph, no picture larger than four times its strike, decoded pictures held to 64 MB rather than 256 of them -- which an emoji picker emptied on every paint; text under a pixel takes the smallest strike; and the system emoji face is proved to draw before it is chosen. (2313882)
- `TextEditor` refuses a code point that is no character -- a surrogate, or past U+10FFFF -- typed or pasted. (8ba751c)
- A colour font drew as black silhouettes, and one of pictures alone -- Apple Color Emoji, Noto Color Emoji -- did not load; an emoji the label's face lacked came from Segoe UI Symbol, in black outline. (078203b, 0113e72)
- Emoji sequences -- a thumb with a skin tone, a flag, a family joined by ZWJs, a tag flag -- were several places to a `TextEditor`'s caret: an arrow stopped inside one, Delete took half of it, and Backspace left a lone skin tone or a flag's second letter. They are one cluster each, formed as HarfBuzz forms them, and Backspace takes a cluster holding an emoji whole. A mark or joiner the label's own font has no longer leaves the emoji font drawing its neighbours, which kept a sequence from ligating. 13,500 emoji and ideographic variation strings now shape as HarfBuzz shapes them, where 11,003 did not. (1c1312d)
- `Font` left ZWJ, ZWNJ, the embedding controls and the soft hyphen out of shaping -- the bidirectional algorithm passes over them -- and split its run at each: "lam ZWJ" was drawn as a lone lam rather than its initial form, a Devanagari half form asked for with ZWJ was not made, and an emoji family came out as its separate people. They are shaped with the run they stand in. (adf85c2)
- A font whose GSUB listed one feature thousands of times, of thousands of lookups, hung shaping -- the plan searched each stage for each lookup it added. It is now made in linear time, as HarfBuzz makes it: a tag takes the first feature its language system lists under it, where both had applied, and a required feature applies whether or not its tag was asked for. (dc68ff5)
- `Font::wrapText` broke lines at ASCII spaces only. Japanese and Chinese, which have none, were cut wherever a character overflowed -- before a full stop, after an opening bracket; and a word too long for its line could be cut inside a cluster, leaving a vowel sign on a dotted circle at the start of the next line. It now breaks where UAX #14 allows, and a word too long for its line between clusters. (39a4a1c)
- Combining marks in a font that cannot place them -- one with no GPOS, or Hebrew in a font whose GPOS lacks the Hebrew script -- sat on the next letter or beside their own; they are now placed above or below their letter by their combining class, as HarfBuzz places them. (91c370b)
- `TextEditor` put its caret, read clicks and drew its selection by measuring prefixes: in Arabic and Hebrew the caret stood on the wrong side of the letters, a caret could land inside a ligature or a Devanagari conjunct, and a click in a long name shaped every prefix of it. The caret now follows the shaped line; the arrows step over a cluster as one place and Delete removes a whole cluster. (ea87652)
- Mongolian, N'Ko, Adlam, Phags-pa, Mandaic, Manichaean, Psalter Pahlavi, Sogdian, Chorasmian, Hanifi Rohingya and Old Uyghur went through the Arabic shaper's stages rather than the Universal one HarfBuzz uses for them, and Mongolian's free variation selectors did not take their letter's form; a context lookup applied per syllable could look past the syllable it began in. (d2c99bf)
- The Indic scripts, Khmer and Myanmar were drawn a glyph per character in stored order -- an i-matra after its consonant, no reph, no conjuncts -- and joiners (ZWJ, ZWNJ, CGJ) in Arabic, Hebrew and Latin text were passed over where HarfBuzz keeps them and kept where it passes over them, so one string in eight with a joiner shaped differently. (e893eb5)
- Thai and Lao SARA AM was drawn whole, with a tone mark before it sitting where its ring belongs, and Hangul jamo as loose pieces rather than syllables; both are now shaped as HarfBuzz shapes them. A Lao font's GSUB was not found under its tag, `lao `. (be16a4c)
- Arabic was drawn as a row of isolated, unjoined letters in stored order -- backwards -- with its vowel marks beside the letters rather than on them, and Hebrew backwards too. (0961847)
- Composite glyphs apply their components' scale and 2x2 transforms; a small capital built from a capital drew full size. A glyph whose hmtx bearing is not its outline's xMin is placed by the bearing, as other rasterisers place it: Times New Roman's box-drawing glyphs sat 190 units to the left. (4033d3b)
- In a plugin, `sonore.audioDevices` lacked `inputs`, `currentInput` and `testSignal`, which only the standalone pushes, so a page reading `sonore.audioDevices.inputs.length` threw on load and showed nothing. (7267df6)
- An SVG path stopped at its first `Z`: the guard against a `Z` that consumes nothing counted the `Z`'s own letter as nothing. Every hole, ring and second shape in one `d=""` was dropped -- 226 of 332 real SVGs drew wrong. (5631ef0)
- A Figma export drew the white rectangle of its frame's clip path over the icon, and every `url(#...)` fill came out black; an Illustrator export, which puts its colours in a `<style>` sheet, was black throughout. (5631ef0)
- At 200% an SVG's strokes were half as heavy as its fills: their width followed the viewBox's scale and not the editor's. (5631ef0)
- Kerning from a font's kern table read its pairs from four bytes too early, so a font that kerns only there (Tahoma, Microsoft YaHei, Malgun Gothic among them) was set with pairs it never had and without the ones it has. They now match fontTools' reading exactly. (7408df7)
- Hostile or unusual fonts: a CID-keyed CFF font with tens of thousands of Font DICTs no longer takes seconds to load; Adobe-style CJK fonts with the FontMatrix in each Font DICT load; a deprecated dotsection no longer leaves a glyph half drawn; GPOS reads stay inside the GPOS table and cost the same per pair however the lookups repeat; a Typeface loaded a second time forgets the first font. (7408df7)
- A Windows screen reader is no longer told about an editor the user has left for another part of the host: a control counts as focused only while the OS focus is on the editor or the host frame holding it. A focused control whose label changes is no longer re-announced as a new focus, and the first request a reader makes always finds the editor's controls. (8243945)
- The Windows accessibility provider walks the editor's components only on the window's own thread. UI Automation calls an element's properties on a worker thread, which walked the live tree while the editor's clock rewrote it and read a snapshot the window thread was rebuilding: a use-after-free with a screen reader polling. Reads now copy under a lock, and the clock refreshes the snapshot while a reader listens. (f732c3b)
- Text in a script the typeface lacks -- a preset named in Japanese, a sample folder in Korean, a file browser listing anything -- is drawn from the system's fallback faces instead of as .notdef boxes, Thai, Khmer, Myanmar, Tibetan, Mongolian, Yi and Cherokee included. Each face is asked only about the scripts it is for and read the first time one of their characters needs it: a character costs its own script's face (Korean 15 MB, Thai 3 MB), and one nothing covers about 3 MB, once. Scripts that need shaping (Devanagari, Khmer) draw their glyphs in order, readable but not typeset. Invisible code points (zero-width joiners, variation selectors, the soft hyphen, the byte-order mark) take no room and draw nothing; they drew a box. Measuring and drawing take one walk, so widths match the ink. `Font::withoutFallback()` keeps the boxes. (5ec80f8, 0fbbc1f)
- A stroke wider than its canvas draws what its geometry says. At 1e29 a slanted line far from the origin drew nothing; past about 1.8e19 every miter became a bevel; an infinite width drew nothing; and round joins that wide were flattened into thousands of points, so one stroke took over a second. The width is now cut to 4096 times the distance to the far side of the clip, and a round join or cap wider than that distance is drawn as the square around it. Both leave the drawn pixels the same. (087b878)
- Widgets take NaN and huge geometry. `Component::setBounds` turns a NaN into 0 and holds other values within 2^24. List, table and tree scroll positions reject NaN. `ColourGradient::addStop` counts a NaN position as 0; stored as NaN, it broke the ordering `std::sort` requires. (f4e92d0)
- A `TreeView` whose lazily listed root's callback rebuilds the tree lists the top level once, not twice. (f4e92d0)
- On a web editor page a read-only parameter is marked, and the bridge's `set`, `begin` and `end` leave it alone. The fallback page disables its slider. A slider dragged over a meter showed the dragged value until the plugin next changed it. (da4dddf)
- Drawing with a NaN, an infinity or a value past the range of an int no longer converts it to an int, which is undefined behaviour. Ordinary values draw exactly as before. The cases were:
  - `clipTo` and `drawImage` with a rectangle at 1e30;
  - an image drawn 1e-12 tall, which sampled its first row on x86 and now samples its last, like any pixel past its bottom;
  - a tiled fill at a NaN or extreme origin or scale, which painted the tile's first row everywhere; a NaN origin now paints nothing;
  - an image transform with an infinity in it;
  - an SVG arc with an infinite radius, now a straight line as a zero radius is;
  - `Colour::withMultipliedAlpha`, `interpolatedTo` and `brightened`/`darkened` given a NaN, which now counts as zero.

  (17d22ec)
- A `TreeView` whose root lists its children lazily (`mightHaveChildren` and `onNeedChildren`) shows them: the undrawn root was never opened, so it was never asked, and the tree stayed empty. (6588bbc)
- A read-only parameter (a meter the plugin reports) is shown and never set by an interface: the generic editor disables its row, and every wrapper drops a page's or editor's write or gesture on it (`uiMayWrite`). Dragging one moved it and told the host as an edit, on a parameter the host had been told it could not set. (c1cf368)
- A curve's flatness is never tested finer than a float can show at its coordinates: a fuzzed SVG with one control point at 3e11, stroked with round joins, flattened into billions of points and never finished drawing; a 3 px circle at y = 23,173,946 alone became 262,145, and now 5. Ordinary geometry flattens bit-identically. (28a42e1, 136bf90, 61210e5; the first cut every curve past 4e6 device pixels to its chord, which also straightened the on-screen arc of a shape whose far end was that far out) `Path::addArc` adds nothing for a non-finite angle and draws at most 64 turns, still ending at the angle it was given. (61210e5, 7c70cda)
- Native editor rendering:
  - a partial repaint no longer wipes the rest of the bitmap; (3e1aff4)
  - hiding a component erases it; (15f6d6c)
  - an idle editor with a switch no longer repaints 30 times a second; (909db35)
  - repaints are cheaper on Windows; (5001a22, f9bc5ba)
  - round caps and joins no longer cut holes in strokes; (a5b9ba8)
  - a stroke honours a tiled image fill; (c85f5de)
  - drawing after `Path::close()` keeps the first corner. (ba88d47)
- Windows:
  - AltGr characters reach text fields; (0c6d3ee)
  - combo menus close on an outside click; (1eecc22)
  - drop-downs and tooltips open in the right place at scale; (1141e60)
  - an embedded editor follows a DPI change; (b6ac878)
  - UIA elements no longer outlive their root. (7dbe7e3)
- macOS:
  - the native editor is no longer drawn upside down; (63a220f)
  - editors keep ticking during host modal loops; (42a15d5)
  - trackpad scrolling is accumulated; (f9de278)
  - Intel Macs get valid frames; (1889636)
  - resize and Retina changes are followed; (d4ac7be)
  - menus close on an outside click and open by their control. (0fc209a, fb21fd2)
- X11:
  - a tooltip no longer takes the clipboard with it; (34476e7)
  - menus and tooltips land correctly on a scaled desktop. (9a5da81)
- Widgets:
  - `MouseRouter` no longer keeps a freed control hovered; (9e3a45f)
  - tooltips no longer reappear after their hold ends; (03bd1b0)
  - a viewport resets scroll on a page switch; (6aff2c0)
  - TextEditor: stale scroll, paste filtering and UTF-8 decoding; (dbae49c)
  - splitter drags no longer jump; (d66838b)
  - FlexBox clamps items to their limits, and reversed rows mirror correctly; (147ed32, ffa34d9)
  - the bounds animator runs Back, Elastic and Bounce curves, and Elastic's phase is corrected; (28fc6dd)
  - a finished value animation calls its setter once; (fdd3b36)
  - re-entrant `Animator::cancelAll` and `AlertOverlay` callbacks are fixed; (454f2d0)
  - default shortcuts no longer steal keys; (096e566)
  - FileBrowser no longer chooses empty folders as files, and refresh keeps folders open; (5a5467c)
  - a ListBox whose model shrank still draws its rows; (7f03301)
  - a CallOutBox detaches old content; (21b09d7)
  - TableListBox column drags survive `setColumns`; (32e8c1f)
  - a TreeView double-click survives a rebuild; (0270e07)
  - the on-screen keyboard's hover clears, and a mouse release no longer darkens a host-held note; (16c2e2f)
  - an unresizable editor axis is pinned at its natural size, not 8192 px; (053494a)
  - RangeSlider keeps its minimum gap at the top. (648d1d6)
- MultiTimer carries over the remainder, so timers fire on schedule. (ae8d87e)
- `FileDialog::saveFile` never replaces an existing file without asking, including when the extension is appended. (cb79eaa)
- `FileDialog` filters written with commas ("*.wav,*.aif", the reference's spelling) or as bare extensions ("wav", ".wav") show their files on Windows and GTK. On Windows the dialog showed an empty folder. (f34ebee)
- SVG reads coordinates correctly under a comma locale. (15051c9)
- TrueType: a composite glyph is limited to 512 parts, which stops a font from hanging the UI. (443c60b)
- The scope's `clear()` is thread-safe. One infinite sample no longer latches the peak meter. (e322cf0, 0af6a13)
- Webview:
  - bridge strings are unescaped; (b7728de)
  - an unknown control asks for no context menu; (e7be914)
  - the generated fallback page's sliders follow the host; (c25a526)
  - WKWebView ignores non-string messages instead of crashing; (8857fff)
  - only valid UTF-8 StateBag entries reach the page. (b33b7d1)

#### Host (`host.h`, graph, plugin cache)

- `HostedPlugin::setEditorSize()` lets the caller resize a plugin's editor -- its window dragged larger -- through each format's own way: CLAP's `can_resize`/`adjust_size`/`set_size`, VST3's `canResize`/`checkSizeConstraint`/`onSize`, LV2's `ui:resize`. A host could only follow an editor's own resizing before. It is a virtual with a default that answers false, so a `HostedPlugin` written outside the SDK still compiles. (1f7d244, ac72723)
- An LV2 `reset()` or `release()` is not beside a state restore or the worker's job: deactivate and activate are LV2's instantiation class, beside nothing else on the instance. A reset is applied at the start of the next block that has the instance with no job under way, as CLAP's is, and a release waits for the worker. A test resetting every 50 blocks during 40 restores counted 37,523 overlaps; now none. (01c13dc)
- A VST3 plugin's controller state is saved with the session: `IEditController::getState` beside the component's, given back through `setState` after it, as the reference does. An editor's tab or zoom kept in the controller came back at its defaults. The blob is framed only when the controller has something to save, so plugins without controller state, every plugin built with this SDK among them, save exactly what they did, and every session saved before still loads. (17d47de)
- A VST3 bundle's binary is found for the host's own architecture (x86-win, arm64-win, arm64ec-win, aarch64-linux and the rest, and arm64x-win for an ARM64 host); it was x86_64 always, so a 32-bit or ARM64 host found nothing in any bundle. (4c90d5f)
- Round 31:
  - An LV2 plugin's latency, bypass and freewheel ports are known by their designation (`lv2:designation`, `lv2:reportsLatency`), not only by the SDK's own symbols. A DPF plugin reported no latency, so a graph did not compensate it, had no bypass, and showed the host's switches as user parameters. (621b232)
  - A mono LV2 plugin on a stereo caller is heard on both sides, as CLAP, VST3 and LADSPA plugins are; the right channel was silent. (fc9d733)
  - An LV2 state restore has the plugin to itself, as the spec requires: it ran beside `run()` and the worker's `work()` (2.9 million overlaps in the test). It is not starved by a busy worker, and a stopping worker drops its queue as documented instead of running every job. (0c943c7, 058ccd6)
  - The LV2 worker allocates its job buffer before it starts, so `process()` stays allocation-free; the allocation audit failed about one run in four. (0a0d61b)
  - A CLAP or VST3 editor that resizes itself is let, and the caller follows: `request_resize` and `resizeView` were always refused. (63d8593)
  - The graph, running a block in slices, hands on the events at its edges: an event before the block or at its end fell into no slice, note-offs included. (e7da089)
  - The plugin cache stamps an LV2 bundle on Windows by its files, so one updated in place is scanned again, and one process's scan mark survives another's writes to the same cache. (31e1969, a5fb470)
- An LV2 plugin that does its slow work on the host's worker loads: the host offers `worker:schedule`, which samplers and convolvers require. Each instance with a worker interface gets a thread of its own; jobs and answers cross in lock-free rings, and the answers go to `work_response()` in order after each `run()`, then `end_run()`. (1bbca98)
- An LV2 plugin that needs `options:options` instantiates. The host gave a plugin urid:map and nothing else, and a plugin built with the DPF framework will not instantiate without the options it reads its block size and rate from. Plugins now also get urid:unmap, options:options (sample rate, maximum and minimum block length, atom buffer size) and buf-size:boundedBlockLength; their interfaces get the options too. (bb449c9)
- A CLAP note port is sent the dialect it speaks. The host sent MIDI to every port, so a plugin whose note port speaks only CLAP's own dialect heard no notes and was handed MIDI it never asked for. Such a port now gets CLAP note-on and note-off events, and messages with no CLAP spelling are left out. Ports that take MIDI get MIDI as before. (b4ff3eb)
- A VST3 plugin is given a host context. Both `initialize()` calls now get an `IHostApplication`. Its `createInstance` makes `IMessage` and `IAttributeList` objects, and it answers `IPlugInterfaceSupport`. `setHostName()` names the host, for CLAP too. Component and controller talk through proxies, as in Steinberg's own hosting code, and a message is delivered only on the thread that loaded the plugin, never from inside `process()`. Before this, the host passed `initialize(nullptr)`, which plugins built on Steinberg's SDK need in order to talk between their halves. A controller that refuses `initialize()` is no longer used anyway. (04c94d4)
- An LV2 interface and its plugin exchange `atom:eventTransfer` events through the host, both ways, over bounded lock-free queues. Messages dropped when a queue is full are counted in `Lv2Plugin::droppedUiEvents()`. The interface gets `urid:map` and `urid:unmap`. (4a159c8)
- An LV2 interface whose `idle()` returns non-zero has closed itself: it is cleaned up at once and can be opened again. The host used to go on calling it. (4dc5266)
- A plugin cache with two STAMP lines for one file (written twice over, or edited) is judged by the last one in every lookup. `isUpToDate` read the first and `knownFor` the last, so with a stale stamp after the real one the file was "up to date" and known to hold nothing, and its plugins were hidden for good. (ea52dc6)
- Round 26, a host that survives other people's plugins. Each fix was proven by a probe that failed first, and the probe was kept.
- LV2 atom outputs:
  - Every LV2 atom output was connected to one buffer, so the UI-notify port erased the MIDI output. The SDK's LV2 arpeggiator gave 0 notes in 2000 blocks where its CLAP and VST3 builds gave 43; now it gives 43. (4bfde54)
  - An LV2 output sequence is read only as far as the plugin wrote it. An unwritten port replayed the last block's events, and an oversized `atom.size` was read off the end of the buffer. (2dee908)
- LV2 bundles and state:
  - An LV2 bundle whose `lv2:index` values are not 0..N-1 is refused. Index -1 corrupted the host's heap, and 2e9 asked for 7.6 GB. (ea84f9a)
  - Blank nodes from different files of one bundle (MOD's `modgui.ttl`) are no longer merged into one node, which had left ports unconnected. (7a9c0c9)
  - LV2 `loadState` reads the blob whole and checks it before applying any of it: NaN and out-of-range values went straight into the ports, and a broken blob was half-applied. (8e9bb87)
  - An LV2 latency of 1e10 hit an undefined float conversion. It is now read as samples, from the copy the audio thread leaves. (d4665c8)
- Events and threads:
  - VST3 and LV2 plugins get every event inside the block it plays in, as CLAP already did. (f417ffb)
  - A VST3 plugin may call its component handler from `process()`. It raced a plain vector and allocated on the audio thread: ASan reported a container overflow, and 29,264 gestures were counted for 20,000 blocks. It is now a lock-free queue. (bca8e15)
  - A CLAP plugin flushed before its first `prepare()` hears the changes queued for it. (c5bccf1)
  - A CLAP `RESCAN_ALL` no longer leaves queued edits pointing past a shorter parameter list. (5053b40)
- What a plugin reports:
  - Unterminated fixed-size strings from a plugin (CLAP parameter names, modules and value text; VST3 vendor and class names) are read within their arrays. (df81f0b)
  - Counts a plugin reports are bounded before the host reserves memory or loops on them. A CLAP parameter count of 4,294,967,295 made `loadPlugin` throw, and a VST3 class count of 2^31-1 took 9.9 s to scan. (455946b)
- Graph: a node's latency is held to `PluginGraph::kMaxLatency` (2^22 samples). 0xFFFFFFFF committed 32 GB, and two latencies of 2^31 summed to 0. (63e6b58)
- LV2 editors are sent output controls, so meters move, the SDK's own LV2 editors' included. (8efed01)
- Turtle: `\u` and `\U` escapes are read as code points and written as UTF-8. `\u` was cast to one byte, `\U` was unknown, and bad hex became NUL. `fuzz_parsers` gains hostile-TTL seeds, and `fuzz_hosted` loads damaged state blobs. (2502a1b, 64a98ca)
- Scanning a file that is not a loadable library -- truncated, built for another architecture, a dependency missing -- no longer stops on Windows' "Bad Image" box. LoadLibrary raised it, modal and often on a desktop nobody sees, and the scan waited for a click: half an hour in a test run. `openLib` turns hard-error boxes off for its own call and fails instead.
- System DLLs the headers load by name -- bcrypt (crypto.h's random numbers), version, winmm, avrt, imm32 -- come from System32 only. By bare name the search starts in the host's folder, where a planted copy would load into the plugin. scripts/header-rules.mjs refuses the bare form.
- `producedMidi()` is empty after a block that was not processed -- a CLAP plugin refusing to start, a block too long or empty -- instead of holding the last processed block's MIDI, which a graph handed downstream again. (c240dc8, f048f82)
- `HostedPlugin` documents the thread and block-size contract of `prepare`, `release`, `reset` and `process`, including that a block longer than prepared is not processed. (77d7e31)
- An LV2 plugin's own state survives `loadState()` before `prepare()` and every later `prepare()`; it was restored into nothing, and each re-instantiation dropped it. (7028836)
- CLAP `request_callback` is answered: `on_main_thread()` runs from `idleEditor()`, `drainParameterEdits()` and `takeStateDirty()`. `latency.changed()` inside `activate()` no longer asks for a restart, so a plugin that announces its latency as it activates is not restarted by every prepare. (283624d)
- CLAP and LV2 event buffers hold a full MidiBuffer; past 341 events a block lost the rest, note-offs included. (ec1df19)
- A CLAP module's entry is initialised once for all its instances and scans, and deinitialised after the last; each instance and scan called init() and deinit() for itself. (96c3aa0)
- LV2 `release()` deactivates only an active instance. (6b54f29)
- VST3 hosting:
  - plugins are destroyed on unload; (dc7cf7b)
  - module exit is paired with a successful entry, and ModuleEntry gets its handle; (8409fa1)
  - non-ASCII names arrive as UTF-8; (09d0b32)
  - the state stream writes at the cursor; (880d29c)
  - the process context carries the real rate and beat position; (c09e9bc)
  - every bus is negotiated, with mono as `kMono`; (e59ed4b)
  - a split controller's edits reach the processor, and the two halves are connected; (3bedfb2)
  - a full parameter queue keeps the newest value; (a1e4554)
  - output MIDI beyond notes is passed on; (511fd47)
  - a processor's output parameter changes reach the controller, so its editor shows a meter or a value its DSP moved, and reach the caller as edits -- the newest value of each, however many blocks pass between drains, found in constant time on the audio thread; (2f0a9dd, 573c8e2)
  - `IComponentHandler2::setDirty` is answered, and `takeStateDirty()` reports it. (f42d7b7)
- CLAP hosting:
  - one buffer per declared port; (f60318b)
  - a `closed(true)` editor is destroyed; (1d6bc34)
  - no allocation after a preset load; (7993212)
  - `flush()` never runs beside `process()`; (02e7400)
  - `start_processing()` and `reset()` run on the audio thread, at the start of the next block, as CLAP requires; a plugin that refuses to start is not processed -- its block is left as it came in -- and is asked again next block, instead of being processed anyway; (ed4be65, 689fd52)
  - event times from a caller's MidiBuffer are kept inside the block; `CLAP_PARAM_RESCAN_ALL` while inactive reads the parameter list again; (ed4be65)
  - the module entry's registry survives static destruction. (a49e894)
- Parameter changes set while audio runs are no longer lost, through a lock-free queue. Parameter id lookup is constant-time. (02e7400, e75497a)
- LV2 hosting:
  - aux ports get their own buffers; (9ddd069)
  - the bundle path ends in a separator, and CV ports are connected; (1e7d365)
  - the URID map is thread-safe; (f5c8bef)
  - deeply nested Turtle no longer overflows the stack; (2a0caec)
  - the state carried into a new instance is kept when instantiate() fails. (f07a7e4)
- Graph: realigns when a node's latency moves, and SysEx follows MIDI edges. (70dbef8)
- Plugin cache:
  - CLAP-only builds compile;
  - non-ASCII paths work;
  - a live scan in another process is not blacklisted;
  - writes are atomic;
  - a BOM is tolerated;
  - bundles on macOS and Linux are stamped by their contents. (a8acf8c, 00972f3)

#### Examples

- Round 29, the synth and the sampler:
  - A host's reset silences their voices. Neither has its own `reset()`, so the wrapper calls `prepare()`, which left every voice sounding: two seconds after a CLAP reset the synth still put out 0.13 and the sampler 0.50. (a0a4d8e)
  - The first block after `prepare()` plays at the Level control's level. `prepare()` also runs on a reset and on the switch to offline rendering, and set the level to a fixed 0.3 or 1.0 before gliding: a bounce starting on a note at -40 dB began 25 to 55 times too loud. (c5e5eb4)
  - Moving a control keeps each sounding note's expression: the synth's Cutoff dropped a note's pressure opening, and the sampler's Tune dropped a note's bend. Automation did this every block. (0585c0a)
  - Synth: a note after a pressed one starts with its filter closed; the previous note's pressure stayed in the filter. (3b7a095)
  - Sampler: a voice freed by a one-shot's end starts its next note silent. It was freed with its envelope up, and the next note on it skipped its attack (0.50 in its first 5 ms against 0.016). (e6fd419)
- Sampler: a file picked in the editor plays at middle C, as a restored one does, not at the previous bank's root. (1e58fa2)
- Sampler:
  - the envelope ran at double speed on mono samples; (883d4d5)
  - the MPE tuning sent with a note-on is applied; (2cd7f14)
  - it now declares the wrapper's `loadFile(purpose, path)` hook, so a file picked through the wrapper loads and marks the session changed. Its loader was not the hook's signature, so only a saved session could load a file. The loader is now `loadSample()`. (e3bc04b)
- Arpeggiator: the first note plays immediately. (3f1f159)
- Splitter: no zipper, and the stereo image no longer splits on output moves. (3db16ee)
- Reverb:
  - impulses are handed to the audio thread safely; (b9a248a)
  - pre-delay and send glide; (63fdac1)
  - the tail matches what plays; (92544bd)
  - has its own `reset()`. (a4b0630)
- Saturator: the page's knobs follow the host. (63fdac1)
- dB-to-gain is converted once per block. (dd50441)

#### Build and CMake

- Round 31, what a new project meets first:
  - A plugin project in a folder with a non-ASCII name builds; the LV2 metadata step got its path through an ANSI argv and failed. (ae423bb)
  - A 32-bit project finds an SDK installed from a 64-bit configure: the version file is architecture-independent. (fb684fb)
  - A generated project builds Release with every generator. With Ninja or Makefiles `--config Release` is ignored, and the plugin was built Debug against the debug runtime, which does not load without Visual Studio. (f1a25ee)
  - Two generated projects build under one CMakeLists; the second added the SDK again. (93e295a)
  - The README's "Trying it" points at the VST3 folder that loads, not a renamed bundle a host cannot read. (8a99358)
- The tests that measure the machine -- callbacks against the display, handles across editor opens, reads against a writer, real devices and WebView2 pages -- wait for what they check instead of a fixed time, and run alone (RUN_SERIAL) under `ctest -j`. Beside other tests or a full build they failed while passing alone. (d671a15)
- A project at /W4 /WX that includes both `crypto.h` and `audio.h` compiles: the Blowfish table, a local `kPi`, hid `sonore::kPi` (C4459). `windows_minmax_test`, which holds many headers in one translation unit, now builds at /W4 /WX too, so headers that clash only together fail the build. (57283f7)
- Headers that gcc and clang refuse, found with clang's front end before CI: `GifDecoder`, `ImageFileFormat`, `JpegDecoder`, `JpegEncoder` and `LAMEEncoderAudioFormat` took a nested `Options` with member initializers as a default argument, which gcc and clang reject while MSVC accepts. Every Linux and macOS TU including them would have failed. They are overloads now, and callers are unchanged. `StrokeStyle` and the CFF charstring aggregate no longer omit an initializer, and `MouseInactivityDetector` lost an unused member; both were errors in the clang-cl `/W4 /WX` targets. clang now reports 0 errors over all 216 translation units. (4aff7d7)
- MSVC builds get `/bigobj` from `sonore_sdk` (with `/utf-8`, decided by the consuming project's compiler when the SDK is installed, not by the one that built the install), so every target that links the SDK can build in Debug: the test suite did not (C1128, too many sections), and the suite now passes in Debug too, its two speed comparisons reported rather than judged there. The standalone builds its per-thread fallback buffers only when used, which a Debug build counted as allocations on every new audio thread. (ec83778, b8b1746, c23213a)
- `install_consume` finds a macOS `.clap` bundle and builds the consumer with the same toolset. (748328c)
- A new test, `install_consume`, installs the SDK into a fresh prefix and, in a project that knows only `find_package(SonoreSDK)`, builds a program, a CLAP, a VST3, an LV2 and a standalone (which runs `--verify`), and a host that must describe all three plugin formats. (6ecaf57, 48ccd87, 06f1773)
- `sonore_enable_hosting(<target>)` puts the VST3 and LV2 headers on a host program's path; without it, `host.h` hosted CLAP only and found no VST3 or LV2 plugin. (06f1773)
- The macOS build of a plugin (every format, the Cocoa editor, `au_view.h`) and of the standalone are compiled on every MSVC build, with `__APPLE__` forced, against the Apple shims -- the only compiler that reads them while macOS CI is not running. The shims declare `objc_msgSend` and `IMP` as Apple's SDK does today. (f49a482, 7c3a4b0, a6555db)
- The C4996 suppressions cover the calls they meant to (a `/sdl` build without `_CRT_SECURE_NO_WARNINGS` now compiles), and two objects build the synth that way. (e11425f)
- `formatShortDuration()`'s micro sign is the same in any source encoding. (890cb37)
- Two source files that include `audiofile.h` (or a standalone plus another file) link: the vendored codecs had C linkage and collided 18 times on MSVC. Their macros no longer leak. (f35f1c6)
- `<windows.h>` without `NOMINMAX` before the SDK no longer breaks it; `audio_wasapi.h` no longer leaves `min`/`max` defined. (eddd9a1)
- No C4996 from the SDK's headers, so a project with `/sdl` compiles. (0a54d86)
- The native editor works with RTTI off. (6229d32)
- `find_package`: honours `-DSONORE_GUI=OFF`, links WebView2 in every directory that asks, works with a multiarch library directory, and the install carries the licences. (483f143)
- No warnings from the SDK's headers under clang `-Wall -Wextra`; a `kDesc` written like the README's no longer warns about `editorLimits`. (0a2b7c5)
- The VST3 helper also assembles `vst3/<target>.vst3`, a bundle a host scans as it is, with its binary named `.so` on Linux; the old `<target>.vst3.bundle` is still built. (22c4129)
- An installed SDK compiles `audiofile.h` and the standalone: minimp3 and stb_vorbis are now installed. (be9085c)
- A project at `/W4 /WX` builds. Vendored codec warnings are silenced, and MSVC gates the warnings. (11182b2, cfe9fcd, 353ae14)
- Including the SDK after `<windows.h>` compiles. (a8acf8c)
- `flac_encode.h` compiles when it is included first. (85353ee)
- `sonore_enable_asio()` no longer fails configure; ASIO is built in. `sonore_add_rt_test` finds its source. (de0b41c)
- The shared WebView2 cache is versioned, locked and checked for completeness. (5815192)
- `sonore_add_au` warns when an AU has no identity of its own. (e1e2ccf)

#### Docs

- Overclaims corrected: the debug descriptor check is the CLAP's, LV2 has no native-editor fallback, `clap_entry` is exported by every module, meters fire on change, and hidden VST3 output parameters have stated limits. (dd58b5a)
- An audit of the README against the code: the RT test, the install route, hosting, the WebView2 fallback, which modules export which entry points, `VERSION`, the standalone's inputs, the debug-build descriptor check, sizes and paths. (915e2f3)
- The feature map and README no longer call plugin hosting out of scope: `host.h` hosts CLAP, VST3 and LV2. Hosting an Audio Unit is listed as a real gap. (2d7270d)
- The README gives the browser (wasm) build: the source half it takes, the em++ flags and export list, and how a worklet drives the module. A failed WebView2 download names the two ways to configure offline. (6dc2a21)
- The README template builds every format. The guide covers:
  - fixed names, append-only parameters;
  - hook signatures, threads;
  - interface, building, validation claims. (bee5405)
- README: the plugin size figure is updated. The parameter snapshot is described as it works. (fcc188a)
- Corrected comments:
  - FrequencyShifter `setMix` (it does not exist); (38eb950)
  - the bridge callback's argument order; (c25a526)
  - Convolver threading; (6038345)
  - SampleVoice sinc limits; (eb82169)
  - JilesAtherton remanence; (54abaf7)
  - the gfx backend comments. (c9aa792)

<!--
Not listed (tests, CI or internal tooling only):
6671385 CI gates and wasm job; a33ea14 CI validators; 1b250d3 unclaimed gate;
204623e Apple syntax check covers CoreMIDI output; 9716746 verify:rt rule for AU;
9f9b473 sanitizer allocator policy; 7be2bad, 48cf227, de487c4, 0da1548, 31b16e3,
567b45d, 9ba16b6, 2415e48 test timing/coverage only.
-->
