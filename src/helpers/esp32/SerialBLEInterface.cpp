#include "SerialBLEInterface.h"
#include "esp_mac.h"
#ifdef BLE_GARMIN
#include "esp_gap_ble_api.h"
#endif

// See the following for generating UUIDs:
// https://www.uuidgenerator.net/

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" // UART service UUID
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#define ADVERT_RESTART_DELAY  1000   // millis

#ifdef BLE_GARMIN
// Slice tags for small-MTU peers. No real frame can start with one of these:
// command codes are below 0x80 and push codes stop well short of 0xF0. When
// slicing is in effect, every frame to or from the peer travels as
// [tag][len][payload] slices.
#define SLICE_TAG_FIRST   0xF0   // first slice of a multi-slice frame
#define SLICE_TAG_MIDDLE  0xF1
#define SLICE_TAG_LAST    0xF2
#define SLICE_TAG_SINGLE  0xF3   // whole frame fits in one slice
#define SLICE_TAG_MASK    0xFC   // (b & MASK) == SLICE_TAG_FIRST matches any tag
#define SPLIT_MAX_MTU     23     // slice outgoing frames when the peer MTU is at or below this

// Heart Rate sensor disguise (see the header).
#define SENSOR_SVC_UUID        0x180D   // Heart Rate
#define SENSOR_CHR_HRM_UUID    0x2A37   // Heart Rate Measurement (notify)
#define SENSOR_CHR_LOC_UUID    0x2A38   // Body Sensor Location (read)
#define SENSOR_APPEARANCE      0x0341   // Heart Rate Belt
#define ADV_NAME_MAX_BYTES     18       // 31 - flags(3) - 16-bit svc(4) - appearance(4) - name header(2)
#endif

void SerialBLEInterface::begin(const char* prefix, char* name, uint32_t pin_code) {
  _pin_code = pin_code;

  if (strcmp(name, "@@MAC") == 0) {
    uint8_t addr[8];
    memset(addr, 0, sizeof(addr));
    esp_efuse_mac_get_default(addr);
    sprintf(name, "%02X%02X%02X%02X%02X%02X",    // modify (IN-OUT param)
          addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
  }
  char dev_name[32+16];
  sprintf(dev_name, "%s%s", prefix, name);

  // Create the BLE Device
  BLEDevice::init(dev_name);
  BLEDevice::setSecurityCallbacks(this);
  BLEDevice::setMTU(MAX_FRAME_SIZE);

  BLESecurity  sec;
  sec.setStaticPIN(pin_code);
#ifdef BLE_GARMIN
  // The watch can only reach an encrypted-but-unverified ("just works") link;
  // drop the MITM requirement so that link is accepted. The static PIN above
  // is then never asked for.
  sec.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
  const esp_gatt_perm_t permRead  = ESP_GATT_PERM_READ_ENCRYPTED;
  const esp_gatt_perm_t permWrite = ESP_GATT_PERM_WRITE_ENCRYPTED;
#else
  sec.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  const esp_gatt_perm_t permRead  = ESP_GATT_PERM_READ_ENC_MITM;
  const esp_gatt_perm_t permWrite = ESP_GATT_PERM_WRITE_ENC_MITM;
#endif

  //BLEDevice::setPower(ESP_PWR_LVL_N8);

  // Create the BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(this);

  // Create the BLE Service
  pService = pServer->createService(SERVICE_UUID);

  // Create a BLE Characteristic
  pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_TX, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->setAccessPermissions(permRead);
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic * pRxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_RX, BLECharacteristic::PROPERTY_WRITE);
  pRxCharacteristic->setAccessPermissions(permWrite);
  pRxCharacteristic->setCallbacks(this);

#ifdef BLE_GARMIN
  // Heart Rate service: notify-only measurement plus body sensor location,
  // with the same link requirement as the UART characteristics, so the
  // watch's sensor menu has to pair before it can subscribe.
  pSensorService = pServer->createService(BLEUUID((uint16_t)SENSOR_SVC_UUID));
  BLECharacteristic* pHrm = pSensorService->createCharacteristic(BLEUUID((uint16_t)SENSOR_CHR_HRM_UUID), BLECharacteristic::PROPERTY_NOTIFY);
  pHrm->setAccessPermissions(permRead);
  BLE2902* pHrmCccd = new BLE2902();
  pHrmCccd->setAccessPermissions(permRead | permWrite);
  pHrm->addDescriptor(pHrmCccd);
  uint8_t hrmInit[2] = { 0x00, 0x00 };   // flags byte + 8-bit value 0
  pHrm->setValue(hrmInit, 2);
  BLECharacteristic* pLoc = pSensorService->createCharacteristic(BLEUUID((uint16_t)SENSOR_CHR_LOC_UUID), BLECharacteristic::PROPERTY_READ);
  pLoc->setAccessPermissions(permRead);
  uint8_t locChest = 1;
  pLoc->setValue(&locChest, 1);

  // Advertising packet: flags, 16-bit Heart Rate UUID, appearance, name
  // (shortened to ADV_NAME_MAX_BYTES at a UTF-8 character boundary if
  // needed). Scan response: the 128-bit UART UUID, so phone apps that filter
  // on it still find the node.
  BLEAdvertisementData adv;
  adv.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
  adv.setCompleteServices(BLEUUID((uint16_t)SENSOR_SVC_UUID));
  adv.setAppearance(SENSOR_APPEARANCE);
  size_t nameLen = strlen(dev_name);
  if (nameLen <= ADV_NAME_MAX_BYTES) {
    adv.setName(std::string(dev_name, nameLen));
  } else {
    size_t cut = ADV_NAME_MAX_BYTES;
    while (cut > 0 && (((uint8_t)dev_name[cut]) & 0xC0) == 0x80) cut--;   // do not split a UTF-8 sequence
    adv.setShortName(std::string(dev_name, cut));
  }
  BLEAdvertisementData rsp;
  rsp.setCompleteServices(BLEUUID(SERVICE_UUID));
  pServer->getAdvertising()->setAdvertisementData(adv);
  pServer->getAdvertising()->setScanResponseData(rsp);
  BLE_DEBUG_PRINTLN("Garmin: HR service advertised, UART in scan response, name bytes=%u", (unsigned)nameLen);
#else
  pServer->getAdvertising()->addServiceUUID(SERVICE_UUID);
#endif
}

// -------- BLESecurityCallbacks methods

uint32_t SerialBLEInterface::onPassKeyRequest() {
  BLE_DEBUG_PRINTLN("onPassKeyRequest()");
  return _pin_code;
}

void SerialBLEInterface::onPassKeyNotify(uint32_t pass_key) {
  BLE_DEBUG_PRINTLN("onPassKeyNotify(%u)", pass_key);
}

bool SerialBLEInterface::onConfirmPIN(uint32_t pass_key) {
  BLE_DEBUG_PRINTLN("onConfirmPIN(%u)", pass_key);
  return true;
}

bool SerialBLEInterface::onSecurityRequest() {
  BLE_DEBUG_PRINTLN("onSecurityRequest()");
  return true;  // allow
}

void SerialBLEInterface::onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) {
  if (cmpl.success) {
    BLE_DEBUG_PRINTLN(" - SecurityCallback - Authentication Success");
    deviceConnected = true;
  } else {
    BLE_DEBUG_PRINTLN(" - SecurityCallback - Authentication Failure*");

    //pServer->removePeerDevice(pServer->getConnId(), true);
    pServer->disconnect(pServer->getConnId());
    adv_restart_time = millis() + ADVERT_RESTART_DELAY;
  }
}

// -------- BLEServerCallbacks methods

void SerialBLEInterface::onConnect(BLEServer* pServer) {
}

void SerialBLEInterface::onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t *param) {
  BLE_DEBUG_PRINTLN("onConnect(), conn_id=%d, mtu=%d", param->connect.conn_id, pServer->getPeerMTU(param->connect.conn_id));
  last_conn_id = param->connect.conn_id;
}

void SerialBLEInterface::onMtuChanged(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) {
  BLE_DEBUG_PRINTLN("onMtuChanged(), mtu=%d", pServer->getPeerMTU(param->mtu.conn_id));
}

void SerialBLEInterface::onDisconnect(BLEServer* pServer) {
  BLE_DEBUG_PRINTLN("onDisconnect()");
  deviceConnected = false;
  if (_isEnabled) {
    adv_restart_time = millis() + ADVERT_RESTART_DELAY;
  }
}

// -------- BLECharacteristicCallbacks methods

void SerialBLEInterface::onWrite(BLECharacteristic* pCharacteristic, esp_ble_gatts_cb_param_t* param) {
  uint8_t* rxValue = pCharacteristic->getData();
  int len = pCharacteristic->getLength();

#ifdef BLE_GARMIN
  if (len >= 1 && (rxValue[0] & SLICE_TAG_MASK) == SLICE_TAG_FIRST) {   // any SLICE_TAG_*
    uint8_t tag = rxValue[0];
    if (tag == SLICE_TAG_FIRST || tag == SLICE_TAG_SINGLE) _rx_len = 0;
    if (_rx_len < 0) return;                                  // middle/last with no start: drop
    if (len < 2) return;                                      // no length byte
    int n = rxValue[1];
    if (n > len - 2) n = len - 2;                             // never read past the write
    if (_rx_len + n > MAX_FRAME_SIZE) { _rx_len = -1; return; }   // oversize: drop the frame
    memcpy(&_rx_buf[_rx_len], rxValue + 2, n);
    _rx_len += n;
    if (tag == SLICE_TAG_LAST || tag == SLICE_TAG_SINGLE) {
      Frame frame = {};
      frame.len = _rx_len;
      memcpy(frame.buf, _rx_buf, _rx_len);
      if (xQueueSend(recv_queue, &frame, 0) != pdTRUE) {
        BLE_DEBUG_PRINTLN("ERROR: onWrite(), recv_queue is full! (sliced)");
      }
      _rx_len = -1;
    }
    return;
  }
#endif
  if (len > MAX_FRAME_SIZE) {
    BLE_DEBUG_PRINTLN("ERROR: onWrite(), frame too big, len=%d", len);
  } else {
    Frame frame = {};
    frame.len = len;
    memcpy(frame.buf, rxValue, len);

    if (xQueueSend(recv_queue, &frame, 0) != pdTRUE) {
      BLE_DEBUG_PRINTLN("ERROR: onWrite(), recv_queue is full!");
    }
  }
}

// ---------- public methods

void SerialBLEInterface::clearBuffers() {
  xQueueReset(recv_queue);
  send_queue_len = 0;
#ifdef BLE_GARMIN
  _tx_off = 0; _rx_len = -1;
#endif
}

void SerialBLEInterface::enable() {
  if (_isEnabled) return;

  _isEnabled = true;
  clearBuffers();

  // Start the service
  pService->start();
#ifdef BLE_GARMIN
  pSensorService->start();
#endif

  // Start advertising

  //pServer->getAdvertising()->setMinInterval(500);
  //pServer->getAdvertising()->setMaxInterval(1000);

  pServer->getAdvertising()->start();
  adv_restart_time = 0;
}

void SerialBLEInterface::disable() {
  _isEnabled = false;

  BLE_DEBUG_PRINTLN("SerialBLEInterface::disable");

  pServer->getAdvertising()->stop();
  pServer->disconnect(last_conn_id);
  pService->stop();
#ifdef BLE_GARMIN
  pSensorService->stop();
#endif
  oldDeviceConnected = deviceConnected = false;
  adv_restart_time = 0;
}

size_t SerialBLEInterface::writeFrame(const uint8_t src[], size_t len) {
  if (len > MAX_FRAME_SIZE) {
    BLE_DEBUG_PRINTLN("writeFrame(), frame too big, len=%d", len);
    return 0;
  }

  if (deviceConnected && len > 0) {
    if (send_queue_len >= FRAME_QUEUE_SIZE) {
      BLE_DEBUG_PRINTLN("writeFrame(), send_queue is full!");
      return 0;
    }

    send_queue[send_queue_len].len = len;  // add to send queue
    memcpy(send_queue[send_queue_len].buf, src, len);
    send_queue_len++;

    return len;
  }
  return 0;
}

#define  BLE_WRITE_MIN_INTERVAL   60

bool SerialBLEInterface::isWriteBusy() const {
  return millis() < _last_write + BLE_WRITE_MIN_INTERVAL;   // still too soon to start another write?
}

size_t SerialBLEInterface::checkRecvFrame(uint8_t dest[]) {
  if (send_queue_len > 0   // first, check send queue
    && millis() >= _last_write + BLE_WRITE_MIN_INTERVAL    // space the writes apart
  ) {
    _last_write = millis();
#ifdef BLE_GARMIN
    bool popFrame = true;
    uint16_t mtu = pServer->getPeerMTU(last_conn_id);
    if (mtu <= SPLIT_MAX_MTU) {
      // Small-MTU peer: send the next slice of the frame at the head of the
      // queue, one slice per pass (keeps BLE_WRITE_MIN_INTERVAL pacing). The
      // frame is only popped once its last slice has gone.
      int room = (int)mtu - 3 - 2;                 // ATT payload minus tag and length bytes
      if (room < 1) room = 1;
      int total = send_queue[0].len;
      int n = total - _tx_off;
      if (n > room) n = room;
      bool first = (_tx_off == 0);
      bool last  = (_tx_off + n >= total);
      uint8_t chunk[MAX_FRAME_SIZE + 2];
      chunk[0] = (first && last) ? SLICE_TAG_SINGLE
               : first ? SLICE_TAG_FIRST
               : last  ? SLICE_TAG_LAST : SLICE_TAG_MIDDLE;
      chunk[1] = (uint8_t)n;
      memcpy(&chunk[2], &send_queue[0].buf[_tx_off], n);
      pTxCharacteristic->setValue(chunk, n + 2);
      pTxCharacteristic->notify();
      BLE_DEBUG_PRINTLN("writeSlice: tag=%02X off=%d n=%d of %d", chunk[0], (int)_tx_off, n, total);
      _tx_off += n;
      if (last) {
        _tx_off = 0;
        BLE_DEBUG_PRINTLN("writeBytes: sz=%d, hdr=%d (sliced)", total, (uint32_t) send_queue[0].buf[0]);
      } else {
        popFrame = false;
      }
    } else {
#endif
    pTxCharacteristic->setValue(send_queue[0].buf, send_queue[0].len);
    pTxCharacteristic->notify();

    BLE_DEBUG_PRINTLN("writeBytes: sz=%d, hdr=%d", (uint32_t)send_queue[0].len, (uint32_t) send_queue[0].buf[0]);
#ifdef BLE_GARMIN
    }
    if (popFrame) {
#endif
    send_queue_len--;
    for (int i = 0; i < send_queue_len; i++) {   // delete top item from queue
      send_queue[i] = send_queue[i + 1];
    }
#ifdef BLE_GARMIN
    }
#endif
  }

  Frame frame;
  if (xQueueReceive(recv_queue, &frame, 0) == pdTRUE) {
    memcpy(dest, frame.buf, frame.len);
    BLE_DEBUG_PRINTLN("readBytes: sz=%d, hdr=%d", (uint32_t) frame.len, (uint32_t) dest[0]);
    return frame.len;
  }

  if (deviceConnected != oldDeviceConnected) {
    if (!deviceConnected) {    // disconnecting
      clearBuffers();

      BLE_DEBUG_PRINTLN("SerialBLEInterface -> disconnecting...");

      //pServer->getAdvertising()->setMinInterval(500);
      //pServer->getAdvertising()->setMaxInterval(1000);

      adv_restart_time = millis() + ADVERT_RESTART_DELAY;
    } else {
      BLE_DEBUG_PRINTLN("SerialBLEInterface -> stopping advertising");
      BLE_DEBUG_PRINTLN("SerialBLEInterface -> connecting...");
      // connecting
      // do stuff here on connecting
      pServer->getAdvertising()->stop();
      adv_restart_time = 0;
    }
    oldDeviceConnected = deviceConnected;
  }

  if (adv_restart_time && millis() >= adv_restart_time) {
    if (pServer->getConnectedCount() == 0) {
      BLE_DEBUG_PRINTLN("SerialBLEInterface -> re-starting advertising");
      pServer->getAdvertising()->start();  // re-Start advertising
    }
    adv_restart_time = 0;
  }
  return 0;
}

bool SerialBLEInterface::isConnected() const {
  return deviceConnected;  //pServer != NULL && pServer->getConnectedCount() > 0;
}