# SCP_protocol

Reverse engineering the SCP bus between a **MATCH UP 8DSP MK2** amplifier and an
**Audiotec Fischer CONDUCTOR** volume knob, and replacing the knob with an ESP32.

The knob talks to the amp over a 4-pin Micro-Fit connector carrying two
unidirectional UART lines. The protocol is decoded far enough to read and write
master volume, subwoofer level and input selection — and, as of 19 Sep 2026,
**an ESP32 has successfully driven a real amplifier with the knob unplugged.**

## What is here

| Path | What it is | State |
|---|---|---|
| [`conductor-scp-protocol.md`](conductor-scp-protocol.md) | The protocol specification: physical layer, framing, checksum, registers, write-enable, power-up handshake, open questions | Framing, checksum, level, input-select and write-enable **confirmed on hardware**; boot register meanings partly guessed |
| [`scp_bench/scp_bench.ino`](scp_bench/scp_bench.ino) | Arduino sketch: ESP32 acts as the knob, driven from an identical console over **USB serial and BLE** | **Working against a real amp.** Read, write-enable, master, sub and input all verified |
| [`ble_echo/ble_echo.ino`](ble_echo/ble_echo.ino) | Stage 0 BLE bring-up test: Nordic UART Service echo, no SCP involvement | Written, logic unit-tested; **not yet flashed** |
| `LICENSE` | MIT | — |

## The protocol in one screen

Plain full-duplex UART, **230400 baud, 8N1, 3.3 V, idle high**. The knob is the
master; the amp only ever answers, and never speaks unprompted. Levels are sent
as **absolute values**, not up/down steps, and the amp echoes every accepted
write.

```
SOF  LEN  ~LEN  01  CMD  REG  data…  CS
```

- `SOF` — `42` request (knob → amp), `43` reply (amp → knob)
- `LEN` — byte count from `01` up to but not including `CS`; a reply's real span is `LEN + 1`
- `~LEN` — bitwise complement, so `LEN + ~LEN = FF`
- `CMD` — `2A` read, `2B` write
- `CS` — sum of `CMD` through the last data byte, mod 256 (the `01` is **not** counted)

### Writes are gated

**Reads work at any time. Writes are rejected until register 01 is enabled, and
the amp forgets the enable on every power-cycle.**

A write attempted while disabled is answered with `01 2B 01 00 2C` — the amp
reports register 01's value instead of echoing your frame, so the rejection is
self-identifying. Send the enable and retry:

```
42 05 FA 01 2B 01 01 01 2E
```

A controller should react to the rejection frame rather than track amp power
state — that also covers the amp restarting with the ignition.

### Ready-made frames

| Purpose | Bytes |
|---|---|
| Read current levels | `42 03 FC 01 2A 04 2E` |
| Write enable (needed after every amp power-up) | `42 05 FA 01 2B 01 01 01 2E` |
| Amp's "writes not enabled" reply body | `01 2B 01 00 2C` |
| Set master to `vv` | `42 06 F9 01 2B 04 00 vv 01 cs`, `cs = 30 + vv` |
| Set sub to `vv` | `42 06 F9 01 2B 04 01 vv 01 cs`, `cs = 31 + vv` |
| Select input `ii` | `42 06 F9 01 2B 07 ii 01 01 cs`, `cs = 34 + ii` |

Inputs: `00` main / analogue highlevel (green), `01` optical (yellow),
`02` extension card slot (blue). For the amp to obey an input write, the digital
source must be set to **remote-controlled**, not automatic signal detection, in
PC-Tool Source Configuration.

See the [spec](conductor-scp-protocol.md) for the power-up handshake, the
register 03 configuration dump, and what is still unknown.

## Running the bench sketch

**Prerequisites:** ESP32 board with a spare UART, Arduino IDE or arduino-cli
with the ESP32 core. Verified on an ESP-WROOM-32 using UART2.

> On an **ESP32-WROVER**, GPIO16/17 are wired to the PSRAM die — move `PIN_RX` /
> `PIN_TX` to free pins. The ESP32 UART matrix will route Serial2 anywhere.

Open `scp_bench/scp_bench.ino`, select your board, upload, then open the serial
monitor at **115200 baud**.

> **Set Tools → Partition Scheme → "Huge APP (3MB No OTA)" first.** The sketch
> carries the BLE stack (~1.3 MB) and the default scheme gives the app only
> 1.2 MB. Arduino IDE 2.x stores this per sketch, so setting it for `ble_echo`
> does not carry over.

### Wiring — knob UNPLUGGED

Two push-pull UART transmitters on one wire will fight, so the knob must be
disconnected before the ESP32 drives the knob→amp line.

Breakout Micro-Fit connector, **viewed from the amp side, latching tab at the top**:

```
        latching tab
   +---------+---------+
   |   ARX   |   ATX   |
   +---------+---------+
   |   GND   |   3V3   |
   +---------+---------+
```

| Pin | Carries | Connect to |
|---|---|---|
| ARX (top left) | Amp RX: knob → amp (analyser D2) | GPIO17 (TX2) via 1 kΩ |
| ATX (top right) | Amp TX: amp → knob (analyser D0) | GPIO16 (RX2) via 1 kΩ |
| GND (bottom left) | Ground | ESP32 GND |
| 3V3 (bottom right) | Amp 3.3 V rail | leave open |

Labels are from the amp's point of view: the ESP32 **drives** ARX and
**listens** to ATX. The layout mirrors left-right if you look at the mating half
or the cable side instead, so check your orientation.

The series resistors are what make a wiring mistake survivable: within the
3.3 V domain, worst-case contention current is ~3.3 mA, safe on both ends.

Before connecting, DMM every Micro-Fit pin against ground with the amp powered.
You should find exactly two data lines near 3.3 V, one 3.3 V rail and one
ground. **If any pin reads 12 V, the pinout assumption is wrong — stop.**

Watch for ground loops: if your PC and the amp's supply are both mains-earthed,
joining the grounds puts current through your USB cable. Run the laptop on
battery, or use a USB isolator.

### Console commands

The same console is live on **USB serial and BLE at once**, so you can drive the
amp from the Mac while watching the frame trace on USB. Over BLE it advertises
as `SCP-Bench` on the Nordic UART Service; `ble_echo/echo_test.py` works as a
BLE terminal against it unchanged.

`N` is **decimal**, or hex when written `0x..`. A value that doesn't parse
completely is refused, not read as zero.

| Key | Action |
|---|---|
| `r` | Read current master and sub levels from register 04 |
| `m N` | Set master volume to `N` |
| `s N` | Set sub level to `N` |
| `i N` | Select input `N` (0 main, 1 optical, 2 extension) |
| `+` / `-` | Step master up / down by one, always re-reading the amp first |
| `e` | Send the write-enable (register 01) |
| `?` | Help, including the current ceilings |

Writes **auto-enable and retry once** on seeing the rejection frame, so `m` /
`s` / `i` work without sending `e` by hand.

Every transaction prints the transmitted frame (`TX`) and the decoded reply body
(`RX`), so the console doubles as a protocol trace.

### Level ceilings

Master and sub have **separate** limits, each set to the highest value ever
observed for that target — `MAX_MASTER = 0x29` (41) and `MAX_SUB = 0x17` (23).
Out-of-range values are **refused, not clamped**, so you never get an `OK` for a
command you didn't issue. Raise them only after capturing the real end stops.

First bring-up is best done with speakers disconnected and no source playing.
A good first write is a no-op: run `r`, then write back the exact value it
reported.

## Bluetooth

The goal is to drive the bench over BLE instead of USB. `ble_echo/` is step one:
a standalone sketch that exposes the **Nordic UART Service** and echoes lines
back in uppercase. It never touches the SCP bus, so if it misbehaves the problem
is BLE, the board or the partition scheme — not the protocol code.

```
service  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
  ...0002...  write   central -> ESP32
  ...0003...  notify  ESP32 -> central
```

**Set Tools → Partition Scheme → "Huge APP (3MB No OTA)" before uploading.** The
BLE stack is ~1.3 MB and the default scheme gives the app only 1.2 MB.

Validate with nRF Connect or LightBlue: scan for `SCP-Bench`, connect, enable
notifications on the `...0003...` characteristic, write `hello` to `...0002...`,
expect `HELLO` back. A `beat N` heartbeat every 5 s confirms the notify path
without typing. USB serial stays live and echoes the same way, so the sketch is
testable with no phone at all.

### From the Mac

Two scripts in `ble_echo/` drive it from Python using
[Bleak](https://github.com/hbldh/bleak). They carry
[PEP 723](https://peps.python.org/pep-0723/) inline dependency metadata, so
[uv](https://docs.astral.sh/uv/) handles everything — no virtualenv to create,
activate or clean up:

```
uv run scan.py        # confirm SCP-Bench is advertising
uv run echo_test.py   # type a line, get it back uppercased
```

macOS gates BLE per-application, and a terminal without permission scans
successfully and finds **nothing, with no error**. If `scan.py` comes back empty,
grant it in System Settings → Privacy & Security → Bluetooth. Note `SCP-Bench`
will *not* appear in System Settings → Bluetooth — that panel only lists Classic
and paired devices, and nothing here needs pairing.

`scp_bench` now carries the same service, so the whole console works over BLE.
`ble_echo/` is kept as an isolation tool: if BLE misbehaves later, it tells you
whether the problem is BLE or the protocol code.

## Status and next steps

The protocol is proven end-to-end for level and input control. What remains:

1. Confirm in PC-Tool that an echoed input write actually **switches** the amp —
   the echo so far only proves the frame was accepted.
2. Capture the real master and sub end stops, then raise the ceilings.
3. Work the remaining [open questions](conductor-scp-protocol.md#8-open-questions-and-next-captures):
   the contents of registers 00, 03, 05, 06, 09, the meaning of the constant
   `01` bytes, and whether the amp ever speaks unprompted.

Beyond the bench: the middle position from §7 — knob TX → ESP32 → amp, treating
knob frames as ±1 deltas — needs a second UART and is not implemented yet.

## Credits and caveats

Reverse-engineered from logic-analyser captures taken at the 4-pin Micro-Fit
junction in the CONDUCTOR cable on 18 Sep 2026 using an fx2lafw analyser and
PulseView/sigrok, then verified by replay from an ESP32 on 19 Sep 2026.

This is unofficial, independently derived documentation. It is not endorsed by
Audiotec Fischer, and following it may void your warranty. Levels are written
absolutely, with end stops still uncaptured — start low and be careful with your
speakers and your ears.

MIT licensed. See [LICENSE](LICENSE).
