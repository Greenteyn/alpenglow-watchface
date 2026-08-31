// settings.h — watchface settings. They arrive from the phone (Clay) as their
// own keys and live in persist. NOT part of DataPacket.

#pragma once
#include <pebble.h>

typedef struct {
  bool use_24h;              // HourFormat (1/0)
  bool use_imperial;         // WeatherUnits: false = m/s and km (see packet.h)
  // NotifyLeadTime: vibrate this many minutes before a light window starts
  // (0 = off). The watch computes the moment from packet times — see main.c.
  uint16_t notify_lead_min;
  // TapControl: whether the watchface listens for taps at all. Off, it drops the
  // accelerometer subscription (which otherwise runs continuously) and the Clock
  // becomes the only screen, the tap being the only way to reach the others.
  // Kept apart from ShowStopwatch on purpose: this switch decides whether there
  // is a cycle, that one decides how long it is.
  bool tap_control;
  uint16_t astro_timeout_sec;   // AstroTimeout, auto-return Astro→Clock (0 = off)
  uint16_t sw_idle_timeout_sec; // StopwatchIdleTimeout, idle exit to Clock (0 = off)
  uint16_t sw_max_duration_min; // StopwatchMaxDuration, auto-stop in minutes (0 = no limit)
  // ShowStopwatch: whether the stopwatch screen stays in the tap cycle. Off, the
  // third screen goes entirely (Clock↔Astro) rather than losing its readout.
  bool show_stopwatch;
  // RingOrientation: where midnight sits on the day ring — 0 = top (the
  // default), 1 = bottom, which lifts daylight into the upper half. Held as an
  // integer rather than a flag because that is what the Clay select sends and
  // what RingOrientation in ring.h enumerates.
  uint16_t ring_orientation;
} Settings;

// NOTE: ANY change to this struct layout must bump SETTINGS_PERSIST_KEY in
// settings.c — otherwise the old blob is read at shifted offsets.

// Default values.
void settings_init_defaults(Settings *s);

// Load from persist on top of the defaults.
void settings_load(Settings *s);

// Save to persist.
void settings_save(const Settings *s);

// Update from an incoming AppMessage dictionary (present keys only).
// Returns true if at least one setting actually changed.
bool settings_update_from_dict(Settings *s, DictionaryIterator *iter);
