// packet.h — one snapshot of data as sent by the phone.
// Primitives only; stored flat in the app heap and written to persist as a
// single block.

#pragma once
#include <pebble.h>

typedef struct {
  uint32_t sun_rise, sun_set;
  // Next UPCOMING light window, shown on the Astro screen. May already belong
  // to tomorrow — the phone picks the first window that has not ended yet.
  uint32_t sun_golden_start, sun_golden_end;
  uint32_t sun_blue_start, sun_blue_end;
  // TODAY's boundaries, used by the ring: the whole day, both window pairs.
  // In chronological order: astro_dawn → dawn → rise → golden_am_end →
  // golden_pm_start → set → dusk → astro_dusk.
  uint32_t sun_astro_dawn;      // end of astronomical night
  uint32_t sun_dawn;            // start of the morning blue hour
  uint32_t sun_golden_am_end;   // end of the morning golden hour
  uint32_t sun_golden_pm_start; // start of the evening golden hour
  uint32_t sun_dusk;            // end of the evening blue hour
  uint32_t sun_astro_dusk;      // start of astronomical night
  uint32_t moon_rise, moon_set;
  uint8_t  moon_phase;   // 0..7
  uint8_t  moon_illum;   // 0..100 %
  // Weather. -1 means "no data" for EVERY field: zero is a meaningful reading
  // here (clear sky, no wind, zero visibility) and cannot double as absence.
  int8_t   wx_cloud;     // 0..100 %
  int8_t   wx_wind_ms;   // m/s; the phone clips at 120, so int8 is enough
  // Visibility is in METRES and therefore 32-bit: Open-Meteo reported 53 180 m
  // in clear air, which fits neither a byte nor a signed int16.
  int32_t  wx_visibility_m;
  uint32_t data_ts;      // freshness (Unix seconds)
} DataPacket;

// Empty packet: zeros, with the weather fields set to -1 (no data).
void packet_init_empty(DataPacket *p);

// Update the packet from an incoming AppMessage dictionary. Only keys that are
// present are touched, so a partial update is safe. Returns true if at least
// one packet field was found.
bool packet_update_from_dict(DataPacket *p, DictionaryIterator *iter);

// True if ts is a meaningful Unix second (the phone sends 0 for "no value").
bool packet_is_valid_ts(uint32_t ts);

// True if the packet is older than threshold_sec relative to now_sec.
bool packet_is_stale(uint32_t data_ts, uint32_t now_sec, uint32_t threshold_sec);

// Whether there is any sun data at all (the ring needs it to be drawn).
bool packet_has_sun_data(const DataPacket *p);
