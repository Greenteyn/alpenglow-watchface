// ring.h — geometry of the day-phase ring.
// Angles are degrees 0..360 (0 = midnight at the top, clockwise).

#pragma once
#include <pebble.h>
#include "packet.h"

typedef enum {
  RING_ARC_DAY = 0,
  RING_ARC_BLUE,
  RING_ARC_GOLDEN,
  RING_ARC_ASTRO_NIGHT, // dark sky: between evening and morning astro twilight
  // Backdrop of the whole circle — what shows through between the blue hour
  // and astronomical night. ring_build_arcs never returns it; the kind exists
  // so twilight is drawn by the same style table as every other phase instead
  // of a special case in the drawing code.
  RING_ARC_TWILIGHT,
} RingArcKind;

// How many arcs ring_build_arcs can return: astro night, morning blue, morning
// golden, day, evening golden, evening blue.
#define RING_MAX_ARCS 6

typedef struct {
  int32_t from_deg; // 0..360
  int32_t to_deg;   // 0..360
  RingArcKind kind;
  bool valid;
} RingArc;

// Local moment (Unix seconds) → angle on the ring, 0..360.
int32_t ring_ts_to_angle(uint32_t ts_sec);

// Angle for a local struct tm (current time → marker position).
int32_t ring_tm_to_angle(const struct tm *t);

// Fill the arc array from a packet: a full day — astro night, both blue and
// golden pairs, and daytime. arcs must hold at least RING_MAX_ARCS entries.
// Arcs come out in drawing order: backdrops first, narrower ones on top.
// Returns the number of valid arcs written.
int ring_build_arcs(const DataPacket *p, RingArc *arcs, int max_arcs);
