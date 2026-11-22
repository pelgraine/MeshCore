#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <helpers/ESP32Board.h>

#ifdef RANGER_NANO_TXDEBUG
  #include <esp_timer.h>
  #include <soc/soc.h>
  #include <soc/gpio_reg.h>
  #define TXDEBUG_SAMPLE_US 20000   // sample the PA/LNA pins 20 ms into each send (packet still on air)
  // Defined in target.cpp (it has the radio object): prints one line per send.
  void ranger_txdebug_report(uint32_t n, int mid_pins, uint8_t after_pins, uint32_t ms);
#endif

#ifndef FAN_HOLD_SECS
  #define FAN_HOLD_SECS 30   // fan keeps running this long after the last transmission ends
#endif

// RadioMaster Ranger Nano board: ESP32Board plus transmit-driven fan control.
//
// MeshCore's RadioLibWrapper calls onBeforeTransmit() right before every send and
// onAfterTransmit() when the send finishes (or fails to start). The fan comes on at
// the start of each transmission and a one-shot timer turns it off FAN_HOLD_SECS after
// the last one ends. A new transmission before then keeps it running and restarts the
// countdown. The fan is off at boot until the first transmission.
//
// With RANGER_NANO_TXDEBUG, each send also samples the PA/LNA switch pins
// (SX128X_RXEN = 32, SX128X_TXEN = 33) part-way through the transmission and prints
// them with the chip power, via ranger_txdebug_report() in target.cpp.
class RangerNanoBoard : public ESP32Board {
  TimerHandle_t _fan_timer = NULL;
  volatile bool _tx_active = false;
  portMUX_TYPE _fan_mux = portMUX_INITIALIZER_UNLOCKED;

#ifdef RANGER_NANO_TXDEBUG
  esp_timer_handle_t _dbg_timer = NULL;
  volatile int _dbg_mid = -1;        // pins sampled mid-send, -1 = not sampled
  uint32_t _dbg_start = 0;
  uint32_t _dbg_n = 0;

  static void dbgTimerCallback(void* arg) {
    ((RangerNanoBoard*)arg)->_dbg_mid = readPaPins();
  }
#endif

  static void fanTimerCallback(TimerHandle_t t) {
    RangerNanoBoard* self = (RangerNanoBoard*)pvTimerGetTimerID(t);
    portENTER_CRITICAL(&self->_fan_mux);
    if (!self->_tx_active) {
      digitalWrite(PIN_FAN_EN, LOW);   // no transmission for FAN_HOLD_SECS: fan off
    }
    portEXIT_CRITICAL(&self->_fan_mux);
  }

public:
#ifdef RANGER_NANO_TXDEBUG
  // Levels the ESP32 is driving on the PA/LNA pins, read from its output register.
  // bit 0 = RXEN (GPIO 32, LNA), bit 1 = TXEN (GPIO 33, PA). Both pins are in bank 1 (32-39).
  static uint8_t readPaPins() {
    uint32_t out = REG_READ(GPIO_OUT1_REG);
    return ((out >> (SX128X_RXEN - 32)) & 1) | (((out >> (SX128X_TXEN - 32)) & 1) << 1);
  }
  // Whether those two pins are enabled as outputs (same bit layout).
  static uint8_t readPaPinsEnabled() {
    uint32_t en = REG_READ(GPIO_ENABLE1_REG);
    return ((en >> (SX128X_RXEN - 32)) & 1) | (((en >> (SX128X_TXEN - 32)) & 1) << 1);
  }
#endif

  // Call once from radio_init(), before the radio can transmit.
  void fanBegin() {
    pinMode(PIN_FAN_EN, OUTPUT);
    digitalWrite(PIN_FAN_EN, LOW);
    _fan_timer = xTimerCreate("fan", pdMS_TO_TICKS(FAN_HOLD_SECS * 1000UL), pdFALSE, this, fanTimerCallback);
#ifdef RANGER_NANO_TXDEBUG
    esp_timer_create_args_t args = {};
    args.callback = dbgTimerCallback;
    args.arg = this;
    args.name = "txdbg";
    esp_timer_create(&args, &_dbg_timer);
#endif
  }

  void onBeforeTransmit() override {
    portENTER_CRITICAL(&_fan_mux);
    _tx_active = true;
    digitalWrite(PIN_FAN_EN, HIGH);
    portEXIT_CRITICAL(&_fan_mux);
#ifdef RANGER_NANO_TXDEBUG
    _dbg_mid = -1;
    _dbg_start = millis();
    if (_dbg_timer) esp_timer_start_once(_dbg_timer, TXDEBUG_SAMPLE_US);
#endif
  }

  void onAfterTransmit() override {
    portENTER_CRITICAL(&_fan_mux);
    _tx_active = false;
    portEXIT_CRITICAL(&_fan_mux);
    if (_fan_timer) {
      xTimerReset(_fan_timer, 0);      // (re)start the FAN_HOLD_SECS countdown
    } else {
      digitalWrite(PIN_FAN_EN, LOW);   // timer could not be created: fall back to TX-only
    }
#ifdef RANGER_NANO_TXDEBUG
    if (_dbg_timer) esp_timer_stop(_dbg_timer);   // harmless if it already fired
    ranger_txdebug_report(++_dbg_n, _dbg_mid, readPaPins(), millis() - _dbg_start);
#endif
  }
};
