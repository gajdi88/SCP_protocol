#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["bleak>=3.0,<4"]
# ///
"""List every BLE device the Mac can see, so you can confirm SCP-Bench is advertising.

    uv run scan.py

On macOS the "address" is an opaque CoreBluetooth UUID, not a MAC address, and it
differs from one Mac to another. Match on the NAME, never on the address.
"""
import asyncio
import sys

from bleak import BleakScanner

TARGET = "SCP-Bench"


async def main() -> int:
    print("scanning for 8 s ...\n")
    devices = await BleakScanner.discover(timeout=8.0)

    if not devices:
        print("No BLE devices at all.")
        print("That usually means your terminal lacks Bluetooth permission:")
        print("  System Settings -> Privacy & Security -> Bluetooth -> enable your terminal")
        return 1

    hit = None
    for d in sorted(devices, key=lambda x: (x.name or "").lower()):
        name = d.name or "(no name)"
        mark = ""
        if d.name == TARGET:
            hit, mark = d, "   <-- this is the ESP32"
        print(f"  {name:<28} {d.address}{mark}")

    print()
    if hit:
        print(f"Found {TARGET}. Next: uv run echo_test.py")
        return 0
    print(f"{len(devices)} device(s) seen, but no {TARGET!r}.")
    print("Check the ESP32's USB serial for 'advertising as \"SCP-Bench\"',")
    print("and make sure nothing else (a phone) is already connected to it.")
    return 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
