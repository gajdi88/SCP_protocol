// The GPIO calls scp_bench makes, so a test can check which pulls it configured.
#pragma once
typedef int gpio_num_t;
static bool g_pullDown[64], g_pullUp[64];
inline int  gpio_pulldown_en(gpio_num_t p)  { g_pullDown[p] = true;  return 0; }
inline int  gpio_pulldown_dis(gpio_num_t p) { g_pullDown[p] = false; return 0; }
inline int  gpio_pullup_en(gpio_num_t p)    { g_pullUp[p]   = true;  return 0; }
inline int  gpio_pullup_dis(gpio_num_t p)   { g_pullUp[p]   = false; return 0; }
