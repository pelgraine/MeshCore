#include <Arduino.h>
#include "target.h"

ESP32Board board;

static SPIClass spi;
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
WRAPPER_CLASS radio_driver(radio, board);

// NOTE: this board's RTC is a PCF85063, which shares I2C address 0x51 with the
// PCF8563. AutoDiscoverRTCClock has no PCF85063 driver, so its 0x51 probe binds
// the PCF8563 driver to the wrong chip and the clock reads a garbage year-2106
// time that cannot be corrected ('time' refuses to go backwards, and writes land
// in the wrong registers so 'clkreboot' does not stick). Until a proper PCF85063
// driver exists upstream, use the ESP32 internal clock: it starts at 15 May 2024
// on power-up, survives soft reboots, and is set with 'time <epoch>' over serial
// or by the app's clock sync.
ESP32RTCClock rtc_clock;
SensorManager sensors;

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

#ifndef LORA_CR
  #define LORA_CR 5
#endif
#ifndef LORA_PREAMBLE
  #define LORA_PREAMBLE 16
#endif

// LilyGo T3S3 v1.3 LR1121 RF switch table (DIO5 / DIO6), from the LilyGo /
// Meshtastic tlora_t3s3_v1 reference. The LR1121 is multi-band, so this table
// covers BOTH bands: MODE_RX / MODE_TX / MODE_TX_HP are the sub-GHz paths
// (MODE_TX_HP is the high-power PA used at e.g. +20 dBm), and MODE_TX_HF is the
// 2.4 GHz transmit path.
// NOTE: this table is the one knob to adjust if TX/RX misbehaves on a particular
// board revision -- verify on hardware for both the sub-GHz and 2.4 GHz paths.
static const uint32_t rfswitch_dio_pins[] = {
  RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC
};
static const Module::RfSwitchMode_t rfswitch_table[] = {
  // mode                  DIO5  DIO6
  { LR11x0::MODE_STBY,   { LOW,  LOW  } },
  { LR11x0::MODE_RX,     { HIGH, LOW  } },
  { LR11x0::MODE_TX,     { LOW,  HIGH } },
  { LR11x0::MODE_TX_HP,  { LOW,  HIGH } },
  { LR11x0::MODE_TX_HF,  { LOW,  LOW  } },   // 2.4 GHz high-frequency path
  { LR11x0::MODE_GNSS,   { LOW,  LOW  } },
  { LR11x0::MODE_WIFI,   { LOW,  LOW  } },
  END_OF_MODE_TABLE,
};

bool radio_init() {
  rtc_clock.begin();

#ifdef LR11X0_DIO3_TCXO_VOLTAGE
  float tcxo = LR11X0_DIO3_TCXO_VOLTAGE;
#else
  float tcxo = 1.6f;
#endif

  spi.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);

  // TCXO reference voltage on DIO3. RadioLib 7.7.x reads this member during begin()'s
  // modSetup(), so set it before calling begin().
  radio.tcxoVoltage = tcxo;

  // The freq-taking LR11x0 begin() overload hardcodes high=false, which rejects the
  // wide 2.4 GHz bandwidths (e.g. 812.5 kHz). Select high mode from the configured
  // frequency: high band (>1 GHz) for the 2.4 GHz plan, low band for sub-GHz (e.g.
  // the 910.525 MHz US plan). Then set the carrier explicitly via setFrequency().
  bool high = (LORA_FREQ > 1000.0f);
  int status = radio.LR11x0::begin(LORA_BW, LORA_SF, LORA_CR, RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE,
                                   LORA_PREAMBLE, high);
  if (status != RADIOLIB_ERR_NONE) {
    Serial.print("ERROR: radio init failed: ");
    Serial.println(status);
    return false;  // fail
  }

  radio.setRfSwitchTable(rfswitch_dio_pins, rfswitch_table);

  // setFrequency before setOutputPower so the correct (HF) PA path is selected.
  status = radio.setFrequency(LORA_FREQ);
  if (status != RADIOLIB_ERR_NONE) {
    Serial.print("ERROR: setFrequency failed: ");
    Serial.println(status);
    return false;
  }

  radio.setOutputPower(LORA_TX_POWER);
  radio.setRegulatorDCDC();
  radio.setCRC(2);

#ifdef LR11X0_RX_BOOSTED_GAIN
  radio.setRxBoostedGainMode(LR11X0_RX_BOOSTED_GAIN);
#else
  radio.setRxBoostedGainMode(true);
#endif

  return true;  // success
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}
