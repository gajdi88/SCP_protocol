// SCP bench controller v2: ESP32 takes the place of the CONDUCTOR knob.
// Protocol per conductor-scp-protocol.md (UART 230400 8N1, 3.3 V, idle high).
//
// Wiring (knob UNPLUGGED, ESP32 on the amp-side half of the Micro-Fit):
//   SCP GND                      -> ESP32 GND
//   AMP TX (old analyser D0)     -> 1k -> RX2 (GPIO16)
//   AMP RX (old analyser D2)     <- 1k <- TX2 (GPIO17)
//   SCP 3.3 V rail               -> leave open
//
// USB console, 115200. Numbers are DECIMAL unless written 0x.. :
//   r        read levels          m N   master level       s N   sub level
//   i N      input (0 main, 1 optical, 2 ext)              + / - master step
//   e        send write-enable    ?     help
//
// v2 changes, all from bench results:
//   - amp rejects writes with "2B 01 00" until register 01 is enabled, and forgets the
//     enable when it power-cycles: writes now auto-enable and retry once.
//   - out-of-range levels are REFUSED, not silently clamped. Master and sub have
//     SEPARATE ceilings, each the highest value ever observed for that target.
//   - numeric arguments must parse completely: a typo is refused, not read as 0.
//   - accepts CR or LF line endings (no 1 s lag in `screen`), ignores line noise.

#include <Arduino.h>

static const int PIN_RX = 16, PIN_TX = 17;
// Separate ceilings. Each is the highest value ever observed for that target, so the
// sub cannot be driven to a master-sized number. Raise only after capturing the real
// end stops (spec section 8).
static const uint8_t MAX_MASTER = 0x29;  // 41, from the master volume captures
static const uint8_t MAX_SUB    = 0x17;  // 23, from the sub level captures

HardwareSerial &scp = Serial2;
uint8_t master = 0, sub = 0;

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
  Serial.print(tag); for (size_t k = 0; k < n; k++) Serial.printf(" %02X", b[k]); Serial.print("\r\n");
}

size_t transact(uint8_t cmd, uint8_t reg, const uint8_t *data, uint8_t n, uint8_t *body, size_t cap) {
  uint8_t req[32]; size_t len = buildRequest(req, cmd, reg, data, n);
  while (scp.available()) scp.read();          // flush stale bytes
  scp.write(req, len); scp.flush();
  dump("TX", req, len);
  size_t r = readReply(body, cap);
  if (r) dump("RX", body, r); else Serial.print("RX: no valid reply\r\n");
  return r;
}

// ---- commands ------------------------------------------------------------
bool enableWrites() {                             // write reg 01 = 01 01
  uint8_t d[2] = {0x01, 0x01}, body[16];
  size_t r = transact(0x2B, 0x01, d, 2, body, sizeof body);
  return r >= 6 && body[1] == 0x2B && body[2] == 0x01 && body[3] == 0x01 && body[4] == 0x01;
}

// amp's "writes not enabled" reply: 01 2B 01 00 CS
bool isRejection(const uint8_t *b, size_t r) { return r == 5 && b[1] == 0x2B && b[2] == 0x01 && b[3] == 0x00; }

bool writeReg(uint8_t reg, uint8_t a, uint8_t b, uint8_t c) {
  uint8_t d[3] = {a, b, c}, body[16];
  for (int attempt = 0; attempt < 2; attempt++) {
    size_t r = transact(0x2B, reg, d, 3, body, sizeof body);
    if (r >= 7 && body[1] == 0x2B && body[2] == reg && body[3] == a && body[4] == b && body[5] == c) return true;
    if (attempt == 0 && isRejection(body, r)) {
      Serial.print("amp says writes not enabled: enabling and retrying\r\n");
      if (!enableWrites()) return false;
    } else return false;
  }
  return false;
}

bool readLevels() {
  uint8_t body[16];
  size_t r = transact(0x2A, 0x04, nullptr, 0, body, sizeof body);
  if (r < 6 || body[1] != 0x2A || body[2] != 0x04) return false;
  master = body[3]; sub = body[4];
  Serial.printf("master=%u (0x%02X)  sub=%u (0x%02X)\r\n", master, master, sub, sub);
  return true;
}

bool setLevel(uint8_t target, long v) {           // target 0 = master, 1 = sub
  const char *name = target ? "sub" : "master";
  uint8_t limit = target ? MAX_SUB : MAX_MASTER;
  if (v < 0 || v > limit) {
    Serial.printf("REFUSED: %s %ld is outside 0..%u\r\n", name, v, limit);
    return false;
  }
  if (!writeReg(0x04, target, (uint8_t)v, 0x01)) return false;
  (target ? sub : master) = (uint8_t)v; return true;
}

bool setInput(long idx) {
  if (idx < 0 || idx > 2) { Serial.print("REFUSED: input must be 0, 1 or 2\r\n"); return false; }
  return writeReg(0x07, (uint8_t)idx, 0x01, 0x01);
}

// ---- console -------------------------------------------------------------
// Strict parse: the WHOLE argument must be a number. Base 10 unless written 0x..,
// so a leading zero is not octal, and a typo is refused rather than read as 0.
bool parseNum(const String &s, long &out) {
  if (!s.length()) return false;
  bool hex = s.startsWith("0x") || s.startsWith("0X");
  char *end = nullptr;
  long v = strtol(s.c_str(), &end, hex ? 16 : 10);
  if (end == s.c_str() || *end != '\0') return false;
  out = v; return true;
}

void badNum() { Serial.print("REFUSED: need a number, e.g. 'm 30' or 'm 0x1E'\r\n"); }

void handle(String line) {
  line.trim(); if (!line.length()) return;
  char c = line[0];
  String arg = line.substring(1); arg.trim();
  long v = 0;
  bool numOk = parseNum(arg, v);
  bool ok = false;
  switch (c) {
    case 'r': ok = readLevels(); break;
    case 'e': ok = enableWrites(); break;
    case 'm': if (!numOk) { badNum(); break; } ok = setLevel(0, v); break;
    case 's': if (!numOk) { badNum(); break; } ok = setLevel(1, v); break;
    case 'i': if (!numOk) { badNum(); break; } ok = setInput(v); break;
    case '+': case '-':
      if (!readLevels()) break;                    // always step from the amp's truth
      ok = setLevel(0, (long)master + (c == '+' ? 1 : -1)); break;
    case '?':
      Serial.printf("r | e | m N | s N | i N | + | -   (N decimal, or 0x.. hex)\r\n"
                    "master 0..%u, sub 0..%u, input 0..2\r\n", MAX_MASTER, MAX_SUB);
      return;
    default: return;                               // ignore line noise
  }
  Serial.print(ok ? "OK\r\n" : "FAILED\r\n");
}

void setup() {
  Serial.begin(115200);
  scp.begin(230400, SERIAL_8N1, PIN_RX, PIN_TX);
  delay(500);
  Serial.print("\r\nSCP bench v2 ready. '?' for help.\r\n");
}

void loop() {
  static String buf;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r' || ch == '\n') { handle(buf); buf = ""; }
    else if (ch >= 0x20 && ch < 0x7F) { if (buf.length() < 16) buf += ch; }
    else buf = "";                                 // non-printable: drop the line
  }
}
