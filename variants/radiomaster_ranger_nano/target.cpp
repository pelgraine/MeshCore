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
  #ifdef RANGER_NANO_TXDEBUG
    // Idle (listening) state of the PA/LNA pins: expect RXEN=1 TXEN=0, both outputs.
    uint8_t p = RangerNanoBoard::readPaPins(), e = RangerNanoBoard::readPaPinsEnabled();
    Serial.printf(" | now RXEN(32)=%d TXEN(33)=%d, outputs %d/%d",
                  p & 1, (p >> 1) & 1, e & 1, (e >> 1) & 1);
  #endif
    Serial.println();
  }
}
#endif

#ifdef RANGER_NANO_TXDEBUG
// One line per send, called from RangerNanoBoard::onAfterTransmit().
//   during TX = pins sampled 20 ms into the send (expect RXEN=0 TXEN=1)
//   after TX  = pins once the send has finished (radio back in standby)
//   chip power = last value passed to RadioLib after the 0 dBm ceiling, and its result code
void ranger_txdebug_report(uint32_t n, int mid_pins, uint8_t after_pins, uint32_t ms) {
  if (mid_pins < 0) {
    Serial.printf("[txdbg] send #%lu: during TX not sampled (send ended or failed before 20 ms)", (unsigned long)n);
  } else {
    Serial.printf("[txdbg] send #%lu: during TX RXEN(32)=%d TXEN(33)=%d",
                  (unsigned long)n, mid_pins & 1, (mid_pins >> 1) & 1);
  }
  Serial.printf(" | after RXEN=%d TXEN=%d | chip power %d dBm (RadioLib result %d) | rxgain %s | %lu ms\n",
                after_pins & 1, (after_pins >> 1) & 1,
                (int)radio.getAppliedPower(), (int)radio.getAppliedPowerErr(),
                radio.getRxBoostedGainMode() ? "on" : "off", (unsigned long)ms);
}
#endif

bool radio_init() {
  // PA cooling fan (ExpressLRS misc_fan_en = GPIO 27): off at boot, on while
  // transmitting, off FAN_HOLD_SECS after the last transmission (RangerNanoBoard.h).
  board.fanBegin();

  // Logo RGB LED (ExpressLRS led_rgb = GPIO 15, WS2812-type addressable LED).
  // Send one "all colours zero" frame to switch it off. Remove PIN_LOGO_RGB from
  // platformio.ini to leave the LED alone.
#ifdef PIN_LOGO_RGB
  neopixelWrite(PIN_LOGO_RGB, 0, 0, 0);
#endif

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
