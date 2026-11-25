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
extern EnvironmentSensorManager sensors;

bool radio_init();
mesh::LocalIdentity radio_new_identity();
