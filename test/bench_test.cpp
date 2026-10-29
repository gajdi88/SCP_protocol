// Tests scp_bench against a simulated amplifier. The sketch is included verbatim,
// so these tests cannot drift out of step with the firmware.
#include "shim/Arduino.h"
#include "shim/BLEDevice.h"
#include "shim/support.h"

struct QueueDef { std::deque<std::vector<char> > q; int esz; };
QueueHandle_t xQueueCreate(int,int e){ QueueDef*d=new QueueDef(); d->esz=e; return (QueueHandle_t)d; }
int xQueueSend(QueueHandle_t h,const void*i,int){ QueueDef*d=(QueueDef*)h;
  d->q.push_back(std::vector<char>((const char*)i,(const char*)i+d->esz)); return pdTRUE; }
int xQueueReceive(QueueHandle_t h,void*o,int){ QueueDef*d=(QueueDef*)h;
  if(d->q.empty()) return 0;
  memcpy(o,&d->q.front()[0],d->esz); d->q.pop_front(); return pdTRUE; }

BLECharacteristicCallbacks *g_rxcb = nullptr;
void BLECharacteristic::setCallbacks(BLECharacteristicCallbacks*c){ g_rxcb = c; }
void BLECharacteristic::notify(){ g_seq.push_back("notify"); g_notifies.push_back(val); }

#include "../scp_bench/scp_bench.ino"

// Both log files, oldest first, without creating either.
static std::string logText(){
  std::string s;
  const char *files[2] = {"/log.old", "/log.txt"};
  for (int i = 0; i < 2; i++) if (g_fs.files.count(files[i])) s += g_fs.files[files[i]];
  return s;
}

static std::string notifyText(){
  std::string s; for(size_t i=0;i<g_notifies.size();i++) s += g_notifies[i];
  g_notifies.clear(); return s;
}

int main(){
  setup(); usb();
  ScpSerial &amp = scp;
  amp.simulateAmp = true;

  section("reads, with writes still disabled on the amp");
  handle(String("r"));
  std::string o = usb();
  ck(has(o,"master=27"),               "r reads master 0x1B as 27");
  ck(has(o,"TX 42 03 FC 01 2A 04 2E"), "the read frame is the one in the spec");

  section("the write-enable gate");
  handle(String("m 30")); o = usb();
  ck(has(o,"amp says writes not enabled"), "the rejection frame is recognised");
  ck(has(o,"OK"),                          "auto-enable then retry succeeds");
  ck(amp.master == 30,                     "the amp ended up at master 30");

  section("ceilings are per target");
  ck(MAX_MASTER == 0x3C, "MAX_MASTER is 0x3C (60), the knob's measured end stop");
  ck(MAX_SUB    == 0x17, "MAX_SUB is 0x17 (23), highest observed rather than measured");
  handle(String("m 60")); ck(has(usb(),"OK"), "m 60 accepted: full volume is reachable");
  handle(String("m 61")); ck(has(usb(),"REFUSED: master 61 is outside 0..60"), "m 61 refused");
  handle(String("s 24")); ck(has(usb(),"REFUSED: sub 24 is outside 0..23"),    "s 24 refused");
  handle(String("s 23")); ck(has(usb(),"OK"), "s 23 accepted at the sub ceiling");
  handle(String("m x"));  ck(has(usb(),"need a number"), "m x refused, not silently read as 0");
  handle(String("m 030"));ck(has(usb(),"OK") && amp.master == 30, "m 030 is 30, not octal 24");
  handle(String("m 0x1E"));ck(amp.master == 0x1E, "m 0x1E is accepted as hex");

  section("a power-cycle re-arms the gate");
  amp.powerCycle();
  handle(String("m 25")); o = usb();
  ck(has(o,"amp says writes not enabled"), "the rejection is seen again");
  ck(has(o,"OK") && amp.master == 25,      "and the controller recovers by itself");

  section("input select");
  handle(String("i 1")); ck(has(usb(),"OK") && amp.input == 1, "i 1 selects optical");
  handle(String("i 3")); ck(has(usb(),"REFUSED"),              "i 3 refused, only 0..2 exist");

  section("the BLE path");
  bleConnected = true; notifyText(); g_seq.clear(); usb();
  BLECharacteristic ch; const char *cmd = "r\n"; ch.feed(cmd);
  g_rxcb->onWrite(&ch);
  ck(g_notifies.empty(), "the BLE callback itself emits nothing, doing no slow work");
  loop();
  std::string nt = notifyText();
  ck(has(nt,"master="),     "the queued command ran and answered over BLE");
  ck(has(nt,"TX 42 03 FC"), "the frame trace goes out over BLE too");

  section("notifies happen after the exchange, never during it");
  g_seq.clear(); handle(String("r")); usb();
  size_t firstNotify = g_seq.size(), lastRead = 0;
  for(size_t i=0;i<g_seq.size();i++){
    if(g_seq[i]=="notify" && firstNotify==g_seq.size()) firstNotify = i;
    if(g_seq[i]=="rd") lastRead = i;
  }
  ck(lastRead < firstNotify, "every UART read precedes the first notify");

  section("notify chunking");
  g_notifies.clear();
  uint8_t big[40]; for(int i=0;i<40;i++) big[i]=(uint8_t)i;
  dump("RX", big, 40);                      // "RX" plus 40*3 characters
  ck(g_notifies.size()==7, "a 122-character line becomes 7 notifies, not 122");
  bool sized = true;
  for(size_t i=0;i+1<g_notifies.size();i++) if(g_notifies[i].size()!=20) sized = false;
  ck(sized, "every chunk but the last is exactly 20 bytes");
  g_notifies.clear(); bleConnected = false;
  handle(String("r")); ck(g_notifies.empty(), "nothing is notified while disconnected");

  section("BLE hardening");
  ck(std::string(deviceName).rfind("SCP-",0)==0 && strlen(deviceName)==8,
     "the device names itself SCP-xxxx from its MAC");
  ServerCB cb; BLEServer *srv = nullptr;
  uint32_t c0 = bleConnects, d0 = bleDisconnects;
  ((BLEServerCallbacks&)cb).onConnect(srv);
  ck(bleConnects==c0+1 && !advertising, "connect counted, advertising flag cleared");
  delay(3000);
  ((BLEServerCallbacks&)cb).onDisconnect(srv);
  ck(bleDisconnects==d0+1 && needAdvertise, "drop counted and a re-arm requested");
  ck(longestSessionMs >= 3000,             "session duration recorded");
  int a0 = g_advCalls; loop();
  ck(g_advCalls > a0 && advertising, "loop() re-armed advertising after the drop");
  advertising = false; bleConnected = false;      // a re-arm that silently failed
  a0 = g_advCalls; delay(2500); loop();
  ck(g_advCalls > a0 && advertising, "the health check recovers a missed re-arm");
  bleConnected = true; usb();
  handle(String("d"));
  std::string dg = usb();
  ck(has(dg,"name SCP-") && has(dg,"connects ") && has(dg,"drops "), "d prints diagnostics");
  ck(has(dg,"connected yes"), "d reports the live connection state");

  section("the v verbosity toggle");
  usb(); handle(String("v 0")); usb();
  handle(String("r")); std::string q = usb();
  ck(!has(q,"TX 42"),   "quiet mode suppresses the frame trace");
  ck(has(q,"master="),  "but still reports the levels an app needs");
  int lines = 0; for(size_t i=0;i+1<q.size();i++) if(q[i]=='\r'&&q[i+1]=='\n') lines++;
  ck(lines==2, "one status line plus OK, not four");
  amp.present = false; usb(); handle(String("r"));
  ck(has(usb(),"no valid reply"), "failures are still reported when quiet");
  amp.present = true; handle(String("v 1")); usb();
  handle(String("r")); ck(has(usb(),"TX 42"), "v 1 restores the trace");
  handle(String("?"));
  ck(has(usb(),"master 0..60, sub 0..23"), "? exposes the ceilings for the app to parse");

  section("with no amp connected at all");
  amp.present = false; amp.rx.clear(); usb(); g_notifies.clear();
  handle(String("d")); ck(has(usb(),"name SCP-"), "d works with no amp: it never touches the bus");
  handle(String("?")); ck(has(usb(),"master 0.."), "? works with no amp");
  g_notifies.clear(); handle(String("r"));
  o = usb(); nt = notifyText();
  ck(has(o,"TX 42 03 FC 01 2A 04 2E"), "r still shows the frame it built");
  ck(has(o,"RX: no valid reply"),      "and reports the silence cleanly");
  ck(has(o,"FAILED"),                  "the command fails rather than hanging");
  ck(has(nt,"RX: no valid reply"),     "all of which reaches the phone over BLE");
  for(int i=0;i<200;i++) amp.rx.push_back((uint8_t)(i*7+3));   // a floating RX pin
  handle(String("r"));
  ck(has(usb(),"no valid reply"), "line noise is never mistaken for a reply");

  // ---- firmware v8: the knob's handshake, the amp line, the event log ----
  amp.present = true; amp.rx.clear(); bleConnected = false;
  handle(String("v 1")); usb();

  section("the knob's power-up handshake, byte for byte");
  amp.powerCycle(); g_tx.clear(); handle(String("h")); o = usb();
  ck(hexs(g_tx) == hexs(hexv(
       "42 03 FC 01 2A 00 2A  42 03 FC 01 2A 03 2D  42 03 FC 01 2A 04 2E"
       "42 03 FC 01 2A 05 2F  42 03 FC 01 2A 06 30  42 03 FC 01 2A 07 31"
       "42 03 FC 01 2A 09 33  42 05 FA 01 2B 01 01 01 2E")),
     "h: the seven reads, then the enable, as in spec 5");
  ck(amp.enabled && has(o,"OK"), "and the amp's writes end up enabled");
  handle(String("g 0")); ck(has(usb(),"reg 00: 01 03"), "g 0 reads register 00 (01 03)");

  section("a refused write runs the whole handshake, then retries");
  amp.powerCycle(); g_tx.clear(); handle(String("m 22")); o = usb();
  std::string tx = hexs(g_tx), wr = "42 06 F9 01 2B 04 00 16 01 46";
  ck(tx.find(wr) == 0 && tx.rfind(wr) == tx.size() - wr.size(),
     "the write is sent, refused, then sent again last");
  ck(has(tx,"42 03 FC 01 2A 03 2D") && has(tx,"42 05 FA 01 2B 01 01 01 2E"),
     "with the full handshake in between, not a bare enable");
  ck(has(o,"OK") && amp.master == 22, "and the write lands");

  section("the amp going away and coming back (v8.1)");
  // v8 pulled RX down and the real amp fell silent: its TX line needs a pull-up.
  ck(g_pullUp[PIN_RX] && !g_pullDown[PIN_RX], "RX is pulled up, not down");
  handle(String("r")); usb();
  ck(ampAnswering, "an answering amp counts as there");
  amp.present = false; amp.powerCycle();
  handle(String("r")); handle(String("r")); o = usb();
  ck(ampAnswering && !has(o,"amp off"), "two silent requests are not enough to count it gone");
  handle(String("r")); o = usb();
  ck(!ampAnswering && has(o,"amp off"), "the third in a row is");
  amp.present = true; g_tx.clear();
  handle(String("r")); o = usb();
  ck(has(o,"amp on") && has(o,"master="), "its first answer after that says amp on, and the read succeeds");
  ck(handshakeDue && !amp.enabled, "the handshake is due, not yet sent mid-command");
  g_tx.clear(); loop(); tx = hexs(g_tx);
  ck(tx.find("42 03 FC 01 2A 00 2A") == 0 && has(tx,"42 05 FA 01 2B 01 01 01 2E") && amp.enabled,
     "loop() then sends the knob's handshake unasked");
  g_tx.clear(); loop(); delay(5000); loop();
  ck(g_tx.empty() && !handshakeDue, "once per return, not repeatedly");
  amp.present = false; amp.powerCycle(); usb();
  handle(String("r")); handle(String("r")); handle(String("r"));
  amp.present = true; handle(String("m 21")); o = usb();
  ck(has(o,"amp on") && has(o,"OK") && amp.master == 21,
     "a write as the first command after silence also recovers");
  ck(!handshakeDue, "and its refusal's handshake counts, so none is left pending");

  section("the event log");
  clearLog(); usb();
  amp.present = false;
  handle(String("r")); handle(String("r")); handle(String("r"));
  amp.present = true;
  usb(); handle(String("log")); o = usb();
  size_t b0 = o.find("LOG BEGIN "), e0 = o.find("LOG END\r\n");
  ck(b0 != std::string::npos && e0 != std::string::npos && b0 < e0, "log is framed by LOG BEGIN / LOG END");
  std::string body = o.substr(o.find("\r\n", b0) + 2, e0 - o.find("\r\n", b0) - 2);
  ck(has(body,"log cleared"), "log clear leaves a line saying so");
  ck(has(body,"no valid reply (silence) to 42 03 FC 01 2A 04 2E, received: nothing"),
     "a silent amp is logged with the request it ignored");
  ck(has(body,"(previous line repeated 2 more times)"), "repeats collapse into one line and a count");
  bool hashes = true;
  for (size_t i = 0; i < body.size(); i = body.find("\r\n", i) + 2) {
    if (body[i] != '#') hashes = false;
    if (body.find("\r\n", i) == std::string::npos) break;
  }
  ck(hashes, "every log line starts with #");
  ck(!has(body,"master="), "no log line can be mistaken for a level reading");
  ck(!has(o.substr(e0),"OK"), "the dump ends at LOG END, with no OK after it");
  ck(atoi(o.c_str() + b0 + 10) == (int)logText().size(), "LOG BEGIN gives the true size in bytes");
  ck(has(g_fs.files["/log.txt"],"no valid reply"), "and it is all in flash, not only in memory");

  section("unexpected things the amp does");
  clearLog();
  amp.feed(hexv("43 05 FA 01 2B 04 00 10 01")); loop();
  ck(has(logText(),"unsolicited from amp (9 bytes): 43 05 FA 01 2B 04 00 10 01"),
     "bytes the amp sends unasked are logged");
  amp.feed(hexv("AA BB")); handle(String("r"));
  ck(has(logText(),"unsolicited bytes before a request: AA BB"),
     "bytes waiting before a request are logged, no longer discarded");
  amp.tail04[0] = 0x01; handle(String("r")); amp.tail04[0] = 0x00;
  ck(has(logText(),"bytes after the levels are 01 00"), "new data in register 04's tail is logged");
  amp.powerCycle(); handle(String("m 23"));
  ck(has(logText(),"write to 04 refused") && has(logText(),"handshake (a write was refused)"),
     "a refused write and the handshake it caused are logged");

  section("handshake registers are remembered between boots");
  clearLog(); handle(String("h"));
  ck(!has(logText(),"reg 03"), "an unchanged register is not logged again");
  ck(!has(logText(),"reg 04"), "register 04 (the levels) is never tracked");
  amp.reg03[20] = 0x3D; handle(String("h"));
  ck(has(logText(),"reg 03 CHANGED: [20] 3C>3D"), "a changed byte is logged as [offset] old>new");
  amp.reg03[20] = 0x3C; g_nvs.erase("r03"); clearLog(); handle(String("h"));
  ck(has(logText(),"reg 03 first seen (78 bytes): 02 00 88 13"), "a register never seen before is logged in full");

  section("the log clock");
  usb(); handle(String("t 5")); ck(has(usb(),"REFUSED"), "t 5 refused: not a plausible Unix time");
  handle(String("t 1761650000")); ck(has(usb(),"OK"), "t takes Unix time from the phone");
  clearLog(); ck(has(logText()," 2025-10-28T11:13:2"), "log lines then carry the date and time");
  handle(String("?")); o = usb();
  ck(has(o,"| log |"), "? lists log, which is how an app spots v8 and later");
  ck(has(o,"\r\nfirmware v8.2\r\n"), "? reports the firmware version on its own line");
  handle(String("d")); ck(has(usb(),"firmware v8.2   name SCP-"), "d reports it too");

  section("the log stays bounded");
  clearLog();
  for (int i = 0; i < 3000; i++) logEvent("filler event number %d, padded out to a realistic length", i);
  ck(fileSize(LOG_OLD) > 0, "a full log rolls over into the old file");
  ck(fileSize(LOG_PATH) <= LOG_MAX + MSG_MAX + 64 && fileSize(LOG_OLD) <= LOG_MAX + MSG_MAX + 64,
     "neither file grows past its cap");
  ck(has(logText(),"number 2999"), "the newest events are kept");

  section("boots and resets are logged");
  clearLog(); g_resetReason = ESP_RST_BROWNOUT;
  uint32_t boots = bootCount; setup(); usb();
  ck(bootCount == boots + 1, "the boot counter survives a reboot (kept in NVS)");
  ck(has(logText(),"boot: firmware v8.2, reset by brownout"), "the reset reason and version are logged");
  g_fs.files.clear(); g_fs.mountOk = false; setup(); usb();
  handle(String("log")); o = usb();
  ck(has(o,"LOG BEGIN 0") && has(o,"LOG END"), "with no flash, log still answers, empty");
  handle(String("d")); ck(has(usb(),"(flash unavailable)"), "and d says why");
  handle(String("r")); ck(has(usb(),"OK"), "control keeps working without the log");

  return report();
}
