#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["bleak>=3.0,<4"]
# ///
"""Talk to the Stage 0 BLE echo sketch over the Nordic UART Service.

    uv run echo_test.py

Type a line, get it back in UPPERCASE. A 'beat N' arrives every 5 s on its own,
which proves notifications work even if you type nothing. Ctrl-C to quit.
"""
import asyncio
import sys

from bleak import BleakClient, BleakScanner

TARGET = "SCP-Bench"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # we WRITE here
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # we get NOTIFIES here


def on_notify(_sender, data: bytearray) -> None:
    # The sketch chunks at 20 bytes and terminates lines with CRLF, so just
    # stream it out and let the line breaks fall where they may.
    sys.stdout.write(data.decode("utf-8", errors="replace"))
    sys.stdout.flush()


async def main() -> int:
    print(f"looking for {TARGET} ...")
    device = await BleakScanner.find_device_by_name(TARGET, timeout=10.0)
    if device is None:
        print(f"not found. Run 'uv run scan.py' first to see what the Mac can see.")
        return 1

    print(f"connecting to {device.address} ...")
    async with BleakClient(device) as client:
        await client.start_notify(NUS_TX, on_notify)
        print("connected. Type a line and press return; Ctrl-C to quit.\n")

        loop = asyncio.get_running_loop()
        while True:
            # readline() blocks, so keep it off the event loop or notifications stall
            line = await loop.run_in_executor(None, sys.stdin.readline)
            if not line:                      # EOF, e.g. Ctrl-D
                break
            payload = line.rstrip("\n") + "\n"
            await client.write_gatt_char(NUS_RX, payload.encode(), response=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        print()
