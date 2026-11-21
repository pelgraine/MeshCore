#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <helpers/ESP32Board.h>

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
class RangerNanoBoard : public ESP32Board {
  TimerHandle_t _fan_timer = NULL;
  volatile bool _tx_active = false;
  portMUX_TYPE _fan_mux = portMUX_INITIALIZER_UNLOCKED;

  static void fanTimerCallback(TimerHandle_t t) {
    RangerNanoBoard* self = (RangerNanoBoard*)pvTimerGetTimerID(t);
    portENTER_CRITICAL(&self->_fan_mux);
    if (!self->_tx_active) {
      digitalWrite(PIN_FAN_EN, LOW);   // no transmission for FAN_HOLD_SECS: fan off
    }
    portEXIT_CRITICAL(&self->_fan_mux);
  }

public:
  // Call once from radio_init(), before the radio can transmit.
  void fanBegin() {
    pinMode(PIN_FAN_EN, OUTPUT);
    digitalWrite(PIN_FAN_EN, LOW);
    _fan_timer = xTimerCreate("fan", pdMS_TO_TICKS(FAN_HOLD_SECS * 1000UL), pdFALSE, this, fanTimerCallback);
  }

  void onBeforeTransmit() override {
    portENTER_CRITICAL(&_fan_mux);
    _tx_active = true;
    digitalWrite(PIN_FAN_EN, HIGH);
    portEXIT_CRITICAL(&_fan_mux);
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
  }
};
