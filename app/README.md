# 8DSP dashboard

Single-screen Web Bluetooth control for the MATCH UP 8DSP: master volume,
subwoofer level and input selection. One self-contained HTML file, no build step,
no APK, no toolchain.

## Putting it on the phone

1. Push this folder, then in the GitHub repo: **Settings → Pages → Source:
   Deploy from a branch → `main` / `/ (root)`**.
2. Wait a minute, then open **`https://<user>.github.io/SCP_protocol/app/`** in
   **Chrome on Android**.
3. Chrome menu → **Add to Home screen**. It then launches full screen with its
   own icon.
4. Tap **Connect**, pick your `SCP-xxxx` board from Chrome's device chooser.

HTTPS is not optional: Web Bluetooth only runs in a secure context, so opening
the file over `file://` will not work. GitHub Pages serves HTTPS, which is why
it is the easy route.

## Requirements

- **Chrome on Android.** Firefox Android has no Web Bluetooth, and Safari/iOS
  never will.
- **Location services on** — Chrome needs it to scan for BLE devices.
- Chrome shows its device chooser on every connect. There is no silent
  auto-reconnect; that is a deliberate browser privacy behaviour, not a bug here.

## How it talks to the amp

It speaks the same text console as the USB serial port, over the Nordic UART
Service. On connect it sends:

| Command | Why |
|---|---|
| `v 0` | Quiet mode. Suppresses the `TX`/`RX` frame trace, so each command answers with one line instead of four |
| `?` | Reads this firmware's ceilings out of the help text and sets the slider ranges from them, so the app cannot drift out of step with the firmware |
| `r` | The amp is the source of truth: the sliders are initialised from it, never guessed |

Then `m N`, `s N` and `i N` as you use the controls.

Slider drags are coalesced to one write per 120 ms, with the final value always
sent on release. Without that, a drag would outrun both the BLE link and the
ESP32's 8-deep command queue, and the volume would lag behind your thumb.

## Known limitation

The amp does not report the selected input through any register this firmware
reads, so the input buttons show **what this app last sent**, not what the amp is
actually doing. On a fresh connect no input is highlighted. Resolving that needs
a register 07 read, which the firmware does not currently expose.
