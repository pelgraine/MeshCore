#pragma once

// Wattcycle LiFePO4 batteries use a "TDT smart BMS". The frame layout, unlock string,
// command codes and field offsets below follow the open-source aiobmsble decoder
// (aiobmsble/bms/tdt_bms.py, v0.33.0, Apache-2.0), and were checked against frames
// captured from a WattCycle 12V 100Ah trolling motor battery (XDZN_001_E4E7).
//
// This file has no Arduino dependencies, so it can be unit tested on a PC.

#include <stdint.h>
#include <stddef.h>

#define TDT_CMD_INFO        0x92   // device info (firmware version string, serial number)
#define TDT_CMD_STATUS      0x8C   // cells, temperatures, current, voltage, charge

#define TDT_HEAD_ALT        0x1E   // command head that XDZN_001_E4E7 answers to
#define TDT_HEAD_DEFAULT    0x7E   // aiobmsble's default command head; also the reply head

#define TDT_CMD_FRAME_LEN   11     // length of a (data-less) command frame
#define TDT_MAX_FRAME       255
#define TDT_MAX_CELLS       16
#define TDT_MAX_TEMPS       8

struct TdtBmsData {
  float    voltage;        // V
  float    current;        // A, negative while discharging
  float    remaining_ah;   // Ah
  uint16_t design_ah;      // Ah
  uint16_t cycles;
  uint8_t  soc;            // state of charge, %
  uint8_t  cell_count;
  float    cell_v[TDT_MAX_CELLS];
  uint8_t  temp_count;
  float    temp_c[TDT_MAX_TEMPS];
  float    avg_temp_c;     // mean of temp_c[]
};

// Modbus CRC-16 (poly 0xA001, init 0xFFFF)
uint16_t tdtCrcModbus(const uint8_t* data, size_t len);

// Builds a read command frame into 'out' (must hold TDT_CMD_FRAME_LEN bytes). Returns the length.
size_t tdtBuildCommand(uint8_t* out, uint8_t head, uint8_t cmd);

// Reassembles a reply frame from BLE notification chunks.
class TdtFrameAssembler {
  uint8_t _buf[TDT_MAX_FRAME];
  size_t  _len;
  size_t  _expected;
  bool    _complete;
public:
  TdtFrameAssembler() { reset(); }
  void reset();
  // Feeds one notification. Returns true once a complete frame with a valid tail and CRC is held.
  bool feed(const uint8_t* data, size_t len);
  bool isComplete() const { return _complete; }
  const uint8_t* frame() const { return _buf; }
  size_t frameLen() const { return _len; }
};

// True if a complete reply frame answers 'cmd' with no error code.
bool tdtIsReplyTo(const uint8_t* frame, size_t len, uint8_t cmd);

// From a 0x92 reply: true if the firmware version string marks the "1.x" field layout
// (aiobmsble: sw_version[:-1].endswith("1.")).
bool tdtIsFirmwareV1(const uint8_t* frame, size_t len);

// Decodes a 0x8C reply. Returns false if the frame is too short or the values are out of range.
bool tdtDecodeStatus(const uint8_t* frame, size_t len, bool fw_v1, TdtBmsData& out);
