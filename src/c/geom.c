// geom.c — see geom.h.

#include "geom.h"

// DO NOT REWRITE AS NEWTON'S METHOD. The form "r = (r + v/r)/2 until r == prev"
// LOOPS FOREVER on values of the form k²−1: the iteration oscillates between
// k−1 and k, and comparing against the previous step alone never detects that
// period. There are 199 such values below 40 000, and the layout does hit them
// (5040 = 71²−1 on chalk) — update_proc never returns and the watchdog kills
// the app. The bit-shift form below is bounded by the word size and always
// terminates.
int32_t isqrt32(int32_t v) {
  if (v <= 0) return 0;
  int32_t rem = v, root = 0, bit = 1 << 30;
  while (bit > rem) bit >>= 2;
  while (bit) {
    if (rem >= root + bit) {
      rem -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return root;
}

int chord_half_at(int cx, int cy, int r, int y, int h) {
  int dy1 = y - cy;     if (dy1 < 0) dy1 = -dy1;
  int dy2 = y + h - cy; if (dy2 < 0) dy2 = -dy2;
  int dy = (dy1 > dy2) ? dy1 : dy2;
  int32_t under = (int32_t)r * r - (int32_t)dy * dy;
  if (under < 1) under = 1;
  int half = (int)isqrt32(under);
  return (half < cx) ? half : cx;
}
