#include "TdtBmsProtocol.h"
#include <string.h>

#define TDT_RSP_HEAD    0x7E
#define TDT_TAIL        0x0D
#define TDT_HDR_LEN     8      // head, version, 0x01, 0x03, error code, command, length (2 bytes)
#define TDT_MIN_FRAME   10
#define TDT_CELL_POS    8

static uint16_t be16(const uint8_t* p) {
  return ((uint16_t)p[0] << 8) | p[1];
}

uint16_t tdtCrcModbus(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

size_t tdtBuildCommand(uint8_t* out, uint8_t head, uint8_t cmd) {
  out[0] = head;
  out[1] = 0x00;   // version
  out[2] = 0x01;
  out[3] = 0x03;
  out[4] = 0x00;
  out[5] = cmd;
  out[6] = 0x00;   // data length (no data)
  out[7] = 0x00;
  uint16_t crc = tdtCrcModbus(out, 8);
  out[8] = crc >> 8;   // CRC is sent big-endian
  out[9] = crc & 0xFF;
  out[10] = TDT_TAIL;
  return TDT_CMD_FRAME_LEN;
}

void TdtFrameAssembler::reset() {
  _len = 0;
  _expected = 0;
  _complete = false;
}

bool TdtFrameAssembler::feed(const uint8_t* data, size_t len) {
  if (_complete) return true;   // hold the completed frame until reset()
  if (len == 0) return false;

  // a reply frame must start with the reply head; ignore stray data before it
  if (_len == 0 && data[0] != TDT_RSP_HEAD) return false;

  if (_len + len > TDT_MAX_FRAME) {   // overflow, discard
    reset();
    return false;
  }
  memcpy(&_buf[_len], data, len);
  _len += len;

  if (_expected == 0) {
    if (_len < TDT_HDR_LEN) return false;   // header (with data length) not complete yet
    _expected = TDT_MIN_FRAME + be16(&_buf[6]);
    if (_expected > TDT_MAX_FRAME) _expected = TDT_MAX_FRAME;
  }

  if (_len < _expected) return false;   // need more chunks

  if (_buf[_len - 1] == TDT_TAIL && _len >= TDT_MIN_FRAME + 1) {
    uint16_t crc = tdtCrcModbus(_buf, _len - 3);
    if (crc == be16(&_buf[_len - 3])) {
      _complete = true;
      return true;
    }
  }
  if (_len >= _expected + 1) {   // longer than any valid frame of this length, discard
    reset();
  }
  return false;
}

bool tdtIsReplyTo(const uint8_t* frame, size_t len, uint8_t cmd) {
  if (len < TDT_MIN_FRAME + 1) return false;
  if (frame[0] != TDT_RSP_HEAD) return false;
  if (frame[1] != 0x00 && frame[1] != 0x04) return false;   // known reply versions
  if (frame[4] != 0) return false;                            // BMS error code
  return frame[5] == cmd;
}

bool tdtIsFirmwareV1(const uint8_t* frame, size_t len) {
  // version string is in bytes 8..27, NUL padded
  if (len < 28) return false;
  char ver[21];
  memcpy(ver, &frame[8], 20);
  ver[20] = 0;
  size_t n = strlen(ver);
  while (n > 0 && (ver[n - 1] == ' ' || ver[n - 1] == '\t')) n--;   // trim trailing spaces
  // sw_version[:-1].endswith("1.")
  return n >= 3 && ver[n - 3] == '1' && ver[n - 2] == '.';
}

bool tdtDecodeStatus(const uint8_t* frame, size_t len, bool fw_v1, TdtBmsData& out) {
  if (len < TDT_MIN_FRAME + 1) return false;
  size_t msg_len = len - 3;   // drop CRC and tail

  uint8_t cells = frame[TDT_CELL_POS];
  if (cells > TDT_MAX_CELLS) return false;
  size_t temp_count_pos = TDT_CELL_POS + cells * 2 + 1;
  if (temp_count_pos >= msg_len) return false;
  uint8_t temps = frame[temp_count_pos];
  if (temps > TDT_MAX_TEMPS) return false;

  size_t base = TDT_CELL_POS + (size_t)(cells + temps) * 2 + 2;
  if (base + 14 > msg_len) return false;   // need fields up to state of charge

  memset(&out, 0, sizeof(out));

  out.cell_count = cells;
  for (int i = 0; i < cells; i++) {
    out.cell_v[i] = be16(&frame[TDT_CELL_POS + 1 + i * 2]) / 1000.0f;
  }

  out.temp_count = temps;
  float temp_sum = 0;
  for (int i = 0; i < temps; i++) {
    float t = ((int)be16(&frame[temp_count_pos + 1 + i * 2]) - 2730) / 10.0f;
    out.temp_c[i] = t;
    temp_sum += t;
  }
  out.avg_temp_c = temps > 0 ? temp_sum / temps : 0.0f;

  uint16_t raw_current = be16(&frame[base + 0]);
  float current = (float)(raw_current & 0x3FFF);
  if (!fw_v1) current /= 10.0f;
  if (raw_current & 0x8000) current = -current;
  out.current = current;

  out.voltage = be16(&frame[base + 2]) / 100.0f;

  uint16_t raw_remaining = be16(&frame[base + 4]);
  out.remaining_ah = fw_v1 ? (float)raw_remaining : raw_remaining / 10.0f;

  out.cycles = be16(&frame[base + 8]);

  uint16_t raw_design = be16(&frame[base + 10]);
  out.design_ah = fw_v1 ? raw_design : raw_design / 10;

  uint16_t soc = be16(&frame[base + 12]);
  if (soc > 100) return false;
  out.soc = (uint8_t)soc;

  return true;
}
