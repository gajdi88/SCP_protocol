// Stage 0: BLE bring-up test. Nothing here touches the SCP bus or the amplifier.
//
// Exposes the Nordic UART Service (NUS) and echoes every line back in UPPERCASE,
// so a round trip is distinguishable from your terminal's local echo. Also mirrors
// everything to USB serial at 115200, and notifies a heartbeat every 5 s so you can
// confirm the notify path without typing anything.
//
// Validate with nRF Connect or LightBlue (phone or Mac): scan for "SCP-Bench",
// connect, enable notifications on the ...0003... characteristic, then write text
// to the ...0002... one. Type "hello" and you should get "HELLO" back.
//
// BEFORE UPLOADING: Tools -> Partition Scheme -> "Huge APP (3MB No OTA)".
// The BLE stack is ~1.3 MB and the default scheme only gives the app 1.2 MB;
// without this you get a linker error about the text section not fitting.
//
// This sketch is deliberately structured the way scp_bench will need:
//   - BLE callbacks NEVER do slow work. They hand a completed line to loop()
//     through a FreeRTOS queue. Blocking inside a BLE callback starves the
//     stack and causes intermittent disconnects.
//   - Notifications are chunked to 20 bytes, the safe payload at the default MTU.
//   - Advertising restarts from loop(), not from the disconnect callback.

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#if __has_include(<BLE2902.h>)
  #include <BLE2902.h>              // arduino-esp32 2.x needs the CCCD added by hand
  #define HAS_BLE2902 1             // 3.x adds it automatically and drops this header
#endif

// Nordic UART Service. The names are from the PERIPHERAL's point of view:
// this device RECEIVES on RX and NOTIFIES on TX. Centrals have it the other way round.
#define NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"   // central writes here
#define NUS_TX      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"   // we notify here

static const char  *DEVICE_NAME = "SCP-Bench";
static const size_t CHUNK       = 20;     // safe notify payload at the default 23-byte MTU
static const size_t MAX_LINE    = 64;
static const uint32_t HEARTBEAT_MS = 5000;

struct Line { char text[MAX_LINE]; };

BLECharacteristic *txChar = nullptr;
QueueHandle_t      lineQ  = nullptr;
volatile bool      connected    = false;
volatile bool      needAdvertise = false;

// ---- output --------------------------------------------------------------
// One notify per line, split into <=CHUNK pieces. Never notify per byte: a long
// line would become hundreds of packets at a ~7.5 ms connection interval.
void bleSend(const char *s) {
  if (!connected || !txChar) return;
  size_t n = strlen(s);
  for (size_t off = 0; off < n; off += CHUNK) {
    size_t take = n - off < CHUNK ? n - off : CHUNK;
    txChar->setValue((uint8_t *)(s + off), take);
    txChar->notify();
    delay(4);                        // let the stack drain; back-to-back notifies drop
  }
}

void emit(const char *s) {           // to both transports
  Serial.print(s);
  bleSend(s);
}

// ---- BLE callbacks: fast only --------------------------------------------
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    connected = true;
    Serial.print("BLE connected\r\n");
  }
  void onDisconnect(BLEServer *) override {
    connected = false;
    needAdvertise = true;            // loop() restarts it; doing it here is flaky
    Serial.print("BLE disconnected\r\n");
  }
};

class RxCB : public BLECharacteristicCallbacks {
  // Assembled only in this task, so no locking needed. Only the finished
  // line crosses to loop(), and it crosses through the queue.
  char buf[MAX_LINE];
  size_t len = 0;

  void onWrite(BLECharacteristic *c) override {
    uint8_t *data = c->getData();    // getData/getLength are stable across core
    size_t n = c->getLength();       // versions; getValue()'s type is not
    for (size_t i = 0; i < n; i++) {
      char ch = (char)data[i];
      if (ch == '\r' || ch == '\n') {
        push();
      } else if (ch >= 0x20 && ch < 0x7F) {
        if (len < MAX_LINE - 1) buf[len++] = ch;
      } else {
        len = 0;                     // non-printable: drop the line
      }
    }
    // A write with no newline still counts as a line. Phone apps often send
    // "hello" with nothing after it, and waiting forever looks like a hang.
    if (len) push();
  }

  void push() {
    if (!len) return;
    buf[len] = '\0';
    Line l;
    strncpy(l.text, buf, MAX_LINE);
    l.text[MAX_LINE - 1] = '\0';
    xQueueSend(lineQ, &l, 0);        // never block inside a BLE callback
    len = 0;
  }
};

// ---- the "work" ----------------------------------------------------------
void handleLine(const char *in) {
  char out[MAX_LINE + 16];
  size_t i = 0;
  for (; in[i] && i < MAX_LINE - 1; i++) out[i] = toupper((unsigned char)in[i]);
  out[i] = '\0';
  strcat(out, "\r\n");
  emit(out);
}

// ---- main ----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.print("\r\nBLE echo (stage 0) starting\r\n");

  lineQ = xQueueCreate(8, sizeof(Line));

  BLEDevice::init(DEVICE_NAME);
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
  BLEDevice::startAdvertising();

  Serial.printf("advertising as \"%s\"\r\n", DEVICE_NAME);
  Serial.print("waiting for a central. Type here too: USB input is echoed the same way.\r\n");
}

void loop() {
  // 1. lines that arrived over BLE
  Line l;
  while (xQueueReceive(lineQ, &l, 0) == pdTRUE) {
    Serial.printf("BLE rx: %s\r\n", l.text);
    handleLine(l.text);
  }

  // 2. lines typed on USB serial, so the sketch is testable with no phone at all
  static char sbuf[MAX_LINE];
  static size_t slen = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r' || ch == '\n') {
      if (slen) { sbuf[slen] = '\0'; handleLine(sbuf); slen = 0; }
    } else if (ch >= 0x20 && ch < 0x7F) {
      if (slen < MAX_LINE - 1) sbuf[slen++] = ch;
    } else slen = 0;
  }

  // 3. restart advertising after a disconnect
  if (needAdvertise) {
    needAdvertise = false;
    delay(200);                      // give the stack a moment to tear the link down
    BLEDevice::startAdvertising();
    Serial.print("re-advertising\r\n");
  }

  // 4. heartbeat, so the notify path is visible without typing
  static uint32_t last = 0;
  static uint32_t beat = 0;
  if (connected && millis() - last >= HEARTBEAT_MS) {
    last = millis();
    char msg[48];
    snprintf(msg, sizeof msg, "beat %lu\r\n", (unsigned long)++beat);
    emit(msg);
  }

  delay(10);
}
