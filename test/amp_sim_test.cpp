// Conformance test for amp_sim. Every request and expected reply below is a real
// frame from conductor-scp-protocol.md, so this checks the simulator against the
// amplifier's actual recorded behaviour rather than against our idea of it.
#include "shim/Arduino.h"
#include "shim/support.h"
#include "../amp_sim/amp_sim.ino"

// Send a request the way the knob did, and compare the answer byte for byte.
static void expect(const char *what, const char *req, const char *want){
  g_tx.clear(); g_usb.clear();
  scp.feed(hexv(req));
  pump();
  std::string got = hexs(g_tx), w = want ? hexs(hexv(want)) : std::string();
  bool ok = (got == w);
  printf("  %-54s %s\n", what, ok ? "PASS" : "** FAIL **");
  if(!ok){
    printf("      sent %s\n      got  %s\n      want %s\n",
           req, got.empty()?"(silence)":got.c_str(), w.empty()?"(silence)":w.c_str());
    g_fails++;
  }
}

int main(){
  setup(); g_usb.clear();
  printf("\nEvery frame below is a real capture from conductor-scp-protocol.md.\n");

  section("reads");
  expect("read levels (Appendix B)",
         "42 03 FC 01 2A 04 2E", "43 06 F9 01 2A 04 12 17 00 00 57");
  expect("read register 03 (Appendix A, 78 bytes)",
         "42 03 FC 01 2A 03 2D",
         "43 50 AF 01 2A 03 02 00 88 13 B8 0B 40 1F 01 B2 59 00 00 01 00 00"
         "00 FF FF FF 3C 01 58 02 A8 FD 01 FF 00 00 18 01"
         "78 00 A8 FD FF 00 00 00 3C 01 58 02 A8 FD FF 00"
         "00 00 3C 01 58 02 A8 FD 00 01 3C 00 FF 00 18 01"
         "78 00 96 00 FF C8 0A 00 18 01 78 00 88 13 3E");
  expect("read register 00 (handshake step 1)",
         "42 03 FC 01 2A 00 2A", "43 04 FB 01 2A 00 01 03 2E");
  expect("read register 07 (handshake step 6)",
         "42 03 FC 01 2A 07 31", "43 06 F9 01 2A 07 00 03 01 00 35");

  section("the write-enable gate");
  expect("a write before enable is refused, naming the cause",
         "42 06 F9 01 2B 04 00 28 01 58", "43 03 FC 01 2B 01 00 2C");
  expect("enable (Appendix B)",
         "42 05 FA 01 2B 01 01 01 2E", "43 04 FB 01 2B 01 01 01 2E");
  expect("master 0x28 now echoes (section 4)",
         "42 06 F9 01 2B 04 00 28 01 58", "43 05 FA 01 2B 04 00 28 01 58");
  expect("sub 0x17 echoes",
         "42 06 F9 01 2B 04 01 17 01 48", "43 05 FA 01 2B 04 01 17 01 48");
  expect("input optical echoes (section 4a)",
         "42 06 F9 01 2B 07 01 01 01 35", "43 05 FA 01 2B 07 01 01 01 35");
  expect("the write actually moved the level",
         "42 03 FC 01 2A 04 2E", "43 06 F9 01 2A 04 28 17 00 00 6D");
  expect("and register 07 now reports optical",
         "42 03 FC 01 2A 07 31", "43 06 F9 01 2A 07 01 03 01 00 36");

  section("a power-cycle drops the gate");
  handle(String("p"));
  expect("a write is refused again afterwards",
         "42 06 F9 01 2B 04 00 20 01 50", "43 03 FC 01 2B 01 00 2C");

  section("malformed frames are refused, and say why");
  g_usb.clear();
  expect("a bad checksum gets no reply", "42 03 FC 01 2A 04 FF", nullptr);
  ck(has(g_usb,"checksum"), "and the console names the checksum");
  g_usb.clear();
  expect("a LEN/~LEN mismatch gets no reply", "42 03 FB 01 2A 04 2E", nullptr);
  ck(has(g_usb,"do not sum to FF"), "and the console names the length");
  g_usb.clear();
  expect("an unknown command gets no reply", "42 03 FC 01 99 04 9D", nullptr);
  ck(has(g_usb,"neither 2A nor 2B"), "and the console says so");
  expect("garbage before the start byte is skipped",
         "FF 00 13 42 03 FC 01 2A 04 2E", "43 06 F9 01 2A 04 28 17 00 00 6D");

  section("deaf mode, for exercising a controller's timeout path");
  handle(String("x"));
  expect("deaf: a valid request is ignored", "42 03 FC 01 2A 04 2E", nullptr);
  handle(String("x"));
  expect("and it answers again once deafness is off",
         "42 03 FC 01 2A 04 2E", "43 06 F9 01 2A 04 28 17 00 00 6D");

  return report();
}
