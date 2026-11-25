#pragma once

#include <stdint.h>

// Nominal conducted output from the ELRS power table, rounded to whole dBm.
// A board-specific profile keeps persisted/UI values in output-power units.
class LoRaTxPowerProfile {
public:
  static int8_t chipDrive(int8_t value) {
    const int8_t output[] = {14, 17, 20, 24, 27, 30};
    const int8_t drive[] = {-17, -13, -9, -5, -2, 5};
    const int8_t* from = output;
    const int8_t* to = drive;
    if (value <= from[0]) return to[0];
    for (unsigned i = 1; i < 6; ++i) {
      if (value <= from[i]) {
        int span = from[i] - from[i - 1];
        int delta = (value - from[i - 1]) * (to[i] - to[i - 1]);
        return to[i - 1] + (delta + span / 2) / span;
      }
    }
    return to[5];
  }
};
