// geom.h — integer geometry shared by the screen layout and the icon pack.
//
// Split out of main.c once a second caller appeared: the moon terminator in
// icons.c is filled row by row and needs the disc half-width for every row.
// A second copy of isqrt32 is not an option — its exit condition is subtle
// (see the warning at its definition) and "optimising" one copy would bring
// the hang back.

#pragma once
#include <pebble.h>

// Integer square root (floor).
int32_t isqrt32(int32_t v);

// Half-width of the chord of a circle of radius r over the vertical band
// [y, y+h]. Measured at the band edge FARTHEST from the centre, so a row fits
// whole rather than just at its middle. Clamped to cx to stay on canvas.
int chord_half_at(int cx, int cy, int r, int y, int h);
