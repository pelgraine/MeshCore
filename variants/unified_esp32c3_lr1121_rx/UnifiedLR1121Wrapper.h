#pragma once

#include "UnifiedBoard.h"
#include <helpers/radiolib/CustomLR1121Wrapper.h>

class UnifiedLR1121Wrapper : public CustomLR1121Wrapper {
  UnifiedBoard& board;
public:
  UnifiedLR1121Wrapper(CustomLR1121& radio, UnifiedBoard& board)
      : CustomLR1121Wrapper(radio, board), board(board) { }

  int recvRaw(uint8_t* bytes, int sz) override {
    int len = CustomLR1121Wrapper::recvRaw(bytes, sz);
    if (len > 0) board.onPacketReceived();
    board.updatePacketLed();
    return len;
  }

  void loop() override {
    CustomLR1121Wrapper::loop();
    board.updatePacketLed();
  }
};
