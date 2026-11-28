#include <Arduino.h>
#include "target.h"

TBeam1W2Board board;

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

static SPIClass spi;

// The SD card shares the radio's SPI pins, so it must use this same SPIClass
// (SD_SETTINGS_BACKUP in examples/companion_radio/main.cpp).
SPIClass& getRadioSPI() { return spi; }

RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);

WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);

#if ENV_INCLUDE_GPS
  #include <helpers/sensors/MicroNMEALocationProvider.h>
  MicroNMEALocationProvider nmea = MicroNMEALocationProvider(Serial1, &rtc_clock);
  EnvironmentSensorManager sensors = EnvironmentSensorManager(nmea);
#else
  EnvironmentSensorManager sensors;
#endif

// LR2021 RF switch table (LilyGo TBeam1W LR2021_PingPong example)
static const uint32_t rfswitch_dios[] = {
  RADIOLIB_LR2021_DIO6,   // 2.4 GHz RX
  RADIOLIB_LR2021_DIO7,   // 2.4 GHz TX
  RADIOLIB_LR2021_DIO8,   // Sub-GHz TX
  RADIOLIB_NC,
  RADIOLIB_NC,
};

static const Module::RfSwitchMode_t rfswitch_table[] = {
  //                    DIO6  DIO7  DIO8
  { LR2021::MODE_STBY,  { LOW,  LOW,  LOW  } },
  { LR2021::MODE_TX,    { LOW,  LOW,  HIGH } },  // Sub-GHz TX
  { LR2021::MODE_RX,    { LOW,  LOW,  LOW  } },  // Sub-GHz RX
  { LR2021::MODE_RX_HF, { HIGH, LOW,  LOW  } },  // 2.4 GHz RX
  { LR2021::MODE_TX_HF, { LOW,  HIGH, LOW  } },  // 2.4 GHz TX
  END_OF_MODE_TABLE,
};

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);

  // GPS serial initialised by EnvironmentSensorManager::begin()

  if (!radio.std_init(&spi)) return false;

  radio.setRfSwitchTable(rfswitch_dios, rfswitch_table);

  return true;
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
