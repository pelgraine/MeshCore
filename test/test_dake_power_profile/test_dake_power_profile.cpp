#include <gtest/gtest.h>
#include "../../variants/dakefpv_2_4g_nano_pro/DakePowerProfile.h"

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

TEST(DakePowerProfile, MatchesElrsCalibrationPoints) {
  const int8_t output[] = {14, 17, 20, 24, 27, 30};
  const int8_t drive[] = {-17, -13, -9, -5, -2, 5};
  for (unsigned i = 0; i < 6; ++i) {
    EXPECT_EQ(LoRaTxPowerProfile::chipDrive(output[i]), drive[i]);
  }
}

TEST(DakePowerProfile, InterpolatesBetweenCalibrationPoints) {
  EXPECT_EQ(LoRaTxPowerProfile::chipDrive(15), -16);
  EXPECT_EQ(LoRaTxPowerProfile::chipDrive(22), -7);
  EXPECT_EQ(LoRaTxPowerProfile::chipDrive(25), -4);
  EXPECT_EQ(LoRaTxPowerProfile::chipDrive(28), 0);
  EXPECT_EQ(LoRaTxPowerProfile::chipDrive(29), 3);
}

TEST(DakePowerProfile, ClampsBelowMinimumTo25Milliwatts) {
  for (int request = -128; request <= 14; ++request) {
    EXPECT_EQ(LoRaTxPowerProfile::chipDrive(request), -17);
  }
}

TEST(DakePowerProfile, NeverExceedsDriveLimitsAndIsMonotonic) {
  int previous = -17;
  for (int request = -128; request <= 127; ++request) {
    int drive = LoRaTxPowerProfile::chipDrive(request);
    EXPECT_GE(drive, previous);
    EXPECT_GE(drive, -17);
    EXPECT_LE(drive, 5);
    previous = drive;
  }
}
