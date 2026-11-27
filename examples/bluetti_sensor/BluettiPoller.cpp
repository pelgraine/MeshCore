#include "BluettiPoller.h"
#include <NimBLEDevice.h>
#include <esp_random.h>
#include <string.h>

#ifndef BLUETTI_SCAN_SECS
  #define BLUETTI_SCAN_SECS              5      // how long to scan for units not yet found
#endif
#ifndef BLUETTI_CONNECT_TIMEOUT_SECS
  #define BLUETTI_CONNECT_TIMEOUT_SECS   10
#endif
#ifndef BLUETTI_HANDSHAKE_TIMEOUT_MS
  #define BLUETTI_HANDSHAKE_TIMEOUT_MS   20000  // key exchange
#endif
#ifndef BLUETTI_REPLY_TIMEOUT_MS
  #define BLUETTI_REPLY_TIMEOUT_MS       5000   // each register read, as the library waits
#endif
#ifndef BLUETTI_TASK_STACK
  #define BLUETTI_TASK_STACK             12288  // mbedTLS elliptic curve maths needs room
#endif
#ifndef BLUETTI_BLE_TX_POWER
  #define BLUETTI_BLE_TX_POWER           ESP_PWR_LVL_P21   // highest level in the core's esp_bt.h (+21 dBm); default is +3 dBm
#endif

#define NOTIFY_CHUNK   244   // bytes per queue item; longer notifications are split
#define NOTIFY_QUEUE   16

#if defined(BLUETTI_EB3A) && BLUETTI_EB3A
// EB3A registers (bluetti-bt-lib 0.1.8 base_device_v1.py, eb3a.py)
#define REG_BATTERY_SOC      43
#define REG_DC_INPUT_POWER   36
#define REG_AC_INPUT_POWER   37
#define REG_AC_OUTPUT_POWER  38
#define REG_DC_OUTPUT_POWER  39
#define REG_DC_INPUT_VOLTAGE 86    // hundredths of a volt
#else
// Registers (bluetti-bt-lib 0.1.8 base_device_v2.py, el30v2.py, pr30v2.py)
#define REG_BATTERY_SOC      102
#define REG_TIME_REMAINING   104   // EL30V2 only, minutes
#define REG_DC_OUTPUT_POWER  140
#define REG_AC_OUTPUT_POWER  142
#define REG_DC_INPUT_POWER   144
#define REG_AC_INPUT_POWER   146
#endif

// Characteristics (bluetti-bt-lib const.py)
static const NimBLEUUID NOTIFY_UUID((uint16_t)0xFF01);
static const NimBLEUUID WRITE_UUID((uint16_t)0xFF02);

struct NotifyItem {
  uint16_t len;
  uint8_t  data[NOTIFY_CHUNK];
};

int bluettiRandom(void* ctx, unsigned char* out, size_t len) {
  esp_fill_random(out, len);   // hardware random generator (true random while Bluetooth is on)
  return 0;
}

// esp_power_level_t index to dBm (N24 = 0 = -24 dBm, 3 dB per step)
static int bleLevelToDbm(int level) {
  return -24 + 3 * level;
}

BluettiPoller::BluettiPoller() {
  _count = 0;
  _interval_ms = 60000;
  _lock = NULL;
  _rx_queue = NULL;
  _client = NULL;
  _session = NULL;
  memset(_units, 0, sizeof(_units));
}

void BluettiPoller::begin(const char* units_csv, uint32_t interval_secs) {
  _interval_ms = interval_secs * 1000;

  // split the comma separated list
  _count = 0;
  const char* sp = units_csv;
  while (sp && *sp && _count < BLUETTI_MAX_UNITS) {
    const char* ep = strchr(sp, ',');
    size_t n = ep ? (size_t)(ep - sp) : strlen(sp);
    while (n > 0 && *sp == ' ') { sp++; n--; }          // trim leading spaces
    while (n > 0 && sp[n - 1] == ' ') n--;              // trim trailing spaces
    if (n > 0 && n < BLUETTI_NAME_MAX) {
      memcpy(_units[_count].match, sp, n);
      _units[_count].match[n] = 0;
      _count++;
    }
    sp = ep ? ep + 1 : NULL;
  }
  BLT_LOG("BLUETTI: %d unit(s) configured\n", _count);
  for (int i = 0; i < _count; i++) {
    BLT_LOG("BLUETTI:   [%d] '%s'\n", i, _units[i].match);
  }
  if (_count == 0) return;   // nothing to poll

  _lock = xSemaphoreCreateMutex();
  _rx_queue = xQueueCreate(NOTIFY_QUEUE, sizeof(NotifyItem));
  // core 0, alongside the BLE host; the mesh loop runs on core 1
  xTaskCreatePinnedToCore(taskEntry, "bluetti_poll", BLUETTI_TASK_STACK, this, 1, NULL, 0);
}

bool BluettiPoller::getReading(int idx, BluettiData& dest, uint32_t& age_secs) {
  if (idx < 0 || idx >= _count || _lock == NULL) return false;
  bool have = false;
  xSemaphoreTake(_lock, portMAX_DELAY);
  Unit& u = _units[idx];
  if (u.have_reading) {
    dest = u.data;
    age_secs = (millis() - u.read_millis) / 1000;
    have = true;
  }
  xSemaphoreGive(_lock);
  return have;
}

void BluettiPoller::taskEntry(void* arg) {
  ((BluettiPoller*)arg)->run();
}

void BluettiPoller::run() {
  NimBLEDevice::init("");
  NimBLEDevice::setMTU(517);   // the key exchange messages are up to 146 bytes

  // transmit power: DEFAULT covers connecting and connections, SCAN covers scan requests
  esp_err_t rc_def  = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, (esp_power_level_t)BLUETTI_BLE_TX_POWER);
  esp_err_t rc_scan = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, (esp_power_level_t)BLUETTI_BLE_TX_POWER);
  BLT_LOG("BLUETTI: TX power set (default rc %d, scan rc %d), now default %d dBm, scan %d dBm\n", (int)rc_def, (int)rc_scan,
          bleLevelToDbm(esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_DEFAULT)),
          bleLevelToDbm(esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_SCAN)));

  _session = new BluettiSession();
  _client = NimBLEDevice::createClient();
  _client->setConnectTimeout(BLUETTI_CONNECT_TIMEOUT_SECS);
  BLT_LOG("BLUETTI: Bluetooth started, polling every %lu s\n", (unsigned long)(_interval_ms / 1000));

  for (;;) {
    scanForMissing();
    for (int i = 0; i < _count; i++) {
      if (_units[i].have_addr) {
        pollOne(_units[i]);
        vTaskDelay(pdMS_TO_TICKS(500));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(_interval_ms));
  }
}

static bool nameMatches(const std::string& name, const char* entry) {
  size_t n = strlen(entry);
  return name.size() >= n && name.compare(name.size() - n, n, entry) == 0;
}

void BluettiPoller::scanForMissing() {
  bool missing = false;
  for (int i = 0; i < _count; i++) {
    if (!_units[i].have_addr) missing = true;
  }
  if (!missing) return;

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);   // the name may only be in the scan response
  scan->setInterval(100);
  scan->setWindow(99);
  BLT_LOG("BLUETTI: scanning for %d s\n", BLUETTI_SCAN_SECS);
  NimBLEScanResults results = scan->start(BLUETTI_SCAN_SECS, false);
  BLT_LOG("BLUETTI: scan saw %d device(s)\n", results.getCount());

  for (int j = 0; j < results.getCount(); j++) {
    NimBLEAdvertisedDevice dev = results.getDevice(j);
    if (!dev.haveName()) continue;
    std::string name = dev.getName();
    for (int i = 0; i < _count; i++) {
      Unit& u = _units[i];
      if (!u.have_addr && nameMatches(name, u.match)) {
        NimBLEAddress addr = dev.getAddress();
        memcpy(u.addr, addr.getNative(), 6);
        u.addr_type = addr.getType();
        u.is_el30v2 = name.compare(0, 6, "EL30V2") == 0;
        xSemaphoreTake(_lock, portMAX_DELAY);
        strncpy(u.found_name, name.c_str(), BLUETTI_NAME_MAX - 1);
        u.found_name[BLUETTI_NAME_MAX - 1] = 0;
        xSemaphoreGive(_lock);
        u.have_addr = true;
        BLT_LOG("BLUETTI: found '%s' at %s (address type %u, RSSI %d)%s\n", u.found_name, addr.toString().c_str(),
                (unsigned)u.addr_type, dev.getRSSI(), u.is_el30v2 ? ", EL30V2: time remaining will be read" : "");
      }
    }
  }

  for (int i = 0; i < _count; i++) {
    if (!_units[i].have_addr) {
      BLT_LOG("BLUETTI: '%s' not seen. Named devices in this scan:\n", _units[i].match);
      for (int j = 0; j < results.getCount(); j++) {
        NimBLEAdvertisedDevice dev = results.getDevice(j);
        if (dev.haveName()) {
          BLT_LOG("BLUETTI:   '%s' (RSSI %d)\n", dev.getName().c_str(), dev.getRSSI());
        }
      }
      break;   // the list is the same for every missing unit
    }
  }
  scan->clearResults();
}

// called from the BLE host task: only copy the data, the polling task does the work
void BluettiPoller::onNotify(const uint8_t* data, size_t len) {
  NotifyItem item;
  while (len > 0) {
    size_t n = len > NOTIFY_CHUNK ? NOTIFY_CHUNK : len;
    item.len = n;
    memcpy(item.data, data, n);
    xQueueSend(_rx_queue, &item, 0);
    data += n;
    len -= n;
  }
}

bool BluettiPoller::write(NimBLERemoteCharacteristic* tx, const uint8_t* data, size_t len) {
  // write with response when the characteristic allows it, as bleak does by default
  bool ok = tx->writeValue(data, len, tx->canWrite());
  if (!ok) BLT_LOG("BLUETTI:   write of %u bytes failed\n", (unsigned)len);
  return ok;
}

// Waits up to wait_ms for one notification and feeds it to the session.
// Returns false on a protocol error.
bool BluettiPoller::processNotifications(NimBLERemoteCharacteristic* tx, uint32_t wait_ms) {
  NotifyItem item;
  if (xQueueReceive(_rx_queue, &item, pdMS_TO_TICKS(wait_ms)) != pdTRUE) return true;
  BluettiSession::Writer writer = [this, tx](const uint8_t* d, size_t n) { return write(tx, d, n); };
  if (!_session->onNotify(item.data, item.len, writer)) {
    BLT_LOG("BLUETTI:   protocol error: %s\n", _session->lastError());
    return false;
  }
  return true;
}

bool BluettiPoller::readRegister(NimBLERemoteCharacteristic* tx, uint16_t addr, uint16_t& value) {
  uint8_t cmd[8];
  uint8_t out[64];
  size_t cmd_len = bluettiBuildRead(cmd, addr, 1);
  size_t out_len = _session->encryptCommand(cmd, cmd_len, out, sizeof(out));
  if (out_len == 0) return false;

  _session->clearResponse();
  if (!write(tx, out, out_len)) return false;

  uint32_t start = millis();
  while (!_session->hasResponse()) {
    if (millis() - start > BLUETTI_REPLY_TIMEOUT_MS) {
      BLT_LOG("BLUETTI:   register %u: no reply\n", (unsigned)addr);
      return false;
    }
    if (!processNotifications(tx, 50)) return false;
  }
  const uint8_t* body;
  if (!bluettiParseRead(_session->response(), _session->responseLen(), 1, body)) {
    BLT_LOG("BLUETTI:   register %u: reply could not be read (%u bytes)\n", (unsigned)addr, (unsigned)_session->responseLen());
    return false;
  }
  value = ((uint16_t)body[0] << 8) | body[1];
  return true;
}

bool BluettiPoller::pollOne(Unit& u) {
  ble_addr_t raw;
  memcpy(raw.val, u.addr, 6);
  raw.type = u.addr_type;
  NimBLEAddress addr(raw);

  _session->reset();
  xQueueReset(_rx_queue);

  BLT_LOG("BLUETTI: connecting to '%s'\n", u.found_name);
  if (!_client->connect(addr, true)) {
    int err = _client->getLastError();
    BLT_LOG("BLUETTI: connect failed (error %d %s), will scan again\n", err, NimBLEUtils::returnCodeToString(err));
    u.have_addr = false;   // scan for it again next cycle
    return false;
  }
  BLT_LOG("BLUETTI: connected (MTU %u)\n", (unsigned)_client->getMTU());

  bool ok = false;
  do {
    // find the two characteristics, whichever service they are in
    NimBLERemoteCharacteristic* rx = NULL;
    NimBLERemoteCharacteristic* tx = NULL;
    std::vector<NimBLERemoteService*>* svcs = _client->getServices(true);
    for (auto svc : *svcs) {
      if (rx == NULL) rx = svc->getCharacteristic(NOTIFY_UUID);
      if (tx == NULL) tx = svc->getCharacteristic(WRITE_UUID);
    }
    if (rx == NULL || tx == NULL) {
      BLT_LOG("BLUETTI: characteristic missing (FF01 %s, FF02 %s)\n", rx ? "ok" : "missing", tx ? "ok" : "missing");
      break;
    }

    // the unit starts the key exchange once notifications are on
    if (!rx->subscribe(true, [this](NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool is_notify) {
          onNotify(data, len);
        }, true)) {
      BLT_LOG("BLUETTI: subscribe to FF01 failed\n");
      break;
    }
    BLT_LOG("BLUETTI: subscribed to FF01, waiting for the key exchange\n");

    uint32_t start = millis();
    bool proto_ok = true;
    while (!_session->isReady() && millis() - start < BLUETTI_HANDSHAKE_TIMEOUT_MS) {
      if (!processNotifications(tx, 100)) { proto_ok = false; break; }
    }
    if (!_session->isReady()) {
      if (proto_ok) BLT_LOG("BLUETTI: key exchange not finished after %u ms\n", (unsigned)BLUETTI_HANDSHAKE_TIMEOUT_MS);
      break;
    }
    BLT_LOG("BLUETTI: key exchange done in %lu ms\n", (unsigned long)(millis() - start));

    BluettiData d;
    memset(&d, 0, sizeof(d));
    uint16_t v;
    if (!readRegister(tx, REG_BATTERY_SOC, v)) break;
    if (v > 100) {
      BLT_LOG("BLUETTI: battery %% out of range (%u)\n", (unsigned)v);
      break;
    }
    d.soc = v;
#if !(defined(BLUETTI_EB3A) && BLUETTI_EB3A)
    if (u.is_el30v2) {
      if (readRegister(tx, REG_TIME_REMAINING, v)) {
        d.time_left_min = v;
        d.have_time = true;
      }
    }
#endif
    if (!readRegister(tx, REG_DC_INPUT_POWER, d.dc_in_w)) break;
    if (!readRegister(tx, REG_AC_INPUT_POWER, d.ac_in_w)) break;
    if (!readRegister(tx, REG_DC_OUTPUT_POWER, d.dc_out_w)) break;
    if (!readRegister(tx, REG_AC_OUTPUT_POWER, d.ac_out_w)) break;

#if defined(BLUETTI_EB3A) && BLUETTI_EB3A
    if (!readRegister(tx, REG_DC_INPUT_VOLTAGE, d.dc_in_v_centi)) break;
    BLT_LOG("BLUETTI: '%s' %u%%, DC in %u W at %u.%02u V, AC in %u W, DC out %u W, AC out %u W\n", u.found_name,
            (unsigned)d.soc, d.dc_in_w, d.dc_in_v_centi / 100, d.dc_in_v_centi % 100, d.ac_in_w, d.dc_out_w, d.ac_out_w);
#else
    if (d.have_time) {
      BLT_LOG("BLUETTI: '%s' %u%%, %u min left, DC in %u W, AC in %u W, DC out %u W, AC out %u W\n", u.found_name,
              (unsigned)d.soc, (unsigned)d.time_left_min, d.dc_in_w, d.ac_in_w, d.dc_out_w, d.ac_out_w);
    } else {
      BLT_LOG("BLUETTI: '%s' %u%%, DC in %u W, AC in %u W, DC out %u W, AC out %u W\n", u.found_name,
              (unsigned)d.soc, d.dc_in_w, d.ac_in_w, d.dc_out_w, d.ac_out_w);
    }
#endif

    xSemaphoreTake(_lock, portMAX_DELAY);
    u.data = d;
    u.read_millis = millis();
    u.have_reading = true;
    xSemaphoreGive(_lock);
    ok = true;
  } while (0);

  _client->disconnect();
  uint32_t start = millis();
  while (_client->isConnected() && millis() - start < 3000) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  BLT_LOG("BLUETTI: disconnected%s\n", _client->isConnected() ? " (still connected after 3 s)" : "");
  return ok;
}
