# Diagnosis test plan: why the ESP32 gets no answer from the amp

**Status:** proposed, not yet run (29 Oct 2026).

## Why this plan exists

The ESP32 (H2, firmware v8.2) gets **no reply at all** from the real amp. With
the `amp_sim` board in place of the amp, the same H2, firmware and phone app
work perfectly. So the H2, the firmware and the app are not the problem on
their own. What sits between the amp and the H2 is.

Several things have gone wrong at different times: an ARX wire that came
loose, firmware v8's pull-down on RX, and possibly the power-up order. Because
these overlapped, earlier observations ("it worked once", "it works if the
knob booted the amp", "re-running the handshake woke the amp") cannot be
relied on, and the conclusions drawn from them may be wrong.

So this plan **assumes nothing from before**. It establishes facts one layer
at a time, from the copper upwards. It makes one change per test and records
every result in the table at the end. The logic analyser decides the central
question, because it is the only thing that can tell the two possible stories
apart:

- **the amp does not answer** (nothing on ATX), or
- **the amp answers, but the ESP32 does not hear it** (replies on ATX, silence
  in the ESP32's log).

## Before you start

### The wiring has no series resistors

The README specifies 1 kΩ in each data line. The current harness has none:
the ESP32's pins go straight to the amp's. That matters in two ways.

**It is a safety issue for some tests.** With no resistor, two outputs on one
wire short each other: the ESP32's TX on the amp's TX (wires swapped), or the
ESP32 and the knob both on ARX. Each test below says what may and may not be
connected. **Never connect the knob and the ESP32 to ARX at the same time.**

**It is also a suspect.** When the amp is off but the ESP32 is powered, the
ESP32's TX pin holds ARX at 3.3 V. Without a resistor, nothing limits the
current that flows from it into the unpowered amp's input pin, and from there
into the amp's 3.3 V supply. That can half-power the amp's processor and upset
how it starts. Test 3 measures exactly this.

Adding the 1 kΩ resistors is recommended in any case. But run tests 1–3 on
the harness **as it is now** first, because the aim is to diagnose the current
setup.

### What you need

- A multimeter, with continuity (beep) mode.
- The logic analyser (fx2lafw) and PulseView, as used for the original
  captures: 2 MHz is enough. Add the UART decoder on each channel: 230400 baud,
  8N1.
- The MacBook with the Arduino IDE Serial Monitor at 115200, plugged into the
  H2's USB port. The console shows every frame with `v 1` (the default after
  boot). Run the MacBook **on battery**, not on its charger, to avoid a ground
  loop through the USB cable.
- The phone app, to download the ESP32's log after each test.

### Terms used below

- **Cold boot:** amp power (ignition / REM) off for at least 10 s, then on.
- **Tap:** a probe clipped to a Micro-Fit pin on the breakout, alongside
  whatever is plugged in.
- **Console:** the H2's USB Serial Monitor. Commands are typed there.

### For every test

1. Before the test, type `log clear` so the log only covers this test.
2. Write down the power order: what was connected and powered, and when.
3. After the test, download the log in the app. Save it with the test number in
   the file name.
4. Fill in the results table.
5. Repeat each test that involves the amp **three times**. A problem that
   comes and goes needs more than one sample.

## Stage 1: the copper (multimeter only, no ESP32)

### Test 1: pin voltages, amp alone

**Setup:** amp on (cold boot). **Nothing** plugged into the Micro-Fit: no knob,
no ESP32.

**Measure** each pin against GND: 3V3, ARX, ATX.

| Result | Meaning |
|---|---|
| 3V3 ≈ 3.3 V | You are on the right pins, and the amp's remote supply is on |
| Any pin ≈ 12 V | **Stop.** The pinout or orientation is wrong |
| ATX ≈ 3.3 V | The amp drives its TX line high itself (push-pull). No pull-up is needed |
| ATX ≈ 0 V | The amp only pulls ATX low and relies on a pull-up elsewhere (open-drain). The ESP32's weak internal pull-up may not be enough |
| ATX between 0.5 and 2.8 V | A weak or floating line. Note the value |

Then **repeat with the knob plugged in**, measuring at the breakout taps. If ATX
is 0 V alone but 3.3 V with the knob, the knob provides the pull-up. That would
explain the old "~0 V on the multimeter" puzzle (spec §8).

### Test 2: the harness

**Setup:** everything unplugged and unpowered.

1. **Continuity, end to end** (beep mode): from each Micro-Fit pin to the ESP32
   pin it is meant to reach.
   - ATX to GPIO4.
   - ARX to GPIO5.
   - GND to GND.

   Then **wiggle** each wire and connector while it beeps, and listen for any
   break.
2. **Shorts:** ATX to ARX, ATX to GND, ARX to GND, each to 3V3. All must stay
   silent.
3. **Which physical pad is GPIO4?** Check it against the H2 board's pinout
   diagram, not its silkscreen order. Write down which pad each wire is on.

Any break or short found here gets fixed before going on. Then Stage 1 is
re-run.

## Stage 2: does the ESP32 feed power into the amp?

### Test 3: back-powering with the amp off

**Setup:** amp **off**. ESP32 powered and connected as it is in the car (TX to
ARX, RX to ATX, GND). No knob.

**Measure** the amp's 3V3 pin against GND.

| Result | Meaning |
|---|---|
| ≈ 0 V | No back-powering. Rule this cause out |
| Anything above ≈ 0.3 V, typically 1–3 V | **The ESP32 is powering the amp's remote side through ARX.** This is the strongest candidate for "the amp starts up wrong when the ESP32 is attached" |

To confirm, unplug **only** the ESP32's TX wire and measure again. If the
voltage drops to 0 V, ARX is the path.

If back-powering shows up, note it and carry on with stage 3, so it is
recorded how much it matters. The fixes are listed under "What happens
after".

## Stage 3: who is silent? (logic analyser)

For every test in this stage, connect the analyser to the breakout taps:

- **D0 → ATX**, what the amp sends;
- **D2 → ARX**, what the amp receives;
- **GND → GND**.

These are the same channels as the original captures. The analyser only
listens, so it is safe to add to any setup.

### Test 4: reference, the knob alone

**Setup:** knob plugged in. **No ESP32.** Analyser on the taps.

1. Start capturing (about 20 s). Cold boot the amp. Turn the knob a few clicks
   after it lights up.
2. **Expect:** the handshake from spec §5 about 1 s after power-up, with a reply
   on D0 to every request on D2. Then a volume write and its echo for each click.

| Result | Meaning |
|---|---|
| Handshake and replies as in the spec | The amp and the harness work today. The fault is specific to the ESP32 setup |
| Requests on D2 but **no replies on D0** | **The amp is not answering even the knob.** The fault is the amp, its setting, or ATX. Stop and investigate that before testing the ESP32 |
| Nothing on D2 either | The analyser taps or the knob's connection are wrong |

### Test 5: the ESP32, under different power orders

**Setup:** ESP32 connected (no knob). Analyser on the taps. Console open on the
MacBook. In every variant, once the amp has been on for at least 5 s, type
these in the console:

```
r
r
h
r
g 0
```

Capture D0 and D2 throughout.

| Variant | Power order |
|---|---|
| **5a** | ESP32 powered and connected first, then cold boot the amp |
| **5b** | Cold boot the amp with **nothing** connected. After 10 s, connect the already-powered ESP32 |
| **5c** | Cold boot the amp with the **ESP32 connected but unpowered**. After 10 s, power the ESP32 (e.g. plug in its USB) |
| **5d** | Cold boot the amp with the **knob**. After 30 s, with the amp still on, unplug the knob and plug in the ESP32 |

For each variant, read the three sources side by side:

| Analyser D2 (ARX) | Analyser D0 (ATX) | ESP32 console / log | Diagnosis |
|---|---|---|---|
| Correct frames (e.g. `42 03 FC 01 2A 04 2E`) | Replies (`43 …`) | Replies shown, levels read | **Working** |
| Correct frames | Replies | "no valid reply (silence)" | **The ESP32 does not hear the amp.** The fault is between the ATX tap and GPIO4: the wire, the pad, or an ATX too weak for the ESP32's input (compare test 1) |
| Correct frames | **Nothing** | "silence" | **The amp does not answer the ESP32.** Compare the variants: if it fails only when the ESP32 was powered during the amp's start-up (5a), the start-up state is the cause (see test 3) |
| Missing or garbled frames | — | — | **The ESP32's TX does not reach the amp properly.** Check the ARX wire, and that GPIO5 is the pad you think it is |

The pattern across 5a–5d is the diagnosis:

- **5a fails, 5b, 5c and 5d work:** the ESP32 being powered while the amp
  starts is the problem. Test 3 will usually show back-powering.
- **All four fail, but test 4 works:** something about the ESP32 itself
  differs from the knob. Look at the D0 trace for replies the ESP32 misses,
  then at test 1's ATX voltage.
- **Results vary between repeats of the same variant:** an intermittent
  contact. Go back to test 2 and wiggle.

### Test 6: intermittent contact

**Setup:** whichever variant from test 5 works. The phone app connected,
reading the levels every few seconds (it retries by itself).

Wiggle the harness and connector, section by section. Note any moment the
levels disappear, and the matching gaps on the analyser and lines in the log.
A connection that drops while being wiggled is a hardware fault, whatever
else is true.

## Results table

Copy this into a note, or add rows here, as you go. "Repeat" is 1–3.

| Test | Repeat | Date / time | Firmware | Connected | Power order | Measured / seen | Log file | Conclusion |
|---|---|---|---|---|---|---|---|---|
| 1 | | | — | amp only | | 3V3 = , ARX = , ATX = | — | |
| 1 + knob | | | — | amp + knob | | ARX = , ATX = | — | |
| 2 | | | — | unpowered | | continuity: , shorts: , GPIO4 pad: | — | |
| 3 | | | v8.2 | ESP32, amp off | | 3V3 = ; without TX wire = | | |
| 4 | | | — | knob + analyser | cold boot | handshake: , replies on D0: | — | |
| 5a | | | v8.2 | ESP32 + analyser | ESP32 first | D2: , D0: , console: | | |
| 5b | | | v8.2 | ESP32 + analyser | amp first, ESP32 after | | | |
| 5c | | | v8.2 | ESP32 + analyser | ESP32 unpowered at boot | | | |
| 5d | | | v8.2 | ESP32 + analyser | knob boots amp, swap | | | |
| 6 | | | v8.2 | | | | | |

## What happens after

Once the table points at a cause, fix that one thing, and repeat test 5 to
confirm it. Only then change anything else. Expected fixes, depending on
the result:

- **A broken wire, a bad contact or the wrong pad:** repair, then test 5 again.
- **ATX needs a pull-up:** 10 kΩ from the Micro-Fit 3V3 pin to ATX. Using the
  amp's own 3.3 V keeps it off when the amp is off, as the knob's is.
- **Back-powering, or the start-up state:** series resistors at minimum.
  Then optionally the firmware change already discussed: sense the amp's 3V3 on
  a spare GPIO, keep TX switched off while the amp is off, and send the handshake
  1 s after it comes on, as the knob does.
- **The amp does not answer the knob either (test 4):** the ESP32 is not the
  problem. Check the amp's PC-Tool settings and its remote input.
