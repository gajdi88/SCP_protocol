// ESP32 NVS key-value storage, as a map. Survives "reboots" within a test run
// (a second setup() call), which is what makes it worth modelling.
#pragma once
#include "Arduino.h"
#include <map>

static std::map<std::string, std::vector<uint8_t> > g_nvs;

struct Preferences {
  bool begin(const char *, bool) { return true; }
  uint32_t getUInt(const char *k, uint32_t def) {
    if (!g_nvs.count(k) || g_nvs[k].size() != 4) return def;
    uint32_t v; memcpy(&v, &g_nvs[k][0], 4); return v;
  }
  size_t putUInt(const char *k, uint32_t v) {
    g_nvs[k].assign((uint8_t *)&v, (uint8_t *)&v + 4); return 4;
  }
  size_t getBytesLength(const char *k) { return g_nvs.count(k) ? g_nvs[k].size() : 0; }
  size_t getBytes(const char *k, void *buf, size_t n) {
    if (!g_nvs.count(k)) return 0;
    size_t m = g_nvs[k].size() < n ? g_nvs[k].size() : n;
    memcpy(buf, &g_nvs[k][0], m); return m;
  }
  size_t putBytes(const char *k, const void *buf, size_t n) {
    g_nvs[k].assign((const uint8_t *)buf, (const uint8_t *)buf + n); return n;
  }
};
