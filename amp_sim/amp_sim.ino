// SCP amp simulator: an ESP32 that pretends to be a MATCH UP 8DSP MK2, so the
// bench controller can be tested and regression-checked with no amplifier and no
// speakers. No Bluetooth here, on purpose - this board has one job.
//
// Behaviour per conductor-scp-protocol.md: 230400 8N1, the amp only ever answers,
// register 01 gates writes and forgets the gate on power-up, and a write while
// disabled is answered by naming the cause instead of echoing.
//
// WIRING - cross over, and both boards must share a ground:
//
//   amp_sim PIN_TX  ---->  scp_bench PIN_RX
//   amp_sim PIN_RX  <----  scp_bench PIN_TX
//   amp_sim GND     -----  scp_bench GND
//
// No series resistors needed between two 3.3 V boards you control, though 1k in
// each line costs nothing and protects against a miswire.
//
// USB console (115200). Numbers are DECIMAL unless written 0x.. :
//   ?      help and current state          m N / s N / i N   preset master/sub/input
//   p      simulate an amp power-cycle (drops the write enable)
//   x      deaf mode: stop answering, to exercise the controller's timeout path
//   v      verbose frame logging on/off

#include <Arduino.h>

// Only the original ESP32 and the S3 have three UARTs, so only they have Serial2.
#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S3)
  #define SCP_SERIAL Serial2
#else
  #define SCP_SERIAL Serial1
#endif

#if defined(CONFIG_IDF_TARGET_ESP32H2)
  static const int PIN_RX = 4, PIN_TX = 5;     // CHECK against your board's pinout
#else
  static const int PIN_RX = 16, PIN_TX = 17;   // WROOM-32 UART2 defaults
#endif

HardwareSerial &scp = SCP_SERIAL;

// ---- simulated amp state -------------------------------------------------
uint8_t master = 0x12, sub = 0x17, input = 0x00;
bool writeEnabled = false;          // register 01. Volatile across a power-cycle.
bool verbose = true, deaf = false;

uint32_t nReq = 0, nBad = 0, nRejected = 0;

// Captured register 03 reply, verbatim from the spec's Appendix A. 78 bytes.
// Only needed if the controller ever performs the full boot handshake.
static const uint8_t REG03[78] = {
  0x02,0x00,0x88,0x13,0xB8,0x0B,0x40,0x1F,0x01,0xB2,0x59,0x00,0x00,0x01,0x00,0x00,
  0x00,0xFF,0xFF,0xFF,0x3C,0x01,0x58,0x02,0xA8,0xFD,0x01,0xFF,0x00,0x00,0x18,0x01,
  0x78,0x00,0xA8,0xFD,0xFF,0x00,0x00,0x00,0x3C,0x01,0x58,0x02,0xA8,0xFD,0xFF,0x00,
  0x00,0x00,0x3C,0x01,0x58,0x02,0xA8,0xFD,0x00,0x01,0x3C,0x00,0xFF,0x00,0x18,0x01,
  0x78,0x00,0x96,0x00,0xFF,0xC8,0x0A,0x00,0x18,0x01,0x78,0x00,0x88,0x13
};

void logBytes(const char *tag, const uint8_t *b, size_t n) {
  if (!verbose) return;
  Serial.print(tag);
  for (size_t k = 0; k < n; k++) Serial.printf(" %02X", b[k]);
  Serial.print("\r\n");
}

// ---- replying ------------------------------------------------------------
// reply: 43 LEN ~LEN 01 CMD REG data... CS, where LEN = 2 + ndata. That is one
// less than the span it covers, which is the asymmetry the spec records as
// "the reply carries one byte its LEN does not count".
void reply(uint8_t cmd, uint8_t reg, const uint8_t *data, size_t n) {
  if (deaf) { if (verbose) Serial.print("   (deaf: not answering)\r\n"); return; }

  uint8_t out[96];
  size_t i = 0;
  uint8_t len = (uint8_t)(2 + n), cs = cmd + reg;
  out[i++] = 0x43; out[i++] = len; out[i++] = (uint8_t)~len;
  out[i++] = 0x01; out[i++] = cmd; out[i++] = reg;
  for (size_t k = 0; k < n; k++) { out[i++] = data[k]; cs += data[k]; }
  out[i++] = cs;

  delayMicroseconds(200);           // the real amp answers in 54-467 us
  scp.write(out, i);
  scp.flush();
  logBytes("  TX", out, i);
}

void showState() {
  Serial.printf("state: master=%u (0x%02X)  sub=%u (0x%02X)  input=%u  writes=%s%s\r\n",
                master, master, sub, sub, input,
                writeEnabled ? "ENABLED" : "disabled", deaf ? "  [DEAF]" : "");
  Serial.printf("seen: %lu requests, %lu malformed, %lu writes refused\r\n",
                (unsigned long)nReq, (unsigned long)nBad, (unsigned long)nRejected);
}

// ---- request handling ----------------------------------------------------
void onRead(uint8_t reg) {
  uint8_t d4[4], d11[11], d8[8], one;
  switch (reg) {
    case 0x00: { uint8_t d[2] = {0x01, 0x03}; reply(0x2A, reg, d, 2); break; }
    case 0x03:   reply(0x2A, reg, REG03, sizeof REG03); break;
    case 0x04:   d4[0] = master; d4[1] = sub; d4[2] = 0; d4[3] = 0;
                 reply(0x2A, reg, d4, 4); break;
    case 0x05:   one = 0x00; reply(0x2A, reg, &one, 1); break;
    case 0x06:   memset(d11, 0, sizeof d11); d11[1] = 0x01;
                 reply(0x2A, reg, d11, sizeof d11); break;
    case 0x07:   d4[0] = input; d4[1] = 0x03; d4[2] = 0x01; d4[3] = 0x00;
                 reply(0x2A, reg, d4, 4); break;
    case 0x09: { const uint8_t d[8] = {0,0,0,0x96,0x01,0,0x13,0x88};
                 memcpy(d8, d, 8); reply(0x2A, reg, d8, 8); break; }
    default:
      // What the real amp does with an unknown register was never captured, so
      // stay silent rather than invent a behaviour the controller might rely on.
      Serial.printf("  read of unknown register %02X: staying silent (real amp behaviour unknown)\r\n", reg);
  }
}

void onWrite(uint8_t reg, const uint8_t *d, size_t n) {
  if (reg == 0x01) {                       // the gate itself is always writable
    writeEnabled = (n >= 1 && d[0] == 0x01);
    Serial.printf("  writes %s\r\n", writeEnabled ? "ENABLED" : "disabled");
    reply(0x2B, 0x01, d, n);
    return;
  }
  if (!writeEnabled) {                     // name the cause, do not echo
    uint8_t zero = 0x00;
    nRejected++;
    Serial.print("  refusing write: register 01 is 00\r\n");
    reply(0x2B, 0x01, &zero, 1);
    return;
  }
  if (reg == 0x04 && n >= 2) {
    if (d[0]) sub = d[1]; else master = d[1];
    Serial.printf("  %s = %u (0x%02X)\r\n", d[0] ? "sub" : "master", d[1], d[1]);
  } else if (reg == 0x07 && n >= 1) {
    input = d[0];
    Serial.printf("  input = %u\r\n", input);
  } else {
    Serial.printf("  write to register %02X accepted but not modelled\r\n", reg);
  }
  reply(0x2B, reg, d, n);                  // the amp echoes an accepted write
}

// Reads one request at a time. Every rejection says why, which is the point of
// having a simulator: a controller that breaks its framing gets told how.
void pump() {
  static uint8_t buf[96];
  static size_t have = 0, want = 0;

  while (scp.available()) {
    uint8_t b = scp.read();

    if (have == 0) { if (b != 0x42) continue; buf[have++] = b; continue; }  // resync on SOF
    buf[have++] = b;

    if (have == 3) {
      if ((uint8_t)(buf[1] + buf[2]) != 0xFF) {
        nBad++;
        Serial.printf("REJECT: LEN %02X and ~LEN %02X do not sum to FF\r\n", buf[1], buf[2]);
        have = 0; continue;
      }
      want = (size_t)buf[1] + 4;           // SOF LEN ~LEN + LEN bytes + CS
      if (want > sizeof buf) { nBad++; Serial.printf("REJECT: LEN %02X too long\r\n", buf[1]); have = 0; }
      continue;
    }
    if (have < 3 || have < want) continue;

    logBytes("RX", buf, have);
    nReq++;

    uint8_t cs = 0;
    for (size_t k = 4; k + 1 < want; k++) cs += buf[k];   // CMD .. last data byte
    if (cs != buf[want - 1]) {
      nBad++;
      Serial.printf("REJECT: checksum %02X, expected %02X\r\n", buf[want - 1], cs);
      have = 0; continue;
    }
    if (buf[3] != 0x01) {
      nBad++;
      Serial.printf("REJECT: byte after ~LEN is %02X, every captured frame has 01\r\n", buf[3]);
      have = 0; continue;
    }

    const uint8_t cmd = buf[4], reg = buf[5];
    const size_t n = (size_t)buf[1] - 3;
    if      (cmd == 0x2A) onRead(reg);
    else if (cmd == 0x2B) onWrite(reg, buf + 6, n);
    else { nBad++; Serial.printf("REJECT: command %02X is neither 2A nor 2B\r\n", cmd); }

    have = 0;
  }
}

// ---- console -------------------------------------------------------------
void handle(String line) {
  line.trim(); if (!line.length()) return;
  char c = line[0];
  String arg = line.substring(1); arg.trim();
  long v = 0;
  bool numOk = false;
  if (arg.length()) {
    bool hex = arg.startsWith("0x") || arg.startsWith("0X");
    char *end = nullptr;
    v = strtol(arg.c_str(), &end, hex ? 16 : 10);
    numOk = (end != arg.c_str() && *end == '\0');
  }
  switch (c) {
    case 'm': if (numOk) { master = (uint8_t)v; showState(); } break;
    case 's': if (numOk) { sub    = (uint8_t)v; showState(); } break;
    case 'i': if (numOk) { input  = (uint8_t)v; showState(); } break;
    case 'p':
      writeEnabled = false;                // exactly what a real power-cycle does
      Serial.print("simulated power-cycle: write enable dropped\r\n");
      showState(); break;
    case 'x': deaf = !deaf;   Serial.printf("deaf mode %s\r\n", deaf ? "ON" : "off"); break;
    case 'v': verbose = !verbose; Serial.printf("verbose %s\r\n", verbose ? "on" : "off"); break;
    case '?':
      Serial.print("m N / s N / i N  preset master, sub, input\r\n"
                   "p  power-cycle   x  deaf mode   v  verbose\r\n");
      showState(); break;
    default: break;
  }
}

// ---- main ----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);   // native-USB parts need this
  delay(200);
  scp.begin(230400, SERIAL_8N1, PIN_RX, PIN_TX);
  Serial.print("\r\nSCP amp simulator ready. '?' for help and state.\r\n");
  showState();
}

void loop() {
  pump();

  static String buf;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r' || ch == '\n') { handle(buf); buf = ""; }
    else if (ch >= 0x20 && ch < 0x7F) { if (buf.length() < 16) buf += ch; }
    else buf = "";
  }
}
