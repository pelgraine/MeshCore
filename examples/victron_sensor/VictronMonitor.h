#pragma once

// Listens for Victron "Instant Readout" advertisements in a background FreeRTOS task,
// so a missing or busy shunt never holds up the mesh loop. No Bluetooth connection is
// made -- the SmartShunt broadcasts its readings, we only receive them.
//
// Each cycle it scans for VTR_SCAN_SECS seconds, picks out the Victron advertisements
// (manufacturer data, company id 0x02E1, readout type 0x02), checks the Instant Readout
// key of each configured device against them, decrypts one reading per device and
// stores it.

#include <Arduino.h>
#include "VictronReadout.h"

// Diagnostic status lines on the serial port (set VTR_DEBUG=1 in platformio.ini)
#if defined(VTR_DEBUG) && VTR_DEBUG
  #define VTR_LOG(...)  Serial.printf(__VA_ARGS__)
#else
  #define VTR_LOG(...)  do { } while (0)
#endif

#ifndef VTR_MAX_DEVICES
  #define VTR_MAX_DEVICES  4
#endif

#define VTR_NAME_MAX  32

class VictronMonitor {
public:
  VictronMonitor();

  // devices_csv: comma separated "name:KEY" entries. 'name' is the device's Bluetooth
  // name as shown by VictronConnect (e.g. "SmartShunt HQ2301RCTU4"), 'KEY' is the 32
  // hex character Instant Readout key from VictronConnect -> Product info -> Instant
  // readout details. An entry written as ":KEY" (no name) accepts any battery monitor
  // advertisement that decrypts with that key.
  void begin(const char* devices_csv, uint32_t scan_secs, uint32_t cycle_secs);

  int getCount() const { return _count; }
  const char* getName(int idx) const { return _devs[idx].name; }

  // Copies the latest reading for device 'idx'. Returns false if none has been received.
  bool getReading(int idx, VictronReading& dest, uint32_t& age_secs);

private:
  struct Device {
    char     name[VTR_NAME_MAX];
    uint8_t  key[VTR_KEY_SIZE];
    bool     any_name;        // configured as ":KEY" -- match on the key alone
    bool     have_iv;
    uint16_t iv;              // IV of the last stored advertisement, to spot repeats
    bool     have_reading;
    uint32_t read_millis;
    VictronReading data;
  };

  Device   _devs[VTR_MAX_DEVICES];
  int      _count;
  uint32_t _scan_ms;
  uint32_t _cycle_ms;
  SemaphoreHandle_t _lock;    // guards have_reading, read_millis, data, have_iv, iv

  static void taskEntry(void* arg);
  void run();
  void scanOnce();
  bool handleManufacturerData(const uint8_t* mfg, size_t mfg_len, const char* name, bool have_name, int rssi);
};
