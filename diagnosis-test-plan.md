# Diagnosis test plan: why the ESP32 gets no answer from the amp

**Status:** proposed, not yet run (30 Oct 2026).

## Why this plan exists

The ESP32 (H2, firmware v8.2) gets **no reply at all** from the real amp. With
the `amp_sim` board in place of the amp, the same H2, firmware and phone app
work perfectly. So the H2, the firmware and the app are not the problem on
their own. What sits between the amp and the H2 is.

Several things have gone wrong at different times: an ARX wire that came
loose, firmware v8's pull-down on RX, and possibly more. Because these
overlapped, earlier observations ("it worked once", "it works if the knob
booted the amp", "re-running the handshake woke the amp") cannot be relied on,
and the conclusions drawn from them may be wrong.

So this plan **assumes nothing from before**. It establishes facts one layer
at a time, from the copper upwards. It makes one change per test and records
every result in the table at the end. The logic analyser decides the central
question, because it is the only thing that can tell the two possible stories
apart:

- **the amp does not answer** (nothing on ATX), or
- **the amp answers, but the ESP32 does not hear it** (replies on ATX, silence
  in the ESP32's log).

## How the H2 is installed

The car install, as described on 30 Oct 2026:

- **The H2 is powered by the amp**, from the 3V3 pin of the 4-pin SCP connector,
  just as the knob is. It is never powered while the amp's remote supply is off.
  It starts when that supply comes on, and stops when it goes off.
- **No series resistors**: the H2's pins go straight to ARX and ATX.

That has consequences for the diagnosis:

- **The H2 cannot back-power an amp that is off,** since it is off too. That
  earlier theory does not apply to this install.
- **Every H2 boot is a power-up of the amp's remote supply.** The log's boot
  lines (`#5 … boot: … reset by power-on`) therefore record when the amp
  switched its remote port on. Boots you cannot match to switching the amp on
  mean the supply dropped out.
- **The supply is a suspect.** The 3V3 pin was designed to run a knob. The H2,
  with Bluetooth running, may draw more than the amp's supply can give. A sag
  could disturb the amp's own remote interface, as well as the H2.

### Never connect USB while the H2 is on the amp's 3V3

The H2 board's USB socket feeds its own 3.3 V regulator. With the amp's 3V3 pin
wired to the board as well, two supplies are tied together. Then:

- the two fight while the amp is on;
- the USB supply powers the amp's remote side while the amp is off.

That would recreate the back-powering this install otherwise avoids, and it
would make any result meaningless. **So, for every test with the H2 wired to the
amp, the H2 runs on the amp's power only: no USB.**

Commands and logs go through the phone app instead:

- the app reads the levels by itself (`r`) as soon as it connects, and every
  3 s while there is no answer;
- **Re-run start-up sequence** sends the handshake (`h`);
- **Download log** fetches the record.

That covers everything the tests need. The one place USB is used, test 4b, says
how to do it safely.

Also ask: was the H2 flashed over USB while still wired to the amp? Boots #1 and
#11 in the log say "reset by other", which is what a USB upload looks like.

### Without series resistors

A swapped wire, or the knob and the H2 on ARX at the same time, is a direct
short between two outputs. **Never plug in the knob and the H2 together,** and
do not swap wires as a test. Adding 1 kΩ in each data line is recommended
afterwards, but the tests below are for the install as it is.

## What the original measurements already settle

`conductor-scp-protocol.md` records what was measured in September, with the
WROOM-32 and the logic analyser. Those measurements are more reliable than
anything remembered since, so every theory is checked against them first.

### Measured facts

| # | Fact | Source |
|---|---|---|
| M1 | The amp answers reads **without any handshake**: the very first read in the replay session, knob unplugged, returned the levels | spec §4b, §9 replay session |
| M2 | **After an amp power-cycle with no knob attached**, and the ESP32 left running on its own power, the amp still answered. The write was answered with the rejection frame, which is a reply | spec §4b, §9 |
| M3 | In the power-up capture, the **amp's TX (ATX) went high at 4.26 s**. The knob's TX went high at 5.28 s, and the handshake followed within 1 ms | spec §5 |
| M4 | The amp answers 54–467 µs after a request. It never speaks unprompted | spec §1, §3 |
| M5 | The ESP32 heard every reply using **only its internal pull-up** on RX, with 1 kΩ in each line | spec §7, §9 (firmware v3–v7) |
| M6 | Hot-swapping worked: knob removed with the amp running, ESP32 connected, levels read back | spec §9, 27 Sep |
| M7 | The proven setup differs from the car install: WROOM-32 on GPIO16/17, **separately powered**, **1 kΩ resistors**, amp's 3V3 left unconnected | spec §7 |

And from the H2's own log in October:

| # | Fact |
|---|---|
| L1 | Under v8.2 (RX pulled up), **not a single byte** arrived in any session: every failure is "silence", not garbage |
| L2 | Under v8 (RX pulled down), the line **never read high for 1 s**: "amp line up" was never logged |
| L3 | The same H2 and v8.2 work perfectly against `amp_sim`, wired with jumpers to GPIO4/5 |

### The theories, against those facts

| Theory | Verdict | Why |
|---|---|---|
| The amp needs the knob's handshake at start-up before it answers | **Contradicted** | M1: it answered a read before any handshake. M2: after a power-cycle with no knob, it still answered |
| The ESP32 back-powers the amp and upsets its start-up | **Does not apply, and contradicted** | The H2 is powered by the amp. M2: the WROOM, powered separately during the amp's power-cycle, did not stop the amp answering |
| ATX is open-drain and needs a stronger pull-up | **Unlikely** | M3: ATX went high a second before the knob's processor started, so it was not the knob's pull-up. M5: the internal pull-up was always enough. L1: an open-drain line with a weak pull-up gives garbled bytes, not total silence |
| v8's pull-down silenced the amp | **Not the explanation** | It was a real change, but L1 shows total silence again with the pull-up back. And with a driven ATX, L2 means the line was not reaching GPIO4 then either |
| **ATX does not reach GPIO4 in the car harness** (open wire, bad contact, wrong pad) | **Consistent with everything** | L1, L2 and L3 together: the H2's RX works on jumpers but sees nothing in the car. A pin on this connector has come loose before |
| **The amp's 3V3 cannot carry the H2** | **Consistent, untested** | M7: no proven session ran the ESP32 from the amp. A sagging supply could stop the amp's remote interface transmitting, without stopping it receiving |
| The amp's remote interface is not transmitting at all (off, standby, a setting) | **Possible, untested** | Nothing measured since September shows the amp transmitting. Test 4a settles it |

So the tests that matter most are, in order:

1. **Test 2**, especially continuity from the ATX pin to the GPIO4 pad;
2. **test 4a**, to see whether the amp transmits to the knob today;
3. **test 3** and **test 4b**, for the supply.

The start-up orders in test 5 matter much less, since the start-up theories are
contradicted by M1 and M2. Run 5a as the normal case. Run 5b and 5c only if
everything else passes and 5a still fails.

## What you need

- A multimeter, with continuity (beep) mode, and ideally min/max recording to
  catch dips.
- The logic analyser (fx2lafw) and PulseView, as used for the original
  captures: 2 MHz is enough. Add the UART decoder on each channel: 230400 baud,
  8N1. The MacBook runs the analyser, **on battery**, not on its charger, so
  that no mains earth joins the car's ground.
- The phone app, v1.0.10 or later, for commands and to download the log.

### Terms used below

- **Cold boot:** amp power (ignition / REM) off for at least 10 s, then on. In
  this install the H2 boots with it.
- **Tap:** a probe clipped to a Micro-Fit pin on the breakout, alongside
  whatever is plugged in.

### For every test

1. Write down what is connected, and the order you powered things in.
2. After the test, download the log in the app. Save it with the test number in
   the file name. The boot numbers in it separate one run from the next, so the
   log does not need clearing.
3. Fill in the results table.
4. Repeat each test that involves the amp **three times**. A problem that
   comes and goes needs more than one sample.

## Stage 1: the copper (multimeter only, no H2)

### Test 1: pin voltages, amp alone

**Setup:** amp on (cold boot). **Nothing** plugged into the Micro-Fit: no knob,
no H2.

**Measure** each pin against GND: 3V3, ARX, ATX.

| Result | Meaning |
|---|---|
| 3V3 ≈ 3.3 V | You are on the right pins, and the amp's remote supply is on |
| Any pin ≈ 12 V | **Stop.** The pinout or orientation is wrong |
| ATX ≈ 3.3 V | The amp drives its TX line high itself (push-pull). No pull-up is needed |
| ATX ≈ 0 V | Unexpected, given M3. Either the amp's remote interface is not running, or ATX only pulls low. Check that 3V3 is really on |
| ATX between 0.5 and 2.8 V | A weak or floating line. Note the value |

Then **repeat with the knob plugged in**, measuring at the breakout taps. If ATX
is 0 V alone but 3.3 V with the knob, the knob provides the pull-up. That would
explain the old "~0 V on the multimeter" puzzle (spec §8).

Finally, **leave the amp on with nothing playing** until it goes into its
low-power mode, and measure 3V3 again. That shows whether the remote supply
stays on in standby, or goes off and so restarts the H2.

### Test 2: the harness

**Setup:** everything unplugged and unpowered.

1. **Continuity, end to end** (beep mode): from each Micro-Fit pin to the H2
   pin it is meant to reach.
   - ATX to GPIO4.
   - ARX to GPIO5.
   - GND to GND.
   - 3V3 to the H2's power pin.

   Then **wiggle** each wire and connector while it beeps, and listen for any
   break.
2. **Shorts:** each pair of the four wires. All must stay silent.
3. **Which physical pads?** Check against the H2 board's pinout diagram, not its
   silkscreen order. Write down which pad each wire is on. For the power wire,
   note whether it goes to the board's **3V3** pin (straight to the chip) or to
   its **5V / VIN** pin. The latter goes through a regulator, which drops the
   voltage below 3.3 V when fed 3.3 V.

Any break or short found here gets fixed before going on. Then Stage 1 is
re-run.

## Stage 2: is the amp's 3.3 V enough to run the H2?

### Test 3: the supply under the H2's load

**Setup:** H2 connected as in the car. No knob, no USB.

1. Cold boot the amp. Measure 3V3 at the breakout tap:
   - while the H2 starts;
   - once it is advertising, with no phone connected;
   - with the phone app connected and reading.

   With min/max recording, note the lowest value seen.
2. Do the same with the **knob** instead of the H2, for comparison.
3. Download the H2's log. Count its boots, and compare them with the number of
   times you switched the amp on. Extra boots, or any `reset by brownout`, mean
   the supply dropped out under the H2.

| Result | Meaning |
|---|---|
| 3V3 stays ≥ 3.2 V with the H2, as with the knob | The supply is fine. Rule this cause out |
| 3V3 sags clearly below what it does with the knob, or dips when the phone connects | **The H2 overloads the amp's remote supply.** That can disturb the amp's remote interface as well as the H2. Power the H2 another way, and re-test |
| Boots you did not cause, or brownout resets | The supply drops out entirely under the H2 |

## Stage 3: who is silent? (logic analyser)

For every test in this stage, connect the analyser to the breakout taps:

- **D0 → ATX**, what the amp sends;
- **D2 → ARX**, what the amp receives;
- **GND → GND**.

These are the same channels as the original captures. The analyser only
listens, so it is safe to add to any setup.

### Test 4a: reference, the knob alone

**Setup:** knob plugged in. **No H2.** Analyser on the taps.

1. Start capturing (about 20 s). Cold boot the amp. Turn the knob a few clicks
   after it lights up.
2. **Expect:** the handshake from spec §5 about 1 s after power-up, with a reply
   on D0 to every request on D2. Then a volume write and its echo for each click.

| Result | Meaning |
|---|---|
| Handshake and replies as in the spec | The amp and the harness work today. The fault is specific to the H2 setup |
| Requests on D2 but **no replies on D0** | **The amp is not answering even the knob.** The fault is the amp, its settings, or ATX. Stop and investigate that before testing the H2 |
| Nothing on D2 either | The analyser taps or the knob's connection are wrong |

### Test 4b (optional): the H2 on its own power

This takes the amp's 3.3 V out of the picture, to tell a supply problem apart
from a signal problem.

**Setup:** **disconnect the H2's power wire** from the amp's 3V3 pin. Keep ARX,
ATX and GND connected. Power the H2 from USB, from a phone charger or the
MacBook on battery. Analyser on the taps.

**Important:** in this setup the H2 is powered while the amp may be off. So
**switch the amp on first**, then power the H2, and unpower the H2 before
switching the amp off. That avoids feeding the amp through ARX.

Run the same steps as test 5. If this works where test 5 fails, the amp's 3.3 V
supply is the cause (see test 3).

### Test 5: the H2 as installed, under different start-up orders

**Setup:** H2 connected as in the car, powered by the amp. No knob, no USB.
Analyser on the taps. Phone app ready.

In every variant, once the H2 is running:

1. Open the app and let it connect. It reads the levels by itself.
2. Wait about 10 s, then press **Re-run start-up sequence**.
3. Wait another 10 s.
4. Download the log.

Capture D0 and D2 throughout.

| Variant | Start-up order |
|---|---|
| **5a** | The normal car case: H2 plugged in, cold boot the amp. The H2 starts with it |
| **5b** | Cold boot the amp with **nothing** plugged in. After 10 s, plug in the H2, which starts then |
| **5c** | Cold boot the amp with the **knob**. After 30 s, with the amp still on, unplug the knob and plug in the H2 |

Comparing the variants answers two questions:

- **5a vs 5b:** does it matter whether the H2 is attached while the amp starts?
- **5b vs 5c:** does it matter whether a knob greeted the amp at start-up?

For each run, read the three sources side by side:

| Analyser D2 (ARX) | Analyser D0 (ATX) | App / log | Diagnosis |
|---|---|---|---|
| Correct frames (e.g. `42 03 FC 01 2A 04 2E`) | Replies (`43 …`) | Levels shown, log says `amp answering` | **Working** |
| Correct frames | Replies | "no valid reply (silence)" | **The H2 does not hear the amp.** The fault is between the ATX tap and GPIO4: the wire, the pad, or an ATX too weak for the H2's input (compare test 1) |
| Correct frames | **Nothing** | "silence" | **The amp does not answer the H2.** Compare the variants |
| Missing or garbled frames | — | — | **The H2's TX does not reach the amp properly.** Check the ARX wire, and that GPIO5 is the pad you think it is |

Only run 5b and 5c if tests 2, 3 and 4a passed and 5a still fails: M1 and M2
already contradict the start-up theories they test.

The pattern across 5a–5c is the diagnosis:

- **5a fails, 5b and 5c work:** the amp minds the H2 being attached while it
  starts. Check test 3 for a supply sag at that moment.
- **5a and 5b fail, 5c works:** the amp needs a knob's greeting at start-up.
  The H2 does not send the handshake until it first gets an answer, or until
  you press the button, which can be too late.
- **All fail, but test 4a works:** something about the H2 differs from the knob
  every time. Look at the D0 trace for replies the H2 misses, then at tests 1
  and 3.
- **Results vary between repeats of the same variant:** an intermittent
  contact or supply. Go back to tests 2 and 3.

### Test 6: intermittent contact

**Setup:** whichever variant from test 5 works. The phone app connected,
reading the levels every few seconds (it retries by itself).

Wiggle the harness and connector, section by section. Note any moment the
levels disappear, and the matching gaps on the analyser and lines in the log.
A connection that drops while being wiggled is a hardware fault, whatever
else is true. A new boot line in the log means the power wire broke contact.

## Results table

Copy this into a note, or add rows here, as you go. "Repeat" is 1–3.

| Test | Repeat | Date / time | Connected | Start-up order | Measured / seen | Log file | Conclusion |
|---|---|---|---|---|---|---|---|
| 1 | | | amp only | | 3V3 = , ARX = , ATX = ; 3V3 in standby = | — | |
| 1 + knob | | | amp + knob | | ARX = , ATX = | — | |
| 2 | | | unpowered | | continuity: , shorts: , pads: , H2 power pin: | — | |
| 3 | | | H2, no USB | cold boot | 3V3 min: start , idle , phone ; with knob: ; boots vs power-ons: | | |
| 4a | | | knob + analyser | cold boot | handshake: , replies on D0: | — | |
| 4b | | | H2 on USB + analyser | amp first | D2: , D0: , app: | | |
| 5a | | | H2 + analyser | H2 starts with amp | D2: , D0: , app: | | |
| 5b | | | H2 + analyser | amp first, H2 after 10 s | | | |
| 5c | | | H2 + analyser | knob starts amp, swap | | | |
| 6 | | | | | | | |

## What happens after

Once the table points at a cause, fix that one thing, and repeat test 5 to
confirm it. Only then change anything else. Expected fixes, depending on
the result:

- **A broken wire, a bad contact or the wrong pad:** repair, then test 5 again.
- **ATX needs a pull-up** (unlikely, see M3 and M5): 10 kΩ from the Micro-Fit
  3V3 pin to ATX.
- **The H2 overloads the amp's 3.3 V:** power the H2 from the car's 12 V through
  a small regulator. Keep its start tied to the amp's, for example by switching
  that regulator from the amp's remote (REM) line, so the H2 still never runs
  while the amp is off.
- **The amp needs a greeting at start-up** (contradicted by M1 and M2, so
  unlikely): a firmware change. Because the H2
  starts when the amp's remote supply comes on, its own boot *is* the amp's
  power-up. So it can send the handshake 1 s after booting, exactly as the knob
  does, without waiting for an answer first.
- **The amp does not answer the knob either (test 4a):** the H2 is not the
  problem. Check the amp's PC-Tool settings and its remote input.
