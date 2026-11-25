#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include "DakeBoard.h"
#include "DakeLR1121.h"
#include "UnifiedLR1121Wrapper.h"
#include <helpers/sensors/EnvironmentSensorManager.h>

extern DakeBoard board;
extern WRAPPER_CLASS radio_driver;
extern ESP32RTCClock rtc_clock;
#ifdef UNIFIED_I2C
extern EnvironmentSensorManager sensors;
#else
// Builds without the INA219 have no I2C pins: the plain manager skips the start-up I2C scan.
extern SensorManager sensors;
#endif

bool radio_init();
mesh::LocalIdentity radio_new_identity();
