#include "TdtBmsPoller.h"
#include <NimBLEDevice.h>
#include <string.h>

#ifndef BMS_SCAN_SECS
  #define BMS_SCAN_SECS              5     // how long to scan for batteries not yet found
#endif
#ifndef BMS_CONNECT_TIMEOUT_SECS
  #define BMS_CONNECT_TIMEOUT_SECS   10
#endif
#ifndef BMS_REPLY_TIMEOUT_MS
  #define BMS_REPLY_TIMEOUT_MS       1500  // wait for each reply
#endif
#ifndef BMS_CMD_ATTEMPTS
  #define BMS_CMD_ATTEMPTS           3     // sends per command head
#endif
#ifndef BMS_TASK_STACK
  #define BMS_TASK_STACK             8192
#endif
#ifndef BMS_BLE_TX_POWER
  #define BMS_BLE_TX_POWER           ESP_PWR_LVL_P21   // highest level in the core's esp_bt.h (+21 dBm); default is +3 dBm
#endif

// esp_power_level_t index to dBm (N24 = 0 = -24 dBm, 3 dB per step)
static int bleLevelToDbm(int level) {
  return -24 + 3 * level;
}

static const char BMS_UNLOCK[] = "HiLink";   // written to 0xFFFA to unlock the BMS

TdtBmsPoller::TdtBmsPoller() {
  _count = 0;
  _interval_ms = 60000;
  _lock = NULL;
  _rx_mux = portMUX_INITIALIZER_UNLOCKED;
  _rx_bytes = 0;
  _client = NULL;
  memset(_batts, 0, sizeof(_batts));
}

void TdtBmsPoller::begin(const char* names_csv, uint32_t interval_secs) {
  _interval_ms = interval_secs * 1000;

  // split the comma separated name list
  _count = 0;
  const char* sp = names_csv;
  while (sp && *sp && _count < BMS_MAX_BATTERIES) {
    const char* ep = strchr(sp, ',');
    size_t n = ep ? (size_t)(ep - sp) : strlen(sp);
    while (n > 0 && *sp == ' ') { sp++; n--; }          // trim leading spaces
    while (n > 0 && sp[n - 1] == ' ') n--;              // trim trailing spaces
    if (n > 0 && n < BMS_NAME_MAX) {
      memcpy(_batts[_count].name, sp, n);
      _batts[_count].name[n] = 0;
      _count++;
    }
    sp = ep ? ep + 1 : NULL;
  }
  BMS_LOG("BMS: %d battery name(s) configured\n", _count);
  for (int i = 0; i < _count; i++) {
    BMS_LOG("BMS:   [%d] '%s'\n", i, _batts[i].name);
  }
  if (_count == 0) return;   // nothing to poll

  _lock = xSemaphoreCreateMutex();
  // core 0, alongside the BLE host; the mesh loop runs on core 1
  xTaskCreatePinnedToCore(taskEntry, "bms_poll", BMS_TASK_STACK, this, 1, NULL, 0);
}

bool TdtBmsPoller::getReading(int idx, TdtBmsData& dest, uint32_t& age_secs) {
  if (idx < 0 || idx >= _count || _lock == NULL) return false;
  bool have = false;
  xSemaphoreTake(_lock, portMAX_DELAY);
  Battery& b = _batts[idx];
  if (b.have_reading) {
    dest = b.data;
    age_secs = (millis() - b.read_millis) / 1000;
    have = true;
  }
  xSemaphoreGive(_lock);
  return have;
}

void TdtBmsPoller::taskEntry(void* arg) {
  ((TdtBmsPoller*)arg)->run();
}

void TdtBmsPoller::run() {
  NimBLEDevice::init("");

  // transmit power: DEFAULT covers connecting and connections, SCAN covers scan requests
  esp_err_t rc_def  = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, (esp_power_level_t)BMS_BLE_TX_POWER);
  esp_err_t rc_scan = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, (esp_power_level_t)BMS_BLE_TX_POWER);
  BMS_LOG("BMS: TX power set (default rc %d, scan rc %d), now default %d dBm, scan %d dBm\n", (int)rc_def, (int)rc_scan,
          bleLevelToDbm(esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_DEFAULT)),
          bleLevelToDbm(esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_SCAN)));

  _client = NimBLEDevice::createClient();
  _client->setConnectTimeout(BMS_CONNECT_TIMEOUT_SECS);
  BMS_LOG("BMS: Bluetooth started, polling every %lu s\n", (unsigned long)(_interval_ms / 1000));

  for (;;) {
    scanForMissing();
    for (int i = 0; i < _count; i++) {
      if (_batts[i].have_addr) {
        pollOne(_batts[i]);
        vTaskDelay(pdMS_TO_TICKS(500));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(_interval_ms));
  }
}

void TdtBmsPoller::scanForMissing() {
  bool missing = false;
  for (int i = 0; i < _count; i++) {
    if (!_batts[i].have_addr) missing = true;
  }
  if (!missing) return;

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);   // the name may only be in the scan response
  scan->setInterval(100);
  scan->setWindow(99);
  BMS_LOG("BMS: scanning for %d s\n", BMS_SCAN_SECS);
  NimBLEScanResults results = scan->start(BMS_SCAN_SECS, false);
  BMS_LOG("BMS: scan saw %d device(s)\n", results.getCount());

  for (int j = 0; j < results.getCount(); j++) {
    NimBLEAdvertisedDevice dev = results.getDevice(j);
    if (!dev.haveName()) continue;
    std::string name = dev.getName();
    for (int i = 0; i < _count; i++) {
      Battery& b = _batts[i];
      if (!b.have_addr && name == b.name) {
        NimBLEAddress addr = dev.getAddress();
        memcpy(b.addr, addr.getNative(), 6);
        b.addr_type = addr.getType();
        b.have_addr = true;
        BMS_LOG("BMS: found '%s' at %s (address type %u, RSSI %d)\n", b.name, addr.toString().c_str(),
                (unsigned)b.addr_type, dev.getRSSI());
      }
    }
  }

  for (int i = 0; i < _count; i++) {
    if (!_batts[i].have_addr) {
      BMS_LOG("BMS: '%s' not seen. Named devices in this scan:\n", _batts[i].name);
      for (int j = 0; j < results.getCount(); j++) {
        NimBLEAdvertisedDevice dev = results.getDevice(j);
        if (dev.haveName()) {
          BMS_LOG("BMS:   '%s' (RSSI %d)\n", dev.getName().c_str(), dev.getRSSI());
        }
      }
      break;   // the list is the same for every missing battery
    }
  }
  scan->clearResults();
}

void TdtBmsPoller::onNotify(const uint8_t* data, size_t len) {
  portENTER_CRITICAL(&_rx_mux);
  _rx.feed(data, len);
  _rx_bytes += len;
  portEXIT_CRITICAL(&_rx_mux);
}

bool TdtBmsPoller::sendWithHead(NimBLERemoteCharacteristic* tx, uint8_t head, uint8_t cmd, uint8_t* frame, size_t& frame_len) {
  uint8_t req[TDT_CMD_FRAME_LEN];
  size_t req_len = tdtBuildCommand(req, head, cmd);

  for (int attempt = 0; attempt < BMS_CMD_ATTEMPTS; attempt++) {
    portENTER_CRITICAL(&_rx_mux);
    _rx.reset();
    portEXIT_CRITICAL(&_rx_mux);

    if (!tx->writeValue(req, req_len, true)) {   // write with response, as aiobmsble does
      BMS_LOG("BMS:   cmd 0x%02X head 0x%02X: write failed\n", cmd, head);
      return false;
    }

    uint32_t start = millis();
    while (millis() - start < BMS_REPLY_TIMEOUT_MS) {
      vTaskDelay(pdMS_TO_TICKS(20));
      bool got = false;
      portENTER_CRITICAL(&_rx_mux);
      if (_rx.isComplete()) {
        frame_len = _rx.frameLen();
        memcpy(frame, _rx.frame(), frame_len);
        _rx.reset();   // ready for the next frame
        got = true;
      }
      portEXIT_CRITICAL(&_rx_mux);
      if (got && tdtIsReplyTo(frame, frame_len, cmd)) {
        BMS_LOG("BMS:   cmd 0x%02X head 0x%02X: reply, %u bytes\n", cmd, head, (unsigned)frame_len);
        return true;
      }
      if (got) {
        BMS_LOG("BMS:   cmd 0x%02X head 0x%02X: other frame (cmd 0x%02X, error 0x%02X)\n", cmd, head,
                frame_len > 5 ? frame[5] : 0, frame_len > 4 ? frame[4] : 0);
      }
    }
    uint32_t rx_bytes;
    portENTER_CRITICAL(&_rx_mux);
    rx_bytes = _rx_bytes;
    portEXIT_CRITICAL(&_rx_mux);
    BMS_LOG("BMS:   cmd 0x%02X head 0x%02X attempt %d: no reply (notification bytes so far: %lu)\n",
            cmd, head, attempt + 1, (unsigned long)rx_bytes);
  }
  return false;
}

bool TdtBmsPoller::sendCommand(NimBLERemoteCharacteristic* tx, Battery& b, uint8_t cmd, uint8_t* frame, size_t& frame_len) {
  if (b.head != 0) {
    return sendWithHead(tx, b.head, cmd, frame, frame_len);
  }
  // command head not known yet: try the one XDZN_001_E4E7 uses, then aiobmsble's default
  const uint8_t heads[] = { TDT_HEAD_ALT, TDT_HEAD_DEFAULT };
  for (size_t i = 0; i < sizeof(heads); i++) {
    if (sendWithHead(tx, heads[i], cmd, frame, frame_len)) {
      b.head = heads[i];
      return true;
    }
  }
  return false;
}

bool TdtBmsPoller::pollOne(Battery& b) {
  ble_addr_t raw;
  memcpy(raw.val, b.addr, 6);
  raw.type = b.addr_type;
  NimBLEAddress addr(raw);

  BMS_LOG("BMS: connecting to '%s'\n", b.name);
  if (!_client->connect(addr, true)) {
    int err = _client->getLastError();
    BMS_LOG("BMS: connect failed (error %d %s), will scan again\n", err, NimBLEUtils::returnCodeToString(err));
    b.have_addr = false;   // scan for it again next cycle
    return false;
  }
  BMS_LOG("BMS: connected (MTU %u)\n", (unsigned)_client->getMTU());

  bool ok = false;
  uint8_t frame[TDT_MAX_FRAME];
  size_t frame_len = 0;

  do {
    NimBLERemoteService* svc = _client->getService(NimBLEUUID((uint16_t)0xFFF0));
    if (svc == NULL) {
      BMS_LOG("BMS: service FFF0 not found\n");
      break;
    }
    NimBLERemoteCharacteristic* rx = svc->getCharacteristic(NimBLEUUID((uint16_t)0xFFF1));
    NimBLERemoteCharacteristic* tx = svc->getCharacteristic(NimBLEUUID((uint16_t)0xFFF2));
    if (rx == NULL || tx == NULL) {
      BMS_LOG("BMS: characteristic missing (FFF1 %s, FFF2 %s)\n", rx ? "ok" : "missing", tx ? "ok" : "missing");
      break;
    }

    // unlock step (aiobmsble skips it when the characteristic is missing)
    NimBLERemoteCharacteristic* cfg = svc->getCharacteristic(NimBLEUUID((uint16_t)0xFFFA));
    if (cfg != NULL) {
      bool wrote = cfg->writeValue((const uint8_t*)BMS_UNLOCK, strlen(BMS_UNLOCK), true);
      NimBLEAttValue v = cfg->readValue();   // aiobmsble reads it back (1 = unlocked); the result is not needed
      BMS_LOG("BMS: unlock write %s, read back %u byte(s), first 0x%02X\n", wrote ? "ok" : "failed",
              (unsigned)v.size(), v.size() > 0 ? v.data()[0] : 0);
    } else {
      BMS_LOG("BMS: no FFFA characteristic, unlock skipped\n");
    }

    portENTER_CRITICAL(&_rx_mux);
    _rx.reset();
    _rx_bytes = 0;
    portEXIT_CRITICAL(&_rx_mux);
    if (!rx->subscribe(true, [this](NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool is_notify) {
          onNotify(data, len);
        }, true)) {
      BMS_LOG("BMS: subscribe to FFF1 failed\n");
      break;
    }
    BMS_LOG("BMS: subscribed to FFF1\n");

    // device info first, as aiobmsble does; it also gives the field layout (firmware 1.x or not)
    if (!sendCommand(tx, b, TDT_CMD_INFO, frame, frame_len)) {
      BMS_LOG("BMS: no reply to device info (0x92)\n");
      break;
    }
    bool fw_v1 = tdtIsFirmwareV1(frame, frame_len);
    BMS_LOG("BMS: device info ok, command head 0x%02X, firmware 1.x layout: %s\n", b.head, fw_v1 ? "yes" : "no");

    if (!sendCommand(tx, b, TDT_CMD_STATUS, frame, frame_len)) {
      BMS_LOG("BMS: no reply to status (0x8C)\n");
      break;
    }
    TdtBmsData d;
    if (!tdtDecodeStatus(frame, frame_len, fw_v1, d)) {
      BMS_LOG("BMS: status reply could not be decoded (%u bytes)\n", (unsigned)frame_len);
      break;
    }
    BMS_LOG("BMS: '%s' %u%%, %.2f V, %.1f A, %.1f Ah, avg temp %.1f C\n", b.name, (unsigned)d.soc,
            d.voltage, d.current, d.remaining_ah, d.avg_temp_c);

    xSemaphoreTake(_lock, portMAX_DELAY);
    b.data = d;
    b.read_millis = millis();
    b.have_reading = true;
    xSemaphoreGive(_lock);
    ok = true;
  } while (0);

  _client->disconnect();
  uint32_t start = millis();
  while (_client->isConnected() && millis() - start < 3000) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  BMS_LOG("BMS: disconnected%s\n", _client->isConnected() ? " (still connected after 3 s)" : "");
  return ok;
}
