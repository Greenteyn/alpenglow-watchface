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

// Subscribe to input. Where this is called inside init() does not matter: an
// accelerometer subscription costs 64 bytes of app heap and only fails when
// those are unavailable (measured on hardware).
void input_subscribe(InputGestureHandler handler);

void input_unsubscribe(void);

// Peak jerk seen this session — the tool for calibrating thresholds on new
// hardware: compare the peak during normal wear with the peak of a deliberate
// tap and put the threshold in between. Read it by drawing the value on screen
// for a while; logs do not reach a watch connected over the CloudPebble proxy.
int32_t input_max_jerk(void);
