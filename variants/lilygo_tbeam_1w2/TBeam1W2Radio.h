#pragma once

#include <helpers/radiolib/CustomLR2021.h>
#include "variant.h"

// LR2021 radio for the LilyGo T-Beam 1W2.
//
// The 1W2 has a separate external PA on each band:
//   - Sub-GHz: LilyGo's maximum setting is 22.
//   - 2.4 GHz: LilyGo warns the setting must not exceed 8, otherwise the PA
//     may be damaged.
//
// MeshCore keeps one TX power setting for both bands, so this class maps the
// requested power to a value for whichever band the radio is on:
//   - Sub-GHz: requested power, capped at MAX_LORA_TX_POWER.
//   - 2.4 GHz: requested power if it is at or below MAX_LORA_TX_POWER_2G4,
//     otherwise LORA_TX_POWER_2G4.
//
// RadioLib requires setOutputPower() after a frequency change so the correct
// PA is selected, so the mapped power is re-applied whenever the frequency
// moves between the sub-GHz and 2.4 GHz paths.

class TBeam1W2Radio : public CustomLR2021 {
  int8_t _requested_power = LORA_TX_POWER;
  bool _high_freq_applied = false;

  // Same check as upstream CustomLR2021::isHighFreqPath(), which this branch's
  // CustomLR2021 does not have.
  bool isHighFreqPath() const { return getFreqMHz() > RADIOLIB_LR2021_LF_CUTOFF_FREQ; }

  int8_t powerForBand(int8_t power) {
    if (isHighFreqPath()) {
      return (power > MAX_LORA_TX_POWER_2G4) ? LORA_TX_POWER_2G4 : power;
    }
    return (power > MAX_LORA_TX_POWER) ? MAX_LORA_TX_POWER : power;
  }

public:
  TBeam1W2Radio(Module *mod) : CustomLR2021(mod) { }

  using LR2021::setOutputPower;
  using LR2021::setFrequency;

  int16_t setOutputPower(int8_t power) override {
    _requested_power = power;
    _high_freq_applied = isHighFreqPath();
    return LR2021::setOutputPower(powerForBand(power));
  }

  int16_t setFrequency(float freq) override {
    int16_t status = LR2021::setFrequency(freq);
    if (status == RADIOLIB_ERR_NONE && isHighFreqPath() != _high_freq_applied) {
      _high_freq_applied = isHighFreqPath();
      LR2021::setOutputPower(powerForBand(_requested_power));
    }
    return status;
  }
};
