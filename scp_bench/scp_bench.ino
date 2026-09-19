// SCP bench controller v3: ESP32 takes the place of the CONDUCTOR knob.
// Protocol per conductor-scp-protocol.md (UART 230400 8N1, 3.3 V, idle high).
//
// Wiring (knob UNPLUGGED, ESP32 on the amp-side half of the Micro-Fit).
// Breakout connector seen from the AMP SIDE, latching tab at the top:
//
//        latching tab            ARX = amp RX  (knob -> amp, old analyser D2)
//   +-------+-------+            ATX = amp TX  (amp -> knob, old analyser D0)
//   |  ARX  |  ATX  |
//   +-------+-------+            ARX  <- 1k <- TX2 (GPIO17)
//   |  GND  |  3V3  |            ATX  -> 1k -> RX2 (GPIO16)
//   +-------+-------+            GND  -> ESP32 GND
//                                3V3  -> leave open
//
// Console, identical over USB serial (115200) and Bluetooth LE. Both are live at
// once, so you can drive it from the Mac while watching the frame trace on USB.
// Numbers are DECIMAL unless written 0x.. :
//   r        read levels          m N   master level       s N   sub level
//   i N      input (0 main, 1 optical, 2 ext)              + / - master step
//   e        send write-enable    d     BLE diagnostics      ?     help
//   v 0/1    frame tracing off/on (an app sends 'v 0'; default on for USB work)
//
// BLE: Nordic UART Service, advertised as "SCP-xxxx" where xxxx is derived from
// this board's MAC, so several boards are distinguishable. One central at a time;
// any phone may take the link once the previous one drops. Open, no pairing.
// Drive it with ble_echo/echo_test.py, or any NUS terminal (nRF Connect on Android).
//
// PIN_RX/PIN_TX below are for a WROOM-32. On an S3/C3/C6 pick free pins; the UART
// matrix will route Serial2 anywhere, and the BLE code here is variant-independent.
//
// BEFORE UPLOADING: Tools -> Partition Scheme -> "Huge APP (3MB No OTA)".
// The BLE stack is ~1.3 MB; the default scheme gives the app only 1.2 MB.
//
// v5: 'v' verbosity toggle. Each command otherwise answers with TX/RX/OK, which is
// ~4 BLE notifications; a slider dragging at 10 Hz would flood the link and the
// 8-deep command queue. An app sends 'v 0' on connect and gets one line per command.
// v4: BLE hardening for phone use. Unique name, always returns to advertising
// (re-armed on disconnect AND by a periodic health check), connection statistics
// via 'd' so a soak test produces numbers rather than impressions.
// v3: BLE console alongside USB. v2: write-enable handling, separate master/sub
// ceilings, strict numeric parsing. See git history for the bench results behind each.

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#if __has_include(<BLE2902.h>)
  #include <BLE2902.h>              // arduino-esp32 2.x needs the CCCD added by hand
  #define HAS_BLE2902 1             // 3.x adds it automatically and drops this header
#endif

static const int PIN_RX = 16, PIN_TX = 17;
// Separate ceilings. Each is the highest value ever observed for that target, so the
// sub cannot be driven to a master-sized number. Raise only after capturing the real
// end stops (spec section 8).
static const uint8_t MAX_MASTER = 0x29;  // 41, from the master volume captures
static const uint8_t MAX_SUB    = 0x17;  // 23, from the sub level captures

HardwareSerial &scp = Serial2;
uint8_t master = 0, sub = 0;
bool verbose = true;                 // frame tracing; 'v 0' quiets it for app use

// ---- BLE transport -------------------------------------------------------
// Nordic UART Service. Names are from the PERIPHERAL's point of view: this
// device RECEIVES on RX and NOTIFIES on TX. Centrals have it the other way round.
#define NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"   // central writes here
#define NUS_TX      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"   // we notify here

char deviceName[16] = "SCP-????";     // filled from the MAC in setup()
static const size_t CHUNK    = 20;    // safe notify payload at the default 23-byte MTU
static const size_t MAX_LINE = 32;    // console commands are short

struct Line { char text[MAX_LINE]; };

BLECharacteristic *txChar = nullptr;
QueueHandle_t      lineQ  = nullptr;
volatile bool      bleConnected  = false;
volatile bool      advertising   = false;
volatile bool      needAdvertise = false;

// Connection statistics. A soak test is only worth running if it produces numbers,
// so every connect and drop is counted and timed; 'd' prints them.
uint32_t bleConnects = 0, bleDisconnects = 0;
uint32_t connectedAtMs = 0, lastSessionMs = 0, longestSessionMs = 0;

// Tees console output to USB serial and, when a central is attached, to a BLE
// notify. Buffers a whole line and sends it as chunks: one notify per byte would
// turn a frame trace into hundreds of packets at a ~7.5 ms connection interval.
class Console : public Print {
  char   buf[320];                    // long enough for a full register 03 dump line
  size_t len = 0;
public:
  using Print::write;
  size_t write(uint8_t b) override {
    Serial.write(b);
    if (len < sizeof buf - 1) buf[len++] = (char)b;
    if (b == '\n' || len >= sizeof buf - 1) flushLine();
    return 1;
  }
  void flushLine() {
    if (len && bleConnected && txChar) {
      for (size_t off = 0; off < len; off += CHUNK) {
        size_t take = len - off < CHUNK ? len - off : CHUNK;
        txChar->setValue((uint8_t *)(buf + off), take);
        txChar->notify();
        delay(4);                     // let the stack drain; back-to-back notifies drop
      }
    }
    len = 0;
  }
};
Console io;

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    bleConnected = true;
    advertising  = false;            // the stack stops advertising on connect
    bleConnects++;
    connectedAtMs = millis();
    Serial.print("BLE connected\r\n");
  }
  void onDisconnect(BLEServer *) override {
    bleConnected = false;
    bleDisconnects++;
    lastSessionMs = millis() - connectedAtMs;
    if (lastSessionMs > longestSessionMs) longestSessionMs = lastSessionMs;
    needAdvertise = true;            // loop() re-arms it; doing it here is flaky
    Serial.print("BLE disconnected\r\n");
  }
};

// A BLE callback must never do slow work: readReply() blocks for up to 50 ms, and
// blocking in BLE context starves the stack and drops the link. So the callback only
// assembles a line and queues it; loop() runs the command.
class RxCB : public BLECharacteristicCallbacks {
  char   buf[MAX_LINE];               // touched only in this task, so no locking
  size_t len = 0;

  void onWrite(BLECharacteristic *c) override {
    uint8_t *data = c->getData();     // getData/getLength are stable across core
    size_t n = c->getLength();        // versions; getValue()'s type is not
    for (size_t i = 0; i < n; i++) {
      char ch = (char)data[i];
      if (ch == '\r' || ch == '\n')          push();
      else if (ch >= 0x20 && ch < 0x7F)    { if (len < MAX_LINE - 1) buf[len++] = ch; }
      else                                   len = 0;   // non-printable: drop the line
    }
    if (len) push();                  // a write with no newline is still a command
  }

  void push() {
    if (!len) return;
    buf[len] = '\0';
    Line l;
    strncpy(l.text, buf, MAX_LINE);
    l.text[MAX_LINE - 1] = '\0';
    xQueueSend(lineQ, &l, 0);         // never block inside a BLE callback
    len = 0;
  }
};

void beginAdvertising() {
  BLEDevice::startAdvertising();
  advertising = true;
}

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
  io.print(tag); for (size_t k = 0; k < n; k++) io.printf(" %02X", b[k]); io.print("\r\n");
}

size_t transact(uint8_t cmd, uint8_t reg, const uint8_t *data, uint8_t n, uint8_t *body, size_t cap) {
  uint8_t req[32]; size_t len = buildRequest(req, cmd, reg, data, n);
  while (scp.available()) scp.read();          // flush stale bytes
  scp.write(req, len); scp.flush();
  size_t r = readReply(body, cap);
  // Printing happens AFTER the exchange, not between request and reply. A BLE notify
  // costs several ms; the amp answers in well under one. Printing first would leave
  // the reply sitting in the UART FIFO while we block. Console output order is
  // unchanged: still TX then RX.
  if (verbose) {
    dump("TX", req, len);
    if (r) dump("RX", body, r);
  }
  if (!r) io.print("RX: no valid reply\r\n");   // failures are reported either way
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
      io.print("amp says writes not enabled: enabling and retrying\r\n");
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
  io.printf("master=%u (0x%02X)  sub=%u (0x%02X)\r\n", master, master, sub, sub);
  return true;
}

bool setLevel(uint8_t target, long v) {           // target 0 = master, 1 = sub
  const char *name = target ? "sub" : "master";
  uint8_t limit = target ? MAX_SUB : MAX_MASTER;
  if (v < 0 || v > limit) {
    io.printf("REFUSED: %s %ld is outside 0..%u\r\n", name, v, limit);
    return false;
  }
  if (!writeReg(0x04, target, (uint8_t)v, 0x01)) return false;
  (target ? sub : master) = (uint8_t)v; return true;
}

bool setInput(long idx) {
  if (idx < 0 || idx > 2) { io.print("REFUSED: input must be 0, 1 or 2\r\n"); return false; }
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

void printDuration(uint32_t ms) {
  uint32_t s = ms / 1000;
  io.printf("%02u:%02u:%02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
}

// Everything a soak test needs: is it reachable, how often has the link dropped,
// and how long does it survive. A phone that walks out of range is not reported
// until the supervision timeout expires, so a drop can lag the event by seconds.
void showDiag() {
  uint32_t now = millis();
  io.printf("name %s   connected %s   advertising %s\r\n",
            deviceName, bleConnected ? "yes" : "no", advertising ? "yes" : "no");
  io.print("uptime ");  printDuration(now);
  io.printf("   connects %lu   drops %lu\r\n",
            (unsigned long)bleConnects, (unsigned long)bleDisconnects);
  io.print("session "); printDuration(bleConnected ? now - connectedAtMs : lastSessionMs);
  io.print("   longest "); printDuration(longestSessionMs);
  io.printf("   heap %lu\r\n", (unsigned long)ESP.getFreeHeap());
}

void badNum() { io.print("REFUSED: need a number, e.g. 'm 30' or 'm 0x1E'\r\n"); }

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
    case 'd': showDiag(); return;
    case 'v': if (!numOk) { badNum(); break; } verbose = (v != 0); ok = true; break;
    case 'm': if (!numOk) { badNum(); break; } ok = setLevel(0, v); break;
    case 's': if (!numOk) { badNum(); break; } ok = setLevel(1, v); break;
    case 'i': if (!numOk) { badNum(); break; } ok = setInput(v); break;
    case '+': case '-':
      if (!readLevels()) break;                    // always step from the amp's truth
      ok = setLevel(0, (long)master + (c == '+' ? 1 : -1)); break;
    case '?':
      // The app parses the second line to learn the ceilings, so it stays in step
      // with this firmware instead of hardcoding them.
      io.printf("r | e | m N | s N | i N | + | - | d | v 0/1  (N decimal, or 0x.. hex)\r\n"
                "master 0..%u, sub 0..%u, input 0..2\r\n",
                MAX_MASTER, MAX_SUB);
      return;
    default: return;                               // ignore line noise
  }
  io.print(ok ? "OK\r\n" : "FAILED\r\n");
}

// ---- main ----------------------------------------------------------------
void startBle() {
  lineQ = xQueueCreate(8, sizeof(Line));

  // Name the board after its own MAC so several units are distinguishable and the
  // name survives reflashing. Folding all 48 bits means whichever bytes actually
  // vary between boards, the tag varies with them.
  uint64_t mac = ESP.getEfuseMac();
  uint16_t tag = (uint16_t)(mac >> 32) ^ (uint16_t)(mac >> 16) ^ (uint16_t)mac;
  snprintf(deviceName, sizeof deviceName, "SCP-%04X", tag);

  BLEDevice::init(deviceName);
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());

  BLEService *svc = server->createService(NUS_SERVICE);

  txChar = svc->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
#ifdef HAS_BLE2902
  txChar->addDescriptor(new BLE2902());
#endif

  BLECharacteristic *rxChar = svc->createCharacteristic(
      NUS_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rxChar->setCallbacks(new RxCB());

  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE);
  adv->setScanResponse(true);
  beginAdvertising();
}

void setup() {
  Serial.begin(115200);
  scp.begin(230400, SERIAL_8N1, PIN_RX, PIN_TX);
  delay(500);
  startBle();
  Serial.printf("\r\nSCP bench v5 ready, advertising as \"%s\". '?' for help.\r\n", deviceName);
}

void loop() {
  // 1. commands that arrived over BLE
  Line l;
  while (xQueueReceive(lineQ, &l, 0) == pdTRUE) {
    Serial.printf("[ble] %s\r\n", l.text);         // trace on USB only, not echoed back
    handle(String(l.text));
  }

  // 2. commands typed on USB serial
  static String buf;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r' || ch == '\n') { handle(buf); buf = ""; }
    else if (ch >= 0x20 && ch < 0x7F) { if (buf.length() < 16) buf += ch; }
    else buf = "";                                 // non-printable: drop the line
  }

  // 3. restart advertising after a disconnect
  if (needAdvertise) {
    needAdvertise = false;
    delay(200);                                    // let the stack tear the link down
    beginAdvertising();
    Serial.print("re-advertising\r\n");
  }

  // 4. belt and braces: if we are neither connected nor advertising, nobody can
  //    ever reach us again. Cheap to check, and it recovers from a missed or
  //    failed re-arm without needing to know why it happened.
  static uint32_t lastAdvCheck = 0;
  if (millis() - lastAdvCheck > 2000) {
    lastAdvCheck = millis();
    if (!bleConnected && !advertising) {
      beginAdvertising();
      Serial.print("advertising restarted by health check\r\n");
    }
  }

  delay(5);                                        // yield to the BLE task
}
