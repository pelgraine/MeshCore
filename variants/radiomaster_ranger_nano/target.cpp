#include <Arduino.h>
#include "target.h"

RangerNanoBoard board;

static SPIClass spi;
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
WRAPPER_CLASS radio_driver(radio, board);

// No RTC chip on this module (and no I2C bus: GPIO 21/22 are radio pins here),
// so the ESP32 internal clock is used. Set it with 'time <epoch>' over serial
// or by the app's clock sync.
ESP32RTCClock rtc_clock;
SensorManager sensors;

#ifdef RANGER_NANO_DIAG
// Diagnostics: every 30 s, print heap headroom and the ESP32's own chip temperature
// to the USB serial console. Remove -D RANGER_NANO_DIAG from platformio.ini to drop it.
//   heap free    = RAM free right now
//   heap min     = lowest free RAM seen since boot (the number that matters for sizing)
//   heap largest = biggest single free block
//   chip temp    = ESP32 internal sensor via temperatureRead(). It measures the ESP32
//                  die, not the SX1281 or the PA, and has no factory calibration.
#include <esp_heap_caps.h>
#include <helpers/ContactInfo.h>
#include <helpers/ChannelDetails.h>

static void ranger_diag_task(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(30000));
    Serial.printf("[diag] heap free=%u min=%u largest=%u | chip temp=%.1f C",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  temperatureRead());
  #ifdef MAX_CONTACTS
    Serial.printf(" | contacts=%d x %uB, channels=%d x %uB",
                  MAX_CONTACTS, (unsigned)sizeof(ContactInfo),
                  MAX_GROUP_CHANNELS, (unsigned)sizeof(ChannelDetails));
  #endif
    Serial.println();
  }
}
#endif

bool radio_init() {
  // PA cooling fan (ExpressLRS misc_fan_en = GPIO 27): off at boot, on while
  // transmitting, off FAN_HOLD_SECS after the last transmission (RangerNanoBoard.h).
  board.fanBegin();

#ifdef RANGER_NANO_DIAG
  xTaskCreate(ranger_diag_task, "diag", 3072, NULL, 1, NULL);
#endif

  rtc_clock.begin();

  // CustomSX1280::std_init() begins the SPI bus, runs begin() with the LORA_* plan and
  // the private 0x12 sync word, then wires the PA/LNA lines from SX128X_RXEN /
  // SX128X_TXEN (GPIO 32 / 33). Chip drive is clamped to MAX_LORA_TX_POWER (0 dBm).
  return radio.std_init(&spi);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}
