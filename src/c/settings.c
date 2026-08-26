// settings.c — see settings.h.

#include "settings.h"
#include <stdlib.h>

// The key number IS the Settings schema version: a shorter old blob must never
// be read into a longer struct, so every layout change takes the next free
// number (DataPacket and the notification marker occupy the gaps). The price is
// a one-off reset to defaults after an upgrade.
#define SETTINGS_PERSIST_KEY 10

// Type-tolerant integer read: Clay "select" fields carry STRING values
// ("0"/"15"/...). When such a value arrives as TUPLE_CSTRING, reading it via
// ->value->int32 yields the string bytes as a number ("0" → 48). So parse
// CSTRING with atoi and take int32 otherwise — the C side is then correct
// whichever type AppMessage delivered.
static int32_t tuple_to_int(const Tuple *t) {
  if (t->type == TUPLE_CSTRING) {
    return (int32_t)atoi(t->value->cstring);
  }
  return t->value->int32;
}

void settings_init_defaults(Settings *s) {
  s->use_24h = true;
  s->use_imperial = false;      // m/s and km
  s->notify_lead_min = 0;       // light notification off by default
  s->tap_control = true;        // all three screens reachable out of the box
  s->astro_timeout_sec = 15;
  s->sw_idle_timeout_sec = 30;  // idle exit Stopwatch→Clock
  s->sw_max_duration_min = 30;  // safety net for a forgotten measurement
  s->show_stopwatch = true;     // stopwatch screen stays in the tap cycle
}

void settings_load(Settings *s) {
  settings_init_defaults(s);
  if (persist_exists(SETTINGS_PERSIST_KEY)) {
    persist_read_data(SETTINGS_PERSIST_KEY, s, sizeof(Settings));
  }
}

void settings_save(const Settings *s) {
  persist_write_data(SETTINGS_PERSIST_KEY, s, sizeof(Settings));
}

// Assign only when the value REALLY differs. Settings ride along in EVERY packet
// from the phone, so "the key is present" does not mean "the user changed
// something" — without this check persist is rewritten and the auto-return timer
// rearmed about once an hour for no reason.
static bool apply_bool(bool *dst, const Tuple *t) {
  if (!t) return false;
  bool v = (tuple_to_int(t) != 0);
  if (*dst == v) return false;
  *dst = v;
  return true;
}

static bool apply_u16(uint16_t *dst, const Tuple *t) {
  if (!t) return false;
  int32_t v = tuple_to_int(t);
  if (v < 0) v = 0;
  if (*dst == (uint16_t)v) return false;
  *dst = (uint16_t)v;
  return true;
}

bool settings_update_from_dict(Settings *s, DictionaryIterator *iter) {
  bool changed = false;

  // Every branch must run, hence no `||`: short-circuiting would skip the
  // remaining keys after the first change.
  if (apply_bool(&s->use_24h, dict_find(iter, MESSAGE_KEY_HourFormat))) changed = true;
  if (apply_bool(&s->use_imperial, dict_find(iter, MESSAGE_KEY_WeatherUnits))) changed = true;
  if (apply_u16(&s->notify_lead_min, dict_find(iter, MESSAGE_KEY_NotifyLeadTime))) changed = true;
  if (apply_bool(&s->tap_control, dict_find(iter, MESSAGE_KEY_TapControl))) changed = true;
  if (apply_u16(&s->astro_timeout_sec, dict_find(iter, MESSAGE_KEY_AstroTimeout))) changed = true;
  if (apply_u16(&s->sw_idle_timeout_sec, dict_find(iter, MESSAGE_KEY_StopwatchIdleTimeout))) changed = true;
  if (apply_u16(&s->sw_max_duration_min, dict_find(iter, MESSAGE_KEY_StopwatchMaxDuration))) changed = true;
  if (apply_bool(&s->show_stopwatch, dict_find(iter, MESSAGE_KEY_ShowStopwatch))) changed = true;

  return changed;
}
