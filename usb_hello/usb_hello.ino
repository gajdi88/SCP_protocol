// Isolation test. No BLE, no second UART, no amp - nothing but the USB console.
// If this board cannot print over USB, nothing else is worth debugging.
//
// Upload with the SAME board settings you use for scp_bench, especially
// Tools -> "USB CDC On Boot: Enabled" (it is a build flag: changing the menu
// without re-uploading does nothing).
//
// Expected, within a few seconds of opening the monitor:
//
//   usb_hello: alive. Type anything - every byte is echoed with its hex value.
//   tick 1
//   tick 2
//
// Read it like this:
//   ticks appear                 -> the chip runs and USB CDC works
//   nothing at all               -> USB CDC On Boot is off, or the sketch is not running
//   ticks, but typing shows      -> the console is fine and the problem is in scp_bench
//     nothing                       (or your terminal is not sending what you think)
//
// Pressing Enter should show ONE of:
//   got 0x0D '.'   CR      - fine
//   got 0x0A '.'   LF      - fine
//   both                   - fine
//   nothing                - your terminal is set to "No line ending", which is why
//                            scp_bench never runs your command: it dispatches on CR/LF.

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);   // wait for the host to attach
  delay(200);
  Serial.print("\r\nusb_hello: alive. Type anything - every byte is echoed with its hex value.\r\n");
}

void loop() {
  static uint32_t last = 0, n = 0;
  if (millis() - last >= 1000) {                        // proof of life, once a second
    last = millis();
    Serial.printf("tick %lu\r\n", (unsigned long)++n);
  }
  while (Serial.available()) {
    int c = Serial.read();
    Serial.printf("got 0x%02X '%c'\r\n", c, (c >= 0x20 && c < 0x7F) ? (char)c : '.');
  }
}
