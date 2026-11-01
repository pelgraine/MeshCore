#include "SerialBLEInterface.h"
#include <stdio.h>
#include <string.h>
#include "ble_gap.h"
#include "ble_hci.h"

// Magic numbers came from actual testing
#define BLE_HEALTH_CHECK_INTERVAL  10000  // Advertising watchdog check every 10 seconds
#define BLE_RETRY_THROTTLE_MS      250    // Throttle retries to 250ms when queue buildup detected

// Connection parameters (units: interval=1.25ms, timeout=10ms)
#define BLE_MIN_CONN_INTERVAL      12     // 15ms
#define BLE_MAX_CONN_INTERVAL      24     // 30ms
#define BLE_SLAVE_LATENCY          4
#define BLE_CONN_SUP_TIMEOUT       200    // 2000ms

// Advertising parameters
#define BLE_ADV_INTERVAL_MIN       32     // 20ms (units: 0.625ms)
#define BLE_ADV_INTERVAL_MAX       244    // 152.5ms (units: 0.625ms)
#define BLE_ADV_FAST_TIMEOUT       30     // seconds

// RX drain buffer size for overflow protection
#define BLE_RX_DRAIN_BUF_SIZE      32

#ifdef BLE_GARMIN
// Slice tags for small-MTU peers. No real frame can start with one of these:
// command codes are below 0x80 and push codes stop well short of 0xF0. When
// slicing is in effect, every frame to or from the peer travels as
// [tag][len][payload] slices. Same scheme as the ESP32 transport.
#define SLICE_TAG_FIRST   0xF0   // first slice of a multi-slice frame
#define SLICE_TAG_MIDDLE  0xF1
#define SLICE_TAG_LAST    0xF2
#define SLICE_TAG_SINGLE  0xF3   // whole frame fits in one slice
#define SLICE_TAG_MASK    0xFC   // (b & MASK) == SLICE_TAG_FIRST matches any tag
#define SPLIT_MAX_MTU     23     // slice outgoing frames when the peer MTU is at or below this

// Heart Rate sensor disguise (see the header).
#define SENSOR_APPEARANCE      0x0341   // Heart Rate Belt
#endif

static SerialBLEInterface* instance = nullptr;

void SerialBLEInterface::onConnect(uint16_t connection_handle) {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: connected handle=0x%04X", connection_handle);
  if (instance) {
    instance->_conn_handle = connection_handle;
    instance->_isDeviceConnected = false;
    instance->clearBuffers();
  }
}

void SerialBLEInterface::onDisconnect(uint16_t connection_handle, uint8_t reason) {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: disconnected handle=0x%04X reason=%u", connection_handle, reason);
  if (instance) {
    if (instance->_conn_handle == connection_handle) {
      instance->_conn_handle = BLE_CONN_HANDLE_INVALID;
      instance->_isDeviceConnected = false;
      instance->clearBuffers();
    }
  }
}

void SerialBLEInterface::onSecured(uint16_t connection_handle) {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: onSecured handle=0x%04X", connection_handle);
  if (instance) {
    if (instance->isValidConnection(connection_handle, true)) {
      instance->_isDeviceConnected = true;
      
      // Connection interval units: 1.25ms, supervision timeout units: 10ms
      // Apple: "The product will not read or use the parameters in the Peripheral Preferred Connection Parameters characteristic."
      // So we explicitly set it here to make Android & Apple match
      ble_gap_conn_params_t conn_params;
      conn_params.min_conn_interval = BLE_MIN_CONN_INTERVAL;
      conn_params.max_conn_interval = BLE_MAX_CONN_INTERVAL;
      conn_params.slave_latency = BLE_SLAVE_LATENCY;
      conn_params.conn_sup_timeout = BLE_CONN_SUP_TIMEOUT;
      
      uint32_t err_code = sd_ble_gap_conn_param_update(connection_handle, &conn_params);
      if (err_code == NRF_SUCCESS) {
        BLE_DEBUG_PRINTLN("Connection parameter update requested: %u-%ums interval, latency=%u, %ums timeout",
                         conn_params.min_conn_interval * 5 / 4,  // convert to ms (1.25ms units)
                         conn_params.max_conn_interval * 5 / 4,
                         conn_params.slave_latency,
                         conn_params.conn_sup_timeout * 10);  // convert to ms (10ms units)
      } else {
        BLE_DEBUG_PRINTLN("Failed to request connection parameter update: %lu", err_code);
      }
    } else {
      BLE_DEBUG_PRINTLN("onSecured: ignoring stale/duplicate callback");
    }
  }
}

bool SerialBLEInterface::onPairingPasskey(uint16_t connection_handle, uint8_t const passkey[6], bool match_request) {
  (void)connection_handle;
  (void)passkey;
  BLE_DEBUG_PRINTLN("SerialBLEInterface: pairing passkey request match=%d", match_request);
  return true;
}

void SerialBLEInterface::onPairingComplete(uint16_t connection_handle, uint8_t auth_status) {
  BLE_DEBUG_PRINTLN("SerialBLEInterface: pairing complete handle=0x%04X status=%u", connection_handle, auth_status);
  if (instance) {
    if (instance->isValidConnection(connection_handle)) {
      if (auth_status == BLE_GAP_SEC_STATUS_SUCCESS) {
        BLE_DEBUG_PRINTLN("SerialBLEInterface: pairing successful");
      } else {
        BLE_DEBUG_PRINTLN("SerialBLEInterface: pairing failed, disconnecting");
        instance->disconnect();
      }
    } else {
      BLE_DEBUG_PRINTLN("onPairingComplete: ignoring stale callback");
    }
  }
}

void SerialBLEInterface::onBLEEvent(ble_evt_t* evt) {
  if (!instance) return;
  
  if (evt->header.evt_id == BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST) {
    uint16_t conn_handle = evt->evt.gap_evt.conn_handle;
    if (instance->isValidConnection(conn_handle)) {
      BLE_DEBUG_PRINTLN("CONN_PARAM_UPDATE_REQUEST: handle=0x%04X, min_interval=%u, max_interval=%u, latency=%u, timeout=%u",
                       conn_handle,
                       evt->evt.gap_evt.params.conn_param_update_request.conn_params.min_conn_interval,
                       evt->evt.gap_evt.params.conn_param_update_request.conn_params.max_conn_interval,
                       evt->evt.gap_evt.params.conn_param_update_request.conn_params.slave_latency,
                       evt->evt.gap_evt.params.conn_param_update_request.conn_params.conn_sup_timeout);
      
      uint32_t err_code = sd_ble_gap_conn_param_update(conn_handle, NULL);
      if (err_code == NRF_SUCCESS) {
        BLE_DEBUG_PRINTLN("Accepted CONN_PARAM_UPDATE_REQUEST (using PPCP)");
      } else {
        BLE_DEBUG_PRINTLN("ERROR: Failed to accept CONN_PARAM_UPDATE_REQUEST: 0x%08X", err_code);
      }
    } else {
      BLE_DEBUG_PRINTLN("CONN_PARAM_UPDATE_REQUEST: ignoring stale callback for handle=0x%04X", conn_handle);
    }
  }
}

void SerialBLEInterface::begin(const char* prefix, char* name, uint32_t pin_code) {
  instance = this;

  char charpin[20];
  snprintf(charpin, sizeof(charpin), "%lu", (unsigned long)pin_code);
  
  // If we want to control BLE LED ourselves, uncomment this:
  // Bluefruit.autoConnLed(false);
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.begin();
 
  char dev_name[32+16];
  if (strcmp(name, "@@MAC") == 0) {
    ble_gap_addr_t addr;
    if (sd_ble_gap_addr_get(&addr) == NRF_SUCCESS) {
      sprintf(name, "%02X%02X%02X%02X%02X%02X",    // modify (IN-OUT param)
          addr.addr[5], addr.addr[4], addr.addr[3], addr.addr[2], addr.addr[1], addr.addr[0]);
    }
  }
  sprintf(dev_name, "%s%s", prefix, name);

  // Connection interval units: 1.25ms, supervision timeout units: 10ms
  ble_gap_conn_params_t ppcp_params;
  ppcp_params.min_conn_interval = BLE_MIN_CONN_INTERVAL;
  ppcp_params.max_conn_interval = BLE_MAX_CONN_INTERVAL;
  ppcp_params.slave_latency = BLE_SLAVE_LATENCY;
  ppcp_params.conn_sup_timeout = BLE_CONN_SUP_TIMEOUT;
  
  uint32_t err_code = sd_ble_gap_ppcp_set(&ppcp_params);
  if (err_code == NRF_SUCCESS) {
    BLE_DEBUG_PRINTLN("PPCP set: %u-%ums interval, latency=%u, %ums timeout",
                     ppcp_params.min_conn_interval * 5 / 4,  // convert to ms (1.25ms units)
                     ppcp_params.max_conn_interval * 5 / 4,
                     ppcp_params.slave_latency,
                     ppcp_params.conn_sup_timeout * 10);  // convert to ms (10ms units)
  } else {
    BLE_DEBUG_PRINTLN("Failed to set PPCP: %lu", err_code);
  }
  
  Bluefruit.setTxPower(BLE_TX_POWER);
  Bluefruit.setName(dev_name);

#ifdef BLE_GARMIN
  // The watch can only reach an encrypted-but-unverified ("just works") link;
  // drop the MITM requirement so that link is accepted. setPIN() is not
  // called: it forces MITM on, legacy pairing and a display-only IO
  // capability as side effects. No PIN is ever asked for.
  Bluefruit.Security.setMITM(false);
  Bluefruit.Security.setIOCaps(false, false, false);
  const SecureMode_t permUart = SECMODE_ENC_NO_MITM;
#else
  Bluefruit.Security.setMITM(true);
  Bluefruit.Security.setPIN(charpin);
  Bluefruit.Security.setIOCaps(true, false, false);
  const SecureMode_t permUart = SECMODE_ENC_WITH_MITM;
#endif
  Bluefruit.Security.setPairPasskeyCallback(onPairingPasskey);
  Bluefruit.Security.setPairCompleteCallback(onPairingComplete);

  Bluefruit.Periph.setConnectCallback(onConnect);
  Bluefruit.Periph.setDisconnectCallback(onDisconnect);
  Bluefruit.Security.setSecuredCallback(onSecured);

  Bluefruit.setEventCallback(onBLEEvent);

  bleuart.setPermission(permUart, permUart);
  bleuart.begin();
  bleuart.setRxCallback(onBleUartRX);



  // Register DFU on the main BLE stack so paired clients can discover it
  // without switching the device into a separate OTA-only BLE mode first.
  bledfu.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);
  bledfu.begin();

#ifdef BLE_GARMIN
  // Heart Rate service: notify-only measurement plus body sensor location,
  // with the same link requirement as the UART characteristics, so the
  // watch's sensor menu has to pair before it can subscribe. (Bluefruit gives
  // the CCCD the characteristic's read permission.)
  hrmSvc.begin();
  hrmChr.setProperties(CHR_PROPS_NOTIFY);
  hrmChr.setPermission(permUart, SECMODE_NO_ACCESS);
  hrmChr.setFixedLen(2);
  hrmChr.begin();
  uint8_t hrmInit[2] = { 0x00, 0x00 };   // flags byte + 8-bit value 0
  hrmChr.write(hrmInit, 2);
  bslChr.setProperties(CHR_PROPS_READ);
  bslChr.setPermission(permUart, SECMODE_NO_ACCESS);
  bslChr.setFixedLen(1);
  bslChr.begin();
  bslChr.write8(1);   // chest

  // Advertising packet: flags, TX power, 16-bit Heart Rate UUID, appearance,
  // name (Bluefruit shortens it to fit). Scan response: the 128-bit UART
  // UUID, so phone apps that filter on it still find the node.
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(hrmSvc);
  Bluefruit.Advertising.addAppearance(SENSOR_APPEARANCE);
  bool fullName = Bluefruit.Advertising.addName();
  (void)fullName;   // only used by the debug line below
  Bluefruit.ScanResponse.addService(bleuart);
  BLE_DEBUG_PRINTLN("Garmin: HR service advertised, UART in scan response, name %s", fullName ? "complete" : "shortened");
#else
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);

  Bluefruit.ScanResponse.addName();
#endif

  Bluefruit.Advertising.setInterval(BLE_ADV_INTERVAL_MIN, BLE_ADV_INTERVAL_MAX);
  Bluefruit.Advertising.setFastTimeout(BLE_ADV_FAST_TIMEOUT);

  Bluefruit.Advertising.restartOnDisconnect(true);

}

void SerialBLEInterface::clearBuffers() {
  send_queue_len = 0;
  recv_queue_len = 0;
  _last_retry_attempt = 0;
#ifdef BLE_GARMIN
  _tx_off = 0; _rx_len = -1;
#endif
  bleuart.flush();
}

void SerialBLEInterface::shiftSendQueueLeft() {
  if (send_queue_len > 0) {
    send_queue_len--;
    for (uint8_t i = 0; i < send_queue_len; i++) {
      send_queue[i] = send_queue[i + 1];
    }
  }
}

void SerialBLEInterface::shiftRecvQueueLeft() {
  if (recv_queue_len > 0) {
    recv_queue_len--;
    for (uint8_t i = 0; i < recv_queue_len; i++) {
      recv_queue[i] = recv_queue[i + 1];
    }
  }
}

bool SerialBLEInterface::isValidConnection(uint16_t handle, bool requireWaitingForSecurity) const {
  if (_conn_handle != handle) {
    return false;
  }
  BLEConnection* conn = Bluefruit.Connection(handle);
  if (conn == nullptr || !conn->connected()) {
    return false;
  }
  if (requireWaitingForSecurity && _isDeviceConnected) {
    return false;
  }
  return true;
}

bool SerialBLEInterface::isAdvertising() const {
  ble_gap_addr_t adv_addr;
  uint32_t err_code = sd_ble_gap_adv_addr_get(0, &adv_addr);
  return (err_code == NRF_SUCCESS);
}

void SerialBLEInterface::enable() {
  if (_isEnabled) return;

  _isEnabled = true;
  clearBuffers();
  _last_health_check = millis();

  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.start(0);
}

void SerialBLEInterface::disconnect() {
  if (_conn_handle != BLE_CONN_HANDLE_INVALID) {
    sd_ble_gap_disconnect(_conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
  }
}

void SerialBLEInterface::disable() {
  _isEnabled = false;
  BLE_DEBUG_PRINTLN("SerialBLEInterface: disable");

  Bluefruit.Advertising.restartOnDisconnect(false);
  Bluefruit.Advertising.stop();
  disconnect();
  _last_health_check = 0;
}

size_t SerialBLEInterface::writeFrame(const uint8_t src[], size_t len) {
  if (len > MAX_FRAME_SIZE) {
    BLE_DEBUG_PRINTLN("writeFrame(), frame too big, len=%u", (unsigned)len);
    return 0;
  }

  bool connected = isConnected();
  if (connected && len > 0) {
    if (send_queue_len >= FRAME_QUEUE_SIZE) {
      BLE_DEBUG_PRINTLN("writeFrame(), send_queue is full!");
      return 0;
    }

    send_queue[send_queue_len].len = len;
    memcpy(send_queue[send_queue_len].buf, src, len);
    send_queue_len++;
    
    return len;
  }
  return 0;
}

size_t SerialBLEInterface::checkRecvFrame(uint8_t dest[]) {
  if (send_queue_len > 0) {
    if (!isConnected()) {
      BLE_DEBUG_PRINTLN("writeBytes: connection invalid, clearing send queue");
      send_queue_len = 0;
    } else {
      unsigned long now = millis();
      bool throttle_active = (_last_retry_attempt > 0 && (now - _last_retry_attempt) < BLE_RETRY_THROTTLE_MS);

      if (!throttle_active) {
#ifdef BLE_GARMIN
        BLEConnection* conn = Bluefruit.Connection(_conn_handle);
        uint16_t mtu = conn ? conn->getMtu() : 23;
        if (mtu <= SPLIT_MAX_MTU) {
          // Small-MTU peer: send the next slice of the frame at the head of the
          // queue, one slice per pass. Bluefruit's own notify would split a long
          // frame into bare MTU-sized pieces the watch cannot tell apart, so the
          // frame is cut here with tags instead. It is only popped once its
          // last slice has gone.
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
          size_t written = bleuart.write(chunk, n + 2);
          if (written == (size_t)(n + 2)) {
            BLE_DEBUG_PRINTLN("writeSlice: tag=%02X off=%d n=%d of %d", chunk[0], (int)_tx_off, n, total);
            _last_retry_attempt = 0;
            _tx_off += n;
            if (last) {
              BLE_DEBUG_PRINTLN("writeBytes: sz=%d, hdr=%u (sliced)", total, (unsigned)send_queue[0].buf[0]);
              _tx_off = 0;
              shiftSendQueueLeft();
            }
          } else if (!isConnected()) {
            BLE_DEBUG_PRINTLN("writeSlice failed: connection lost, dropping frame");
            _last_retry_attempt = 0;
            _tx_off = 0;
            shiftSendQueueLeft();
          } else {
            BLE_DEBUG_PRINTLN("writeSlice failed (buffer full), keeping frame for retry");
            _last_retry_attempt = now;
          }
        } else {
#endif
        Frame frame_to_send = send_queue[0];

        size_t written = bleuart.write(frame_to_send.buf, frame_to_send.len);
        if (written == frame_to_send.len) {
          BLE_DEBUG_PRINTLN("writeBytes: sz=%u, hdr=%u", (unsigned)frame_to_send.len, (unsigned)frame_to_send.buf[0]);
          _last_retry_attempt = 0;
          shiftSendQueueLeft();
        } else if (written > 0) {
          BLE_DEBUG_PRINTLN("writeBytes: partial write, sent=%u of %u, dropping corrupted frame", (unsigned)written, (unsigned)frame_to_send.len);
          _last_retry_attempt = 0;
          shiftSendQueueLeft();
        } else {
          if (!isConnected()) {
            BLE_DEBUG_PRINTLN("writeBytes failed: connection lost, dropping frame");
            _last_retry_attempt = 0;
            shiftSendQueueLeft();
          } else {
            BLE_DEBUG_PRINTLN("writeBytes failed (buffer full), keeping frame for retry");
            _last_retry_attempt = now;
          }
        }
#ifdef BLE_GARMIN
        }
#endif
      }
    }
  }

  if (recv_queue_len > 0) {
    size_t len = recv_queue[0].len;
    memcpy(dest, recv_queue[0].buf, len);
    
    BLE_DEBUG_PRINTLN("readBytes: sz=%u, hdr=%u", (unsigned)len, (unsigned)dest[0]);
    
    shiftRecvQueueLeft();
    return len;
  }
  
  // Advertising watchdog: periodically check if advertising is running, restart if not
  // Only run when truly disconnected (no connection handle), not during connection establishment
  unsigned long now = millis();
  if (_isEnabled && !isConnected() && _conn_handle == BLE_CONN_HANDLE_INVALID) {
    if (now - _last_health_check >= BLE_HEALTH_CHECK_INTERVAL) {
      _last_health_check = now;
      
      if (!isAdvertising()) {
        BLE_DEBUG_PRINTLN("SerialBLEInterface: advertising watchdog - advertising stopped, restarting");
        Bluefruit.Advertising.start(0);
      }
    }
  }
  
  return 0;
}

void SerialBLEInterface::onBleUartRX(uint16_t conn_handle) {
  if (!instance) {
    return;
  }
  
  if (instance->_conn_handle != conn_handle || !instance->isConnected()) {
    while (instance->bleuart.available() > 0) {
      instance->bleuart.read();
    }
    return;
  }

#ifdef BLE_GARMIN
  // Sliced writes from a small-MTU peer. Each write lands in the UART RX
  // buffer whole, so the buffer holds complete [tag][len][payload] slices,
  // possibly several. Consume them here; anything else falls through to the
  // normal one-write-one-frame path below.
  while (instance->bleuart.available() >= 2 && (instance->bleuart.peek() & SLICE_TAG_MASK) == SLICE_TAG_FIRST) {   // any SLICE_TAG_*
    uint8_t tag = (uint8_t)instance->bleuart.read();
    int n = instance->bleuart.read();
    if (tag == SLICE_TAG_FIRST || tag == SLICE_TAG_SINGLE) instance->_rx_len = 0;
    int avail = instance->bleuart.available();
    if (n > avail) n = avail;                                   // never read past what has arrived
    if (instance->_rx_len < 0 || instance->_rx_len + n > MAX_FRAME_SIZE) {
      // middle/last with no start, or oversize: drop this slice's payload and the frame
      for (int i = 0; i < n; i++) instance->bleuart.read();
      instance->_rx_len = -1;
      continue;
    }
    for (int i = 0; i < n; i++) {
      instance->_rx_buf[instance->_rx_len++] = (uint8_t)instance->bleuart.read();
    }
    if (tag == SLICE_TAG_LAST || tag == SLICE_TAG_SINGLE) {
      if (instance->recv_queue_len < FRAME_QUEUE_SIZE) {
        instance->recv_queue[instance->recv_queue_len].len = instance->_rx_len;
        memcpy(instance->recv_queue[instance->recv_queue_len].buf, instance->_rx_buf, instance->_rx_len);
        instance->recv_queue_len++;
      } else {
        BLE_DEBUG_PRINTLN("onBleUartRX: recv queue full, dropping sliced frame");
      }
      instance->_rx_len = -1;
    }
  }
#endif

  while (instance->bleuart.available() > 0) {
    if (instance->recv_queue_len >= FRAME_QUEUE_SIZE) {
      while (instance->bleuart.available() > 0) {
        instance->bleuart.read();
      }
      BLE_DEBUG_PRINTLN("onBleUartRX: recv queue full, dropping data");
      break;
    }
    
    int avail = instance->bleuart.available();
    
    if (avail > MAX_FRAME_SIZE) {
      BLE_DEBUG_PRINTLN("onBleUartRX: WARN: BLE RX overflow, avail=%d, draining all", avail);
      uint8_t drain_buf[BLE_RX_DRAIN_BUF_SIZE];
      while (instance->bleuart.available() > 0) {
        int chunk = instance->bleuart.available() > BLE_RX_DRAIN_BUF_SIZE ? BLE_RX_DRAIN_BUF_SIZE : instance->bleuart.available();
        instance->bleuart.readBytes(drain_buf, chunk);
      }
      continue;
    }
    
    int read_len = avail;
    instance->recv_queue[instance->recv_queue_len].len = read_len;
    instance->bleuart.readBytes(instance->recv_queue[instance->recv_queue_len].buf, read_len);
    instance->recv_queue_len++;
  }
}

bool SerialBLEInterface::isConnected() const {
  return _isDeviceConnected && Bluefruit.connected() > 0;
}

bool SerialBLEInterface::isWriteBusy() const {
  return send_queue_len >= (FRAME_QUEUE_SIZE * 2 / 3);
}