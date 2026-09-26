# SCP amp simulator

An ESP32 that pretends to be a MATCH UP 8DSP MK2, so `scp_bench` can be exercised
and regression-checked with no amplifier, no speakers and nothing at risk.

No Bluetooth: this board has one job, and leaving BLE out keeps it small and means
a failure here can only be the protocol.

## Wiring

Cross over, and share a ground:

```
amp_sim PIN_TX  ---->  scp_bench PIN_RX
amp_sim PIN_RX  <----  scp_bench PIN_TX
amp_sim GND     -----  scp_bench GND
```

Default pins are GPIO16/17 on a WROOM-32 and GPIO4/5 on an ESP32-H2 — check the
latter against your board's pinout. 1 kΩ in each line is optional between two
3.3 V boards you control, but costs nothing and survives a miswire.

## What it models

| Behaviour | Source |
|---|---|
| Framing, `LEN`/`~LEN`, checksum over `CMD`…last data byte | spec §3 |
| Answers only when asked, never speaks first | spec §1 |
| Register 01 gates writes, and forgets the gate on power-up | spec §4b |
| A write while disabled is answered `01 2B 01 00`, naming the cause | spec §4b |
| Master and sub levels, one byte each, absolute | spec §4 |
| Input selection, and register 07 reporting it back | spec §4a |
| The full boot handshake: registers 00, 03, 04, 05, 06, 07, 09 | spec §5, Appendix A |
| A ~200 µs answer delay | spec §3 (real amp: 54–467 µs) |

Register 03's 78-byte reply is the real captured block, so the handshake can be
replayed against realistic data if `scp_bench` ever gains a generic read.

## Console (115200)

| Key | Action |
|---|---|
| `?` | Help and current state |
| `m N` / `s N` / `i N` | Preset master, sub, input — set up a starting condition |
| `p` | Simulate an amp power-cycle: drops the write enable |
| `x` | Deaf mode: stop answering, to exercise the controller's timeout path |
| `v` | Verbose frame logging on/off |

It counts requests, malformed frames and refused writes, and **every rejection
says why** — "checksum 2E, expected 6D", "LEN 03 and ~LEN FB do not sum to FF".
That is the point of having a simulator: break the controller's framing and it
tells you how, instead of just going quiet.

## What it deliberately does not do

Reads of registers it does not model get **silence**, not a guess. What the real
amp does with an unknown register was never captured, and inventing a behaviour
the controller might come to rely on would make the simulator worse than useless.

## Testing it

`x` (deaf) exercises `scp_bench`'s "RX: no valid reply" path and the app's read
retries. `p` exercises the auto-enable-and-retry path: after a simulated
power-cycle the next write is refused, and a correct controller recovers by
itself.
