## What this changes

<!-- One or two sentences. If it fixes an issue, link it. -->

## How it was verified

<!-- Which measurement proves it. For a DSP change, the number a test now
     asserts and what predicts that number (a paper, an analytic result, a
     published table). "It sounds right" is not a measurement. -->

- [ ] `npm run verify:sdk` is green, or I have said below which leg could not
      run here (no WSL / no emscripten / no validators; those skip loudly)
- [ ] New public types have a row in `scripts/feature-map.mjs`
      (`npm run verify:unclaimed`)
- [ ] New claims in a header are asserted in `sdk/tests/sdk_tests.cpp`
- [ ] Commits are signed off (`git commit -s`); see `CONTRIBUTING.md`

## Anything a reviewer should know

<!-- Trade-offs, things you tried that did not work, measurements that
     corrected an assumption. This project keeps those in the code; they are
     the most useful thing in it. -->
