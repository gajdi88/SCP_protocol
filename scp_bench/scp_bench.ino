// SCP bench controller: ESP32 takes the place of the CONDUCTOR knob.
// Protocol per conductor-scp-protocol.md (UART 230400 8N1, 3.3 V, idle high).
//
// Wiring (knob UNPLUGGED, ESP32 on the amp-side half of the Micro-Fit):
//   SCP GND                         -> ESP32 GND
//   amp TX  (analyser D0 line)      -> 470R -> GPIO16 (RX2)
//   amp RX  (analyser D2 line)      <- 470R <- GPIO17 (TX2)
//   SCP 3.3 V rail                  -> leave open (or 10k -> GPIO34 as "amp is on" sense)
//
// USB serial console (115200):  r = read levels | m 30 = master | s 12 = sub
//   i 1 = input (0 main, 1 optical, 2 ext) | + / - = master step | e = boot "enable" write

#include <Arduino.h>

static const int PIN_RX = 16, PIN_TX = 17;
static const uint8_t MAX_LEVEL = 0x30;   // safety clamp. Raise only after you have captured the real end stop.

HardwareSerial &scp = Serial2;
uint8_t master = 0, sub = 0;
bool levelsKnown = false;

// ---- framing -------------------------------------------------------------
// request: 42 LEN ~LEN 01 CMD REG data... CS     LEN = 3 + ndata, CS = CMD+REG+data
size_t buildRequest(uint8_t *out, uint8_t cmd, uint8_t reg, const uint8_t *data, uint8_t n) {
  uint8_t len = 3 + n, cs = cmd + reg, i = 0;
  out[i++] = 0x42; out[i++] = len; out[i++] = (uint8_t)~len;
  out[i++] = 0x01; out[i++] = cmd; out[i++] = reg;
  for (uint8_t k = 0; k < n; k++) { out[i++] = data[k]; cs += data[k]; }
  out[i++] = cs;
  return i;
}

// reply: 43 LEN ~LEN then LEN+2 bytes (01 CMD REG data... CS). Returns body length, 0 on failure.
size_t readReply(uint8_t *body, size_t cap, uint32_t timeoutMs = 50) {
  uint8_t hdr[3]; size_t got = 0; uint32_t t0 = millis();
  while (got < 3 && millis() - t0 < timeoutMs)
    if (scp.available()) { uint8_t b = scp.read(); if (got || b == 0x43) hdr[got++] = b; }
  if (got < 3 || (uint8_t)(hdr[1] + hdr[2]) != 0xFF) return 0;
  size_t need = hdr[1] + 2; if (need > cap) return 0;
  got = 0;
  while (got < need && millis() - t0 < timeoutMs) if (scp.available()) body[got++] = scp.read();
  if (got < need) return 0;
  uint8_t cs = 0; for (size_t k = 1; k + 1 < need; k++) cs += body[k];
  return cs == body[need - 1] ? need : 0;
}

void dump(const char *tag, const uint8_t *b, size_t n) {
  Serial.print(tag); for (size_t k = 0; k < n; k++) Serial.printf(" %02X", b[k]); Serial.println();
}

size_t transact(uint8_t cmd, uint8_t reg, const uint8_t *data, uint8_t n, uint8_t *body, size_t cap) {
  uint8_t req[32]; size_t len = buildRequest(req, cmd, reg, data, n);
  while (scp.available()) scp.read();          // flush stale bytes
  scp.write(req, len); scp.flush();
  dump("TX", req, len);
  size_t r = readReply(body, cap);
  if (r) dump("RX", body, r); else Serial.println("RX: no valid reply");
  return r;
}

// ---- commands ------------------------------------------------------------
bool writeReg(uint8_t reg, uint8_t a, uint8_t b, uint8_t c) {
  uint8_t d[3] = {a, b, c}, body[16];
  size_t r = transact(0x2B, reg, d, 3, body, sizeof body);
  // a good reply echoes 01 2B reg a b c
  return r >= 7 && body[1] == 0x2B && body[2] == reg && body[3] == a && body[4] == b && body[5] == c;
}

bool readLevels() {
  uint8_t body[16];
  size_t r = transact(0x2A, 0x04, nullptr, 0, body, sizeof body);
  if (r < 6 || body[1] != 0x2A || body[2] != 0x04) return false;
  master = body[3]; sub = body[4]; levelsKnown = true;
  Serial.printf("master=%u (0x%02X)  sub=%u (0x%02X)\n", master, master, sub, sub);
  return true;
}

bool setLevel(uint8_t target, uint8_t v) {       // target 0 = master, 1 = sub
  if (v > MAX_LEVEL) v = MAX_LEVEL;
  if (!writeReg(0x04, target, v, 0x01)) return false;
  (target ? sub : master) = v; return true;
}

bool setInput(uint8_t idx) { return writeReg(0x07, idx, 0x01, 0x01); }

bool enableWrite() {                              // the knob's last boot frame: write reg 01 = 01 01
  uint8_t d[2] = {0x01, 0x01}, body[16];
  return transact(0x2B, 0x01, d, 2, body, sizeof body) > 0;
}

// ---- main ----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  scp.begin(230400, SERIAL_8N1, PIN_RX, PIN_TX);
  delay(1500);                                    // the real knob waits about 1 s after the amp's line goes high
  Serial.println("SCP bench ready. Try 'r' first.");
}

void loop() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n'); line.trim(); if (!line.length()) return;
  char c = line[0]; int v = line.substring(1).toInt();
  bool ok = false;
  switch (c) {
    case 'r': ok = readLevels(); break;
    case 'e': ok = enableWrite(); break;
    case 'm': ok = setLevel(0, v); break;
    case 's': ok = setLevel(1, v); break;
    case 'i': ok = setInput(v); break;
    case '+': case '-':
      if (!levelsKnown && !readLevels()) break;
      ok = setLevel(0, c == '+' ? master + 1 : (master ? master - 1 : 0)); break;
    default: Serial.println("r | e | m N | s N | i N | + | -"); return;
  }
  Serial.println(ok ? "OK" : "FAILED");
}
