// settings.h — watchface settings. They arrive from the phone (Clay) as their
// own keys and live in persist. NOT part of DataPacket.

#pragma once
#include <pebble.h>

typedef struct {
  bool use_24h;              // HourFormat (1/0)
  // WeatherUnits: false = m/s and km, true = mph and miles. The packet always
  // arrives in SI — the watch converts at draw time, just as it does for the
  // time format, so flipping the switch shows immediately without a new packet.
  bool use_imperial;
  // NotifyLeadTime: vibrate this many minutes before a light window starts
  // (0 = off). The watch computes the moment from packet times — see main.c.
  uint16_t notify_lead_min;
  uint16_t astro_timeout_sec;   // AstroTimeout, auto-return Astro→Clock (0 = off)
  uint16_t sw_idle_timeout_sec; // StopwatchIdleTimeout, idle exit to Clock (0 = off)
  uint16_t sw_max_duration_min; // StopwatchMaxDuration, auto-stop in minutes (0 = no limit)
  // ShowStopwatch: whether the stopwatch screen stays in the tap cycle. Turning
  // it off drops the third screen entirely (Clock↔Astro) rather than hiding its
  // readout: not everyone needs a stopwatch, and an extra screen makes the
  // cycle longer for everyone else.
  bool show_stopwatch;
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
