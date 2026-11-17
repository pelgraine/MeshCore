#include "VictronMonitor.h"
#include <NimBLEDevice.h>
#include <AES.h>
#include <string.h>

#ifndef VTR_TASK_STACK
  #define VTR_TASK_STACK        8192   // same as the Wattcycle poller's Bluetooth task
#endif
#ifndef VTR_MAX_SCAN_RESULTS
  #define VTR_MAX_SCAN_RESULTS  60    // advertisements kept from one scan
#endif

VictronMonitor::VictronMonitor() {
  _count = 0;
  _scan_ms = 10000;
  _cycle_ms = 60000;
  _lock = NULL;
  memset(_devs, 0, sizeof(_devs));
}

// Copies 'src' to 'dst', without the leading and trailing spaces of a platformio.ini value.
static size_t trimmedCopy(char* dst, size_t dst_size, const char* src, size_t len) {
  while (len > 0 && *src == ' ') { src++; len--; }
  while (len > 0 && src[len - 1] == ' ') len--;
  if (len >= dst_size) len = dst_size - 1;
  memcpy(dst, src, len);
  dst[len] = 0;
  return len;
}

void VictronMonitor::begin(const char* devices_csv, uint32_t scan_secs, uint32_t cycle_secs) {
  _scan_ms = scan_secs * 1000;
  _cycle_ms = cycle_secs * 1000;

  _count = 0;
  const char* sp = devices_csv;
  while (sp && *sp && _count < VTR_MAX_DEVICES) {
    const char* ep = strchr(sp, ',');
    size_t entry_len = ep ? (size_t)(ep - sp) : strlen(sp);
    const char* colon = (const char*)memchr(sp, ':', entry_len);

    if (colon == NULL) {
      VTR_LOG("VTR: entry '%.*s' has no ':KEY' part, ignored\n", (int)entry_len, sp);
    } else {
      char name[VTR_NAME_MAX];
      char key_hex[2 * VTR_KEY_SIZE + 1];
      size_t name_len = trimmedCopy(name, sizeof(name), sp, (size_t)(colon - sp));
      size_t key_len = trimmedCopy(key_hex, sizeof(key_hex), colon + 1, entry_len - (size_t)(colon - sp) - 1);

      if (key_len == 0 || !victronParseKey(key_hex, _devs[_count].key)) {
        VTR_LOG("VTR: entry for '%s': Instant Readout key must be 32 hex characters, ignored\n", name);
      } else {
        memcpy(_devs[_count].name, name, name_len + 1);
        _devs[_count].any_name = (name_len == 0);
        VTR_LOG("VTR: [%d] %s key starts 0x%02X\n", _count,
                name_len == 0 ? "(any battery monitor)" : name, _devs[_count].key[0]);
        _count++;
      }
    }
    sp = ep ? ep + 1 : NULL;
  }
  VTR_LOG("VTR: %d Victron device(s) configured\n", _count);
  if (_count == 0) return;   // nothing to listen for

  _lock = xSemaphoreCreateMutex();
  // core 0, alongside the BLE host; the mesh loop runs on core 1
  xTaskCreatePinnedToCore(taskEntry, "vtr_scan", VTR_TASK_STACK, this, 1, NULL, 0);
}

bool VictronMonitor::getReading(int idx, VictronReading& dest, uint32_t& age_secs) {
  if (idx < 0 || idx >= _count || _lock == NULL) return false;
  bool have = false;
  xSemaphoreTake(_lock, portMAX_DELAY);
  Device& d = _devs[idx];
  if (d.have_reading) {
    dest = d.data;
    age_secs = (millis() - d.read_millis) / 1000;
    have = true;
  }
  xSemaphoreGive(_lock);
  return have;
}

void VictronMonitor::taskEntry(void* arg) {
  ((VictronMonitor*)arg)->run();
}

void VictronMonitor::run() {
  NimBLEDevice::init("");

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);   // a device's name is often only in the scan response
  scan->setInterval(100);
  scan->setWindow(99);
  scan->setMaxResults(VTR_MAX_SCAN_RESULTS);
  VTR_LOG("VTR: Bluetooth receiver started, %lu s scan every %lu s\n",
          (unsigned long)(_scan_ms / 1000), (unsigned long)(_cycle_ms / 1000));

  for (;;) {
    scanOnce();
    vTaskDelay(pdMS_TO_TICKS(_cycle_ms));
  }
}

void VictronMonitor::scanOnce() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  NimBLEScanResults results = scan->start(_scan_ms / 1000, false);
  int victron_seen = 0, readings = 0;

  for (int j = 0; j < results.getCount(); j++) {
    NimBLEAdvertisedDevice dev = results.getDevice(j);
    uint8_t mfg_count = dev.getManufacturerDataCount();
    if (mfg_count == 0) continue;

    std::string adv_name;
    if (dev.haveName()) adv_name = dev.getName();

    for (int k = 0; k < mfg_count; k++) {
      std::string mfg = dev.getManufacturerData(k);
      if (mfg.size() < 9) continue;   // 2 byte company id + 7 byte header minimum
      victron_seen++;
      if (handleManufacturerData((const uint8_t*)mfg.data(), mfg.size(),
                                 adv_name.c_str(), !adv_name.empty(), dev.getRSSI())) {
        readings++;
      }
    }
  }
  scan->clearResults();
  VTR_LOG("VTR: scan saw %d Victron advertisement(s), %d new reading(s)\n", victron_seen, readings);
}

bool VictronMonitor::handleManufacturerData(const uint8_t* mfg, size_t mfg_len, const char* name, bool have_name, int rssi) {
  VictronContainer c;
  if (!victronSplitManufacturerData(mfg, mfg_len, c)) return false;

  if (c.readout_type != VTR_TYPE_BATTERY_MONITOR) {
    VTR_LOG("VTR: '%s' (model 0x%04X) sends readout type 0x%02X, not a battery monitor - skipped\n",
            have_name ? name : "(unnamed)", c.model_id, c.readout_type);
    return false;
  }

  for (int i = 0; i < _count; i++) {
    Device& d = _devs[i];
    // Matching rule: the Bluetooth name when the advertisement we are holding carries
    // one, the key alone when it does not. NimBLE rebuilds a device's payload from
    // scratch on each new advertisement (NimBLEAdvertisedDevice::setPayload with
    // append=false), so a name that arrived in a scan response can be gone again by the
    // end of the scan. With more than one configured device an unnamed advertisement is
    // attributed by its key check byte, which is a single byte -- if two devices' keys
    // start with the same byte their readings could be swapped.
    bool by_name = have_name && strcmp(name, d.name) == 0;
    bool by_key_only = d.any_name || !have_name;
    if (!by_name && !by_key_only) continue;

    if (!victronKeyMatches(c, d.key)) {
      VTR_LOG("VTR: '%s' key check byte 0x%02X does not match this device's key (0x%02X) - wrong key in platformio.ini?\n",
              d.name, c.encrypted[0], d.key[0]);
      continue;
    }
    if (d.have_iv && c.iv == d.iv) {
      VTR_LOG("VTR: '%s' advertisement IV 0x%04X already stored, skipping\n", d.name, c.iv);
      continue;
    }

    uint8_t block[16], keystream[16];
    victronCounterBlock(c.iv, block);
    AES128 aes;
    if (!aes.setKey(d.key, VTR_KEY_SIZE)) {
      VTR_LOG("VTR: '%s' AES key rejected by the crypto library\n", d.name);
      continue;
    }
    aes.encryptBlock(keystream, block);   // one keystream block covers the 15 cipher bytes

    VictronReading reading;
    if (!victronDecodeBatteryMonitor(c, keystream, reading)) {
      VTR_LOG("VTR: '%s' payload of %u bytes could not be decoded as a battery monitor reading\n",
              d.name, (unsigned)c.encrypted_len);
      continue;
    }

    if (!by_name && !d.any_name) {
      VTR_LOG("VTR:   matched '%s' on the key alone: this advertisement carried no name\n", d.name);
    }

    char soc_txt[16], volt_txt[16], cur_txt[16], ah_txt[16];
    if (reading.have_soc)          snprintf(soc_txt, sizeof(soc_txt), "%.1f%%", (double)reading.soc_pct);
    else                           snprintf(soc_txt, sizeof(soc_txt), "no SoC");
    if (reading.have_voltage)      snprintf(volt_txt, sizeof(volt_txt), "%.2f V", (double)reading.voltage_v);
    else                           snprintf(volt_txt, sizeof(volt_txt), "no voltage");
    if (reading.have_current)      snprintf(cur_txt, sizeof(cur_txt), "%.3f A", (double)reading.current_a);
    else                           snprintf(cur_txt, sizeof(cur_txt), "no current");
    if (reading.have_consumed_ah)  snprintf(ah_txt, sizeof(ah_txt), "%.1f Ah", (double)reading.consumed_ah);
    else                           snprintf(ah_txt, sizeof(ah_txt), "no Ah");

    VTR_LOG("VTR: '%s' %s (model 0x%04X, IV 0x%04X, RSSI %d): %s, %s, %s, %s consumed, aux mode %u, alarm 0x%04X\n",
            d.name, victronModelName(c.model_id), c.model_id, c.iv, rssi,
            soc_txt, volt_txt, cur_txt, ah_txt, (unsigned)reading.aux_mode, reading.alarm);
    if (reading.have_aux_temp_c) {
      VTR_LOG("VTR:   aux temperature %.1f C\n", (double)reading.aux_temp_c);
    }

    xSemaphoreTake(_lock, portMAX_DELAY);
    d.data = reading;
    d.read_millis = millis();
    d.have_reading = true;
    d.have_iv = true;
    d.iv = c.iv;
    xSemaphoreGive(_lock);
    return true;
  }
  return false;
}
