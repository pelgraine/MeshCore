#pragma once
// =============================================================================
// JoystickComposeScreens -- on-device channel messaging for the Seeed Wio
// Tracker L1 E-Ink (nRF52840, 2.13" 250x122 e-ink, four-way joystick with
// centre press, separate menu/back button). Compiled only when
// UI_JOYSTICK_COMPOSE is defined (set in the e-ink variant's platformio.ini).
//
// Three UIScreen subclasses driven entirely by key events from UITask:
//
//   JCChannelPicker  -- long-hold of the joystick press on the home screen's
//                       first page opens this. UP/DOWN move the ">" cursor,
//                       press selects, back button returns home.
//   JCChannelScreen  -- recent messages for the selected channel, read from a
//                       shared ring (JCHistory) that holds the last
//                       JC_HISTORY_SIZE messages across all channels. Lines
//                       are word-wrapped; UP/DOWN scroll. Scrolling past the
//                       newest line moves the highlight onto the Compose box;
//                       press with Compose highlighted opens the keyboard.
//                       Back button returns home.
//   JCKeyboardScreen -- on-screen QWERTY with lower / UPPER / SYM layers.
//                       UP/DOWN/LEFT/RIGHT move the highlighted key, press
//                       types it. Bottom row: mode | SPACE | DEL | SND | ESC.
//                       Back button also acts as ESC (returns to the channel
//                       screen, discarding the draft).
//
// Geometry. The GxEPD driver scales a virtual canvas onto the panel. This
// variant sets EINK_VIRTUAL_H=80 (with EINK_VIRTUAL_W at the 128 default), so
// screens see a 128x80 canvas. All text uses size 0, the built-in 6x8 pixel
// GFX font, which the driver draws in PHYSICAL pixels: on this panel that is
// about 41 columns and a row pitch of 8 virtual units (10 physical) gives
// 10 rows. Two driver quirks matter for layout and are handled here:
//   * setCursor() applies EINK_Y_OFFSET before scaling, drawRect() does not,
//     so a box drawn around a text row must be offset by EINK_Y_OFFSET.
//   * The built-in font positions the cursor at the glyph's TOP-left
//     (unlike the FreeSans fonts used at sizes 1-3, which use the baseline).
// =============================================================================

#ifdef UI_JOYSTICK_COMPOSE

#include <Arduino.h>
#include <string.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/ui/UIScreen.h>

#ifndef EINK_Y_OFFSET
  #define EINK_Y_OFFSET 0
#endif

// ---- tunables ---------------------------------------------------------------
#define JC_HISTORY_SIZE     30    // shared ring across all channels
#define JC_TEXT_LEN         160   // stored per message (channel text arrives as "Sender: body")
#define JC_OUT_BUF_LEN      134   // MeshCore per-channel message cap (~133) + NUL
#define JC_CH_NAME_LEN      32
#ifdef MAX_GROUP_CHANNELS
  #define JC_PICKER_MAX     MAX_GROUP_CHANNELS
#else
  #define JC_PICKER_MAX     20
#endif

// ---- layout (virtual units, text space) --------------------------------------
#define JC_ROW_H            8     // row pitch: 8 virtual = ~10 physical px
#define JC_COLS             41    // characters per row at size 0 (250 / 6)
#define JC_RECT_Y(y)        ((y) + EINK_Y_OFFSET - 1)   // box top for a text row at y
#define JC_RECT_H           11    // box height around one 8px glyph row

// -----------------------------------------------------------------------------
// Shared message history (one ring for every channel)
// -----------------------------------------------------------------------------
struct JCHistoryEntry {
  uint32_t timestamp;
  uint8_t  channel_idx;
  bool     valid;
  char     text[JC_TEXT_LEN];
};

class JCHistory {
  JCHistoryEntry _ring[JC_HISTORY_SIZE];
  int _newest;   // index of newest entry, -1 when empty
  int _count;

public:
  JCHistory() : _newest(-1), _count(0) {
    memset(_ring, 0, sizeof(_ring));
  }

  void add(uint8_t channel_idx, uint32_t ts, const char* text) {
    _newest = (_newest + 1) % JC_HISTORY_SIZE;
    JCHistoryEntry& e = _ring[_newest];
    e.timestamp = ts;
    e.channel_idx = channel_idx;
    e.valid = true;
    strncpy(e.text, text, JC_TEXT_LEN - 1);
    e.text[JC_TEXT_LEN - 1] = 0;
    if (_count < JC_HISTORY_SIZE) _count++;
  }

  int count() const { return _count; }

  // Oldest-first access: i = 0 is the oldest retained entry.
  const JCHistoryEntry& oldestFirst(int i) const {
    int idx = (_newest - (_count - 1) + i) % JC_HISTORY_SIZE;
    if (idx < 0) idx += JC_HISTORY_SIZE;
    return _ring[idx];
  }
};

extern JCHistory jc_history;

// -----------------------------------------------------------------------------
// Word-wrap helper: copies the next line of `text` starting at `start` into
// `out` (up to `cols` chars, breaking at the last space where possible) and
// returns the index the next line begins at. Returns -1 when `start` is at
// or past the end of the text.
// -----------------------------------------------------------------------------
static inline int jc_wrapLine(const char* text, int start, char* out, int cols) {
  int len = strlen(text);
  if (start >= len) return -1;
  int remain = len - start;
  if (remain <= cols) {
    memcpy(out, text + start, remain);
    out[remain] = 0;
    return len;
  }
  int cut = cols;
  for (int i = cols; i > 0; i--) {
    if (text[start + i] == ' ') { cut = i; break; }
  }
  memcpy(out, text + start, cut);
  out[cut] = 0;
  int next = start + cut;
  while (text[next] == ' ') next++;
  return next;
}

// -----------------------------------------------------------------------------
// JCChannelPicker
// -----------------------------------------------------------------------------
class JCChannelPicker : public UIScreen {
  struct Entry { uint8_t idx; char name[JC_CH_NAME_LEN]; };
  Entry _entries[JC_PICKER_MAX];
  uint8_t _count;
  int _cursor;
  bool _confirmed;
  bool _wantsExit;

  static const int VISIBLE_ROWS = 9;   // rows 1..9 (row 0 is the title)

public:
  JCChannelPicker() : _count(0), _cursor(0), _confirmed(false), _wantsExit(false) {}

  void activate() {
    _count = 0;
    _cursor = 0;
    _confirmed = false;
    _wantsExit = false;
  }
  void addChannel(uint8_t idx, const char* name) {
    if (_count >= JC_PICKER_MAX) return;
    _entries[_count].idx = idx;
    strncpy(_entries[_count].name, name, JC_CH_NAME_LEN - 1);
    _entries[_count].name[JC_CH_NAME_LEN - 1] = 0;
    _count++;
  }

  bool isConfirmed() const { return _confirmed; }
  void acknowledgeConfirm() { _confirmed = false; }
  bool wantsExit() const { return _wantsExit; }
  void acknowledgeExit() { _wantsExit = false; }
  uint8_t getSelectedChannelIdx() const { return _count ? _entries[_cursor].idx : 0; }
  const char* getSelectedChannelName() const { return _count ? _entries[_cursor].name : ""; }

  bool handleInput(char c) override {
    if (c == KEY_UP) {
      if (_count) _cursor = (_cursor + _count - 1) % _count;
      return true;
    }
    if (c == KEY_DOWN) {
      if (_count) _cursor = (_cursor + 1) % _count;
      return true;
    }
    if (c == KEY_ENTER) {
      if (_count) _confirmed = true;
      return true;
    }
    if (c == KEY_CANCEL) {
      _wantsExit = true;
      return true;
    }
    return false;
  }

  int render(DisplayDriver& display) override {
    display.setTextSize(0);
    display.setColor(UIColor::primary_txt);
    display.setCursor(0, 0);
    display.print("Channels");
    if (_count == 0) {
      display.setCursor(0, JC_ROW_H * 2);
      display.print("(none saved)");
      return 1000;
    }
    int first = 0;
    if (_cursor >= VISIBLE_ROWS) first = _cursor - VISIBLE_ROWS + 1;
    for (int r = 0; r < VISIBLE_ROWS && (first + r) < _count; r++) {
      int i = first + r;
      int y = JC_ROW_H * (r + 1);
      display.setCursor(0, y);
      display.print(i == _cursor ? ">" : " ");
      display.setCursor(8, y);
      display.print(_entries[i].name);
    }
    return 1000;
  }
};

// -----------------------------------------------------------------------------
// JCChannelScreen
// -----------------------------------------------------------------------------
class JCChannelScreen : public UIScreen {
  uint8_t _channelIdx;
  char    _channelName[JC_CH_NAME_LEN];
  int     _scrollUp;     // lines scrolled back from the newest (0 = newest visible)
  bool    _composeSel;   // Compose box highlighted
  bool    _wantsCompose;
  bool    _wantsExit;

  static const int MSG_ROWS = 8;        // rows 1..8
  static const int COMPOSE_ROW_Y = JC_ROW_H * 9;

  // Count wrapped lines for this channel's messages.
  int totalLines() const {
    char line[JC_COLS + 1];
    int total = 0;
    for (int i = 0; i < jc_history.count(); i++) {
      const JCHistoryEntry& e = jc_history.oldestFirst(i);
      if (!e.valid || e.channel_idx != _channelIdx) continue;
      int pos = 0;
      while ((pos = jc_wrapLine(e.text, pos, line, JC_COLS)) >= 0) total++;
    }
    return total;
  }

public:
  JCChannelScreen() : _channelIdx(0), _scrollUp(0), _composeSel(false),
                      _wantsCompose(false), _wantsExit(false) {
    _channelName[0] = 0;
  }

  void activate(uint8_t idx, const char* name) {
    _channelIdx = idx;
    strncpy(_channelName, name, JC_CH_NAME_LEN - 1);
    _channelName[JC_CH_NAME_LEN - 1] = 0;
    _scrollUp = 0;
    _composeSel = false;
    _wantsCompose = false;
    _wantsExit = false;
  }
  // Called after a send or a cancelled draft: back to the newest line.
  void resume() { _scrollUp = 0; _composeSel = false; }

  uint8_t getChannelIdx() const { return _channelIdx; }
  const char* getChannelName() const { return _channelName; }
  bool wantsCompose() const { return _wantsCompose; }
  void acknowledgeCompose() { _wantsCompose = false; }
  bool wantsExit() const { return _wantsExit; }
  void acknowledgeExit() { _wantsExit = false; }

  bool handleInput(char c) override {
    if (c == KEY_UP) {
      if (_composeSel) {
        _composeSel = false;
      } else {
        int total = totalLines();
        int maxUp = total > MSG_ROWS ? total - MSG_ROWS : 0;
        if (_scrollUp < maxUp) _scrollUp++;
      }
      return true;
    }
    if (c == KEY_DOWN) {
      if (_scrollUp > 0) _scrollUp--;
      else _composeSel = true;
      return true;
    }
    if (c == KEY_ENTER) {
      if (_composeSel) _wantsCompose = true;
      return true;
    }
    if (c == KEY_CANCEL) {
      _wantsExit = true;
      return true;
    }
    return false;
  }

  int render(DisplayDriver& display) override {
    const int W = display.width();
    char line[JC_COLS + 1];

    display.setTextSize(0);
    display.setColor(UIColor::primary_txt);

    // title row
    char title[JC_CH_NAME_LEN + 2];
    snprintf(title, sizeof(title), "#%s", _channelName);
    display.setCursor(0, 0);
    display.print(title);

    // message lines: window of MSG_ROWS lines ending (total - _scrollUp)
    int total = totalLines();
    int maxUp = total > MSG_ROWS ? total - MSG_ROWS : 0;
    if (_scrollUp > maxUp) _scrollUp = maxUp;
    int lastLine = total - _scrollUp;             // exclusive
    int firstLine = lastLine - MSG_ROWS;
    if (firstLine < 0) firstLine = 0;

    int lineNo = 0;
    int row = 0;
    for (int i = 0; i < jc_history.count() && lineNo < lastLine; i++) {
      const JCHistoryEntry& e = jc_history.oldestFirst(i);
      if (!e.valid || e.channel_idx != _channelIdx) continue;
      int pos = 0;
      while ((pos = jc_wrapLine(e.text, pos, line, JC_COLS)) >= 0) {
        if (lineNo >= firstLine && lineNo < lastLine) {
          display.setCursor(0, JC_ROW_H * (row + 1));
          display.print(line);
          row++;
        }
        lineNo++;
        if (lineNo >= lastLine) break;
      }
    }
    if (total == 0) {
      display.setCursor(0, JC_ROW_H * 2);
      display.print("No messages yet");
    }

    // compose box (fixed bottom row)
    const char* label = "[ Compose ]";
    int lw = display.getTextWidth(label);
    int lx = (W - lw) / 2;
    display.setCursor(lx, COMPOSE_ROW_Y);
    display.print(label);
    if (_composeSel) {
      display.drawRect(lx - 2, JC_RECT_Y(COMPOSE_ROW_Y), lw + 4, JC_RECT_H);
    }
    return 1000;
  }
};

// -----------------------------------------------------------------------------
// JCKeyboardScreen
// -----------------------------------------------------------------------------
class JCKeyboardScreen : public UIScreen {
  uint8_t _channelIdx;
  char    _channelName[JC_CH_NAME_LEN];
  char    _outBuf[JC_OUT_BUF_LEN];
  uint16_t _outLen;

  enum Mode { LOWER, UPPER, SYM };
  Mode _mode;

  int  _row;   // 0..3 (3 = bottom action row)
  int  _col;   // 0..9 cell index
  bool _wantsSend;
  bool _wantsExit;

  static const int MAXLEN   = JC_OUT_BUF_LEN - 1;   // 133
  static const int CELLS    = 10;
  static const int TEXT_Y0  = 0;                    // draft rows 0,1
  static const int LABEL_Y  = JC_ROW_H * 2;         // channel + count
  static const int GRID_Y   = 26;                   // row 0 of keys
  static const int KEY_PITCH = 12;                  // rows at 26, 38, 50, 62
  static const int CELL_W   = 12;                   // box width; cells are 12.8 apart

  struct ActionKey { const char* label; int cell; int span; };
  static const int N_ACTIONS = 5;
  // bottom row: mode | SPACE | DEL | SND | ESC
  static const ActionKey* actions() {
    static const ActionKey a[N_ACTIONS] = {
      { "mode",  0, 2 },
      { "SPACE", 2, 4 },
      { "DEL",   6, 2 },
      { "SND",   8, 1 },
      { "ESC",   9, 1 },
    };
    return a;
  }
  int actionAtCell(int cell) const {
    const ActionKey* a = actions();
    for (int i = 0; i < N_ACTIONS; i++) {
      if (cell >= a[i].cell && cell < a[i].cell + a[i].span) return i;
    }
    return N_ACTIONS - 1;
  }

  const char* const* layout() const {
    static const char* const lower[3] = { "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?" };
    static const char* const upper[3] = { "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM<>/" };
    static const char* const syms[3]  = { "123!@#$%^&", "456()[]{}-", "7890+=_:;|" };
    return _mode == SYM ? syms : (_mode == UPPER ? upper : lower);
  }
  const char* modeLabel() const {
    return _mode == SYM ? "sym" : (_mode == UPPER ? "ABC" : "abc");
  }

  static int cellX(int cell) { return (cell * 128) / CELLS; }

  void appendChar(char ch) {
    if (_outLen < MAXLEN) { _outBuf[_outLen++] = ch; _outBuf[_outLen] = 0; }
  }
  void backspace() {
    if (_outLen > 0) { _outLen--; _outBuf[_outLen] = 0; }
  }
  void cycleMode() {
    _mode = (_mode == LOWER) ? UPPER : (_mode == UPPER ? SYM : LOWER);
  }
  void act() {
    if (_row < 3) {
      char ch = layout()[_row][_col];
      if (ch) appendChar(ch);
      return;
    }
    switch (actionAtCell(_col)) {
      case 0: cycleMode(); break;
      case 1: appendChar(' '); break;
      case 2: backspace(); break;
      case 3: if (_outLen > 0) _wantsSend = true; break;
      default: _wantsExit = true; break;
    }
  }

public:
  JCKeyboardScreen() : _channelIdx(0), _outLen(0), _mode(LOWER), _row(0), _col(0),
                       _wantsSend(false), _wantsExit(false) {
    _outBuf[0] = 0;
    _channelName[0] = 0;
  }

  void activate(uint8_t idx, const char* name) {
    _channelIdx = idx;
    strncpy(_channelName, name, JC_CH_NAME_LEN - 1);
    _channelName[JC_CH_NAME_LEN - 1] = 0;
    _outLen = 0; _outBuf[0] = 0;
    _mode = LOWER;
    _row = 0; _col = 0;
    _wantsSend = false; _wantsExit = false;
  }

  uint8_t getChannelIdx() const { return _channelIdx; }
  bool consumeSendRequest(const char** textOut) {
    if (!_wantsSend) return false;
    _wantsSend = false;
    if (textOut) *textOut = _outBuf;
    return true;
  }
  void clearOutBuf() { _outLen = 0; _outBuf[0] = 0; }
  bool wantsExit() const { return _wantsExit; }
  void acknowledgeExit() { _wantsExit = false; }

  bool handleInput(char c) override {
    if (c == KEY_UP) {
      _row = (_row + 3) % 4;
      return true;
    }
    if (c == KEY_DOWN) {
      _row = (_row + 1) % 4;
      return true;
    }
    if (c == KEY_LEFT) {
      if (_row < 3) {
        _col = (_col + CELLS - 1) % CELLS;
      } else {
        int i = actionAtCell(_col);
        i = (i + N_ACTIONS - 1) % N_ACTIONS;
        _col = actions()[i].cell;
      }
      return true;
    }
    if (c == KEY_RIGHT) {
      if (_row < 3) {
        _col = (_col + 1) % CELLS;
      } else {
        int i = actionAtCell(_col);
        i = (i + 1) % N_ACTIONS;
        _col = actions()[i].cell;
      }
      return true;
    }
    if (c == KEY_ENTER) {
      act();
      return true;
    }
    if (c == KEY_CANCEL) {
      _wantsExit = true;
      return true;
    }
    return false;
  }

  int render(DisplayDriver& display) override {
    char tmp[JC_COLS + 1];
    display.setTextSize(0);
    display.setColor(UIColor::primary_txt);

    // draft: last two wrapped rows so the end of the text (with cursor) is visible
    {
      char draft[JC_OUT_BUF_LEN + 1];
      snprintf(draft, sizeof(draft), "%s_", _outBuf);
      char rows[2][JC_COLS + 1];
      int n = 0;
      int pos = 0;
      while ((pos = jc_wrapLine(draft, pos, tmp, JC_COLS)) >= 0) {
        if (n < 2) {
          strcpy(rows[n], tmp);
          n++;
        } else {
          strcpy(rows[0], rows[1]);
          strcpy(rows[1], tmp);
        }
      }
      for (int i = 0; i < n; i++) {
        display.setCursor(0, TEXT_Y0 + JC_ROW_H * i);
        display.print(rows[i]);
      }
    }

    // label row: channel and count
    snprintf(tmp, sizeof(tmp), "#%s", _channelName);
    display.setCursor(0, LABEL_Y);
    display.print(tmp);
    snprintf(tmp, sizeof(tmp), "%d/%d", (int)_outLen, MAXLEN);
    display.setCursor(128 - display.getTextWidth(tmp), LABEL_Y);
    display.print(tmp);

    // letter rows
    const char* const* rows = layout();
    for (int r = 0; r < 3; r++) {
      int y = GRID_Y + r * KEY_PITCH;
      for (int c = 0; c < CELLS; c++) {
        char s[2] = { rows[r][c], 0 };
        int x = cellX(c);
        display.setCursor(x + 4, y);
        display.print(s);
        if (_row == r && _col == c) {
          display.drawRect(x, JC_RECT_Y(y), CELL_W, JC_RECT_H);
        }
      }
    }

    // action row
    {
      int y = GRID_Y + 3 * KEY_PITCH;
      const ActionKey* a = actions();
      int sel = actionAtCell(_col);
      for (int i = 0; i < N_ACTIONS; i++) {
        const char* label = (i == 0) ? modeLabel() : a[i].label;
        int x = cellX(a[i].cell);
        int w = cellX(a[i].cell + a[i].span) - x - 1;
        int lw = display.getTextWidth(label);
        int lx = x + (w - lw) / 2;
        if (lx < x + 1) lx = x + 1;
        display.setCursor(lx, y);
        display.print(label);
        if (_row == 3 && i == sel) {
          display.drawRect(x, JC_RECT_Y(y), w, JC_RECT_H);
        }
      }
    }
    return 1000;
  }
};

#endif // UI_JOYSTICK_COMPOSE
