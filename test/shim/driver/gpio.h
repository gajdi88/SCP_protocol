// The GPIO calls scp_bench makes. A test sets g_gpioLevel[pin] to stand in for
// the amp's TX line, and can check which pulls the sketch configured.
#pragma once
typedef int gpio_num_t;
static int  g_gpioLevel[64];               // 0 = low, as a powered-off amp reads
static bool g_pullDown[64], g_pullUp[64];
inline int  gpio_get_level(gpio_num_t p) { return g_gpioLevel[p]; }
inline int  gpio_pulldown_en(gpio_num_t p) { g_pullDown[p] = true;  return 0; }
inline int  gpio_pullup_dis(gpio_num_t p)  { g_pullUp[p]   = false; return 0; }
