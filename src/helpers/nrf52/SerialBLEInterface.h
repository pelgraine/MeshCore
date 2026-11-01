#pragma once

#include "../BaseSerialInterface.h"
#include <bluefruit.h>

#ifndef BLE_TX_POWER
#define BLE_TX_POWER 4
#endif

// BLE_GARMIN (build flag): make the node usable from a Garmin Connect IQ watch.
//   1. Just-works pairing: the watch cannot enter a PIN, so the UART
//      characteristics require an encrypted but unauthenticated link. The
//      static PIN is not set up at all (Bluefruit's setPIN() forces MITM on).
//   2. Small-MTU slicing: the watch never raises the ATT MTU above 23, so
//      frames travel as tagged 20-byte slices in both directions.
//   3. Sensor advertising: the watch's own Connect IQ app never starts pairing
//      with an unknown node, but its Sensors & Accessories menu does. The node
//      also advertises as a heart-rate sensor so that menu lists it; the bond
//      made there is then used by the watch app. No heart-rate data is sent.
// nRF52 port of the ESP32 BLE_GARMIN transport (pelgraine/MeshCore, garmin
// branch). The DFU service keeps its MITM permissions and is not changed.

class SerialBLEInterface : public BaseSerialInterface {
  BLEDfu bledfu;
  BLEUart bleuart;
#ifdef BLE_GARMIN
  BLEService        hrmSvc;          // Heart Rate service (sensor disguise)
  BLECharacteristic hrmChr;          // Heart Rate Measurement (notify)
  BLECharacteristic bslChr;          // Body Sensor Location (read)
  uint16_t _tx_off;                  // bytes of send_queue[0] already sent as slices (0 = none yet)
  uint8_t  _rx_buf[MAX_FRAME_SIZE];  // reassembly buffer for sliced writes
  int      _rx_len;                  // bytes collected so far (-1 = not mid-frame)
#endif
  bool _isEnabled;
  bool _isDeviceConnected;
  uint16_t _conn_handle;
  unsigned long _last_health_check;
  unsigned long _last_retry_attempt;

  struct Frame {
    uint8_t len;
    uint8_t buf[MAX_FRAME_SIZE];
  };

  #define FRAME_QUEUE_SIZE  12
  
  uint8_t send_queue_len;
  Frame send_queue[FRAME_QUEUE_SIZE];
  
  uint8_t recv_queue_len;
  Frame recv_queue[FRAME_QUEUE_SIZE];

  void clearBuffers();
  void shiftSendQueueLeft();
  void shiftRecvQueueLeft();
  bool isValidConnection(uint16_t handle, bool requireWaitingForSecurity = false) const;
  bool isAdvertising() const;
  static void onConnect(uint16_t connection_handle);
  static void onDisconnect(uint16_t connection_handle, uint8_t reason);
  static void onSecured(uint16_t connection_handle);
  static bool onPairingPasskey(uint16_t connection_handle, uint8_t const passkey[6], bool match_request);
  static void onPairingComplete(uint16_t connection_handle, uint8_t auth_status);
  static void onBLEEvent(ble_evt_t* evt);
  static void onBleUartRX(uint16_t conn_handle);

public:
#ifdef BLE_GARMIN
  SerialBLEInterface()
    : hrmSvc(UUID16_SVC_HEART_RATE),
      hrmChr(UUID16_CHR_HEART_RATE_MEASUREMENT),
      bslChr(UUID16_CHR_BODY_SENSOR_LOCATION) {
    _tx_off = 0; _rx_len = -1;
#else
  SerialBLEInterface() {
#endif
    _isEnabled = false;
    _isDeviceConnected = false;
    _conn_handle = BLE_CONN_HANDLE_INVALID;
    _last_health_check = 0;
    _last_retry_attempt = 0;
    send_queue_len = 0;
    recv_queue_len = 0;
  }

  /**
   * init the BLE interface.
   * @param prefix   a prefix for the device name
   * @param name  IN/OUT - a name for the device (combined with prefix). If "@@MAC", is modified and returned
   * @param pin_code   the BLE security pin
   */
  void begin(const char* prefix, char* name, uint32_t pin_code);

  void disconnect();
  void enable() override;
  void disable() override;
  bool isEnabled() const override { return _isEnabled; }
  bool isConnected() const override;
  bool isWriteBusy() const override;
  size_t writeFrame(const uint8_t src[], size_t len) override;
  size_t checkRecvFrame(uint8_t dest[]) override;
};

#if BLE_DEBUG_LOGGING && ARDUINO
  #include <Arduino.h>
  #define BLE_DEBUG_PRINT(F, ...) Serial.printf("BLE: " F, ##__VA_ARGS__)
  #define BLE_DEBUG_PRINTLN(F, ...) Serial.printf("BLE: " F "\n", ##__VA_ARGS__)
#else
  #define BLE_DEBUG_PRINT(...) {}
  #define BLE_DEBUG_PRINTLN(...) {}
#endif