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
  if(d->q.empty()) return 0; memcpy(o,&d->q.front()[0],d->esz); d->q.pop_front(); return pdTRUE; }

BLECharacteristicCallbacks *g_rxcb = nullptr;
void BLECharacteristic::setCallbacks(BLECharacteristicCallbacks*c){ g_rxcb = c; }
void BLECharacteristic::notify(){ g_seq.push_back("notify"); g_notifies.push_back(val); }

#include "../scp_bench/scp_bench.ino"

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

  return report();
}
