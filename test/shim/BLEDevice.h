// Stubbed Bluedroid BLE. Records notifications and lets a test drive the write
// callback directly, standing in for a connected central.
#pragma once
#include "Arduino.h"
extern std::vector<std::string> g_notifies;
extern int g_advCalls;                     // how many times advertising was started

struct BLECharacteristic {
  static const uint32_t PROPERTY_NOTIFY=1, PROPERTY_WRITE=2, PROPERTY_WRITE_NR=4;
  std::string val;
  uint8_t *data_ = nullptr; size_t len_ = 0;
  void setValue(uint8_t*p,size_t n){ val.assign((char*)p,n); }
  void notify();
  uint8_t *getData(){ return data_; }
  size_t   getLength(){ return len_; }
  void setCallbacks(struct BLECharacteristicCallbacks*);
  void addDescriptor(void*){}
  void feed(const char*s){ data_=(uint8_t*)s; len_=strlen(s); }   // as if a central wrote it
};
struct BLECharacteristicCallbacks {
  virtual void onWrite(BLECharacteristic*){} virtual ~BLECharacteristicCallbacks(){}
};
struct BLEServer;
struct BLEServerCallbacks {
  virtual void onConnect(BLEServer*){} virtual void onDisconnect(BLEServer*){}
  virtual ~BLEServerCallbacks(){}
};
struct BLEService {
  BLECharacteristic* createCharacteristic(const char*,uint32_t){ return new BLECharacteristic(); }
  void start(){}
};
struct BLEServer {
  void setCallbacks(BLEServerCallbacks*){}
  BLEService* createService(const char*){ return new BLEService(); }
};
struct BLEUUID { BLEUUID(const char*){} };
struct BLEAdvertisementData {
  void setFlags(uint8_t){} void setName(const char*){} void setCompleteServices(BLEUUID){}
};
struct BLEAdvertising {
  void addServiceUUID(const char*){} void setScanResponse(bool){}
  void setAdvertisementData(BLEAdvertisementData&){}
  void setScanResponseData(BLEAdvertisementData&){}
};
struct BLEDeviceT {
  static void init(const char*){}
  static BLEServer* createServer(){ return new BLEServer(); }
  static BLEAdvertising* getAdvertising(){ static BLEAdvertising a; return &a; }
  static void startAdvertising(){ g_advCalls++; }
};
#define BLEDevice BLEDeviceT

typedef struct QueueDef* QueueHandle_t;
#define pdTRUE 1
QueueHandle_t xQueueCreate(int,int);
int xQueueSend(QueueHandle_t,const void*,int);
int xQueueReceive(QueueHandle_t,void*,int);
