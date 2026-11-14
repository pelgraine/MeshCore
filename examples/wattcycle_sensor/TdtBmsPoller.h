#pragma once

// Reads Wattcycle (TDT BMS) batteries over Bluetooth LE, in a background FreeRTOS task,
// so slow or missing batteries never hold up the mesh loop.
//
// Each cycle it connects to each battery in turn, unlocks it, requests device info (0x92)
// and status (0x8C), then disconnects, leaving the battery free for the phone app.

#include <Arduino.h>
#include "TdtBmsProtocol.h"

#ifndef BMS_MAX_BATTERIES
  #define BMS_MAX_BATTERIES  4
#endif

// Diagnostic status lines on the serial port (set BMS_DEBUG=1 in platformio.ini)
#if defined(BMS_DEBUG) && BMS_DEBUG
  #define BMS_LOG(...)  Serial.printf(__VA_ARGS__)
#else
  #define BMS_LOG(...)  do { } while (0)
#endif

#define BMS_NAME_MAX  32

class NimBLEClient;
class NimBLERemoteCharacteristic;

class TdtBmsPoller {
public:
  TdtBmsPoller();

  // names_csv: comma separated Bluetooth names, e.g. "XDZN_001_E4E7" or "XDZN_001_E4E7,XDZN_001_F814"
  void begin(const char* names_csv, uint32_t interval_secs);

  int getCount() const { return _count; }
  const char* getName(int idx) const { return _batts[idx].name; }

  // Copies the latest reading for battery 'idx'. Returns false if it has never been read.
  bool getReading(int idx, TdtBmsData& dest, uint32_t& age_secs);

private:
  struct Battery {
    char       name[BMS_NAME_MAX];
    bool       have_addr;
    uint8_t    addr[6];
    uint8_t    addr_type;
    uint8_t    head;           // command head the battery answers to (0 = not known yet)
    bool       have_reading;
    uint32_t   read_millis;
    TdtBmsData data;
  };

  Battery  _batts[BMS_MAX_BATTERIES];
  int      _count;
  uint32_t _interval_ms;
  SemaphoreHandle_t _lock;        // guards have_reading, read_millis, data
  portMUX_TYPE      _rx_mux;      // guards _rx (filled from the BLE host task)
  TdtFrameAssembler _rx;
  uint32_t          _rx_bytes;    // notification bytes received (diagnostics)
  NimBLEClient*     _client;

  static void taskEntry(void* arg);
  void run();
  void scanForMissing();
  bool pollOne(Battery& b);
  bool sendCommand(NimBLERemoteCharacteristic* tx, Battery& b, uint8_t cmd, uint8_t* frame, size_t& frame_len);
  bool sendWithHead(NimBLERemoteCharacteristic* tx, uint8_t head, uint8_t cmd, uint8_t* frame, size_t& frame_len);
  void onNotify(const uint8_t* data, size_t len);
};
