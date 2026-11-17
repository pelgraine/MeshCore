#pragma once

// Decodes Victron "Instant Readout" Bluetooth advertisements -- the unconnected
// broadcasts VictronConnect turns on under Product info -> "Instant readout via
// Bluetooth", which is what a SmartShunt sends.
//
// Everything in the layout, the encryption and the bit fields below is taken from the
// open-source decoder keshavdv/victron-ble (MIT license), as read from github.com at
// commit c721522 (dated 16 Jan 2026):
//   victron_ble/scanner.py                       how the advertisement is picked out
//   victron_ble/devices/base.py                  container split, AES decrypt, bit reader
//   victron_ble/devices/battery_monitor.py       the battery-monitor bit fields
//   tests/test_battery_monitor.py                a known-good SmartShunt sample
//
// Only the battery-monitor readout (type 0x02), which is what the SmartShunt sends, is
// decoded here. Other Victron device types (chargers, BatteryProtect, Battery Sense)
// are recognised but not parsed.
//
// Advertisement manufacturer data (BLE AD type 0xFF), as NimBLE hands it over
// (NimBLEAdvertisedDevice::getManufacturerData() returns the field value, which starts
// with the 2-byte company id):
//
//   company id      2 bytes, little endian: 0xE1 0x02  (0x02E1 = Victron)
//   prefix          2 bytes, little endian, low byte 0x10
//   model id        2 bytes, little endian, e.g. 0xA389 = SmartShunt 500A/50mV
//   readout type    1 byte, 0x02 = battery monitor
//   IV              2 bytes, little endian
//   encrypted part  16 bytes for a SmartShunt: [0] is a copy of the key's first byte,
//                                              [1..15] is 15 bytes of ciphertext
//
// The ciphertext is AES-128 in counter mode; the counter block is the IV written as a
// 128-bit little-endian integer. Victron's 15 cipher bytes need a single keystream
// block, so the keystream is just AES-128 ECB of that counter block. The key is the
// 16-byte (32 hex character) Instant Readout key shown by VictronConnect.

#include <stdint.h>
#include <stddef.h>

#define VTR_KEY_SIZE              16
#define VTR_TYPE_BATTERY_MONITOR  0x02
#define VTR_CIPHER_LEN            16   // check byte + 15 cipher bytes, the SmartShunt size

// Auxiliary input mode, from victron_ble/devices/battery_monitor.py AuxMode
enum VictronAuxMode {
  VTR_AUX_STARTER_VOLTAGE = 0,
  VTR_AUX_MIDPOINT_VOLTAGE = 1,
  VTR_AUX_TEMPERATURE = 2,
  VTR_AUX_DISABLED = 3
};

struct VictronContainer {
  uint16_t prefix;
  uint16_t model_id;
  uint8_t  readout_type;
  uint16_t iv;
  const uint8_t* encrypted;   // [0] = key check byte, then the ciphertext
  uint16_t encrypted_len;
};

struct VictronReading {
  bool     have_voltage;
  float    voltage_v;
  bool     have_current;
  float    current_a;          // Victron's sign: positive = charging
  bool     have_soc;
  float    soc_pct;
  bool     have_consumed_ah;
  float    consumed_ah;        // as victron-ble reports it: negative = amp-hours consumed
  bool     have_remaining_mins;
  uint16_t remaining_mins;
  bool     have_aux_temp_c;    // only when aux_mode == VTR_AUX_TEMPERATURE
  float    aux_temp_c;
  uint16_t alarm;              // Victron alarm bit field, 0 = no alarm
  uint8_t  aux_mode;           // VictronAuxMode
  uint16_t aux_raw;            // raw 16 bit auxiliary input value (10 mV or 0.01 K steps)
};

// 'mfg'/'len' are the whole manufacturer data field, company id included.
// Returns false if it is not a Victron Instant Readout advertisement, or is too short.
bool victronSplitManufacturerData(const uint8_t* mfg, size_t len, VictronContainer& out);

// The first encrypted byte is a copy of the key's first byte; this is how victron-ble
// detects that the wrong key was supplied.
bool victronKeyMatches(const VictronContainer& c, const uint8_t key[VTR_KEY_SIZE]);

// Fills 'block' with the AES counter block for this IV (IV as 128-bit little endian).
void victronCounterBlock(uint16_t iv, uint8_t block[16]);

// XORs the ciphertext with 'keystream' (the first AES keystream block) and decodes the
// battery-monitor bit fields. Returns false for other readout types or payload lengths.
bool victronDecodeBatteryMonitor(const VictronContainer& c, const uint8_t keystream[16], VictronReading& out);

// Reads 32 hex characters into 'key'. Returns false if the text is not exactly that.
bool victronParseKey(const char* hex, uint8_t key[VTR_KEY_SIZE]);

// Battery-monitor model ids, for the debug log lines only. Returns "" if unknown.
const char* victronModelName(uint16_t model_id);
