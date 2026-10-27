# SCP Remote (Android)

The native version of the web app in `../app/`: master volume, subwoofer level
and input selection on one screen. It also does one thing the web app cannot:
while it is connected, **the phone's volume buttons change the amp's main
volume, including with the screen off.** The buttons never touch the
subwoofer; that is set from the screen only.

## Installing

1. On the phone, open
   **https://github.com/gajdi88/SCP_protocol/releases/latest/download/scp-remote.apk**
2. Open the downloaded file. Android asks once to allow your browser to
   "install unknown apps"; allow it, then tap **Install**.
3. Open **SCP Remote**, grant the Bluetooth (and notification) permissions, and
   tap **Allow running in the background** when offered.

Updates install the same way, over the top of the old version. The version is
shown at the bottom of the screen.

## Using it

Opening the app starts the connection. The first time, it searches for any
`SCP-` board and remembers it; after that it only looks for that one.

A notification shows the connection and volume while it runs. While connected:

- volume up/down on the phone steps the amp's main volume by one, 0 to 60;
- with the screen on, Android shows its volume slider, scaled to the amp.

To give the volume buttons back to the phone, tap **Stop** in the notification
or the app. If the link drops, the buttons revert to the phone until it
reconnects; it retries by itself, backing off to every 30 s while the amp is off.

The input buttons show what the app last sent, as in the web app: the amp does
not report its selected input.

**Forget this amp** clears the remembered board, for when it is replaced.

## Limitations

- **One connection at a time.** The ESP32 bridge accepts one Bluetooth client, so
  stop this app before using the web app, and vice versa.
- **Another app playing audio** on the phone can take the volume buttons back,
  because Android gives them to the most recent playing media.
- Some phone makers stop background apps aggressively. If the connection dies
  with the screen off, check that battery optimisation is off for SCP Remote.

## How it works

`AmpService` is a foreground service that holds the Bluetooth link (Nordic UART
Service, the same text console the web app uses) and owns a media session
with *remote* playback. Its `VolumeProvider` is what Android hands volume key
presses to, the same mechanism Cast apps use to drive a speaker's volume.

Each press sets an absolute level (`m N`). Presses that arrive while a write
is in flight are coalesced, so holding the button cannot flood the ESP32's
8-deep command queue. If the last confirmed level is more than 15 s old, the
app re-reads it (`r`) first, so a turn of the physical knob is respected.

## Building

GitHub Actions builds it (`.github/workflows/android.yml`) on every change under
`android/` and, on `main`, publishes it as the latest release. Locally:
`./gradlew assembleRelease` with an Android SDK.

Android only installs an update if it is signed with the same key as the
installed app. The key is not in the repo, which is public. The first build on
`main` generates one and the GitHub Actions cache keeps it for later builds.
A cache entry unused for 7 days is evicted. If that happens, the next APK has a
new key and Android refuses it as an update: uninstall SCP Remote once and
install the new one. To make the key permanent, store a base64 keystore as the
repository secret `SCP_KEYSTORE_B64`. The build then uses that instead, which
also means one uninstall when you switch.
