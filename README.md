# SCP_protocol

Reverse engineering the SCP bus between a **MATCH UP 8DSP MK2** amplifier and an
**Audiotec Fischer CONDUCTOR** volume knob, and replacing the knob with an ESP32.

The knob talks to the amp over a 4-pin Micro-Fit connector carrying two
unidirectional UART lines. The protocol has been decoded from logic-analyser
captures far enough to read and write master volume, subwoofer level and input
selection. This repo holds the protocol notes and a bench sketch that speaks it.

## What is here

| Path | What it is | State |
|---|---|---|
| [`conductor-scp-protocol.md`](conductor-scp-protocol.md) | The protocol specification: physical layer, framing, checksum, registers, power-up handshake, open questions | Physical layer, framing, checksum, level and input commands **confirmed**; boot register meanings partly guessed |
| [`scp_bench/scp_bench.ino`](scp_bench/scp_bench.ino) | Arduino sketch: ESP32 acts as the knob, driven from the USB serial console | Written, framing verified against the spec on paper — **not yet run against real hardware** |
| `LICENSE` | MIT | — |

## The protocol in one screen

Plain full-duplex UART, **230400 baud, 8N1, 3.3 V, idle high**. The knob is the
master; the amp only ever answers. Levels are sent as **absolute values**, not
up/down steps, and the amp echoes every write.

```
SOF  LEN  ~LEN  01  CMD  REG  data…  CS
```

- `SOF` — `42` request (knob → amp), `43` reply (amp → knob)
- `LEN` — byte count from `01` up to but not including `CS`; a reply's real span is `LEN + 1`
- `~LEN` — bitwise complement, so `LEN + ~LEN = FF`
- `CMD` — `2A` read, `2B` write
- `CS` — sum of `CMD` through the last data byte, mod 256 (the `01` is **not** counted)

Ready-made frames:

| Purpose | Bytes |
|---|---|
| Read current levels | `42 03 FC 01 2A 04 2E` |
| Set master to `vv` | `42 06 F9 01 2B 04 00 vv 01 cs`, `cs = 30 + vv` |
| Set sub to `vv` | `42 06 F9 01 2B 04 01 vv 01 cs`, `cs = 31 + vv` |
| Select input `ii` | `42 06 F9 01 2B 07 ii 01 01 cs`, `cs = 34 + ii` |
| Boot "enable" write | `42 05 FA 01 2B 01 01 01 2E` |

Inputs: `00` main / analogue highlevel (green), `01` optical (yellow),
`02` extension card slot (blue). For the amp to obey an input write, the digital
source must be set to **remote-controlled**, not automatic signal detection, in
PC-Tool Source Configuration.

See the [spec](conductor-scp-protocol.md) for the power-up handshake, the
register 03 configuration dump, and what is still unknown.

## Running the bench sketch

**Prerequisites:** ESP32 board (any dev board with a spare UART), Arduino IDE or
arduino-cli with the ESP32 core installed.

Open `scp_bench/scp_bench.ino`, select your ESP32 board, upload, then open the
serial monitor at **115200 baud**.

### Wiring — knob UNPLUGGED

Two push-pull UART transmitters on one wire will fight each other, so the knob
must be disconnected before the ESP32 drives the knob→amp line.

```
SCP GND                    -> ESP32 GND
amp TX  (analyser D0 line) -> 470R -> GPIO16 (RX2)
amp RX  (analyser D2 line) <- 470R <- GPIO17 (TX2)
SCP 3.3 V rail             -> leave open
```

> **Not yet determined:** which physical Micro-Fit pin carries D0 and which
> carries D2. Identify them with a scope or analyser before connecting, and
> record the answer in the spec.

### Console commands

| Key | Action |
|---|---|
| `r` | Read current master and sub levels from register 04 |
| `m N` | Set master volume to `N` |
| `s N` | Set sub level to `N` |
| `i N` | Select input `N` (0 main, 1 optical, 2 extension) |
| `+` / `-` | Step master up / down by one |
| `e` | Send the boot "enable" write (register 01) |

`N` is parsed as **decimal**, while the spec is written in hex. `m 40` means
master = `0x28`.

Every transaction prints the transmitted frame (`TX`) and the decoded reply body
(`RX`), so the console doubles as a protocol trace. The sketch clamps levels to
`MAX_LEVEL` (`0x30`) as a safety limit, because the real end stops have not been
captured yet.

## Status and next steps

The protocol is understood well enough to control the amp; the bench sketch has
never been run against hardware. Bring-up order:

1. Identify the D0/D2 Micro-Fit pins and record them in the spec.
2. Run `r` with the knob unplugged. A valid reply confirms wiring, baud rate and
   framing in one step.
3. Answer the spec's [open questions](conductor-scp-protocol.md#8-open-questions-and-next-captures)
   on the bench — chiefly whether level writes work without the boot handshake,
   whether the ~0.6 ms intra-frame pause matters, and where the level end stops are.

## Credits and caveats

Reverse-engineered from logic-analyser captures taken at the 4-pin Micro-Fit
junction in the CONDUCTOR cable on 18 Sep 2026, using an fx2lafw analyser and
PulseView/sigrok.

This is unofficial, independently derived documentation. It is not endorsed by
Audiotec Fischer, and following it may void your warranty. Levels are written
absolutely, with no verified upper bound — start low and be careful with your
speakers and your ears.

MIT licensed. See [LICENSE](LICENSE).
