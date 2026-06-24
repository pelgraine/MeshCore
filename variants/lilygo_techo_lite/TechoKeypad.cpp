// TechoKeypad.cpp
// Self-contained TCA8418 keypad driver for the T-Echo Lite KeyShield.
//
// Provides a single free function consumed by the UITask hook:
//     char techo_keypad_read();   // UIScreen KEY_* code or ASCII char, 0 if none
//
// Raw-Wire register driver (ported style from the Meck T-Deck Max TCA8418
// driver). Knows nothing about text entry / T9 — it only reports which key
// was pressed. All compose logic lives in the screen.

#ifdef TECHO_KEYPAD

#include <Arduino.h>
#include <Wire.h>
#include <helpers/ui/UIScreen.h>   // KEY_PREV / KEY_NEXT / KEY_ENTER / KEY_CANCEL

// ---- TCA8418 (KeyShield keypad) --------------------------------------------
// I2C 0x34 on the shared Wire bus (PIN_WIRE_SDA P1.04 / PIN_WIRE_SCL P1.02).
// Wire.begin() (TechoBoard::begin) and the shield 3V3 rail (RT9080, PIN_PWR_EN
// P0.30, driven HIGH in initVariant) are both up before the UI loop runs, so
// this lazy-inits on first read.
#define TCA8418_ADDR            0x34
#define TCA8418_REG_CFG         0x01
#define TCA8418_REG_INT_STAT    0x02
#define TCA8418_REG_KEY_LCK_EC  0x03
#define TCA8418_REG_KEY_EVENT_A 0x04
#define TCA8418_REG_KP_GPIO1    0x1D
#define TCA8418_REG_KP_GPIO2    0x1E
#define TCA8418_REG_KP_GPIO3    0x1F
#define TCA8418_REG_GPI_EM1     0x20
#define TCA8418_REG_GPI_EM2     0x21
#define TCA8418_REG_GPI_EM3     0x22
#define TCA8418_REG_DEBOUNCE    0x29

static void tca_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA8418_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static uint8_t tca_read(uint8_t reg) {
  Wire.beginTransmission(TCA8418_ADDR);
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom((uint8_t)TCA8418_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

// Map a TCA8418 key-event number to a UI value.
// Matrix is 5 rows x 4 cols; event number = row*10 + col + 1.
// Layout (from the LilyGo KeyShield Tca8418_Map):
//   r0: Yes  *    0    #       r1: No   7    8    9
//   r2: Down 4    5    6       r3: Cent 1    2    3
//   r4: Up   Esc  Home Mail
static char techo_keypad_map(uint8_t num) {
  switch (num) {
    case  2: return '*';  case  3: return '0';  case  4: return '#';
    case 12: return '7';  case 13: return '8';  case 14: return '9';
    case 22: return '4';  case 23: return '5';  case 24: return '6';
    case 32: return '1';  case 33: return '2';  case 34: return '3';
    case 41: return KEY_PREV;    // Up
    case 21: return KEY_NEXT;    // Down
    case 31: return KEY_ENTER;   // Center
    case 42: return KEY_CANCEL;  // Esc
    case  1: return ' ';                // square ("Yes") -> space
    case 11: return '\b';               // X ("No")       -> backspace
    case 44: return KEY_CONTEXT_MENU;   // Mail -> open compose / channel select
    case 43: return KEY_HOME;           // Home -> toggle keyshield backlight
    default: return 0;
  }
}

static void techo_keypad_init() {
  // The TCA8418 has no reset line and stays powered across MCU resets, so the
  // scanner may still be running from a previous session. Quiet it, drain any
  // stale events, then configure the 5x4 matrix and re-enable.
  tca_write(TCA8418_REG_CFG, 0x00);                 // scanner off
  for (int i = 0; i < 16; i++) {
    if ((tca_read(TCA8418_REG_KEY_LCK_EC) & 0x0F) == 0) break;
    tca_read(TCA8418_REG_KEY_EVENT_A);
  }
  tca_write(TCA8418_REG_INT_STAT, 0x1F);            // clear all int flags
  tca_write(TCA8418_REG_GPI_EM1, 0x00);
  tca_write(TCA8418_REG_GPI_EM2, 0x00);
  tca_write(TCA8418_REG_GPI_EM3, 0x00);
  tca_write(TCA8418_REG_KP_GPIO1, 0x1F);            // ROW0-ROW4 in keypad matrix
  tca_write(TCA8418_REG_KP_GPIO2, 0x0F);            // COL0-COL3 in keypad matrix
  tca_write(TCA8418_REG_KP_GPIO3, 0x00);
  tca_write(TCA8418_REG_DEBOUNCE, 0x03);            // mirrors Meck init
  tca_write(TCA8418_REG_INT_STAT, 0x1F);
  tca_write(TCA8418_REG_CFG, 0x11);                 // KE_IEN + INT_CFG, scanner on
  delay(5);
  for (int i = 0; i < 16; i++) {
    if ((tca_read(TCA8418_REG_KEY_LCK_EC) & 0x0F) == 0) break;
    tca_read(TCA8418_REG_KEY_EVENT_A);
  }
  tca_write(TCA8418_REG_INT_STAT, 0x1F);
}

char techo_keypad_read() {
  static bool inited = false;
  if (!inited) { techo_keypad_init(); inited = true; }

  if ((tca_read(TCA8418_REG_KEY_LCK_EC) & 0x0F) == 0) return 0;  // FIFO empty

  uint8_t ev = tca_read(TCA8418_REG_KEY_EVENT_A);
  tca_write(TCA8418_REG_INT_STAT, 0x1F);            // clear int

  if ((ev & 0x80) == 0) return 0;                   // key release -> ignore
  return techo_keypad_map(ev & 0x7F);               // key press -> mapped value
}

#endif // TECHO_KEYPAD