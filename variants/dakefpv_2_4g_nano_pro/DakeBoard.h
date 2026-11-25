#pragma once

#include "UnifiedBoard.h"

class DakeBoard : public UnifiedBoard {
public:
  const char* getManufacturerName() const override { return "DakeFPV 2.4G Nano Pro"; }
};
