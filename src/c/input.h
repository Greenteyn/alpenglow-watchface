// input.h — the single source of gestures for the watchface.
//
// Hides from main.c where a gesture comes from. There is exactly one source:
// an accelerometer-based tap detector. The firmware does not deliver touch
// events to watchfaces at all — see input.c for the measurement.

#pragma once
#include <pebble.h>

typedef enum {
  GESTURE_TAP = 0,
} Gesture;

typedef void (*InputGestureHandler)(Gesture gesture);

// Both calls are idempotent and may be made at any time, not only at start-up:
// the setting that governs them (TapControl) arrives from the phone after
// init(), so the subscription has to be raised and dropped at runtime. They are
// reached far more often than the state actually changes — re-subscribing on
// each packet would wipe the detector state, possibly mid-strike.
//
// An accelerometer subscription costs 64 bytes of app heap; what it costs in
// ENERGY is the reason the TapControl switch exists at all — holding the sensor
// roughly doubles the discharge.
void input_subscribe(InputGestureHandler handler);

void input_unsubscribe(void);

// Peak jerk seen since the last subscribe — the instrument for calibrating the
// thresholds on new hardware: compare the peak during normal wear against the
// peak of a deliberate strike and put the threshold in between.
//
// DELIBERATELY UNCALLED, and not dead code: it is wired up by hand for a
// calibration session, by drawing the value on screen for a while. Logs do not
// reach a watch connected over the CloudPebble proxy, so the screen is the only
// channel a bench measurement has.
int32_t input_max_jerk(void);

// --- Bench counters, same contract as input_max_jerk above ---
//
// Uncalled by design, and MUTE: nothing here prints, draws or logs. That is the
// whole point — a build made for measuring wires them up to a readout of its
// own, while an ordinary build carries the arithmetic and shows nothing.
//
// Callbacks answer whether the accelerometer subscription is really gone when
// tap control is switched off: a screen that stops responding proves only that
// the gesture no longer reaches the handler, not that the sensor was released.
// A callback count that stays put for a whole day proves it.
//
// Both counters deliberately survive unsubscribe and subscribe — the question
// they answer spans a whole wearing session, not one subscription.
uint32_t input_dbg_callbacks(void);
uint32_t input_dbg_taps(void);
bool input_dbg_subscribed(void);
