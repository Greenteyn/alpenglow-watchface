// packet.c — see packet.h.

#include "packet.h"

void packet_init_empty(DataPacket *p) {
  memset(p, 0, sizeof(DataPacket));
  // Weather: "no data" for all three fields. memset leaves 0, which reads as
  // clear sky / no wind / zero visibility.
  p->wx_cloud = -1;
  p->wx_wind_ms = -1;
  p->wx_visibility_m = -1;
}

static uint32_t read_u32(DictionaryIterator *iter, uint32_t key, bool *found) {
  Tuple *t = dict_find(iter, key);
  if (t) {
    *found = true;
    return (uint32_t)t->value->int32;
  }
  return 0;
}

static int32_t read_i32(DictionaryIterator *iter, uint32_t key, bool *found) {
  Tuple *t = dict_find(iter, key);
  if (t) {
    *found = true;
    return t->value->int32;
  }
  return 0;
}

bool packet_update_from_dict(DataPacket *p, DictionaryIterator *iter) {
  bool any = false;

  uint32_t v;
  bool f;

  f = false; v = read_u32(iter, MESSAGE_KEY_SunRise, &f);        if (f) { p->sun_rise = v; any = true; }
  f = false; v = read_u32(iter, MESSAGE_KEY_SunSet, &f);         if (f) { p->sun_set = v; any = true; }
  f = false; v = read_u32(iter, MESSAGE_KEY_SunGoldenStart, &f); if (f) { p->sun_golden_start = v; any = true; }
  f = false; v = read_u32(iter, MESSAGE_KEY_SunGoldenEnd, &f);   if (f) { p->sun_golden_end = v; any = true; }
  f = false; v = read_u32(iter, MESSAGE_KEY_SunBlueStart, &f);   if (f) { p->sun_blue_start = v; any = true; }
  f = false; v = read_u32(iter, MESSAGE_KEY_SunBlueEnd, &f);     if (f) { p->sun_blue_end = v; any = true; }
  f = false; v = read_u32(iter, MESSAGE_KEY_SunAstroDusk, &f);   if (f) { p->sun_astro_dusk = v; any = true; }
  f = false; v = read_u32(iter, MESSAGE_KEY_SunAstroDawn, &f);   if (f) { p->sun_astro_dawn = v; any = true; }
  f = false; v = read_u32(iter, MESSAGE_KEY_SunDawn, &f);        if (f) { p->sun_dawn = v; any = true; }
  f = false; v = read_u32(iter, MESSAGE_KEY_SunGoldenAmEnd, &f); if (f) { p->sun_golden_am_end = v; any = true; }
  f = false; v = read_u32(iter, MESSAGE_KEY_SunGoldenPmStart, &f); if (f) { p->sun_golden_pm_start = v; any = true; }
  f = false; v = read_u32(iter, MESSAGE_KEY_SunDusk, &f);        if (f) { p->sun_dusk = v; any = true; }
  f = false; v = read_u32(iter, MESSAGE_KEY_MoonRise, &f);       if (f) { p->moon_rise = v; any = true; }
  f = false; v = read_u32(iter, MESSAGE_KEY_MoonSet, &f);        if (f) { p->moon_set = v; any = true; }

  f = false; int32_t iv = read_i32(iter, MESSAGE_KEY_MoonPhase, &f); if (f) { p->moon_phase = (uint8_t)iv; any = true; }
  f = false; iv = read_i32(iter, MESSAGE_KEY_MoonIllum, &f);         if (f) { p->moon_illum = (uint8_t)iv; any = true; }
  f = false; iv = read_i32(iter, MESSAGE_KEY_WxCloud, &f);           if (f) { p->wx_cloud = (int8_t)iv; any = true; }
  f = false; iv = read_i32(iter, MESSAGE_KEY_WxWind, &f);            if (f) { p->wx_wind_ms = (int8_t)iv; any = true; }
  // Visibility is NOT narrowed: metres do not fit in a byte (see packet.h).
  f = false; iv = read_i32(iter, MESSAGE_KEY_WxVisibility, &f);      if (f) { p->wx_visibility_m = iv; any = true; }

  f = false; v = read_u32(iter, MESSAGE_KEY_DataTs, &f);        if (f) { p->data_ts = v; any = true; }

  return any;
}

bool packet_is_valid_ts(uint32_t ts) {
  return ts > 0;
}

bool packet_is_stale(uint32_t data_ts, uint32_t now_sec, uint32_t threshold_sec) {
  if (!packet_is_valid_ts(data_ts)) return true;
  if (now_sec <= data_ts) return false;
  return (now_sec - data_ts) > threshold_sec;
}

bool packet_has_sun_data(const DataPacket *p) {
  return packet_is_valid_ts(p->sun_rise) || packet_is_valid_ts(p->sun_set) ||
         packet_is_valid_ts(p->sun_golden_start) || packet_is_valid_ts(p->sun_blue_start);
}
