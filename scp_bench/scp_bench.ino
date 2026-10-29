// SCP bench controller v3: ESP32 takes the place of the CONDUCTOR knob.
// Protocol per conductor-scp-protocol.md (UART 230400 8N1, 3.3 V, idle high).
//
// Wiring (knob UNPLUGGED, ESP32 on the amp-side half of the Micro-Fit).
// Breakout connector seen from the AMP SIDE, latching tab at the top:
//
//        latching tab            ARX = amp RX  (knob -> amp, old analyser D2)
//   +-------+-------+            ATX = amp TX  (amp -> knob, old analyser D0)
//   |  ARX  |  ATX  |
//   +-------+-------+            ARX  <- 1k <- PIN_TX
//   |  GND  |  3V3  |            ATX  -> 1k -> PIN_RX
//   +-------+-------+            GND  -> ESP32 GND
//                                3V3  -> leave open
//   (WROOM-32: PIN_RX = 16, PIN_TX = 17.  ESP32-H2: see the pin block below.)
//
// Console, identical over USB serial (115200) and Bluetooth LE. Both are live at
// once, so you can drive it from the Mac while watching the frame trace on USB.
// Numbers are DECIMAL unless written 0x.. :
//   r        read levels          m N   master level       s N   sub level
//   i N      input (0 main, 1 optical, 2 ext)              + / - master step
//   e        send write-enable    d     diagnostics          ?     help
//   h        replay the knob's power-up handshake (reads + enable)
//   g N      read register N and print its data bytes
//   t N      set the log clock: N = Unix time, sent by an app on connect
//   log      print the event log between LOG BEGIN / LOG END lines
//   log clear  erase the event log
//   v 0/1    frame tracing off/on (an app sends 'v 0'; default on for USB work)
//
// BLE: Nordic UART Service, advertised as "SCP-xxxx" where xxxx is derived from
// this board's MAC, so several boards are distinguishable. One central at a time;
// any phone may take the link once the previous one drops. Open, no pairing.
// Drive it with ble_echo/echo_test.py, or any NUS terminal (nRF Connect on Android).
//
// Board support: see the UART/pin block below. The BLE code is variant-independent.
// On a USB-native part (H2, C3, C6, S3) set Tools -> "USB CDC On Boot: Enabled",
// or the console goes to the UART0 pins instead of the USB cable and the Serial
// Monitor stays empty.
//
// BEFORE UPLOADING: Tools -> Partition Scheme -> "Huge APP (3MB No OTA)".
// The BLE stack is ~1.3 MB; the default scheme gives the app only 1.2 MB.
//
// v8.2: reports its own version, in '?' and 'd', so an app can show which
// firmware it is talking to. 28 Oct 2026: the bench board was still on v8 and
// nobody could tell.
// v8.1: v8 pulled RX down to read the amp's power state from its TX line, and the
// amp then went completely silent (28 Oct 2026 log: not one byte in four boots,
// the line never read high). The amp evidently does not drive its TX line high
// itself; the knob, and the RX pull-up v7 inherited from the UART driver, do. So
// RX is pulled up again, and "the amp came up" is now judged by the amp starting
// to answer after silence, which then triggers the handshake.
// v8: behaves like the knob at amp power-up. The amp's TX line is watched; once
// it has been high for a second (the knob's own delay, spec section 5) the full
// handshake is replayed: reads of 00, 03, 04, 05, 06, 07, 09, then the enable. A
// refused write also triggers it. Unexpected amp behaviour is kept in an event
// log in flash ('log'), which an app can download.
// v7: builds for ESP32-H2 and other two-UART parts, which have no Serial2.
// v6: advertise the name in the primary packet and the 128-bit service UUID in the
// scan response, so neither packet can overflow the 31-byte advertising limit.
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
#include <LittleFS.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <stdarg.h>
#include <time.h>
#if __has_include(<BLE2902.h>)
  #include <BLE2902.h>              // arduino-esp32 2.x needs the CCCD added by hand
  #define HAS_BLE2902 1             // 3.x adds it automatically and drops this header
#endif

// ---- board differences ----------------------------------------------------
// Only the original ESP32 and the S3 have three UARTs, so only they have Serial2.
// The H2, C3, C6 and S2 have two, where the spare one is Serial1. Referring to
// Serial2 on those parts is a compile error, not a run-time surprise.
#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S3)
  #define SCP_SERIAL Serial2
#else
  #define SCP_SERIAL Serial1
#endif

#if defined(CONFIG_IDF_TARGET_ESP32H2)
  // ESP32-H2-Zero. CHECK THESE AGAINST YOUR BOARD'S PINOUT before wiring: avoid
  // the strapping pins (GPIO8, GPIO9) and anything taken by flash or USB.
  static const int PIN_RX = 4, PIN_TX = 5;
#else
  // WROOM-32, the UART2 defaults. Not usable on a WROVER, where GPIO16/17 are PSRAM.
  static const int PIN_RX = 16, PIN_TX = 17;
#endif
// Separate ceilings, so the sub cannot be driven to a master-sized number.
// MAX_MASTER is the real end stop: the knob was turned to maximum and register 04
// read back 0x3C (spec section 4). MAX_SUB is still only the highest value ever
// observed, not a measured limit, so it stays conservative until captured the same way.
static const uint8_t MAX_MASTER = 0x3C;  // 60, the knob's own maximum
static const uint8_t MAX_SUB    = 0x17;  // 23, highest seen - not a known limit

// Shown by '?' (on its own line, for apps to parse), 'd', the boot log line and the
// USB banner. Bump it with every change that is flashed.
static const char *FW_VERSION = "8.2";

HardwareSerial &scp = SCP_SERIAL;
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

// ---- event log -------------------------------------------------------------
// Anything unexpected from the amp, plus amp power and handshake events, goes to
// a text file in flash, so it survives a power cut and can be fetched later over
// BLE with 'log'. Normal traffic is NOT logged: the point is to make the rare odd
// event findable, not to record every volume step. Two files of LOG_MAX each:
// when the current one fills, it replaces the old one.
//
// Lines start with '#' and never contain "master=", so an app parsing the
// console for levels cannot mistake a log line for a reading.
static const char  *LOG_PATH = "/log.txt";
static const char  *LOG_OLD  = "/log.old";
static const size_t LOG_MAX  = 64 * 1024;     // x2 files, about a minute over BLE
static const size_t MSG_MAX  = 320;

Preferences prefs;
bool     logReady  = false;
uint32_t bootCount = 0;
uint32_t syncEpoch = 0, syncMs = 0;           // wall clock from the phone ('t N')
char     lastMsg[MSG_MAX] = "";
uint32_t lastMsgMs = 0, repeats = 0;

// "#12 00:03:21.456", plus " 2026-10-28T12:00:01Z" once a phone has set the clock.
// The boot number tells events from different power-ups apart; without a clock
// that, and the uptime, is all there is.
void stamp(char *out, size_t cap) {
  uint32_t ms = millis(), s = ms / 1000;
  int n = snprintf(out, cap, "#%lu %02lu:%02lu:%02lu.%03lu", (unsigned long)bootCount,
                   (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
                   (unsigned long)(s % 60), (unsigned long)(ms % 1000));
  if (syncEpoch && n > 0 && (size_t)n < cap) {
    time_t t = (time_t)syncEpoch + (time_t)((ms - syncMs) / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out + n, cap - n, " %Y-%m-%dT%H:%M:%SZ", &tm);
  }
}

size_t fileSize(const char *path) {
  if (!logReady || !LittleFS.exists(path)) return 0;
  File f = LittleFS.open(path, FILE_READ);
  size_t n = f ? f.size() : 0;
  if (f) f.close();
  return n;
}

void appendLog(const char *msg) {
  char ts[48];
  stamp(ts, sizeof ts);
  Serial.printf("[log] %s %s\r\n", ts, msg);      // USB only, never over BLE
  if (!logReady) return;
  if (fileSize(LOG_PATH) > LOG_MAX) {
    LittleFS.remove(LOG_OLD);
    LittleFS.rename(LOG_PATH, LOG_OLD);
  }
  File f = LittleFS.open(LOG_PATH, FILE_APPEND);
  if (!f) return;
  f.printf("%s %s\n", ts, msg);
  f.close();                                      // closed per line: a power cut loses nothing
}

void flushRepeats() {
  if (!repeats) return;
  char m[64];
  snprintf(m, sizeof m, "(previous line repeated %lu more times)", (unsigned long)repeats);
  repeats = 0;
  appendLog(m);
}

void logEvent(const char *fmt, ...) {
  char msg[MSG_MAX];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  // A fault that repeats - an amp that stays silent while the app keeps asking -
  // becomes one line and a count, not a flood that pushes everything else out.
  uint32_t now = millis();
  if (!strcmp(msg, lastMsg) && now - lastMsgMs < 60000) {
    repeats++;
    lastMsgMs = now;
    return;
  }
  flushRepeats();
  strncpy(lastMsg, msg, sizeof lastMsg - 1);
  lastMsgMs = now;
  appendLog(msg);
}

// "AA BB CC", truncated with ".." if it will not fit.
const char *toHex(char *out, size_t cap, const uint8_t *b, size_t n) {
  size_t at = 0;
  out[0] = '\0';
  for (size_t k = 0; k < n; k++) {
    if (at + 4 >= cap) { if (at + 3 < cap) strcpy(out + at, ".."); break; }
    at += snprintf(out + at, cap - at, k ? " %02X" : "%02X", b[k]);
  }
  return out;
}

const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout (supply dipped)";
    case ESP_RST_EXT:       return "reset pin";
    default:                return "other";
  }
}

void dumpLog() {
  flushRepeats();
  io.printf("LOG BEGIN %u\r\n", (unsigned)(fileSize(LOG_OLD) + fileSize(LOG_PATH)));
  const char *files[2] = {LOG_OLD, LOG_PATH};     // oldest first
  for (const char *path : files) {
    if (!fileSize(path)) continue;
    File f = LittleFS.open(path, FILE_READ);
    if (!f) continue;
    char buf[MSG_MAX + 64];
    size_t n = 0;
    while (f.available()) {
      char c = (char)f.read();
      if (c == '\n' || n == sizeof buf - 1) {
        buf[n] = '\0';
        io.print(buf);
        io.print("\r\n");
        n = 0;
        if (c == '\n') continue;
      }
      if (c != '\n') buf[n++] = c;
    }
    if (n) { buf[n] = '\0'; io.print(buf); io.print("\r\n"); }
    f.close();
  }
  io.print("LOG END\r\n");
}

void clearLog() {
  if (logReady) { LittleFS.remove(LOG_OLD); LittleFS.remove(LOG_PATH); }
  repeats = 0;
  lastMsg[0] = '\0';
  logEvent("log cleared");
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

// ---- is the amp there? ---------------------------------------------------
// The line's idle level cannot say: it needs a pull-up to read high at all (see
// v8.1 above), so it reads high with the amp off too. What can say is whether the
// amp answers. The first answer after silence counts as the amp having come up,
// and the knob's handshake follows from loop(), once the current command is done.
// Three unanswered requests in a row count as the amp having gone.
static const int SILENT_LIMIT = 3;
bool ampAnswering = false;
bool handshakeDue = false;
int  silentInARow = 0;

void noteAnswer() {
  silentInARow = 0;
  if (ampAnswering) return;
  ampAnswering = true;
  handshakeDue = true;
  logEvent("amp answering");
  io.print("amp on\r\n");
}

void noteSilence() {
  if (++silentInARow < SILENT_LIMIT || !ampAnswering) return;
  ampAnswering = false;
  logEvent("amp silent: %d requests in a row unanswered", SILENT_LIMIT);
  io.print("amp off\r\n");
}

// Every byte readReply() saw, and why it gave up, so a failure can be logged with
// the evidence rather than just "no reply".
uint8_t     rxRaw[112];
size_t      rxRawLen = 0;
const char *rxErr    = "";

int scpRead() {                                   // read one byte and keep a copy
  int b = scp.read();
  if (b >= 0 && rxRawLen < sizeof rxRaw) rxRaw[rxRawLen++] = (uint8_t)b;
  return b;
}

// reply: 43 LEN ~LEN then LEN+2 bytes (01 CMD REG data... CS). Returns body length, 0 on failure.
size_t readReply(uint8_t *body, size_t cap, uint32_t timeoutMs = 50) {
  uint8_t hdr[3]; size_t got = 0; uint32_t t0 = millis();
  rxRawLen = 0;
  while (got < 3 && millis() - t0 < timeoutMs)
    if (scp.available()) { uint8_t b = scpRead(); if (got || b == 0x43) hdr[got++] = b; }
  if (got < 3) { rxErr = rxRawLen ? "no 43 header" : "silence"; return 0; }
  if ((uint8_t)(hdr[1] + hdr[2]) != 0xFF) { rxErr = "LEN and ~LEN disagree"; return 0; }
  size_t need = hdr[1] + 2; if (need > cap) { rxErr = "longer than expected"; return 0; }
  got = 0;
  while (got < need && millis() - t0 < timeoutMs) if (scp.available()) body[got++] = scpRead();
  if (got < need) { rxErr = "cut short"; return 0; }
  uint8_t cs = 0; for (size_t k = 1; k + 1 < need; k++) cs += body[k];
  if (cs != body[need - 1]) { rxErr = "bad checksum"; return 0; }
  return need;
}

void dump(const char *tag, const uint8_t *b, size_t n) {
  io.print(tag); for (size_t k = 0; k < n; k++) io.printf(" %02X", b[k]); io.print("\r\n");
}

size_t transact(uint8_t cmd, uint8_t reg, const uint8_t *data, uint8_t n, uint8_t *body, size_t cap) {
  uint8_t req[32]; size_t len = buildRequest(req, cmd, reg, data, n);
  // Anything already waiting was sent by the amp unasked. The spec says it never
  // does that, so these bytes used to be thrown away; now they are evidence.
  if (scp.available()) {
    uint8_t stale[64]; size_t k = 0; char h[200];
    while (scp.available()) { int b = scp.read(); if (k < sizeof stale) stale[k++] = (uint8_t)b; }
    logEvent("unsolicited bytes before a request: %s", toHex(h, sizeof h, stale, k));
  }
  scp.write(req, len); scp.flush();
  size_t r = readReply(body, cap);
  if (!r) {
    char hq[100], hr[340];
    logEvent("no valid reply (%s) to %s, received: %s", rxErr,
             toHex(hq, sizeof hq, req, len), rxRawLen ? toHex(hr, sizeof hr, rxRaw, rxRawLen) : "nothing");
  }
  // After the failure itself is logged, so the log reads cause, then conclusion.
  if (r) noteAnswer();
  else if (!rxRawLen) noteSilence();              // garbage still means something is there
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

bool handshake(const char *why);

bool writeReg(uint8_t reg, uint8_t a, uint8_t b, uint8_t c) {
  uint8_t d[3] = {a, b, c}, body[16];
  for (int attempt = 0; attempt < 2; attempt++) {
    size_t r = transact(0x2B, reg, d, 3, body, sizeof body);
    if (r >= 7 && body[1] == 0x2B && body[2] == reg && body[3] == a && body[4] == b && body[5] == c) return true;
    if (attempt == 0 && isRejection(body, r)) {
      // The amp forgets the enable when it restarts, so a refusal means it has
      // restarted without us noticing. Greet it the way the knob would.
      io.print("amp says writes not enabled: handshaking and retrying\r\n");
      logEvent("write to %02X refused: writes not enabled (amp restarted?)", reg);
      if (!handshake("a write was refused")) return false;
    } else {
      if (r) {
        char h[100];
        logEvent("write %02X %02X %02X %02X: unexpected answer %s", reg, a, b, c, toHex(h, sizeof h, body, r));
      }
      return false;
    }
  }
  return false;
}

bool readLevels() {
  uint8_t body[16];
  size_t r = transact(0x2A, 0x04, nullptr, 0, body, sizeof body);
  if (r < 6 || body[1] != 0x2A || body[2] != 0x04) {
    if (r) { char h[100]; logEvent("level read: unexpected answer %s", toHex(h, sizeof h, body, r)); }
    return false;
  }
  master = body[3]; sub = body[4];
  // The two bytes after the levels have always been 00 00; anything else is new.
  if (r >= 8 && (body[5] || body[6]))
    logEvent("level read: bytes after the levels are %02X %02X, always 00 00 before", body[5], body[6]);
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

// ---- the knob's power-up handshake ---------------------------------------
// Spec section 5, byte for byte and in the knob's order: seven reads, then the
// write enable, with no pauses between them. The reads are not needed for
// control, but they are what the real CONDUCTOR does every time the amp starts,
// and the amp may treat a remote that has introduced itself this way differently,
// e.g. in what PC-Tool offers for it.
static const uint8_t HANDSHAKE_READS[] = {0x00, 0x03, 0x04, 0x05, 0x06, 0x07, 0x09};

// Each register's reply is kept in flash, so a change - say after a PC-Tool
// session - is logged with exactly which bytes moved. Register 04 is left out:
// it holds the levels, which change all the time.
void rememberRegister(uint8_t reg, const uint8_t *data, size_t n) {
  char key[4], h[240];
  h[0] = '\0';
  snprintf(key, sizeof key, "r%02X", reg);
  uint8_t old[96];
  size_t oldN = prefs.getBytesLength(key);
  if (oldN > sizeof old) oldN = 0;
  if (oldN) prefs.getBytes(key, old, oldN);

  if (!oldN) {
    logEvent("reg %02X first seen (%u bytes): %s", reg, (unsigned)n, toHex(h, sizeof h, data, n));
  } else if (oldN != n) {
    logEvent("reg %02X CHANGED length %u -> %u: %s", reg, (unsigned)oldN, (unsigned)n, toHex(h, sizeof h, data, n));
  } else if (memcmp(old, data, n)) {
    // List only the bytes that moved: register 03 is 78 bytes long.
    size_t at = 0;
    for (size_t k = 0; k < n && at + 16 < sizeof h; k++)
      if (old[k] != data[k]) at += snprintf(h + at, sizeof h - at, " [%u] %02X>%02X", (unsigned)k, old[k], data[k]);
    logEvent("reg %02X CHANGED:%s", reg, h);
  } else {
    return;                                       // as before: nothing to say
  }
  prefs.putBytes(key, data, n);
}

bool handshake(const char *why) {
  int answered = 0;
  for (uint8_t reg : HANDSHAKE_READS) {
    uint8_t body[100];                            // register 03's reply is 82 bytes
    size_t r = transact(0x2A, reg, nullptr, 0, body, sizeof body);
    if (r < 4 || body[1] != 0x2A || body[2] != reg) continue;
    answered++;
    const uint8_t *data = body + 3;               // between REG and CS
    size_t n = r - 4;
    if (reg == 0x04) { if (n >= 2) { master = data[0]; sub = data[1]; } }
    else rememberRegister(reg, data, n);
  }
  bool enabled = enableWrites();
  handshakeDue = false;                           // done, whichever way it went
  logEvent("handshake (%s): %d of %u reads answered, enable %s", why, answered,
           (unsigned)sizeof HANDSHAKE_READS, enabled ? "accepted" : "NOT accepted");
  return enabled;
}

// 'g N': read any register and print its data bytes, for exploring.
bool readRegister(long reg) {
  if (reg < 0 || reg > 0xFF) { io.print("REFUSED: register must be 0..255\r\n"); return false; }
  uint8_t body[100]; char h[300];
  size_t r = transact(0x2A, (uint8_t)reg, nullptr, 0, body, sizeof body);
  if (r < 4 || body[1] != 0x2A || body[2] != (uint8_t)reg) return false;
  io.printf("reg %02X: %s\r\n", (unsigned)reg, toHex(h, sizeof h, body + 3, r - 4));
  return true;
}

bool setInput(long idx) {
  if (idx < 0 || idx > 2) { io.print("REFUSED: input must be 0, 1 or 2\r\n"); return false; }
  return writeReg(0x07, (uint8_t)idx, 0x01, 0x01);
}

// ---- watching the amp -----------------------------------------------------
// Bytes arriving while no request is outstanding: the amp speaking unprompted,
// which no capture has ever shown. Collected until the line has been quiet 5 ms.
void watchUnsolicited() {
  if (!scp.available()) return;
  uint8_t b[96]; size_t n = 0; uint32_t last = millis();
  while (millis() - last < 5) {
    if (scp.available()) { int c = scp.read(); if (n < sizeof b) b[n++] = (uint8_t)c; last = millis(); }
  }
  char h[300];
  logEvent("unsolicited from amp (%u bytes): %s", (unsigned)n, toHex(h, sizeof h, b, n));
}

// 't N': the phone's clock, so log lines carry a real date. Nothing before
// 2023 is a plausible "now", which catches a phone sending milliseconds or junk.
bool setClock(long epoch) {
  if (epoch < 1672531200L) { io.print("REFUSED: need Unix time in seconds\r\n"); return false; }
  bool first = !syncEpoch;
  syncEpoch = (uint32_t)epoch;
  syncMs = millis();
  if (first) logEvent("clock set by the phone");   // ties this boot's uptime to a date
  return true;
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
  io.printf("firmware v%s   name %s   connected %s   advertising %s\r\n",
            FW_VERSION, deviceName, bleConnected ? "yes" : "no", advertising ? "yes" : "no");
  io.print("uptime ");  printDuration(now);
  io.printf("   connects %lu   drops %lu\r\n",
            (unsigned long)bleConnects, (unsigned long)bleDisconnects);
  io.print("session "); printDuration(bleConnected ? now - connectedAtMs : lastSessionMs);
  io.print("   longest "); printDuration(longestSessionMs);
  io.printf("   heap %lu\r\n", (unsigned long)ESP.getFreeHeap());
  io.printf("amp answering %s   boot #%lu   log %u bytes%s   clock %s\r\n",
            ampAnswering ? "yes" : "no", (unsigned long)bootCount,
            (unsigned)(fileSize(LOG_OLD) + fileSize(LOG_PATH)), logReady ? "" : " (flash unavailable)",
            syncEpoch ? "set" : "not set");
}

void badNum() { io.print("REFUSED: need a number, e.g. 'm 30' or 'm 0x1E'\r\n"); }

void handle(String line) {
  line.trim(); if (!line.length()) return;
  // Whole-word commands first: 'log' would otherwise be read as 'l' + "og".
  if (line == "log")       { dumpLog(); return; }
  if (line == "log clear") { clearLog(); io.print("OK\r\n"); return; }
  char c = line[0];
  String arg = line.substring(1); arg.trim();
  long v = 0;
  bool numOk = parseNum(arg, v);
  bool ok = false;
  switch (c) {
    case 'r': ok = readLevels(); break;
    case 'e': ok = enableWrites(); break;
    case 'h': ok = handshake("console"); break;
    case 'g': if (!numOk) { badNum(); break; } ok = readRegister(v); break;
    case 't': if (!numOk) { badNum(); break; } ok = setClock(v); break;
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
      io.printf("r | e | h | m N | s N | i N | + | - | g N | t N | log | log clear | d | v 0/1"
                "  (N decimal, or 0x.. hex)\r\n"
                "master 0..%u, sub 0..%u, input 0..2\r\n"
                "firmware v%s\r\n",
                MAX_MASTER, MAX_SUB, FW_VERSION);
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

  // A BLE advertisement carries at most 31 bytes. Flags (3) + the 128-bit NUS
  // UUID (18) + the name (10) is exactly 31 with nothing to spare, and the
  // library may also add TX power, which overflows it. Rather than depend on how
  // the library happens to split that, set both packets explicitly: the NAME goes
  // in the primary advertisement, where even a passive scan sees it, and the
  // 128-bit UUID goes in the scan response. 13 and 18 bytes, both comfortable.
  BLEAdvertisementData advData;
  advData.setFlags(0x06);                        // LE General Discoverable, no BR/EDR
  advData.setName(deviceName);

  BLEAdvertisementData scanData;
  scanData.setCompleteServices(BLEUUID(NUS_SERVICE));

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->setScanResponse(true);
  beginAdvertising();
}

void setup() {
  Serial.begin(115200);

  // On a native-USB part (H2, C3, C6, S3) the host takes a moment to enumerate
  // and a terminal longer still to attach, so a fixed short delay emits the
  // banner into a port nobody is listening to yet and the console looks dead.
  // Wait for the host to open the port, but bounded, so a board running headless
  // still boots. On a UART part this returns true at once and costs nothing.
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  delay(200);

  prefs.begin("scp", false);
  bootCount = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", bootCount);
  logReady = LittleFS.begin(true);                 // formats the partition on first use
  logEvent("boot: firmware v%s, reset by %s%s", FW_VERSION, resetReason(), logReady ? "" : ", flash log unavailable");

  scp.begin(230400, SERIAL_8N1, PIN_RX, PIN_TX);
  // Pull RX UP. The amp's TX line needs it to read high between bytes, as the knob
  // provides; v8's pull-down silenced the amp entirely (see v8.1 at the top). The
  // UART driver normally sets this already; it is stated here so nothing undoes it.
  gpio_pulldown_dis((gpio_num_t)PIN_RX);
  gpio_pullup_en((gpio_num_t)PIN_RX);
  startBle();
  Serial.printf("\r\nSCP bench v%s ready, advertising as \"%s\". '?' for help.\r\n", FW_VERSION, deviceName);
  Serial.print("If you missed this banner, just type '?' - the console is live either way.\r\n");
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

  // 3. the amp: the knob's handshake once it has started answering, and anything
  //    it says unasked
  if (handshakeDue) handshake("amp started answering");
  watchUnsolicited();

  // 4. restart advertising after a disconnect
  if (needAdvertise) {
    needAdvertise = false;
    delay(200);                                    // let the stack tear the link down
    beginAdvertising();
    Serial.print("re-advertising\r\n");
  }

  // 5. belt and braces: if we are neither connected nor advertising, nobody can
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
