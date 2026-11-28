#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/radiolib/CustomLR2021Wrapper.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/sensors/EnvironmentSensorManager.h>
#include "TBeam1W2Board.h"
#include "TBeam1W2Radio.h"

#ifdef DISPLAY_CLASS
  #include <helpers/ui/SH1106Display.h>
  #include <helpers/ui/MomentaryButton.h>
  extern DISPLAY_CLASS display;
  extern MomentaryButton user_btn;
#endif

extern TBeam1W2Board board;
extern WRAPPER_CLASS radio_driver;
extern AutoDiscoverRTCClock rtc_clock;
extern EnvironmentSensorManager sensors;

bool radio_init();
mesh::LocalIdentity radio_new_identity();
