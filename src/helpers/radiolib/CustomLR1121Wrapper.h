#pragma once

#include "CustomLR1121.h"
#include "RadioLibWrappers.h"
#include "LR11x0Reset.h"

class CustomLR1121Wrapper : public RadioLibWrapper {
public:
  CustomLR1121Wrapper(CustomLR1121& radio, mesh::MainBoard& board) : RadioLibWrapper(radio, board) { }

  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) override {
    // The LR11x0 family only accepts configuration commands in standby. Issued
    // while the chip is receiving they are refused, and because the return
    // codes were discarded the radio silently stayed on its previous plan
    // (tempradio, the automatic revert and app radio changes never took effect
    // until a reboot). Drop to standby first: idle() also flags the wrapper so
    // the dispatcher issues a fresh startReceive() on the new plan.
    RadioLibWrapper::idle();
    CustomLR1121* radio = (CustomLR1121 *)_radio;
    int16_t st = radio->setFrequency(freq);
    if (st != RADIOLIB_ERR_NONE) { MESH_DEBUG_PRINTLN("LR1121 setParams: setFrequency failed (%d)", st); }
    st = radio->setSpreadingFactor(sf);
    if (st != RADIOLIB_ERR_NONE) { MESH_DEBUG_PRINTLN("LR1121 setParams: setSpreadingFactor failed (%d)", st); }
    st = radio->setBandwidth(bw, freq > 1000.0f);   // high-mode BW on the 2.4 GHz band
    if (st != RADIOLIB_ERR_NONE) { MESH_DEBUG_PRINTLN("LR1121 setParams: setBandwidth failed (%d)", st); }
    st = radio->setCodingRate(cr);
    if (st != RADIOLIB_ERR_NONE) { MESH_DEBUG_PRINTLN("LR1121 setParams: setCodingRate failed (%d)", st); }
    updatePreamble(sf);
    PacketMillis pm = calcMaxPacketMillis(sf, bw, cr, preambleLengthForSF(sf));
    radio->setPreambleMillis(pm.preambleMillis);
    radio->setMaxPayloadMillis(pm.payloadMillis);
  }

  bool isReceivingPacket() override {
    return ((CustomLR1121 *)_radio)->isReceiving();
  }
  float getCurrentRSSI() override {
    float rssi = -110;
    ((CustomLR1121 *)_radio)->getRssiInst(&rssi);
    return rssi;
  }

  uint32_t getEstAirtimeFor(int len_bytes) override {
    auto airtime = RadioLibWrapper::getEstAirtimeFor(len_bytes);
    return airtime < 200 ? 200 : airtime;   // at least 200 millis
  }

  void onSendFinished() override {
    RadioLibWrapper::onSendFinished();
    _radio->setPreambleLength(preambleLengthForSF(getSpreadingFactor())); // overcomes weird issues with small and big pkts
  }

  float getLastRSSI() const override { return ((CustomLR1121 *)_radio)->getRSSI(); }
  float getLastSNR() const override { return ((CustomLR1121 *)_radio)->getSNR(); }

  uint8_t getSpreadingFactor() const override { return ((CustomLR1121 *)_radio)->getSpreadingFactor(); }
  
  bool setRxBoostedGainMode(bool en) override {
    return ((CustomLR1121 *)_radio)->setRxBoostedGainMode(en) == RADIOLIB_ERR_NONE;
  }
  bool getRxBoostedGainMode() const override {
    return ((CustomLR1121 *)_radio)->getRxBoostedGainMode();
  }

  void doResetAGC() override { lr11x0ResetAGC((LR11x0 *)_radio, ((CustomLR1121 *)_radio)->getFreqMHz(), getRxBoostedGainMode()); }
};