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
   This is a one-time pairing step: from then on the app connects by itself
   every time it starts.

HTTPS is not optional: Web Bluetooth only runs in a secure context, so opening
the file over `file://` will not work. GitHub Pages serves HTTPS, which is why
it is the easy route.

## Requirements

- **Chrome on Android.** Firefox Android has no Web Bluetooth, and Safari/iOS
  never will.
- **Location services on** — Chrome needs it to scan for BLE devices.
- Chrome shows its device chooser whenever **Connect** is tapped; a page cannot
  scan and pick a device on its own. Only the first connection needs it, though
  (see Auto-connect below).

## Auto-connect

Once the amp has been chosen in the chooser, Chrome remembers the permission.
On every later launch the app asks Chrome for the remembered amp
(`navigator.bluetooth.getDevices()`), waits up to 15 s for it to advertise
(`watchAdvertisements()`, where available), and connects without a tap. The amp
picked most recently is preferred, in case an old board is still remembered.

It also reconnects when the app comes back to the foreground, because Android
often drops the link while the screen is locked. Tapping **Disconnect** holds
that off until you tap **Connect** or relaunch the app.

If this Chrome lacks `getDevices()`, the app behaves as before (tap Connect
each time), and the version line at the bottom says
"auto-connect unavailable in this browser".

## Offline use

Open the app once with an internet connection and it saves itself on the phone
(a service worker, `sw.js`). After that it opens with no connection at all, and
Bluetooth needs none. The version line at the bottom adds
"saved for offline" once the copy is stored.

Whenever the phone does have a connection, the app loads the latest published
version and refreshes the saved copy, so updates still arrive by themselves. If
the network does not answer within 3 s, the saved copy is used instead.

Android can discard saved site data when storage runs very low or when Chrome's
site data is cleared. Installing via **Add to Home screen** makes that much
less likely. If it ever happens, open the app once while online.

## Version

The line at the bottom of the screen shows `APP_VERSION` from `index.html`.
Bump it with every published change, so you can tell at a glance whether the
phone has loaded the new build. GitHub Pages caches for about 10 minutes; if the
old version persists, close the app fully and reopen it.

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

## Reliability

**Connecting retries.** Android's BLE stack fails a first connect attempt often
enough that one try is not a fair test — the familiar "status 133". The app
retries up to four times with a growing pause, which needs no new tap because
only the device chooser requires a user gesture. An unexpected drop triggers up
to three reconnect attempts; tapping Disconnect yourself does not.

**Reading levels retries.** A single `r` can go unanswered if the amp is settling
or the command is lost on a link that has just come up. The app asks up to four
times, stops the instant an answer arrives, and only then reports the amp as
silent. The ⟳ button re-reads on demand.

## Known limitation

The amp does not report the selected input through any register this firmware
reads, so the input buttons show **what this app last sent**, not what the amp is
actually doing. On a fresh connect no input is highlighted. Resolving that needs
a register 07 read, which the firmware does not currently expose.
