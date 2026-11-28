#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <helpers/ESP32Board.h>
#include "variant.h"

// LilyGo T-Beam 1W2 with LR2021 + external PAs (LilyGo order code H768-B)
//
// Power and RF control (LilyGo-LoRa-Series docs, t_beam_1w_lr2021):
//   - LDO_EN (GPIO 40): HIGH turns the radio on, LOW turns it off
//   - TCXO: 3.0V (LR2021_TCXO_VOLTAGE)
//   - RF switch: LR2021 DIO6/DIO7/DIO8 via RadioLib RF switch table (target.cpp)
//       Sub-GHz TX: DIO8 HIGH   Sub-GHz RX: all LOW
//       2.4 GHz TX: DIO7 HIGH   2.4 GHz RX: DIO6 HIGH
//   - IRQ: GPIO 1 = LR2021 DIO10
//   - GPIO 21 = LR2021 DIO11 (not driven by the MCU)
//
// Power notes (LilyGo):
//   - The battery must support 2A+ discharge, otherwise battery protection may
//     trigger during high-power TX
//   - 2.4 GHz power setting must not exceed 8dBm (see TBeam1W2Radio.h)

class TBeam1W2Board : public ESP32Board {
public:
  enum FanMode { FAN_ON, FAN_OFF, FAN_AUTO };

private:
  bool radio_powered = false;
  bool _stopped = false;
  KeyValueStore* _prefs = nullptr;
  FanMode _mode = FAN_AUTO;
  int _lo_c = FAN_DEFAULT_LO_C;
  int _hi_c = FAN_DEFAULT_HI_C;
  bool _thermal_on = false;  // onoff hysteresis; TX boost must not latch this
  bool _fan_on = true;
  float _temp_c = NAN;
  bool _tx_active = false;
  bool _tx_cooldown_active = false;
  uint32_t _tx_until_ms = 0;
  TaskHandle_t _fan_task = nullptr;
  mutable portMUX_TYPE _fan_mux = portMUX_INITIALIZER_UNLOCKED;

  void startFanTask();
  void updateFan();
  void setFanOutputLocked(bool enabled);
  float readNtcTempC();
  int cooldownSecsLocked();
  bool isTxCoolingLocked(uint32_t now);
  bool ntcImplausible(float temp_c) const;
  bool persistKey(const char* key, const char* value);
  static bool parseIntArg(const char* text, int& value);
  void loadFanPrefs();
  const char* modeNameLocked() const;
  static void fanTaskThunk(void* arg);

public:
  void begin();
  void attachDynamicPrefs(KeyValueStore* prefs);
  bool handleCommand(const char* command, uint32_t sender_timestamp, char* reply) override;
  void onBeforeTransmit() override;
  void onAfterTransmit() override;
  uint16_t getBattMilliVolts() override;
  const char* getManufacturerName() const override;
  void powerOff() override;

  void setFanEnabled(bool enabled);
  bool isFanEnabled() const;
};
