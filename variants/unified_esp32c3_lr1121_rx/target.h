#pragma once

#include <Arduino.h>
#include <RadioLib.h>
#include "UnifiedBoard.h"
#include "UnifiedLR1121Wrapper.h"
#include <helpers/radiolib/CustomLR1121.h>
#include <helpers/sensors/EnvironmentSensorManager.h>

class UnifiedLR1121Board : public UnifiedBoard {
public:
  const char* getManufacturerName() const override { return "Unified ESP32-C3 LR1121 RX"; }
};

class UnifiedLR1121Radio : public CustomLR1121 {
public:
  using CustomLR1121::CustomLR1121;

  int16_t setFrequency(float freq) override {
    if (freq < 863.0f || freq > 928.0f) return RADIOLIB_ERR_INVALID_FREQUENCY;
    return CustomLR1121::setFrequency(freq);
  }
};

extern UnifiedLR1121Board board;
extern WRAPPER_CLASS radio_driver;
extern ESP32RTCClock rtc_clock;
extern EnvironmentSensorManager sensors;
bool radio_init();
mesh::LocalIdentity radio_new_identity();
