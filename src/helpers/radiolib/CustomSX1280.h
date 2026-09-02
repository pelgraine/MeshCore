#pragma once

#include <RadioLib.h>
#include "MeshCore.h"

// Hard ceiling for the SX1280 chip output, in dBm. Boards with an external FEM
// (e.g. LilyGo T3-S3 SX1280PA) set this to the FEM's safe input level so that no
// code path -- app command, serial 'set tx', prefs file -- can overdrive it.
// Without a definition it defaults to the SX1280's own +13 dBm maximum.
#ifndef SX128X_MAX_CHIP_DBM
  #ifdef MAX_LORA_TX_POWER
    #define SX128X_MAX_CHIP_DBM MAX_LORA_TX_POWER
  #else
    #define SX128X_MAX_CHIP_DBM 13
  #endif
#endif

class CustomSX1280 : public SX1280 {
  uint32_t _preambleMillis = 66;
  uint32_t _maxPayloadMillis = 3934;
  uint32_t _activityAt = 0;
  bool _headerSeen = false;
  uint8_t _sf = 0;          // tracked here: RadioLib keeps the SX128x SF as a raw register value
  bool _hiSens = false;     // high-sensitivity (LNA boost) mode, the SX128x analogue of boosted RX gain

  public:
    CustomSX1280(Module *mod) : SX1280(mod) { }

  #ifdef RP2040_PLATFORM
    bool std_init(SPIClassRP2040* spi = NULL)
  #else
    bool std_init(SPIClass* spi = NULL)
  #endif
    {
  #ifdef LORA_CR
      uint8_t cr = LORA_CR;
  #else
      uint8_t cr = 5;
  #endif

  #if defined(P_LORA_SCLK)
    #ifdef NRF52_PLATFORM
      if (spi) { spi->setPins(P_LORA_MISO, P_LORA_SCLK, P_LORA_MOSI); spi->begin(); }
    #elif defined(RP2040_PLATFORM)
      if (spi) {
        spi->setMISO(P_LORA_MISO);
        spi->setSCK(P_LORA_SCLK);
        spi->setMOSI(P_LORA_MOSI);
        spi->begin();
      }
    #else
      if (spi) spi->begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);
    #endif
  #endif

      // RADIOLIB_SX128X_SYNC_WORD_PRIVATE == 0x12 (the MeshCore "private" network word).
      // Preamble is the MeshCore default of 16 symbols; the wrapper re-applies the
      // SF-derived length after every transmit, so no LORA_PREAMBLE override here.
      int status = begin(LORA_FREQ, LORA_BW, LORA_SF, cr, RADIOLIB_SX128X_SYNC_WORD_PRIVATE, LORA_TX_POWER, 16);
      if (status != RADIOLIB_ERR_NONE) {
        Serial.print("ERROR: radio init failed: ");
        Serial.println(status);
        return false;  // fail
      }
      _sf = LORA_SF;

      setCRC(2);

      // External RF switch / FEM control lines (RXEN, TXEN). Wired right after
      // begin(), which never transmits, so the FEM sits idle (both lines low)
      // until RadioLib drives them for the first real RX or TX.
  #if defined(SX128X_RXEN) || defined(SX128X_TXEN)
    #ifndef SX128X_RXEN
      #define SX128X_RXEN RADIOLIB_NC
    #endif
    #ifndef SX128X_TXEN
      #define SX128X_TXEN RADIOLIB_NC
    #endif
      setRfSwitchPins(SX128X_RXEN, SX128X_TXEN);
  #endif

  #ifdef SX128X_HIGH_SENSITIVITY
      setRxBoostedGainMode(SX128X_HIGH_SENSITIVITY);
  #endif

      MESH_DEBUG_PRINTLN("SX1280 init OK, chip output ceiling %d dBm", (int)SX128X_MAX_CHIP_DBM);
      return true;  // success
    }

    // Clamp every power request to the chip ceiling. RadioLib calls this virtual
    // from begin() as well, so the initial LORA_TX_POWER is covered too.
    int16_t setOutputPower(int8_t pwr) override {
      if (pwr > SX128X_MAX_CHIP_DBM) {
        MESH_DEBUG_PRINTLN("SX1280 setOutputPower(%d) clamped to %d dBm ceiling", (int)pwr, (int)SX128X_MAX_CHIP_DBM);
        pwr = SX128X_MAX_CHIP_DBM;
      }
      return SX1280::setOutputPower(pwr);
    }

    int16_t setSpreadingFactor(uint8_t sf) {
      _sf = sf;
      return SX1280::setSpreadingFactor(sf);
    }
    uint8_t getSpreadingFactor() const { return _sf; }

    int16_t setRxBoostedGainMode(bool en) {
      int16_t st = setHighSensitivityMode(en);
      if (st == RADIOLIB_ERR_NONE) _hiSens = en;
      return st;
    }
    bool getRxBoostedGainMode() const { return _hiSens; }

    int16_t startReceive() override {
      // include the PREAMBLE_DETECTED irq bit in reported flags
      return SX1280::startReceive(RADIOLIB_SX128X_RX_TIMEOUT_INF, RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED), RADIOLIB_IRQ_RX_DEFAULT_MASK, 0);
    }

    bool isReceiving() {
      uint32_t irq = getIrqFlags();
      bool preamble = irq & RADIOLIB_SX128X_IRQ_PREAMBLE_DETECTED; // bit 15
      bool header   = irq & RADIOLIB_SX128X_IRQ_HEADER_VALID;      // bit 4
      bool hdrErr   = irq & RADIOLIB_SX128X_IRQ_HEADER_ERROR;      // bit 5
      uint32_t now  = millis();
      if (hdrErr) {
        clearIrqFlags(RADIOLIB_SX128X_IRQ_PREAMBLE_DETECTED | RADIOLIB_SX128X_IRQ_HEADER_VALID | RADIOLIB_SX128X_IRQ_HEADER_ERROR | RADIOLIB_SX128X_IRQ_SYNC_WORD_VALID);
        _activityAt = 0;
        _headerSeen = false;
        return false;
      }
      if (!header && _headerSeen) {
        // something cleared the header flag, reset our state.
        _activityAt = 0; _headerSeen = false;
        return false;
      }

      if (header) {
        if (!_headerSeen) { _headerSeen = true; _activityAt = now; };
        if (now - _activityAt > _maxPayloadMillis) {
          MESH_DEBUG_PRINTLN("Clearing header IRQ after %ums", _maxPayloadMillis);
          clearIrqFlags(RADIOLIB_SX128X_IRQ_PREAMBLE_DETECTED | RADIOLIB_SX128X_IRQ_HEADER_VALID | RADIOLIB_SX128X_IRQ_HEADER_ERROR | RADIOLIB_SX128X_IRQ_SYNC_WORD_VALID);
          _activityAt = 0; _headerSeen = false;
          return false;
        }
        return true;
      }
      if (preamble) {
        if (_activityAt == 0) _activityAt = now;
        if (now - _activityAt > _preambleMillis) {
          clearIrqFlags(RADIOLIB_SX128X_IRQ_PREAMBLE_DETECTED);
          _activityAt = 0;
          MESH_DEBUG_PRINTLN("Clearing preamble IRQ after %ums", _preambleMillis);
          return false;
        }
        return true;
      }
      _activityAt = 0; _headerSeen = false;
      return false;
    }

    void setPreambleMillis(uint32_t preambleMillis) {
      _preambleMillis = preambleMillis;
      MESH_DEBUG_PRINTLN("Set _preambleMillis=%u", _preambleMillis);
    }
    void setMaxPayloadMillis(uint32_t payloadMillis) {
      _maxPayloadMillis = payloadMillis;
      MESH_DEBUG_PRINTLN("Set _maxPayloadMillis=%u", _maxPayloadMillis);
    }
};
