# Host tests

Compiles the sketches on your machine against a stubbed ESP32 and runs them
against a simulated amplifier. No board, no Arduino toolchain, about a second.

```sh
./test/run.sh
```

These catch a different class of problem from `amp_sim/`: logic and framing
regressions rather than wiring. Both are worth having — this suite tells you a
change was safe before you flash it, and `amp_sim` tells you the wiring is right
after you do.

## What runs

| Suite | Covers |
|---|---|
| `bench_test.cpp` | `scp_bench`: framing, the write-enable retry, per-target ceilings, strict number parsing, the BLE queue and chunking, advertising recovery, diagnostics, the verbosity toggle, and behaviour with no amp attached |
| `amp_sim_test.cpp` | `amp_sim` against **every captured frame in the spec**, byte for byte |
| `app_test.mjs` | The web app's chunk reassembly, both regexes, connect retry, level-read retry and slider throttling (needs `node`) |

Both sketches are also cross-compiled for `CONFIG_IDF_TARGET_ESP32H2`, so a change
that breaks the two-UART parts fails here rather than on the bench.

## Why it is trustworthy

**The tests `#include` the real `.ino` files.** Nothing is copied or paraphrased, so
the suite cannot drift out of step with the firmware — if a sketch stops compiling
or changes behaviour, this fails.

**`amp_sim_test.cpp` asserts against real captures**, not against our idea of the
protocol. Every request and expected reply is a frame from
`conductor-scp-protocol.md`, traceable to the logic-analyser sessions. When a test
and the simulator disagree, the capture decides — and on one occasion it was the
test that was wrong.

**`app_test.mjs` checks its own copies against `app/index.html`** and fails if the
regexes there have changed, so the JavaScript copies cannot silently rot.

## What it does not cover

- **The real BLE stack.** `shim/BLEDevice.h` is a stub, so this proves the sketch's
  logic, never that Bluedroid behaves. Only hardware shows that.
- **Real timing.** `millis()` is a counter that advances on every call and `delay()`
  just adds to it, so nothing here proves the 230400 baud UART keeps up.
- **Wiring and electrical behaviour.** That is what `amp_sim` and a DMM are for.

## Layout

```
shim/Arduino.h      stubbed Arduino API, plus the simulated amp behind the UART
shim/BLEDevice.h    stubbed Bluedroid BLE; records notifications
shim/support.h      assertions and hex helpers shared by both programs
```

The shim models only what the sketches actually use. If a sketch starts calling
something new, the compile fails and the stub gets extended — which is the
intended way for it to grow.
