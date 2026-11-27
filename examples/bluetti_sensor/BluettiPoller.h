#pragma once

// Reads Bluetti power stations (IoT protocol V2, encrypted, e.g. EL30V2 and PR30V2) over
// Bluetooth LE, in a background FreeRTOS task, so slow or missing units never hold up the mesh loop.
//
// Each cycle it connects to each unit in turn, does the key exchange, reads the registers,
// then disconnects, leaving the unit free for the phone app.

#include <Arduino.h>
#include "BluettiProtocol.h"

#ifndef BLUETTI_MAX_UNITS
  #define BLUETTI_MAX_UNITS  4
#endif

// Diagnostic status lines on the serial port (set BLUETTI_DEBUG=1 in platformio.ini)
#if defined(BLUETTI_DEBUG) && BLUETTI_DEBUG
  #define BLT_LOG(...)  Serial.printf(__VA_ARGS__)
#else
  #define BLT_LOG(...)  do { } while (0)
#endif

#define BLUETTI_NAME_MAX  32

struct BluettiData {
  uint8_t  soc;              // battery %
  bool     have_time;        // time remaining read (EL30V2 only)
  uint16_t time_left_min;    // time remaining, minutes
  uint16_t dc_in_w;          // DC input (solar) watts
  uint16_t ac_in_w;
  uint16_t dc_out_w;
  uint16_t ac_out_w;
#if defined(BLUETTI_EB3A) && BLUETTI_EB3A
  uint16_t dc_in_v_centi;    // DC input (solar) volts x 100
#endif
};

class NimBLEClient;
class NimBLERemoteCharacteristic;

class BluettiPoller {
public:
  BluettiPoller();

  // units_csv: comma separated serial numbers or full Bluetooth names,
  // e.g. "2603110572960,2534001795459". A unit matches when its Bluetooth name ends with the entry.
  void begin(const char* units_csv, uint32_t interval_secs);

  int getCount() const { return _count; }

  // Bluetooth name once the unit has been found, otherwise the configured entry
  const char* getName(int idx) const { return _units[idx].found_name[0] ? _units[idx].found_name : _units[idx].match; }

  // Copies the latest reading for unit 'idx'. Returns false if it has never been read.
  bool getReading(int idx, BluettiData& dest, uint32_t& age_secs);

private:
  struct Unit {
    char        match[BLUETTI_NAME_MAX];        // configured entry
    char        found_name[BLUETTI_NAME_MAX];   // Bluetooth name, once seen
    bool        have_addr;
    uint8_t     addr[6];
    uint8_t     addr_type;
    bool        is_el30v2;
    bool        have_reading;
    uint32_t    read_millis;
    BluettiData data;
  };

  Unit     _units[BLUETTI_MAX_UNITS];
  int      _count;
  uint32_t _interval_ms;
  SemaphoreHandle_t _lock;       // guards have_reading, read_millis, data, found_name
  QueueHandle_t     _rx_queue;   // notifications, from the BLE host task to the polling task
  NimBLEClient*     _client;
  BluettiSession*   _session;

  static void taskEntry(void* arg);
  void run();
  void scanForMissing();
  bool pollOne(Unit& u);
  bool processNotifications(NimBLERemoteCharacteristic* tx, uint32_t wait_ms);
  bool readRegister(NimBLERemoteCharacteristic* tx, uint16_t addr, uint16_t& value);
  bool write(NimBLERemoteCharacteristic* tx, const uint8_t* data, size_t len);
  void onNotify(const uint8_t* data, size_t len);
};
