// Definitions shared by both test programs. Included once, after the shim headers
// and before the sketch under test.
#pragma once
#include "Arduino.h"

UsbSerial Serial;
ScpSerial Serial1, Serial2;
EspClass  ESP;
std::string g_usb;
std::vector<uint8_t> g_tx;
std::vector<std::string> g_seq, g_notifies;
int g_advCalls = 0;

// millis() always advances, so no wait loop in a sketch can spin forever here.
// It moves a quarter of a millisecond per call: fine-grained enough that reading
// register 03's 85-byte reply byte by byte fits the sketch's 50 ms reply timeout,
// as it does on the real UART (about 4 ms at 230400 baud).
static unsigned long g_us = 0;
unsigned long millis(){ g_us += 250; return g_us / 1000; }
void delay(unsigned long m){ g_us += m * 1000; }
void delayMicroseconds(unsigned long){}

// ---- assertions ----
static int g_fails = 0;
inline void ck(bool ok, const char *what) {
  printf("  %-54s %s\n", what, ok ? "PASS" : "** FAIL **");
  if (!ok) g_fails++;
}
inline void section(const char *title){ printf("\n-- %s --\n", title); }
inline int report(){ printf("\n%s\n", g_fails ? "** FAILURES **" : "all checks passed"); return g_fails; }

// ---- helpers ----
inline std::string usb(){ std::string s = g_usb; g_usb.clear(); return s; }
inline bool has(const std::string &h, const char *n){ return h.find(n) != std::string::npos; }
inline std::vector<uint8_t> hexv(const char *s){
  std::vector<uint8_t> v; const char *p = s;
  while(*p){ if(*p==' '||*p=='\n'){ p++; continue; }
             v.push_back((uint8_t)strtol(std::string(p,2).c_str(),nullptr,16)); p+=2; }
  return v;
}
inline std::string hexs(const std::vector<uint8_t> &v){
  std::string s; char b[4];
  for(size_t i=0;i<v.size();i++){ snprintf(b,sizeof b,"%02X",v[i]); if(i) s+=' '; s+=b; }
  return s;
}
