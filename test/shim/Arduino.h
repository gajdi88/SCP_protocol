// Enough of the Arduino and ESP32 API to compile the sketches on a host, plus a
// simulated amplifier behind the SCP UART. Deliberately minimal: it models what
// the sketches actually use, and nothing else.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <cctype>
#include <string>
#include <vector>
#include <deque>

// Ordered log of side effects, so a test can assert that no UART read is
// interleaved with a BLE notify.
extern std::vector<std::string> g_seq;
extern std::string g_usb;                  // everything written to the USB console
extern std::vector<uint8_t> g_tx;          // every byte put on the SCP line

struct String : std::string {
  String(){} String(const char*s):std::string(s){} String(const std::string&s):std::string(s){}
  bool startsWith(const char*p) const { return rfind(p,0)==0; }
  void trim(){ size_t a=find_first_not_of(" \t\r\n"), b=find_last_not_of(" \t\r\n");
               if(a==npos){clear();return;} assign(substr(a,b-a+1)); }
  String substring(size_t i) const { return i>=size()?String(""):String(std::string(substr(i))); }
  size_t length() const { return std::string::length(); }
  String& operator+=(char c){ push_back(c); return *this; }
};

class Print {
public:
  virtual ~Print(){}
  virtual size_t write(uint8_t)=0;
  virtual size_t write(const uint8_t*b,size_t n){ size_t c=0; for(size_t i=0;i<n;i++) c+=write(b[i]); return c; }
  size_t print(const char*s){ return write((const uint8_t*)s,strlen(s)); }
  size_t printf(const char*f,...){ char t[1024]; va_list a; va_start(a,f);
    int n=vsnprintf(t,sizeof t,f,a); va_end(a); return write((const uint8_t*)t,(size_t)n); }
};

struct UsbSerial : public Print {
  void begin(unsigned long){}
  operator bool() const { return true; }   // a UART part is always "attached"
  int available(){ return 0; }
  int read(){ return -1; }
  size_t write(uint8_t b) override { g_usb.push_back((char)b); return 1; }
  using Print::write;
};

// The SCP line. Bytes written by the sketch land in g_tx. Bytes the sketch will
// read come from rx, either injected by a test with feed(), or generated here when
// simulateAmp is on.
struct ScpSerial : public Print {
  std::deque<uint8_t> rx;
  bool     simulateAmp = false;            // answer requests as the amp would
  bool     present     = true;             // false = nothing on the other end
  uint8_t  master = 0x1B, sub = 0x17, input = 0x00;
  bool     enabled = false;                // register 01 write gate
  uint8_t  tail04[2] = {0x00, 0x00};       // the two bytes after the levels in reg 04
  // Register 03 as captured at power-up (spec Appendix A), 78 data bytes. A test can
  // change a byte to stand in for a PC-Tool configuration change.
  std::vector<uint8_t> reg03 = {
    0x02,0x00,0x88,0x13,0xB8,0x0B,0x40,0x1F,0x01,0xB2,0x59,0x00,0x00,0x01,0x00,0x00,
    0x00,0xFF,0xFF,0xFF,0x3C,0x01,0x58,0x02,0xA8,0xFD,0x01,0xFF,0x00,0x00,0x18,0x01,
    0x78,0x00,0xA8,0xFD,0xFF,0x00,0x00,0x00,0x3C,0x01,0x58,0x02,0xA8,0xFD,0xFF,0x00,
    0x00,0x00,0x3C,0x01,0x58,0x02,0xA8,0xFD,0x00,0x01,0x3C,0x00,0xFF,0x00,0x18,0x01,
    0x78,0x00,0x96,0x00,0xFF,0xC8,0x0A,0x00,0x18,0x01,0x78,0x00,0x88,0x13};

  void begin(unsigned long,int,int,int){}
  int  available(){ return (int)rx.size(); }
  int  read(){ g_seq.push_back("rd"); if(rx.empty()) return -1;
               int v=rx.front(); rx.pop_front(); return v; }
  void flush(){}
  size_t write(uint8_t b) override { g_tx.push_back(b); return 1; }
  size_t write(const uint8_t*b,size_t n) override {
    g_tx.insert(g_tx.end(), b, b+n);
    if (simulateAmp) respond(b,n);
    return n;
  }
  void feed(const std::vector<uint8_t>&f){ for(size_t i=0;i<f.size();i++) rx.push_back(f[i]); }
  void powerCycle(){ enabled=false; }

  // LEN = 2 + ndata, one less than the span it covers: the asymmetry the spec
  // records as "the reply carries one byte its LEN does not count".
  void reply(const std::vector<uint8_t>&body){
    uint8_t cs=0; for(size_t i=1;i<body.size();i++) cs+=body[i];
    uint8_t len=(uint8_t)(body.size()+1-2);
    rx.push_back(0x43); rx.push_back(len); rx.push_back((uint8_t)~len);
    for(size_t i=0;i<body.size();i++) rx.push_back(body[i]);
    rx.push_back(cs);
  }
  void respond(const uint8_t*f,size_t n){
    if(!present || n<7 || f[0]!=0x42) return;
    uint8_t cmd=f[4], reg=f[5];
    std::vector<uint8_t> d(f+6, f+n-1);
    // The power-up handshake registers, with the data from the spec's capture (§5).
    if(cmd==0x2A && reg==0x00){ reply({0x01,0x2A,0x00,0x01,0x03}); return; }
    if(cmd==0x2A && reg==0x03){ std::vector<uint8_t> b={0x01,0x2A,0x03};
                                b.insert(b.end(),reg03.begin(),reg03.end()); reply(b); return; }
    if(cmd==0x2A && reg==0x04){ reply({0x01,0x2A,0x04,master,sub,tail04[0],tail04[1]}); return; }
    if(cmd==0x2A && reg==0x05){ reply({0x01,0x2A,0x05,0x00}); return; }
    if(cmd==0x2A && reg==0x06){ reply({0x01,0x2A,0x06,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}); return; }
    if(cmd==0x2A && reg==0x07){ reply({0x01,0x2A,0x07,input,0x03,0x01,0x00}); return; }
    if(cmd==0x2A && reg==0x09){ reply({0x01,0x2A,0x09,0x00,0x00,0x00,0x96,0x01,0x00,0x13,0x88}); return; }
    if(cmd==0x2B){
      if(reg==0x01){ enabled = !d.empty() && d[0]==0x01; reply({0x01,0x2B,0x01,0x01,0x01}); return; }
      if(!enabled){ reply({0x01,0x2B,0x01,0x00}); return; }       // the rejection frame
      if(reg==0x04 && d.size()>=2){ (d[0]?sub:master)=d[1]; }
      if(reg==0x07 && d.size()>=1){ input=d[0]; }
      std::vector<uint8_t> b={0x01,0x2B,reg}; b.insert(b.end(),d.begin(),d.end()); reply(b);
    }
  }
};
typedef ScpSerial HardwareSerial;

extern UsbSerial Serial;
extern ScpSerial Serial1, Serial2;
#define SERIAL_8N1 0

unsigned long millis();
void delay(unsigned long);
void delayMicroseconds(unsigned long);

struct EspClass {
  uint64_t getEfuseMac(){ return 0x30AEA4C1B2D4ULL; }
  unsigned long getFreeHeap(){ return 149576; }
};
extern EspClass ESP;
