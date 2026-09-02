#pragma once

#include "CustomSX1280.h"
#include "RadioLibWrappers.h"

#ifndef USE_SX1280
#define USE_SX1280
#endif

class CustomSX1280Wrapper : public RadioLibWrapper {
public:
  CustomSX1280Wrapper(CustomSX1280& radio, mesh::MainBoard& board) : RadioLibWrapper(radio, board) { }

  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) override {
    // Apply retunes from standby and let the dispatcher issue a fresh
    // startReceive() on the new plan (idle() drops to standby and flags the
    // wrapper). Every setter result is checked so a refused command can never
    // leave the radio silently on its previous plan.
    RadioLibWrapper::idle();
    CustomSX1280* radio = (CustomSX1280 *)_radio;
    int16_t st = radio->setFrequency(freq);
    if (st != RADIOLIB_ERR_NONE) { MESH_DEBUG_PRINTLN("SX1280 setParams: setFrequency failed (%d)", st); }
    st = radio->setSpreadingFactor(sf);
    if (st != RADIOLIB_ERR_NONE) { MESH_DEBUG_PRINTLN("SX1280 setParams: setSpreadingFactor failed (%d)", st); }
    st = radio->setBandwidth(bw);
    if (st != RADIOLIB_ERR_NONE) { MESH_DEBUG_PRINTLN("SX1280 setParams: setBandwidth failed (%d)", st); }
    st = radio->setCodingRate(cr);
    if (st != RADIOLIB_ERR_NONE) { MESH_DEBUG_PRINTLN("SX1280 setParams: setCodingRate failed (%d)", st); }
    updatePreamble(sf);
    PacketMillis pm = calcMaxPacketMillis(sf, bw, cr, preambleLengthForSF(sf));
    radio->setPreambleMillis(pm.preambleMillis);
    radio->setMaxPayloadMillis(pm.payloadMillis);
  }

  bool isReceivingPacket() override {
    return ((CustomSX1280 *)_radio)->isReceiving();
  }
  float getCurrentRSSI() override {
    return ((CustomSX1280 *)_radio)->getRSSI(false);   // instantaneous RSSI
  }
  float getLastRSSI() const override { return ((CustomSX1280 *)_radio)->getRSSI(); }
  float getLastSNR() const override { return ((CustomSX1280 *)_radio)->getSNR(); }

  float packetScore(float snr, int packet_len) override {
    int sf = ((CustomSX1280 *)_radio)->getSpreadingFactor();
    return packetScoreInt(snr, sf, packet_len);
  }
  uint8_t getSpreadingFactor() const override { return ((CustomSX1280 *)_radio)->getSpreadingFactor(); }

  void onSendFinished() override {
    RadioLibWrapper::onSendFinished();
    _radio->setPreambleLength(preambleLengthForSF(getSpreadingFactor())); // overcomes weird issues with small and big pkts
  }

  // Standby only, no sleep(): LilyGo's own T-Watch S3 code skips radio.sleep()
  // on its SX1280 revision ("SX1280 died here, the reason is not analyzed yet"),
  // and the Meck Watch build does the same. That evidence is for the watch; on
  // the T3-S3 PA it is applied as a precaution for the same chip, as GrayHatGuy's
  // T3-S3 SX1280 variant also did. Standby parks the FEM lines low as well.
  virtual void powerOff() override {
    ((CustomSX1280 *)_radio)->standby();
  }
  void doResetAGC() override {
    ((CustomSX1280 *)_radio)->standby();
  }

  bool setRxBoostedGainMode(bool en) override {
    return ((CustomSX1280 *)_radio)->setRxBoostedGainMode(en) == RADIOLIB_ERR_NONE;
  }
  bool getRxBoostedGainMode() const override {
    return ((CustomSX1280 *)_radio)->getRxBoostedGainMode();
  }
};