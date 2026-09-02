#include <Arduino.h>
#include "target.h"

ESP32Board board;

static SPIClass spi;
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
WRAPPER_CLASS radio_driver(radio, board);

// NOTE: this board's RTC is a PCF85063, which shares I2C address 0x51 with the
// PCF8563. AutoDiscoverRTCClock has no PCF85063 driver, so its 0x51 probe binds
// the PCF8563 driver to the wrong chip and the clock reads a garbage year-2106
// time that cannot be corrected (same finding as the T3-S3 LR1121 variant).
// Until a proper PCF85063 driver exists upstream, use the ESP32 internal clock:
// it starts at 15 May 2024 on power-up, survives soft reboots, and is set with
// 'time <epoch>' over serial or by the app's clock sync.
ESP32RTCClock rtc_clock;
SensorManager sensors;

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

bool radio_init() {
  rtc_clock.begin();

  // CustomSX1280::std_init() begins the SPI bus (P_LORA_SCLK/MISO/MOSI), runs
  // begin() with the LORA_* plan and the private 0x12 sync word, then wires the
  // FEM control lines from SX128X_RXEN / SX128X_TXEN (GPIO 21 / 10 on the T3-S3).
  // Chip output is clamped to SX128X_MAX_CHIP_DBM inside the class, so the FEM
  // can never be overdriven by any later power command.
  return radio.std_init(&spi);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}
