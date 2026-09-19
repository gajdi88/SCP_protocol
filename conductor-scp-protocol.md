# CONDUCTOR ↔ MATCH UP 8DSP MK2: SCP bus protocol notes

Reverse-engineered from logic-analyser captures at the 4-pin Micro-Fit junction in the CONDUCTOR cable, 18 Sep 2026.

**Status:** physical layer, framing, checksum, volume, sub-level and input-select commands are **confirmed, and proven by replay from an ESP32 with the knob unplugged** (19 Sep 2026). Boot-time register meanings are **partly guessed**.

Confidence markers used below: **[C]** confirmed from captures, **[G]** guess consistent with the data, **[?]** unknown.

---

## 1. Summary

- The bus is **plain full-duplex UART, 230400 baud, 8N1, 3.3 V, idle high**. [C]
- **The knob is the master. The amp only ever answers.** No polling, no keep-alive, no unsolicited amp traffic. [C]
- The knob sends **absolute levels**, not up/down steps. The amp echoes every write. [C]
- Master volume and sub level are the same command with a **one-byte target selector**. [C]
- **Input selection is a write to register 07** with a one-byte input index. It is only sent on a press-and-hold confirm. [C]
- **Menu position (master / sub / input menu) is internal knob state.** Short presses and menu changes put nothing on the bus. [C]
- **The amp is the source of truth for levels.** The knob reads them once at power-up. [C]
- **Reads work at any time. Writes are rejected until register 01 is enabled**, and the amp forgets the enable every time it power-cycles. [C]
- **An ESP32 can fully replace the knob.** Read, enable, master, sub and input select all replayed successfully. [C]
- All **86 frames** across 11 captures pass every framing rule below (stop bits, length complement, length, checksum). [C]

---

## 2. Physical layer

| Item | Value | Confidence |
|---|---|---|
| Lines | 2 unidirectional data lines + 3.3 V + GND | C |
| Analyser D2 | knob → amp (knob TX) | C |
| Analyser D0 | amp → knob (amp TX) | C |
| Logic level | 3.3 V, idle high, active low start bit | C |
| Format | UART 8N1, LSB first | C |
| Bit time | 4.340 µs measured → 230 407 baud (nominal 230400) | C |
| Framing errors seen | none | C |

Which physical Micro-Fit pin is D0 and which is D2 has not been recorded here. Note it on the tap board.

---

## 3. Frame format

```
SOF  LEN  ~LEN  01  CMD  REG  data…  CS
```

| Field | Meaning | Confidence |
|---|---|---|
| SOF | `42` = request from knob, `43` = reply from amp | C |
| LEN | Byte count, see rule below | C |
| ~LEN | Bitwise complement of LEN (`LEN + ~LEN = FF`) | C |
| `01` | Constant in every frame. Address or protocol class | ? |
| CMD | `2A` = read, `2B` = write | C (by behaviour) |
| REG | Register / parameter number | C |
| data | Register-dependent | see §4, §5 |
| CS | Sum of all bytes from CMD up to but not including CS, mod 256. The `01` is **not** included | C |

**Length rule** (empirical, holds for all 86 frames):

- Request (`42`): LEN = number of bytes from `01` up to but not including CS.
- Reply (`43`): the same span is LEN + 1 bytes. The reply carries one byte its LEN does not count. Which byte that is, is unknown. [?]

**Timing:**

- Amp reply starts 54–467 µs after the end of the request (median 215 µs). [C]
- Every runtime write from the knob contains one **~0.6 ms pause** (0.58–0.62 ms). On register 04 writes it falls after byte 2 (`42 06`); on register 07 writes it falls after byte 3 (`42 06 F9`). Boot-time frames, including the boot-time write, have no pause and the amp accepts them. **The pause is a knob firmware artefact, not a protocol requirement:** the ESP32 sends every frame with no pause and the amp accepts them all. [C]

---

## 4. Level write (master volume and sub level)

Request and echo, example master volume = `0x28`:

```
knob:  42 06 F9  01 2B 04  00 28 01  58
amp:   43 05 FA  01 2B 04  00 28 01  58
```

| Byte | Meaning | Confidence |
|---|---|---|
| `2B 04` | Write register 04 (levels) | C |
| Target | `00` = master volume, `01` = subwoofer level | C |
| Level | One byte, absolute. One detent = ±1 | C |
| Trailing `01` | Constant in all 32 writes | ? |

- Checksum shortcut: `CS = 0x30 + target + level` (mod 256).
- Observed master values: `0x12`–`0x29`. Observed sub values: `0x06`–`0x17`. **End stops not yet captured.**
- Up and down are indistinguishable except by the value. [C]

---

## 4a. Input select

Sent once, when a selection in the input menu is confirmed with a press-and-hold (about 3 s). Example, switch to optical:

```
knob:  42 06 F9  01 2B 07  01 01 01  35
amp:   43 05 FA  01 2B 07  01 01 01  35
```

| Input byte | Knob colour | Input | Confidence |
|---|---|---|---|
| `00` | Green | Main (analogue highlevel). **Default** | C |
| `01` | Yellow | Optical digital | C |
| `02` | Blue | Extension card slot | C (colour and index), G (meaning, from CONDUCTOR manual) |

- The two trailing bytes `01 01` are constant in all three captures. [?]
- Checksum shortcut: `CS = 0x34 + input`.
- Turning the knob in the input menu, letting it time out, or short-pressing sends **nothing**. Only the confirmed selection reaches the bus. [C]
- For the amp to obey, the Digital source must be set to remote-controlled rather than automatic signal detection in PC-Tool Source Configuration. With automatic detection on, the amp switches by itself. [C, from bench behaviour]

---

## 4b. Write enable and the rejection frame

Found by replay from the ESP32.

| Situation | Exchange |
|---|---|
| Write attempted before enable | request `42 06 F9 01 2B 04 00 1B 01 4B` → reply `43 .. .. 01 2B 01 00 2C` |
| Enable | request `42 05 FA 01 2B 01 01 01 2E` → reply body `01 2B 01 01 01 2E` |
| Same write after enable | reply body `01 2B 04 00 1B 01 4B` (full echo) |

- **Register 01 is a write-enable.** `01 01` turns it on. [C]
- **Rejection frame:** any write while disabled is answered with `01 2B 01 00 2C`, i.e. "register 01 is 00". It names the cause instead of echoing the write. [C]
- **Reads are never gated.** Register 04 read back correctly before any enable. [C]
- **The enable does not survive an amp power-cycle.** After amp off/on with the ESP32 left running, the next write was rejected again. [C] A controller must send the enable after every amp start, or on seeing the rejection frame.
- None of the boot-time reads (registers 00, 03, 05, 06, 07, 09) are needed for control. [C]

---

## 5. Power-up handshake

Timeline from the power-up capture:

- Amp TX line goes high at 4.26 s.
- Knob TX line goes high at 5.28 s. First request follows within 1 ms.
- Entire handshake takes about 10 ms. The knob initiates everything.

| # | Knob request | Amp reply data | Interpretation |
|---|---|---|---|
| 1 | read `00` | `01 03` | Version or device type [G] |
| 2 | read `03` | 78-byte block (Appendix A) | Configuration / range table [G] |
| 3 | read `04` | `12 17 00 00` | **Current levels: master `12`, sub `17`** [C], last two bytes [?] |
| 4 | read `05` | `00` | ? |
| 5 | read `06` | `00 01 00 00 00 00 00 00 00 00 00` | ? |
| 6 | read `07` | `00 03 01 00` | **Input register.** Likely current input `00` and input count `03`, matching the three selectable colours [G] |
| 7 | read `09` | `00 00 00 96 01 00 13 88` | ? |
| 8 | write `01` = `01 01` | echoed | **Write enable** [C], see §4b |

Evidence for row 3: the first volume click after boot sent master `13` (= `12` + 1), and `17` is the last sub level set in the earlier sub-up capture.

Read request template (REG = `rr`):

```
42 03 FC  01 2A rr  CS        where CS = 2A + rr
```

So **`42 03 FC 01 2A 04 2E` reads the current master and sub levels.** [C]

---

## 6. What produces no bus traffic

- Short button press (0.6 s capture, and three presses in a 14 s capture). [C]
- Moving between the master, sub and input menus. [C]
- Turning the knob in the input menu without confirming (9 s capture, zero edges). [C]

The earlier "input selection does nothing" puzzle was an operating error, not a configuration problem: **selection needs a press-and-hold to confirm.** A short press just moves to the next menu.

---

## 7. Implications for the ESP32 gateway

- **Proven on the bench:** ESP-WROOM-32, UART2 (RX2 = GPIO16, TX2 = GPIO17), 1 kΩ in series with each data line, SCP 3.3 V rail left unconnected, knob unplugged.
- **Controller rule:** on a rejection frame, send the enable and retry the write once. That also covers the amp restarting with the ignition.
- **Encoder injection is no longer necessary.** The ESP32 can speak the protocol directly.
- **No mode switching or button emulation needed.** Master and sub are addressed independently via the target byte, and the input is set with a single register 07 write.
- **Levels can be read back at any time** with a read of register 04, so the ESP32 never has to guess state.
- **The knob and ESP32 cannot share the knob→amp line.** Two push-pull UART transmitters on one wire will fight. Options:
  - Remove the knob. ESP32 is the only master.
  - ESP32 in the middle: knob TX → ESP32 UART RX, second ESP32 UART TX → amp. Amp TX can fan out to both.
- **In the middle position, treat knob frames as ±1 deltas, not absolutes.** The knob reads the levels only at boot, so its stored value goes stale the moment the ESP32 changes the volume.

---

## 8. Open questions and next captures

| Question | How to answer |
|---|---|
| Min / max of master and sub ranges; does the knob stop sending at the limit? | Capture turning to both end stops |
| Does read `07` really return current input and input count? | Now trivial: select optical, then read register 07 from the ESP32; first data byte should become `01` |
| Does an echoed input write actually switch the amp? | Confirm in PC-Tool with the Digital source set to remote-controlled. Echo alone only proves acceptance |
| What do registers 00, 03, 05, 06, 09 hold, and do they change with configuration? | Read them from the ESP32 before and after PC-Tool changes |
| Meaning of `01 01` after the input byte | Experiment |
| Is there a separate digital-input volume target (`02`?) in register 04 | Enable "Digital volume" as a CONDUCTOR volume menu in PC-Tool and capture |
| Does the amp ever speak unprompted (mute, preset change from PC-Tool, error)? | Long capture while changing things in PC-Tool |
| Knob hot-plugged into a running amp: same handshake? | Capture it |
| Meaning of the constant `01` after ~LEN and the trailing `01` in level writes | Probably only resolvable by experiment |
| Earlier DMM reading of ~0 V idle on both data lines contradicts idle-high on the analyser | Re-measure with DMM at the same tap. Unresolved |

---

## 9. Method notes

- First capture at 200 kHz was **undersampled and undecodable**: bit time (4.34 µs) is shorter than the sample period (5 µs). Only burst timing was usable from it.
- 8 MHz was used to identify the baud rate. **2 MHz (8.7 samples per bit) is sufficient** for decoding and allows long captures.
- `.sr` files compress an idle bus to almost nothing: 15 s at 2 MHz is about 30 KB.
- PulseView: add the UART decoder on D2 and D0, 230400 baud, 8N1, LSB first.
- Command line:
  ```
  sigrok-cli -d fx2lafw --config samplerate=2m --channels D0,D2 --time 40s -o capture.sr
  ```

### ESP32 replay session, 19 Sep 2026

All with the knob unplugged. Every reply passed the checksum and length rules.

| Step | Result |
|---|---|
| Read levels, no handshake | OK: master `1B`, sub `17` |
| Write master `1B`, no handshake | **Rejected** with `2B 01 00` |
| Enable, then same write | OK, full echo |
| Master `1A`, read back | OK, read confirms `1A` |
| Sub `16`, read back, restore `17` | OK, read confirms |
| Input `01`, then `00` | Both echoed. Register 04 read unchanged, so it holds levels only |
| Amp power-cycled, write master | **Rejected** again: enable is volatile |

### Captures analysed

| File | Rate | Length | Frames | Content |
|---|---|---|---|---|
| up_then_down_then_button_press (CSV) | 200 kHz | 23.5 s | not decodable | 36 transactions, timing only |
| up8mhz | 8 MHz | 0.25 s | 4 | master `28` → `29` |
| down8mhz | 8 MHz | 0.25 s | 10 | master `29` → `25` |
| button8mhz | 8 MHz | 0.63 s | 0 | silent |
| subup | 8 MHz | 0.63 s | 16 | sub `10` → `17` |
| subdown | 8 MHz | 0.63 s | 18 | sub `0E` → `06` |
| powerup_And_three_volume_ups | 2 MHz | 14.8 s | 22 | handshake, then master `13` → `15` |
| 3button_mode_switch…volumeup | 2 MHz | 13.9 s | 10 | silent presses, then master `17` → `1B` |
| input_mode_selection | 2 MHz | 9.1 s | 0 | silent (selection never confirmed) |
| switch_to_yellow | 2 MHz | 3.2 s | 2 | input → `01` |
| switch_to_blue | 2 MHz | 2.3 s | 2 | input → `02` |
| switch_to_green_default | 2 MHz | 2.5 s | 2 | input → `00` |

Frame counts are individual frames (request and reply counted separately). Total decodable: 86, all valid.

---

## Appendix A: register 03 reply, full frame

```
43 50 AF 01 2A 03
02 00 88 13 B8 0B 40 1F 01 B2 59 00 00 01 00 00
00 FF FF FF 3C 01 58 02 A8 FD 01 FF 00 00 18 01
78 00 A8 FD FF 00 00 00 3C 01 58 02 A8 FD FF 00
00 00 3C 01 58 02 A8 FD 00 01 3C 00 FF 00 18 01
78 00 96 00 FF C8 0A 00 18 01 78 00 88 13
3E
```

Unverified observations [G]: several byte pairs read sensibly as little-endian 16-bit values, e.g. `88 13` = 5000, `B8 0B` = 3000, `40 1F` = 8000, `58 02` = 600, `A8 FD` = −600, `3C 01` = 316, `18 01` = 280, `78 00` = 120. The repeating `3C 01 58 02 A8 FD` group suggests per-control range records. Not needed for volume control.

## Appendix B: ready-made frames

| Purpose | Bytes |
|---|---|
| Read current levels | `42 03 FC 01 2A 04 2E` |
| Set master to `vv` | `42 06 F9 01 2B 04 00 vv 01 cs`, `cs = 30 + vv` |
| Set sub to `vv` | `42 06 F9 01 2B 04 01 vv 01 cs`, `cs = 31 + vv` |
| Select input `ii` (`00` main, `01` optical, `02` extension) | `42 06 F9 01 2B 07 ii 01 01 cs`, `cs = 34 + ii` |
| Write enable (needed after every amp power-up) | `42 05 FA 01 2B 01 01 01 2E` |
| Amp's "writes not enabled" reply body | `01 2B 01 00 2C` |

All values hex, checksums mod 256.
