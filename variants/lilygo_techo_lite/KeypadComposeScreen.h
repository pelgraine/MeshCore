#pragma once
// =============================================================================
// KeypadComposeScreen — channel-select + T9 multi-tap compose/receive for the
// T-Echo Lite KeyShield (5x4 phone keypad via TCA8418).
//
// Two internal modes, so the whole feature lives in one screen (no separate
// picker class, minimal UITask wiring):
//
//   MODE_SELECT  — channel picker. UITask calls beginChannelSelect() then
//                  addChannel() per channel and shows the screen. Up/Down move
//                  the highlight, Center selects (-> MODE_COMPOSE), Esc exits.
//   MODE_COMPOSE — T9 text entry on the chosen channel.
//
// Compose input (chars from techo_keypad_read() via UITask -> handleInput):
//   '0'-'9'    multi-tap letter/number keys (T9 cycling)
//   '*'        symbol cycler (walks a punctuation set; literal # and * live here)
//   '#'        case toggle (lower <-> UPPER)
//   ' '        space      (square key)
//   '\b'       backspace  (X key)
//   KEY_ENTER  send the message on the selected channel  (Center)
//   KEY_CANCEL exit back to home                         (Esc)
//
// A letter commits on a ~1s tap gap (KP_T9_TIMEOUT_MS) or when a different key
// is pressed.
//
// e-ink note: renderCompose() draws only *committed* text. The pending multi-tap
// candidate is deliberately NOT drawn, so rapid tapping does not change the
// frame and therefore does not trigger a blocking ~644ms e-ink refresh that
// would eat presses. Composing is semi-blind: the letter appears once committed.
//
// Sending/exit are delegated to UITask via the consume/flag pattern, so this
// header has no dependency on MyMesh / BaseChatMesh.
// =============================================================================

#ifdef TECHO_KEYPAD

#include <Arduino.h>
#include <string.h>
#include <helpers/ui/UIScreen.h>
#include <helpers/ui/DisplayDriver.h>
#include <MeshCore.h>

// ---- Tunables ---------------------------------------------------------------
#define KP_T9_TIMEOUT_MS   1000   // tap gap that commits the current letter
#define KP_OUT_BUF_LEN     134    // MeshCore per-channel msg cap (~133) + NUL
#define KP_INBOX_SIZE      3
#define KP_INBOX_TEXT_LEN  96
#define KP_INBOX_NAME_LEN  32
#define KP_PICKER_MAX      8      // max channels shown in the picker

// T9 multi-tap sets, indexed by (key - '0'): letters then the digit.
static const char* const KP_T9_SETS[10] = {
  "0", "1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9"
};

// Symbol set walked by the '*' key. Literal '#' and '*' live in here.
static const char KP_T9_SYMBOLS[] = ".,?!@-/:;'\"()#*";

class KeypadComposeScreen : public UIScreen {
  mesh::RTCClock* _rtc;

  enum Mode { MODE_SELECT, MODE_COMPOSE };
  Mode _mode;

  // Channel select
  struct ChannelEntry { uint8_t idx; char name[32]; };
  ChannelEntry _channels[KP_PICKER_MAX];
  uint8_t      _numChannels;
  uint8_t      _highlighted;

  // Outgoing composition (committed chars only)
  char      _outBuf[KP_OUT_BUF_LEN];
  uint16_t  _outLen;
  uint8_t   _channelIdx;
  char      _channelName[32];

  // T9 multi-tap staging (uncommitted candidate)
  bool          _hasPending;
  char          _pendingKey;     // key char being cycled ('0'-'9' or '*')
  const char*   _pendingSet;     // cycle set for that key
  uint8_t       _tapIndex;       // index within _pendingSet
  unsigned long _lastTapAt;
  bool          _upperCase;      // case toggle state

  // Cross-screen requests (UITask polls these)
  bool          _wantsExit;
  bool          _wantsSend;

  // Incoming ring buffer
  struct InboxEntry {
    uint32_t timestamp;
    char     from[KP_INBOX_NAME_LEN];
    char     text[KP_INBOX_TEXT_LEN];
    bool     valid;
  };
  InboxEntry _inbox[KP_INBOX_SIZE];
  uint8_t    _inboxNewest;
  uint8_t    _inboxCount;

  bool          _dirty;

  // --- compose helpers -------------------------------------------------------
  void appendChar(char ch) {
    if (_outLen < KP_OUT_BUF_LEN - 1) {
      _outBuf[_outLen++] = ch;
      _outBuf[_outLen] = 0;
      _dirty = true;
    }
  }

  char currentCandidate() const {
    if (!_hasPending || !_pendingSet) return 0;
    char ch = _pendingSet[_tapIndex];
    if (_upperCase && ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
    return ch;
  }

  void commitPending() {
    if (!_hasPending) return;
    char ch = currentCandidate();
    _hasPending = false;
    _pendingKey = 0;
    _pendingSet = nullptr;
    if (ch) appendChar(ch);
  }

  void cycleKey(char key, const char* set) {
    unsigned long now = millis();
    if (_hasPending && _pendingKey == key
        && (now - _lastTapAt) < KP_T9_TIMEOUT_MS) {
      _tapIndex = (uint8_t)((_tapIndex + 1) % strlen(set));   // advance candidate
    } else {
      commitPending();                                        // commit previous letter
      _pendingKey = key;
      _pendingSet = set;
      _tapIndex   = 0;
      _hasPending = true;
    }
    _lastTapAt = now;
  }

  void backspace() {
    if (_hasPending) {            // discard the uncommitted candidate
      _hasPending = false;
      _pendingKey = 0;
      _pendingSet = nullptr;
      return;
    }
    if (_outLen > 0) {           // delete last committed char
      _outLen--;
      _outBuf[_outLen] = 0;
      _dirty = true;
    }
  }

  // --- mode-specific input ---------------------------------------------------
  bool handleSelectInput(char c) {
    switch (c) {
      case KEY_PREV:   // Up
        if (_numChannels > 0)
          _highlighted = (_highlighted == 0) ? (uint8_t)(_numChannels - 1)
                                             : (uint8_t)(_highlighted - 1);
        _dirty = true; return true;
      case KEY_NEXT:   // Down
        if (_numChannels > 0)
          _highlighted = (uint8_t)((_highlighted + 1) % _numChannels);
        _dirty = true; return true;
      case KEY_ENTER:  // select -> compose
        if (_highlighted < _numChannels)
          activate(_channels[_highlighted].idx, _channels[_highlighted].name);
        return true;
      case KEY_CANCEL: // back out to home
        _wantsExit = true; return true;
    }
    return false;
  }

  bool handleComposeInput(char c) {
    switch (c) {
      case KEY_ENTER:                 // Center -> send
        commitPending();
        if (_outLen > 0) _wantsSend = true;
        return true;
      case KEY_CANCEL:                // Esc -> exit
        commitPending();
        _wantsExit = true;
        return true;
      case '\b':                      // X -> backspace
        backspace();
        return true;
      case ' ':                       // square -> space
        commitPending();
        appendChar(' ');
        return true;
      case '#':                       // case toggle
        commitPending();
        _upperCase = !_upperCase;
        _dirty = true;
        return true;
      case '*':                       // symbol cycler
        cycleKey('*', KP_T9_SYMBOLS);
        return true;
      default: break;
    }
    if (c >= '0' && c <= '9') {       // T9 multi-tap
      cycleKey(c, KP_T9_SETS[c - '0']);
      return true;
    }
    return false;                     // KEY_PREV / KEY_NEXT etc.: ignore for now
  }

  // --- mode-specific render --------------------------------------------------
  int renderSelect(DisplayDriver& display) {
    const int W = display.width();
    display.setTextSize(1);

    display.setColor(DisplayDriver::YELLOW);
    display.setCursor(0, 0);
    display.print("SELECT CHANNEL");
    display.setColor(DisplayDriver::LIGHT);
    display.drawRect(0, 11, W, 1);

    if (_numChannels == 0) {
      display.setCursor(0, 16);
      display.print("(no channels)");
    } else {
      int y = 16;
      for (uint8_t i = 0; i < _numChannels; i++) {
        if (i == _highlighted) {
          display.setColor(DisplayDriver::DARK);
          display.fillRect(0, y - 1, W, 12);
          display.setColor(DisplayDriver::LIGHT);
        } else {
          display.setColor(DisplayDriver::LIGHT);
        }
        char line[40];
        snprintf(line, sizeof(line), "  %s", _channels[i].name);
        if (i == _highlighted) line[0] = '>';
        display.setCursor(0, y);
        display.print(line);
        y += 14;
      }
    }
    return 5000;
  }

  int renderCompose(DisplayDriver& display) {
    const int W = display.width();
    display.setTextSize(1);

    // ---- Header: channel + case mode ----
    display.setColor(DisplayDriver::YELLOW);
    display.setCursor(0, 0);
    char hdr[40];
    snprintf(hdr, sizeof(hdr), "MSG > %s", _channelName);
    display.print(hdr);
    display.setColor(DisplayDriver::GREEN);
    display.drawTextRightAlign(W - 1, 0, _upperCase ? "ABC" : "abc");

    display.setColor(DisplayDriver::LIGHT);
    display.drawRect(0, 11, W, 1);

    // ---- Inbox (last 2) ----
    display.setColor(DisplayDriver::GREEN);
    display.setCursor(0, 13);
    display.print("IN");
    display.setColor(DisplayDriver::LIGHT);
    if (_inboxCount == 0) {
      display.setCursor(18, 13);
      display.print("(no messages)");
    } else {
      int y = 13;
      for (int i = 0; i < _inboxCount && i < 2; i++) {
        int idx = (int)_inboxNewest - i;
        while (idx < 0) idx += KP_INBOX_SIZE;
        const InboxEntry& e = _inbox[idx];
        if (!e.valid) continue;
        display.drawTextEllipsized(18, y, W - 20, e.text);
        y += 10;
      }
    }
    display.drawRect(0, 33, W, 1);

    // ---- Outgoing buffer (committed text + cursor; pending NOT drawn) ----
    display.setColor(DisplayDriver::GREEN);
    display.setCursor(0, 35);
    display.print("OUT");
    display.setColor(DisplayDriver::LIGHT);
    char outWithCursor[KP_OUT_BUF_LEN + 2];
    if (_outLen == 0) {
      strcpy(outWithCursor, "_");
    } else {
      strncpy(outWithCursor, _outBuf, sizeof(outWithCursor) - 2);
      outWithCursor[sizeof(outWithCursor) - 2] = 0;
      size_t n = strlen(outWithCursor);
      if (n < sizeof(outWithCursor) - 1) { outWithCursor[n] = '_'; outWithCursor[n + 1] = 0; }
    }
    display.setCursor(0, 46);
    display.printWordWrap(outWithCursor, W);

    display.drawRect(0, 66, W, 1);

    // ---- Status: char count ----
    display.setColor(DisplayDriver::LIGHT);
    char cc[16];
    snprintf(cc, sizeof(cc), "%u/%u", (unsigned)_outLen, (unsigned)(KP_OUT_BUF_LEN - 1));
    display.setCursor(0, 68);
    display.print(cc);

    _dirty = false;
    return 800;   // throttle; the endFrame CRC check makes unchanged frames instant
  }

public:
  KeypadComposeScreen(mesh::RTCClock* rtc)
    : _rtc(rtc), _mode(MODE_SELECT), _numChannels(0), _highlighted(0),
      _outLen(0), _channelIdx(0),
      _hasPending(false), _pendingKey(0), _pendingSet(nullptr),
      _tapIndex(0), _lastTapAt(0), _upperCase(false),
      _wantsExit(false), _wantsSend(false),
      _inboxNewest(0), _inboxCount(0), _dirty(true)
  {
    _outBuf[0] = 0;
    strcpy(_channelName, "Public");
    memset(_inbox, 0, sizeof(_inbox));
    memset(_channels, 0, sizeof(_channels));
  }

  // --- channel-select entry (called by UITask before showing the screen) ----
  void beginChannelSelect() {
    _mode = MODE_SELECT;
    _numChannels = 0;
    _highlighted = 0;
    _wantsExit = false;
    _wantsSend = false;
    _dirty = true;
  }

  void addChannel(uint8_t idx, const char* name) {
    if (_numChannels >= KP_PICKER_MAX) return;
    _channels[_numChannels].idx = idx;
    strncpy(_channels[_numChannels].name, name ? name : "", 31);
    _channels[_numChannels].name[31] = 0;
    _numChannels++;
  }

  // Switch to compose on the given channel (called when a channel is selected).
  void activate(uint8_t channelIdx, const char* channelName) {
    _channelIdx = channelIdx;
    strncpy(_channelName, channelName ? channelName : "", sizeof(_channelName) - 1);
    _channelName[sizeof(_channelName) - 1] = 0;
    _outLen = 0; _outBuf[0] = 0;
    _hasPending = false; _pendingKey = 0; _pendingSet = nullptr;
    _upperCase = false;
    _wantsExit = false; _wantsSend = false;
    _mode = MODE_COMPOSE;
    _dirty = true;
  }

  uint8_t getChannelIdx() const { return _channelIdx; }
  const char* getChannelName() const { return _channelName; }

  // Called from UITask::newMsg for incoming messages, filtered to this channel.
  // NOTE: `from` is matched against the channel name, carried over from the
  // Morse port. Verify how newMsg supplies this for channel messages when the
  // inbox feed is wired up.
  void notifyPublicMsg(const char* from, const char* text) {
    if (!from || strcmp(from, _channelName) != 0) return;  // wrong channel
    _inboxNewest = (_inboxCount == 0) ? 0 : ((_inboxNewest + 1) % KP_INBOX_SIZE);
    InboxEntry& e = _inbox[_inboxNewest];
    e.timestamp = _rtc ? _rtc->getCurrentTime() : 0;
    strncpy(e.from, from, KP_INBOX_NAME_LEN - 1); e.from[KP_INBOX_NAME_LEN - 1] = 0;
    if (text) { strncpy(e.text, text, KP_INBOX_TEXT_LEN - 1); e.text[KP_INBOX_TEXT_LEN - 1] = 0; }
    else      { e.text[0] = 0; }
    e.valid = true;
    if (_inboxCount < KP_INBOX_SIZE) _inboxCount++;
    _dirty = true;
  }

  // UITask bridges — polled each loop iteration
  bool consumeSendRequest(const char** textOut) {
    if (!_wantsSend) return false;
    _wantsSend = false;
    if (textOut) *textOut = _outBuf;
    return true;
  }
  bool wantsExit() const { return _wantsExit; }
  void acknowledgeExit() { _wantsExit = false; }
  void clearOutBuf() { _outLen = 0; _outBuf[0] = 0; _dirty = true; }

  // UIScreen contract ---------------------------------------------------------
  bool handleInput(char c) override {
    return (_mode == MODE_SELECT) ? handleSelectInput(c) : handleComposeInput(c);
  }

  void poll() override {
    if (_mode == MODE_COMPOSE && _hasPending
        && (millis() - _lastTapAt) >= KP_T9_TIMEOUT_MS) {
      commitPending();
    }
  }

  int render(DisplayDriver& display) override {
    return (_mode == MODE_SELECT) ? renderSelect(display) : renderCompose(display);
  }
};

#endif // TECHO_KEYPAD