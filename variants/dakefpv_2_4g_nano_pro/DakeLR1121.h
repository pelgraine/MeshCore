#pragma once

#include <helpers/radiolib/CustomLR1121.h>
#include "DakePowerProfile.h"

class DakeLR1121 : public CustomLR1121 {
  bool checkInit(const char* stage, int16_t status) {
    if (status == RADIOLIB_ERR_NONE) return true;
    Serial.printf("ERROR: LR1121 %s failed: %d\n", stage, status);
    reportDevice();
    return false;
  }

public:
  DakeLR1121(Module* mod) : CustomLR1121(mod) { }

  void reportDevice() {
    LR11x0VersionInfo_t version = {};
    int16_t status = getVersionInfo(&version);
    if (status == RADIOLIB_ERR_NONE) {
      Serial.printf("LR1121: hardware=0x%02X device=0x%02X firmware=%02X%02X\n",
                    version.hardware, version.device, version.fwMajor, version.fwMinor);
    } else {
      Serial.printf("LR1121: version read failed: %d\n", status);
    }
    // Error flags must be read in standby. Do not reset before reading them.
    status = standby();
    uint16_t errors = 0;
    if (status == RADIOLIB_ERR_NONE) status = getErrors(&errors);
    if (status == RADIOLIB_ERR_NONE) {
      Serial.printf("LR1121: device errors=0x%04X\n", errors);
    } else {
      Serial.printf("LR1121: device error read failed: %d\n", status);
    }
  }

  bool std_init(SPIClass* spi) {
    spi->begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);
    Serial.printf("LR1121: init %.3f MHz, TCXO %.1f V\n",
                  (double)LORA_FREQ, (double)LR11X0_DIO3_TCXO_VOLTAGE);
#ifdef LORA_CR
    const uint8_t cr = LORA_CR;
#else
    const uint8_t cr = 5;
#endif
    // The pinned LR1120::begin selects legacy HF bandwidths whenever freq >
    // 1000 MHz. Bootstrap with a supported legacy BW, then apply the desired
    // native or legacy bandwidth explicitly (as the runtime wrapper does).
    if (!checkInit("begin", begin(LORA_FREQ, 406.25f, LORA_SF, cr,
        RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE, LORA_TX_POWER, 16,
        LR11X0_DIO3_TCXO_VOLTAGE))) return false;
    const float bw = LORA_BW;
    const bool legacy = fabsf(bw - 203.125f) <= 0.001f ||
                        fabsf(bw - 406.25f) <= 0.001f ||
                        fabsf(bw - 812.5f) <= 0.001f;
    if (!checkInit("bandwidth", setBandwidth(bw, legacy))) return false;
    if (!checkInit("DCDC regulator", setRegulatorDCDC())) return false;
    if (!checkInit("CRC", setCRC(2))) return false;
    if (!checkInit("explicit header", explicitHeader())) return false;
#ifdef RX_BOOSTED_GAIN
    if (!checkInit("boosted RX", setRxBoostedGainMode(true))) return false;
#endif
    reportDevice();
    return true;
  }

  int16_t setFrequency(float freq) override {
    // The chip is dual band, but this board's RF frontend is 2.4 GHz only.
    if (freq < 2400.0f || freq > 2500.0f) return RADIOLIB_ERR_INVALID_FREQUENCY;
    return CustomLR1121::setFrequency(freq);
  }

  int16_t setOutputPower(int8_t power) override {
    return CustomLR1121::setOutputPower(LoRaTxPowerProfile::chipDrive(power));
  }
};
