#include <Arduino.h>
#include "target.h"

DakeBoard board;
#if defined(UNIFIED_OTA_STOP_BLE) && defined(ENABLE_COMPANION_OTA) && !defined(DISABLE_WIFI_OTA)
#include <helpers/MultiSerialInterface.h>
#include <helpers/esp32/SerialBLEInterface.h>
extern MultiSerialInterface interface_manager;
extern SerialBLEInterface bluetooth_interface;

void unifiedStopBluetoothForOTA() {
  if (bluetooth_interface.isConnected()) bluetooth_interface.disable();
  interface_manager.removeInterface(&bluetooth_interface);
  BLEDevice::deinit(true);
  Serial.printf("OTA: Bluetooth released (heap=%u, largest=%u)\n",
    ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}
#endif
// Arduino defaults to HSPI, but ESP32-C3 only exposes FSPI.
static SPIClass spi(FSPI);
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
WRAPPER_CLASS radio_driver(radio, board);
ESP32RTCClock rtc_clock;
#ifdef UNIFIED_I2C
EnvironmentSensorManager sensors;
#else
SensorManager sensors;
#endif

// I2C builds hand the RX/TX pads to the sensor, so there is no serial console to print to.
#if defined(DAKE_DIAG) && !defined(UNIFIED_I2C)
#define DAKE_DIAG_ACTIVE 1
#endif

#ifdef DAKE_DIAG_ACTIVE
// Diagnostics: every 30 s, print heap headroom and the fixed memory sizes to the
// serial console on the RX/TX pads (115200 baud). Remove -D DAKE_DIAG from
// platformio.ini to drop it.
//   heap free    = RAM free right now
//   heap min     = lowest free RAM seen since boot (the number that matters for sizing)
//   heap largest = biggest single free block
//   storage      = bytes used / usable bytes on the SPIFFS partition (contacts, channels, adverts, prefs)
// This task's own 3 KB stack comes out of the same heap.
#include <esp_heap_caps.h>
#include <SPIFFS.h>
#include <helpers/ContactInfo.h>
#include <helpers/ChannelDetails.h>
#include <helpers/BaseSerialInterface.h>

static void dake_diag_task(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(30000));
    Serial.printf("[diag] heap free=%u min=%u largest=%u",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  #ifdef MAX_CONTACTS
    Serial.printf(" | contacts=%d x %uB, channels=%d x %uB",
                  MAX_CONTACTS, (unsigned)sizeof(ContactInfo),
                  MAX_GROUP_CHANNELS, (unsigned)sizeof(ChannelDetails));
  #endif
  #ifdef OFFLINE_QUEUE_SIZE
    Serial.printf(" | queue=%d x %uB", OFFLINE_QUEUE_SIZE, (unsigned)(MAX_FRAME_SIZE + 1));
  #endif
    Serial.printf(" | storage used=%u of %u",
                  (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes());
    Serial.println();
  }
}
#endif

bool radio_init() {
#ifdef DAKE_DIAG_ACTIVE
  xTaskCreate(dake_diag_task, "diag", 3072, NULL, 1, NULL);
#endif
  // Use the ESP32 internal clock; optional I2C is initialized by board.begin().
  rtc_clock.begin();
  if (!radio.std_init(&spi)) return false;
  // ExpressLRS default LR1121 switch bytes, with a checked command result.
  int16_t status = radio.setDioAsRfSwitch(15, 0, 4, 8, 8, 2, 0, 1);
  if (status != RADIOLIB_ERR_NONE) {
    Serial.printf("ERROR: LR1121 RF switch failed: %d\n", status);
    radio.reportDevice();
    return false;
  }
  return true;
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
