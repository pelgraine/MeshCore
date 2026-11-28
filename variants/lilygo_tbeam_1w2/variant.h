// LilyGo T-Beam 1W2 variant.h
// LR2021 version of the T-Beam 1W (LilyGo order code H768-B, 830-945 MHz + 2.4 GHz).
// Configuration based on LilyGo-LoRa-Series docs (t_beam_1w_lr2021), the
// TBeam1W LR2021_PingPong example and the T-Beam_1W_V1.1 schematic.

#pragma once

// I2C for OLED display (SH1106 at 0x3C)
#define I2C_SDA 8
#define I2C_SCL 9

// GPS - Quectel L76K
// GNSS_TXD (IO5) = GPS transmits -> MCU RX (setPins rxPin)
// GNSS_RXD (IO6) = GPS receives -> MCU TX (setPins txPin)
#define PIN_GPS_TX 5       // MCU receives from GPS TX
#define PIN_GPS_RX 6       // MCU transmits to GPS RX
#define PIN_GPS_PPS 7      // GPS PPS output
#define PIN_GPS_EN 16      // GPS wake-up/enable
#define HAS_GPS 1
#define GPS_BAUDRATE 9600

// Buttons
#define BUTTON_PIN 0         // BUTTON 1 (boot)
#define BUTTON_PIN_ALT 17    // BUTTON 2

// SPI (shared by LoRa and SD)
#define SPI_MOSI 11
#define SPI_SCK 13
#define SPI_MISO 12
#define SPI_CS 10

// SD Card
#define HAS_SDCARD
#define SDCARD_USE_SPI1
#define SDCARD_CS SPI_CS

// LoRa Radio - LR2021 with Sub-GHz PA and 2.4 GHz PA/LNA
#define LORA_SCK SPI_SCK
#define LORA_MISO SPI_MISO
#define LORA_MOSI SPI_MOSI
#define LORA_CS 15
#define LORA_RESET 3
#define LORA_IRQ 1         // GPIO1 is connected to LR2021 DIO10
#define LORA_BUSY 38
// GPIO21 is connected to LR2021 DIO11. It is not driven by the MCU.

// Radio power enable - MUST be HIGH before radio init.
// GPIO 40 controls the LDO that powers the LR2021 module.
#define RADIO_LDO_EN 40

// 2.4 GHz TX power handling (see TBeam1W2Radio.h).
// LilyGo: "2.4G power setting must not exceed 8dBm, otherwise it may damage the PA."
#ifndef LORA_TX_POWER_2G4
  #define LORA_TX_POWER_2G4 8
#endif
#ifndef MAX_LORA_TX_POWER_2G4
  #define MAX_LORA_TX_POWER_2G4 12
#endif

// LED
#define LED_PIN 18
#define LED_STATE_ON 1  // HIGH = ON

// Battery ADC (300k / 150k divider)
#define BATTERY_PIN 4
#define ADC_CHANNEL ADC1_GPIO4_CHANNEL
#define BATTERY_SENSE_SAMPLES 30
#define ADC_MULTIPLIER 3.0

// NTC thermistor (Murata NCP18XH103F03RB, 10k, B25/50=3380K)
// Divider: 3.3V -> NTC -> GPIO14 -> 10k pull-down -> GND (Vadc rises with temp)
#define NTC_PIN 14
#define NTC_B 3380.0f
#define NTC_R25 10000.0f
#define NTC_R_FIXED 10000.0f
#define NTC_VCC_MV 3300.0f

// Fan control (GPIO41). NTC is PA-adjacent PCB temp, not die temp, so trip
// well below the 85C operating limit. The fan/MOSFET path is on/off.
#define FAN_CTRL_PIN 41
#define FAN_TX_COOLDOWN_MS 15000
#define FAN_DEFAULT_LO_C 30
#define FAN_DEFAULT_HI_C 36

// Display - SH1106 OLED (128x64)
#define USE_SH1106
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
