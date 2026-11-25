#include "target.h"
UnifiedLR1121Board board;
#if defined(UNIFIED_OTA_STOP_BLE) && defined(ENABLE_COMPANION_OTA) && !defined(DISABLE_WIFI_OTA)
#include <helpers/MultiSerialInterface.h>
#include <helpers/esp32/SerialBLEInterface.h>
extern MultiSerialInterface interface_manager;
extern SerialBLEInterface bluetooth_interface;

void unifiedStopBluetoothForOTA() {
  if (bluetooth_interface.isConnected()) bluetooth_interface.disable();
  interface_manager.removeInterface(&bluetooth_interface);
  BLEDevice::deinit(true);
}
#endif

static SPIClass spi(FSPI);
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
WRAPPER_CLASS radio_driver(radio, board);
ESP32RTCClock rtc_clock;
EnvironmentSensorManager sensors;

bool radio_init() {
  rtc_clock.begin();
  spi.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);
  int16_t state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, 5,
      RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE, LORA_TX_POWER, 16,
      LR11X0_DIO3_TCXO_VOLTAGE);
  if (state == RADIOLIB_ERR_NONE) state = radio.setDioAsRfSwitch(15, 0, 4, 8, 8, 2, 0, 1);
  if (state == RADIOLIB_ERR_NONE) state = radio.setCRC(2);
  if (state == RADIOLIB_ERR_NONE) state = radio.explicitHeader();
  if (state == RADIOLIB_ERR_NONE) state = radio.setRxBoostedGainMode(true);
  if (state != RADIOLIB_ERR_NONE) Serial.printf("LR1121 init failed: %d\n", state);
  return state == RADIOLIB_ERR_NONE;
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
