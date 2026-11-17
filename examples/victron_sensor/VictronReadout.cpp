#include "VictronReadout.h"
#include <string.h>

#define VTR_COMPANY_ID   0x02E1
#define VTR_PREFIX_BYTE  0x10
#define VTR_HEADER_LEN   7            // prefix(2) + model id(2) + readout type(1) + IV(2)

// Sentinel values used by Victron for "this field is not available", from
// victron_ble/devices/battery_monitor.py. Note that victron-ble tests the *signed*
// value against these (so 0x7FFF and 0x3FFFFF slip through there as -1); here the raw
// unsigned value is tested, which is what the sentinels are.
#define VTR_SENTINEL_REMAINING_MINS  0xFFFFu
#define VTR_SENTINEL_VOLTAGE         0x7FFFu
#define VTR_SENTINEL_CURRENT         0x3FFFFFu
#define VTR_SENTINEL_CONSUMED_AH     0xFFFFFu
#define VTR_SENTINEL_SOC             0x3FFu

static uint16_t readLE16(const uint8_t* p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

bool victronSplitManufacturerData(const uint8_t* mfg, size_t len, VictronContainer& out) {
  if (len < (size_t)VTR_HEADER_LEN + 1) return false;
  if (readLE16(mfg) != VTR_COMPANY_ID) return false;

  const uint8_t* p = mfg + 2;         // past the company id
  out.prefix       = readLE16(p);
  out.model_id     = readLE16(p + 2);
  out.readout_type = p[4];
  out.iv           = readLE16(p + 5);
  out.encrypted    = p + VTR_HEADER_LEN;
  out.encrypted_len = (uint16_t)(len - 2 - VTR_HEADER_LEN);

  if ((out.prefix & 0xFF) != VTR_PREFIX_BYTE) return false;   // victron-ble checks this too
  return true;
}

bool victronKeyMatches(const VictronContainer& c, const uint8_t key[VTR_KEY_SIZE]) {
  return c.encrypted_len >= 1 && c.encrypted[0] == key[0];
}

void victronCounterBlock(uint16_t iv, uint8_t block[16]) {
  memset(block, 0, 16);
  block[0] = (uint8_t)(iv & 0xFF);
  block[1] = (uint8_t)(iv >> 8);
}

// victron-ble's BitReader: fields are packed LSB first, byte by byte.
struct BitReader {
  const uint8_t* data;
  uint32_t bit;
};

static uint32_t readBits(BitReader& r, int num_bits) {
  uint32_t value = 0;
  for (int i = 0; i < num_bits; i++) {
    value |= (uint32_t)((r.data[r.bit >> 3] >> (r.bit & 7)) & 1) << i;
    r.bit++;
  }
  return value;
}

bool victronDecodeBatteryMonitor(const VictronContainer& c, const uint8_t keystream[16], VictronReading& out) {
  if (c.readout_type != VTR_TYPE_BATTERY_MONITOR) return false;
  if (c.encrypted_len != VTR_CIPHER_LEN) return false;

  // 15 cipher bytes, PKCS#7 padded by victron-ble to one 16 byte block. Bit 117 is the
  // last bit the fields below use, so the padded byte is never read.
  uint8_t plain[16];
  for (int i = 0; i < 15; i++) {
    plain[i] = (uint8_t)(c.encrypted[1 + i] ^ keystream[i]);
  }
  plain[15] = (uint8_t)(0x01 ^ keystream[15]);

  BitReader r = { plain, 0 };
  uint32_t remaining_mins = readBits(r, 16);
  uint32_t raw_voltage    = readBits(r, 16);
  uint32_t alarm          = readBits(r, 16);
  uint32_t aux            = readBits(r, 16);
  uint32_t aux_mode       = readBits(r, 2);
  uint32_t raw_current    = readBits(r, 22);
  uint32_t consumed       = readBits(r, 20);
  uint32_t raw_soc        = readBits(r, 10);

  memset(&out, 0, sizeof(out));
  out.alarm    = (uint16_t)alarm;
  out.aux_mode = (uint8_t)aux_mode;
  out.aux_raw  = (uint16_t)aux;

  out.have_remaining_mins = (remaining_mins != VTR_SENTINEL_REMAINING_MINS);
  if (out.have_remaining_mins) out.remaining_mins = (uint16_t)remaining_mins;

  out.have_voltage = (raw_voltage != VTR_SENTINEL_VOLTAGE);
  if (out.have_voltage) out.voltage_v = (float)(int16_t)raw_voltage / 100.0f;

  out.have_current = (raw_current != VTR_SENTINEL_CURRENT);
  if (out.have_current) {
    uint32_t sign_extended = raw_current | ((raw_current & 0x200000u) ? 0xFFC00000u : 0);
    out.current_a = (float)(int32_t)sign_extended / 1000.0f;
  }

  out.have_consumed_ah = (consumed != VTR_SENTINEL_CONSUMED_AH);
  if (out.have_consumed_ah) out.consumed_ah = -(float)consumed / 10.0f;

  out.have_soc = (raw_soc != VTR_SENTINEL_SOC);
  if (out.have_soc) out.soc_pct = (float)raw_soc / 10.0f;

  if (aux_mode == VTR_AUX_TEMPERATURE) {
    out.have_aux_temp_c = true;
    out.aux_temp_c = (float)aux / 100.0f - 273.15f;   // the aux value is kelvin in 0.01 K steps
  }
  return true;
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool victronParseKey(const char* hex, uint8_t key[VTR_KEY_SIZE]) {
  for (int i = 0; i < VTR_KEY_SIZE; i++) {
    int hi = hexNibble(hex[i * 2]);
    int lo = hexNibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    key[i] = (uint8_t)((hi << 4) | lo);
  }
  return hex[VTR_KEY_SIZE * 2] == 0;
}

const char* victronModelName(uint16_t model_id) {
  // Battery-monitor ids from MODEL_ID_MAPPING in victron_ble/devices/base.py
  switch (model_id) {
    case 0xA380: return "BMV-710 Smart";
    case 0xA381: return "BMV-712 Smart";
    case 0xA382: return "BMV-710H Smart";
    case 0xA383: return "BMV-712 Smart";
    case 0xA389: return "SmartShunt 500A/50mV";
    case 0xA38A: return "SmartShunt 1000A/50mV";
    case 0xA38B: return "SmartShunt 2000A/50mV";
    case 0xA38C: return "SmartShunt IP67 500A/50mV";
    case 0xA38D: return "SmartShunt IP67 1000A/50mV";
    case 0xA38E: return "SmartShunt IP67 2000A/50mV";
    case 0xC030: return "SmartShunt IP65 500A/50mV";
    case 0xC031: return "SmartShunt IP65 1000A/50mV";
    case 0xC032: return "SmartShunt IP65 2000A/50mV";
    case 0xC034: return "BMV-800 Smart";
    case 0xC035: return "SmartShunt IP65 500A/50mV";
    case 0xC036: return "SmartShunt IP65 1000A/50mV";
    case 0xC037: return "SmartShunt IP65 2000A/50mV";
    case 0xC038: return "SmartShunt 300A/50mV";
    default:     return "";
  }
}
