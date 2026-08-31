// ring.c — see ring.h.

#include "ring.h"

#define RING_SECONDS_PER_DAY 86400

// Half-turns of the ring: 0 = midnight at the top, 1 = at the bottom. A count
// rather than a flag keeps the angle below free of a branch, and 180 * n modulo
// 360 is only ever 0 or 180 — a stray value cannot hang the day at an angle of
// its own.
static uint8_t s_half_turns = 0;

void ring_set_orientation(RingOrientation orientation) {
  s_half_turns = (orientation == RING_MIDNIGHT_BOTTOM) ? 1 : 0;
}

// THE one place the day is mapped onto the circle. Both entry points below go
// through it, so the orientation reaches the phase arcs, the light-window pip
// and the current-time marker from here alone.
static int32_t angle_from_seconds_of_day(int sec_of_day) {
  // 0..86400 → 0..360 degrees
  int32_t deg = (int32_t)(((int64_t)sec_of_day * 360) / RING_SECONDS_PER_DAY);
  return (deg + 180 * s_half_turns) % 360;
}

int32_t ring_ts_to_angle(uint32_t ts_sec) {
  // "No such moment" — polar day or night, where suncalc has no answer. The
  // sentinel leaves BEFORE any rotation is applied, which is why the ring is
  // turned inside angle_from_seconds_of_day and not here: rotating -1 would
  // hand callers a perfectly plausible 179°.
  if (ts_sec == 0) return -1;
  time_t t = (time_t)ts_sec;
  struct tm *lt = localtime(&t);
  int sec = lt->tm_hour * 3600 + lt->tm_min * 60 + lt->tm_sec;
  return angle_from_seconds_of_day(sec);
}

int32_t ring_tm_to_angle(const struct tm *t) {
  int sec = t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
  return angle_from_seconds_of_day(sec);
}

static void push_arc(RingArc *arcs, int *n, int max_arcs,
                     uint32_t a, uint32_t b, RingArcKind kind) {
  if (*n >= max_arcs) return;
  int32_t from = ring_ts_to_angle(a);
  int32_t to = ring_ts_to_angle(b);
  if (from < 0 || to < 0) return;
  arcs[*n].from_deg = from;
  arcs[*n].to_deg = to;
  arcs[*n].kind = kind;
  (*n)++;
}

// A full day, not a single window: the ring always draws the current day, while
// the golden/blue pair on the Astro screen is the nearest UPCOMING window and
// may belong to tomorrow. The two come from separate packet fields.
//
// Call order is drawing order: wide backdrop arcs first, narrow ones on top.
// Otherwise daytime paints over the golden hour at its edges.
int ring_build_arcs(const DataPacket *p, RingArc *arcs, int max_arcs) {
  int n = 0;
  // Dark sky: evening astro twilight → morning one, ACROSS midnight (from > to,
  // and the drawing code splits such an arc in two by itself).
  push_arc(arcs, &n, max_arcs, p->sun_astro_dusk, p->sun_astro_dawn,
           RING_ARC_ASTRO_NIGHT);
  // Daytime — from the end of the morning golden hour to the start of the
  // evening one.
  push_arc(arcs, &n, max_arcs, p->sun_golden_am_end, p->sun_golden_pm_start,
           RING_ARC_DAY);
  // Blue hour: morning (dawn → sunrise) and evening (sunset → dusk).
  push_arc(arcs, &n, max_arcs, p->sun_dawn, p->sun_rise, RING_ARC_BLUE);
  push_arc(arcs, &n, max_arcs, p->sun_set, p->sun_dusk, RING_ARC_BLUE);
  // Golden hour: morning (sunrise → end) and evening (start → sunset).
  push_arc(arcs, &n, max_arcs, p->sun_rise, p->sun_golden_am_end, RING_ARC_GOLDEN);
  push_arc(arcs, &n, max_arcs, p->sun_golden_pm_start, p->sun_set, RING_ARC_GOLDEN);
  return n;
}
