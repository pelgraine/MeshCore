#include <Arduino.h>
#include "target.h"
#include <helpers/ArduinoHelpers.h>
#include <helpers/sensors/MicroNMEALocationProvider.h>

WioTrackerL1Board board;

RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI);

WRAPPER_CLASS radio_driver(radio, board);

VolatileRTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);

#if defined(ENV_INCLUDE_GPS) && defined(WIO_TRACKER_L1_EINK)
// Count NMEA sentences on the way to the parser (shown on the GPS home page),
// and put the L76K into multi-constellation mode each time it is powered up.
GPSStreamCounter gpsStream(Serial1);

class L76KLocationProvider : public MicroNMEALocationProvider {
public:
  L76KLocationProvider(Stream& ser, mesh::RTCClock* clock) : MicroNMEALocationProvider(ser, clock) {}
  void begin() override {
    MicroNMEALocationProvider::begin();   // wakes the module via the standby pin
    delay(300);                           // let it finish booting before it will take config
    Serial1.print("$PCAS04,7*1E\r\n");    // GPS + GLONASS + BeiDou
    gpsStream.resetCounters();
  }
};
L76KLocationProvider nmea = L76KLocationProvider(gpsStream, &rtc_clock);
EnvironmentSensorManager sensors = EnvironmentSensorManager(nmea);
#elif defined(ENV_INCLUDE_GPS)
MicroNMEALocationProvider nmea = MicroNMEALocationProvider(Serial1, &rtc_clock);
EnvironmentSensorManager sensors = EnvironmentSensorManager(nmea);
#else
EnvironmentSensorManager sensors = EnvironmentSensorManager();
#endif

#ifndef USER_BTN_LONG_PRESS_MS
  #define USER_BTN_LONG_PRESS_MS 1000
#endif

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, USER_BTN_LONG_PRESS_MS, true, false, false);
  MomentaryButton joystick_left(JOYSTICK_LEFT, 1000, true, false, false);
  MomentaryButton joystick_right(JOYSTICK_RIGHT, 1000, true, false, false);
  MomentaryButton joystick_up(JOYSTICK_UP, 1000, true, false, false);
  MomentaryButton joystick_down(JOYSTICK_DOWN, 1000, true, false, false);
  MomentaryButton back_btn(PIN_BACK_BTN, 1000, true, false, true);
#endif

bool radio_init() {
  rtc_clock.begin(Wire);

  return radio.std_init(&SPI);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}

